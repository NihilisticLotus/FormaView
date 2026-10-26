#include "formaview/extended_raster.hpp"
#include "raster_profile.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <tiffio.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>

namespace formaview::imaging {
namespace {
[[noreturn]] void fail(const char *m, ErrorCode c = ErrorCode::Unsupported) {
  throw skeletal::Failure(c, m);
}
void cancel(std::stop_token s) {
  if (s.stop_requested())
    fail("Image operation cancelled", ErrorCode::Cancelled);
}
void budget(bool b) {
  if (!b)
    fail("Image resource budget exceeded", ErrorCode::ResourceLimit);
}
void valid(bool b, const char *m) {
  if (!b)
    fail(m, ErrorCode::InvalidArgument);
}
std::size_t bytes_per(ScalarType t) {
  switch (t) {
  case ScalarType::UInt8:
    return 1;
  case ScalarType::UInt16:
  case ScalarType::Int16:
  case ScalarType::Half:
    return 2;
  case ScalarType::UInt32:
  case ScalarType::Int32:
  case ScalarType::Float32:
    return 4;
  case ScalarType::Float64:
    return 8;
  default:
    fail("Unsupported TIFF scalar");
  }
}
std::string hex(std::span<const std::byte> bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (auto b : bytes) {
    const auto v = std::to_integer<unsigned>(b);
    out += digits[v >> 4];
    out += digits[v & 15];
  }
  return out;
}
struct Memory {
  std::span<const std::byte> input;
  std::vector<std::byte> output;
  std::uint64_t position{}, maximum{};
  bool writing{}, failed{};
  std::stop_token stop;
};
tmsize_t read_mem(thandle_t h, void *out, tmsize_t size) {
  auto &m = *static_cast<Memory *>(h);
  if (m.stop.stop_requested() || size < 0 || m.position > m.input.size())
    return -1;
  const auto n = std::min<std::uint64_t>(static_cast<std::uint64_t>(size),
                                         m.input.size() - m.position);
  std::memcpy(out, m.input.data() + m.position, static_cast<std::size_t>(n));
  m.position += n;
  return static_cast<tmsize_t>(n);
}
tmsize_t write_mem(thandle_t h, void *in, tmsize_t size) {
  auto &m = *static_cast<Memory *>(h);
  if (m.stop.stop_requested() || size < 0 || m.position > m.maximum ||
      static_cast<std::uint64_t>(size) > m.maximum - m.position) {
    m.failed = true;
    return -1;
  }
  try {
    const auto end =
        static_cast<std::size_t>(m.position + static_cast<std::uint64_t>(size));
    if (end > m.output.size())
      m.output.resize(end);
    std::memcpy(m.output.data() + m.position, in,
                static_cast<std::size_t>(size));
    m.position = end;
    return size;
  } catch (...) {
    m.failed = true;
    return -1;
  }
}
toff_t seek_mem(thandle_t h, toff_t offset, int origin) {
  auto &m = *static_cast<Memory *>(h);
  const auto size = m.writing ? m.output.size() : m.input.size();
  const auto base = origin == SEEK_SET   ? 0
                    : origin == SEEK_CUR ? m.position
                    : origin == SEEK_END ? size
                                         : UINT64_MAX;
  if (base == UINT64_MAX)
    return static_cast<toff_t>(-1);
  std::uint64_t target{};
  // TIFF uses an unsigned offset type even for negative relative offsets.
  if (origin != SEEK_SET && offset > INT64_MAX) {
    const auto distance = ~offset + 1;
    if (distance > base)
      return static_cast<toff_t>(-1);
    target = base - distance;
  } else {
    if (offset > UINT64_MAX - base)
      return static_cast<toff_t>(-1);
    target = base + offset;
  }
  if (target > (m.writing ? m.maximum : size))
    return static_cast<toff_t>(-1);
  m.position = target;
  return target;
}
int close_mem(thandle_t) { return 0; }
toff_t size_mem(thandle_t h) {
  const auto &m = *static_cast<Memory *>(h);
  return m.writing ? m.output.size() : m.input.size();
}
int map_mem(thandle_t, void **, toff_t *) { return 0; }
void unmap_mem(thandle_t, void *, toff_t) {}
int diagnostic(TIFF *, void *, const char *, const char *, va_list) {
  return 1;
}
using Tiff = std::unique_ptr<TIFF, decltype(&TIFFClose)>;
Tiff open_tiff(Memory &memory, Limits limits) {
  std::unique_ptr<TIFFOpenOptions, decltype(&TIFFOpenOptionsFree)> options(
      TIFFOpenOptionsAlloc(), TIFFOpenOptionsFree);
  budget(static_cast<bool>(options));
  const auto maximum = static_cast<tmsize_t>(
      std::min<std::uint64_t>(limits.decoded_bytes, INT64_MAX));
  TIFFOpenOptionsSetMaxSingleMemAlloc(options.get(), maximum);
  TIFFOpenOptionsSetMaxCumulatedMemAlloc(options.get(), maximum);
  TIFFOpenOptionsSetErrorHandlerExtR(options.get(), diagnostic, nullptr);
  TIFFOpenOptionsSetWarningHandlerExtR(options.get(), diagnostic, nullptr);
  Tiff ptr(TIFFClientOpenExt("memory", memory.writing ? "w" : "r", &memory,
                             read_mem, write_mem, seek_mem, close_mem, size_mem,
                             map_mem, unmap_mem, options.get()),
           TIFFClose);
  cancel(memory.stop);
  if (!ptr)
    fail("TIFF open rejected invalid or unsupported input");
  return ptr;
}
struct TiffInfo {
  std::uint32_t w{}, h{};
  std::uint16_t bits{}, count{}, format{}, photo{}, planar{}, orientation{},
      compression{};
  std::vector<std::uint16_t> extras;
  ScalarType scalar{};
  AlphaMode alpha{AlphaMode::Absent};
};
TiffInfo tiff_info(TIFF *t, Limits limits) {
  TiffInfo i;
  valid(TIFFGetField(t, TIFFTAG_IMAGEWIDTH, &i.w) &&
            TIFFGetField(t, TIFFTAG_IMAGELENGTH, &i.h),
        "Missing TIFF dimensions");
  TIFFGetFieldDefaulted(t, TIFFTAG_BITSPERSAMPLE, &i.bits);
  TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLESPERPIXEL, &i.count);
  TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLEFORMAT, &i.format);
  TIFFGetFieldDefaulted(t, TIFFTAG_PHOTOMETRIC, &i.photo);
  TIFFGetFieldDefaulted(t, TIFFTAG_PLANARCONFIG, &i.planar);
  TIFFGetFieldDefaulted(t, TIFFTAG_ORIENTATION, &i.orientation);
  TIFFGetFieldDefaulted(t, TIFFTAG_COMPRESSION, &i.compression);
  budget(i.w && i.h && i.w <= limits.dimension && i.h <= limits.dimension &&
         i.count && i.count <= limits.channels);
  // Native samples are kept unchanged. The common Y channel and output
  // encoders mean BlackIsZero; accepting WhiteIsZero would silently reverse
  // brightness in both the viewport and converted files.
  if (i.photo == PHOTOMETRIC_MINISWHITE)
    fail("TIFF WhiteIsZero grayscale requires an explicit photometric conversion; "
         "convert to BlackIsZero in an image editor before importing");
  if (i.photo != PHOTOMETRIC_RGB && i.photo != PHOTOMETRIC_MINISBLACK)
    fail("TIFF photometric interpretation requires a separate conversion path");
  valid(
      (i.planar == PLANARCONFIG_CONTIG || i.planar == PLANARCONFIG_SEPARATE) &&
          i.orientation >= 1 && i.orientation <= 8,
      "Invalid TIFF layout");
  const auto base = i.photo == PHOTOMETRIC_RGB ? 3U : 1U;
  valid(i.count >= base, "Missing TIFF colour samples");
  std::uint16_t count = 0, *extras = nullptr;
  if (TIFFGetField(t, TIFFTAG_EXTRASAMPLES, &count, &extras))
    i.extras.assign(extras, extras + count);
  valid(i.extras.size() <= i.count - base, "Invalid TIFF extra sample count");
  for (auto e : i.extras)
    if (e == EXTRASAMPLE_ASSOCALPHA || e == EXTRASAMPLE_UNASSALPHA) {
      valid(i.alpha == AlphaMode::Absent,
            "Multiple TIFF alpha channels require explicit mapping");
      i.alpha = e == EXTRASAMPLE_ASSOCALPHA ? AlphaMode::Premultiplied
                                            : AlphaMode::Straight;
    }
  if (i.format == SAMPLEFORMAT_UINT &&
      (i.bits == 8 || i.bits == 16 || i.bits == 32))
    i.scalar = i.bits == 8    ? ScalarType::UInt8
               : i.bits == 16 ? ScalarType::UInt16
                              : ScalarType::UInt32;
  else if (i.format == SAMPLEFORMAT_INT && (i.bits == 16 || i.bits == 32))
    i.scalar = i.bits == 16 ? ScalarType::Int16 : ScalarType::Int32;
  else if (i.format == SAMPLEFORMAT_IEEEFP &&
           (i.bits == 16 || i.bits == 32 || i.bits == 64))
    i.scalar = i.bits == 16   ? ScalarType::Half
               : i.bits == 32 ? ScalarType::Float32
                              : ScalarType::Float64;
  else
    fail("TIFF sample format/bit depth is not implemented");
  const auto pixel_bytes =
      static_cast<std::uint64_t>(i.count) * bytes_per(i.scalar);
  budget(static_cast<std::uint64_t>(i.w) * i.h <=
         limits.decoded_bytes / (pixel_bytes * 2));
  if (!TIFFIsCODECConfigured(i.compression))
    fail("TIFF compression codec is not deployed");
  return i;
}
ImageSubresource tiff_descriptor(const TiffInfo &i, std::size_t page) {
  ImageSubresource s;
  s.part = static_cast<std::uint32_t>(page);
  s.data_window = s.display_window = {0, 0, i.w, i.h};
  const auto base = i.photo == PHOTOMETRIC_RGB ? 3U : 1U;
  for (std::size_t c = 0; c < i.count; ++c) {
    std::string name = c < base
                           ? (base == 1 ? "Y" : std::array{"R", "G", "B"}[c])
                           : "Sample" + std::to_string(c);
    if (c >= base && c - base < i.extras.size() &&
        (i.extras[c - base] == EXTRASAMPLE_ASSOCALPHA ||
         i.extras[c - base] == EXTRASAMPLE_UNASSALPHA))
      name = "A";
    Buffer b;
    b.scalar = i.scalar;
    b.element_count = static_cast<std::uint64_t>(i.w) * i.h;
    s.channels.push_back({name, std::move(b),
                          i.format == SAMPLEFORMAT_UINT && i.bits <= 16
                              ? SampleEncoding::UNorm
                              : SampleEncoding::Numeric});
  }
  return s;
}
using Demux = std::unique_ptr<WebPDemuxer, decltype(&WebPDemuxDelete)>;
Demux webp_demux(std::span<const std::byte> bytes) {
  const WebPData data{reinterpret_cast<const std::uint8_t *>(bytes.data()),
                      bytes.size()};
  Demux p(WebPDemux(&data), WebPDemuxDelete);
  if (!p)
    fail("Invalid WebP container");
  return p;
}
int webp_writer(const std::uint8_t *data, std::size_t size,
                const WebPPicture *picture) {
  auto &m = *static_cast<Memory *>(picture->custom_ptr);
  if (size > INT64_MAX)
    return 0;
  return write_mem(&m, const_cast<std::uint8_t *>(data),
                   static_cast<tmsize_t>(size)) == static_cast<tmsize_t>(size);
}
int webp_progress(int, const WebPPicture *p) {
  return !static_cast<Memory *>(p->custom_ptr)->stop.stop_requested();
}
} // namespace

