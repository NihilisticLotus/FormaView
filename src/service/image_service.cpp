#include "formaview/image_endpoint.hpp"
#include "formaview/image_groups.hpp"
#include <QCoreApplication>
#include <QDir>
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
    const auto groupDir = QDir::cleanPath(arguments[4]) + "-groups";
    const auto groupPath = validated_local_path(std::filesystem::path(groupDir.toStdWString()));
    std::shared_ptr<imaging::ImageTaskGroups> groups;
    if (QDir(groupDir).exists() || QDir().mkpath(groupDir)) {
      groups = std::make_shared<imaging::ImageTaskGroups>(scope, groupPath);
    } else {
      // A provisioned journal may be writable while its parent is read-only.
      std::cerr << "Grouped export storage unavailable; single batches remain available\n";
    }
    imaging::ImageEndpoint endpoint(service, arguments[6], nullptr, groups);
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