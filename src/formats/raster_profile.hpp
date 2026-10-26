#pragma once
#include "formaview/skeletal.hpp"
#include <cstring>
#include <span>

namespace formaview::imaging {
// ICC.1 header: data colour space at 16, profile signature at 36. This
// checks model compatibility only; opaque TIFF/WebP metadata remains opaque.
inline void validate_icc_model(std::span<const std::byte> profile, bool gray) {
  if (profile.size() < 128 || std::memcmp(profile.data() + 36, "acsp", 4))
    return;
  if (std::memcmp(profile.data() + 16, gray ? "GRAY" : "RGB ", 4))
    throw skeletal::Failure(
        ErrorCode::Unsupported,
        "ICC colour model does not match output channels; choose metadata "
        "drop or a matching profile");
}
} // namespace formaview::imaging
