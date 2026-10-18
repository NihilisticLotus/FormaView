#include "formaview/pro_image_write.hpp"
#include <DirectXTex.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfIO.h>
#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfTiledOutputFile.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace formaview::imaging {
namespace {
namespace E = OPENEXR_IMF_NAMESPACE;
[[noreturn]] void invalid(const char *s) {
  throw skeletal::Failure(ErrorCode::InvalidArgument, s);
}
void cancel(std::stop_token stop) {
  if (stop.stop_requested())
    throw skeletal::Failure(ErrorCode::Cancelled,
                            "Professional encoding cancelled");
}
void budget(bool ok) {
  if (!ok)
    throw skeletal::Failure(ErrorCode::ResourceLimit,
                            "Professional encoding budget exceeded");
}
void hr(HRESULT r) {
  if (FAILED(r))
    throw skeletal::Failure(ErrorCode::Unsupported,
                            "DirectXTex encode failure: " +
                                std::to_string(static_cast<unsigned long>(r)));
}
class Output final : public E::OStream {
public:
  Output(std::uint64_t max, std::stop_token stop)
      : E::OStream("memory"), max_(max), stop_(stop) {}
  void write(const char *bytes, int n) override {
    cancel(stop_);
    budget(n >= 0 && pos_ <= max_ &&
           static_cast<std::uint64_t>(n) <= max_ - pos_ &&
           pos_ + n <= SIZE_MAX);
    if (pos_ + n > data.size())
      data.resize(static_cast<std::size_t>(pos_ + n));
    std::memcpy(data.data() + pos_, bytes, static_cast<std::size_t>(n));
    pos_ += n;
  }
  std::uint64_t tellp() override { return pos_; }
  void seekp(std::uint64_t n) override {
    budget(n <= max_);
    pos_ = n;
  }
  std::vector<std::byte> data;

private:
  std::uint64_t pos_{}, max_;
  std::stop_token stop_;
};
IMATH_NAMESPACE::Box2i box(ImageWindow w) {
  if (!w.width || !w.height || w.width > INT_MAX || w.height > INT_MAX ||
      w.x < INT_MIN || w.y < INT_MIN ||
      w.x > INT_MAX - static_cast<std::int64_t>(w.width) + 1 ||
      w.y > INT_MAX - static_cast<std::int64_t>(w.height) + 1)
    invalid("Invalid EXR window");
  return {{static_cast<int>(w.x), static_cast<int>(w.y)},
          {static_cast<int>(w.x + static_cast<std::int64_t>(w.width) - 1),
           static_cast<int>(w.y + static_cast<std::int64_t>(w.height) - 1)}};
}
DXGI_FORMAT native_format(ScalarType type, SampleEncoding encoding,
                          std::size_t count) {
  if (type == ScalarType::UInt8 && encoding == SampleEncoding::UNorm) {
    if (count == 1)
      return DXGI_FORMAT_R8_UNORM;
    if (count == 2)
      return DXGI_FORMAT_R8G8_UNORM;
    if (count == 4)
      return DXGI_FORMAT_R8G8B8A8_UNORM;
  }
  if (type == ScalarType::UInt16 && encoding == SampleEncoding::UNorm) {
    if (count == 1)
      return DXGI_FORMAT_R16_UNORM;
    if (count == 2)
      return DXGI_FORMAT_R16G16_UNORM;
    if (count == 4)
      return DXGI_FORMAT_R16G16B16A16_UNORM;
  }
  if (type == ScalarType::Half && encoding == SampleEncoding::Numeric) {
    if (count == 1)
      return DXGI_FORMAT_R16_FLOAT;
    if (count == 2)
      return DXGI_FORMAT_R16G16_FLOAT;
    if (count == 4)
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
  }
  if (type == ScalarType::Float32 && encoding == SampleEncoding::Numeric) {
    if (count == 1)
      return DXGI_FORMAT_R32_FLOAT;
    if (count == 2)
      return DXGI_FORMAT_R32G32_FLOAT;
    if (count == 3)
      return DXGI_FORMAT_R32G32B32_FLOAT;
    if (count == 4)
      return DXGI_FORMAT_R32G32B32A32_FLOAT;
  }
  if (type == ScalarType::UInt32 && encoding == SampleEncoding::Numeric) {
    if (count == 1)
      return DXGI_FORMAT_R32_UINT;
    if (count == 2)
      return DXGI_FORMAT_R32G32_UINT;
    if (count == 4)
      return DXGI_FORMAT_R32G32B32A32_UINT;
  }
  if (type == ScalarType::UInt16 && encoding == SampleEncoding::Numeric &&
      count == 1)
    return DXGI_FORMAT_R16_UINT;
  if (type == ScalarType::UInt8 && encoding == SampleEncoding::Numeric &&
      count == 1)
    return DXGI_FORMAT_R8_UINT;
  invalid("Unsupported native DDS channel layout; transform explicitly");
}
} // namespace
std::vector<std::byte> encode_professional(const ImageSubresource &s,
                                           const ProfessionalEncodeOptions &o,
                                           Limits limits,
                                           std::stop_token stop) {
  cancel(stop);
  const auto w = s.data_window.width, h = s.data_window.height;
  budget(w && h && w <= limits.dimension && h <= limits.dimension &&
         w <= SIZE_MAX / h && w * h <= limits.decoded_bytes / 64 &&
         !s.channels.empty() && s.channels.size() <= limits.channels);
  const auto pixels = static_cast<std::size_t>(w * h);
  std::set<std::string> names;
  for (const auto &c : s.channels) {
    if (c.name.empty() || !names.insert(c.name).second ||
        c.samples.components != 1 || c.samples.element_count != pixels)
      invalid("Invalid output channel");
    (void)value(c, pixels - 1);
  }
  if (o.target == ProfessionalTarget::Exr) {
    const auto data_box = box(s.data_window),
               display_box = box(s.display_window);
    E::Header header(display_box, data_box);
    header.compression() = E::ZIP_COMPRESSION;
    if (!s.part_name.empty())
      header.setName(s.part_name);
    if (o.exr_tiled)
      header.setTileDescription(E::TileDescription(64, 64, E::ONE_LEVEL));
    E::FrameBuffer frame;
    for (const auto &c : s.channels) {
      if (c.encoding != SampleEncoding::Numeric)
        invalid(
            "EXR numeric output requires explicit normalization conversion");
      const auto type = c.samples.scalar == ScalarType::Half      ? E::HALF
                        : c.samples.scalar == ScalarType::Float32 ? E::FLOAT
                        : c.samples.scalar == ScalarType::UInt32
                            ? E::UINT
                            : E::NUM_PIXELTYPES;
      if (type == E::NUM_PIXELTYPES)
        invalid("EXR requires half, float or uint channels");
      header.channels().insert(c.name, E::Channel(type));
      frame.insert(c.name,
                   E::Slice::Make(type, c.samples.bytes.data(), data_box));
    }
    Output output(limits.input_bytes, stop);
    if (o.exr_tiled) {
      E::TiledOutputFile file(output, header, 1);
      file.setFrameBuffer(frame);
      for (int y = 0; y < file.numYTiles(); ++y) {
        cancel(stop);
        file.writeTiles(0, file.numXTiles() - 1, y, y);
      }
    } else {
      E::OutputFile file(output, header, 1);
      file.setFrameBuffer(frame);
      for (std::size_t y = 0; y < h;) {
        cancel(stop);
        const auto count = std::min<std::size_t>(64, h - y);
        file.writePixels(static_cast<int>(count));
        y += count;
      }
    }
    cancel(stop);
    return std::move(output.data);
  }
  if (s.data_window.x || s.data_window.y || s.display_window.x ||
      s.display_window.y || s.display_window.width != w ||
      s.display_window.height != h)
    invalid("Non-EXR output needs an explicit window flattening policy");
  const bool hdr = o.target == ProfessionalTarget::Hdr,
             tga = o.target == ProfessionalTarget::Tga;
  if (s.channels.size() > 4)
    invalid("Non-EXR output requires at most four explicit channels");
  const std::array<std::string, 4> ordered{"R", "G", "B", "A"};
  for (std::size_t i = 0; i < s.channels.size(); ++i)
    if (s.channels[i].name != ordered[i] &&
        !(s.channels.size() == 1 && s.channels[i].name == "Y"))
      invalid("Non-EXR output requires ordered R/G/B/A or Y");
  if (hdr && (s.channels.size() != 3 || !o.allow_lossy))
    invalid("HDR requires RGB and explicit RGBE quantization consent");
  if (tga && s.channels.size() != 1 && s.channels.size() != 3 &&
      s.channels.size() != 4)
    invalid("TGA requires Y/RGB/RGBA");
  const auto &first = s.channels.front();
  for (const auto &c : s.channels)
    if (c.samples.scalar != first.samples.scalar ||
        c.encoding != first.encoding)
      invalid("Non-EXR channels must have a common scalar/encoding");
  if (tga && (first.samples.scalar != ScalarType::UInt8 ||
              first.encoding != SampleEncoding::UNorm))
    invalid("TGA requires explicit UInt8 quantization");
  const bool pad_alpha =
      (s.channels.size() == 3 && first.samples.scalar != ScalarType::Float32) ||
      hdr;
  const auto count = pad_alpha ? 4U : s.channels.size();
  auto format =
      hdr ? DXGI_FORMAT_R32G32B32A32_FLOAT
          : native_format(first.samples.scalar, first.encoding, count);
  const bool tga_rgb = tga && s.channels.size() == 3;
  if (tga_rgb)
    format = DXGI_FORMAT_B8G8R8X8_UNORM;
  DirectX::ScratchImage image;
  hr(image.Initialize2D(format, static_cast<std::size_t>(w),
                        static_cast<std::size_t>(h), 1, 1));
  const auto component = hdr                                         ? 4U
                         : first.samples.scalar == ScalarType::UInt8 ? 1U
                         : first.samples.scalar == ScalarType::UInt16 ||
                                 first.samples.scalar == ScalarType::Half
                             ? 2U
                             : 4U;
  auto *dest = image.GetImage(0, 0, 0);
  for (std::size_t y = 0; y < h; ++y) {
    cancel(stop);
    for (std::size_t x = 0; x < w; ++x)
      for (std::size_t c = 0; c < count; ++c) {
        auto *out =
            dest->pixels + y * dest->rowPitch + (x * count + c) * component;
        if (hdr) {
          const auto v =
              c == 3 ? 1. : interpreted_value(s.channels[c], y * w + x);
          if (!std::isfinite(v) || v < 0 || v > 1e30)
            invalid(
                "RGBE output requires finite nonnegative representable values");
          const auto f = static_cast<float>(v);
          std::memcpy(out, &f, 4);
        } else if (c < s.channels.size()) {
          const auto channel = tga_rgb ? 2 - c : c;
          std::memcpy(out,
                      s.channels[channel].samples.bytes.data() +
                          (y * w + x) * component,
                      component);
        } else if (first.samples.scalar == ScalarType::Half) {
          const std::uint16_t one = 0x3c00;
          std::memcpy(out, &one, 2);
        } else if (first.encoding == SampleEncoding::Numeric) {
          const std::uint32_t one = 1;
          std::memcpy(out, &one, component);
        } else
          std::memset(out, 255, component);
      }
  }
  auto metadata = image.GetMetadata();
  metadata.SetAlphaMode(
      s.channels.size() < 4            ? DirectX::TEX_ALPHA_MODE_OPAQUE
      : o.alpha == AlphaMode::Straight ? DirectX::TEX_ALPHA_MODE_STRAIGHT
      : o.alpha == AlphaMode::Premultiplied
          ? DirectX::TEX_ALPHA_MODE_PREMULTIPLIED
      : o.alpha == AlphaMode::Absent ? DirectX::TEX_ALPHA_MODE_OPAQUE
                                     : DirectX::TEX_ALPHA_MODE_UNKNOWN);
  DirectX::Blob blob;
  if (hdr)
    hr(DirectX::SaveToHDRMemory(*dest, blob));
  else if (tga)
    hr(DirectX::SaveToTGAMemory(*dest, DirectX::TGA_FLAGS_NONE, blob,
                                &metadata));
  else {
    const auto target =
        o.dds_format ? static_cast<DXGI_FORMAT>(o.dds_format) : format;
    DirectX::ScratchImage compressed;
    if (DirectX::IsCompressed(target)) {
      if (!o.allow_lossy)
        invalid("BC encoding requires explicit lossy consent");
      // sRGB is an encoding tag here, not an implicit colour transform.
      const auto linear_target = DirectX::MakeLinear(target);
      const auto channels = linear_target == DXGI_FORMAT_BC4_UNORM ||
                                    linear_target == DXGI_FORMAT_BC4_SNORM
                                ? 1U
                            : linear_target == DXGI_FORMAT_BC5_UNORM ||
                                    linear_target == DXGI_FORMAT_BC5_SNORM
                                ? 2U
                            : linear_target == DXGI_FORMAT_BC6H_UF16 ||
                                    linear_target == DXGI_FORMAT_BC6H_SF16
                                ? 3U
                                : 4U;
      if (s.channels.size() != channels &&
          !(channels == 4 && s.channels.size() == 3))
        invalid("BC channel loss requires explicit channel selection before "
                "encoding");
      const bool signed_bc = linear_target == DXGI_FORMAT_BC4_SNORM ||
                             linear_target == DXGI_FORMAT_BC5_SNORM ||
                             linear_target == DXGI_FORMAT_BC6H_SF16;
      const bool float_bc = linear_target == DXGI_FORMAT_BC6H_UF16 ||
                            linear_target == DXGI_FORMAT_BC6H_SF16;
      const double maximum = float_bc ? 65504. : 1.;
      for (const auto &c : s.channels)
        for (std::size_t i = 0; i < pixels; ++i) {
          if ((i & 4095) == 0)
            cancel(stop);
          const auto v = interpreted_value(c, i);
          if (!std::isfinite(v) || v < (signed_bc ? -maximum : 0.) ||
              v > maximum)
            invalid("BC clipping/non-finite replacement must be explicit "
                    "before encoding");
        }
      const DirectX::CompressOptions compression{DirectX::TEX_COMPRESS_DEFAULT,
                                                 .5F, 1.F};
      const auto encoded = DirectX::CompressEx(
          *dest, linear_target, compression, compressed,
          [stop](std::size_t, std::size_t) { return !stop.stop_requested(); });
      cancel(stop);
      hr(encoded);
      dest = compressed.GetImage(0, 0, 0);
      metadata.format = target;
      auto tagged = *dest;
      tagged.format = target;
      hr(DirectX::SaveToDDSMemory(
          &tagged, 1, metadata, DirectX::DDS_FLAGS_FORCE_DX10_EXT_MISC2, blob));
    } else {
      if (DirectX::MakeLinear(target) != format)
        invalid("DDS target needs an explicit scalar/channel transform");
      metadata.format = target;
      auto tagged = *dest;
      tagged.format = target;
      hr(DirectX::SaveToDDSMemory(
          &tagged, 1, metadata, DirectX::DDS_FLAGS_FORCE_DX10_EXT_MISC2, blob));
    }
  }
  budget(blob.GetBufferSize() <= limits.input_bytes);
  cancel(stop);
  std::vector<std::byte> out(blob.GetBufferSize());
  std::memcpy(out.data(), blob.GetBufferPointer(), out.size());
  return out;
}
} // namespace formaview::imaging
