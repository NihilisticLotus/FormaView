#include "formaview/image_endpoint.hpp"
#include "formaview/image_groups.hpp"
#include "formaview/scheduler.hpp"
#include <QJsonDocument>
#include <QJsonArray>
#include <cmath>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <algorithm>

namespace formaview::imaging {
namespace {
SchedulerLimits limits() {
  SchedulerLimits l;
  l.workers = 1;
  l.queue_capacity = 16;
  l.foreground_queue_reserve = 0;
  l.foreground_worker_reserve = 0;
  l.capacity = {1, 1, 3ULL * 1024 * 1024 * 1024, 0};
  l.foreground_reserve = {0, 0, 0, 0};
  return l;
}
void respond(QLocalSocket *socket, const QJsonObject &response) {
  if (socket->state() != QLocalSocket::ConnectedState)
    return;
  const auto bytes = QJsonDocument(response).toJson(QJsonDocument::Compact);
  if (bytes.size() > 4 * 1024 * 1024) {
    socket->abort();
    return;
  }
  socket->write(bytes + '\n');
  socket->disconnectFromServer();
}
} // namespace
struct ImageEndpoint::Impl {
  std::shared_ptr<ImageBatchService> service;
  std::shared_ptr<ImageTaskGroups> groups;
  QJsonObject dispatch(const QJsonObject &request, std::stop_token stop = {}) {
    const auto operation = request["operation"].toString();
    const auto id = request.value("plan_id").toString(request.value("id").toString());
    const bool grouped = groups && groups->contains(id);
    if (!groups || (operation != "list_jobs" && !grouped)) return service->request(request, stop);
    if (grouped && !QStringList{"get_plan", "get_job", "list_jobs", "cancel",
                                "submit", "retry_plan", "pause", "resume"}.contains(operation))
      return image_error_json(skeletal::Failure(ErrorCode::InvalidArgument,
                                                "Operation is not available through the grouped endpoint"));
    auto result = grouped ? groups->request(request, stop) : QJsonObject{};
    if (operation != "list_jobs" && operation != "get_job" && operation != "get_plan") return result;
    if (result.contains("error")) return result;
    const auto offset = request.value("offset").toDouble(0), limit = request.value("limit").toDouble(100);
    if ((request.contains("offset") && !request["offset"].isDouble()) ||
        (request.contains("limit") && !request["limit"].isDouble()) ||
        !std::isfinite(offset) || !std::isfinite(limit) || offset < 0 ||
        limit < 1 || limit > 100 || std::floor(offset) != offset || std::floor(limit) != limit)
      return image_error_json(skeletal::Failure(ErrorCode::InvalidArgument, "Invalid result page"));
    QJsonArray all;
    if (operation == "list_jobs") {
      auto page = request; page["offset"] = 0; page["limit"] = 1;
      result = service->request(page, stop);
      if (result.contains("error")) return result;
      const auto groupedResult = groups->request(request, stop);
      if (groupedResult.contains("error")) return groupedResult;
      const auto groupItems = groupedResult["items"].toArray();
      auto ordinaryCount = result["total"].toInteger();
      auto total = ordinaryCount + groupItems.size();
      if (offset > total)
        return image_error_json(skeletal::Failure(ErrorCode::InvalidArgument, "Invalid result page"));
      const auto start = static_cast<qint64>(offset);
      if (start < ordinaryCount) {
        page["offset"] = start; page["limit"] = static_cast<int>(limit);
        const auto ordinary = service->request(page, stop);
        if (ordinary.contains("error")) return ordinary;
        ordinaryCount = ordinary["total"].toInteger();
        total = ordinaryCount + groupItems.size();
        for (const auto &row : ordinary["items"].toArray()) all.append(row);
      }
      for (auto i = std::max<qint64>(0, start - ordinaryCount);
           i < groupItems.size() && all.size() < limit; ++i)
        all.append(groupItems[i]);
      const auto end = start + all.size();
      result["items"] = all; result["total"] = total;
      result["next_offset"] = end < total ? QJsonValue(end) : QJsonValue(QJsonValue::Null);
      return result;
    } else all = result["items"].toArray();
    if (offset > all.size())
      return image_error_json(skeletal::Failure(ErrorCode::InvalidArgument, "Invalid result page"));
    QJsonArray page;
    const auto end = std::min(all.size(), static_cast<qsizetype>(offset + limit));
    for (auto n = static_cast<qsizetype>(offset); n < end; ++n) page.append(all[n]);
    result["items"] = page; result["total"] = static_cast<qint64>(all.size());
    result["next_offset"] = end < all.size() ? QJsonValue(static_cast<qint64>(end)) : QJsonValue(QJsonValue::Null);
    return result;
  }
  Scheduler control{limits()};
  QLocalServer server;
  int clients{};
  std::vector<TaskHandle> requests;
  Impl(ImageEndpoint *owner, std::shared_ptr<ImageBatchService> s,
       const QString &name, std::shared_ptr<ImageTaskGroups> g)
      : service(std::move(s)), groups(std::move(g)) {
    if (!QRegularExpression("^[A-Za-z0-9_-]{1,128}$").match(name).hasMatch())
      throw skeletal::Failure(ErrorCode::InvalidArgument,
                              "Invalid local endpoint name");
    server.setSocketOptions(QLocalServer::UserAccessOption);
    server.setMaxPendingConnections(16);
    if (!server.listen(name))
      throw skeletal::Failure(ErrorCode::Conflict,
                              "Cannot own local image endpoint");
    QObject::connect(
        &server, &QLocalServer::newConnection, owner, [this, owner] {
          while (server.hasPendingConnections()) {
            auto *socket = server.nextPendingConnection();
            if (clients >= 32) {
              socket->abort();
              socket->deleteLater();
              continue;
            }
            ++clients;
            socket->setReadBufferSize(1024 * 1024 + 1);
            QObject::connect(socket, &QLocalSocket::disconnected, &server,
                             [this, socket] {
                               --clients;
                               socket->deleteLater();
                             });
            auto buffer = std::make_shared<QByteArray>();
            auto submitted = std::make_shared<bool>(false);
            QTimer::singleShot(10000, socket, [socket, submitted] {
              if (!*submitted)
                socket->abort();
            });
            QObject::connect(
                socket, &QLocalSocket::readyRead, owner,
                [this, owner, socket, buffer, submitted] {
                  if (*submitted) {
                    socket->abort();
                    return;
                  }
                  buffer->append(socket->readAll());
                  if (buffer->size() > 1024 * 1024) {
                    socket->abort();
                    return;
                  }
                  if (!buffer->contains('\n'))
                    return;
                  *submitted = true;
                  QJsonParseError error;
                  const auto document =
                      QJsonDocument::fromJson(*buffer, &error);
                  if (error.error != QJsonParseError::NoError ||
                      !document.isObject()) {
                    respond(socket, image_error_json(skeletal::Failure(
                                        ErrorCode::InvalidArgument,
                                        "Invalid bounded request JSON")));
                    return;
                  }
                  auto request = document.object();
                  const auto client = request.take("_mcp_client").toObject();
                  if (!client.isEmpty() && owner->observedClient &&
                      QJsonDocument(client)
                              .toJson(QJsonDocument::Compact)
                              .size() <= 2048)
                    owner->observedClient(client);
                  const auto operation = request["operation"].toString();
                  if (QStringList{"get_plan", "get_job", "list_jobs",
                                  "get_discovery", "cancel"}
                          .contains(operation)) {
                    const auto result = dispatch(request);
                    if (operation == "cancel" && owner->workChanged)
                      owner->workChanged();
                    respond(socket, result);
                    return;
                  }
                  QPointer<QLocalSocket> receiver(socket);
                  TaskRequest task;
                  task.resources = {1, 1, 3ULL * 1024 * 1024 * 1024, 0};
                  task.work = [this, owner, receiver,
                               request](std::stop_token stop) {
                    const auto result = dispatch(request, stop);
                    QMetaObject::invokeMethod(
                        owner,
                        [owner, receiver, result, operation = request.value("operation").toString()] {
                          if (QStringList{"create_plan", "submit", "retry_plan", "discover"}
                                  .contains(operation) && owner->workChanged)
                            owner->workChanged();
                          if (receiver)
                            respond(receiver, result);
                        },
                        Qt::QueuedConnection);
                    return TaskResult{};
                  };
                  const auto accepted = control.submit(std::move(task));
                  if (!accepted.task)
                    respond(socket, image_error_json(skeletal::Failure(
                                        accepted.error->code,
                                        accepted.error->message)));
                  else {
                    std::erase_if(requests, [](const auto &r) {
                      return r.snapshot().result.has_value();
                    });
                    requests.push_back(*accepted.task);
                  }
                });
          }
        });
  }
  void stop() {
    server.close();
    for (auto *socket : server.findChildren<QLocalSocket *>()) {
      socket->disconnect(&server);
      socket->abort();
    }
    for (const auto &r : requests)
      r.cancel();
  }
  ~Impl() {
    stop();
    control.shutdown();
  }
};
ImageEndpoint::ImageEndpoint(std::shared_ptr<ImageBatchService> service,
                             const QString &name, QObject *parent, std::shared_ptr<ImageTaskGroups> groups)
    : QObject(parent),
      impl_(std::make_unique<Impl>(this, std::move(service), name, std::move(groups))) {}
ImageEndpoint::~ImageEndpoint() = default;
void ImageEndpoint::stop() { impl_->stop(); }
bool ImageEndpoint::idle() const {
  const auto s = impl_->control.stats();
  return !s.queued && !s.running;
}
} // namespace formaview::imaging
