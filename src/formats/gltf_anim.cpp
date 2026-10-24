#include "formaview/gltf_anim.hpp"
#include "formaview/file_access.hpp"
#include "gltf_storage.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <fstream>

// The implementation header pulls the vendored cgltf into this TU only.
#include <cgltf.h>

namespace formaview::gltf {
namespace {

[[noreturn]] void fail(const char *message) {
  throw skeletal::Failure(ErrorCode::Unsupported, message);
}

void checkStop(std::stop_token stop) {
  if (stop.stop_requested())
    throw skeletal::Failure(ErrorCode::Cancelled, "glTF animation read cancelled");
}

std::uint32_t componentsOf(cgltf_type type) {
  switch (type) {
  case cgltf_type_scalar: return 1;
  case cgltf_type_vec2: return 2;
  case cgltf_type_vec3: return 3;
  case cgltf_type_vec4: return 4;
  default: fail("Unsupported glTF accessor type");
  }
}

double lerp(double a, double b, double t) { return a + (b - a) * t; }

skeletal::Vec3 lerp(const skeletal::Vec3 &a, const skeletal::Vec3 &b,
                    double t) {
  return {lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t)};
}

skeletal::Quat slerp(const skeletal::Quat &x, const skeletal::Quat &y,
                     double t) {
  double dot = x.x * y.x + x.y * y.y + x.z * y.z + x.w * y.w;
  skeletal::Quat to = y;
  if (dot < 0) {
    dot = -dot;
    to = {-y.x, -y.y, -y.z, -y.w};
  }
  if (dot > 1 - 1e-12)
    return {lerp(x.x, to.x, t), lerp(x.y, to.y, t), lerp(x.z, to.z, t),
            lerp(x.w, to.w, t)};
  const double theta = std::acos(std::min(1.0, dot));
  const double sinTheta = std::sin(theta);
  const double w1 = std::sin((1 - t) * theta) / sinTheta;
  const double w2 = std::sin(t * theta) / sinTheta;
  return {w1 * x.x + w2 * to.x, w1 * x.y + w2 * to.y, w1 * x.z + w2 * to.z,
          w1 * x.w + w2 * to.w};
}

// Cubic Hermite spline for scalar components per glTF cubic_spline layout
// [inTangent, value, outTangent] per key.
double hermite(double p0, double p1, double m0, double m1, double dt,
               double duration) {
  const double t = dt, t2 = t * t, t3 = t2 * t;
  return (2 * t3 - 3 * t2 + 1) * p0 + (t3 - 2 * t2 + t) * duration * m0 +
         (-2 * t3 + 3 * t2) * p1 + (t3 - t2) * duration * m1;
}

struct ChannelSample {
  std::size_t lo{}, hi{};
  double t{};
};

// Times are ascending per glTF; binary search with a linear fallback for
// equal boundaries. Outside the range clamps to the ends (spec behaviour).
ChannelSample locate(const std::vector<double> &times, double seconds) {
  ChannelSample sample;
  if (seconds <= times.front()) {
    sample.hi = sample.lo = 0;
    return sample;
  }
  if (seconds >= times.back()) {
    sample.hi = sample.lo = times.size() - 1;
    return sample;
  }
  const auto upper =
      std::upper_bound(times.begin(), times.end(), seconds);
  sample.hi = static_cast<std::size_t>(upper - times.begin());
  sample.lo = sample.hi - 1;
  sample.t = (seconds - times[sample.lo]) /
             std::max(1e-30, times[sample.hi] - times[sample.lo]);
  return sample;
}

std::vector<char> decodeDataUri(const std::string &uri) {
  const auto comma = uri.find(',');
  if (comma == std::string::npos || comma < 7 ||
      uri.compare(comma - 7, 7, ";base64") != 0)
    fail("Unsupported glTF data URI encoding");
  static const std::string alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::size_t end = uri.size(), padding = 0;
  while (end > comma + 1 && uri[end - 1] == '=') {
    ++padding;
    --end;
  }
  const std::size_t dataChars = end - comma - 1;
  if ((dataChars + padding) % 4 != 0 || padding > 2 ||
      padding != (4 - dataChars % 4) % 4)
    fail("Malformed glTF base64 data URI");
  std::vector<char> decoded;
  decoded.reserve(dataChars / 4 * 3 + 3);
  std::uint32_t group = 0;
  int digits = 0;
  for (std::size_t at = comma + 1; at < end; ++at) {
    const unsigned char value = uri[at];
    if (value == '\n' || value == '\r')
      continue; // cgltf tolerated embedded newlines; keep that behavior.
    const auto index = alphabet.find(static_cast<char>(value));
    if (index == std::string::npos)
      fail("Malformed glTF base64 data URI");
    group = (group << 6) | static_cast<std::uint32_t>(index);
    if (++digits == 4) {
      decoded.push_back(static_cast<char>((group >> 16) & 0xFF));
      decoded.push_back(static_cast<char>((group >> 8) & 0xFF));
      decoded.push_back(static_cast<char>(group & 0xFF));
      group = 0;
      digits = 0;
    }
  }
  if (digits == 2)
    decoded.push_back(static_cast<char>((group >> 4) & 0xFF));
  else if (digits == 3) {
    decoded.push_back(static_cast<char>((group >> 10) & 0xFF));
    decoded.push_back(static_cast<char>((group >> 2) & 0xFF));
  } else if (digits != 0)
    fail("Malformed glTF base64 data URI");
  return decoded;
}
std::filesystem::path decodeUriPath(const std::string &uri) {
  std::string decoded;
  for (std::size_t at = 0; at < uri.size(); ++at) {
    if (uri[at] == '%') {
      if (at + 2 >= uri.size() ||
          !std::isxdigit(static_cast<unsigned char>(uri[at + 1])) ||
          !std::isxdigit(static_cast<unsigned char>(uri[at + 2])))
        fail("Malformed glTF buffer URI escape");
      const auto hex = [](char c) {
        return c <= '9' ? c - '0' : (c | 32) - 'a' + 10;
      };
      decoded.push_back(static_cast<char>((hex(uri[at + 1]) << 4) |
                                          hex(uri[at + 2])));
      at += 2;
    } else if (uri[at] == '\0' || uri[at] == '?' || uri[at] == '#') {
      fail("Invalid glTF buffer URI");
    } else {
      decoded.push_back(uri[at]);
    }
  }
  if (decoded.empty() || decoded.front() == '/' || decoded.front() == '\\' ||
      decoded.find(':') != std::string::npos ||
      decoded.find("..") != std::string::npos)
    fail("glTF buffer URI must be a relative local path under the input root");
  // glTF URIs are UTF-8; build the wide path without the ANSI code page.
  std::wstring wide;
  for (std::size_t i = 0; i < decoded.size();) {
    const auto lead = static_cast<unsigned char>(decoded[i]);
    const std::size_t length = lead < 0x80    ? 1
                               : lead >= 0xF0 ? 4
                               : lead >= 0xE0 ? 3
                               : lead >= 0xC0 ? 2
                                              : 0;
    if (length == 0 || i + length > decoded.size())
      fail("Malformed glTF buffer URI UTF-8");
    std::uint32_t code =
        lead & (length == 1 ? 0x7F : length == 2   ? 0x1F
                             : length == 3 ? 0x0F
                                           : 0x07);
    for (std::size_t k = 1; k < length; ++k) {
      const auto cont = static_cast<unsigned char>(decoded[i + k]);
      if ((cont & 0xC0) != 0x80)
        fail("Malformed glTF buffer URI UTF-8");
      code = (code << 6) | (cont & 0x3F);
    }
    if (code >= 0x10000) {
      code -= 0x10000;
      wide.push_back(static_cast<wchar_t>(0xD800 + (code >> 10)));
      wide.push_back(static_cast<wchar_t>(0xDC00 + (code & 0x3FF)));
    } else {
      wide.push_back(static_cast<wchar_t>(code));
    }
    i += length;
  }
  return wide;
}
} // namespace

