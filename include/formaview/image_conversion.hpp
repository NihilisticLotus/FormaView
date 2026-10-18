#pragma once
#include "formaview/extended_raster.hpp"
#include "formaview/pro_image_write.hpp"
#include "formaview/raster_image.hpp"
#include <variant>

namespace formaview::imaging {
class ImageDocument {
public:
  static ImageDocument open(const std::filesystem::path &, Limits = {},
                            std::stop_token = {});
  [[nodiscard]] const ImageData &description() const;
  [[nodiscard]] ImageSubresource decode(std::size_t,
                                        std::stop_token = {}) const;
  [[nodiscard]] std::size_t resident_bytes() const;

private:
  explicit ImageDocument(
      std::variant<ImageFile, RasterFile, ExtendedRasterFile>);
  std::variant<ImageFile, RasterFile, ExtendedRasterFile> file_;
};
enum class ImageTarget { Png, Jpeg, Bmp, Tga, Tiff, Webp, Dds, Hdr, Exr };
enum class WindowPolicy { Preserve, SelectedDataWindow };
enum class MetadataPolicy { PreserveSupported, Drop };
struct ImageConversionOptions {
  std::size_t subresource{};
  ImageTarget target{ImageTarget::Png};
  TransformOptions transform;
  WindowPolicy window{WindowPolicy::Preserve};
  MetadataPolicy metadata{MetadataPolicy::PreserveSupported};
  bool allow_lossy{};
  int quality{90}, png_compression{6};
  bool webp_lossless{true}, exr_tiled{};
  TiffCompression tiff_compression{TiffCompression::Deflate};
  std::uint32_t dds_format{};
};
struct ConversionLoss {
  std::string code, detail;
};
struct ImageConversionProduct {
  std::string input_sha256;
  std::size_t subresource{};
  TransformReport transform;
  std::vector<ConversionLoss> losses;
  std::vector<std::byte> encoded;
};
// Shared worker operation for preflight and execution. The caller owns scope,
// snapshot validation, loss acknowledgement and atomic commit. This pure byte
// producer never writes to a destination, and never reads viewport settings.
[[nodiscard]] ImageConversionProduct
prepare_image_conversion(const ImageDocument &, const ImageConversionOptions &,
                         Limits = {}, std::stop_token = {});
[[nodiscard]] std::string_view image_target_name(ImageTarget);
} // namespace formaview::imaging
