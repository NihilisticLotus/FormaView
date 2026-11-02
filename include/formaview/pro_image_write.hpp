#pragma once
#include "formaview/image_transform.hpp"

namespace formaview::imaging {
enum class ProfessionalTarget { Exr, Hdr, Tga, Dds };
struct ProfessionalEncodeOptions {
  ProfessionalTarget target{ProfessionalTarget::Exr};
  bool allow_lossy{};
  bool exr_tiled{};
  // 0 preserves a supported native layout; otherwise an explicit DXGI encoding.
  std::uint32_t dds_format{};
  AlphaMode alpha{AlphaMode::Unknown};
  // Transfer tag only; no implicit pixel transform or quantization.
  bool srgb{};
};
[[nodiscard]] bool professional_preserves_srgb(const ImageSubresource &,
                                               const ProfessionalEncodeOptions &);
// Single explicitly selected 2D subresource. Cubemap/array assembly is not
// implicit. OpenEXR retains named channels and data/display windows.
[[nodiscard]] std::vector<std::byte>
encode_professional(const ImageSubresource &, const ProfessionalEncodeOptions &,
                    Limits = {}, std::stop_token = {});
} // namespace formaview::imaging
