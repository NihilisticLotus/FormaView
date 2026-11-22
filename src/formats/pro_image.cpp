#include "formaview/pro_image.hpp"
#include "formaview/image_preview.hpp"
#include "image_input.hpp"
#include <DirectXTex.h>
#include <Imath/half.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfCompression.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfIO.h>
#include <OpenEXR/ImfInputPart.h>
#include <OpenEXR/ImfMultiPartInputFile.h>
#include <OpenEXR/ImfPartType.h>
#include <OpenEXR/ImfStandardAttributes.h>
#include <OpenEXR/ImfTiledInputPart.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace formaview::imaging {
namespace {
namespace exr = OPENEXR_IMF_NAMESPACE;
[[noreturn]] void fail(const char *s) {
  throw skeletal::Failure(ErrorCode::Unsupported, s);
}
void cancel(std::stop_token stop) {
  if (stop.stop_requested())
    throw skeletal::Failure(ErrorCode::Cancelled, "Image read cancelled");
}
void budget(bool ok) {
  if (!ok)
    throw skeletal::Failure(ErrorCode::ResourceLimit, "Image budget exceeded");
}
// Decoder libraries report malformed files with their own exception types.
// Keep the structured contract: malformed input is invalid_argument and a
// failed library allocation is a resource limit, never an internal fault.
template <class F> decltype(auto) library(F &&body) {
  try {
    return body();
  } catch (const skeletal::Failure &) {
    throw;
  } catch (const std::bad_alloc &) {
    throw skeletal::Failure(ErrorCode::ResourceLimit,
                            "Image decoder allocation failed");
  } catch (const std::exception &error) {
    throw skeletal::Failure(ErrorCode::InvalidArgument,
                            std::string("Image decoder rejected the file: ") +
                                error.what());
  }
}
void hr(HRESULT result) {
  if (FAILED(result))
    throw skeletal::Failure(
        ErrorCode::Unsupported,
        "DirectXTex error: " +
            std::to_string(static_cast<unsigned long>(result)));
}
class MemoryStream final : public exr::IStream {
public:
  explicit MemoryStream(std::span<const std::byte> b)
      : exr::IStream("memory"), bytes(b) {}
  bool read(char c[], int n) override {
    if (n < 0 || static_cast<std::size_t>(n) > bytes.size() - offset)
      throw std::runtime_error("Truncated EXR");
    std::memcpy(c, bytes.data() + offset, static_cast<std::size_t>(n));
    offset += static_cast<std::size_t>(n);
    return offset < bytes.size();
  }
  std::uint64_t tellg() override { return offset; }
  void seekg(std::uint64_t p) override {
    if (p > bytes.size())
      throw std::runtime_error("EXR seek out of bounds");
    offset = static_cast<std::size_t>(p);
  }

private:
  std::span<const std::byte> bytes;
  std::size_t offset{};
};
ImageWindow window(const IMATH_NAMESPACE::Box2i &box) {
  const auto w = static_cast<std::int64_t>(box.max.x) - box.min.x + 1;
  const auto h = static_cast<std::int64_t>(box.max.y) - box.min.y + 1;
  if (w <= 0 || h <= 0)
    fail("Invalid EXR window");
  return {box.min.x, box.min.y, static_cast<std::uint64_t>(w),
          static_cast<std::uint64_t>(h)};
}
std::size_t scalar_size(ScalarType t) {
  if (t == ScalarType::Float64)
    return 8;
  return t == ScalarType::Half || t == ScalarType::UInt16 ||
                 t == ScalarType::Int16
             ? 2
         : t == ScalarType::UInt8 ? 1
                                  : 4;
}
struct Layout {
  ScalarType type;
  std::uint32_t count;
  bool normalized;
  bool bgra{};
};
struct PackedUnorm {
  unsigned bytes{};
  std::array<std::uint32_t, 4> masks{};

