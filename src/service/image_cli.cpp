#include "formaview/image_requests.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalSocket>
#include <iostream>

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QJsonObject result;
  try {
    const auto arguments = app.arguments();
    if (arguments.size() != 3 ||
        (arguments[1] != "--scope" && arguments[1] != "--endpoint"))
      throw formaview::skeletal::Failure(
          formaview::ErrorCode::InvalidArgument,
          "Usage: formaview_image_cli --scope EXPLICIT_SCOPE_JSON or "
          "--endpoint LOCAL_NAME; one "
          "versioned request on stdin");
    QJsonParseError parse{};
    std::string line;
    for (char c; std::cin.get(c) && c != '\n';) {
      if (line.size() >= 1024 * 1024)
        throw formaview::skeletal::Failure(formaview::ErrorCode::ResourceLimit,
                                           "Request exceeds one MiB");
      line += c;
    }
    const auto request =
        QJsonDocument::fromJson(QByteArray::fromStdString(line), &parse);
    if (parse.error != QJsonParseError::NoError || !request.isObject())
      throw formaview::skeletal::Failure(formaview::ErrorCode::InvalidArgument,
                                         "Invalid request JSON");
    if (arguments[1] == "--endpoint") {
      QLocalSocket socket;
      socket.connectToServer(arguments[2]);
      if (!socket.waitForConnected(3000))
        throw formaview::skeletal::Failure(formaview::ErrorCode::Io,
                                           "Cannot connect to image service");
      const auto bytes = request.toJson(QJsonDocument::Compact) + '\n';
      if (socket.write(bytes) != bytes.size() ||
          !socket.waitForBytesWritten(3000))
        throw formaview::skeletal::Failure(formaview::ErrorCode::Io,
                                           "Cannot send service request");
      QByteArray response;
      socket.setReadBufferSize(4 * 1024 * 1024 + 1);
      while (!response.contains('\n')) {
        if (!socket.bytesAvailable() && !socket.waitForReadyRead(300000))
          throw formaview::skeletal::Failure(formaview::ErrorCode::Io,
                                             "Service response interrupted");
        response += socket.readAll();
        if (response.size() > 4 * 1024 * 1024)
          throw formaview::skeletal::Failure(
              formaview::ErrorCode::ResourceLimit,
              "Service response exceeds four MiB");
      }
      const auto reply = QJsonDocument::fromJson(response, &parse);
      if (parse.error != QJsonParseError::NoError || !reply.isObject())
        throw formaview::skeletal::Failure(formaview::ErrorCode::Io,
                                           "Invalid service response");
      result = reply.object();
    } else {
      QFile config(arguments[2]);
      if (!config.open(QIODevice::ReadOnly) || config.size() > 1024 * 1024)
        throw formaview::skeletal::Failure(
            formaview::ErrorCode::Io,
            "Cannot read bounded scope configuration");
      const auto policy = QJsonDocument::fromJson(config.readAll(), &parse);
      if (parse.error != QJsonParseError::NoError || !policy.isObject())
        throw formaview::skeletal::Failure(
            formaview::ErrorCode::InvalidArgument, "Invalid scope JSON");
      result = formaview::imaging::execute_image_request(
          request.object(),
          formaview::imaging::parse_image_scope(policy.object()));
    }
  } catch (const formaview::skeletal::Failure &error) {
    result = formaview::imaging::image_error_json(error);
  } catch (const std::exception &error) {
    result = formaview::imaging::image_error_json(formaview::skeletal::Failure(
        formaview::ErrorCode::Internal, error.what()));
  }
  std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData()
            << '\n';
  return result.contains("error") ? 1 : 0;
}
