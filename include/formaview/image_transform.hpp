#pragma once
#include "formaview/pro_image.hpp"

namespace formaview::imaging {
enum class AlphaAction { Preserve, Drop, Composite };
enum class Transfer { Preserve, LinearToSrgb, SrgbToLinear };
enum class Quantization { RejectLoss, ClampRound };
enum class NonFinite { Preserve, Reject, ReplaceZero };
struct ChannelMapping {
  std::string source, destination;
};
// Explicit conversion parameters, independent of viewport state. A null output
// scalar preserves each mapped source channel's scalar and sample encoding.
struct TransformOptions {
  std::vector<ChannelMapping> channels;
  std::optional<ScalarType> output_scalar;
  AlphaAction alpha_action{AlphaAction::Preserve};
  AlphaMode source_alpha{AlphaMode::Unknown};
  std::string alpha_channel;
  ImageMeaning meaning{ImageMeaning::Data};
  Transfer transfer{Transfer::Preserve};
  double exposure{};
  std::array<double, 3> background_linear{};
  Quantization quantization{Quantization::RejectLoss};
  NonFinite non_finite{NonFinite::Preserve};
};
struct TransformReport {
  std::uint64_t samples{}, clipped{}, precision_changed{}, non_finite_replaced{};
  bool alpha_removed{}, colour_transformed{};
  std::vector<std::string> omitted_channels;
};
struct TransformedImage {
  ImageSubresource image;
  TransformReport report;
};
// Worker-only pure operation. No I/O, GUI, GPU, implicit channel reconstruction
// or mutations of source buffers. Output allocation is explicitly bounded.
[[nodiscard]] TransformedImage transform(const ImageSubresource &,
                                         const TransformOptions &,
                                         Limits = {}, std::stop_token = {});
} // namespace formaview::imaging