struct ExtendedRasterFile::Impl {
  Limits limits;
  std::vector<std::byte> bytes;
  ImageData desc;
  bool webp{};
};
ExtendedRasterFile::ExtendedRasterFile(std::unique_ptr<Impl> p)
    : impl_(std::move(p)) {}
ExtendedRasterFile::ExtendedRasterFile(ExtendedRasterFile &&) noexcept =
    default;
ExtendedRasterFile &
ExtendedRasterFile::operator=(ExtendedRasterFile &&) noexcept = default;
ExtendedRasterFile::~ExtendedRasterFile() = default;
const ImageData &ExtendedRasterFile::description() const { return impl_->desc; }
std::size_t ExtendedRasterFile::resident_bytes() const {
  return impl_->bytes.size();
}
ExtendedRasterFile ExtendedRasterFile::open(const std::filesystem::path &path,
                                            Limits limits,
                                            std::stop_token stop) {
  cancel(stop);
  auto p = std::make_unique<Impl>();
  p->limits = limits;
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
    fail("Cannot open image", ErrorCode::Io);
  const auto length = file.tellg();
  budget(length >= 0 &&
         static_cast<std::uint64_t>(length) <= limits.input_bytes &&
         static_cast<std::uint64_t>(length) <= SIZE_MAX);
  p->bytes.resize(static_cast<std::size_t>(length));
  file.seekg(0);
  for (std::size_t offset = 0; offset < p->bytes.size();) {
    cancel(stop);
    auto n = std::min<std::size_t>(1024 * 1024, p->bytes.size() - offset);
    if (!file.read(reinterpret_cast<char *>(p->bytes.data() + offset),
                   static_cast<std::streamsize>(n)))
      fail("Short image read", ErrorCode::Io);
    offset += n;
  }
  p->desc.source.identity.sha256 = skeletal::sha256(p->bytes, stop);
  p->webp = p->bytes.size() >= 12 &&
            std::memcmp(p->bytes.data(), "RIFF", 4) == 0 &&
            std::memcmp(p->bytes.data() + 8, "WEBP", 4) == 0;
  if (p->webp) {
    auto demux = webp_demux(p->bytes);
    const auto flags = WebPDemuxGetI(demux.get(), WEBP_FF_FORMAT_FLAGS);
    if (flags & ANIMATION_FLAG)
      fail("Animated WebP requires a separate frame composition path");
    const auto w = WebPDemuxGetI(demux.get(), WEBP_FF_CANVAS_WIDTH),
               h = WebPDemuxGetI(demux.get(), WEBP_FF_CANVAS_HEIGHT);
    budget(w && h && w <= limits.dimension && h <= limits.dimension &&
           static_cast<std::uint64_t>(w) * h <= limits.decoded_bytes / 16);
    p->desc.alpha =
        flags & ALPHA_FLAG ? AlphaMode::Straight : AlphaMode::Absent;
    ImageSubresource s;
    s.data_window = s.display_window = {0, 0, w, h};
    for (const char *name : {"R", "G", "B", "A"}) {
      if (name[0] == 'A' && p->desc.alpha == AlphaMode::Absent)
        break;
      Buffer b;
      b.scalar = ScalarType::UInt8;
      b.element_count = static_cast<std::uint64_t>(w) * h;
      s.channels.push_back({name, std::move(b), SampleEncoding::UNorm});
    }
    p->desc.subresources.push_back(std::move(s));
    p->desc.source.identity.adapter = "libwebp";
    p->desc.source.identity.adapter_version = "1.6.0";
    WebPChunkIterator chunk{};
    if (WebPDemuxGetChunk(demux.get(), "ICCP", 1, &chunk)) {
      if (chunk.chunk.size <= 1024 * 1024)
        p->desc.source.properties["icc_hex"] =
            hex({reinterpret_cast<const std::byte *>(chunk.chunk.bytes),
                 chunk.chunk.size});
      else {
        WebPDemuxReleaseChunkIterator(&chunk);
        fail("ICC profile budget exceeded", ErrorCode::ResourceLimit);
      }
      WebPDemuxReleaseChunkIterator(&chunk);
    }
  } else {
    Memory memory;
    memory.input = p->bytes;
    memory.stop = stop;
    auto t = open_tiff(memory, limits);
    p->desc.source.identity.adapter = "libtiff";
    p->desc.source.identity.adapter_version = "4.7.2";
    do {
      cancel(stop);
      const auto page = p->desc.subresources.size();
      budget(page < limits.subresources);
      const auto i = tiff_info(t.get(), limits);
      p->desc.subresources.push_back(tiff_descriptor(i, page));
      p->desc.alpha = page == 0                  ? i.alpha
                      : p->desc.alpha == i.alpha ? i.alpha
                                                 : AlphaMode::Unknown;
      const auto key = "page." + std::to_string(page) + ".";
      auto &props = p->desc.source.properties;
      props[key + "orientation"] = std::to_string(i.orientation);
      props[key + "photometric"] = std::to_string(i.photo);
      props[key + "compression"] = std::to_string(i.compression);
      props[key + "alpha"] = std::to_string(static_cast<int>(i.alpha));
      props[key + "layout"] = TIFFIsTiled(t.get()) ? "tiled" : "strip";
      std::uint32_t n = 0;
      void *icc = nullptr;
      if (TIFFGetField(t.get(), TIFFTAG_ICCPROFILE, &n, &icc)) {
        budget(n <= 1024 * 1024);
        props[key + "icc_hex"] = hex({static_cast<const std::byte *>(icc), n});
      }
      // TIFFReadDirectory returning false can mean corrupt metadata; never
      // silently treat that as the end of a multi-page image.
      if (TIFFLastDirectory(t.get()))
        break;
      if (!TIFFReadDirectory(t.get()))
        fail("Invalid following TIFF page");
    } while (true);
  }
  cancel(stop);
  return ExtendedRasterFile(std::move(p));
}
ImageSubresource ExtendedRasterFile::decode(std::size_t entry,
                                            std::stop_token stop) const {
  cancel(stop);
  valid(entry < impl_->desc.subresources.size(), "Image entry out of range");
  auto s = impl_->desc.subresources[entry];
  const auto w = static_cast<std::size_t>(s.data_window.width),
             h = static_cast<std::size_t>(s.data_window.height),
             count = s.channels.size();
  for (auto &c : s.channels)
    c.samples.bytes.resize(w * h * bytes_per(c.samples.scalar));
  if (impl_->webp) {
    std::vector<std::uint8_t> rgba(w * h * 4);
    if (!WebPDecodeRGBAInto(
            reinterpret_cast<const std::uint8_t *>(impl_->bytes.data()),
            impl_->bytes.size(), rgba.data(), rgba.size(),
            static_cast<int>(w * 4))) {
      cancel(stop);
      fail("WebP decode failed");
    }
    for (std::size_t y = 0; y < h; ++y) {
      cancel(stop);
      for (std::size_t x = 0; x < w; ++x)
        for (std::size_t c = 0; c < count; ++c)
          s.channels[c].samples.bytes[y * w + x] =
              static_cast<std::byte>(rgba[(y * w + x) * 4 + c]);
    }
  } else {
    Memory memory;
    memory.input = impl_->bytes;
    memory.stop = stop;
    auto t = open_tiff(memory, impl_->limits);
    if (!TIFFSetDirectory(t.get(), static_cast<tdir_t>(entry))) {
      cancel(stop);
      fail("Missing TIFF page");
    }
    const auto i = tiff_info(t.get(), impl_->limits);
    const auto component = bytes_per(i.scalar),
               samples = i.planar == PLANARCONFIG_CONTIG ? count : 1;
    const auto planes = i.planar == PLANARCONFIG_CONTIG ? 1 : count;
    const auto copy = [&](const std::byte *row, std::size_t y, std::size_t x,
                          std::size_t width, std::size_t plane) {
      for (std::size_t px = 0; px < width; ++px)
        for (std::size_t c = 0; c < samples; ++c)
          std::memcpy(s.channels[i.planar == PLANARCONFIG_CONTIG ? c : plane]
                              .samples.bytes.data() +
                          ((y * w) + x + px) * component,
                      row + (px * samples + c) * component, component);
    };
    if (TIFFIsTiled(t.get())) {
      std::uint32_t tw = 0, th = 0;
      TIFFGetField(t.get(), TIFFTAG_TILEWIDTH, &tw);
      TIFFGetField(t.get(), TIFFTAG_TILELENGTH, &th);
      const auto tile_size = TIFFTileSize64(t.get());
      budget(tw && th && tile_size <= impl_->limits.decoded_bytes / 2 &&
             tile_size <= SIZE_MAX);
      valid(tile_size ==
                static_cast<std::uint64_t>(tw) * th * samples * component,
            "Unsupported TIFF tile layout");
      std::vector<std::byte> tile(static_cast<std::size_t>(tile_size));
      for (std::size_t plane = 0; plane < planes; ++plane)
        for (std::size_t y = 0; y < h; y += th)
          for (std::size_t x = 0; x < w; x += tw) {
            cancel(stop);
            const auto got = TIFFReadTile(t.get(), tile.data(),
                                          static_cast<std::uint32_t>(x),
                                          static_cast<std::uint32_t>(y), 0,
                                          static_cast<std::uint16_t>(plane));
            if (got < 0 || static_cast<std::uint64_t>(got) != tile_size) {
              cancel(stop);
              fail("Truncated TIFF tile");
            }
            for (std::size_t row = 0; row < std::min<std::size_t>(th, h - y);
                 ++row)
              copy(tile.data() + row * tw * samples * component, y + row, x,
                   std::min<std::size_t>(tw, w - x), plane);
          }
    } else {
      const auto row_bytes = TIFFScanlineSize64(t.get());
      valid(row_bytes == w * samples * component,
            "Unsupported TIFF scanline layout");
      budget(row_bytes <= impl_->limits.decoded_bytes / 2);
      std::vector<std::byte> row(static_cast<std::size_t>(row_bytes));
      for (std::size_t plane = 0; plane < planes; ++plane)
        for (std::size_t y = 0; y < h; ++y) {
          cancel(stop);
          if (TIFFReadScanline(t.get(), row.data(),
                               static_cast<std::uint32_t>(y),
                               static_cast<std::uint16_t>(plane)) < 0) {
            cancel(stop);
            fail("Truncated TIFF scanline");
          }
          copy(row.data(), y, 0, w, plane);
        }
    }
  }
  cancel(stop);
  return s;
}

