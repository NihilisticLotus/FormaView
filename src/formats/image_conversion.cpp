#include "formaview/image_conversion.hpp"
#include <QByteArray>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <fstream>

namespace formaview::imaging {
namespace {
[[noreturn]] void invalid(const char *message) {
  throw skeletal::Failure(ErrorCode::InvalidArgument, message);
}
bool has_alpha(const ImageSubresource &s) {
  return std::any_of(s.channels.begin(), s.channels.end(),
                     [](const auto &c) { return c.name == "A"; });
}
std::vector<std::byte> profile(const ImageData &data, std::size_t entry) {
  const auto &props = data.source.properties;
  for (const auto &key :
       {"entry." + std::to_string(entry) + ".icc_base64",
        "page." + std::to_string(entry) + ".icc_hex", std::string("icc_hex")}) {
    const auto it = props.find(key);
    if (it == props.end())
      continue;
    const auto encoded = QByteArray::fromStdString(it->second);
    const auto raw = key.ends_with("base64") ? QByteArray::fromBase64(encoded)
                                             : QByteArray::fromHex(encoded);
    std::vector<std::byte> out(static_cast<std::size_t>(raw.size()));
    std::memcpy(out.data(), raw.constData(), out.size());
    return out;
  }
  return {};
}
} // namespace
ImageDocument::ImageDocument(
    std::variant<ImageFile, RasterFile, ExtendedRasterFile> file)
    : file_(std::move(file)) {}
ImageDocument ImageDocument::open(const std::filesystem::path &path,
                                  Limits limits, std::stop_token stop) {
  if (stop.stop_requested())
    throw skeletal::Failure(ErrorCode::Cancelled, "Image open cancelled");
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw skeletal::Failure(ErrorCode::Io, "Cannot open image");
  std::array<char, 12> magic{};
  in.read(magic.data(), magic.size());
  const auto n = in.gcount();
  const auto is = [&](const char *bytes, std::size_t size) {
    return n >= static_cast<std::streamsize>(size) &&
           std::memcmp(magic.data(), bytes, size) == 0;
  };
  auto extension = path.extension().wstring();
  std::transform(
      extension.begin(), extension.end(), extension.begin(),
      [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  if (is("II\x2a\0", 4) || is("MM\0\x2a", 4) || is("II\x2b\0", 4) ||
      is("MM\0\x2b", 4) ||
      (is("RIFF", 4) && n == 12 &&
       std::memcmp(magic.data() + 8, "WEBP", 4) == 0))
    return ImageDocument(ExtendedRasterFile::open(path, limits, stop));
  if (is("\x76\x2f\x31\x01", 4) || is("DDS ", 4) || is("#?RADIANCE", 10) ||
      is("#?RGBE", 6) ||
      (extension.size() == 4 && extension[0] == L'.' && extension[1] == L't' &&
       extension[2] == L'g' && extension[3] == L'a'))
    return ImageDocument(ImageFile::open(path, limits, stop));
  return ImageDocument(RasterFile::open(path, limits, stop));
}
const ImageData &ImageDocument::description() const {
  return std::visit(
      [](const auto &file) -> const ImageData & { return file.description(); },
      file_);
}
ImageSemantics ImageDocument::semantics(std::size_t entry) const {
  const auto &d = description();
  if (entry >= d.subresources.size())
    invalid("Image semantics subresource out of range");
  ImageSemantics result{d.alpha, d.meaning, d.colour_space, {}};
  const auto &props = d.source.properties;
  for (const auto &prefix : {"entry." + std::to_string(entry) + ".",
                             "page." + std::to_string(entry) + "."}) {
    if (const auto it = props.find(prefix + "alpha"); it != props.end()) {
      const auto &value = it->second;
      if (value == "straight" || value == "2")
        result.alpha = AlphaMode::Straight;
      else if (value == "premultiplied" || value == "3")
        result.alpha = AlphaMode::Premultiplied;
      else if (value == "absent" || value == "1")
        result.alpha = AlphaMode::Absent;
      else
        result.alpha = AlphaMode::Unknown;
    }
    if (const auto it = props.find(prefix + "colour_space"); it != props.end())
      result.colour_space = it->second;
  }
  const auto icc = profile(d, entry);
  if (!icc.empty())
    result.icc_profile_sha256 = skeletal::sha256(icc);
  return result;
}
ImageSubresource ImageDocument::decode(std::size_t entry,
                                       std::stop_token stop) const {
  return std::visit([&](const auto &file) { return file.decode(entry, stop); },
                    file_);
}
std::size_t ImageDocument::resident_bytes() const {
  return std::visit([](const auto &file) { return file.resident_bytes(); },
                    file_);
}
std::string_view image_target_name(ImageTarget target) {
  switch (target) {
  case ImageTarget::Png:
    return "png";
  case ImageTarget::Jpeg:
    return "jpeg";
  case ImageTarget::Bmp:
    return "bmp";
  case ImageTarget::Tga:
    return "tga";
  case ImageTarget::Tiff:
    return "tiff";
  case ImageTarget::Webp:
    return "webp";
  case ImageTarget::Dds:
    return "dds";
  case ImageTarget::Hdr:
    return "hdr";
  case ImageTarget::Exr:
    return "exr";
  }
  invalid("Unknown image target");
}
ImageConversionProduct
prepare_image_conversion(const ImageDocument &document,
                         const ImageConversionOptions &options, Limits limits,
                         std::stop_token stop) {
  (void)image_target_name(options.target);
  const auto &description = document.description();
  auto source = document.decode(options.subresource, stop);
  auto transform_options = options.transform;
  // Standard A is an actual channel. Layered EXR alpha is explicitly selected
  // by name in the same transform options; never guessed from a suffix.
  if (has_alpha(source) && transform_options.alpha_channel.empty())
    invalid("Source A requires an explicit alpha policy/channel");
  if (!transform_options.alpha_channel.empty() &&
      transform_options.source_alpha == AlphaMode::Unknown)
    invalid("Source alpha interpretation must be explicit");
  auto transformed = transform(source, transform_options, limits, stop);
  ImageConversionProduct result;
  result.input_sha256 = description.source.identity.sha256;
  result.subresource = options.subresource;
  result.transform = std::move(transformed.report);
  const auto loss = [&](std::string code, std::string detail) {
    result.losses.push_back({std::move(code), std::move(detail)});
  };
  if (description.subresources.size() > 1)
    loss("selected_subresource", "Only the explicitly selected "
                                 "entry/part/mip/face/layer/slice is written");
  for (const auto &name : result.transform.omitted_channels)
    loss("omitted_channel", "Source channel omitted: " + name);
  if (result.transform.alpha_removed)
    loss("alpha_removed", "Source alpha removed/composited by explicit policy");
  if (result.transform.clipped)
    loss("clipped_samples", std::to_string(result.transform.clipped));
  if (result.transform.precision_changed)
    loss("precision_changed",
         std::to_string(result.transform.precision_changed));
  if (result.transform.non_finite_replaced)
    loss("non_finite_replaced",
         std::to_string(result.transform.non_finite_replaced));
  if (result.transform.colour_transformed)
    loss("colour_transform",
         "Explicit exposure/transfer/compositing applied to colour samples");
  auto &image = transformed.image;
  // Channel names carry meaning; EXR enumeration is often alphabetical (ABGR).
  // Put conventional named channels into encoder order without dropping or
  // inventing channels. Unsupported channel sets still fail the encoder checks.
  if (options.target != ImageTarget::Exr) {
    const auto rank = [](const std::string &name) {
      const std::array<std::string_view, 5> order{"Y", "R", "G", "B", "A"};
      return std::find(order.begin(), order.end(), name) - order.begin();
    };
    if (std::all_of(
            image.channels.begin(), image.channels.end(),
            [&](const auto &channel) { return rank(channel.name) < 5; }))
      std::stable_sort(image.channels.begin(), image.channels.end(),
                       [&](const auto &left, const auto &right) {
                         return rank(left.name) < rank(right.name);
                       });
  }
  const auto &d = image.data_window;
  const auto &v = image.display_window;
  if (options.window == WindowPolicy::SelectedDataWindow) {
    if (d.x || d.y || v.x || v.y || v.width != d.width || v.height != d.height)
      loss("window_metadata", "Data-window pixels retained; original "
                              "placement/display window discarded");
    image.data_window = image.display_window = {0, 0, d.width, d.height};
  }
  auto icc = profile(description, options.subresource);
  if (!icc.empty()) {
    const bool supported = options.target == ImageTarget::Png ||
                           options.target == ImageTarget::Jpeg ||
                           options.target == ImageTarget::Tiff ||
                           options.target == ImageTarget::Webp;
    if (options.metadata == MetadataPolicy::Drop || !supported) {
      loss("icc_profile", "Source ICC profile is not copied");
      icc.clear();
    } else if (result.transform.colour_transformed)
      invalid("A transformed colour encoding needs an explicit new profile or "
              "metadata-drop policy");
  }
  // Source metadata is described separately from pixels. Report keys which
  // this encoder does not retain rather than calling all metadata preserved.
  for (const auto &[key, value] : description.source.properties) {
    if (key.find("chromaticities") != std::string::npos ||
        key.find("white_luminance") != std::string::npos ||
        (key.ends_with("orientation") && value != "1" &&
         !value.starts_with("stored pixel order")))
      loss("metadata_field", "Not copied: " + key + "=" + value);
  }
  const auto alpha =
      has_alpha(image) ? transform_options.source_alpha : AlphaMode::Absent;
  if (description.source.identity.adapter == "OpenEXR" &&
      !image.part_name.empty() && options.target != ImageTarget::Exr)
    loss("part_name", "Container part/entry name is not retained");
  if (options.target == ImageTarget::Dds && !options.dds_format &&
      image.channels.size() == 3 &&
      image.channels[0].samples.scalar != ScalarType::Float32)
    loss("padding_channel", "DXGI storage adds an opaque padding component; it "
                            "is not a source alpha channel");
  if (options.target == ImageTarget::Png ||
      options.target == ImageTarget::Jpeg ||
      options.target == ImageTarget::Bmp) {
    if (options.target == ImageTarget::Jpeg) {
      if (!options.allow_lossy)
        invalid("JPEG requires explicit lossy consent");
      loss("lossy_codec", "JPEG recompression");
    }
    RasterEncodeOptions o;
    o.target = options.target == ImageTarget::Png    ? RasterTarget::Png
               : options.target == ImageTarget::Jpeg ? RasterTarget::Jpeg
                                                     : RasterTarget::Bmp;
    o.quality = options.quality;
    o.compression = options.png_compression;
    o.alpha = alpha;
    o.icc_profile = std::move(icc);
    result.encoded = encode_raster(image, o, limits, stop);
  } else if (options.target == ImageTarget::Tiff ||
             options.target == ImageTarget::Webp) {
    ExtendedEncodeOptions o;
    o.target = options.target == ImageTarget::Tiff ? ExtendedRasterTarget::Tiff
                                                   : ExtendedRasterTarget::Webp;
    o.compression = options.tiff_compression;
    o.alpha = alpha;
    o.webp_lossless = options.webp_lossless;
    o.quality = static_cast<float>(options.quality);
    o.allow_lossy = options.allow_lossy;
    o.icc_profile = std::move(icc);
    if (o.target == ExtendedRasterTarget::Webp && !o.webp_lossless)
      loss("lossy_codec", "WebP recompression");
    result.encoded = encode_extended(image, o, limits, stop);
  } else {
    ProfessionalEncodeOptions o;
    o.target = options.target == ImageTarget::Exr   ? ProfessionalTarget::Exr
               : options.target == ImageTarget::Hdr ? ProfessionalTarget::Hdr
               : options.target == ImageTarget::Tga ? ProfessionalTarget::Tga
                                                    : ProfessionalTarget::Dds;
    o.allow_lossy = options.allow_lossy;
    o.alpha = alpha;
    o.dds_format = options.dds_format;
    o.exr_tiled = options.exr_tiled;
    if (o.target == ProfessionalTarget::Hdr)
      loss("lossy_codec", "RGBE shared-exponent quantization");
    if (o.target == ProfessionalTarget::Dds &&
        ((o.dds_format >= 70 && o.dds_format <= 84) ||
         (o.dds_format >= 94 && o.dds_format <= 99)))
      loss("lossy_codec", "BC block compression; BC1 alpha is binary");
    result.encoded = encode_professional(image, o, limits, stop);
  }
  return result;
}
} // namespace formaview::imaging
