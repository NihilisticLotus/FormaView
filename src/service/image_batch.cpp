#include "formaview/image_batch.hpp"
#include "formaview/scheduler.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace formaview::imaging {
namespace {
// A single shared admission gate bounds simultaneous image allocations across
// inspection and batch work. Foreground inspection wins the next image
// boundary; lightweight state/cancel operations never wait on this gate.
class ImageGate {
  std::mutex mutex;
  std::condition_variable_any changed;
  bool active{};
  std::size_t foreground_waiters{};

public:
  struct Lease {
    ImageGate *gate{};
    explicit Lease(ImageGate *value) : gate(value) {}
    Lease(const Lease &) = delete;
    Lease(Lease &&other) noexcept : gate(std::exchange(other.gate, nullptr)) {}
    ~Lease() {
      if (!gate)
        return;
      {
        const std::lock_guard lock(gate->mutex);
        gate->active = false;
      }
      gate->changed.notify_all();
    }
  };
  Lease acquire(bool foreground, std::stop_token stop) {
    std::unique_lock lock(mutex);
    if (foreground)
      ++foreground_waiters;
    const auto ready = changed.wait(lock, stop, [&] {
      return !active && (foreground || !foreground_waiters);
    });
    if (foreground)
      --foreground_waiters;
    if (!ready) {
      changed.notify_all();
      throw skeletal::Failure(ErrorCode::Cancelled,
                              "Image allocation admission cancelled");
    }
    active = true;
    return Lease(this);
  }
};
struct JournalFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};
[[noreturn]] void fail(ErrorCode code, const char *message) {
  throw skeletal::Failure(code, message);
}
QString id() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString hash(const QJsonObject &o) {
  return QCryptographicHash::hash(
             QJsonDocument(o).toJson(QJsonDocument::Compact),
             QCryptographicHash::Sha256)
      .toHex();
}
QString text(const QJsonObject &o, const char *key) {
  if (!o[key].isString() || o[key].toString().isEmpty() ||
      o[key].toString().size() > 32768)
    fail(ErrorCode::InvalidArgument, "Missing or invalid string field");
  return o[key].toString();
}
void fields(const QJsonObject &o, std::initializer_list<const char *> allowed) {
  for (auto it = o.begin(); it != o.end(); ++it)
    if (std::none_of(allowed.begin(), allowed.end(),
                     [&](auto key) { return it.key() == key; }))
      fail(ErrorCode::InvalidArgument, "Unknown batch request field");
}
bool active(const QString &state) {
  return state == "planning" || state == "queued" || state == "running";
}
QJsonObject page(QJsonObject result, const QJsonObject &request) {
  const auto all = result.take("items").toArray();
  const auto offset = request.value("offset").toDouble(0);
  const auto limit = request.value("limit").toDouble(100);
  if ((request.contains("offset") && !request["offset"].isDouble()) ||
      (request.contains("limit") && !request["limit"].isDouble()) ||
      !std::isfinite(offset) || !std::isfinite(limit) || offset < 0 ||
      offset > all.size() || limit < 1 || limit > 100 ||
      std::floor(offset) != offset || std::floor(limit) != limit)
    fail(ErrorCode::InvalidArgument, "Invalid result page");
  QJsonArray items;
  const auto end = std::min(all.size(), static_cast<qsizetype>(offset + limit));
  for (auto n = static_cast<qsizetype>(offset); n < end; ++n)
    items.append(all[n]);
  result["items"] = items;
  result["total"] = static_cast<qint64>(all.size());
  result["next_offset"] = end < all.size()
                              ? QJsonValue(static_cast<qint64>(end))
                              : QJsonValue(QJsonValue::Null);
  return result;
}
SchedulerLimits scheduler_limits(const ImageBatchLimits &limits) {
  SchedulerLimits result;
  result.workers = 1;
  result.queue_capacity = limits.queue;
  result.foreground_queue_reserve = 0;
  result.foreground_worker_reserve = 0;
  result.capacity = {1, 1, 3ULL * 1024 * 1024 * 1024, 0};
  result.foreground_reserve = {0, 0, 0, 0};
  return result;
}
QJsonObject failed_item(const QJsonObject &request, qsizetype index,
                        const skeletal::Failure &error) {
  auto result = image_error_json(error);
  result["index"] = static_cast<qint64>(index);
  result["request"] = request;
  result["status"] =
      error.error.code == ErrorCode::Cancelled ? "cancelled" : "failed";
  return result;
}
} // namespace
struct ImageAdmission::Impl {
  ImageGate::Lease lease;
  static ImageGate &gate() {
    static ImageGate value;
    return value;
  }
  Impl(bool foreground, std::stop_token stop)
      : lease(gate().acquire(foreground, stop)) {}
};
ImageAdmission::ImageAdmission(bool foreground, std::stop_token stop)
    : impl_(std::make_unique<Impl>(foreground, stop)) {}