std::vector<std::byte> encode_extended(const ImageSubresource &s,
                                       const ExtendedEncodeOptions &o,
                                       Limits limits, std::stop_token stop) {
  cancel(stop);
  const auto w = s.data_window.width, h = s.data_window.height,
             count = s.channels.size();
  budget(w && h && w <= limits.dimension && h <= limits.dimension && count &&
         count <= 4 && w <= SIZE_MAX / h);
  valid(!s.data_window.x && !s.data_window.y && !s.display_window.x &&
            !s.display_window.y && s.display_window.width == w &&
            s.display_window.height == h,
        "Output requires explicit window flattening");
  const bool gray = count <= 2, alpha = count == 2 || count == 4;
  const std::array<std::string, 4> names =
      gray ? std::array<std::string, 4>{"Y", "A", "", ""}
           : std::array<std::string, 4>{"R", "G", "B", "A"};
  const auto scalar = s.channels[0].samples.scalar;
  const auto encoding = s.channels[0].encoding;
  const auto component = bytes_per(scalar);
  budget(w * h <= limits.decoded_bytes / 64);
  for (std::size_t c = 0; c < count; ++c)
    valid(s.channels[c].name == names[c] &&
              s.channels[c].samples.scalar == scalar &&
              s.channels[c].encoding == encoding &&
              s.channels[c].samples.components == 1 &&
              s.channels[c].samples.element_count == w * h &&
              s.channels[c].samples.bytes.size() == w * h * component,
          "Output needs uniform explicit Y/YA/RGB/RGBA channels");
  valid(!alpha || o.alpha == AlphaMode::Straight ||
            o.alpha == AlphaMode::Premultiplied,
        "Explicit alpha interpretation required");
  budget(o.icc_profile.size() <= 1024 * 1024);
  validate_icc_model(o.icc_profile, gray);
  Memory memory;
  memory.maximum = std::min<std::uint64_t>(limits.input_bytes, SIZE_MAX);
  memory.writing = true;
  memory.stop = stop;
  if (o.target == ExtendedRasterTarget::Tiff) {
    const bool normalized =
        scalar == ScalarType::UInt8 || scalar == ScalarType::UInt16;
    valid(encoding ==
              (normalized ? SampleEncoding::UNorm : SampleEncoding::Numeric),
          "TIFF does not encode this sample interpretation");
    auto t = open_tiff(memory, limits);
    const auto field = [&](auto tag, auto value) {
      if (!TIFFSetField(t.get(), tag, value))
        fail("TIFF output metadata rejected");
    };
    field(TIFFTAG_IMAGEWIDTH, static_cast<std::uint32_t>(w));
    field(TIFFTAG_IMAGELENGTH, static_cast<std::uint32_t>(h));
    field(TIFFTAG_SAMPLESPERPIXEL, static_cast<std::uint16_t>(count));
    field(TIFFTAG_BITSPERSAMPLE, static_cast<std::uint16_t>(component * 8));
    const auto format =
        scalar == ScalarType::Half || scalar == ScalarType::Float32 ||
                scalar == ScalarType::Float64
            ? SAMPLEFORMAT_IEEEFP
        : scalar == ScalarType::Int16 || scalar == ScalarType::Int32
            ? SAMPLEFORMAT_INT
            : SAMPLEFORMAT_UINT;
    field(TIFFTAG_SAMPLEFORMAT, format);
    field(TIFFTAG_PHOTOMETRIC, gray ? PHOTOMETRIC_MINISBLACK : PHOTOMETRIC_RGB);
    field(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    field(TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    const auto compression =
        o.compression == TiffCompression::Lzw        ? COMPRESSION_LZW
        : o.compression == TiffCompression::Deflate  ? COMPRESSION_ADOBE_DEFLATE
        : o.compression == TiffCompression::PackBits ? COMPRESSION_PACKBITS
                                                     : COMPRESSION_NONE;
    field(TIFFTAG_COMPRESSION, compression);
    field(TIFFTAG_ROWSPERSTRIP,
          static_cast<std::uint32_t>(std::min<std::uint64_t>(h, 32)));
    if (alpha) {
      std::uint16_t extra = o.alpha == AlphaMode::Premultiplied
                                ? EXTRASAMPLE_ASSOCALPHA
                                : EXTRASAMPLE_UNASSALPHA;
      if (!TIFFSetField(t.get(), TIFFTAG_EXTRASAMPLES, 1, &extra))
        fail("TIFF alpha metadata rejected");
    }
    if (!o.icc_profile.empty() &&
        !TIFFSetField(t.get(), TIFFTAG_ICCPROFILE,
                      static_cast<std::uint32_t>(o.icc_profile.size()),
                      o.icc_profile.data()))
      fail("TIFF ICC metadata rejected");
    std::vector<std::byte> row(static_cast<std::size_t>(w) * count * component);
    for (std::size_t y = 0; y < h; ++y) {
      cancel(stop);
      for (std::size_t x = 0; x < w; ++x)
        for (std::size_t c = 0; c < count; ++c)
          std::memcpy(row.data() + (x * count + c) * component,
                      s.channels[c].samples.bytes.data() +
                          (y * w + x) * component,
                      component);
      if (TIFFWriteScanline(t.get(), row.data(), static_cast<std::uint32_t>(y),
                            0) < 0) {
        cancel(stop);
        if (memory.failed)
          budget(false);
        fail("TIFF output failed");
      }
    }
    if (!TIFFWriteDirectory(t.get())) {
      cancel(stop);
      if (memory.failed)
        budget(false);
      fail("TIFF finalization failed");
    }
    t.reset();
  } else {
    valid(!gray && scalar == ScalarType::UInt8 &&
              encoding == SampleEncoding::UNorm,
          "WebP requires explicit RGB/RGBA UInt8 quantization");
    valid(!alpha || o.alpha == AlphaMode::Straight,
          "WebP requires straight alpha");
    valid(o.webp_lossless || o.allow_lossy, "Lossy WebP requires consent");
    valid(std::isfinite(o.quality) && o.quality >= 0 && o.quality <= 100,
          "Invalid WebP quality");
    WebPConfig config;
    if (!WebPConfigInit(&config))
      fail("WebP ABI mismatch");
    config.lossless = o.webp_lossless;
    config.quality = o.quality;
    config.method = 4;
    config.exact = 1;
    config.thread_level = 0;
    WebPPicture picture;
    if (!WebPPictureInit(&picture))
      fail("WebP picture ABI mismatch");
    struct Release {
      WebPPicture &p;
      ~Release() { WebPPictureFree(&p); }
    } release{picture};
    picture.width = static_cast<int>(w);
    picture.height = static_cast<int>(h);
    picture.use_argb = 1;
    picture.writer = webp_writer;
    picture.progress_hook = webp_progress;
    picture.custom_ptr = &memory;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w * h) * 4, 255);
    for (std::size_t y = 0; y < h; ++y) {
      cancel(stop);
      for (std::size_t x = 0; x < w; ++x)
        for (std::size_t c = 0; c < count; ++c)
          rgba[(y * w + x) * 4 + c] = std::to_integer<std::uint8_t>(
              s.channels[c].samples.bytes[y * w + x]);
    }
    if (!WebPPictureImportRGBA(&picture, rgba.data(), static_cast<int>(w * 4)))
      budget(false);
    if (!WebPEncode(&config, &picture)) {
      cancel(stop);
      if (memory.failed)
        budget(false);
      fail("WebP encoding failed");
    }
    if (!o.icc_profile.empty()) {
      const WebPData image{
          reinterpret_cast<const std::uint8_t *>(memory.output.data()),
          memory.output.size()};
      std::unique_ptr<WebPMux, decltype(&WebPMuxDelete)> mux(
          WebPMuxCreate(&image, 1), WebPMuxDelete);
      budget(static_cast<bool>(mux));
      const WebPData icc{
          reinterpret_cast<const std::uint8_t *>(o.icc_profile.data()),
          o.icc_profile.size()};
      if (WebPMuxSetChunk(mux.get(), "ICCP", &icc, 1) != WEBP_MUX_OK)
        fail("WebP ICC metadata rejected");
      WebPData out{};
      const auto result = WebPMuxAssemble(mux.get(), &out);
      struct FreeData {
        WebPData &d;
        ~FreeData() { WebPDataClear(&d); }
      } free{out};
      if (result != WEBP_MUX_OK)
        fail("WebP ICC assembly failed");
      budget(out.size <= memory.maximum);
      memory.output.assign(reinterpret_cast<const std::byte *>(out.bytes),
                           reinterpret_cast<const std::byte *>(out.bytes) +
                               out.size);
    }
  }
  cancel(stop);
  budget(!memory.failed);
  return std::move(memory.output);
}
} // namespace formaview::imaging