GltfAnimationData read_gltf_animations(const std::filesystem::path &path,
                                       const skeletal::Limits &limits,
                                       std::stop_token stop) {
  checkStop(stop);
  // Unified with the product host loader: constrained wide-path access via
  // InputLease (per-file and cumulative input budget plus change detection)
  // and parsing from memory - cgltf_parse_file(path.string()) cannot open
  // paths outside the ANSI code page and bypasses the lease.
  std::vector<InputLease> leases;
  std::uint64_t leased = 0;
  const auto read = [&](const std::filesystem::path &target) {
    checkStop(stop);
    AccessScope scope;
    scope.input_roots = {target.parent_path()};
    auto lease = InputLease::open(scope, target, limits.input_bytes, stop);
    if (lease.snapshot().size_bytes > limits.input_bytes - leased)
      fail("glTF input budget exceeded");
    leased += lease.snapshot().size_bytes;
    std::ifstream stream(target, std::ios::binary);
    if (!stream)
      fail("Cannot read glTF resource");
    std::vector<char> bytes(
        static_cast<std::size_t>(lease.snapshot().size_bytes));
    if (!bytes.empty() &&
        !stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
      fail("glTF resource read failed");
    lease.require_unchanged(lease.snapshot());
    leases.push_back(std::move(lease));
    return bytes;
  };
  const auto main = read(path);
  GltfAnimationData result;
  cgltf_options options{};
  cgltf_data *raw = nullptr;
  cgltf_result status =
      cgltf_parse(&options, main.data(), static_cast<cgltf_size>(main.size()), &raw);
  if (status != cgltf_result_success || !raw)
    fail("cgltf parse failed");
  std::unique_ptr<cgltf_data, decltype(&cgltf_free)> owned{raw, cgltf_free};
  if (raw->nodes_count > limits.bones || raw->animations_count > limits.clips)
    fail("glTF node/clip budget exceeded");
  std::uint64_t bufferBytes = 0;
  for (cgltf_size i = 0; i < raw->buffers_count; ++i) {
    if (raw->buffers[i].size > limits.input_bytes - bufferBytes)
      fail("glTF buffer budget exceeded");
    bufferBytes += raw->buffers[i].size;
  }
  checkStop(stop);
  // Buffers resolve by hand, mirroring the product loader: the GLB binary
  // chunk, strict base64 data URIs, and leased relative external files -
  // never cgltf_load_buffers(path.string()). The storage outlives every
  // accessor read below.
  std::vector<std::vector<char>> storage(raw->buffers_count);
  for (cgltf_size i = 0; i < raw->buffers_count; ++i) {
    checkStop(stop);
    auto &buffer = raw->buffers[i];
    if (!buffer.uri) {
      if (i != 0 || !raw->bin || buffer.size > raw->bin_size)
        fail("Missing GLB binary buffer");
      buffer.data = const_cast<void *>(static_cast<const void *>(raw->bin));
      buffer.data_free_method = cgltf_data_free_method_none;
      continue;
    }
    const std::string uri(buffer.uri);
    storage[static_cast<std::size_t>(i)] =
        uri.rfind("data:", 0) == 0
            ? decodeDataUri(uri)
            : read(path.parent_path() / decodeUriPath(uri));
    if (buffer.size > storage[static_cast<std::size_t>(i)].size())
      fail("Truncated glTF external buffer");
    buffer.data = storage[static_cast<std::size_t>(i)].data();
    buffer.data_free_method = cgltf_data_free_method_none;
  }
  validate_loaded_storage(*raw, limits, stop, false);
  status = cgltf_validate(raw);
  if (status != cgltf_result_success)
    fail("cgltf validation failed");
  if (raw->nodes_count > limits.bones)
    fail("glTF node budget exceeded");
  result.node_count = static_cast<std::uint32_t>(raw->nodes_count);

  std::uint64_t elements = 0;
  for (cgltf_size c = 0; c < raw->animations_count; ++c) {
    checkStop(stop);
    const cgltf_animation &animation = raw->animations[c];
    AnimationClip clip;
    clip.name = animation.name ? animation.name : "";
    for (cgltf_size k = 0; k < animation.channels_count; ++k) {
      const cgltf_animation_channel &channel = animation.channels[k];
      if (!channel.target_node) {
        result.warnings.push_back("Animation channel without target node");
        continue;
      }
      AnimationChannel parsed;
      parsed.node_index = static_cast<std::uint32_t>(channel.target_node -
                                                     raw->nodes);
      switch (channel.target_path) {
      case cgltf_animation_path_type_translation:
        parsed.path = TrackPath::Translation;
        parsed.components = 3;
        break;
      case cgltf_animation_path_type_rotation:
        parsed.path = TrackPath::Rotation;
        parsed.components = 4;
        break;
      case cgltf_animation_path_type_scale:
        parsed.path = TrackPath::Scale;
        parsed.components = 3;
        break;
      default:
        parsed.path = TrackPath::Unsupported;
        result.warnings.push_back(
            "Unsupported animation path (weights) retained as flagged");
        parsed.components = 0;
        break;
      }
      if (!channel.sampler || !channel.sampler->input || !channel.sampler->output)
        fail("Animation sampler without accessors");
      switch (channel.sampler->interpolation) {
      case cgltf_interpolation_type_step:
        parsed.interpolation = Interpolation::Step;
        break;
      case cgltf_interpolation_type_cubic_spline:
        parsed.interpolation = Interpolation::CubicSpline;
        break;
      default:
        parsed.interpolation = Interpolation::Linear;
        break;
      }
      if (!channel.sampler->input || !channel.sampler->output)
        fail("Animation sampler without accessors");
      const auto &input = *channel.sampler->input;
      const auto &output = *channel.sampler->output;
      if (componentsOf(input.type) != 1)
        fail("Animation times must be scalar");
      const std::uint32_t valueComponents = componentsOf(output.type);
      if (parsed.path != TrackPath::Unsupported &&
          valueComponents != parsed.components)
        fail("Animation value type does not match target path");
      const auto multiplier = parsed.interpolation == Interpolation::CubicSpline ? 3u : 1u;
      const auto targets = parsed.path == TrackPath::Unsupported && channel.target_node->mesh &&
                                   channel.target_node->mesh->primitives_count > 0
                               ? channel.target_node->mesh->primitives[0].targets_count : 1u;
      if (!input.count || !targets || input.count > limits.elements / multiplier / targets ||
          output.count != input.count * multiplier * targets)
        fail("Animation key count mismatch");
      if (output.count > limits.elements / valueComponents || input.count > limits.elements ||
          output.count * valueComponents > limits.elements - input.count ||
          elements > limits.elements - input.count - output.count * valueComponents)
        fail("glTF animation element budget exceeded");
      elements += input.count + output.count * valueComponents;
      if (elements > limits.decoded_bytes / sizeof(double))
        fail("glTF animation decoded byte budget exceeded");
      parsed.components = valueComponents;
      for (cgltf_size i = 0; i < input.count; ++i) {
        cgltf_float value[4]{};
        if (!cgltf_accessor_read_float(&input, i, value, 1))
          fail("Animation time read failed");
        checkStop(stop);
        if (!std::isfinite(value[0]) || value[0] < 0 ||
            (!parsed.times.empty() && value[0] <= parsed.times.back()))
          fail("Animation times must be finite, nonnegative and strictly increasing");
        parsed.times.push_back(value[0]);
      }
      std::vector<cgltf_float> element(valueComponents);
      for (cgltf_size i = 0; i < output.count; ++i) {
        if (!cgltf_accessor_read_float(&output, i, element.data(),
                                       valueComponents))
          fail("Animation value read failed");
        if ((i & 1023) == 0) checkStop(stop);
        for (auto v : element) if (!std::isfinite(v)) fail("Non-finite animation value");
        parsed.values.insert(parsed.values.end(), element.begin(),
                             element.end());
      }
      if (!parsed.times.empty()) {
        clip.start = clip.channels.empty()
                         ? parsed.times.front()
                         : std::min(clip.start, parsed.times.front());
        clip.end = clip.channels.empty()
                       ? parsed.times.back()
                       : std::max(clip.end, parsed.times.back());
      }
      clip.channels.push_back(std::move(parsed));
    }
    result.clips.push_back(std::move(clip));
  }
  return result;
}

std::vector<NodeOverride> evaluate_clip(const GltfAnimationData &data,
                                        std::size_t clip, double seconds,
                                        std::stop_token stop) {
  if (clip >= data.clips.size() || !std::isfinite(seconds))
    fail("Invalid glTF clip/time");
  const AnimationClip &animation = data.clips[clip];
  std::vector<NodeOverride> pose(data.node_count);
  for (const auto &channel : animation.channels) {
    checkStop(stop);
    if (channel.path == TrackPath::Unsupported || channel.path == TrackPath::Weights ||
        channel.times.empty() ||
        channel.node_index >= pose.size())
      continue;
    const auto expected = channel.path == TrackPath::Rotation ? 4u : 3u;
    const auto factor = channel.interpolation == Interpolation::CubicSpline ? 3u : 1u;
    if (channel.components != expected ||
        channel.times.size() > std::numeric_limits<std::size_t>::max() / expected / factor ||
        channel.values.size() != channel.times.size() * expected * factor)
      fail("Malformed animation channel value shape");
    const auto sample = locate(channel.times, seconds);
    const std::uint32_t per = channel.interpolation ==
                                      Interpolation::CubicSpline
                                  ? 3 * channel.components
                                  : channel.components;
    const auto key = [&](std::size_t index) {
      return &channel.values[index * per];
    };
    auto &override = pose[channel.node_index];
    switch (channel.path) {
    case TrackPath::Translation: {
      if (channel.interpolation == Interpolation::Step) {
        override.local.translation = {key(sample.lo)[0], key(sample.lo)[1],
                                      key(sample.lo)[2]};
      } else if (channel.interpolation == Interpolation::CubicSpline) {
        const double duration =
            sample.hi == sample.lo
                ? 0.0
                : channel.times[sample.hi] - channel.times[sample.lo];
        std::array<double,3> value{};
        for (int e = 0; e < 3; ++e)
          value[e] = hermite(
              key(sample.lo)[3 + e], key(sample.hi)[3 + e],
              key(sample.lo)[6 + e], key(sample.hi)[e], sample.t, duration);
        override.local.translation={value[0],value[1],value[2]};
      } else {
        const auto a = key(sample.lo), b = key(sample.hi);
        override.local.translation = skeletal::Vec3{
            lerp(a[0], b[0], sample.t), lerp(a[1], b[1], sample.t),
            lerp(a[2], b[2], sample.t)};
      }
      override.translation = true;
      break;
    }
    case TrackPath::Rotation: {
      if (channel.interpolation == Interpolation::Step) {
        const auto a = key(sample.lo);
        override.local.rotation = {a[0], a[1], a[2], a[3]};
      } else if (channel.interpolation == Interpolation::CubicSpline) {
        const double duration =
            sample.hi == sample.lo
                ? 0.0
                : channel.times[sample.hi] - channel.times[sample.lo];
        std::array<double,4> value{};
        skeletal::Quat rotation;
        for (int e = 0; e < 4; ++e)
          value[e] = hermite(key(sample.lo)[4 + e],
                                     key(sample.hi)[4 + e],
                                     key(sample.lo)[8 + e], key(sample.hi)[e],
                                     sample.t, duration);
        rotation={value[0],value[1],value[2],value[3]};
        const double norm = std::sqrt(rotation.x * rotation.x +
                                      rotation.y * rotation.y +
                                      rotation.z * rotation.z +
                                      rotation.w * rotation.w);
        if (norm > 1e-12)
          rotation = {rotation.x / norm, rotation.y / norm, rotation.z / norm,
                      rotation.w / norm};
        override.local.rotation = rotation;
      } else {
        const auto a = key(sample.lo), b = key(sample.hi);
        override.local.rotation = slerp({a[0], a[1], a[2], a[3]},
                                        {b[0], b[1], b[2], b[3]}, sample.t);
      }
      override.rotation = true;
      break;
    }
    case TrackPath::Scale: {
      if (channel.interpolation == Interpolation::Step) {
        override.local.scale = {key(sample.lo)[0], key(sample.lo)[1],
                                key(sample.lo)[2]};
      } else if (channel.interpolation == Interpolation::CubicSpline) {
        const double duration =
            sample.hi == sample.lo
                ? 0.0
                : channel.times[sample.hi] - channel.times[sample.lo];
        std::array<double,3> value{};
        for (int e = 0; e < 3; ++e)
          value[e] = hermite(
              key(sample.lo)[3 + e], key(sample.hi)[3 + e],
              key(sample.lo)[6 + e], key(sample.hi)[e], sample.t, duration);
        override.local.scale={value[0],value[1],value[2]};
      } else {
        const auto a = key(sample.lo), b = key(sample.hi);
        override.local.scale = skeletal::Vec3{
            lerp(a[0], b[0], sample.t), lerp(a[1], b[1], sample.t),
            lerp(a[2], b[2], sample.t)};
      }
      override.scale = true;
      break;
    }
    default:
      break;
    }
  }
  return pose;
}

} // namespace formaview::gltf
