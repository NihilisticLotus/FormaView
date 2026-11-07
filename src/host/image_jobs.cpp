#include "image_jobs.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
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
#include <mutex>

struct ImageJobs::PreparationState {
  std::mutex mutex;
  QVariantMap status;
  void update(const QJsonObject &value) {
    const std::lock_guard lock(mutex);
    status = value.toVariantMap();
  }
};

void ImageJobs::finishPreparation() {
  preparationState_.reset();
  preparationStatus_["active"] = false;
}

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
void writeHistory(const QString &path, const QVariantMap &history) {
  const auto bytes = QJsonDocument(QJsonObject::fromVariantMap(history)).toJson(QJsonDocument::Compact);
  QDir().mkpath(QFileInfo(path).absolutePath());
  QSaveFile file(path); file.setDirectWriteFallback(false);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    throw std::runtime_error("Cannot persist conversion task history");
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
  return s == "queued" || s == "running" || s == "planning" || s == "paused" || s == "pausing";
}
QString outputName(const QString &input, qsizetype index,
                   const QString &target, QString name, const QString &clip = {}) {
  if (name.contains("{clip}")) {
    if (clip.isEmpty()) throw skeletal::Failure(ErrorCode::InvalidArgument,
        "{clip} needs one selected animation per output; use {name}-animations.{ext} for multiple stacks");
    // Clip labels are asset metadata, not filesystem names (FBX commonly
    // uses 'model|take'). Keep the source label unchanged; sanitize only the
    // generated filename token. Existing collision policy still applies.
    auto filename = clip;
    for (auto &c : filename)
      if (c.unicode() < 32 || QStringLiteral("/\\:*?\"<>|{}").contains(c)) c = QChar('_');
    while (!filename.isEmpty() && (filename.endsWith('.') || filename.endsWith(' '))) filename.chop(1);
    if (filename.isEmpty()) filename = "clip";
    const auto stem = filename.section('.',0,0).toUpper();
    if (QStringList{"CON","PRN","AUX","NUL"}.contains(stem) ||
        (stem.size() == 4 && (stem.startsWith("COM") || stem.startsWith("LPT")) && stem[3] >= '0' && stem[3] <= '9'))
      filename.prepend('_');
    name.replace("{clip}", filename);
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
      for (auto it = archivedJobs_.begin(); it != archivedJobs_.end(); ++it) {
        auto job = it.value().toMap();
        if (job.value("deleted").toBool()) continue;
        auto items = job.value("items").toList();
        bool unfinished = false;
        for (auto &value : items) {
          auto item = value.toMap();
          if (item.value("status") == "pending" || item.value("status") == "running") {
            item["status"] = "interrupted"; value = item; unfinished = true;
          }
        }
        if (unfinished) { job["state"] = "interrupted"; job["items"] = items; }
        it.value() = job; jobs_.append(job);
      }
      if (!jobs_.isEmpty()) { job_ = jobs_.last().toMap(); jobId_ = job_.value("id").toString(); }
    } else error_ = tr("Cannot read conversion task history");
  }
  timer_.setInterval(100);
  connect(&timer_, &QTimer::timeout, this, &ImageJobs::poll);
}
void ImageJobs::saveHistory() {
  ++historyRevision_;
  historyDirty_ = true;
  timer_.start();
  pollHistory();
}
void ImageJobs::pollHistory() {
  bool notify = false;
  if (historyTask_) {
    const auto completed = historyTask_->snapshot();
    if (!completed.result) return;
    if (completed.result->error) {
      error_ = tr("Cannot persist conversion task history");
      notify = true;
    } else {
      historyPersistedRevision_ = historySubmittedRevision_;
      if (error_ == tr("Cannot persist conversion task history")) {
        error_.clear();
        notify = true;
      }
    }
    historyTask_.reset();
  }
  if (!historyDirty_) {
    if (notify) emit changed();
    return;
  }
  const auto path = historyPath_;
  const auto history = archivedJobs_;
  TaskRequest task;
  task.priority = Priority::Background;
  task.work = [path, history](std::stop_token) {
    writeHistory(path, history);
    return TaskResult{};
  };
  const auto submitted = control_.submit(std::move(task));
  if (submitted.task) {
    historyTask_ = submitted.task;
    historySubmittedRevision_ = historyRevision_;
    historyDirty_ = false;
  } // Queue pressure retains the latest snapshot for the next timer tick.
  if (notify) emit changed();
}
ImageJobs::~ImageJobs() {
  timer_.stop();
  control_.shutdown();
  // Shutdown cancels queued tasks. Flush after joining so an older asynchronous
  // snapshot cannot overwrite the final history during normal application exit.
  if (historyRevision_ != historyPersistedRevision_) {
    try { writeHistory(historyPath_, archivedJobs_); }
    catch (const std::exception &e) { qWarning("%s", e.what()); }
  }
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
  try {
    for (const auto &v : service_->job_summaries())
      if (active(v.toObject()["state"].toString())) return true;
  } catch (...) {
    // A query failure must not be mistaken for permission to discard work.
    return true;
  }
  return false;
}
void ImageJobs::preflight(const QStringList &inputs, const QVariantMap &scope,
                          const QVariantMap &options,
                          const QString &nameTemplate, const QString &conflict,
                          const QVariantList &itemOverrides, const QVariantMap &metadata) {
  prepare(inputs, scope, options, nameTemplate, conflict, itemOverrides, metadata, false);
}
void ImageJobs::startExport(const QStringList &inputs, const QVariantMap &scope,
                          const QVariantMap &options, const QString &nameTemplate,
                          const QString &conflict, const QVariantMap &metadata) {
  if (options.value("target") != "fbx") {
    error_ = tr("Direct export requires model or animation options"); emit changed(); return;
  }
  prepare(inputs, scope, options, nameTemplate, conflict, {}, metadata, true);
}
void ImageJobs::prepare(const QStringList &inputs, const QVariantMap &scope,
                          const QVariantMap &options,
                          const QString &nameTemplate, const QString &conflict,
                          const QVariantList &itemOverrides, const QVariantMap &metadata, bool direct) {
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
  if (direct) { ++jobSelectionRevision_; jobId_.clear(); job_.clear(); polledJob_ = {}; }
  const auto selectionRevision = jobSelectionRevision_;
  QElapsedTimer requested; requested.start();
  preparationClock_.start();
  const auto progress = std::make_shared<PreparationState>();
  progress->update({{"stage", "queued"}});
  preparationState_ = progress;
  preparationStatus_ = {{"active", true}, {"stage", "queued"}, {"elapsed_ms", 0},
                        {"cancellation_requested", false}};
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
               revision, selectionRevision, requested, direct, progress](std::stop_token stop) mutable {
    QJsonObject response;
    QVariantList inputInfo;
    QElapsedTimer elapsed; elapsed.start();
    try {
      progress->update({{"stage", "setup"}});
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
      if (!inputs.isEmpty() && outputRoots.size() != 1)
        throw skeletal::Failure(
            ErrorCode::InvalidArgument,
            "GUI conversion requires one explicit output directory");
      const auto setupMs = elapsed.elapsed();
      QJsonArray pending = QJsonArray::fromVariantList(metadata.value("retry_requests").toList());
      for (qsizetype i = 0; i < inputs.size(); ++i) {
        auto parameters = QJsonObject::fromVariantMap(options);
        if (!itemOverrides.isEmpty()) {
          const auto overrides = QJsonObject::fromVariantMap(itemOverrides[i].toMap());
          for (auto it = overrides.begin(); it != overrides.end(); ++it) parameters[it.key()] = it.value();
        }
        pending.append(QJsonObject{{"gui_source", QJsonObject{{"input", inputs[i]}, {"options", parameters},
          {"name_template", nameTemplate}, {"conflict", conflict}, {"output_directory", outputRoots[0]}}}});
      }
      QJsonArray items;
      QJsonArray protectedSources = QJsonArray::fromVariantList(metadata.value("protected_inputs").toList());
      QHash<QString, QJsonObject> inspections;
      std::vector<InputIdentityLease> inspectionLeases;
      const auto access = parse_image_scope(policy);
      int inspectionCount = 0;
      const auto inspectOnce = [&](const QJsonObject &q) {
        try {
        // Hold each source against write/delete for this preparation. Do not
        // share results across paths: relative auxiliary references may differ.
        const auto path = validated_local_path(std::filesystem::path(q["input"].toString().toStdWString()));
        const auto key = QString::fromStdWString(path.wstring()).toCaseFolded() + ':' + QString::number(q["subresource"].toInt());
        if (inspections.contains(key)) return inspections.value(key);
        // This lease only pins identity; inspect applies the format's decode
        // budget. An image-specific limit must not reject a larger model here.
        auto lease = InputIdentityLease::open(access, path, stop);
        ++inspectionCount;
        progress->update({{"stage", "inspect"}, {"input", q["input"]}});
        auto result = existing->request(q, stop);
        if (!result.contains("error")) {
          inspectionLeases.push_back(std::move(lease));
          inspections.insert(key, result);
        }
        return result;
        } catch (const skeletal::Failure &e) {
          if (e.error.code == ErrorCode::Cancelled) throw;
          return image_error_json(e);
        }
      };
      for (const auto &pendingValue : pending) {
        if (stop.stop_requested())
          throw skeletal::Failure(ErrorCode::Cancelled,
                                  "Plan preparation cancelled");
        const auto pendingItem = pendingValue.toObject();
        auto guiSource = pendingItem["gui_source"].toObject();
        if (guiSource.isEmpty()) { items.append(pendingItem); continue; }
        const auto input = guiSource["input"].toString();
        const auto itemTemplate = guiSource["name_template"].toString();
        const auto itemConflict = guiSource["conflict"].toString();
        const auto outputDirectory = guiSource["output_directory"].toString();
        protectedSources.append(input);
        const auto originalPair = guiSource["options"].toObject()["paired_file"].toString();
        if (!originalPair.isEmpty()) protectedSources.append(originalPair);
        const auto initialSize = items.size();
        const auto outputIndex = guiSource.contains("output_index") ? guiSource["output_index"].toInteger() : initialSize;
        guiSource["output_index"] = outputIndex;
        try {
        auto itemOptions = guiSource["options"].toObject();
        auto inspect = request("inspect");
        inspect["input"] = input;
        inspect["subresource"] = itemOptions.value("subresource").toInt();
        const auto source = inspectOnce(inspect);
        if (itemOptions["target"] == "fbx") {
          const auto content = itemOptions.take("export_content").toString("combined");
          const auto organization = itemOptions.take("file_organization").toString("per-clip");
          const auto paired = itemOptions.take("paired_file").toString();
          const auto suffix = QFileInfo(input).suffix().toLower();
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
              animation_source = inspectOnce(inspect_animation);
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
          inputInfo.append(QVariantMap{{"input", input}, {"clips", clips.toVariantList()},
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
            if (selection_explicit && !direct) continue;
            throw skeletal::Failure(ErrorCode::InvalidArgument,
                "No source animation selected; choose model-only, or supply an ActorX animation pair");
          }
          if (content == "model" && !source["has_mesh"].toBool() && !source.contains("error"))
            throw skeletal::Failure(ErrorCode::InvalidArgument, "Model-only export needs a model input");
          const auto append_model = [&](QJsonObject parameters, const QString &clip_name) {
            items.append(QJsonObject{{"input", input},
                {"output", QDir(outputDirectory).filePath(outputName(input, outputIndex + items.size() - initialSize, "fbx", itemTemplate, clip_name))},
                {"options", parameters}, {"conflict", itemConflict}});
          };
          // An ABC frame export names its single model after the sample.
          if (content == "model" || clips.isEmpty())
            append_model(itemOptions, itemOptions.contains("abc_sample")
                                          ? QStringLiteral("frame%1").arg(itemOptions["abc_sample"].toInteger())
                                          : QStringLiteral("model"));
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
            {"input", input},
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
        const auto name = outputName(input, outputIndex,
                                    itemOptions["target"].toString(), itemTemplate);
        items.append(QJsonObject{
            {"input", input},
            {"output", QDir(outputDirectory).filePath(name)},
            {"options", itemOptions},
            {"conflict", itemConflict}});
        } catch (const skeletal::Failure &e) {
          if (!direct || e.error.code == ErrorCode::Cancelled) throw;
          items.append(QJsonObject{{"input", input},
            {"output", QDir(outputDirectory).filePath(QFileInfo(input).completeBaseName() + "-failed.fbx")},
            {"options", QJsonObject{{"target", "fbx"}}}, {"conflict", itemConflict}, {"gui_source", guiSource},
            {"preparation_error", image_error_json(e)["error"]}});
        }
      }
      progress->update({{"stage", "expand"}, {"completed", items.size()}, {"total", items.size()}});
      if (items.isEmpty() && !inputs.isEmpty())
        throw skeletal::Failure(ErrorCode::InvalidArgument, "No selected source clips match the current inputs");
      if (items.isEmpty())
        response = {{"configured", true}};
      else {
        auto create = request(direct ? "create_job" : "create_plan");
        create["items"] = items;
        create["name"] = metadata.value("name").toString();
        create["category"] = metadata.value("category").toString();
        create["protected_inputs"] = protectedSources;
        if (direct) create["timings"] = QJsonObject{{"preparation_ms", requested.elapsed()},
            {"request_started_ms", requested.msecsSinceReference()}, {"dispatch_wait_ms", requested.elapsed() - elapsed.elapsed()},
            {"setup_ms", setupMs}, {"expansion_ms", elapsed.elapsed() - setupMs}, {"source_inspections", inspectionCount}};
        response = existingGroups->request(create, stop, [progress](const QJsonObject &value) { progress->update(value); });
        if (direct && stop.stop_requested() && response.contains("job_id")) {
          auto cancel = request("cancel"); cancel["kind"] = "job"; cancel["id"] = response["job_id"];
          (void)existingGroups->request(cancel);
        }
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
          [receiver, existing, existingGroups, requestedScope, response, revision, selectionRevision, inputInfo, direct] {
            if (!receiver)
              return;
            receiver->busy_ = false;
            receiver->finishPreparation();
            if (existing) {
              if (receiver->service_ != existing) {
                receiver->discoveryId_.clear();
                receiver->discovery_.clear();
                receiver->discoveryOffset_ = 0;
                if (selectionRevision == receiver->jobSelectionRevision_) receiver->jobId_.clear();
              }
              receiver->service_ = existing;
              receiver->groups_ = existingGroups;
              receiver->scope_ = requestedScope;
              receiver->timer_.start();
            }
            if (direct || revision == receiver->revision_) {
              receiver->inputInfo_ = inputInfo;
              receiver->error_ = errorText(response);
              receiver->planId_ = response["plan_id"].toString();
              if (direct && selectionRevision == receiver->jobSelectionRevision_) receiver->jobId_ = response["job_id"].toString();
            } else if (!response["plan_id"].toString().isEmpty())
              receiver->cancel("plan", response["plan_id"].toString());
            receiver->poll();
            if (direct) emit receiver->directExportFinished(response["job_id"].toString());
            emit receiver->changed();
          },
          Qt::QueuedConnection);
    return TaskResult{};
  };
  const auto submitted = control_.submit(std::move(task));
  preparation_ = submitted.task;
  if (!submitted.task) {
    busy_ = false;
    finishPreparation();
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
  timer_.start(); // Queued cancellation has no worker completion callback.
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
  if (preparationState_) {
    preparationStatus_["cancellation_requested"] = true;
    emit changed();
  }
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
  const auto selectionRevision = jobSelectionRevision_;
  QPointer<ImageJobs> receiver(this);
  TaskRequest task;
  task.priority = Priority::Foreground;
  task.work = [service, groups, receiver, q, selectionRevision](std::stop_token stop) {
    const auto id = q.value("plan_id").toString(q.value("id").toString());
    const auto response = groups && groups->contains(id) ? groups->request(q, stop)
                                                        : service->request(q, stop);
    if (receiver)
      QMetaObject::invokeMethod(
          receiver,
          [receiver, response, q, selectionRevision] {
            if (!receiver)
              return;
            receiver->busy_ = false;
            receiver->error_ = errorText(response);
            if (response.contains("plan_id") && !response.contains("job_id")) {
              receiver->planId_ = response["plan_id"].toString();
              receiver->idempotency_.clear();
            }
            if (response.contains("job_id") && selectionRevision == receiver->jobSelectionRevision_)
              receiver->jobId_ = response["job_id"].toString();
            if (response.contains("discovery_id")) {
              receiver->discoveryId_ = response["discovery_id"].toString();
              receiver->discoveryOffset_ = 0;
            }
            receiver->poll();
            if (q["operation"] == "submit")
              emit receiver->submissionFinished(q["plan_id"].toString(), response["job_id"].toString());
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
  ++jobSelectionRevision_;
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
  if (busy_) return;
  const auto previous = jobSnapshot(id);
  const auto previousItems = previous.value("items").toList();
  const bool needsExpansion = std::any_of(previousItems.begin(), previousItems.end(), [](const auto &v) {
    return v.toMap().value("request").toMap().contains("gui_source");
  });
  if (!groups_ || !groups_->contains(id) || needsExpansion) {
    if (previous.isEmpty() || active(previous.value("state").toString())) return;
    QVariantList requests, protectedInputs;
    QStringList outputs;
    for (const auto &value : previous.value("items").toList()) {
      const auto item = value.toMap(), original = item.value("request").toMap();
      protectedInputs.append(original.value("input"));
      const auto options = original.value("options").toMap();
      const auto guiPair = original.value("gui_source").toMap().value("options").toMap().value("paired_file").toString();
      if (!guiPair.isEmpty()) protectedInputs.append(guiPair);
      for (const auto *key : {"reference", "animation"})
        if (!options.value(key).toString().isEmpty()) protectedInputs.append(options.value(key));
      const auto approved = item.value("preflight").toMap();
      for (const auto *key : {"sources", "referenced_sources"})
        for (const auto &source : approved.value(key).toList()) {
          const auto path = source.metaType().id() == QMetaType::QString
                                ? source.toString() : source.toMap().value("path").toString();
          if (!path.isEmpty()) protectedInputs.append(path);
        }
      if (item.value("status") == "succeeded" || item.value("status") == "skipped") continue;
      auto retryRequest = original;
      retryRequest.remove("preparation_error");
      requests.append(retryRequest);
      outputs.append(QFileInfo(original.value("output").toString()).absolutePath());
    }
    if (requests.isEmpty()) return;
    outputs.removeDuplicates();
    auto retryScope = previous.value("scope").toMap();
    if (retryScope.isEmpty() && groups_ && groups_->contains(id)) retryScope = scope_;
    if (retryScope.isEmpty()) retryScope = {{"input_roots", localInputRoots()}, {"output_roots", outputs}, {"allow_overwrite", false}};
    preflight({}, retryScope, {}, {}, "skip", {},
              {{"name", previous.value("name")}, {"category", previous.value("category")},
               {"retry_requests", requests}, {"protected_inputs", protectedInputs}});
    return;
  }
  invalidatePlan();
  auto q = request("retry_plan");
  q["id"] = id;
  dispatch(q);
}
void ImageJobs::pauseJob(const QString &id, bool pause) {
  auto q = request(pause ? "pause" : "resume"); q["id"] = id;
  dispatch(q);
}
void ImageJobs::removeJob(const QString &id) {
  if (busy_ || id.isEmpty()) return;
  const auto found = std::find_if(jobs_.cbegin(), jobs_.cend(), [&](const auto &v) { return v.toMap().value("id") == id; });
  if (found == jobs_.cend() || active(found->toMap().value("state").toString())) return;
  // A durable tombstone hides live-service and restored snapshots alike.
  // Exported files and service recovery journals are never removed here.
  archivedJobs_[id] = QVariantMap{{"id", id}, {"deleted", true}};
  jobs_.removeIf([&](const auto &v) { return v.toMap().value("id") == id; });
  if (jobId_ == id) {
    jobId_.clear(); job_.clear(); polledJob_ = {};
    if (!jobs_.isEmpty()) { job_ = jobs_.first().toMap(); jobId_ = job_.value("id").toString(); }
  }
  saveHistory(); poll(); emit changed();
}
QVariantMap ImageJobs::jobSnapshot(const QString &id) const {
  if (id.isEmpty()) return {};
  const auto archived = archivedJobs_.value(id).toMap();
  if (archived.value("deleted").toBool()) return {};
  if (id == jobId_) return job_;
  if (!service_) return archived;
  auto q = request("get_job");
  q["id"] = id;
  q["limit"] = 100;
  const bool grouped = groups_ && groups_->contains(id);
  auto result = grouped ? groups_->request(q) : service_->request(q);
  if (result.contains("error")) return archived;
  auto items = result["items"].toArray();
  while (!grouped && !result["next_offset"].isNull()) {
    q["offset"] = result["next_offset"];
    const auto page = service_->request(q);
    if (page.contains("error")) return {};
    for (const auto &item : page["items"].toArray()) items.append(item);
    result["next_offset"] = page["next_offset"];
  }
  result["items"] = items;
  return result.toVariantMap();
}
void ImageJobs::selectJob(const QString &id) {
  ++jobSelectionRevision_;
  jobId_ = id;
  if (!service_ && archivedJobs_.contains(id)) { job_ = archivedJobs_.value(id).toMap(); emit changed(); return; }
  poll();
}
void ImageJobs::poll() {
  pollHistory();
  std::erase_if(retiredEndpoints_, [](const auto &e) { return e->idle(); });
  if (busy_ && preparation_) {
    const auto state = preparation_->snapshot();
    if (state.result && state.result->error) {
      busy_ = false;
      finishPreparation();
      error_ = QString::fromStdString(state.result->error->message);
      emit changed();
    }
  }
  if (busy_ && preparationState_) {
    QVariantMap status;
    { const std::lock_guard lock(preparationState_->mutex); status = preparationState_->status; }
    status["active"] = true;
    status["elapsed_ms"] = preparationClock_.elapsed();
    status["cancellation_requested"] = preparationStatus_.value("cancellation_requested");
    preparationStatus_ = std::move(status);
    emit changed();
  }
  if (!service_) {
    if (!busy_ && retiredEndpoints_.empty() && !historyTask_ && !historyDirty_) timer_.stop();
    return;
  }
  QVariantList rows;
  try { rows = service_->job_summaries().toVariantList(); }
  catch (const std::exception &error) {
    error_ = QString::fromUtf8(error.what());
    emit changed();
    return;
  }
  if (groups_) rows.append(groups_->request(request("list_jobs"))["items"].toArray().toVariantList());
  rows.removeIf([&](const auto &v) { return archivedJobs_.value(v.toMap().value("id").toString()).toMap().value("deleted").toBool(); });
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
    // Failed jobs can legitimately carry a top-level error. Only reject an
    // error response that is not the requested job snapshot.
    if (completed.value("id").toString() != id ||
        completed.value("kind").toString() != "job") continue;
    completed["archived"] = true;
    completed["scope"] = scope_;
    archivedJobs_[id] = completed;
    historyChanged = true;
  }
  if (historyChanged) saveHistory();
  for (auto it = archivedJobs_.cbegin(); it != archivedJobs_.cend(); ++it)
    if (!liveIds.contains(it.key()) && !it.value().toMap().value("deleted").toBool()) rows.append(it.value());
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
      busy_ || historyTask_ || historyDirty_ || !retiredEndpoints_.empty() || !discoveryId_.isEmpty() ||
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
