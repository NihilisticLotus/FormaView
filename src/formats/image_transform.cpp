#include "formaview/image_transform.hpp"
#include <Imath/half.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>

namespace formaview::imaging {
namespace {
[[noreturn]] void invalid(const char *message) {
  throw skeletal::Failure(ErrorCode::InvalidArgument, message);
}
void check_cancel(std::stop_token stop) {
  if (stop.stop_requested())
    throw skeletal::Failure(ErrorCode::Cancelled, "Image transform cancelled");
}
std::size_t bytes_per_sample(ScalarType t) {
  switch (t) {
  case ScalarType::UInt8: return 1;
  case ScalarType::UInt16: case ScalarType::Int16: case ScalarType::Half: return 2;
  case ScalarType::UInt32: case ScalarType::Int32: case ScalarType::Float32: return 4;
  case ScalarType::Float64: return 8;
  }
  invalid("Invalid sample scalar");
}
double unsigned_max(ScalarType t) {
  switch (t) {
  case ScalarType::UInt8: return 255;
  case ScalarType::UInt16: return 65535;
  case ScalarType::UInt32: return 4294967295.;
  default: invalid("UNorm requires an unsigned integer scalar");
  }
}
double srgb_to_linear(double x) {
  return x <= .04045 ? x / 12.92 : std::pow((x + .055) / 1.055, 2.4);
}
double linear_to_srgb(double x) {
  return x <= .0031308 ? 12.92 * x : 1.055 * std::pow(x, 1 / 2.4) - .055;
}
int colour_component(const std::string &name) {
  return name == "R" ? 0 : name == "G" ? 1 : name == "B" ? 2 : -1;
}
template<class T> void store(Buffer &b, std::size_t i, T v) {
  std::memcpy(b.bytes.data() + i * sizeof(T), &v, sizeof(T));
}
bool same_number(double a, double b) {
  return (std::isnan(a) && std::isnan(b)) ||
         (a == b && (a != 0 || std::signbit(a) == std::signbit(b)));
}
} // namespace

TransformedImage transform(const ImageSubresource &source,
                           const TransformOptions &options, Limits limits,
                           std::stop_token stop) {
  check_cancel(stop);
  const auto w = source.data_window.width, h = source.data_window.height;
  if (!w || !h || w > limits.dimension || h > limits.dimension ||
      w > SIZE_MAX / h || source.channels.size() > limits.channels ||
      options.channels.empty() || options.channels.size() > limits.channels)
    throw skeletal::Failure(ErrorCode::ResourceLimit, "Invalid image transform dimensions or channels");
  const auto pixels = static_cast<std::size_t>(w * h);
  if (!std::isfinite(options.exposure) || std::abs(options.exposure) > 64)
    invalid("Exposure must be finite and within [-64,64]");
  if (options.meaning != ImageMeaning::Colour &&
      (options.exposure != 0 || options.transfer != Transfer::Preserve ||
       options.alpha_action == AlphaAction::Composite))
    invalid("Colour operations require explicit colour interpretation");
  const auto transformed = options.exposure != 0 || options.transfer != Transfer::Preserve;
  if (options.output_scalar && *options.output_scalar != ScalarType::UInt8 &&
      *options.output_scalar != ScalarType::UInt16 &&
      *options.output_scalar != ScalarType::Half &&
      *options.output_scalar != ScalarType::Float32)
    invalid("Output scalar requires a validated conversion path");
  std::set<std::string> source_names;
  for (const auto &c : source.channels) {
    if (c.name.empty() || !source_names.insert(c.name).second ||
        c.samples.components != 1 || c.samples.element_count != pixels ||
        pixels > SIZE_MAX / bytes_per_sample(c.samples.scalar) ||
        c.samples.bytes.size() != pixels * bytes_per_sample(c.samples.scalar))
      invalid("Malformed or ambiguous source channel");
    (void)interpreted_value(c, 0); // Validate encoding even on the raw-copy path.
  }
  auto find = [&](const std::string &name) -> const ImageChannel * {
    for (const auto &c : source.channels) if (c.name == name) return &c;
    return nullptr;
  };
  const auto *alpha = options.alpha_channel.empty() ? nullptr : find(options.alpha_channel);
  if (!options.alpha_channel.empty() && !alpha) invalid("Alpha channel missing");
  if (options.source_alpha == AlphaMode::Absent && alpha)
    invalid("Absent alpha conflicts with selected alpha channel");
  if (options.alpha_action == AlphaAction::Composite &&
      (!alpha || (options.source_alpha != AlphaMode::Straight &&
                  options.source_alpha != AlphaMode::Premultiplied)))
    invalid("Compositing requires explicit straight or premultiplied alpha");
  // Transfer/exposure of premultiplied or unknown-alpha RGB is ambiguous without
  // an explicit unpremultiply/re-premultiply policy. Never silently guess one.
  if (transformed && alpha && options.source_alpha != AlphaMode::Straight &&
      options.alpha_action != AlphaAction::Composite)
    invalid("Colour conversion requires straight alpha or explicit compositing");
  for (auto v : options.background_linear)
    if (!std::isfinite(v)) invalid("Composite background must be finite linear RGB");
  TransformedImage result;
  result.image.part = source.part;
  result.image.part_name = source.part_name;
  result.image.mip = source.mip;
  result.image.array_layer = source.array_layer;
  result.image.cube_face = source.cube_face;
  result.image.depth_slice = source.depth_slice;
  result.image.data_window = source.data_window;
  result.image.display_window = source.display_window;
  std::set<std::string> destinations, used;
  std::uint64_t total_bytes = 0;
  for (const auto &mapping : options.channels) {
    check_cancel(stop);
    const auto *c = find(mapping.source);
    if (!c || mapping.destination.empty() ||
        !destinations.insert(mapping.destination).second)
      invalid("Missing source channel or duplicate destination channel");
    const bool is_alpha = c == alpha;
    if (is_alpha && options.alpha_action != AlphaAction::Preserve)
      invalid("Dropped/composited alpha cannot remain in output channel mapping");
    const int component = colour_component(mapping.destination);
    if (options.alpha_action == AlphaAction::Composite && component < 0)
      invalid("Composite output must map explicitly to RGB");
    if (is_alpha && component >= 0 && transformed)
      invalid("Alpha cannot be treated as colour by a transfer operation");
    used.insert(mapping.source);
    ImageChannel out;
    out.name = mapping.destination;
    out.samples.scalar = options.output_scalar.value_or(c->samples.scalar);
    out.samples.element_count = pixels;
    out.encoding = options.output_scalar ?
        (out.samples.scalar == ScalarType::UInt8 || out.samples.scalar == ScalarType::UInt16
          ? SampleEncoding::UNorm : SampleEncoding::Numeric) : c->encoding;
    const auto stride = bytes_per_sample(out.samples.scalar);
    if (pixels > limits.decoded_bytes / stride ||
        total_bytes > limits.decoded_bytes - pixels * stride)
      throw skeletal::Failure(ErrorCode::ResourceLimit, "Image transform output budget exceeded");
    total_bytes += pixels * stride;
    const bool raw = !options.output_scalar && !transformed &&
                     options.alpha_action != AlphaAction::Composite &&
                     options.non_finite == NonFinite::Preserve;
    if (raw) {
      out.samples.bytes.resize(pixels * stride);
      for (std::size_t offset = 0; offset < out.samples.bytes.size();) {
        check_cancel(stop);
        const auto count = std::min<std::size_t>(1024 * 1024, out.samples.bytes.size() - offset);
        std::memcpy(out.samples.bytes.data() + offset, c->samples.bytes.data() + offset, count);
        offset += count;
      }
      result.report.samples += pixels;
      result.image.channels.push_back(std::move(out));
      continue;
    }
    if (!options.output_scalar)
      invalid("Numeric operations require an explicit output scalar");
    out.samples.bytes.resize(pixels * stride);
    for (std::size_t i = 0; i < pixels; ++i) {
      if ((i & 4095) == 0) check_cancel(stop);
      double v = interpreted_value(*c, i);
      if (component >= 0 && !is_alpha) {
        // Composite background is linear. Transfer names define source/output
        // encoding: SrgbToLinear decodes before compositing; LinearToSrgb
        // encodes after compositing. Preserve composes linear numeric RGB.
        if (options.transfer == Transfer::SrgbToLinear) v = srgb_to_linear(v);
        if (options.alpha_action == AlphaAction::Composite) {
          const double a = interpreted_value(*alpha, i);
          if (!std::isfinite(a) || a < 0 || a > 1)
            invalid("Composite alpha must be finite in [0,1]");
          if (options.source_alpha == AlphaMode::Premultiplied) {
            if (options.transfer == Transfer::SrgbToLinear)
              invalid("Premultiplied sRGB compositing requires a separate explicit policy");
            v = v * std::exp2(options.exposure) + options.background_linear[static_cast<std::size_t>(component)] * (1 - a);
          } else
            v = v * std::exp2(options.exposure) * a + options.background_linear[static_cast<std::size_t>(component)] * (1 - a);
        } else v *= std::exp2(options.exposure);
        if (options.transfer == Transfer::LinearToSrgb) v = linear_to_srgb(v);
      }
      if (!std::isfinite(v)) {
        if (options.non_finite == NonFinite::Reject)
          invalid("Non-finite output requires an explicit policy");
        if (options.non_finite == NonFinite::ReplaceZero) {
          v = 0;
          ++result.report.non_finite_replaced;
        }
      }
      const auto original = v;
      if (out.encoding == SampleEncoding::UNorm) {
        if (!std::isfinite(v)) invalid("Integer output cannot preserve NaN/Inf");
        if (v < 0 || v > 1) {
          if (options.quantization == Quantization::RejectLoss)
            invalid("Integer clipping requires explicit clamp/round policy");
          ++result.report.clipped;
          v = std::clamp(v, 0., 1.);
        }
        const auto maximum = unsigned_max(out.samples.scalar);
        const auto integer = std::floor(v * maximum + .5);
        if (out.samples.scalar == ScalarType::UInt8)
          store(out.samples, i, static_cast<std::uint8_t>(integer));
        else store(out.samples, i, static_cast<std::uint16_t>(integer));
        v = integer / maximum;
      } else if (out.samples.scalar == ScalarType::Half) {
        const IMATH_NAMESPACE::half half(static_cast<float>(v));
        store(out.samples, i, half);
        v = static_cast<float>(half);
      } else {
        const auto f = static_cast<float>(v);
        store(out.samples, i, f);
        v = f;
      }
      if (!same_number(original, v)) {
        ++result.report.precision_changed;
        if (options.quantization == Quantization::RejectLoss)
          invalid("Precision loss requires explicit quantization policy");
      }
      // Finite input must never silently overflow a floating output to Inf.
      if (std::isfinite(original) && !std::isfinite(v))
        invalid("Finite value overflows selected floating output scalar");
      ++result.report.samples;
    }
    result.image.channels.push_back(std::move(out));
  }
  if (alpha && options.alpha_action == AlphaAction::Preserve && !used.contains(alpha->name))
    invalid("Omitting alpha requires an explicit drop/composite action");
  for (const auto &c : source.channels)
    if (!used.contains(c.name)) result.report.omitted_channels.push_back(c.name);
  result.report.alpha_removed = alpha && !used.contains(alpha->name);
  result.report.colour_transformed = transformed || options.alpha_action == AlphaAction::Composite;
  check_cancel(stop);
  return result;
}
} // namespace formaview::imaging
