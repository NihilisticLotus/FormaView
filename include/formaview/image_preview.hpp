#pragma once
#include "formaview/pro_image.hpp"
#include <algorithm>
#include <cstring>

namespace formaview::imaging {
// Worker preview coordinates relative to the selected data window; sampling is
// nearest-neighbour so a one-pixel request retains the exact stored sample,
// including NaN payloads.
struct PreviewRequest {
  std::uint64_t x{}, y{}, width{}, height{}, output_width{}, output_height{};
  void validate(const ImageSubresource &s) const {
    if (!width || !height || !output_width || !output_height ||
        output_width > 2048 || output_height > 2048 ||
        x > s.data_window.width || width > s.data_window.width - x ||
        y > s.data_window.height || height > s.data_window.height - y)
      throw skeletal::Failure(ErrorCode::InvalidArgument,
                              "Invalid preview region");
  }
  std::uint64_t sx(std::uint64_t px) const {
    return x + px * width / output_width;
  }
  std::uint64_t sy(std::uint64_t py) const {
    return y + py * height / output_height;
  }
  std::uint64_t first_x(std::uint64_t source) const {
    return source <= x
               ? 0
               : std::min(output_width,
                          ((source - x) * output_width + width - 1) / width);
  }
  std::uint64_t first_y(std::uint64_t source) const {
    return source <= y
               ? 0
               : std::min(output_height,
                          ((source - y) * output_height + height - 1) / height);
  }
};
inline std::size_t preview_component(ScalarType t) {
  return t == ScalarType::UInt8 ? 1
         : t == ScalarType::UInt16 || t == ScalarType::Int16 ||
                 t == ScalarType::Half
             ? 2
         : t == ScalarType::Float64 ? 8
                                    : 4;
}
inline ImageSubresource preview_output(ImageSubresource s,
                                       const PreviewRequest &r) {
  r.validate(s);
  s.encoded_blocks.clear();
  s.data_window = s.display_window = {0, 0, r.output_width, r.output_height};
  std::uint64_t bytes = 0;
  for (auto &c : s.channels) {
    c.samples.element_count = r.output_width * r.output_height;
    bytes += c.samples.element_count * preview_component(c.samples.scalar);
    if (bytes > 128ULL * 1024 * 1024)
      throw skeletal::Failure(ErrorCode::ResourceLimit,
                              "Preview channel working set exceeds 128 MiB; "
                              "select a smaller region");
    c.samples.bytes.resize(static_cast<std::size_t>(c.samples.element_count) *
                           preview_component(c.samples.scalar));
  }
  return s;
}
inline void preview_cancel(std::stop_token stop) {
  if (stop.stop_requested())
    throw skeletal::Failure(ErrorCode::Cancelled, "Preview cancelled");
}
} // namespace formaview::imaging
