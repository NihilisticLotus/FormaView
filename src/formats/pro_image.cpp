#include "formaview/pro_image.hpp"
#include <DirectXTex.h>
#include <Imath/half.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfIO.h>
#include <OpenEXR/ImfInputPart.h>
#include <OpenEXR/ImfMultiPartInputFile.h>
#include <OpenEXR/ImfPartType.h>
#include <OpenEXR/ImfStandardAttributes.h>
#include <OpenEXR/ImfTiledInputPart.h>
#include <algorithm>
#include <cmath>
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
  std::vector<std::byte> bytes;
  ImageData desc;
  bool is_exr{}, hdr{}, tga{};
  std::uint32_t tga_channels{};
  DirectX::ScratchImage dds;
  std::vector<std::array<std::size_t, 3>> indices;
};
ImageFile::ImageFile(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
ImageFile::ImageFile(ImageFile &&) noexcept = default;
ImageFile &ImageFile::operator=(ImageFile &&) noexcept = default;
ImageFile::~ImageFile() = default;
const ImageData &ImageFile::description() const { return impl_->desc; }
std::size_t ImageFile::resident_bytes() const {
  return impl_->bytes.size() + impl_->dds.GetPixelsSize();
}
ImageFile ImageFile::open(const std::filesystem::path &path, Limits limits,
                          std::stop_token stop) {
  cancel(stop);
  auto p = std::make_unique<Impl>();
  p->limits = limits;
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
    p->tga = extension == L".tga";
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
      p->tga_channels = bits == 8                         ? 1U
                        : (bits == 24 || attributes == 0) ? 3U
                                                          : 4U;
      p->desc.source.properties["tga_pixel_bits"] = std::to_string(bits);
      p->desc.source.properties["tga_descriptor"] =
          std::to_string(std::to_integer<unsigned>(p->bytes.at(17)));
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
             slice <= limits.decoded_bytes / planes);
      resident += slice * planes;
      count += planes;
      budget(resident <= limits.decoded_bytes && count <= limits.subresources);
    }
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
    p->desc.colour_space =
        DirectX::IsSRGB(meta.format) ? "sRGB" : "unspecified";
    p->desc.alpha =
        p->hdr || (p->tga && p->tga_channels != 4) ? AlphaMode::Absent
        : meta.GetAlphaMode() == DirectX::TEX_ALPHA_MODE_PREMULTIPLIED
            ? AlphaMode::Premultiplied
        : meta.GetAlphaMode() == DirectX::TEX_ALPHA_MODE_STRAIGHT
            ? AlphaMode::Straight
        : meta.GetAlphaMode() == DirectX::TEX_ALPHA_MODE_OPAQUE && !p->tga
            ? AlphaMode::Absent
            : AlphaMode::Unknown;
    for (std::size_t a = 0; a < meta.arraySize; ++a)
      for (std::size_t m = 0; m < meta.mipLevels; ++m)
        for (std::size_t z = 0; z < std::max<std::size_t>(1, meta.depth >> m);
             ++z) {
          const auto *image = p->dds.GetImage(m, a, z);
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
          s.block_encoding = "DXGI:" + std::to_string(meta.format);
          p->desc.subresources.push_back(std::move(s));
          p->indices.push_back({m, a, z});
        }
  }
  cancel(stop);
  return ImageFile(std::move(p));
}
ImageSubresource ImageFile::decode(std::size_t index,
                                   std::stop_token stop) const {
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
    } else if (format == DXGI_FORMAT_R10G10B10A2_UNORM ||
               (impl_->tga && format == DXGI_FORMAT_B5G5R5A1_UNORM)) {
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
  const auto pixels = source.data_window.width * source.data_window.height;
  if (pixels > SIZE_MAX / (4 * sizeof(float)))
    throw skeletal::Failure(ErrorCode::ResourceLimit, "Display size overflow");
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
