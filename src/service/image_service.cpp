#include "formaview/image_endpoint.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <iostream>

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  using namespace formaview;
  try {
    const auto arguments = app.arguments();
    if (arguments.size() != 7 || arguments[1] != "--scope" ||
        arguments[3] != "--journal" || arguments[5] != "--endpoint")
      throw skeletal::Failure(ErrorCode::InvalidArgument,
                              "Usage: formaview_image_service --scope JSON "
                              "--journal DIRECTORY --endpoint LOCAL_NAME");
    QFile file(arguments[2]);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
      throw skeletal::Failure(ErrorCode::Io, "Cannot read bounded scope");
    QJsonParseError error{};
    const auto config = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !config.isObject())
      throw skeletal::Failure(ErrorCode::InvalidArgument, "Invalid scope JSON");
    const auto scope = imaging::parse_image_scope(config.object());
    auto service = std::make_shared<imaging::ImageBatchService>(
        scope, std::filesystem::path(arguments[4].toStdWString()));
    imaging::ImageEndpoint endpoint(service, arguments[6]);
    std::cout << QJsonDocument(QJsonObject{{"version", 1},
                                           {"endpoint", arguments[6]},
                                           {"ready", true}})
                     .toJson(QJsonDocument::Compact)
                     .constData()
              << std::endl;
    return app.exec();
  } catch (const skeletal::Failure &error) {
    std::cerr << QJsonDocument(imaging::image_error_json(error))
                     .toJson(QJsonDocument::Compact)
                     .constData()
              << std::endl;
  } catch (const std::exception &error) {
    std::cerr << error.what() << std::endl;
  }
  return 1;
}