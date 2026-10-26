#include "formaview/raster_image.hpp"
#include "raster_profile.hpp"
#include <QBuffer>
#include <QColorSpace>
#include <QImageReader>
#include <QImageWriter>
#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <set>
#include <zlib.h>

namespace formaview::imaging {
namespace {
void cancel(std::stop_token stop) {
  if (stop.stop_requested())
    throw skeletal::Failure(ErrorCode::Cancelled, "Raster operation cancelled");
}
void budget(bool ok) {
  if (!ok)
    throw skeletal::Failure(ErrorCode::ResourceLimit,
                            "Raster allocation budget exceeded");
}
[[noreturn]] void invalid(const std::string &s) {
  throw skeletal::Failure(ErrorCode::InvalidArgument, s);
}
[[noreturn]] void unsupported(const std::string &s) {
  throw skeletal::Failure(ErrorCode::Unsupported, s);
}
std::uint32_t u32(const QByteArray &data, qsizetype at) {
  if (at < 0 || at > data.size() - 4)
    invalid("Truncated raster header");
  return qFromLittleEndian<std::uint32_t>(data.constData() + at);
}
std::uint16_t u16(const QByteArray &data, qsizetype at) {
  if (at < 0 || at > data.size() - 2)
    invalid("Truncated raster header");
  return qFromLittleEndian<std::uint16_t>(data.constData() + at);
}
QByteArray read(const std::filesystem::path &path, Limits limits,
                std::stop_token stop) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in)
    throw skeletal::Failure(ErrorCode::Io, "Cannot open raster input");
  const auto n = in.tellg();
  budget(n > 0 && static_cast<std::uint64_t>(n) <= limits.input_bytes &&
         static_cast<std::uint64_t>(n) <=
             static_cast<std::uint64_t>(PTRDIFF_MAX));
  QByteArray data(static_cast<qsizetype>(n), Qt::Uninitialized);
  in.seekg(0);
  for (qsizetype offset = 0; offset < data.size();) {
    cancel(stop);
    const auto count = std::min<qsizetype>(1024 * 1024, data.size() - offset);
    if (!in.read(data.data() + offset, count))
      throw skeletal::Failure(ErrorCode::Io, "Short raster read");
    offset += count;
  }
  return data;
}
void size_budget(QSize size, Limits limits) {
  budget(size.width() > 0 && size.height() > 0 &&
         static_cast<std::uint32_t>(size.width()) <= limits.dimension &&
         static_cast<std::uint32_t>(size.height()) <= limits.dimension);
  // Source QImage + straight converted QImage + planar result; worst case
  // 64-bit.
  budget(static_cast<std::uint64_t>(size.width()) * size.height() <=
         limits.decoded_bytes / 24);
}
QByteArray detect(const QByteArray &bytes, const std::filesystem::path &path) {
  if (bytes.startsWith("\x89PNG\r\n\x1a\n"))
    return "png";
  if (bytes.startsWith("\xff\xd8"))
    return "jpeg";
  if (bytes.startsWith("BM"))
    return "bmp";
  if (bytes.startsWith("GIF87a") || bytes.startsWith("GIF89a"))
    return "gif";
  if (bytes.size() >= 6 && u16(bytes, 0) == 0 && u16(bytes, 2) == 1)
    return "ico";
  auto suffix = path.extension().wstring();
  std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](wchar_t c) {
    return static_cast<wchar_t>(std::towlower(c));
  });
  if (suffix == L".dib" && bytes.size() >= 40 && u32(bytes, 0) >= 40)
    return "dib";
  unsupported("Raster signature has no validated decoder");
}
// Explicit supported bare-DIB subset. Palette/mask offset is derived from the
// actual header, never the extension or a guessed 54-byte pixel offset.
QByteArray wrap_dib(const QByteArray &data) {
  const auto header = u32(data, 0), compression = u32(data, 16),
             used = u32(data, 32);
  const auto bits = u16(data, 14);
  if (header != 40 && header != 108 && header != 124)
    unsupported("DIB header variant not validated");
  if (compression != 0 && compression != 3)
    unsupported("DIB compression not validated");
  if (bits != 1 && bits != 4 && bits != 8 && bits != 16 && bits != 24 &&
      bits != 32)
    unsupported("DIB bit depth not validated");
  if (used > 256)
    invalid("DIB palette exceeds supported size");
  const std::uint64_t palette = used ? used : bits <= 8 ? (1ULL << bits) : 0;
  const std::uint64_t offset =
      14 + header + (header == 40 && compression == 3 ? 12 : 0) + palette * 4;
  if (offset > static_cast<std::uint64_t>(data.size()) + 14 ||
      data.size() > UINT32_MAX - 14)
    invalid("DIB pixel offset out of bounds");
  QByteArray bytes(14, '\0');
  bytes[0] = 'B';
  bytes[1] = 'M';
  qToLittleEndian(static_cast<std::uint32_t>(data.size() + 14),
                  bytes.data() + 2);
  qToLittleEndian(static_cast<std::uint32_t>(offset), bytes.data() + 10);
  bytes += data;
  return bytes;
}
struct Entry {
  qsizetype offset{}, size{};
  QSize dimensions;
};
QByteArray embedded_icc(const QByteArray &bytes, const QByteArray &format,
                        std::stop_token stop) {
  if (format == "png") {
    QByteArray profile;
    for (qsizetype at = 8; at + 12 <= bytes.size();) {
      cancel(stop);
      const auto length = qFromBigEndian<std::uint32_t>(bytes.constData() + at);
      if (length > static_cast<std::uint64_t>(bytes.size() - at - 12))
        invalid("Truncated PNG metadata chunk");
      const auto type = bytes.mid(at + 4, 4);
      if (type == "iCCP") {
        if (!profile.isEmpty())
          invalid("Duplicate PNG ICC profile");
        const auto data = bytes.mid(at + 8, length);
        const auto name_end = data.indexOf('\0');
        if (name_end < 1 || name_end > 79 || name_end + 2 >= data.size() ||
            data[name_end + 1] != 0)
          invalid("Invalid PNG ICC metadata");
        const auto expected =
            qFromBigEndian<std::uint32_t>(bytes.constData() + at + 8 + length);
        const auto crc = crc32(
            0, reinterpret_cast<const Bytef *>(bytes.constData() + at + 4),
            length + 4);
        if (crc != expected)
          invalid("PNG ICC checksum mismatch");
        profile.resize(1024 * 1024);
        uLongf size = static_cast<uLongf>(profile.size());
        const auto status = uncompress(
            reinterpret_cast<Bytef *>(profile.data()), &size,
            reinterpret_cast<const Bytef *>(data.constData() + name_end + 2),
            static_cast<uLong>(data.size() - name_end - 2));
        if (status == Z_BUF_ERROR)
          budget(false);
        if (status != Z_OK || !size)
          invalid("Invalid compressed PNG ICC profile");
        profile.resize(static_cast<qsizetype>(size));
      }
      if (type == "IEND")
        break;
      at += static_cast<qsizetype>(length) + 12;
    }
    return profile;
  }
  if (format == "jpeg") {
    std::map<unsigned, QByteArray> parts;
    unsigned count = 0;
    qsizetype total = 0;
    for (qsizetype at = 2; at < bytes.size();) {
      cancel(stop);
      if (static_cast<unsigned char>(bytes[at++]) != 0xff)
        invalid("Invalid JPEG marker");
      while (at < bytes.size() && static_cast<unsigned char>(bytes[at]) == 0xff)
        ++at;
      if (at >= bytes.size())
        invalid("Truncated JPEG marker");
      const auto marker = static_cast<unsigned char>(bytes[at++]);
      if (marker == 0xda || marker == 0xd9)
        break;
      if (marker == 1 || (marker >= 0xd0 && marker <= 0xd7))
        continue;
      if (at + 2 > bytes.size())
        invalid("Truncated JPEG metadata");
      const auto size = qFromBigEndian<std::uint16_t>(bytes.constData() + at);
      if (size < 2 || size > bytes.size() - at)
        invalid("Invalid JPEG metadata length");
      const auto data = bytes.mid(at + 2, size - 2);
      if (marker == 0xe2 && data.startsWith(QByteArray("ICC_PROFILE\0", 12))) {
        if (data.size() <= 14)
          invalid("Truncated JPEG ICC segment");
        const auto index = static_cast<unsigned char>(data[12]),
                   segments = static_cast<unsigned char>(data[13]);
        if (!index || !segments || index > segments ||
            (count && segments != count) || parts.contains(index))
          invalid("Invalid JPEG ICC segment sequence");
        count = segments;
        total += data.size() - 14;
        budget(total <= 1024 * 1024);
        parts.emplace(index, data.mid(14));
      }
      at += size;
    }
    if (parts.size() != count)
      invalid("Incomplete JPEG ICC profile");
    QByteArray result;
    for (const auto &[index, segment] : parts)
      result += segment;
    return result;
  }
  return {};
}
class BoundedBuffer final : public QBuffer {
public:
  BoundedBuffer(QByteArray *data, std::uint64_t limit)
      : QBuffer(data), limit_(limit) {}

protected:
  qint64 writeData(const char *data, qint64 length) override {
    if (length < 0 || pos() < 0 || static_cast<std::uint64_t>(pos()) > limit_ ||
        static_cast<std::uint64_t>(length) >
            limit_ - static_cast<std::uint64_t>(pos()))
      return -1;
    return QBuffer::writeData(data, length);
  }

private:
  std::uint64_t limit_;
};
bool source_gray(const QImage &image, const QByteArray &format,
                 const QByteArray &bytes) {
  if (format == "png" && bytes.size() >= 26)
    return bytes[25] == 0 || bytes[25] == 4;
  return image.format() == QImage::Format_Grayscale8 ||
         image.format() == QImage::Format_Grayscale16;
}
QImage decode_image(const QByteArray &bytes, QByteArray format,
                    const Entry &entry, Limits limits, std::stop_token stop) {
  cancel(stop);
  auto data = bytes;
  if (format == "ico") {
    // One-entry container keeps entry addressing independent of plugin seeking.
    QByteArray single = bytes.left(6);
    qToLittleEndian<std::uint16_t>(1, single.data() + 4);
    auto header = bytes.mid(entry.offset, 16);
    qToLittleEndian<std::uint32_t>(22, header.data() + 12);
    single += header;
    single += bytes.mid(u32(bytes, entry.offset + 12), entry.size);
    data = std::move(single);
  } else if (format == "dib") {
    data = wrap_dib(bytes);
    format = "bmp";
  }
  QBuffer buffer(&data);
  buffer.open(QIODevice::ReadOnly);
  QImageReader reader(&buffer, format);
  reader.setAutoTransform(false);
  reader.setDecideFormatFromContent(false);
  const auto size = format == "gif" ? entry.dimensions : reader.size();
  size_budget(size, limits);
  auto image = reader.read();
  if (image.isNull())
    unsupported("Qt raster decode failed: " +
                reader.errorString().toStdString());
  if (image.format() == QImage::Format_CMYK8888)
    unsupported("CMYK JPEG requires a separate colour policy");
  size_budget(image.size(), limits);
  cancel(stop);
  return image;
}
// Qt's QImage has no two-component gray/alpha storage. Keep that PNG layout
// explicit instead of silently expanding source Y/A into RGBA.
std::vector<std::byte> encode_gray_alpha_png(const ImageSubresource &s,
                                             const RasterEncodeOptions &options,
                                             Limits limits,
                                             std::stop_token stop) {
  std::vector<std::byte> out;
  const auto append = [&](const void *p, std::size_t n) {
    if (!n)
      return;
    budget(out.size() <= limits.input_bytes &&
           n <= limits.input_bytes - out.size());
    const auto *b = static_cast<const std::byte *>(p);
    out.insert(out.end(), b, b + n);
  };
  const auto number = [&](std::uint32_t n) {
    const auto big = qToBigEndian(n);
    append(&big, 4);
  };
  const auto chunk = [&](const char *type, std::span<const std::byte> bytes) {
    budget(bytes.size() <= UINT_MAX);
    number(static_cast<std::uint32_t>(bytes.size()));
    append(type, 4);
    append(bytes.data(), bytes.size());
    auto crc = crc32(0, reinterpret_cast<const Bytef *>(type), 4);
    if (!bytes.empty())
      crc = crc32(crc, reinterpret_cast<const Bytef *>(bytes.data()),
                  static_cast<uInt>(bytes.size()));
    number(static_cast<std::uint32_t>(crc));
  };
  append("\x89PNG\r\n\x1a\n", 8);
  const auto wide = s.channels[0].samples.scalar == ScalarType::UInt16;
  std::array<std::byte, 13> header{};
  qToBigEndian(static_cast<std::uint32_t>(s.data_window.width), header.data());
  qToBigEndian(static_cast<std::uint32_t>(s.data_window.height),
               header.data() + 4);
  header[8] = static_cast<std::byte>(wide ? 16 : 8);
  header[9] = std::byte{4};
  chunk("IHDR", header);
  if (!options.icc_profile.empty()) {
    std::vector<std::byte> profile(
        compressBound(static_cast<uLong>(options.icc_profile.size())) + 5);
    std::memcpy(profile.data(), "ICC\0\0", 5);
    uLongf length = static_cast<uLongf>(profile.size() - 5);
    if (compress2(reinterpret_cast<Bytef *>(profile.data() + 5), &length,
                  reinterpret_cast<const Bytef *>(options.icc_profile.data()),
                  static_cast<uLong>(options.icc_profile.size()),
                  options.compression) != Z_OK)
      unsupported("PNG profile compression failed");
    profile.resize(length + 5);
    chunk("iCCP", profile);
  }
  z_stream stream{};
  if (deflateInit(&stream, options.compression) != Z_OK)
    budget(false);
  struct End {
    z_stream &s;
    ~End() { deflateEnd(&s); }
  } end{stream};
  const auto w = static_cast<std::size_t>(s.data_window.width),
             h = static_cast<std::size_t>(s.data_window.height);
  const auto component = wide ? 2U : 1U;
  std::vector<std::byte> row(w * 2 * component + 1);
  std::array<std::byte, 65536> compressed{};
  for (std::size_t y = 0; y <= h; ++y) {
    cancel(stop);
    if (y < h)
      for (std::size_t x = 0; x < w; ++x)
        for (std::size_t c = 0; c < 2; ++c) {
          const auto *source =
              s.channels[c].samples.bytes.data() + (y * w + x) * component;
          auto *target = row.data() + 1 + (x * 2 + c) * component;
          if (wide) {
            std::uint16_t value;
            std::memcpy(&value, source, 2);
            qToBigEndian(value, target);
          } else
            *target = *source;
        }
    stream.next_in = reinterpret_cast<Bytef *>(row.data());
    stream.avail_in = y < h ? static_cast<uInt>(row.size()) : 0;
    int status = Z_OK;
    do {
      cancel(stop);
      stream.next_out = reinterpret_cast<Bytef *>(compressed.data());
      stream.avail_out = static_cast<uInt>(compressed.size());
      status = deflate(&stream, y < h ? Z_NO_FLUSH : Z_FINISH);
      if (status != Z_OK && status != Z_STREAM_END)
        unsupported("PNG compression failed");
      const auto n = compressed.size() - stream.avail_out;
      if (n)
        chunk("IDAT", std::span(compressed).first(n));
    } while (stream.avail_in || (y == h && status != Z_STREAM_END));
  }
  chunk("IEND", {});
  return out;
}
} // namespace
struct RasterFile::Impl {
  QByteArray bytes, format;
  std::vector<Entry> entries;
  ImageData desc;
  Limits limits;
};
RasterFile::RasterFile(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
RasterFile::RasterFile(RasterFile &&) noexcept = default;
RasterFile &RasterFile::operator=(RasterFile &&) noexcept = default;
RasterFile::~RasterFile() = default;
const ImageData &RasterFile::description() const { return impl_->desc; }
std::size_t RasterFile::resident_bytes() const {
  return static_cast<std::size_t>(impl_->bytes.size());
}
RasterFile RasterFile::open(const std::filesystem::path &path, Limits limits,
                            std::stop_token stop) {
  cancel(stop);
  auto p = std::make_unique<Impl>();
  p->limits = limits;
  p->bytes = read(path, limits, stop);
  p->format = detect(p->bytes, path);
  p->desc.source.identity.sha256 = skeletal::sha256(
      std::as_bytes(std::span(p->bytes.constData(),
                              static_cast<std::size_t>(p->bytes.size()))),
      stop);
  p->desc.source.identity.adapter = "QtCpuRaster";
  p->desc.source.identity.adapter_version = QT_VERSION_STR;
  p->desc.source.properties["container"] = p->format.toStdString();
  p->desc.source.properties["orientation"] =
      "stored pixel order; no automatic EXIF transform";
  if (p->format == "gif")
    p->desc.source.properties["scope"] =
        "first static frame only; no animation playback";
  if (p->format == "ico") {
    const auto count = u16(p->bytes, 4);
    budget(count > 0 && count <= std::min(limits.subresources, 256U));
    if (p->bytes.size() < 6 + count * 16)
      invalid("Truncated ICO directory");
    for (std::uint32_t i = 0; i < count; ++i) {
      const qsizetype at = 6 + i * 16;
      const auto length = u32(p->bytes, at + 8), start = u32(p->bytes, at + 12);
      if (start < 6U + count * 16U ||
          static_cast<std::uint64_t>(start) + length >
              static_cast<std::uint64_t>(p->bytes.size()) ||
          !length)
        invalid("ICO entry out of bounds");
      const auto w = static_cast<unsigned char>(p->bytes[at]),
                 h = static_cast<unsigned char>(p->bytes[at + 1]);
      p->entries.push_back({at, length, QSize(w ? w : 256, h ? h : 256)});
      p->desc.source.properties["entry." + std::to_string(i) +
                                ".directory_bits_per_pixel"] =
          std::to_string(u16(p->bytes, at + 6));
      p->desc.source.properties["entry." + std::to_string(i) + ".encoding"] =
          p->bytes.mid(start, 8).startsWith("\x89PNG\r\n\x1a\n")
              ? "PNG"
              : "DIB/AND mask";
    }
  } else {
    auto data = p->format == "dib" ? wrap_dib(p->bytes) : p->bytes;
    QBuffer buffer(&data);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer,
                        p->format == "dib" ? QByteArray("bmp") : p->format);
    reader.setAutoTransform(false);
    const auto size = p->format == "gif"
                          ? QSize(u16(p->bytes, 6), u16(p->bytes, 8))
                          : reader.size();
    size_budget(size, limits);
    p->entries.push_back({0, data.size(), size});
  }
  for (std::size_t i = 0; i < p->entries.size(); ++i) {
    const auto size = p->entries[i].dimensions;
    size_budget(size, limits);
    ImageSubresource s;
    s.part = static_cast<std::uint32_t>(i);
    s.part_name = "entry." + std::to_string(i);
    s.data_window =
        s.display_window = {0, 0, static_cast<std::uint64_t>(size.width()),
                            static_cast<std::uint64_t>(size.height())};
    const auto image =
        decode_image(p->bytes, p->format, p->entries[i], limits, stop);
    if (image.size() != size)
      invalid("Raster directory dimensions mismatch");
    const bool gray = source_gray(image, p->format, p->bytes);
    const bool wide =
        image.depth() == 64 || image.format() == QImage::Format_Grayscale16;
    const bool alpha = image.hasAlphaChannel();
    auto names = gray ? std::vector<std::string>{"Y"}
                      : std::vector<std::string>{"R", "G", "B"};
    if (alpha)
      names.push_back("A");
    budget(names.size() <= limits.channels);
    for (const auto &name : names) {
      ImageChannel c;
      c.name = name;
      c.encoding = SampleEncoding::UNorm;
      c.samples.scalar = wide ? ScalarType::UInt16 : ScalarType::UInt8;
      c.samples.element_count = s.data_window.width * s.data_window.height;
      s.channels.push_back(std::move(c));
    }
    const auto prefix = "entry." + std::to_string(i) + ".";
    p->desc.source.properties[prefix + "alpha"] = alpha ? "straight" : "absent";
    if (i == 0)
      p->desc.alpha = alpha ? AlphaMode::Straight : AlphaMode::Absent;
    else if (p->desc.alpha != (alpha ? AlphaMode::Straight : AlphaMode::Absent))
      p->desc.alpha = AlphaMode::Unknown;
    auto icc = embedded_icc(p->bytes, p->format, stop);
    if (p->format == "ico") {
      const auto &entry = p->entries[i];
      const auto payload =
          p->bytes.mid(u32(p->bytes, entry.offset + 12), entry.size);
      if (payload.startsWith("\x89PNG\r\n\x1a\n"))
        icc = embedded_icc(payload, "png", stop);
    }
    if (icc.isEmpty())
      icc = image.colorSpace().iccProfile();
    budget(icc.size() <= 1024 * 1024);
    if (!icc.isEmpty()) {
      p->desc.source.properties[prefix + "icc_base64"] =
          icc.toBase64().toStdString();
      p->desc.source.properties[prefix + "icc_status"] =
          QColorSpace::fromIccProfile(icc).isValid()
              ? "recognized"
              : "unrecognized raw profile retained";
    }
    p->desc.source.properties[prefix + "colour_space"] =
        image.colorSpace().isValid()
            ? image.colorSpace().description().toStdString()
            : "unspecified";
    p->desc.subresources.push_back(std::move(s));
  }
  cancel(stop);
  return RasterFile(std::move(p));
}
ImageSubresource RasterFile::decode(std::size_t entry,
                                    std::stop_token stop) const {
  if (entry >= impl_->entries.size())
    invalid("Raster entry out of range");
  auto image = decode_image(impl_->bytes, impl_->format, impl_->entries[entry],
                            impl_->limits, stop);
  auto s = impl_->desc.subresources[entry];
  if (image.width() != static_cast<int>(s.data_window.width) ||
      image.height() != static_cast<int>(s.data_window.height))
    invalid("Decoded raster dimensions disagree with directory");
  // QImage's colourSpace is metadata only; no implicit transform is requested.
  const bool gray = source_gray(image, impl_->format, impl_->bytes);
  const bool packed_gray = image.format() == QImage::Format_Grayscale8 ||
                           image.format() == QImage::Format_Grayscale16;
  const bool wide =
      image.depth() == 64 || image.format() == QImage::Format_Grayscale16;
  const bool alpha = image.hasAlphaChannel();
  if (!packed_gray)
    image = image.convertToFormat(wide ? QImage::Format_RGBA64
                                       : QImage::Format_RGBA8888);
  if (image.isNull())
    throw skeletal::Failure(ErrorCode::ResourceLimit,
                            "Raster conversion allocation failed");
  std::vector<std::string> names =
      gray ? std::vector<std::string>{"Y"}
           : std::vector<std::string>{"R", "G", "B"};
  if (alpha)
    names.push_back("A");
  s.channels.clear();
  budget(names.size() <= impl_->limits.channels);
  const auto pixels = s.data_window.width * s.data_window.height,
             component = wide ? 2ULL : 1ULL;
  for (std::size_t c = 0; c < names.size(); ++c) {
    ImageChannel out;
    out.name = names[c];
    out.encoding = SampleEncoding::UNorm;
    out.samples.scalar = wide ? ScalarType::UInt16 : ScalarType::UInt8;
    out.samples.element_count = pixels;
    out.samples.bytes.resize(static_cast<std::size_t>(pixels * component));
    for (int y = 0; y < image.height(); ++y) {
      cancel(stop);
      const auto *row = image.constScanLine(y);
      for (int x = 0; x < image.width(); ++x)
        std::memcpy(out.samples.bytes.data() +
                        (static_cast<std::size_t>(y) * image.width() + x) *
                            component,
                    row + (static_cast<std::size_t>(x) * (packed_gray ? 1 : 4) +
                           (gray && alpha && c == 1 ? 3 : c)) *
                              component,
                    static_cast<std::size_t>(component));
    }
    s.channels.push_back(std::move(out));
  }
  cancel(stop);
  return s;
}
std::vector<std::byte> encode_raster(const ImageSubresource &s,
                                     const RasterEncodeOptions &options,
                                     Limits limits, std::stop_token stop) {
  cancel(stop);
  const auto w = s.data_window.width, h = s.data_window.height;
  budget(w <= INT_MAX && h <= INT_MAX);
  size_budget(QSize(static_cast<int>(w), static_cast<int>(h)), limits);
  if (s.data_window.x || s.data_window.y || s.display_window.x ||
      s.display_window.y ||
      (s.display_window.width &&
       (s.display_window.width != w || s.display_window.height != h)))
    invalid("Raster target cannot preserve EXR windows; explicit window policy "
            "required");
  if (options.quality < 0 || options.quality > 100 || options.compression < 0 ||
      options.compression > 9)
    invalid("Invalid raster quality/compression");
  if (s.channels.empty())
    invalid("No raster channels");
  const bool gray = s.channels.size() <= 2 && s.channels[0].name == "Y";
  const bool alpha = s.channels.size() == 4 || (gray && s.channels.size() == 2);
  if (!gray && s.channels.size() != 3 && !alpha)
    invalid("Raster output requires Y, YA, RGB or RGBA");
  if (options.target != RasterTarget::Png && alpha)
    invalid("Target alpha removal must be explicit before encoding");
  if (alpha && options.alpha != AlphaMode::Straight)
    invalid("PNG requires explicit straight alpha");
  budget(options.icc_profile.size() <= 1024 * 1024);
  validate_icc_model(options.icc_profile, gray);
  QColorSpace colour;
  if (!options.icc_profile.empty()) {
    if (options.target == RasterTarget::Bmp)
      unsupported("BMP ICC preservation is not implemented");
    colour = QColorSpace::fromIccProfile(
        QByteArray(reinterpret_cast<const char *>(options.icc_profile.data()),
                   static_cast<qsizetype>(options.icc_profile.size())));
    if (!colour.isValid())
      unsupported(
          "ICC profile is not understood; choose an explicit metadata policy");
  }
  const auto scalar = s.channels.front().samples.scalar;
  if (scalar != ScalarType::UInt8 && scalar != ScalarType::UInt16)
    invalid("Raster output requires explicit integer quantization");
  const bool wide = scalar == ScalarType::UInt16;
  if (wide && options.target != RasterTarget::Png)
    unsupported("16-bit output is validated only for PNG");
  const auto names = gray ? std::array<std::string, 4>{"Y", "A", "", ""}
                          : std::array<std::string, 4>{"R", "G", "B", "A"};
  for (std::size_t i = 0; i < s.channels.size(); ++i) {
    const auto &c = s.channels[i];
    if (c.name != names[i] || c.samples.scalar != scalar ||
        c.samples.components != 1 || c.encoding != SampleEncoding::UNorm ||
        c.samples.element_count != w * h ||
        c.samples.bytes.size() != w * h * (wide ? 2 : 1))
      invalid("Malformed raster channel layout");
  }
  if (gray && alpha)
    return encode_gray_alpha_png(s, options, limits, stop);
  const auto format =
      gray   ? (wide ? QImage::Format_Grayscale16 : QImage::Format_Grayscale8)
      : wide ? (alpha ? QImage::Format_RGBA64 : QImage::Format_RGBX64)
             : (alpha ? QImage::Format_RGBA8888 : QImage::Format_RGBX8888);
  QImage image(static_cast<int>(w), static_cast<int>(h), format);
  if (image.isNull())
    throw skeletal::Failure(ErrorCode::ResourceLimit,
                            "Raster encode allocation failed");
  image.fill(Qt::white);
  // JPEG's Qt handler drops grayscale profiles. Write APP2 ourselves for both
  // models so the exact source bytes, including multi-segment profiles, survive.
  if (colour.isValid() && options.target != RasterTarget::Jpeg)
    image.setColorSpace(colour);
  const auto component = wide ? 2U : 1U;
  for (int y = 0; y < image.height(); ++y) {
    cancel(stop);
    auto *row = image.scanLine(y);
    for (std::size_t x = 0; x < w; ++x)
      for (std::size_t c = 0; c < s.channels.size(); ++c)
        std::memcpy(row + (x * (gray ? 1 : 4) + c) * component,
                    s.channels[c].samples.bytes.data() +
                        (static_cast<std::size_t>(y) * w + x) * component,
                    component);
  }
  QByteArray bytes;
  BoundedBuffer buffer(&bytes, limits.input_bytes);
  buffer.open(QIODevice::WriteOnly);
  const QByteArray format_name = options.target == RasterTarget::Png    ? "png"
                                 : options.target == RasterTarget::Jpeg ? "jpeg"
                                                                        : "bmp";
  QImageWriter writer(&buffer, format_name);
  writer.setQuality(options.quality);
  if (options.target == RasterTarget::Png)
    writer.setCompression(options.compression * 100 / 9);
  if (!writer.write(image))
    throw skeletal::Failure(ErrorCode::Io,
                            "Raster encode failed: " +
                                writer.errorString().toStdString());
  if (options.target == RasterTarget::Jpeg && !options.icc_profile.empty()) {
    constexpr std::size_t partSize = 65519; // APP2 length minus length/ICC header.
    const auto count = (options.icc_profile.size() + partSize - 1) / partSize;
    budget(static_cast<std::uint64_t>(bytes.size()) + options.icc_profile.size() +
               count * 18 <= limits.input_bytes);
    QByteArray segments;
    for (std::size_t i = 0; i < count; ++i) {
      cancel(stop);
      const auto size = std::min(partSize, options.icc_profile.size() - i * partSize);
      segments.append("\xff\xe2", 2);
      const auto length = static_cast<std::uint16_t>(size + 16);
      char encodedLength[2];
      qToBigEndian(length, encodedLength);
      segments.append(encodedLength, 2);
      segments.append("ICC_PROFILE\0", 12);
      segments.append(static_cast<char>(i + 1));
      segments.append(static_cast<char>(count));
      segments.append(reinterpret_cast<const char *>(options.icc_profile.data() + i * partSize),
                      static_cast<qsizetype>(size));
    }
    // Keep the JFIF APP0 marker immediately after SOI when it is present.
    qsizetype offset = 2;
    if (bytes.size() >= 6 && bytes.mid(2, 2) == QByteArray("\xff\xe0", 2)) {
      const auto length = qFromBigEndian<std::uint16_t>(bytes.constData() + 4);
      if (length < 2 || length > bytes.size() - 4)
        invalid("Invalid encoded JPEG APP0 marker");
      offset = 4 + length;
    }
    bytes.insert(offset, segments);
  }
  budget(static_cast<std::uint64_t>(bytes.size()) <= limits.input_bytes);
  cancel(stop);
  std::vector<std::byte> out(static_cast<std::size_t>(bytes.size()));
  std::memcpy(out.data(), bytes.constData(), out.size());
  return out;
}
} // namespace formaview::imaging
