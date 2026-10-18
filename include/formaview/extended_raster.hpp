#pragma once
#include "formaview/pro_image.hpp"

namespace formaview::imaging {
// TIFF native samples and static WebP. Worker-only, no Qt/window dependency.
class ExtendedRasterFile {
public:
  static ExtendedRasterFile open(const std::filesystem::path &, Limits = {},
                                 std::stop_token = {});
  ExtendedRasterFile(ExtendedRasterFile &&) noexcept;
  ExtendedRasterFile &operator=(ExtendedRasterFile &&) noexcept;
  ~ExtendedRasterFile();
  [[nodiscard]] const ImageData &description() const;
  [[nodiscard]] ImageSubresource decode(std::size_t entry,
                                        std::stop_token = {}) const;
  [[nodiscard]] std::size_t resident_bytes() const;

private:
  struct Impl;
  explicit ExtendedRasterFile(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
};
enum class ExtendedRasterTarget { Tiff, Webp };
enum class TiffCompression { None, Lzw, Deflate, PackBits };
struct ExtendedEncodeOptions {
  ExtendedRasterTarget target{ExtendedRasterTarget::Tiff};
  TiffCompression compression{TiffCompression::Deflate};
  AlphaMode alpha{AlphaMode::Unknown};
  bool webp_lossless{true}, allow_lossy{};
  float quality{90};
  std::vector<std::byte> icc_profile;
};
[[nodiscard]] std::vector<std::byte>
encode_extended(const ImageSubresource &, const ExtendedEncodeOptions &,
                Limits = {}, std::stop_token = {});
} // namespace formaview::imaging
