#include "model_texture.hpp"
#include "formaview/skeletal.hpp"
#ifdef FORMAVIEW_MODEL_IMAGES
#include "formaview/pro_image.hpp"
#ifdef FORMAVIEW_MODEL_IMAGE_DOCUMENT
#include "formaview/image_conversion.hpp"
#endif
#endif
#include <QBuffer>
#include <QImageReader>

namespace {
void check(std::stop_token stop) {
  if (stop.stop_requested())
    throw formaview::skeletal::Failure(formaview::ErrorCode::Cancelled,
                                       "Texture read cancelled");
}
} // namespace
std::shared_ptr<formaview::render::Texture>
readModelTexture(const QString &path, std::stop_token stop) {
#ifdef FORMAVIEW_MODEL_IMAGES
  formaview::imaging::Limits limits;
  limits.dimension = 4096;
  limits.decoded_bytes = 256ULL * 1024 * 1024;
  auto file =
#ifdef FORMAVIEW_MODEL_IMAGE_DOCUMENT
      formaview::imaging::ImageDocument::open(
#else
      formaview::imaging::ImageFile::open(
#endif
          std::filesystem::path(path.toStdWString()), limits, stop);
  auto pixels = file.decode(0, stop);
  check(stop);
  auto texture = std::make_shared<formaview::render::Texture>();
  texture->width = static_cast<int>(pixels.data_window.width);
  texture->height = static_cast<int>(pixels.data_window.height);
  texture->rgba = formaview::imaging::display_rgba(pixels, {});
  check(stop);
  return texture;
#else
  Q_UNUSED(path);
  Q_UNUSED(stop);
  throw formaview::skeletal::Failure(formaview::ErrorCode::Unsupported,
                                     "Image adapter unavailable");
#endif
}
std::shared_ptr<formaview::render::Texture>
decodeModelTexture(const QByteArray &encoded, std::stop_token stop) {
  check(stop);
  QBuffer source;
  source.setData(encoded);
  source.open(QIODevice::ReadOnly);
  QImageReader reader(&source);
  const auto size = reader.size();
  if (!size.isValid() || size.width() > 4096 || size.height() > 4096)
    throw formaview::skeletal::Failure(
        formaview::ErrorCode::ResourceLimit,
        "Embedded texture exceeds preview dimensions");
  const auto pixels = reader.read().convertToFormat(QImage::Format_RGBA8888);
  if (pixels.isNull())
    throw formaview::skeletal::Failure(formaview::ErrorCode::InvalidArgument,
                                       "Embedded texture cannot decode");
  auto texture = std::make_shared<formaview::render::Texture>();
  texture->width = pixels.width();
  texture->height = pixels.height();
  texture->rgba.resize(static_cast<std::size_t>(pixels.width()) *
                       pixels.height() * 4);
  for (int y = 0; y < pixels.height(); ++y) {
    check(stop);
    const auto *row = pixels.constScanLine(y);
    for (int x = 0; x < pixels.width() * 4; ++x)
      texture->rgba[static_cast<std::size_t>(y) * pixels.width() * 4 +
                    static_cast<std::size_t>(x)] = row[x] / 255.F;
  }
  return texture;
}
