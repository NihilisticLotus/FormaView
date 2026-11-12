#include "image_session.hpp"
#include <limits>

using namespace formaview;
using namespace formaview::imaging;

void ImagePixels::prepareUpload(std::stop_token stop) {
  const auto cancelled = [&] {
    if (stop.stop_requested())
      throw skeletal::Failure(ErrorCode::Cancelled,
                              "Image preparation cancelled");
  };
  cancelled();
  const auto width = source.data_window.width;
  const auto height = source.data_window.height;
  if (!width || !height ||
      width > std::numeric_limits<std::size_t>::max() / height)
    throw skeletal::Failure(ErrorCode::InvalidArgument,
                            "Invalid image dimensions");
  if (!rgba.empty() || !rgba8.empty())
    throw skeletal::Failure(ErrorCode::InvalidArgument,
                            "Image upload already prepared");
  const auto pixels = static_cast<std::size_t>(width * height);
  bool normalizedBytes = true;
  std::array<const std::byte *, 4> planes{};
  for (std::size_t c = 0; c < mapping.size(); ++c) {
    const auto index = mapping[c];
    if (index == -1)
      continue;
    if (index < 0 || static_cast<std::size_t>(index) >= source.channels.size())
      throw skeletal::Failure(ErrorCode::InvalidArgument,
                              "Invalid image channel mapping");
    const auto &channel = source.channels[static_cast<std::size_t>(index)];
    // Validate scalar/encoding and the last sample once before the byte loop.
    (void)interpreted_value(channel, pixels - 1);
    normalizedBytes &= channel.samples.scalar == ScalarType::UInt8 &&
                       channel.encoding == SampleEncoding::UNorm;
    planes[c] = channel.samples.bytes.data();
  }
  // RGBA8 stores the original UNorm samples exactly. HDR, numeric data and
  // higher precision channels retain the existing floating-point upload path.
  const std::size_t stride = normalizedBytes ? 4 : 4 * sizeof(float);
  constexpr std::size_t budget = 512ULL * 1024 * 1024;
  if (bytes > budget || pixels > (budget - bytes) / stride)
    throw skeletal::Failure(
        ErrorCode::ResourceLimit,
        "Source plus display upload exceeds 512 MiB preview budget");
  if (normalizedBytes) {
    rgba8.resize(pixels * 4);
    for (std::size_t i = 0; i < pixels; ++i) {
      if ((i & 4095) == 0)
        cancelled();
      for (std::size_t c = 0; c < 4; ++c)
        rgba8[i * 4 + c] = planes[c] ? planes[c][i]
                           : c == 3  ? std::byte{255}
                                     : std::byte{0};
    }
  } else {
    rgba.resize(pixels * 4);
    for (std::size_t i = 0; i < pixels; ++i) {
      if ((i & 4095) == 0)
        cancelled();
      for (std::size_t c = 0; c < 4; ++c)
        rgba[i * 4 + c] =
            mapping[c] >= 0
                ? static_cast<float>(interpreted_value(
                      source.channels[static_cast<std::size_t>(mapping[c])], i))
            : c == 3 ? 1.F
                     : 0.F;
    }
  }
  cancelled();
  bytes += uploadBytes();
}
