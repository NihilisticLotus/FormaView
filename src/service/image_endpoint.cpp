#include "formaview/image_endpoint.hpp"
#include "formaview/scheduler.hpp"
#include <QJsonDocument>
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
  Scheduler control{limits()};
  QLocalServer server;
  int clients{};
  std::vector<TaskHandle> requests;
  Impl(ImageEndpoint *owner, std::shared_ptr<ImageBatchService> s,
       const QString &name)
      : service(std::move(s)) {
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
                    const auto result = service->request(request);
                    if (operation == "cancel" && owner->workChanged)
                      owner->workChanged();
                    respond(socket, result);
                    return;
                  }
                  QPointer<QLocalSocket> receiver(socket);
                  TaskRequest task;
                  task.resources = {1, 1, 3ULL * 1024 * 1024 * 1024, 0};
                  task.work = [s = service, owner, receiver,
                               request](std::stop_token stop) {
                    const auto result = s->request(request, stop);
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
                             const QString &name, QObject *parent)
    : QObject(parent),
      impl_(std::make_unique<Impl>(this, std::move(service), name)) {}
ImageEndpoint::~ImageEndpoint() = default;
void ImageEndpoint::stop() { impl_->stop(); }
bool ImageEndpoint::idle() const {
  const auto s = impl_->control.stats();
  return !s.queued && !s.running;
}
} // namespace formaview::imaging