  std::uint32_t read(const std::uint8_t *pixel) const {
    std::uint32_t word{};
    std::memcpy(&word, pixel, bytes);
    return word;
  }
  float sample(std::uint32_t word, unsigned channel) const {
    const auto mask = masks[channel];
    if (!mask)
      return 1.F;
    const auto shift = std::countr_zero(mask);
    return float((word & mask) >> shift) / float(mask >> shift);
  }
};
PackedUnorm packedUnorm(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_B5G6R5_UNORM: return {2, {0xf800, 0x07e0, 0x001f, 0}};
  case DXGI_FORMAT_B5G5R5A1_UNORM: return {2, {0x7c00, 0x03e0, 0x001f, 0x8000}};
  case DXGI_FORMAT_B4G4R4A4_UNORM: return {2, {0x0f00, 0x00f0, 0x000f, 0xf000}};
  case DXGI_FORMAT_R10G10B10A2_UNORM: return {4, {0x3ff, 0xffc00, 0x3ff00000, 0xc0000000}};
  default: return {};
  }
}
Layout layout(DXGI_FORMAT f) {
  switch (f) {
  case DXGI_FORMAT_R32G32B32A32_FLOAT:
    return {ScalarType::Float32, 4, false};
  case DXGI_FORMAT_R32G32B32A32_UINT:
    return {ScalarType::UInt32, 4, false};
  case DXGI_FORMAT_R32G32B32_FLOAT:
    return {ScalarType::Float32, 3, false};
  case DXGI_FORMAT_R32G32_FLOAT:
    return {ScalarType::Float32, 2, false};
  case DXGI_FORMAT_R32G32_UINT:
    return {ScalarType::UInt32, 2, false};
  case DXGI_FORMAT_R32_FLOAT:
    return {ScalarType::Float32, 1, false};
  case DXGI_FORMAT_R32_UINT:
    return {ScalarType::UInt32, 1, false};
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
    return {ScalarType::Half, 4, false};
  case DXGI_FORMAT_R16G16_FLOAT:
    return {ScalarType::Half, 2, false};
  case DXGI_FORMAT_R16_FLOAT:
    return {ScalarType::Half, 1, false};
  case DXGI_FORMAT_R16G16B16A16_UNORM:
    return {ScalarType::UInt16, 4, true};
  case DXGI_FORMAT_R16G16_UNORM:
    return {ScalarType::UInt16, 2, true};
  case DXGI_FORMAT_R16_UNORM:
    return {ScalarType::UInt16, 1, true};
  case DXGI_FORMAT_R16_UINT:
    return {ScalarType::UInt16, 1, false};
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    return {ScalarType::UInt8, 4, true};
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8X8_UNORM:
  case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    return {ScalarType::UInt8, 4, true, true};
  case DXGI_FORMAT_R8G8_UNORM:
    return {ScalarType::UInt8, 2, true};
  case DXGI_FORMAT_R8_UNORM:
    return {ScalarType::UInt8, 1, true};
  case DXGI_FORMAT_R8_UINT:
    return {ScalarType::UInt8, 1, false};
  default:
    fail("DDS format has no validated native channel layout");
  }
}
} // namespace
struct ImageFile::Impl {
  Limits limits;
  ImageInput bytes;
  ImageData desc;
  bool is_exr{}, hdr{}, tga{};
  std::uint32_t tga_channels{};
  bool legacy_expanded{};
  std::uint32_t legacy_bits{};
  std::size_t legacy_palette{};
  DirectX::ScratchImage dds;
  DirectX::TexMetadata native_metadata{};
  std::vector<DirectX::Image> surfaces;
  std::vector<std::array<std::size_t, 3>> indices;
};
ImageFile::ImageFile(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
ImageFile::ImageFile(ImageFile &&) noexcept = default;
ImageFile &ImageFile::operator=(ImageFile &&) noexcept = default;
ImageFile::~ImageFile() = default;
const ImageData &ImageFile::description() const { return impl_->desc; }
std::size_t ImageFile::resident_bytes() const {
  return impl_->bytes.resident_bytes() + impl_->dds.GetPixelsSize();
}
ImageFile ImageFile::open(const std::filesystem::path &path, Limits limits,
                          std::stop_token stop) {
  return library([&]() -> ImageFile {
  cancel(stop);
  auto p = std::make_unique<Impl>();
  p->limits = limits;
  if (limits.preview) {
    p->bytes.map(path);
  } else {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
      throw skeletal::Failure(ErrorCode::Io, "Cannot open image");
    const auto size = file.tellg();
    budget(size >= 0 && static_cast<std::uint64_t>(size) <= limits.input_bytes);
    p->bytes.resize(static_cast<std::size_t>(size));
    file.seekg(0);
    for (std::size_t offset = 0; offset < p->bytes.size();) {
      cancel(stop);
      const auto count =
          std::min<std::size_t>(1024 * 1024, p->bytes.size() - offset);
      if (!file.read(reinterpret_cast<char *>(p->bytes.data() + offset),
                     static_cast<std::streamsize>(count)))
        throw skeletal::Failure(ErrorCode::Io, "Short image read");
      offset += count;
    }
    p->desc.source.identity.sha256 = skeletal::sha256(p->bytes, stop);
  }
  p->is_exr = p->bytes.size() >= 4 &&
              std::memcmp(p->bytes.data(), "\x76\x2f\x31\x01", 4) == 0;
  if (p->is_exr) {
    p->desc.source.identity.adapter = "OpenEXR";
    p->desc.source.identity.adapter_version = "3.5.2";
    MemoryStream stream(p->bytes);
    exr::MultiPartInputFile input(stream, 1);
    budget(input.parts() > 0 &&
           static_cast<unsigned>(input.parts()) <= limits.subresources);
    for (int part = 0; part < input.parts(); ++part) {
      const auto &h = input.header(part);
      const auto prefix = "part." + std::to_string(part) + ".";
      p->desc.source.properties[prefix + "compression"] =
          std::to_string(h.compression());
      p->desc.source.properties[prefix + "pixel_aspect_ratio"] =
          std::to_string(h.pixelAspectRatio());
      if (exr::hasChromaticities(h)) {
        const auto &c = exr::chromaticities(h);
        std::ostringstream values;
        values << std::setprecision(9) << c.red.x << ',' << c.red.y << ','
               << c.green.x << ',' << c.green.y << ',' << c.blue.x << ','
               << c.blue.y << ',' << c.white.x << ',' << c.white.y;
        p->desc.source.properties[prefix + "chromaticities"] = values.str();
      }
      if (exr::hasWhiteLuminance(h))
        p->desc.source.properties[prefix + "white_luminance"] =
            std::to_string(exr::whiteLuminance(h));
      if (h.type() != exr::SCANLINEIMAGE && h.type() != exr::TILEDIMAGE)
        fail("Deep/non-image EXR is outside the ordinary-image path");
      ImageSubresource s;
      s.part = static_cast<std::uint32_t>(part);
      s.part_name = h.hasName() ? h.name() : "";
      s.data_window = window(h.dataWindow());
      s.display_window = window(h.displayWindow());
      budget(s.data_window.width <= limits.dimension &&
             s.data_window.height <= limits.dimension);
      for (auto c = h.channels().begin(); c != h.channels().end(); ++c) {
        if (c.channel().xSampling != 1 || c.channel().ySampling != 1)
          fail("Subsampled EXR channels require a separate sampling path");
        Buffer b;
        b.scalar = c.channel().type == exr::HALF    ? ScalarType::Half
                   : c.channel().type == exr::FLOAT ? ScalarType::Float32
                                                    : ScalarType::UInt32;
        b.element_count = s.data_window.width * s.data_window.height;
        s.channels.push_back({c.name(), std::move(b)});
      }
      budget(s.channels.size() <= limits.channels);
      p->desc.subresources.push_back(std::move(s));
    }
    // Alpha and color interpretation may vary by layer/part. No global guess.
    p->desc.alpha = AlphaMode::Unknown;
  } else {
    auto extension = path.extension().wstring();
    std::transform(
        extension.begin(), extension.end(), extension.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    p->tga = extension.size() == 4 && extension[0] == L'.' && extension[1] == L't' &&
             extension[2] == L'g' && extension[3] == L'a';
    p->hdr = !p->tga && !(p->bytes.size() >= 4 &&
                          std::memcmp(p->bytes.data(), "DDS ", 4) == 0);
    DirectX::TexMetadata meta;
    if (p->tga)
      hr(DirectX::GetMetadataFromTGAMemory(
          p->bytes.data(), p->bytes.size(),
          DirectX::TGA_FLAGS_ALLOW_ALL_ZERO_ALPHA, meta));
    else if (p->hdr)
      hr(DirectX::GetMetadataFromHDRMemory(p->bytes.data(), p->bytes.size(),
                                           meta));
    else
      hr(DirectX::GetMetadataFromDDSMemory(p->bytes.data(), p->bytes.size(),
                                           DirectX::DDS_FLAGS_NONE, meta));
    if (p->tga) {
      // The decoded DXGI surface can contain a synthesized fourth component.
      // Expose source channels, not the padding used by the codec.
      const auto bits = std::to_integer<unsigned>(p->bytes.at(16));
      const auto attributes = std::to_integer<unsigned>(p->bytes.at(17)) & 15U;
      const auto type = std::to_integer<unsigned>(p->bytes.at(2));
      if (type == 1 || type == 9) {
        if (std::to_integer<unsigned>(p->bytes.at(7)) != 24 ||
            meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
          fail("Unsupported expanded TGA palette format");
        p->tga_channels = 3;
      } else
        p->tga_channels = bits == 8                         ? 1U
                          : (bits == 24 || attributes == 0) ? 3U
                                                            : 4U;
      p->desc.source.properties["tga_pixel_bits"] = std::to_string(bits);
      p->desc.source.properties["tga_descriptor"] =
          std::to_string(std::to_integer<unsigned>(p->bytes.at(17)));
      if (limits.preview && p->tga_channels == 4 &&
          (meta.GetAlphaMode() == DirectX::TEX_ALPHA_MODE_STRAIGHT ||
           meta.GetAlphaMode() == DirectX::TEX_ALPHA_MODE_PREMULTIPLIED)) {
        // DirectXTex reports a fully opaque TGA as opaque even when its
        // footer declares straight/premultiplied alpha. Preserve that policy
        // without allocating the decoded surface.
        const auto stride = bits / 8;
        auto cursor =
            std::size_t(18 + std::to_integer<unsigned>(p->bytes.at(0)));
        const auto pixels = std::uint64_t(meta.width) * meta.height;
        std::uint64_t done = 0;
        bool opaque = true;
        const auto alpha = [&](std::size_t at) {
          const auto v =
              std::to_integer<unsigned>(p->bytes.at(at + stride - 1));
          return stride == 2 ? (v & 128) != 0 : v == 255;
        };
        while (done < pixels && opaque) {
          cancel(stop);
          unsigned n = 1;
          bool repeat = false;
          if (type >= 9) {
            const auto packet =
                std::to_integer<unsigned>(p->bytes.at(cursor++));
            n = (packet & 127) + 1;
            repeat = (packet & 128) != 0;
          }
          if (n > pixels - done || cursor > p->bytes.size() ||
              std::uint64_t(repeat ? 1 : n) * stride > p->bytes.size() - cursor)
            fail("Truncated TGA alpha stream");
          for (unsigned k = 0; k < (repeat ? 1 : n); ++k)
            if (!alpha(cursor + std::size_t(k) * stride)) {
              opaque = false;
              break;
            }
          cursor += std::size_t(repeat ? 1 : n) * stride;
          done += n;
        }
        if (opaque)
          meta.SetAlphaMode(DirectX::TEX_ALPHA_MODE_OPAQUE);
      }
    }
    budget(meta.width <= limits.dimension && meta.height <= limits.dimension &&
           meta.depth <= limits.dimension);
    budget(meta.arraySize <= limits.subresources && meta.mipLevels <= 32);
    std::uint64_t resident = 0, count = 0;
    for (std::size_t m = 0; m < meta.mipLevels; ++m) {
      std::size_t row = 0, slice = 0;
      hr(DirectX::ComputePitch(
          meta.format, std::max<std::size_t>(1, meta.width >> m),
          std::max<std::size_t>(1, meta.height >> m), row, slice));
      const auto planes =
          meta.arraySize * std::max<std::size_t>(1, meta.depth >> m);
      budget(planes <= limits.subresources &&
             (limits.preview || slice <= limits.decoded_bytes / planes));
      resident += slice * planes;
      count += planes;
      budget((limits.preview || resident <= limits.decoded_bytes) &&
             count <= limits.subresources);
    }
    if (!limits.preview) {
      if (p->tga)
        hr(DirectX::LoadFromTGAMemory(p->bytes.data(), p->bytes.size(),
                                      DirectX::TGA_FLAGS_ALLOW_ALL_ZERO_ALPHA,
                                      &meta, p->dds));
      else if (p->hdr)
        hr(DirectX::LoadFromHDRMemory(p->bytes.data(), p->bytes.size(), &meta,
                                      p->dds));
      else
        hr(DirectX::LoadFromDDSMemory(p->bytes.data(), p->bytes.size(),
                                      DirectX::DDS_FLAGS_NONE, &meta, p->dds));
    }
    p->native_metadata = meta;
    p->desc.source.identity.adapter = "DirectXTex";
    p->desc.source.identity.adapter_version = "2.1.1-may2026";
    if (p->hdr)
      p->desc.source.properties["hdr_decode"] =
          "RGBE midpoint: (mantissa + 0.5) * 2^(exponent-136), divided by "
          "header EXPOSURE; RGB float32, no alpha";
    if (p->tga)
      p->desc.source.properties["tga_decode"] =
          "Stored channel values; all-zero alpha preserved; origin normalized "
          "by DirectXTex";
    p->desc.source.properties["dxgi_format"] = std::to_string(meta.format);
    if (!p->hdr && !p->tga)
      p->desc.source.properties["dds_alpha_mode"] = std::to_string(meta.GetAlphaMode());
    p->desc.colour_space =
        DirectX::IsSRGB(meta.format) ? "sRGB" : "unspecified";
    p->desc.alpha =
        p->hdr || (p->tga && p->tga_channels != 4) ||
                (!p->tga && !DirectX::HasAlpha(meta.format)) ? AlphaMode::Absent
        : meta.GetAlphaMode() == DirectX::TEX_ALPHA_MODE_PREMULTIPLIED
            ? AlphaMode::Premultiplied
        : meta.GetAlphaMode() == DirectX::TEX_ALPHA_MODE_STRAIGHT
            ? AlphaMode::Straight
        : meta.GetAlphaMode() == DirectX::TEX_ALPHA_MODE_OPAQUE && !p->tga
            ? AlphaMode::Absent
            : AlphaMode::Unknown;
    std::size_t offset = 0;
    if (limits.preview && !p->tga && !p->hdr) {
      std::uint32_t fourcc{};
      if (p->bytes.size() < 128)
        fail("Truncated DDS header");
      std::memcpy(&fourcc, p->bytes.data() + 84, 4);
      offset = fourcc == 0x30315844 ? 148 : 128;
      std::uint32_t flags{}, bits{};
      std::memcpy(&flags, p->bytes.data() + 80, 4);
      std::memcpy(&bits, p->bytes.data() + 88, 4);
      p->legacy_expanded = fourcc != 0x30315844 && !DirectX::IsCompressed(meta.format) && bits &&
          (bits < DirectX::BitsPerPixel(meta.format) || ((flags&0x40) && !(flags&1) && DirectX::HasAlpha(meta.format) && bits==32));
      if(p->legacy_expanded) {
        if(bits!=8 && bits!=16 && bits!=24 && bits!=32) fail("Unsupported expanded DDS source depth");
        p->legacy_bits=bits;p->legacy_palette=(flags&0x20)?1024:0;
        offset+=p->legacy_palette;
      }
    }
    for (std::size_t a = 0; a < meta.arraySize; ++a)
      for (std::size_t m = 0; m < meta.mipLevels; ++m)
        for (std::size_t z = 0; z < std::max<std::size_t>(1, meta.depth >> m);
             ++z) {
          DirectX::Image view{};
          if (limits.preview) {
            view.width = std::max<std::size_t>(1, meta.width >> m);
            view.height = std::max<std::size_t>(1, meta.height >> m);
            view.format = meta.format;
            hr(DirectX::ComputePitch(meta.format, view.width, view.height,
                                     view.rowPitch, view.slicePitch));
            if (p->legacy_expanded) {
              view.rowPitch = view.width * (p->legacy_bits/8);
              view.slicePitch = view.rowPitch * view.height;
            }
            if (!p->tga && !p->hdr) {
              if (offset > p->bytes.size() ||
                  view.slicePitch > p->bytes.size() - offset)
                fail("Truncated DDS surface");
              view.pixels =
                  reinterpret_cast<std::uint8_t *>(p->bytes.data() + offset);
              offset += view.slicePitch;
            }
            p->surfaces.push_back(view);
          }
          const auto *image = limits.preview ? &view : p->dds.GetImage(m, a, z);
          if (!image)
            fail("Missing DDS subresource");
          ImageSubresource s;
          s.mip = static_cast<std::uint32_t>(m);
          s.array_layer =
              static_cast<std::uint32_t>(meta.IsCubemap() ? a / 6 : a);
          s.cube_face =
              static_cast<std::uint32_t>(meta.IsCubemap() ? a % 6 : 0);
          s.depth_slice = static_cast<std::uint32_t>(z);
          s.data_window =
              s.display_window = {0, 0, image->width, image->height};
          if (DirectX::IsCompressed(meta.format))
            s.block_encoding = "DXGI:" + std::to_string(meta.format);
          p->desc.subresources.push_back(std::move(s));
          p->indices.push_back({m, a, z});
        }
  }
  cancel(stop);
  return ImageFile(std::move(p));
  });
}
ImageSubresource ImageFile::decode(std::size_t index,
                                   std::stop_token stop) const {
  if (impl_->limits.preview)
    fail("Preview documents require decode_preview");
  return library([&]() -> ImageSubresource {
  cancel(stop);
  if (index >= impl_->desc.subresources.size())
    throw skeletal::Failure(ErrorCode::InvalidArgument,
                            "Subresource out of range");
  auto s = impl_->desc.subresources[index];
  const auto pixels = s.data_window.width * s.data_window.height;
  if (impl_->is_exr) {
    std::uint64_t total = 0;
    for (auto &c : s.channels) {
      total += pixels * scalar_size(c.samples.scalar);
      budget(total <= impl_->limits.decoded_bytes);
    }
    MemoryStream stream(impl_->bytes);
    exr::MultiPartInputFile input(stream, 1);
    const auto &h = input.header(static_cast<int>(s.part));
    exr::FrameBuffer frame;
    for (auto &c : s.channels) {
      c.samples.bytes.resize(static_cast<std::size_t>(pixels) *
                             scalar_size(c.samples.scalar));
      const auto type = c.samples.scalar == ScalarType::Half      ? exr::HALF
                        : c.samples.scalar == ScalarType::Float32 ? exr::FLOAT
                                                                  : exr::UINT;
      frame.insert(c.name, exr::Slice::Make(type, c.samples.bytes.data(),
                                            h.dataWindow()));
    }
    if (h.type() == exr::TILEDIMAGE) {
      exr::TiledInputPart part(input, static_cast<int>(s.part));
      part.setFrameBuffer(frame);
      // Level zero is explicit; additional EXR mip/ripmap levels are not
      // advertised.
      part.readTiles(0, part.numXTiles(0) - 1, 0, part.numYTiles(0) - 1, 0, 0);
    } else {
      exr::InputPart part(input, static_cast<int>(s.part));
      part.setFrameBuffer(frame);
      for (int y = h.dataWindow().min.y;;) {
        cancel(stop);
        const auto end = static_cast<int>(std::min<std::int64_t>(
            static_cast<std::int64_t>(y) + 63, h.dataWindow().max.y));
        part.readPixels(y, end);
        if (end == h.dataWindow().max.y)
          break;
        y = end + 1;
      }
    }
  } else {
    const auto idx = impl_->indices[index];
    const auto *native = impl_->dds.GetImage(idx[0], idx[1], idx[2]);
    budget(pixels <= impl_->limits.decoded_bytes / 32 &&
           native->slicePitch <= impl_->limits.decoded_bytes - pixels * 32);
    s.encoded_blocks.resize(native->slicePitch);
    std::memcpy(s.encoded_blocks.data(), native->pixels, native->slicePitch);
    DirectX::ScratchImage decoded;
    auto format = native->format;
    if (DirectX::IsCompressed(format)) {
      // Decompress numeric encoded samples. DirectXTex otherwise linearizes
      // sRGB when converting to float; transfer conversion belongs to display.
      auto encoded = *native;
      encoded.format = DirectX::MakeLinear(format);
      hr(DirectX::Decompress(encoded, DXGI_FORMAT_R32G32B32A32_FLOAT, decoded));
      native = decoded.GetImage(0, 0, 0);
      format = native->format;
    } else if (packedUnorm(format).bytes) {
      // Packed source words stay available in encoded_blocks, decoded values
      // are UNORM.
      hr(DirectX::Convert(*native, DXGI_FORMAT_R32G32B32A32_FLOAT,
                          DirectX::TEX_FILTER_DEFAULT, 0, decoded));
      native = decoded.GetImage(0, 0, 0);
      format = native->format;
    }
    auto l = layout(format);
    const auto stored_count = l.count;
    const auto source_format = impl_->dds.GetMetadata().format;
    if (source_format == DXGI_FORMAT_BC4_UNORM ||
        source_format == DXGI_FORMAT_BC4_SNORM)
      l.count = 1;
    if (source_format == DXGI_FORMAT_BC5_UNORM ||
        source_format == DXGI_FORMAT_BC5_SNORM)
      l.count = 2;
    if (source_format == DXGI_FORMAT_BC6H_UF16 ||
        source_format == DXGI_FORMAT_BC6H_SF16)
      l.count = 3;
    if (impl_->hdr)
      l.count = 3;
    if (impl_->tga)
      l.count = impl_->tga_channels;
    if (!impl_->hdr && !impl_->tga && l.count == 4 &&
        impl_->desc.alpha == AlphaMode::Absent) {
      // Opaque DDS padding is not a semantic A channel. Reject contradictory
      // declarations rather than silently discard meaningful stored samples.
      if (DirectX::HasAlpha(source_format)) {
        for (std::size_t y = 0; y < native->height; ++y) {
          cancel(stop);
          for (std::size_t x = 0; x < native->width; ++x) {
            const auto *a = native->pixels + y * native->rowPitch +
                            (x * stored_count + 3) * scalar_size(l.type);
            bool opaque = false;
            if (l.type == ScalarType::UInt8) opaque = *a == 255;
            else if (l.type == ScalarType::UInt16 || l.type == ScalarType::Half) {
              std::uint16_t v; std::memcpy(&v, a, sizeof(v));
              opaque = v == (l.type == ScalarType::Half ? 0x3c00 : 65535);
            } else if (l.type == ScalarType::Float32) {
              float v; std::memcpy(&v, a, sizeof(v)); opaque = v == 1.F;
            } else if (l.type == ScalarType::UInt32) {
              std::uint32_t v; std::memcpy(&v, a, sizeof(v)); opaque = v == 1;
            }
            if (!opaque) fail("DDS opaque alpha declaration conflicts with stored values");
          }
        }
      }
      l.count = 3;
    }
    auto names = l.bgra ? std::array<std::string, 4>{"B", "G", "R", "A"}
                        : std::array<std::string, 4>{"R", "G", "B", "A"};
    if (impl_->tga && l.count == 1)
      names[0] = "Y";
    const auto component = scalar_size(l.type);
    const auto stride = (impl_->hdr || DirectX::IsCompressed(source_format))
                            ? 16
                            : component * stored_count;
    for (std::uint32_t c = 0; c < l.count; ++c) {
      Buffer b;
      b.scalar = l.type;
      b.element_count = pixels;
      b.bytes.resize(static_cast<std::size_t>(pixels) * component);
      for (std::size_t y = 0; y < native->height; ++y) {
        cancel(stop);
        for (std::size_t x = 0; x < native->width; ++x)
          std::memcpy(b.bytes.data() + (y * native->width + x) * component,
                      native->pixels + y * native->rowPitch + x * stride +
                          c * component,
                      component);
      }
      s.channels.push_back(
          {names[c], std::move(b),
           l.normalized ? SampleEncoding::UNorm : SampleEncoding::Numeric});
    }
  }
  cancel(stop);
  return s;
  });
}
ImageSubresource ImageFile::decode_preview(std::size_t index,
                                           const PreviewRequest &r,
                                           std::stop_token stop) const {
  return library([&]() -> ImageSubresource {
    cancel(stop);
    if (index >= impl_->desc.subresources.size())
      fail("Preview entry out of range");
    auto descriptor = impl_->desc.subresources[index];
    r.validate(descriptor);
    const auto ow = r.output_width, oh = r.output_height;
    const auto add = [&](ImageSubresource &s, const std::string &name,
                         ScalarType type, SampleEncoding encoding) {
      Buffer b;
      b.scalar = type;
      s.channels.push_back({name, std::move(b), encoding});
    };
    const auto copyPatch = [&](ImageSubresource &out,
                               const ImageSubresource &patch,
                               std::uint64_t left, std::uint64_t top) {
      const auto pw = patch.data_window.width, ph = patch.data_window.height;
      for (auto y = r.first_y(top); y < r.first_y(top + ph); ++y) {
        const auto sy = r.sy(y);
        if (sy < top || sy - top >= ph)
          continue;
        cancel(stop);
        for (auto x = r.first_x(left); x < r.first_x(left + pw); ++x) {
          const auto sx = r.sx(x);
          if (sx < left || sx - left >= pw)
            continue;
          for (std::size_t c = 0; c < out.channels.size(); ++c) {
            const auto bytes = scalar_size(out.channels[c].samples.scalar);
            std::memcpy(out.channels[c].samples.bytes.data() +
                            (y * ow + x) * bytes,
                        patch.channels[c].samples.bytes.data() +
                            ((sy - top) * pw + sx - left) * bytes,
                        bytes);
          }
        }
      }
    };
    if (impl_->is_exr) {
      auto out = preview_output(descriptor, r);
      MemoryStream stream(impl_->bytes);
      exr::MultiPartInputFile input(stream, 1);
      const auto &h = input.header(static_cast<int>(descriptor.part));
      std::uint64_t pixelBytes = 0;
      for (const auto &c : descriptor.channels)
        pixelBytes += scalar_size(c.samples.scalar);
      if (h.type() == exr::SCANLINEIMAGE) {
        const auto lines = exr::getCompressionNumScanlines(h.compression());
        const auto &bounds = h.dataWindow();
        const auto width = descriptor.data_window.width;
        budget(lines > 0 && pixelBytes &&
               width <= 64ULL * 1024 * 1024 / pixelBytes /
                            static_cast<std::uint64_t>(lines));
        const auto chunkHeight = static_cast<std::uint64_t>(lines);
        auto scratch = descriptor.channels;
        for (auto &c : scratch)
          c.samples.bytes.resize(
              width * chunkHeight * scalar_size(c.samples.scalar));
        const bool contiguousX = r.width == ow;
        std::vector<std::uint64_t> sampleX;
        if (!contiguousX) {
          sampleX.reserve(static_cast<std::size_t>(ow));
          for (std::uint64_t x = 0; x < ow; ++x)
            sampleX.push_back(r.sx(x));
        }

        exr::InputPart part(input, static_cast<int>(descriptor.part));
        // A scanline request decompresses its containing chunk. Read all
        // needed rows of that chunk at once and skip unsampled chunks.
        for (std::uint64_t y = 0; y < oh;) {
          cancel(stop);
          const auto first = r.sy(y);
          const auto next = r.first_y((first / chunkHeight + 1) * chunkHeight);
          const auto sourceRow = [&](std::uint64_t row) {
            return static_cast<int>(std::int64_t(bounds.min.y) +
                                    static_cast<std::int64_t>(row));
          };
          const auto firstRow = sourceRow(first);
          const auto lastRow = sourceRow(r.sy(next - 1));
          const IMATH_NAMESPACE::Box2i box{{bounds.min.x, firstRow},
                                           {bounds.max.x, lastRow}};
          exr::FrameBuffer frame;
          for (auto &c : scratch) {
            const auto type = c.samples.scalar == ScalarType::Half ? exr::HALF
                              : c.samples.scalar == ScalarType::Float32
                                  ? exr::FLOAT
                                  : exr::UINT;
            frame.insert(c.name,
                         exr::Slice::Make(type, c.samples.bytes.data(), box));
          }
          part.setFrameBuffer(frame);
          part.readPixels(firstRow, lastRow);
          for (auto row = y; row < next; ++row) {
            cancel(stop);
            const auto rowOffset = (r.sy(row) - first) * width;
            for (std::size_t c = 0; c < scratch.size(); ++c) {
              const auto bytes = scalar_size(scratch[c].samples.scalar);
              const auto *source = scratch[c].samples.bytes.data() +
                                   rowOffset * bytes;
              auto *dest =
                  out.channels[c].samples.bytes.data() + row * ow * bytes;
              if (contiguousX) {
                std::memcpy(dest, source + r.x * bytes, ow * bytes);
              } else {
                for (std::uint64_t x = 0; x < ow; ++x)
                  std::memcpy(dest + x * bytes, source + sampleX[x] * bytes,
                              bytes);
              }
            }
          }
          y = next;
        }
        return out;
      }
      const auto readPatch = [&](IMATH_NAMESPACE::Box2i box, auto &part,
                                 auto read) {
        auto patch = descriptor;
        patch.data_window = window(box);
        const auto n = patch.data_window.width * patch.data_window.height;
        std::uint64_t bytes = 0;
        exr::FrameBuffer frame;
        for (auto &c : patch.channels) {
          bytes += n * scalar_size(c.samples.scalar);
          budget(bytes <= 64ULL * 1024 * 1024);
          c.samples.bytes.resize(static_cast<std::size_t>(n) *
                                 scalar_size(c.samples.scalar));
          const auto type = c.samples.scalar == ScalarType::Half ? exr::HALF
                            : c.samples.scalar == ScalarType::Float32
                                ? exr::FLOAT
                                : exr::UINT;
          frame.insert(c.name,
                       exr::Slice::Make(type, c.samples.bytes.data(), box));
        }
        part.setFrameBuffer(frame);
        cancel(stop);
        read();
        copyPatch(out, patch,
                  static_cast<std::uint64_t>(std::int64_t(box.min.x) -
                                             h.dataWindow().min.x),
                  static_cast<std::uint64_t>(std::int64_t(box.min.y) -
                                             h.dataWindow().min.y));
      };
      if (h.type() == exr::TILEDIMAGE) {
        exr::TiledInputPart part(input, static_cast<int>(descriptor.part));
        const auto tw = part.tileXSize(), th = part.tileYSize();
        budget(std::uint64_t(tw) * th <= 64ULL * 1024 * 1024 / pixelBytes);
        std::uint64_t previousY = UINT64_MAX;
        for (std::uint64_t y = 0; y < oh; ++y) {
          const auto ty = r.sy(y) / th;
          if (ty == previousY)
            continue;
          previousY = ty;
          std::uint64_t previousX = UINT64_MAX;
          for (std::uint64_t x = 0; x < ow; ++x) {
            const auto tx = r.sx(x) / tw;
            if (tx == previousX)
              continue;
            previousX = tx;
            readPatch(part.dataWindowForTile(static_cast<int>(tx),
                                             static_cast<int>(ty)),
                      part, [&] {
                        part.readTile(static_cast<int>(tx),
                                      static_cast<int>(ty));
                      });
          }
        }
      }
      return out;
    }
    const auto *data =
        reinterpret_cast<const std::uint8_t *>(impl_->bytes.data());
    const auto length = impl_->bytes.size();
    std::size_t cursor = 0;
    const auto byte = [&]() {
      if (cursor >= length)
        fail("Truncated preview stream");
      return data[cursor++];
    };
    if (impl_->tga || impl_->hdr) {
      const bool hdr = impl_->hdr;
      const auto scalar =
          hdr ? ScalarType::Float32
          : impl_->native_metadata.format == DXGI_FORMAT_B5G5R5A1_UNORM
              ? ScalarType::Float32
              : ScalarType::UInt8;
      const auto count = hdr ? 3U : impl_->tga_channels;
      for (std::uint32_t c = 0; c < count; ++c)
        add(descriptor,
            count == 1 ? "Y"
                       : std::array<std::string, 4>{"R", "G", "B", "A"}[c],
            scalar,
            scalar == ScalarType::UInt8 ? SampleEncoding::UNorm
                                        : SampleEncoding::Numeric);
      auto out = preview_output(descriptor, r);
      const auto w = descriptor.data_window.width,
                 height = descriptor.data_window.height;
      budget(w <= 64ULL * 1024 * 1024 / 16);
      ImageSubresource row = descriptor;
      row.data_window = {0, 0, w, 1};
      for (auto &c : row.channels)
        c.samples.bytes.resize(static_cast<std::size_t>(w) *
                               scalar_size(scalar));
      float exposure = 1;
      bool top = true, right = false;
      int bits = 0, type = 0;
      std::vector<std::array<std::uint8_t, 4>> palette;
      unsigned paletteFirst = 0;
      if (!hdr) {
        if (length < 18)
          fail("Truncated TGA header");
        type = data[2];
        bits = data[16];
        top = (data[17] & 32) != 0;
        right = (data[17] & 16) != 0;
        if (data[17] & 0xc0)
          fail("Interleaved TGA is unsupported");
        cursor = 18 + data[0];
        if (data[1]) {
          const auto first = unsigned(data[3]) | (unsigned(data[4]) << 8),
                     n = unsigned(data[5]) | (unsigned(data[6]) << 8);
          if (data[7] != 24 || first + n > 65536)
            fail("Unsupported TGA palette");
          palette.resize(first + n);
          paletteFirst = first;
          for (unsigned i = first; i < first + n; ++i) {
            const auto b = byte(), g = byte(), red = byte();
            palette[i] = {red, g, b, 255};
          }
        }
      } else {
        const auto line = [&]() {
          std::string result;
          for (;;) {
            const auto b = byte();
            if (b == '\n')
              break;
            if (b != '\r')
              result += char(b);
            if (result.size() > 65536)
              fail("HDR header line too long");
          }
          return result;
        };
        for (;;) {
          const auto text = line();
          if (text.empty())
            break;
          if (text.starts_with("EXPOSURE=")) {
            const auto value = static_cast<float>(std::atof(text.c_str() + 9));
            if (value >= 1e-12F && value <= 1e12F)
              exposure *= value;
          }
        }
        const auto resolution = line();
        std::istringstream parser(resolution);
        std::string ya, xa;
        std::uint64_t rh{}, rw{};
        parser >> ya >> rh >> xa >> rw;
        if (!parser || (ya != "-Y" && ya != "+Y") ||
            (xa != "+X" && xa != "-X") || rw != w || rh != height)
          fail("Unsupported HDR axis order");
        top = ya == "-Y";
        right = xa == "-X";
      }
      std::vector<std::array<std::uint8_t, 4>> encoded(
          static_cast<std::size_t>(w));
      unsigned remaining = 0;
      bool repeat = false;
      std::array<std::uint8_t, 4> held{};
      const auto tgaPixel = [&]() {
        std::array<std::uint8_t, 4> v{0, 0, 0, 255};
        const auto b = byte();
        if (type == 1 || type == 9) {
          const auto idx =
              bits == 16 ? unsigned(b) | (unsigned(byte()) << 8) : unsigned(b);
          if (idx < paletteFirst || idx >= palette.size())
            fail("Invalid TGA palette index");
          return palette[idx];
        }
        if (bits == 8)
          v = {b, b, b, 255};
        else if (bits == 16) {
          const auto word = unsigned(b) | (unsigned(byte()) << 8);
          v = {std::uint8_t((word >> 10) & 31), std::uint8_t((word >> 5) & 31),
               std::uint8_t(word & 31), std::uint8_t(word >> 15)};
        } else if (bits == 24 || bits == 32) {
          const auto g = byte(), red = byte();
          v = {red, g, b, bits == 32 ? byte() : std::uint8_t(255)};
        } else
          fail("Unsupported TGA pixel depth");
        return v;
      };
      for (std::uint64_t y = 0; y < height; ++y) {
        cancel(stop);
        const auto sy = top ? y : height - 1 - y;
        bool wanted = false;
        for (std::uint64_t py = 0; py < oh; ++py)
          if (r.sy(py) == sy) {
            wanted = true;
            break;
          }
        if (!hdr && type < 9 && !wanted) {
          const auto bytes = w * unsigned(bits / 8);
          if (cursor > length || bytes > length - cursor)
            fail("Truncated TGA row");
          cursor += static_cast<std::size_t>(bytes);
          continue;
        }
        if (hdr) {
          std::array<std::uint8_t, 4> first{byte(), byte(), byte(), byte()};
          if (w >= 8 && w <= 32767 && first[0] == 2 && first[1] == 2 &&
              !(first[2] & 128)) {
            if ((unsigned(first[2]) << 8 | first[3]) != w)
              fail("Invalid HDR RLE width");
            for (int c = 0; c < 4; ++c)
              for (std::uint64_t x = 0; x < w;) {
                const auto code = byte();
                const auto n = code > 128 ? code - 128 : code;
                if (!n || unsigned(n) > w - x)
                  fail("Invalid HDR RLE packet");
                const auto v = code > 128 ? byte() : 0;
                for (int i = 0; i < n; ++i)
                  encoded[x++][c] = code > 128 ? std::uint8_t(v) : byte();
              }
          } else {
            encoded[0] = first;
            unsigned shift = 0;
            for (std::uint64_t x = 1; x < w;) {
              const std::array<std::uint8_t, 4> v{byte(), byte(), byte(),
                                                  byte()};
              if (v[0] == 1 && v[1] == 1 && v[2] == 1) {
                if (shift > 24)
                  fail("Invalid HDR legacy run");
                const auto n = std::uint64_t(v[3]) << shift;
                if (!n || n > w - x)
                  fail("Invalid HDR legacy run length");
                const auto previous = encoded[x - 1];
                for (std::uint64_t i = 0; i < n; ++i)
                  encoded[x++] = previous;
                shift += 8;
              } else {
                encoded[x++] = v;
                shift = 0;
              }
            }
          }
        } else {
          for (std::uint64_t x = 0; x < w; ++x) {
            if (!remaining) {
              if (type >= 9) {
                const auto packet = byte();
                repeat = (packet & 128) != 0;
                remaining = (packet & 127) + 1;
              } else {
                repeat = false;
                remaining = 1;
              }
              if (repeat)
                held = tgaPixel();
            }
            encoded[x] = repeat ? held : tgaPixel();
            --remaining;
          }
        }
        if (!wanted)
          continue;
        for (std::uint64_t x = 0; x < w; ++x)
          for (std::uint32_t c = 0; c < count; ++c) {
            const auto &v = encoded[right ? w - 1 - x : x];
            auto *dst =
                row.channels[c].samples.bytes.data() + x * scalar_size(scalar);
            if (hdr) {
              const float f = (1.F / exposure) *
                              std::ldexp(float(v[c]) + 0.5F, int(v[3]) - 136);
              std::memcpy(dst, &f, 4);
            } else if (scalar == ScalarType::Float32) {
              const float f = float(v[c]) / (c == 3 ? 1.F : 31.F);
              std::memcpy(dst, &f, 4);
            } else
              *dst = std::byte(v[c]);
          }
        copyPatch(out, row, 0, sy);
      }
      if (!hdr && remaining)
        fail("TGA RLE packet exceeds the image");
      return out;
    }
    const auto &native = impl_->surfaces.at(index);
    auto format = native.format;
    const bool compressed = DirectX::IsCompressed(format);
    const auto packed = packedUnorm(format);
    auto l = compressed || packed.bytes
                 ? Layout{ScalarType::Float32, 4, false}
                 : layout(format);
    const auto storedCount = l.count;
    if (format == DXGI_FORMAT_BC4_UNORM || format == DXGI_FORMAT_BC4_SNORM)
      l.count = 1;
    if (format == DXGI_FORMAT_BC5_UNORM || format == DXGI_FORMAT_BC5_SNORM)
      l.count = 2;
    if (format == DXGI_FORMAT_BC6H_UF16 || format == DXGI_FORMAT_BC6H_SF16)
      l.count = 3;
    if (l.count == 4 && impl_->desc.alpha == AlphaMode::Absent)
      l.count = 3;
    if (storedCount == 4 && l.count == 3 && DirectX::HasAlpha(format) && !impl_->legacy_expanded) {
      // Keep the existing contradiction check even when the offending alpha
      // lies outside the visible region. Validate with one block at a time.
      const auto opaque = [&](const std::uint8_t *p) {
        if (l.type == ScalarType::UInt8)
          return *p == 255;
        if (l.type == ScalarType::Half || l.type == ScalarType::UInt16) {
          std::uint16_t v;
          std::memcpy(&v, p, 2);
          return v == (l.type == ScalarType::Half ? 0x3c00 : 65535);
        }
        if (l.type == ScalarType::Float32) {
          float v;
          std::memcpy(&v, p, 4);
          return v == 1.F;
        }
        if (l.type == ScalarType::UInt32) {
          std::uint32_t v;
          std::memcpy(&v, p, 4);
          return v == 1;
        }
        return false;
      };
      for (std::size_t y = 0; y < native.height; y += compressed ? 4 : 1) {
        cancel(stop);
        for (std::size_t x = 0; x < native.width; x += compressed ? 4 : 1) {
          if (compressed) {
            const auto bytes = DirectX::BytesPerBlock(format);
            DirectX::Image block{
                4,
                4,
                DirectX::MakeLinear(format),
                bytes,
                bytes,
                native.pixels + (y / 4) * native.rowPitch + (x / 4) * bytes};
            DirectX::ScratchImage decoded;
            hr(DirectX::Decompress(block, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                   decoded));
            const auto *pixels = decoded.GetImage(0, 0, 0);
            for (std::size_t by = 0;
                 by < std::min<std::size_t>(4, native.height - y); ++by)
              for (std::size_t bx = 0;
                   bx < std::min<std::size_t>(4, native.width - x); ++bx)
                if (!opaque(pixels->pixels + by * pixels->rowPitch + bx * 16 +
                            12))
                  fail("DDS opaque alpha declaration conflicts with stored "
                       "values");
          } else if (packed.bytes) {
            const auto word = packed.read(native.pixels + y * native.rowPitch + x * packed.bytes);
            if ((word & packed.masks[3]) != packed.masks[3])
              fail("DDS opaque alpha declaration conflicts with stored values");
          } else if (!opaque(native.pixels + y * native.rowPitch +
                             (x * 4 + 3) * scalar_size(l.type)))
            fail("DDS opaque alpha declaration conflicts with stored values");
        }
      }
    }
    const auto names = l.bgra ? std::array<std::string, 4>{"B", "G", "R", "A"}
                              : std::array<std::string, 4>{"R", "G", "B", "A"};
    for (std::uint32_t c = 0; c < l.count; ++c)
      add(descriptor, names[c], l.type,
          l.normalized ? SampleEncoding::UNorm : SampleEncoding::Numeric);
    auto out = preview_output(descriptor, r);
    const auto component = scalar_size(l.type);
    for (std::uint64_t y = 0; y < oh; ++y) {
      cancel(stop);
      std::uint64_t previousBlock = UINT64_MAX;
      DirectX::ScratchImage decoded;
      const DirectX::Image *patch = nullptr;
      std::vector<std::uint8_t> legacyRow;
      if (impl_->legacy_expanded) {
        // Feed a single cropped raw row to the existing legacy DDS expander.
        // This retains palette/A8P8/A4L4/RGB24 rules without loading all mips.
        budget(r.width * std::max<std::uint64_t>(impl_->legacy_bits / 8,
                                                 storedCount * component) <=
               64ULL * 1024 * 1024);
        const auto prefix = 128 + impl_->legacy_palette;
        const auto bytes = std::size_t(r.width) * (impl_->legacy_bits / 8);
        legacyRow.resize(prefix + bytes);
        std::memcpy(legacyRow.data(), impl_->bytes.data(), prefix);
        const auto field = [&](std::size_t at, std::uint32_t v) {
          std::memcpy(legacyRow.data() + at, &v, 4);
        };
        field(8, 0x100f);
        field(12, 1);
        field(16, static_cast<std::uint32_t>(r.width));
        field(20, static_cast<std::uint32_t>(bytes));
        field(24, 0);
        field(28, 1);
        field(108, 0x1000);
        field(112, 0);
        std::memcpy(legacyRow.data() + prefix,
                    native.pixels + r.sy(y) * native.rowPitch +
                        r.x * (impl_->legacy_bits / 8),
                    bytes);
        hr(DirectX::LoadFromDDSMemory(legacyRow.data(), legacyRow.size(),
                                      DirectX::DDS_FLAGS_NONE, nullptr,
                                      decoded));
        patch = decoded.GetImage(0, 0, 0);
        if (!patch || patch->format != format || patch->height != 1 ||
            patch->width != r.width)
          fail("DDS legacy row expansion mismatch");
      }
      for (std::uint64_t x = 0; x < ow; ++x) {
        const auto sx = r.sx(x), sy = r.sy(y);
        const std::uint8_t *pixel{};
        std::array<float, 4> unpacked;
        if (packed.bytes) {
          const auto *src = impl_->legacy_expanded
              ? patch->pixels + (sx - r.x) * packed.bytes
              : native.pixels + sy * native.rowPitch + sx * packed.bytes;
          const auto word = packed.read(src);
          for (unsigned c = 0; c < l.count; ++c)
            unpacked[c] = packed.sample(word, c);
          pixel = reinterpret_cast<const std::uint8_t *>(unpacked.data());
        } else if (impl_->legacy_expanded) {
          pixel = patch->pixels + (sx - r.x) * storedCount * component;
        } else if (compressed) {
          const auto block = sx / 4;
          if (block != previousBlock) {
            previousBlock = block;
            const auto blockBytes = DirectX::BytesPerBlock(format);
            DirectX::Image encoded{4,
                                   4,
                                   DirectX::MakeLinear(format),
                                   blockBytes,
                                   blockBytes,
                                   native.pixels + (sy / 4) * native.rowPitch +
                                       block * blockBytes};
            hr(DirectX::Decompress(encoded, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                   decoded));
            patch = decoded.GetImage(0, 0, 0);
          }
          pixel = patch->pixels + (sy % 4) * patch->rowPitch + (sx % 4) * 16;
        } else
          pixel = native.pixels + sy * native.rowPitch +
                  sx * storedCount * component;
        for (std::uint32_t c = 0; c < l.count; ++c)
          std::memcpy(out.channels[c].samples.bytes.data() +
                          (y * ow + x) * component,
                      pixel + c * component, component);
      }
    }
    return out;
  });
}

double value(const ImageChannel &c, std::size_t pixel) {
  const auto bytes = scalar_size(c.samples.scalar);
  if (pixel >= c.samples.element_count ||
      c.samples.bytes.size() / bytes <= pixel)
    throw skeletal::Failure(ErrorCode::InvalidArgument, "Pixel out of range");
  const auto *p = c.samples.bytes.data() + pixel * bytes;
  switch (c.samples.scalar) {
  case ScalarType::UInt8:
    return std::to_integer<unsigned char>(*p);
  case ScalarType::UInt16: {
    std::uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
  }
  case ScalarType::UInt32: {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
  }
  case ScalarType::Half: {
    IMATH_NAMESPACE::half v;
    std::memcpy(&v, p, 2);
    return static_cast<float>(v);
  }
  case ScalarType::Float32: {
    float v;
    std::memcpy(&v, p, 4);
    return v;
  }
  case ScalarType::Int16: {
    std::int16_t v;
    std::memcpy(&v, p, 2);
    return v;
  }
  case ScalarType::Int32: {
    std::int32_t v;
    std::memcpy(&v, p, 4);
    return v;
  }
  case ScalarType::Float64: {
    double v;
    std::memcpy(&v, p, 8);
    return v;
  }
  default:
    fail("Unsupported pixel scalar");
  }
}
double interpreted_value(const ImageChannel &c, std::size_t pixel) {
  const auto v = value(c, pixel);
  switch (c.encoding) {
  case SampleEncoding::Numeric:
    return v;
  case SampleEncoding::UNorm:
    if (c.samples.scalar == ScalarType::UInt8)
      return v / 255.;
    if (c.samples.scalar == ScalarType::UInt16)
      return v / 65535.;
    if (c.samples.scalar == ScalarType::UInt32)
      return v / 4294967295.;
    break;
  case SampleEncoding::SNorm:
    if (c.samples.scalar == ScalarType::Int16)
      return std::max(-1., v / 32767.);
    if (c.samples.scalar == ScalarType::Int32)
      return std::max(-1., v / 2147483647.);
    break;
  }
  throw skeletal::Failure(ErrorCode::InvalidArgument,
                          "Sample encoding does not match scalar type");
}
std::vector<float> display_rgba(const ImageSubresource &source,
                                const DisplayOptions &options) {
  if (source.data_window.height &&
      source.data_window.width >
          SIZE_MAX / (4 * sizeof(float)) / source.data_window.height)
    throw skeletal::Failure(ErrorCode::ResourceLimit, "Display size overflow");
  const auto pixels = source.data_window.width * source.data_window.height;
  std::array<const ImageChannel *, 4> channels{};
  for (std::size_t i = 0; i < 4; ++i)
    for (const auto &c : source.channels)
      if (c.name == options.channels[i])
        channels[i] = &c;
  for (std::size_t i = 0; i < 3; ++i)
    if (!channels[i])
      throw skeletal::Failure(ErrorCode::InvalidArgument,
                              "Selected display channel missing");
  if (options.composite && options.alpha == AlphaMode::Unknown)
    throw skeletal::Failure(ErrorCode::InvalidArgument,
                            "Explicit alpha interpretation required");
  std::vector<float> out(static_cast<std::size_t>(pixels) * 4);
  for (std::size_t i = 0; i < pixels; ++i) {
    std::array<double, 4> v{0, 0, 0, 1};
    for (std::size_t c = 0; c < 4; ++c)
      if (channels[c])
        v[c] = interpreted_value(*channels[c], i);
    if (!std::all_of(v.begin(), v.end(),
                     [](double x) { return std::isfinite(x); }))
      v = {1, 0, 1, 1};
    else
      for (std::size_t c = 0; c < 3; ++c) {
        if (options.alpha == AlphaMode::Premultiplied && v[3] != 0)
          v[c] /= v[3];
        if (options.meaning == ImageMeaning::Colour) {
          v[c] *= std::exp2(options.exposure);
        }
        if (options.composite && options.alpha != AlphaMode::Absent)
          v[c] = v[c] * std::clamp(v[3], 0., 1.) +
                 0.18 * (1 - std::clamp(v[3], 0., 1.));
        if (options.meaning == ImageMeaning::Colour && options.apply_srgb)
          v[c] = v[c] <= 0.0031308 ? 12.92 * v[c]
                                   : 1.055 * std::pow(v[c], 1 / 2.4) - 0.055;
      }
    if (options.composite)
      v[3] = 1;
    for (std::size_t c = 0; c < 4; ++c)
      out[i * 4 + c] = static_cast<float>(v[c]);
  }
  return out;
}
} // namespace formaview::imaging
