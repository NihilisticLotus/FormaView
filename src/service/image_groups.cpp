#include "formaview/image_groups.hpp"
#ifdef FORMAVIEW_MODEL_EXPORT
#include "formaview/model_export.hpp"
#endif
#include "formaview/scheduler.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QUuid>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>

namespace formaview::imaging {
namespace {
[[noreturn]] void fail(ErrorCode c, const char *m) { throw skeletal::Failure(c, m); }
bool active(const QString &s) { return s == "planning" || s == "queued" || s == "running" || s == "paused" || s == "pausing"; }
QJsonObject query(const char *op, const QString &id = {}) {
  QJsonObject q{{"version", 1}, {"operation", op}};
  if (!id.isEmpty()) q["id"] = id;
  return q;
}
QString hash(const QJsonObject &o) {
  return QCryptographicHash::hash(QJsonDocument(o).toJson(QJsonDocument::Compact),
                                  QCryptographicHash::Sha256).toHex();
}
QJsonObject checked(QJsonObject o) {
  if (o.contains("error"))
    fail(ErrorCode::Io, o["error"].toObject()["message"].toString().toUtf8().constData());
  return o;
}
void save(const QString &path, const QJsonObject &o) {
  const auto bytes = QJsonDocument(o).toJson(QJsonDocument::Compact);
  if (bytes.size() > 1024 * 1024) fail(ErrorCode::ResourceLimit, "Group journal fragment exceeds one MiB");
  QSaveFile f(path); f.setDirectWriteFallback(false);
  if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit())
    fail(ErrorCode::Io, "Cannot atomically save conversion group");
}
QJsonObject load(const QString &path) {
  AccessScope s; s.input_roots = {std::filesystem::path(QFileInfo(path).absolutePath().toStdWString())};
  const auto lease = InputLease::open(s, std::filesystem::path(path.toStdWString()), 2 * 1024 * 1024);
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly) || f.size() > 2 * 1024 * 1024) fail(ErrorCode::Io, "Cannot read conversion group");
  QJsonParseError e;
  const auto d = QJsonDocument::fromJson(f.readAll(), &e);
  if (e.error != QJsonParseError::NoError || !d.isObject()) fail(ErrorCode::Io, "Corrupt conversion group journal");
  return d.object();
}
SchedulerLimits groupLimits() {
  SchedulerLimits l; l.workers = 1; l.queue_capacity = 4;
  l.foreground_queue_reserve = 0; l.foreground_worker_reserve = 0;
  l.capacity = {1, 1, 3ULL * 1024 * 1024 * 1024, 0}; l.foreground_reserve = {0, 0, 0, 0};
  return l;
}
}
struct ImageTaskGroups::Impl {
  struct Group {
    QJsonObject manifest, plan, job;
    std::stop_source stop;
    std::atomic_bool pause_requested{};
  };
  AccessScope scope;
  QString directory;
  PrivateDirectory protection;
  QLockFile owner;
  mutable std::mutex state;
  std::mutex mutation;
  std::condition_variable wake;
  std::map<QString, std::shared_ptr<Group>> groups;
  Scheduler scheduler{groupLimits()};

