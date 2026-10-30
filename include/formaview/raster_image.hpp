#pragma once
#include "formaview/pro_image.hpp"

namespace formaview::imaging {
// Ordinary raster decoder; QtGui's CPU image handlers do not require a window,
// QGuiApplication or a rendering context. This API remains worker-only.
class RasterFile {
public:
  static RasterFile open(const std::filesystem::path &, Limits = {},
                         std::stop_token = {});
  RasterFile(RasterFile &&) noexcept;
  RasterFile &operator=(RasterFile &&) noexcept;
  ~RasterFile();
  [[nodiscard]] const ImageData &description() const;
  [[nodiscard]] ImageSubresource decode(std::size_t entry,
                                        std::stop_token = {}) const;
  [[nodiscard]] std::size_t resident_bytes() const;

private:
  struct Impl;
  explicit RasterFile(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
};
enum class RasterTarget { Png, Jpeg, Bmp };
struct RasterEncodeOptions {
  RasterTarget target{RasterTarget::Png};
  int quality{92};
  int compression{0}; // PNG defaults to stored (uncompressed) DEFLATE blocks.
  AlphaMode alpha{AlphaMode::Unknown};
  std::vector<std::byte> icc_profile;
};
// Encodes only already-preflighted, explicit Y/YA/RGB/RGBA UInt8/UInt16 UNorm
// samples. No implicit alpha removal, quantization or colour transform.
// Byte output is handed to the task's atomic commit service, never a path here.
[[nodiscard]] std::vector<std::byte> encode_raster(const ImageSubresource &,
                                                   const RasterEncodeOptions &,
                                                   Limits = {},
                                                   std::stop_token = {});
} // namespace formaview::imaging
