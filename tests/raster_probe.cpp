#include "formaview/extended_raster.hpp"
#include "formaview/raster_image.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>
#include <variant>

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    const auto args = app.arguments();
    if (args.size() < 4 || args.size() > 5)
      throw std::runtime_error(
          "raster_probe INPUT OUTPUT_DIR ENTRY [png|jpeg|bmp]");
    const auto input = std::filesystem::path(args[1].toStdWString());
    const auto output = std::filesystem::path(args[2].toStdWString());
    std::filesystem::create_directories(output);
    const auto extension =
        QString::fromStdWString(input.extension().wstring()).toLower();
    const bool extended =
        extension == ".tif" || extension == ".tiff" || extension == ".webp";
    std::variant<formaview::imaging::RasterFile,
                 formaview::imaging::ExtendedRasterFile>
        file =
            extended
                ? decltype(file)(
                      formaview::imaging::ExtendedRasterFile::open(input))
                : decltype(file)(formaview::imaging::RasterFile::open(input));
    bool ok = false;
    const auto entry = args[3].toUInt(&ok);
    if (!ok)
      throw std::runtime_error("Invalid entry");
    auto sample = std::visit([&](auto &f) { return f.decode(entry); }, file);
    const auto &description = std::visit(
        [](auto &f) -> const formaview::ImageData & { return f.description(); },
        file);
    QJsonArray channels;
    for (std::size_t i = 0; i < sample.channels.size(); ++i) {
      const auto &c = sample.channels[i];
      const auto name = "channel-" + std::to_string(i) + ".bin";
      QFile out(QString::fromStdWString((output / name).wstring()));
      if (!out.open(QIODevice::WriteOnly) ||
          out.write(reinterpret_cast<const char *>(c.samples.bytes.data()),
                    static_cast<qint64>(c.samples.bytes.size())) !=
              static_cast<qint64>(c.samples.bytes.size()))
        throw std::runtime_error("Channel output failed");
      channels.append(
          QJsonObject{{"name", QString::fromStdString(c.name)},
                      {"scalar", static_cast<int>(c.samples.scalar)},
                      {"encoding", static_cast<int>(c.encoding)},
                      {"file", QString::fromStdString(name)}});
    }
    QJsonObject properties;
    for (const auto &[k, v] : description.source.properties)
      properties[QString::fromStdString(k)] = QString::fromStdString(v);
    QJsonObject result{
        {"channels", channels},
        {"width", static_cast<qint64>(sample.data_window.width)},
        {"height", static_cast<qint64>(sample.data_window.height)},
        {"entries", static_cast<qint64>(description.subresources.size())},
        {"alpha", static_cast<int>(description.alpha)},
        {"metadata", properties}};
    if (args.size() == 5) {
      std::vector<std::byte> bytes;
      if (args[4] == "tiff" || args[4] == "webp") {
        formaview::imaging::ExtendedEncodeOptions options;
        options.alpha = description.alpha;
        options.target = args[4] == "tiff"
                             ? formaview::imaging::ExtendedRasterTarget::Tiff
                             : formaview::imaging::ExtendedRasterTarget::Webp;
        bytes = formaview::imaging::encode_extended(sample, options);
      } else {
        formaview::imaging::RasterEncodeOptions options;
        options.alpha = description.alpha;
        if (args[4] == "jpeg")
          options.target = formaview::imaging::RasterTarget::Jpeg;
        else if (args[4] == "bmp")
          options.target = formaview::imaging::RasterTarget::Bmp;
        else if (args[4] != "png")
          throw std::runtime_error("Unknown export format");
        bytes = formaview::imaging::encode_raster(sample, options);
      }
      const auto name = "export." + args[4];
      QFile out(QString::fromStdWString(output.wstring()) + "/" + name);
      if (!out.open(QIODevice::WriteOnly) ||
          out.write(reinterpret_cast<const char *>(bytes.data()),
                    static_cast<qint64>(bytes.size())) !=
              static_cast<qint64>(bytes.size()))
        throw std::runtime_error("Encoded output failed");
      result["export"] = name;
    }
    QFile json(QString::fromStdWString((output / "result.json").wstring()));
    if (!json.open(QIODevice::WriteOnly))
      throw std::runtime_error("JSON output failed");
    json.write(QJsonDocument(result).toJson());
    std::cout << "PASS\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