  Impl(AccessScope s, std::filesystem::path dir)
      : scope(std::move(s)), directory(QDir::cleanPath(QString::fromStdWString(dir.wstring()))),
        protection(PrivateDirectory::open(dir)),
        owner(QStringLiteral("\\\\?\\") + QDir::toNativeSeparators(QDir::cleanPath(directory + "/groups.lock"))) {
    owner.setStaleLockTime(0);
    if (!owner.tryLock()) fail(ErrorCode::Conflict, "Conversion groups already have an owner");
    const auto dirs = QDir(directory).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto &name : dirs) {
      if (QUuid(name).isNull()) fail(ErrorCode::Io, "Invalid conversion group directory");
      const auto path = directory + '/' + name;
      if (!QFileInfo::exists(path + "/index.json")) continue; // Uncommitted creation is never adopted.
      auto g = std::make_shared<Group>();
      g->manifest = load(path + "/index.json");
      if (g->manifest["version"] != 1 || g->manifest["id"] != "group-" + name ||
          !g->manifest["batches"].isArray() || g->manifest["batches"].toArray().isEmpty())
        fail(ErrorCode::Io, "Invalid conversion group manifest");
      QJsonArray plans, jobs;
      auto batches = g->manifest["batches"].toArray();
      qsizetype total = 0;
      for (qsizetype i = 0; i < batches.size(); ++i) {
        auto b = batches[i].toObject();
        const auto requests = load(path + '/' + QString::number(i) + "/requests.json")["items"].toArray();
        if (requests.isEmpty() || requests.size() > 64) fail(ErrorCode::Io, "Invalid conversion group batch");
        total += requests.size();
        const auto records = path + '/' + QString::number(i) + "/records";
        ImageBatchService child(scope, std::filesystem::path(records.toStdWString()));
        // Recover a submission committed just before its group index update.
        for (const auto &file : QDir(records).entryList({"*.json"}, QDir::Files, QDir::Name)) {
          const auto data = load(records + '/' + file);
          if (data["kind"] == "plan") b["plan_id"] = data["id"];
          if (data["kind"] == "job") b["job_id"] = data["id"];
        }
        append(plans, childSnapshot(child, b["plan_id"].toString(), "get_plan", requests), i);
        if (g->manifest.contains("job_id"))
          append(jobs, childSnapshot(child, b["job_id"].toString(), "get_job", requests), i);
        batches[i] = b;
      }
      g->manifest["batches"] = batches;
      if (active(g->manifest["plan_state"].toString())) g->manifest["plan_state"] = "interrupted";
      if (active(g->manifest["job_state"].toString())) g->manifest["job_state"] = "interrupted";
      g->plan = snapshot(g, false, plans);
      if (g->manifest.contains("job_id")) {
        for (qsizetype i = 0; i < jobs.size(); ++i) {
          auto item = jobs[i].toObject();
          if (item["status"] == "pending" || item["status"] == "running") {
            item["status"] = "interrupted";
            g->manifest["job_state"] = "interrupted";
          }
          jobs[i] = item;
        }
        g->job = snapshot(g, true, jobs);
      }
      save(path + "/index.json", g->manifest);
      groups.emplace(g->manifest["id"].toString(), g);
    }
  }
  ~Impl() {
    for (const auto &[id, g] : groups) { (void)id; g->stop.request_stop(); }
    wake.notify_all(); scheduler.shutdown();
  }
  QString path(const std::shared_ptr<Group> &g) const {
    return directory + '/' + g->manifest["id"].toString().mid(6);
  }
  static void append(QJsonArray &dest, QJsonArray items, qsizetype batch) {
    for (auto value : items) {
      auto item = value.toObject(); item["index"] = dest.size(); item["batch"] = batch;
      dest.append(item);
    }
  }
  static QJsonArray childSnapshot(ImageBatchService &child, const QString &id,
                                  const char *op, const QJsonArray &requests) {
    if (!id.isEmpty()) return checked(child.request(query(op, id)))["items"].toArray();
    QJsonArray items;
    for (auto r : requests) items.append(QJsonObject{{"request", r}, {"status", "pending"}});
    return items;
  }
  QJsonObject snapshot(const std::shared_ptr<Group> &g, bool job, const QJsonArray &items) const {
    QJsonObject o{{"version", 1}, {"id", g->manifest[job ? "job_id" : "id"]},
      {"kind", job ? "job" : "plan"}, {"state", g->manifest[job ? "job_state" : "plan_state"]},
      {"items", items}, {"total", items.size()}, {"grouped", true},
      {"batch_count", g->manifest["batches"].toArray().size()},
      {"cancellation_requested", g->stop.stop_requested()}};
    o["pause_requested"] = g->pause_requested.load();
    for (const auto key : {"name", "category", "created_at"}) o[key] = g->manifest[key];
    if (job) {
      o["plan_id"] = g->manifest["id"];
      o["submission_digest"] = g->manifest["submission_digest"];
    }
    else if (g->manifest.contains("approval_digest")) o["approval_digest"] = g->manifest["approval_digest"];
    if (g->manifest.contains("error")) o["error"] = g->manifest["error"];
    return o;
  }
  void publish(const std::shared_ptr<Group> &g, bool job, QJsonArray items) {
    save(path(g) + "/index.json", g->manifest);
    const std::lock_guard lock(state);
    (job ? g->job : g->plan) = snapshot(g, job, items);
  }
  std::shared_ptr<Group> find(const QString &id) {
    const std::lock_guard lock(state);
    for (const auto &[key, g] : groups)
      if (key == id || g->job.value("id") == id) return g;
    fail(ErrorCode::InvalidArgument, "Unknown conversion group");
  }
  QJsonArray allRequests(const std::shared_ptr<Group> &g) {
    QJsonArray all;
    const auto count = g->manifest["batches"].toArray().size();
    for (qsizetype i = 0; i < count; ++i)
      for (auto r : load(path(g) + '/' + QString::number(i) + "/requests.json")["items"].toArray()) all.append(r);
    return all;
  }
  QJsonArray protectedPaths(const std::shared_ptr<Group> &g) {
    if (!g->manifest.contains("protection_fragments"))
      return load(path(g) + "/protection.json")["inputs"].toArray();
    const auto count = g->manifest["protection_fragments"].toInt();
    if (count < 1) fail(ErrorCode::Io, "Invalid protection fragment count");
    QJsonArray inputs;
    for (int i = 0; i < count; ++i) {
      const auto chunk = load(path(g) + "/protected-" + QString::number(i) + ".json")["inputs"].toArray();
      if (chunk.isEmpty() || chunk.size() > 64) fail(ErrorCode::Io, "Invalid protection fragment");
      for (auto value : chunk) inputs.append(value);
    }
    return inputs;
  }
  void run(const std::shared_ptr<Group> &g, bool job, std::stop_token schedulerStop) {
    std::stop_callback callback(schedulerStop, [&] { g->stop.request_stop(); });
    auto items = (job ? g->job : g->plan)["items"].toArray();
    try {
      g->manifest[job ? "job_state" : "plan_state"] = job ? "running" : "planning";
      publish(g, job, items);
      const auto all = allRequests(g);
      std::vector<InputLease> leases;
      std::vector<InputSnapshot> protectedInputs;
      if (job) {
        // A member may have been renamed or replaced since approval. Retain
        // its approved identity as well as the currently readable identities,
        // so an earlier output cannot replace the moved source file.
        for (auto value : g->plan["items"].toArray()) {
          const auto preflight = value.toObject()["preflight"].toObject();
          auto sources = preflight["sources"].toArray();
          sources.append(preflight["input"]);
          for (auto source : preflight["referenced_sources"].toArray()) sources.append(source);
          for (auto source : sources) {
            const auto approved = source.toObject();
            if (approved.isEmpty()) continue;
            InputSnapshot input;
            input.resolved_path = validated_local_path(std::filesystem::path(approved["path"].toString().toStdWString()));
            input.file_identity = approved["file_identity"].toString().toStdString();
            protectedInputs.push_back(std::move(input));
          }
        }
      }
      std::set<QString> protectedPathsSeen;
      for (const auto &v : protectedPaths(g)) {
        if (g->stop.stop_requested()) fail(ErrorCode::Cancelled, "Conversion group cancelled");
        const auto p = std::filesystem::path(v.toString().toStdWString());
        auto key = QString::fromStdWString(validated_local_path(p).wstring()).toCaseFolded();
        if (!protectedPathsSeen.insert(key).second) continue;
        try {
          const ImageAdmission admission(false, g->stop.get_token());
          leases.push_back(InputLease::open(scope, p, ImageBatchLimits{}.image.input_bytes, g->stop.get_token()));
          protectedInputs.push_back(leases.back().snapshot());
        } catch (const skeletal::Failure &e) {
          if (e.error.code == ErrorCode::Cancelled) throw;
          InputSnapshot fallback; fallback.resolved_path = validated_local_path(p);
          protectedInputs.push_back(std::move(fallback));
        }
      }
      std::vector<std::filesystem::path> allocated;
      auto batches = g->manifest["batches"].toArray();
      qsizetype offset = 0;
      bool failed = false;
      for (qsizetype i = 0; i < batches.size(); ++i) {
        while (job && g->pause_requested && !g->stop.stop_requested()) {
          g->manifest["job_state"] = "paused";
          { const std::lock_guard lock(state); g->job = snapshot(g, true, items); }
          std::unique_lock lock(state); wake.wait_for(lock, std::chrono::milliseconds(40));
        }
        if (job) g->manifest["job_state"] = "running";
        if (g->stop.stop_requested()) fail(ErrorCode::Cancelled, "Conversion group cancelled");
        auto b = batches[i].toObject();
        const auto requests = load(path(g) + '/' + QString::number(i) + "/requests.json")["items"].toArray();
        ImageBatchContext context{protectedInputs, allocated};
        for (qsizetype n = offset + requests.size(); n < all.size(); ++n)
          context.reserved_outputs.emplace_back(all[n].toObject()["output"].toString().toStdWString());
        if (job)
          for (qsizetype n = 0; n < items.size(); ++n)
            if (n < offset || n >= offset + requests.size()) {
              const auto out = items[n].toObject()["preflight"].toObject()["output"].toString();
              if (!out.isEmpty()) context.reserved_outputs.emplace_back(out.toStdWString());
            }
        const auto recordPath = path(g) + '/' + QString::number(i) + "/records";
        ImageBatchService child(scope, std::filesystem::path(recordPath.toStdWString()), {}, std::move(context));
        if (job) {
          auto q = query("submit"); q["plan_id"] = b["plan_id"];
          q["approval_digest"] = b["approval_digest"];
          q["idempotency_key"] = g->manifest["idempotency_key"].toString() + '-' + QString::number(i);
          q["acknowledge_losses"] = g->manifest["acknowledge_losses"];
          b["job_id"] = checked(child.request(q, g->stop.get_token()))["job_id"];
        } else {
          auto q = query("create_plan"); q["items"] = requests;
          b["plan_id"] = checked(child.request(q, g->stop.get_token()))["plan_id"];
        }
        batches[i] = b; g->manifest["batches"] = batches;
        publish(g, job, items);
        QJsonObject result;
        for (;;) {
          if (job) {
            const auto control = child.request(query(g->pause_requested ? "pause" : "resume", b["job_id"].toString()));
            (void)control; // Child may have completed between polling and control.
          }
          if (g->stop.stop_requested()) {
            auto q = query("cancel", b[job ? "job_id" : "plan_id"].toString());
            q["kind"] = job ? "job" : "plan"; (void)child.request(q);
          }
          result = checked(child.request(query(job ? "get_job" : "get_plan", b[job ? "job_id" : "plan_id"].toString())));
          const auto partial = result["items"].toArray();
          for (qsizetype n = 0; n < partial.size(); ++n) {
            auto item = partial[n].toObject(); item["index"] = offset + n; item["batch"] = i;
            items[offset + n] = item;
          }
          if (job) g->manifest["job_state"] = result["state"] == "paused" ? "paused" : g->pause_requested ? "pausing" : "running";
          { const std::lock_guard lock(state); (job ? g->job : g->plan) = snapshot(g, job, items); }
          if (!active(result["state"].toString())) break;
          std::unique_lock lock(state); wake.wait_for(lock, std::chrono::milliseconds(40));
        }
        if (!job) {
          b["approval_digest"] = result["approval_digest"]; batches[i] = b;
          for (auto v : result["items"].toArray()) {
            const auto out = v.toObject()["preflight"].toObject()["output"].toString();
            if (!out.isEmpty()) allocated.emplace_back(out.toStdWString());
          }
        }
        if (result["state"] != (job ? "succeeded" : "ready")) failed = true;
        offset += requests.size(); g->manifest["batches"] = batches;
        publish(g, job, items);
      }
      if (g->stop.stop_requested()) fail(ErrorCode::Cancelled, "Conversion group cancelled");
      if (job) for (qsizetype n = 0; n < items.size(); ++n) {
        auto item = items[n].toObject();
        if (item["status"] == "pending" || item["status"] == "running") {
          item["status"] = "interrupted"; items[n] = item; failed = true;
        }
      }
      const bool anySucceeded = std::any_of(items.cbegin(), items.cend(), [](const auto &v) {
        const auto status = v.toObject()["status"].toString();
        return status == "ready" || status == "succeeded" || status == "skipped";
      });
      g->manifest[job ? "job_state" : "plan_state"] = failed ? (anySucceeded ? "partially_succeeded" : "failed") : (job ? "succeeded" : "ready");
      if (!job && !failed) {
        g->manifest["approval_digest"] = hash({{"id", g->manifest["id"]}, {"batches", batches},
          {"policy_revision", QString::fromStdString(scope.policy_revision)}});
      }
      publish(g, job, items);
    } catch (const std::exception &e) {
      const bool cancelled = g->stop.stop_requested();
      g->manifest[job ? "job_state" : "plan_state"] = cancelled ? "cancelled" : "failed";
      g->manifest["error"] = QJsonObject{{"code", cancelled ? "cancelled" : "io"}, {"message", QString::fromUtf8(e.what())}};
      for (qsizetype i = 0; i < items.size(); ++i) {
        auto item = items[i].toObject();
        if (item["status"] == "pending" || item["status"] == "running") item["status"] = cancelled ? "cancelled" : "interrupted";
        items[i] = item;
      }
      try { publish(g, job, items); }
      catch (...) {
        const std::lock_guard lock(state);
        (job ? g->job : g->plan) = snapshot(g, job, items);
      }
    }
  }
  void enqueue(const std::shared_ptr<Group> &g, bool job) {
    TaskRequest task; task.priority = Priority::Background; task.resources = {1, 1, 0, 0};
    task.work = [this, g, job](std::stop_token s) { run(g, job, s); return TaskResult{}; };
    const auto submitted = scheduler.submit(std::move(task));
    if (!submitted.task) {
      g->manifest[job ? "job_state" : "plan_state"] = "failed";
      g->manifest["error"] = QJsonObject{{"code", "resource_limit"}, {"message", QString::fromStdString(submitted.error->message)}};
      publish(g, job, (job ? g->job : g->plan)["items"].toArray());
      fail(submitted.error->code, submitted.error->message.c_str());
    }
  }
  QJsonObject create(const QJsonArray &requests, QJsonArray protect = {}, const QJsonObject &metadata = {}) {
    if (requests.isEmpty()) fail(ErrorCode::InvalidArgument, "Conversion group needs at least one input");
    std::set<QString> outputs;
    for (auto v : requests) {
      const auto r = v.toObject();
      if (!r["input"].isString() || !r["output"].isString() || !r["options"].isObject())
        fail(ErrorCode::InvalidArgument, "Conversion groups require input, output and options");
#ifdef FORMAVIEW_MODEL_EXPORT
      if (r["options"].toObject()["target"] == "fbx") model_export::validate_options(r["options"].toObject());
      else
#endif
        (void)parse_image_options(r["options"].toObject());
      auto key = r["output"].toString(); key.replace('\\', '/');
      if (r["conflict"] != "rename" && !outputs.insert(key.toCaseFolded()).second)
        fail(ErrorCode::Conflict, "Non-renaming group destinations must be distinct");
    }
    auto g = std::make_shared<Group>();
    const auto uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    g->manifest = {{"version", 1}, {"id", "group-" + uuid}, {"plan_state", "planning"}};
    g->manifest["created_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const auto fallback = QFileInfo(requests.first().toObject()["input"].toString()).completeBaseName()
        + " · " + QDateTime::currentDateTime().toString("MM-dd HH:mm:ss");
    g->manifest["name"] = metadata["name"].toString().trimmed().isEmpty() ? fallback : metadata["name"].toString().trimmed().left(200);
    g->manifest["category"] = metadata["category"].toString().trimmed().isEmpty()
        ? (requests.first().toObject()["options"].toObject()["target"] == "fbx" ? "model" : "image")
        : metadata["category"].toString().trimmed().left(80);
    if (!QDir().mkpath(path(g))) fail(ErrorCode::Io, "Cannot create conversion group directory");
    auto privacy = PrivateDirectory::open(std::filesystem::path(path(g).toStdWString()));
    QJsonArray batches, items;
    // Model results contain sizeable verification evidence. Keep each model
    // in its own durable record instead of accumulating it past the journal cap.
    const qsizetype chunkSize = std::any_of(requests.begin(), requests.end(), [](const auto &v) {
      return v.toObject()["options"].toObject()["target"] == "fbx";
    }) ? 1 : 64;
    for (qsizetype offset = 0; offset < requests.size(); offset += chunkSize) {
      QJsonArray chunk;
      for (qsizetype n = offset; n < std::min(requests.size(), offset + chunkSize); ++n) chunk.append(requests[n]);
      const auto index = batches.size();
      const auto folder = path(g) + '/' + QString::number(index);
      if (!QDir().mkpath(folder + "/records")) fail(ErrorCode::Io, "Cannot create child batch directory");
      save(folder + "/requests.json", {{"items", chunk}});
      batches.append(QJsonObject{{"count", chunk.size()}});
      for (auto r : chunk) items.append(QJsonObject{{"index", items.size()}, {"batch", index}, {"request", r}, {"status", "pending"}});
    }
    for (auto r : requests) {
      const auto request = r.toObject();
      protect.append(request["input"]);
#ifdef FORMAVIEW_MODEL_EXPORT
      if (request["options"].toObject()["target"] == "fbx") {
        for (const auto &p : model_export::auxiliary_inputs(request)) protect.append(QString::fromStdWString(p.wstring()));
        const auto suffix = QFileInfo(request["input"].toString()).suffix().toLower();
        if (suffix != "psk" && suffix != "pskx" && suffix != "psa") {
          auto inspect = request; inspect["version"] = 1; inspect["operation"] = "inspect";
          const auto source = execute_image_request(inspect, scope);
          for (const auto key : {"sources", "referenced_sources"})
            for (const auto &v : source[key].toArray()) protect.append(v.toObject()["path"]);
        }
      }
#endif
    }
    int protectionFragments = 0;
    for (qsizetype offset = 0; offset < protect.size(); offset += 64) {
      QJsonArray chunk;
      for (qsizetype n = offset; n < std::min(protect.size(), offset + 64); ++n) chunk.append(protect[n]);
      save(path(g) + "/protected-" + QString::number(protectionFragments++) + ".json", {{"inputs", chunk}});
    }
    g->manifest["protection_fragments"] = protectionFragments;
    g->manifest["batches"] = batches;
    save(path(g) + "/index.json", g->manifest);
    { const std::lock_guard lock(state); g->plan = snapshot(g, false, items); groups.emplace(g->manifest["id"].toString(), g); }
    enqueue(g, false);
    return {{"version", 1}, {"plan_id", g->manifest["id"]}};
  }
  QJsonObject dispatch(const QJsonObject &q) {
    const auto op = q["operation"].toString();
    if (op == "get_plan" || op == "get_job") {
      auto g = find(q["id"].toString()); const std::lock_guard lock(state);
      auto result = op == "get_job" ? g->job : g->plan;
      result["cancellation_requested"] = g->stop.stop_requested(); return result;
    }
    if (op == "list_jobs") {
      QJsonArray items; const std::lock_guard lock(state);
      // The worker mutates manifest outside state; only published snapshots
      // may be read here while a batch is running.
      for (const auto &[id, g] : groups) { (void)id; if (!g->job.isEmpty()) items.append(QJsonObject{{"id", g->job.value("id")}, {"plan_id", g->plan.value("id")}, {"state", g->job.value("state")}, {"grouped", true}, {"name", g->job.value("name")}, {"category", g->job.value("category")}, {"created_at", g->job.value("created_at")}, {"total", g->job.value("total")}}); }
      return {{"items", items}, {"next_offset", QJsonValue::Null}};
    }
    if (op == "cancel") {
      auto g = find(q["id"].toString());
      const std::lock_guard lock(state);
      const auto view = q["kind"] == "job" ? g->job : g->plan;
      const bool accepted = active(view["state"].toString()) && g->stop.request_stop(); wake.notify_all();
      return {{"accepted", accepted}};
    }
    if (op == "pause" || op == "resume") {
      auto g = find(q["id"].toString());
      const std::lock_guard lock(state);
      if (!active(g->job["state"].toString()) || g->stop.stop_requested())
        fail(ErrorCode::Conflict, "Only active groups can pause or resume");
      g->pause_requested = op == "pause";
      g->job["pause_requested"] = g->pause_requested.load();
      wake.notify_all(); return {{"accepted", true}};
    }
    const std::lock_guard write(mutation);
    if (op == "create_plan") return create(q["items"].toArray(), q["protected_inputs"].toArray(), q);
    if (op == "retry_plan") {
      auto g = find(q["id"].toString()); QJsonObject job;
      { const std::lock_guard lock(state); job = g->job; }
      if (active(job["state"].toString())) fail(ErrorCode::Conflict, "Cannot retry active conversion group");
      QJsonArray items;
      for (auto v : job["items"].toArray()) {
        const auto item = v.toObject();
        if (item["status"] != "succeeded" && item["status"] != "skipped") items.append(item["request"]);
      }
      return create(items, protectedPaths(g), g->manifest);
    }
    if (op == "submit") {
      auto g = find(q["plan_id"].toString());
      { const std::lock_guard lock(state);
        if (!g->job.isEmpty()) {
          if (g->job.value("submission_digest") != hash(q)) fail(ErrorCode::Conflict, "Group already submitted with different parameters");
          return {{"job_id", g->job["id"]}, {"reused", true}};
        }
        if (g->plan["state"] != "ready" || g->plan["approval_digest"] != q["approval_digest"])
          fail(ErrorCode::Conflict, "Group requires a complete approved preflight");
        if (q["idempotency_key"].toString().isEmpty() || q["idempotency_key"].toString().size() > 96)
          fail(ErrorCode::InvalidArgument, "Invalid group submission key");
        for (auto v : g->plan["items"].toArray())
          if (!v.toObject()["preflight"].toObject()["losses"].toArray().isEmpty() && !q["acknowledge_losses"].toBool())
            fail(ErrorCode::InvalidArgument, "Group losses require explicit acknowledgement");
      }
      g->manifest["job_id"] = g->manifest["id"].toString() + "-job";
      g->manifest["job_state"] = "queued"; g->manifest["idempotency_key"] = q["idempotency_key"];
      g->manifest["submission_digest"] = hash(q); g->manifest["acknowledge_losses"] = q["acknowledge_losses"].toBool();
      auto items = g->plan["items"].toArray();
      for (qsizetype i = 0; i < items.size(); ++i) { auto item = items[i].toObject(); item["status"] = "pending"; items[i] = item; }
      publish(g, true, items); enqueue(g, true);
      return {{"job_id", g->manifest["job_id"]}};
    }
    fail(ErrorCode::InvalidArgument, "Unknown conversion group operation");
  }
};
ImageTaskGroups::ImageTaskGroups(AccessScope scope, std::filesystem::path journal)
  : impl_(std::make_unique<Impl>(std::move(scope), std::move(journal))) {}
ImageTaskGroups::~ImageTaskGroups() = default;
QJsonObject ImageTaskGroups::request(const QJsonObject &q, std::stop_token stop) {
  try {
    if (stop.stop_requested()) fail(ErrorCode::Cancelled, "Conversion group request cancelled");
    return impl_->dispatch(q);
  } catch (const skeletal::Failure &e) { return image_error_json(e); }
  catch (const std::exception &e) { return image_error_json(skeletal::Failure(ErrorCode::Internal, e.what())); }
}
bool ImageTaskGroups::contains(const QString &id) const {
  const std::lock_guard lock(impl_->state);
  for (const auto &[key, g] : impl_->groups) if (key == id || g->job.value("id") == id) return true;
  return false;
}
bool ImageTaskGroups::has_active_work() const {
  const auto s = impl_->scheduler.stats(); return s.running != 0 || s.queued != 0;
}
}
