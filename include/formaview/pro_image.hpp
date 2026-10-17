#pragma once
#include "formaview/skeletal.hpp"
#include <memory>

namespace formaview::imaging {
struct Limits {
  std::uint64_t input_bytes{512ULL * 1024 * 1024},
      decoded_bytes{1024ULL * 1024 * 1024};
  std::uint32_t dimension{32768}, subresources{65536}, channels{256};
};
// Open/decode are blocking worker APIs. Call via the bounded scheduler, never
// on the GUI or render thread. Cancellation is checked between codec
// operations.
class ImageFile {
public:
  static ImageFile open(const std::filesystem::path &, Limits = {},
                        std::stop_token = {});
  ImageFile(ImageFile &&) noexcept;
  ImageFile &operator=(ImageFile &&) noexcept;
  ~ImageFile();
  [[nodiscard]] const ImageData &description() const;
  [[nodiscard]] ImageSubresource decode(std::size_t index,
                                        std::stop_token = {}) const;
  [[nodiscard]] std::size_t resident_bytes() const;

private:
  struct Impl;
  explicit ImageFile(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
};
[[nodiscard]] double value(const ImageChannel &, std::size_t pixel);
struct DisplayOptions {
  std::array<std::string, 4> channels{"R", "G", "B", "A"};
  ImageMeaning meaning{ImageMeaning::Data};
  AlphaMode alpha{AlphaMode::Unknown};
  float exposure{};
  bool apply_srgb{}, composite{};
};
// RGBA32F upload data; NaN/Inf are marked magenta only in this derived buffer.
[[nodiscard]] std::vector<float> display_rgba(const ImageSubresource &,
                                              const DisplayOptions &);
} // namespace formaview::imaging
