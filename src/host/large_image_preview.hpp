#pragma once
#include "image_session.hpp"
#include <QColorSpace>
#include <QCryptographicHash>
#include <QFile>
#include <QImageReader>
#include <QtEndian>
#include <algorithm>
#include <stdexcept>

// Preview-only reader. Full-quality conversion keeps using ImageDocument.
// JPEG uses the codec's scaled/clip decode; uncompressed TGA reads only the
// selected rows/pixels. Neither path retains a full-resolution source plane.
namespace large_preview {
inline QSize size(const QString &path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return {};
  const auto h = f.read(18);
  if (h.size() == 18 && path.endsWith(".tga", Qt::CaseInsensitive) &&
      h[1] == 0 &&
      ((h[2] == 2 && (h[16] == 24 || h[16] == 32)) ||
       (h[2] == 3 && h[16] == 8)) &&
      (static_cast<unsigned char>(h[17]) & 15) <= 8 &&
      !(static_cast<unsigned char>(h[17]) & 0xc0))
    return {qFromLittleEndian<quint16>(h.constData() + 12),
            qFromLittleEndian<quint16>(h.constData() + 14)};
  QImageReader reader(path);
  if (reader.format() == "jpeg")
    return reader.size();
  return {};
}
inline std::shared_ptr<ImagePixels> read(const QString &path, QSize full,
                                         QRect region, QSize target,
                                         QStringList mapping, quint64 revision,
                                         std::stop_token stop,
                                         bool identity = true) {
  using namespace formaview;
  const auto check = [&] {
    if (stop.stop_requested())
      throw skeletal::Failure(ErrorCode::Cancelled, "Preview cancelled");
  };
  check();
  region = region.isEmpty() ? QRect(QPoint(), full)
                            : region.intersected(QRect(QPoint(), full));
  if (region.isEmpty())
    throw std::runtime_error("Empty preview region");
  target = region.size().scaled(target.boundedTo(QSize(2048, 2048)),
                                Qt::KeepAspectRatio);
  target = target.boundedTo(region.size());
  target = target.expandedTo(QSize(1, 1));
  QImage image;
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    throw std::runtime_error("Cannot read preview");
  const auto h = f.read(18);
  const bool tga = path.endsWith(".tga", Qt::CaseInsensitive);
  bool alpha = false, gray = false;
  if (tga) {
    const int bytes = static_cast<unsigned char>(h[16]) / 8;
    gray = bytes == 1;
    alpha = bytes == 4 && (static_cast<unsigned char>(h[17]) & 15);
    const qint64 start = 18 + static_cast<unsigned char>(h[0]);
    if (f.size() < start + qint64(full.width()) * full.height() * bytes)
      throw std::runtime_error("Truncated TGA pixels");
    image = QImage(target, QImage::Format_RGBA8888);
    if (image.isNull())
      throw std::runtime_error("Preview allocation failed");
    for (int y = 0; y < target.height(); ++y) {
      check();
      const int sy = region.y() + qint64(y) * region.height() / target.height();
      const int row = (static_cast<unsigned char>(h[17]) & 32)
                          ? sy
                          : full.height() - 1 - sy;
      if (!f.seek(start + qint64(row) * full.width() * bytes))
        throw std::runtime_error("TGA seek failed");
      const auto data = f.read(qint64(full.width()) * bytes);
      if (data.size() != qint64(full.width()) * bytes)
        throw std::runtime_error("Short TGA row");
      auto *out = image.scanLine(y);
      for (int x = 0; x < target.width(); ++x) {
        int sx = region.x() + qint64(x) * region.width() / target.width();
        if (static_cast<unsigned char>(h[17]) & 16)
          sx = full.width() - 1 - sx;
        const auto *p =
            reinterpret_cast<const unsigned char *>(data.constData()) +
            sx * bytes;
        out[4 * x] = gray ? p[0] : p[2];
        out[4 * x + 1] = gray ? p[0] : p[1];
        out[4 * x + 2] = p[0];
        out[4 * x + 3] = alpha ? p[3] : 255;
      }
    }
  } else {
    QImageReader reader(path);
    reader.setAutoTransform(false);
    // Clip before scale, so zoomed regions do not allocate the entire image.
    reader.setClipRect(region);
    reader.setScaledSize(target);
    image = reader.read();
    if (image.isNull())
      throw std::runtime_error(reader.errorString().toStdString());
    if (image.format() == QImage::Format_CMYK8888)
      throw std::runtime_error("CMYK JPEG requires a separate colour policy");
    gray = image.format() == QImage::Format_Grayscale8;
    image = image.convertToFormat(QImage::Format_RGBA8888);
    if (image.size() != target)
      throw std::runtime_error("Preview codec ignored requested dimensions");
  }
  check();
  auto p = std::make_shared<ImagePixels>();
  p->path = path;
  p->revision = revision;
  p->previewFull = full;
  p->previewRegion = region;
  p->source.data_window = p->source.display_window = {
      0, 0, quint64(target.width()), quint64(target.height())};
  QStringList names = gray ? QStringList{"Y"} : QStringList{"R", "G", "B"};
  if (alpha)
    names.append("A");
  if (mapping.isEmpty())
    mapping = gray ? QStringList{"Y", "Y", "Y", "A"}
                   : QStringList{"R", "G", "B", "A"};
  if (mapping.size() != 4)
    throw std::runtime_error("Preview requires four channel mappings");
  for (const auto &name : names) {
    ImageChannel c;
    c.name = name.toStdString();
    c.encoding = SampleEncoding::UNorm;
    c.samples.scalar = ScalarType::UInt8;
    c.samples.element_count = quint64(target.width()) * target.height();
    c.samples.bytes.resize(c.samples.element_count);
    const int component = name == "A"   ? 3
                          : name == "G" ? 1
                          : name == "B" ? 2
                                        : 0;
    for (int y = 0; y < target.height(); ++y) {
      check();
      for (int x = 0; x < target.width(); ++x)
        c.samples.bytes[std::size_t(y) * target.width() + x] =
            std::byte(image.constScanLine(y)[4 * x + component]);
    }
    const int index = int(p->source.channels.size());
    for (int n = 0; n < 4; ++n)
      if (mapping[n] == name)
        p->mapping[n] = index;
    p->bytes += c.samples.bytes.size();
    p->source.channels.push_back(std::move(c));
    p->channels.append(
        QVariantMap{{"name", name}, {"type", "uint8"}, {"encoding", "UNorm"}});
  }
  if (std::all_of(p->mapping.begin(), p->mapping.end(),
                  [](int i) { return i < 0; }))
    throw std::runtime_error("Map at least one existing source channel");
  QVariantList mapped;
  for (int c : p->mapping)
    mapped.append(c < 0 ? QString{}
                        : QString::fromStdString(p->source.channels[c].name));
  const QVariantMap window{
      {"x", 0}, {"y", 0}, {"width", full.width()}, {"height", full.height()}};
  p->info = {{"dataWindow", window},
             {"displayWindow", window},
             {"mapping", mapped},
             {"alpha", alpha ? 2 : 1},
             {"format", tga ? "TGA" : "JPG"},
             {"boundedPreview", true},
             {"colourSpace", image.colorSpace().description()},
             {"metadata", QVariantMap{{"entry.0.encoding",
                                       tga ? "TGA uncompressed" : "JPEG"}}},
             {"meaning", static_cast<int>(ImageMeaning::Colour)},
             {"layers", QStringList{QString{}}}};
  if (identity) {
    f.seek(0);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!f.atEnd()) {
      check();
      const auto chunk = f.read(1024 * 1024);
      if (chunk.isEmpty())
        throw std::runtime_error("Preview identity read failed");
      hash.addData(chunk);
    }
    p->info["sha256"] = QString::fromLatin1(hash.result().toHex());
  }
  p->subresources.append(QVariantMap{{"index", 0},
                                     {"name", "entry.0"},
                                     {"part", 0},
                                     {"mip", 0},
                                     {"array", 0},
                                     {"face", 0},
                                     {"slice", 0},
                                     {"width", full.width()},
                                     {"height", full.height()}});
  p->prepareUpload(stop);
  return p;
}
} // namespace large_preview