ImageAdmission::~ImageAdmission() = default;
struct ImageBatchService::Impl {
  struct Record {
    QJsonObject data;
    std::stop_source stop;
  };
  AccessScope scope;
  QString directory;
  ImageBatchLimits limits;
  // Disk serialisation never holds the state mutex used by queries/cancel.
  std::mutex mutation, state;
  std::map<QString, std::shared_ptr<Record>> records;
  std::unique_ptr<QLockFile> owner;
  std::optional<PrivateDirectory> private_directory;
  Scheduler scheduler;
  bool closing{};

  Impl(AccessScope s, std::filesystem::path dir, ImageBatchLimits l)
      : scope(std::move(s)), directory(QString::fromStdWString(dir.wstring())),
        limits(l), scheduler(scheduler_limits(l)) {
    if (!dir.is_absolute() || !limits.records || !limits.items ||
        limits.items > 64 || !limits.queue)
      fail(ErrorCode::InvalidArgument,
           "Invalid batch service limits or journal path");
    if (!QDir(directory).exists())
      fail(ErrorCode::Io, "Explicit journal directory must exist");
    // Require a dedicated directory before restricting its ACL. Do not alter
    // an arbitrary folder containing unrelated user data.
    const auto entries = QDir(directory).entryList(
        QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const auto &entry : entries)
      if (entry != "service.lock" &&
          (entry.size() != 41 || !entry.endsWith(".json") ||
           QUuid(entry.left(36)).isNull()))
        fail(ErrorCode::InvalidArgument,
             "Journal directory contains unrelated entries");
    private_directory.emplace(PrivateDirectory::open(dir));
    owner = std::make_unique<QLockFile>(directory + "/service.lock");
    owner->setStaleLockTime(0);
    if (!owner->tryLock())
      fail(ErrorCode::Conflict, "Journal already has an owner");
    const auto files =
        QDir(directory).entryList({"*.json"}, QDir::Files, QDir::Name);
    if (files.size() > static_cast<qsizetype>(limits.records))
      fail(ErrorCode::ResourceLimit, "Journal record limit exceeded");
    for (const auto &file : files) {
      AccessScope journal_scope;
      journal_scope.input_roots = {dir};
      auto journal_lease = std::make_optional(InputLease::open(
          journal_scope, dir / file.toStdWString(), 2 * 1024 * 1024));
      QFile input(directory + '/' + file);
      if (!input.open(QIODevice::ReadOnly) || input.size() > 2 * 1024 * 1024)
        fail(ErrorCode::Io, "Cannot read bounded journal record");
      QJsonParseError error{};
      const auto doc = QJsonDocument::fromJson(input.readAll(), &error);
      input.close(); // Windows must release the read handle before atomic
                     // replacement.
      journal_lease.reset();
      if (error.error != QJsonParseError::NoError || !doc.isObject())
        fail(ErrorCode::Io, "Corrupt journal record; refusing silent loss");
      auto data = doc.object();
      const auto record_id = text(data, "id");
      if (file != record_id + ".json" || QUuid(record_id).isNull() ||
          data["version"].toInt() != 1 ||
          (data["kind"] != "plan" && data["kind"] != "job" &&
           data["kind"] != "discovery") ||
          !data["items"].isArray() ||
          data["items"].toArray().size() >
              static_cast<qsizetype>(
                  data["kind"] == "discovery" ? 4096 : limits.items))
        fail(ErrorCode::Io, "Invalid journal record contract");
      if (active(data["state"].toString())) {
        data["state"] = "interrupted";
        data["recovery_note"] = "Service stopped before terminal journal "
                                "record. Committed item records are retained; "
                                "unfinished items are not automatically rerun.";
        auto items = data["items"].toArray();
        for (qsizetype i = 0; i < items.size(); ++i) {
          auto item = items[i].toObject();
          if (item["status"] == "pending" || item["status"] == "running") {
            item["status"] = "interrupted";
            item["outcome_uncertain"] = true;
            reconcile(item);
            items[i] = item;
          }
        }
        data["items"] = items;
        save(data);
      }
      auto record = std::make_shared<Record>();
      record->data = data;
      records.emplace(record_id, std::move(record));
    }
  }
  ~Impl() {
    {
      const std::lock_guard lock(state);
      closing = true;
      for (const auto &[key, record] : records)
        record->stop.request_stop();
    }
    scheduler.shutdown();
    // Queued callbacks may not run during scheduler shutdown. Persist them as
    // interrupted, never as successful or implicitly resubmitted.
    try {
      const std::lock_guard write(mutation);
      for (const auto &[key, record] : records) {
        auto data = copy(record);
        if (active(data["state"].toString())) {
          data["state"] = "interrupted";
          publish(record, data);
        }
      }
    } catch (...) { /* Previous durable state is recovered on next startup. */
    }
  }
  QJsonObject copy(const std::shared_ptr<Record> &record) {
    const std::lock_guard lock(state);
    return record->data;
  }
  void reconcile(QJsonObject &item) {
    if (!item["commit_intent"].isObject())
      return;
    const auto intent = item["commit_intent"].toObject();
    OutputIntent owned;
    owned.temporary =
        std::filesystem::path(text(intent, "temporary").toStdWString());
    owned.file_identity = text(intent, "file_identity").toStdString();
    const auto target = intent["destination"].toString();
    if (!target.isEmpty()) {
      try {
        auto recovery_scope = scope;
        recovery_scope.input_roots = scope.output_roots;
        auto output = InputLease::open(
            recovery_scope, std::filesystem::path(target.toStdWString()),
            limits.image.decoded_bytes * 2);
        if (output.snapshot().file_identity == owned.file_identity &&
            output.snapshot().content_sha256 ==
                text(intent, "output_sha256").toStdString()) {
          auto result = intent["result"].toObject();
          result["status"] = "succeeded";
          result["committed_output"] = target;
          result["output_sha256"] = intent["output_sha256"];
          item["result"] = result;
          item["status"] = "succeeded";
          item["outcome_uncertain"] = false;
          item["recovered_commit"] = true;
        }
      } catch (const skeletal::Failure &error) {
        item["recovery_check"] = image_error_json(error)["error"];
      }
    }
    try {
      remove_owned_temporary(scope, owned);
    } catch (const skeletal::Failure &error) {
      item["temporary_cleanup_error"] = image_error_json(error)["error"];
    }
  }
  void save(const QJsonObject &data) {
    const auto bytes = QJsonDocument(data).toJson(QJsonDocument::Compact);
    if (bytes.size() > 2 * 1024 * 1024)
      fail(ErrorCode::ResourceLimit, "Journal record exceeds two MiB");
    QSaveFile file(directory + '/' + text(data, "id") + ".json");
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
        !file.commit())
      throw JournalFailure("Atomic journal write failed");
  }
  void publish(const std::shared_ptr<Record> &record, const QJsonObject &data) {
    save(data);
    const std::lock_guard lock(state);
    record->data = data;
  }
  std::shared_ptr<Record> find(const QString &key, const char *kind) {
    const std::lock_guard lock(state);
    const auto it = records.find(key);
    if (it == records.end() || it->second->data["kind"] != kind)
      fail(ErrorCode::InvalidArgument, "Unknown plan or job");
    return it->second;
  }
  void update_item(const std::shared_ptr<Record> &record, qsizetype index,
                   const QJsonObject &item) {
    const std::lock_guard write(mutation);
    auto data = copy(record);
    auto items = data["items"].toArray();
    items[index] = item;
    data["items"] = items;
    publish(record, data);
  }
  void finish(const std::shared_ptr<Record> &record, const QString &status,
              QJsonObject error = {}) {
    const std::lock_guard write(mutation);
    auto data = copy(record);
    data["state"] = status;
    if (!error.isEmpty())
      data["error"] = error["error"];
    if (data["kind"] == "plan" && status == "ready") {
      data["approval_digest"] = hash(QJsonObject{
          {"id", data["id"]},
          {"items", data["items"]},
          {"policy_revision", QString::fromStdString(scope.policy_revision)},
          {"implementation", "M05-batch-v1"}});
    }
    publish(record, data);
  }
  void run(const std::shared_ptr<Record> &record, bool planning,
           std::stop_token scheduler_stop) {
    std::stop_callback cancelled(scheduler_stop,
                                 [&] { record->stop.request_stop(); });
    const auto stop = record->stop.get_token();
    try {
      if (!planning)
        finish(record, "running");
      const auto data = copy(record);
      const auto entries = data["items"].toArray();
      // Hold every readable batch input, including aliases, throughout all
      // commits. Failed opens still retain lexical path protection.
      std::vector<InputLease> leases;
      std::vector<InputSnapshot> protected_inputs;
      for (const auto &entry : entries) {
        const auto request = entry.toObject()["request"].toObject();
        InputSnapshot fallback;
        fallback.resolved_path =
            std::filesystem::path(text(request, "input").toStdWString());
        try {
          const auto allocation = ImageAdmission(false, stop);
          leases.push_back(InputLease::open(scope, fallback.resolved_path,
                                            limits.image.input_bytes, stop));
          protected_inputs.push_back(leases.back().snapshot());
        } catch (const skeletal::Failure &) {
          protected_inputs.push_back(std::move(fallback));
        }
      }
      qsizetype succeeded = 0, failed = 0, cancelled_count = 0;
      std::vector<std::filesystem::path> allocated_outputs;
      for (qsizetype index = 0; index < entries.size(); ++index) {
        auto entry = entries[index].toObject();
        auto request = entry["request"].toObject();
        try {
          if (stop.stop_requested())
            fail(ErrorCode::Cancelled, "Batch cancellation requested");
          request["version"] = 1;
          request["operation"] = planning ? "preflight" : "convert";
          if (!planning) {
            request["approval_digest"] =
                entry["preflight"].toObject()["approval_digest"];
            request["acknowledge_losses"] = data["acknowledge_losses"];
            entry["status"] = "running";
            update_item(record, index, entry);
          }
          const auto allocation = ImageAdmission(false, stop);
          auto reserved_outputs = allocated_outputs;
          if (planning && request["conflict"] == "rename") {
            // Reserve future requested names too: an earlier rename must not
            // consume a later skip/overwrite/rename item's destination.
            for (qsizetype other = 0; other < entries.size(); ++other) {
              if (other != index)
                reserved_outputs.emplace_back(
                    text(entries[other].toObject()["request"].toObject(),
                         "output")
                        .toStdWString());
            }
          }
          const auto approved_output =
              planning ? std::filesystem::path{}
                       : std::filesystem::path(
                             text(entry["preflight"].toObject(), "output")
                                 .toStdWString());
          auto result = execute_image_request(
              request, scope, limits.image, stop, protected_inputs,
              [&](const QJsonObject &intent) {
                entry["commit_intent"] = intent;
                update_item(record, index, entry);
              },
              reserved_outputs, approved_output);
          if (planning)
            allocated_outputs.emplace_back(
                text(result, "output").toStdWString());
          entry[planning ? "preflight" : "result"] = result;
          entry["status"] = planning ? QJsonValue("ready") : result["status"];
          ++succeeded;
        } catch (const skeletal::Failure &error) {
          entry = failed_item(entry["request"].toObject(), index, error);
          if (error.error.code == ErrorCode::Cancelled)
            ++cancelled_count;
          else
            ++failed;
        } catch (const JournalFailure &) {
          throw;
        } catch (const std::exception &error) {
          entry =
              failed_item(entry["request"].toObject(), index,
                          skeletal::Failure(ErrorCode::Internal, error.what()));
          ++failed;
        }
        update_item(record, index, entry);
      }
      finish(record, cancelled_count ? "cancelled"
                     : failed   ? (succeeded ? "partially_succeeded" : "failed")
                     : planning ? "ready"
                                : "succeeded");
    } catch (const std::exception &error) {
      // In-memory status makes persistence failure visible. Never continue to
      // further output commits after a journal failure.
      const std::lock_guard lock(state);
      record->data["state"] = "interrupted";
      record->data["persistence_error"] = QString::fromUtf8(error.what());
    }
  }
  std::shared_ptr<Record> enqueue(QJsonObject data, bool planning) {
    // Caller holds mutation so admission and idempotency are serialised.
    {
      const std::lock_guard lock(state);
      if (closing)
        fail(ErrorCode::ShuttingDown, "Service is stopping");
      if (records.size() >= limits.records)
        fail(ErrorCode::ResourceLimit, "Retained record limit reached");
    }
    const auto stats = scheduler.stats();
    if (stats.queued >= limits.queue)
      fail(ErrorCode::QueueFull, "Batch queue is full");
    auto record = std::make_shared<Record>();
    record->data = data;
    save(data);
    {
      const std::lock_guard lock(state);
      records.emplace(text(data, "id"), record);
    }
    TaskRequest task;
    task.resources = {1, 1, 3ULL * 1024 * 1024 * 1024, 0};
    task.work = [this, record, planning](std::stop_token stop) {
      run(record, planning, stop);
      return TaskResult{};
    };
    const auto submitted = scheduler.submit(std::move(task));
    if (!submitted.task) {
      data["state"] = "failed";
      data["admission_error"] =
          QString::fromStdString(submitted.error->message);
      publish(record, data);
      fail(submitted.error->code, submitted.error->message.c_str());
    }
    return record;
  }
  QJsonObject plan(const QJsonArray &requests) {
    if (requests.isEmpty() ||
        requests.size() > static_cast<qsizetype>(limits.items))
      fail(ErrorCode::InvalidArgument, "Batch needs one to 64 items");
    QJsonArray items;
    std::set<QString> outputs;
    for (qsizetype i = 0; i < requests.size(); ++i) {
      if (!requests[i].isObject())
        fail(ErrorCode::InvalidArgument, "Expected image request object");
      const auto request = requests[i].toObject();
      fields(request, {"input", "output", "options", "conflict"});
      (void)text(request, "input");
      (void)text(request, "output");
      auto output_key = text(request, "output");
      output_key.replace('\\', '/');
      if (request["conflict"] != "rename" &&
          !outputs.insert(output_key.toCaseFolded()).second)
        fail(ErrorCode::Conflict,
             "Non-renaming batch destinations must be distinct");
      if (!request["options"].isObject())
        fail(ErrorCode::InvalidArgument, "Image options required");
      (void)parse_image_options(request["options"].toObject());
      items.append(QJsonObject{{"index", static_cast<qint64>(i)},
                               {"request", request},
                               {"status", "pending"}});
    }
    const std::lock_guard write(mutation);
    const auto record = enqueue({{"version", 1},
                                 {"kind", "plan"},
                                 {"id", id()},
                                 {"state", "planning"},
                                 {"items", items}},
                                true);
    const auto data = copy(record);
    return {{"version", 1}, {"plan_id", data["id"]}, {"state", data["state"]}};
  }
  QJsonObject dispatch(const QJsonObject &request, std::stop_token stop) {
    if (request["version"].toDouble() != 1)
      fail(ErrorCode::InvalidArgument, "Batch request version must be 1");
    if (QJsonDocument(request).toJson(QJsonDocument::Compact).size() >
        1024 * 1024)
      fail(ErrorCode::ResourceLimit, "Batch request exceeds one MiB");
    const auto operation = text(request, "operation");
    if (operation == "capabilities") {
      fields(request, {"version", "operation"});
      return image_capabilities();
    }
    if (operation == "inspect") {
      const auto allocation = ImageAdmission(true, stop);
      return execute_image_request(request, scope, limits.image, stop);
    }
    if (operation == "discover") {
      fields(request, {"version", "operation", "directory", "recursive"});
      if (request.contains("recursive") && !request["recursive"].isBool())
        fail(ErrorCode::InvalidArgument, "Recursive discovery must be boolean");
      static constexpr std::wstring_view extensions[] = {
          L".png", L".jpg", L".jpeg", L".jpe",  L".jfif", L".bmp",
          L".dib", L".gif", L".ico",  L".tif",  L".tiff", L".webp",
          L".tga", L".dds", L".hdr",  L".rgbe", L".exr"};
      const auto found = discover_files(
          scope,
          std::filesystem::path(text(request, "directory").toStdWString()),
          request["recursive"].toBool(), extensions, stop);
      QJsonArray items, rejected;
      for (const auto &path : found.files)
        items.append(
            QJsonObject{{"input", QString::fromStdWString(path.wstring())}});
      for (const auto &path : found.rejected_reparse_points)
        rejected.append(QString::fromStdWString(path.wstring()));
      auto record = std::make_shared<Record>();
      record->data = {
          {"version", 1},
          {"kind", "discovery"},
          {"id", id()},
          {"state", "ready"},
          {"items", items},
          {"rejected_reparse_points", rejected},
          {"visited_entries", static_cast<qint64>(found.visited_entries)},
          {"ignored_files", static_cast<qint64>(found.ignored_files)},
          {"directory", request["directory"]},
          {"recursive", request["recursive"].toBool()},
          {"membership_only", true}};
      const std::lock_guard write(mutation);
      {
        const std::lock_guard lock(state);
        if (records.size() >= limits.records)
          fail(ErrorCode::ResourceLimit, "Retained record limit reached");
      }
      save(record->data);
      {
        const std::lock_guard lock(state);
        records.emplace(record->data["id"].toString(), record);
      }
      return {{"version", 1},
              {"discovery_id", record->data["id"]},
              {"total", static_cast<qint64>(items.size())}};
    }
    if (operation == "create_plan") {
      fields(request, {"version", "operation", "items"});
      if (!request["items"].isArray())
        fail(ErrorCode::InvalidArgument, "Items array required");
      return plan(request["items"].toArray());
    }
    if (operation == "get_plan" || operation == "get_job" ||
        operation == "get_discovery") {
      fields(request, {"version", "operation", "id", "offset", "limit"});
      const auto record =
          find(text(request, "id"), operation == "get_plan"  ? "plan"
                                    : operation == "get_job" ? "job"
                                                             : "discovery");
      auto data = copy(record);
      data.remove("idempotency_key");
      data["cancellation_requested"] = record->stop.stop_requested();
      return page(data, request);
    }
    if (operation == "list_jobs") {
      fields(request, {"version", "operation", "offset", "limit"});
      QJsonArray items;
      {
        const std::lock_guard lock(state);
        for (const auto &[key, record] : records)
          if (record->data["kind"] == "job")
            items.append(QJsonObject{{"id", key},
                                     {"state", record->data["state"]},
                                     {"plan_id", record->data["plan_id"]}});
      }
      return page({{"version", 1}, {"items", items}}, request);
    }
    if (operation == "cancel") {
      fields(request, {"version", "operation", "id", "kind"});
      const auto kind = text(request, "kind");
      if (kind != "job" && kind != "plan")
        fail(ErrorCode::InvalidArgument, "Invalid cancellation kind");
      const auto record =
          find(text(request, "id"), kind == "job" ? "job" : "plan");
      const auto data = copy(record);
      const bool accepted =
          active(data["state"].toString()) && record->stop.request_stop();
      return {{"version", 1},
              {"accepted", accepted},
              {"state", data["state"]},
              {"cancellation_requested", record->stop.stop_requested()}};
    }
    if (operation == "retry_plan") {
      fields(request, {"version", "operation", "id"});
      const auto data = copy(find(text(request, "id"), "job"));
      if (active(data["state"].toString()))
        fail(ErrorCode::Conflict, "Cannot retry an active job");
      QJsonArray items;
      for (const auto &value : data["items"].toArray()) {
        const auto item = value.toObject();
        const auto status = item["status"].toString();
        if (status != "succeeded" && status != "skipped")
          items.append(item["request"]);
      }
      // A fresh plan requires fresh approval, including any uncertain outcome.
      return plan(items);
    }
    if (operation == "submit") {
      fields(request, {"version", "operation", "plan_id", "approval_digest",
                       "idempotency_key", "acknowledge_losses"});
      const auto key = text(request, "idempotency_key");
      if (key.size() > 128)
        fail(ErrorCode::InvalidArgument,
             "Idempotency key exceeds 128 characters");
      if (request.contains("acknowledge_losses") &&
          !request["acknowledge_losses"].isBool())
        fail(ErrorCode::InvalidArgument, "Acknowledgement must be boolean");
      const auto fingerprint = hash(request);
      const std::lock_guard write(mutation);
      {
        const std::lock_guard lock(state);
        for (const auto &[record_id, record] : records)
          if (record->data["kind"] == "job" &&
              record->data["idempotency_key"] == key) {
            if (record->data["submission_digest"] != fingerprint)
              fail(ErrorCode::Conflict,
                   "Idempotency key was used with different parameters");
            return {{"version", 1},
                    {"job_id", record_id},
                    {"state", record->data["state"]},
                    {"reused", true}};
          }
      }
      const auto data = copy(find(text(request, "plan_id"), "plan"));
      if (data["state"] != "ready")
        fail(ErrorCode::Conflict, "Plan is not fully preflighted");
      if (data["approval_digest"] != text(request, "approval_digest"))
        fail(ErrorCode::InputChanged, "Plan digest mismatch");
      auto items = data["items"].toArray();
      for (qsizetype i = 0; i < items.size(); ++i) {
        auto item = items[i].toObject();
        if (!item["preflight"].toObject()["losses"].toArray().isEmpty() &&
            !request["acknowledge_losses"].toBool())
          fail(ErrorCode::InvalidArgument,
               "Plan losses require explicit acknowledgement");
        item["status"] = "pending";
        items[i] = item;
      }
      auto record = enqueue(
          {{"version", 1},
           {"kind", "job"},
           {"id", id()},
           {"plan_id", data["id"]},
           {"state", "queued"},
           {"items", items},
           {"idempotency_key", key},
           {"submission_digest", fingerprint},
           {"acknowledge_losses", request["acknowledge_losses"].toBool()}},
          false);
      const auto result = copy(record);
      return {{"version", 1},
              {"job_id", result["id"]},
              {"state", result["state"]},
              {"reused", false}};
    }
    fail(ErrorCode::InvalidArgument, "Unknown batch operation");
  }
};
ImageBatchService::ImageBatchService(AccessScope scope,
                                     std::filesystem::path journal,
                                     ImageBatchLimits limits)
    : impl_(std::make_unique<Impl>(std::move(scope), std::move(journal),
                                   limits)) {}
ImageBatchService::~ImageBatchService() = default;
bool ImageBatchService::has_active_work() const {
  const auto stats = impl_->scheduler.stats();
  return stats.running != 0 || stats.queued != 0;
}
QJsonObject ImageBatchService::request(const QJsonObject &request,
                                       std::stop_token stop) {
  try {
    return impl_->dispatch(request, stop);
  } catch (const skeletal::Failure &error) {
    return image_error_json(error);
  } catch (const std::exception &error) {
    return image_error_json(
        skeletal::Failure(ErrorCode::Internal, error.what()));
  }
}
} // namespace formaview::imaging
