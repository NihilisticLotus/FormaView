#include "image_jobs.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSet>
#include <QStandardPaths>
#include <QSaveFile>
#include <QUuid>
#include <algorithm>
#include <set>

namespace {
using namespace formaview;
using namespace formaview::imaging;
SchedulerLimits controlLimits() {
  SchedulerLimits l;
  l.workers = 1;
  l.queue_capacity = 4;
  l.foreground_queue_reserve = 0;
  l.foreground_worker_reserve = 0;
  l.capacity = {1, 1, 3ULL * 1024 * 1024 * 1024, 0};
  l.foreground_reserve = {0, 0, 0, 0};
  return l;
}
QJsonObject request(const char *operation) {
  return {{"version", 1}, {"operation", operation}};
}
QString errorText(const QJsonObject &r) {
  const auto e = r["error"].toObject();
  return e.isEmpty() ? QString{}
                     : e["code"].toString() + ": " + e["message"].toString();
}
// Reverse of the service's error_name(): plan-item errors round-trip as JSON.
ErrorCode error_code(const QString &name) {
  static const QHash<QString, ErrorCode> codes{
      {"unsupported", ErrorCode::Unsupported},
      {"invalid_argument", ErrorCode::InvalidArgument},
      {"queue_full", ErrorCode::QueueFull},
      {"resource_limit", ErrorCode::ResourceLimit},
      {"shutting_down", ErrorCode::ShuttingDown},
      {"cancelled", ErrorCode::Cancelled},
      {"stale_result", ErrorCode::StaleResult},
      {"input_changed", ErrorCode::InputChanged},
      {"out_of_scope", ErrorCode::OutOfScope},
      {"conflict", ErrorCode::Conflict},
      {"io", ErrorCode::Io},
      {"internal", ErrorCode::Internal}};
  return codes.value(name, ErrorCode::Internal);
}
bool active(const QString &s) {
  return s == "queued" || s == "running" || s == "planning";
}
QString outputName(const QString &input, qsizetype index,
                   const QString &target, QString name, const QString &clip = {}) {
  if (name.contains("{clip}")) {
    if (clip.isEmpty()) throw skeletal::Failure(ErrorCode::InvalidArgument,
        "{clip} needs one selected animation per output; use {name}-animations.{ext} for multiple stacks");
    name.replace("{clip}", clip);
  }
  name.replace("{name}", QFileInfo(input).completeBaseName());
  name.replace("{index}", QString::number(index + 1));
  name.replace("{ext}", target);
  if (name.isEmpty() || name.contains('/') || name.contains('\\') ||
      name.contains('{') || name.contains('}'))
    throw skeletal::Failure(ErrorCode::InvalidArgument,
                            "Output template must be a filename using "
                            "{name}, {clip}, {index}, {ext}");
  return name;
}
} // namespace
QStringList ImageJobs::localInputRoots() const {
  QStringList roots;
  for (const auto &drive : QDir::drives()) roots.append(drive.absoluteFilePath());
  return roots;
}
void ImageJobs::setSourceInputs(const QStringList &paths) {
  cancelImport();
  sourcePaths_.clear(); sourceRoots_.clear(); importResult_.clear();
  emit sourcesChanged();
  if (!paths.isEmpty()) addSourceFiles(paths);
}
void ImageJobs::cancelImport() {
  ++importRevision_;
  if (importTask_) importTask_->cancel();
  importing_ = false;
  emit sourcesChanged();
}
void ImageJobs::clearSources() {
  cancelImport(); sourcePaths_.clear(); sourceRoots_.clear(); importResult_.clear();
  invalidatePlan(); emit sourcesChanged();
}
void ImageJobs::removeSource(const QString &path) {
  cancelImport(); sourcePaths_.removeAll(path);
  sourceRoots_.removeIf([&](const QString &r) {
    auto prefix = QDir::fromNativeSeparators(r).toCaseFolded();
    if (!prefix.endsWith('/')) prefix += '/';
    return std::none_of(sourcePaths_.cbegin(), sourcePaths_.cend(), [&](const QString &p) {
      return QDir::fromNativeSeparators(p).toCaseFolded().startsWith(prefix);
    });
  });
  importResult_.clear(); invalidatePlan(); emit sourcesChanged();
}
void ImageJobs::addSourceFiles(const QStringList &paths) { importSources(paths, {}, false); }
void ImageJobs::addSourceFolder(const QString &folder, bool recursive) { importSources({}, folder, recursive); }
void ImageJobs::importSources(QStringList paths, QString folder, bool recursive) {
  cancelImport(); importing_ = true; importResult_.clear();
  const auto revision = importRevision_;
  const auto previous = sourcePaths_, roots = sourceRoots_;
  QPointer<ImageJobs> receiver(this);
  emit sourcesChanged();
  TaskRequest task; task.priority = Priority::Foreground; task.resources = {1, 1, 0, 0};
  task.work = [receiver, paths, folder, recursive, previous, roots, revision](std::stop_token stop) mutable {
    QVariantMap result; QStringList merged = previous, mergedRoots = roots;
    try {
      const auto extensions = image_input_extensions();
      std::set<std::wstring> allowed(extensions.begin(), extensions.end());
      QStringList incomingRoots;
      if (!folder.isEmpty()) {
        const auto root = validated_local_path(std::filesystem::path(folder.toStdWString()));
        AccessScope scope; scope.input_roots = {root};
        std::vector<std::wstring_view> views;
        for (const auto &v : extensions) views.push_back(v);
        const auto found = discover_files(scope, root, recursive, views, stop, false);
        for (const auto &p : found.files) paths.append(QString::fromStdWString(p.wstring()));
        incomingRoots.append(QString::fromStdWString(root.wstring()));
        result["ignored"] = static_cast<qulonglong>(found.ignored_files);
        result["rejected"] = static_cast<qulonglong>(found.rejected_paths.size() + found.rejected_reparse_points.size());
        QVariantList rejected;
        for (const auto &entry : found.rejected_paths)
          rejected.append(QVariantMap{{"path", QString::fromStdWString(entry.path.wstring())}, {"reason", QString::fromStdString(entry.error.message)}});
        for (const auto &entry : found.rejected_reparse_points)
          rejected.append(QVariantMap{{"path", QString::fromStdWString(entry.wstring())}, {"reason", "Reparse traversal is forbidden"}});
        result["rejected_paths"] = rejected;
      }
      QStringList normalized;
      for (const auto &p : paths) {
        if (stop.stop_requested()) throw skeletal::Failure(ErrorCode::Cancelled, "Image import cancelled");
        const auto path = validated_local_path(std::filesystem::path(p.toStdWString()));
        const auto qpath = QDir::fromNativeSeparators(QString::fromStdWString(path.wstring()));
        if (!allowed.contains((L"." + QFileInfo(qpath).suffix().toLower().toStdWString()))) {
          result["ignored"] = result.value("ignored").toULongLong() + 1; continue;
        }
        normalized.append(qpath);
        if (folder.isEmpty()) incomingRoots.append(QFileInfo(qpath).absolutePath());
      }
      std::sort(normalized.begin(), normalized.end(), [](const auto &a, const auto &b) {
        const auto folded = QString::compare(a, b, Qt::CaseInsensitive);
        return folded == 0 ? a < b : folded < 0;
      });
      QSet<QString> keys;
      for (const auto &p : merged) keys.insert(QDir::fromNativeSeparators(p).toCaseFolded());
      int duplicates = 0;
      for (const auto &p : normalized) {
        if (keys.contains(p.toCaseFolded())) { ++duplicates; continue; }
        keys.insert(p.toCaseFolded()); merged.append(p);
      }
      if (!normalized.isEmpty()) mergedRoots.append(incomingRoots);
      QStringList reduced;
      std::sort(mergedRoots.begin(), mergedRoots.end(), [](const auto &a, const auto &b) { return a.size() < b.size(); });
      for (auto r : mergedRoots) {
        r = QDir::fromNativeSeparators(r); while (r.endsWith('/') && r.size() > 3) r.chop(1);
        if (std::none_of(reduced.cbegin(), reduced.cend(), [&](const QString &existing) {
          return r.compare(existing, Qt::CaseInsensitive) == 0 || r.startsWith(existing.endsWith('/') ? existing : existing + '/', Qt::CaseInsensitive);
        })) reduced.append(r);
      }
      mergedRoots = reduced;
      result["added"] = merged.size() - previous.size(); result["duplicates"] = duplicates;
      result["empty"] = normalized.isEmpty();
    } catch (const skeletal::Failure &e) {
      result = image_error_json(e).toVariantMap(); merged = previous; mergedRoots = roots;
    } catch (const std::exception &e) {
      result = image_error_json(skeletal::Failure(ErrorCode::Io, e.what())).toVariantMap(); merged = previous; mergedRoots = roots;
    }
    if (receiver) QMetaObject::invokeMethod(receiver, [receiver, revision, merged, mergedRoots, result] {
      if (!receiver || receiver->importRevision_ != revision) return;
      receiver->importing_ = false; receiver->importResult_ = result;
      if (!result.contains("error")) {
        const bool changed = receiver->sourcePaths_ != merged;
        receiver->sourcePaths_ = merged; receiver->sourceRoots_ = mergedRoots;
        if (changed) receiver->invalidatePlan();
      }
      emit receiver->sourcesChanged();
    }, Qt::QueuedConnection);
    return TaskResult{};
  };
  const auto submitted = control_.submit(std::move(task)); importTask_ = submitted.task;
  if (!submitted.task) { importing_ = false; importResult_ = {{"error", QString::fromStdString(submitted.error->message)}}; emit sourcesChanged(); }
}
QVariantMap ImageJobs::nameExamples(const QStringList &inputs,
                                    const QString &nameTemplate,
                                    const QString &target,
                                    const QVariantList &itemOverrides) const {
  if (!itemOverrides.isEmpty() && itemOverrides.size() != inputs.size())
    return {{"error", tr("Input and per-item options do not match")}};
  QStringList names;
  try {
    for (qsizetype i = 0; i < std::min(inputs.size(), qsizetype(3)); ++i) {
      const auto overrides = itemOverrides.isEmpty() ? QVariantMap{}
                                                     : itemOverrides[i].toMap();
      names.append(outputName(inputs[i], i,
          overrides.value("target", target).toString(), nameTemplate,
          target == "fbx" ? tr("clip") : QString{}));
    }
  } catch (const skeletal::Failure &e) {
    return {{"error", QString::fromUtf8(e.what())}};
  }
  return {{"names", names}};
}
ImageJobs::ImageJobs(QObject *parent)
    : QObject(parent), control_(controlLimits()) {
  historyPath_ = qEnvironmentVariable("FORMAVIEW_IMAGE_STATE_ROOT",
      QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/image-journal") + "/desktop-history.json";
  QFile history(historyPath_);
  if (history.open(QIODevice::ReadOnly)) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(history.readAll(), &error);
    if (error.error == QJsonParseError::NoError && document.isObject()) {
      archivedJobs_ = document.object().toVariantMap();
      for (auto it = archivedJobs_.cbegin(); it != archivedJobs_.cend(); ++it) jobs_.append(it.value());
      if (!jobs_.isEmpty()) { job_ = jobs_.last().toMap(); jobId_ = job_.value("id").toString(); }
    } else error_ = tr("Cannot read conversion task history");
  }
  timer_.setInterval(100);
  connect(&timer_, &QTimer::timeout, this, &ImageJobs::poll);
}
void ImageJobs::saveHistory() {
  const auto path = historyPath_;
  const auto history = archivedJobs_;
  TaskRequest task;
  task.priority = Priority::Background;
  task.work = [path, history](std::stop_token) {
    const auto bytes = QJsonDocument(QJsonObject::fromVariantMap(history)).toJson(QJsonDocument::Compact);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path); file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
      throw std::runtime_error("Cannot persist conversion task history");
    return TaskResult{};
  };
  (void)control_.submit(std::move(task));
}
ImageJobs::~ImageJobs() {
  control_.shutdown();
  endpoint_.reset();
  retiredEndpoints_.clear();
  groups_.reset();
  service_.reset();
}
bool ImageJobs::activeJobs() const {
  if (groups_ && groups_->has_active_work()) return true;
  if (!service_)
    return false;
  if (service_->has_active_work())
    return true;
  auto q = request("list_jobs");
  q["limit"] = 100;
  const auto r = service_->request(q);
  for (const auto &v : r["items"].toArray())
    if (active(v.toObject()["state"].toString()))
      return true;
  // The second page is required for the bounded 128-record store.
  if (!r["next_offset"].isNull()) {
    q["offset"] = r["next_offset"];
    for (const auto &v : service_->request(q)["items"].toArray())
      if (active(v.toObject()["state"].toString()))
        return true;
  }
  return false;
}
void ImageJobs::preflight(const QStringList &inputs, const QVariantMap &scope,
                          const QVariantMap &options,
                          const QString &nameTemplate, const QString &conflict,
                          const QVariantList &itemOverrides, const QVariantMap &metadata) {
  if (busy_)
    return;
  if (!itemOverrides.isEmpty() && itemOverrides.size() != inputs.size()) {
    error_ = tr("Per-item options must match the fixed input list");
    emit changed();
    return;
  }
  auto policy = QJsonObject::fromVariantMap(scope);
  policy["policy_revision"] =
      QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(policy).toJson(
                                                       QJsonDocument::Compact),
                                                   QCryptographicHash::Sha256)
                              .toHex());
  const auto requestedScope = policy.toVariantMap();
  const auto sameScope = scope_.value("policy_revision") ==
                         requestedScope.value("policy_revision");
  if (service_ && !sameScope &&
      (endpoint_ || !retiredEndpoints_.empty() || activeJobs() ||
       active(plan_.value("state").toString()))) {
    error_ = tr("Cancel active work before changing its access scope");
    emit changed();
    return;
  }
  invalidatePlan();
  busy_ = true;
  error_.clear();
  timer_.start();
  emit changed();
  const auto revision = revision_;
  auto existing = sameScope ? service_ : nullptr;
  auto existingGroups = sameScope ? groups_ : nullptr;
  const auto base = qEnvironmentVariable(
      "FORMAVIEW_IMAGE_STATE_ROOT",
      QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
          "/image-journal");
  const auto journalRoot = base + "/" + policy["policy_revision"].toString();
  QPointer<ImageJobs> receiver(this);
  TaskRequest task;
  task.priority = Priority::Foreground;
  task.resources = {1, 1, 3ULL * 1024 * 1024 * 1024, 0};
  task.work = [receiver, existing, existingGroups, inputs, policy, requestedScope, options,
               nameTemplate, conflict, journalRoot, itemOverrides, metadata,
               revision](std::stop_token stop) mutable {
    QJsonObject response;
    QVariantList inputInfo;
    try {
      if (!existing) {
        (void)validated_local_path(std::filesystem::path(journalRoot.toStdWString()));
        if (!QDir().mkpath(journalRoot))
          throw skeletal::Failure(ErrorCode::Io,
                                  "Cannot create image journal directory");
        existing = std::make_shared<ImageBatchService>(
            parse_image_scope(policy),
            std::filesystem::path(journalRoot.toStdWString()));
      }
      if (!existingGroups) {
        const auto groupRoot = journalRoot + "-groups";
        (void)validated_local_path(std::filesystem::path(groupRoot.toStdWString()));
        if (!QDir().mkpath(groupRoot))
          throw skeletal::Failure(ErrorCode::Io, "Cannot create conversion group journal");
        existingGroups = std::make_shared<ImageTaskGroups>(
            parse_image_scope(policy), std::filesystem::path(groupRoot.toStdWString()));
      }
      const auto outputRoots = policy["output_roots"].toArray();
      if (outputRoots.size() != 1)
        throw skeletal::Failure(
            ErrorCode::InvalidArgument,
            "GUI conversion requires one explicit output directory");
      QJsonArray items;
      for (qsizetype i = 0; i < inputs.size(); ++i) {
        if (stop.stop_requested())
          throw skeletal::Failure(ErrorCode::Cancelled,
                                  "Plan preparation cancelled");
        auto itemOptions = QJsonObject::fromVariantMap(options);
        if (!itemOverrides.isEmpty()) {
          const auto overrideOptions =
              QJsonObject::fromVariantMap(itemOverrides[i].toMap());
          for (auto it = overrideOptions.begin(); it != overrideOptions.end();
               ++it)
            itemOptions[it.key()] = it.value();
        }
        auto inspect = request("inspect");
        inspect["input"] = inputs[i];
        inspect["subresource"] = itemOptions.value("subresource").toInt();
        const auto source = existing->request(inspect, stop);
        if (itemOptions["target"] == "fbx") {
          const auto content = itemOptions.take("export_content").toString("combined");
          const auto organization = itemOptions.take("file_organization").toString("per-clip");
          const auto paired = itemOptions.take("paired_file").toString();
          const auto suffix = QFileInfo(inputs[i]).suffix().toLower();
          const bool actorx = suffix == "psk" || suffix == "pskx" || suffix == "psa";
          itemOptions["coordinates"] = actorx ? "actorx-z-up-x-forward-rh-cm" : "source";
          if (!actorx) itemOptions.remove("pairing_policy");
          const bool selection_explicit = itemOptions.contains("selected_clips");
          const auto selection = itemOptions.take("selected_clips").toArray();
          if (content != "model" && content != "combined" && content != "animation")
            throw skeletal::Failure(ErrorCode::InvalidArgument, "Unknown export content");
          if (organization != "per-clip" && organization != "multiple-stacks")
            throw skeletal::Failure(ErrorCode::InvalidArgument, "Unknown file organization");
          auto animation_source = source;
          if (actorx && content == "combined" && !source["has_mesh"].toBool() &&
              paired.isEmpty() && !source.contains("error"))
            throw skeletal::Failure(ErrorCode::InvalidArgument,
                "Model + animation export needs a paired reference model for PSA input");
          if (actorx && content != "model" && !paired.isEmpty()) {
            if (source["has_mesh"].toBool()) {
              itemOptions["animation"] = paired;
              auto inspect_animation = request("inspect");
              inspect_animation["input"] = paired;
              animation_source = existing->request(inspect_animation, stop);
            } else itemOptions["reference"] = paired;
          }
          itemOptions["include_mesh"] = content != "animation";
          if (content == "model") itemOptions.remove("pairing_policy");
          // Read-only snapshot before any access: Qt's non-const operator[]
          // inserts a null value on a missing key, which used to plant an
          // empty "error" here and permanently disable the guards below.
          const QJsonObject animation_info = animation_source;
          const QJsonObject inspect_error =
              animation_info.value("error").toObject();
          QJsonArray clips;
          if (content != "model")
            for (const auto &v : animation_info.value("clips").toArray())
              if (!selection_explicit || selection.contains(v.toObject()["id"])) clips.append(v);
          inputInfo.append(QVariantMap{{"input", inputs[i]}, {"clips", clips.toVariantList()},
              {"error", inspect_error.toVariantMap()}});
          if (content != "model" && !inspect_error.isEmpty()) {
            // Surface the real inspection failure instead of fabricating a
            // clip-less item that only fails later in the writer.
            throw skeletal::Failure(
                error_code(inspect_error.value("code").toString()),
                "Paired animation could not be inspected: " +
                    inspect_error.value("message").toString().toStdString());
          }
          if (content != "model" && clips.isEmpty()) {
            if (selection_explicit) continue;
            throw skeletal::Failure(ErrorCode::InvalidArgument,
                "No source animation selected; choose model-only, or supply an ActorX animation pair");
          }
          if (content == "model" && !source["has_mesh"].toBool() && !source.contains("error"))
            throw skeletal::Failure(ErrorCode::InvalidArgument, "Model-only export needs a model input");
          const auto append_model = [&](QJsonObject parameters, const QString &clip_name) {
            items.append(QJsonObject{{"input", inputs[i]},
                {"output", QDir(outputRoots[0].toString()).filePath(outputName(inputs[i], items.size(), "fbx", nameTemplate, clip_name))},
                {"options", parameters}, {"conflict", conflict}});
          };
          if (content == "model" || clips.isEmpty()) append_model(itemOptions, "model");
          else if (organization == "multiple-stacks") {
            QJsonArray ids;
            for (const auto &v : clips) ids.append(v.toObject()["id"]);
            itemOptions["clip_ids"] = ids;
            append_model(itemOptions, clips.size() == 1 ? clips[0].toObject()["name"].toString() : QString{});
          } else for (const auto &v : clips) {
            auto parameters = itemOptions;
            parameters["clip_id"] = v.toObject()["id"];
            append_model(parameters, v.toObject()["name"].toString());
          }
          continue;
        }
        inputInfo.append(QVariantMap{
            {"input", inputs[i]},
            {"channels",
             source.value("selected_channels").toArray().toVariantList()},
            {"error", source.value("error").toObject().toVariantMap()},
            {"subresource", inspect.value("subresource").toInt()}});
        if (!itemOptions.contains("channels")) {
          QJsonArray channels;
          QString alpha;
          for (const auto &v : source["selected_channels"].toArray()) {
            const auto name = v.toObject()["name"].toString();
            if (name == "A")
              alpha = name;
            if (name == "A" &&
                itemOptions.value("alpha_action").toString("preserve") !=
                    "preserve")
              continue;
            channels.append(
                QJsonObject{{"source", name}, {"destination", name}});
          }
          // Keep unreadable inputs in the plan so their real read error is
          // reported per item by the shared service. This placeholder is never
          // used to decode or successfully convert an unreadable source.
          if (channels.isEmpty() && source.contains("error"))
            channels.append(QJsonObject{{"source", "R"}, {"destination", "R"}});
          itemOptions["channels"] = channels;
          if (!alpha.isEmpty())
            itemOptions["alpha_channel"] = alpha;
        }
        if (!itemOptions.contains("source_alpha") && source.contains("alpha"))
          itemOptions["source_alpha"] = source["alpha"];
        const auto name = outputName(inputs[i], i,
                                    itemOptions["target"].toString(), nameTemplate);
        items.append(QJsonObject{
            {"input", inputs[i]},
            {"output", QDir(outputRoots[0].toString()).filePath(name)},
            {"options", itemOptions},
            {"conflict", conflict}});
      }
      if (items.isEmpty() && !inputs.isEmpty())
        throw skeletal::Failure(ErrorCode::InvalidArgument, "No selected source clips match the current inputs");
      if (items.isEmpty())
        response = {{"configured", true}};
      else {
        auto create = request("create_plan");
        create["items"] = items;
        create["name"] = metadata.value("name").toString();
        create["category"] = metadata.value("category").toString();
        response = existingGroups->request(create, stop);
      }
    } catch (const skeletal::Failure &e) {
      response = image_error_json(e);
    } catch (const std::exception &e) {
      response =
          image_error_json(skeletal::Failure(ErrorCode::Internal, e.what()));
    }
    if (receiver)
      QMetaObject::invokeMethod(
          receiver,
          [receiver, existing, existingGroups, requestedScope, response, revision, inputInfo] {
            if (!receiver)
              return;
            receiver->busy_ = false;
            if (existing) {
              if (receiver->service_ != existing) {
                receiver->discoveryId_.clear();
                receiver->discovery_.clear();
                receiver->discoveryOffset_ = 0;
                receiver->jobId_.clear();
              }
              receiver->service_ = existing;
              receiver->groups_ = existingGroups;
              receiver->scope_ = requestedScope;
              receiver->timer_.start();
            }
            if (revision == receiver->revision_) {
              receiver->inputInfo_ = inputInfo;
              receiver->error_ = errorText(response);
              receiver->planId_ = response["plan_id"].toString();
            } else if (!response["plan_id"].toString().isEmpty())
              receiver->cancel("plan", response["plan_id"].toString());
            receiver->poll();
            emit receiver->changed();
          },
          Qt::QueuedConnection);
    return TaskResult{};
  };
  const auto submitted = control_.submit(std::move(task));
  preparation_ = submitted.task;
  if (!submitted.task) {
    busy_ = false;
    error_ = QString::fromStdString(submitted.error->message);
    emit changed();
  }
}
void ImageJobs::inspectModelInputs(const QStringList &inputs, const QVariantMap &scope, const QString &paired) {
  if (busy_) return;
  if (inputs.isEmpty()) {
    error_ = tr("Select at least one input"); emit changed(); return;
  }
  busy_ = true;
  error_.clear();
  modelSources_.clear();
  const auto revision = revision_;
  emit changed();
  QPointer<ImageJobs> receiver(this);
  TaskRequest task;
  task.priority = Priority::Foreground;
  task.resources = {1, 1, 3ULL * 1024 * 1024 * 1024, 0};
  task.work = [receiver, inputs, scope, paired, revision](std::stop_token stop) {
    QVariantList sources;
    QString error;
    try {
      auto scope_json = QJsonObject::fromVariantMap(scope);
      scope_json["policy_revision"] = "model-inspection";
      const auto policy = parse_image_scope(scope_json);
      for (const auto &input : inputs) {
        auto q = request("inspect"); q["input"] = input;
        auto source = execute_image_request(q, policy, {}, stop);
        const auto suffix = QFileInfo(input).suffix().toLower();
        if ((suffix == "psk" || suffix == "pskx") && source["has_mesh"].toBool() && !paired.isEmpty()) {
          q["input"] = paired;
          source = execute_image_request(q, policy, {}, stop);
        }
        source["input_path"] = input;
        source["animation_path"] = q["input"];
        sources.append(source.toVariantMap());
      }
    } catch (const std::exception &e) { error = QString::fromUtf8(e.what()); }
    if (receiver) QMetaObject::invokeMethod(receiver, [receiver, sources, error, revision] {
      if (!receiver) return;
      receiver->busy_ = false;
      if (receiver->revision_ == revision) {
        receiver->modelSources_ = sources;
        receiver->error_ = error;
      }
      emit receiver->changed();
    }, Qt::QueuedConnection);
    return TaskResult{};
  };
  const auto submitted = control_.submit(std::move(task));
  preparation_ = submitted.task;
  if (!submitted.task) {
    busy_ = false; error_ = QString::fromStdString(submitted.error->message); emit changed();
  }
}
void ImageJobs::invalidatePlan() {
  ++revision_;
  error_.clear();
  if (service_ && !planId_.isEmpty() && active(plan_["state"].toString()))
    cancel("plan", planId_);
  planId_.clear();
  plan_.clear();
  idempotency_.clear();
  emit changed();
}
void ImageJobs::cancelPreparation() {
  ++revision_;
  if (preparation_)
    preparation_->cancel();
  if (!planId_.isEmpty())
    cancel("plan", planId_);
}
void ImageJobs::dispatch(QJsonObject q) {
  if (!service_ || busy_)
    return;
  preparation_.reset();
  busy_ = true;
  error_.clear();
  timer_.start(); // poll() stops the timer once nothing is left to watch
  emit changed();
  const auto service = service_;
  const auto groups = groups_;
  QPointer<ImageJobs> receiver(this);
  TaskRequest task;
  task.priority = Priority::Foreground;
  task.work = [service, groups, receiver, q](std::stop_token stop) {
    const auto id = q.value("plan_id").toString(q.value("id").toString());
    const auto response = groups && groups->contains(id) ? groups->request(q, stop)
                                                        : service->request(q, stop);
    if (receiver)
      QMetaObject::invokeMethod(
          receiver,
          [receiver, response] {
            if (!receiver)
              return;
            receiver->busy_ = false;
            receiver->error_ = errorText(response);
            if (response.contains("plan_id") && !response.contains("job_id")) {
              receiver->planId_ = response["plan_id"].toString();
              receiver->idempotency_.clear();
            }
            if (response.contains("job_id"))
              receiver->jobId_ = response["job_id"].toString();
            if (response.contains("discovery_id")) {
              receiver->discoveryId_ = response["discovery_id"].toString();
              receiver->discoveryOffset_ = 0;
            }
            receiver->poll();
            emit receiver->changed();
          },
          Qt::QueuedConnection);
    return TaskResult{};
  };
  const auto submitted = control_.submit(std::move(task));
  if (!submitted.task) {
    busy_ = false;
    error_ = QString::fromStdString(submitted.error->message);
    emit changed();
  }
}
void ImageJobs::submit(bool acknowledge) {
  if (plan_["state"] != "ready")
    return;
  if (idempotency_.isEmpty())
    idempotency_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
  auto q = request("submit");
  q["plan_id"] = planId_;
  q["approval_digest"] = plan_["approval_digest"].toString();
  q["idempotency_key"] = idempotency_;
  q["acknowledge_losses"] = acknowledge;
  jobId_.clear(); job_.clear(); polledJob_ = {};
  dispatch(q);
}
void ImageJobs::cancel(const QString &kind, const QString &id) {
  if (!service_)
    return;
  auto q = request("cancel");
  q["kind"] = kind;
  q["id"] = id;
  error_ = errorText(groups_ && groups_->contains(id) ? groups_->request(q) : service_->request(q));
  poll();
  emit changed();
}
void ImageJobs::retry(const QString &id) {
  invalidatePlan();
  auto q = request("retry_plan");
  q["id"] = id;
  dispatch(q);
}
void ImageJobs::selectJob(const QString &id) {
  jobId_ = id;
  if (!service_ && archivedJobs_.contains(id)) { job_ = archivedJobs_.value(id).toMap(); emit changed(); return; }
  poll();
}
void ImageJobs::poll() {
  std::erase_if(retiredEndpoints_, [](const auto &e) { return e->idle(); });
  if (busy_ && preparation_) {
    const auto state = preparation_->snapshot();
    if (state.result && state.result->error) {
      busy_ = false;
      error_ = QString::fromStdString(state.result->error->message);
      emit changed();
    }
  }
  if (!service_)
    return;
  auto list = request("list_jobs");
  list["limit"] = 100;
  const auto listing = service_->request(list);
  auto rows = listing["items"].toArray().toVariantList();
  if (!listing["next_offset"].isNull()) {
    list["offset"] = listing["next_offset"];
    rows.append(service_->request(list)["items"].toArray().toVariantList());
  }
  if (groups_) rows.append(groups_->request(request("list_jobs"))["items"].toArray().toVariantList());
  auto get = [&](const char *operation, const QString &id, bool useCache = true) {
    if (QByteArrayView(operation) == "get_job" && archivedJobs_.contains(id) &&
        std::none_of(rows.cbegin(), rows.cend(), [&](const auto &v) { return v.toMap().value("id") == id; }))
      return archivedJobs_.value(id).toMap();
    auto q = request(operation);
    q["id"] = id;
    q["limit"] = 100;
    const bool grouped = groups_ && groups_->contains(id);
    auto result = grouped ? groups_->request(q) : service_->request(q);
    auto items = result["items"].toArray();
    while (!grouped && !result["next_offset"].isNull() && !result.contains("error")) {
      q["offset"] = result["next_offset"];
      const auto page = service_->request(q);
      for (const auto &item : page["items"].toArray()) items.append(item);
      result["next_offset"] = page["next_offset"];
      if (page.contains("error")) break;
    }
    result["items"] = items;
    const bool isPlan = QByteArrayView(operation) == "get_plan";
    auto &cached = isPlan ? polledPlan_ : polledJob_;
    if (useCache && result == cached) return isPlan ? plan_ : job_;
    if (useCache) cached = result;
    return result.toVariantMap();
  };
  const auto plan =
      planId_.isEmpty() ? QVariantMap{} : get("get_plan", planId_);
  const auto job = jobId_.isEmpty() ? QVariantMap{} : get("get_job", jobId_);
  bool historyChanged = false;
  QSet<QString> liveIds;
  for (const auto &row : rows) {
    const auto summary = row.toMap();
    const auto id = summary.value("id").toString();
    liveIds.insert(id);
    if (active(summary.value("state").toString()) || archivedJobs_.contains(id)) continue;
    auto completed = get("get_job", id, false);
    if (completed.contains("error")) continue;
    completed["archived"] = true;
    archivedJobs_[id] = completed;
    historyChanged = true;
  }
  if (historyChanged) saveHistory();
  for (auto it = archivedJobs_.cbegin(); it != archivedJobs_.cend(); ++it)
    if (!liveIds.contains(it.key())) rows.append(it.value());
  QVariantMap discovery;
  if (!discoveryId_.isEmpty()) {
    auto q = request("get_discovery");
    q["id"] = discoveryId_;
    q["offset"] = discoveryOffset_;
    q["limit"] = 100;
    discovery = service_->request(q).toVariantMap();
  }
  if (rows != jobs_ || plan != plan_ || job != job_ ||
      discovery != discovery_) {
    jobs_ = rows;
    plan_ = plan;
    job_ = job;
    discovery_ = discovery;
    emit changed();
  }
  // Idle polling burns GUI-thread wakeups at 10 Hz: once nothing is in
  // flight, stop; every action (preflight/dispatch/endpoint change) restarts.
  const auto watching =
      busy_ || !retiredEndpoints_.empty() || !discoveryId_.isEmpty() ||
      active(plan_.value("state").toString()) ||
      active(job_.value("state").toString()) ||
      std::any_of(rows.cbegin(), rows.cend(), [](const auto &v) {
        return active(v.toMap().value("state").toString());
      });
  if (!watching && timer_.isActive())
    timer_.stop();
}
void ImageJobs::configureScope(const QVariantMap &scope) {
  preflight({}, scope, {}, {}, "skip");
}
void ImageJobs::discover(const QString &directory, bool recursive) {
  auto q = request("discover");
  q["directory"] = directory;
  q["recursive"] = recursive;
  dispatch(q);
}
void ImageJobs::discoveryPage(int offset) {
  if (offset < 0 || offset >= discovery_.value("total").toInt())
    return;
  discoveryOffset_ = offset;
  poll();
}
void ImageJobs::setMcpEnabled(bool enabled) {
  if (enabled == mcpEnabled())
    return;
  if (!enabled) {
    endpoint_->stop();
    retiredEndpoints_.push_back(std::move(endpoint_));
    endpointName_.clear();
    lastClient_.clear();
    timer_.start();
    emit changed();
    return;
  }
  if (!service_ || busy_) {
    error_ = tr("Configure explicit input and output scope first");
    emit changed();
    return;
  }
  try {
    endpointName_ = "formaview-" +
                    QString::number(QCoreApplication::applicationPid()) + '-' +
                    QUuid::createUuid().toString(QUuid::WithoutBraces);
    endpoint_ = std::make_unique<ImageEndpoint>(service_, endpointName_, nullptr, groups_);
    endpoint_->workChanged = [this] {
      // Observe on the next event-loop turn: poll() may retire this endpoint.
      // Do not erase the callback owner inside its own completion event.
      if (!timer_.isActive()) timer_.start();
    };
    endpoint_->observedClient = [this](const QJsonObject &client) {
      lastClient_ = client.toVariantMap();
      lastClient_["observed_at"] =
          QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
      emit changed();
    };
    error_.clear();
  } catch (const std::exception &e) {
    error_ = QString::fromUtf8(e.what());
    endpointName_.clear();
  }
  emit changed();
}
QString ImageJobs::mcpConfiguration(const QString &python,
                                    const QString &adapter) {
  if (!endpoint_ || !QFileInfo(python).isAbsolute() ||
      !QFileInfo(python).isFile() || !QFileInfo(adapter).isAbsolute() ||
      !QFileInfo(adapter).isFile()) {
    error_ = tr("Choose existing absolute Python and adapter paths, then "
                "enable the endpoint");
    emit changed();
    return {};
  }
  const QJsonObject command{
      {"command", python},
      {"args", QJsonArray{adapter}},
      {"env", QJsonObject{{"FORMAVIEW_IMAGE_ENDPOINT", endpointName_}}}};
  const QJsonObject config{
      {"mcpServers", QJsonObject{{"formaview-images", command}}}};
  return QString::fromUtf8(
      QJsonDocument(config).toJson(QJsonDocument::Indented));
}
