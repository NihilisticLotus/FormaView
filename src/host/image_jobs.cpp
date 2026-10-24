#include "image_jobs.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>

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
QVariantMap ImageJobs::nameExamples(const QStringList &inputs,
                                    const QString &nameTemplate,
                                    const QString &target,
                                    const QVariantList &itemOverrides) const {
  if (inputs.size() > 64 ||
      (!itemOverrides.isEmpty() && itemOverrides.size() != inputs.size()))
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
  timer_.setInterval(100);
  connect(&timer_, &QTimer::timeout, this, &ImageJobs::poll);
}
ImageJobs::~ImageJobs() {
  control_.shutdown();
  endpoint_.reset();
  retiredEndpoints_.clear();
  service_.reset();
}
bool ImageJobs::activeJobs() const {
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
                          const QVariantList &itemOverrides) {
  if (busy_)
    return;
  if (inputs.size() > 64) {
    error_ = tr("Select up to 64 inputs");
    emit changed();
    return;
  }
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
  const auto base = qEnvironmentVariable(
      "FORMAVIEW_IMAGE_STATE_ROOT",
      QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
          "/image-journal");
  const auto journalRoot = base + "/" + policy["policy_revision"].toString();
  QPointer<ImageJobs> receiver(this);
  TaskRequest task;
  task.priority = Priority::Foreground;
  task.resources = {1, 1, 3ULL * 1024 * 1024 * 1024, 0};
  task.work = [receiver, existing, inputs, policy, requestedScope, options,
               nameTemplate, conflict, journalRoot, itemOverrides,
               revision](std::stop_token stop) mutable {
    QJsonObject response;
    QVariantList inputInfo;
    try {
      if (!existing) {
        if (!QDir().mkpath(journalRoot))
          throw skeletal::Failure(ErrorCode::Io,
                                  "Cannot create image journal directory");
        existing = std::make_shared<ImageBatchService>(
            parse_image_scope(policy),
            std::filesystem::path(journalRoot.toStdWString()));
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
          const bool selection_explicit = itemOptions.contains("selected_clips");
          const auto selection = itemOptions.take("selected_clips").toArray();
          if (content != "model" && content != "combined" && content != "animation")
            throw skeletal::Failure(ErrorCode::InvalidArgument, "Unknown export content");
          if (organization != "per-clip" && organization != "multiple-stacks")
            throw skeletal::Failure(ErrorCode::InvalidArgument, "Unknown file organization");
          auto animation_source = source;
          if (content == "combined" && !source["has_mesh"].toBool() &&
              paired.isEmpty() && !source.contains("error"))
            throw skeletal::Failure(ErrorCode::InvalidArgument,
                "Model + animation export needs a paired reference model for PSA input");
          if (content != "model" && !paired.isEmpty()) {
            if (source["has_mesh"].toBool()) {
              itemOptions["animation"] = paired;
              auto inspect_animation = request("inspect");
              inspect_animation["input"] = paired;
              animation_source = existing->request(inspect_animation, stop);
            } else itemOptions["reference"] = paired;
          }
          itemOptions["include_mesh"] = content != "animation";
          if (content == "model") itemOptions.remove("pairing_policy");
          QJsonArray clips;
          if (content != "model")
            for (const auto &v : animation_source["clips"].toArray())
              if (!selection_explicit || selection.contains(v.toObject()["id"])) clips.append(v);
          inputInfo.append(QVariantMap{{"input", inputs[i]}, {"clips", clips.toVariantList()},
              {"error", animation_source["error"].toObject().toVariantMap()}});
          if (content != "model" && clips.isEmpty() && !animation_source.contains("error")) {
            if (selection_explicit) continue;
            throw skeletal::Failure(ErrorCode::InvalidArgument,
                "No source animation selected; choose model-only or pair a PSA animation");
          }
          if (content == "model" && !source["has_mesh"].toBool() && !source.contains("error"))
            throw skeletal::Failure(ErrorCode::InvalidArgument, "Model-only export needs a model input");
          const auto append_model = [&](QJsonObject parameters, const QString &clip_name) {
            if (items.size() >= 64)
              throw skeletal::Failure(ErrorCode::ResourceLimit, "Expanded plan exceeds 64 outputs; select fewer clips");
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
        response = existing->request(create, stop);
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
          [receiver, existing, requestedScope, response, revision, inputInfo] {
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
  if (inputs.isEmpty() || inputs.size() > 64) {
    error_ = tr("Select between 1 and 64 inputs"); emit changed(); return;
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
        if (source["has_mesh"].toBool() && !paired.isEmpty()) {
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
  emit changed();
  const auto service = service_;
  QPointer<ImageJobs> receiver(this);
  TaskRequest task;
  task.priority = Priority::Foreground;
  task.work = [service, receiver, q](std::stop_token stop) {
    const auto response = service->request(q, stop);
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
  dispatch(q);
}
void ImageJobs::cancel(const QString &kind, const QString &id) {
  if (!service_)
    return;
  auto q = request("cancel");
  q["kind"] = kind;
  q["id"] = id;
  error_ = errorText(service_->request(q));
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
  auto get = [&](const char *operation, const QString &id) {
    auto q = request(operation);
    q["id"] = id;
    q["limit"] = 100;
    return service_->request(q).toVariantMap();
  };
  const auto plan =
      planId_.isEmpty() ? QVariantMap{} : get("get_plan", planId_);
  const auto job = jobId_.isEmpty() ? QVariantMap{} : get("get_job", jobId_);
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
    endpoint_ = std::make_unique<ImageEndpoint>(service_, endpointName_);
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
