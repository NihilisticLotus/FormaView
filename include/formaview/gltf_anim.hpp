#pragma once
#include "formaview/contracts.hpp"
#include "formaview/skeletal.hpp"
#include <stop_token>
#include <string>
#include <vector>

namespace formaview::gltf {

enum class TrackPath { Translation, Rotation, Scale, Weights, Unsupported };
enum class Interpolation { Linear, Step, CubicSpline };

struct AnimationChannel {
  std::uint32_t node_index{}; // Stable node index within the file.
  TrackPath path{};
  Interpolation interpolation{};
  std::vector<double> times;  // Real seconds, stored order.
  std::vector<double> values; // components doubles per key.
  std::uint32_t components{};
};

struct AnimationClip {
  std::string name;
  double start{}, end{}; // Real seconds: min/max over channel times.
  std::vector<AnimationChannel> channels;
};

struct GltfAnimationData {
  std::uint32_t node_count{};
  std::vector<AnimationClip> clips;
  std::vector<std::string> warnings;
};

// Reads all animation clips (GLB or .gltf with embedded/relative buffers).
// Unsupported paths (weights) become Unsupported channels with a warning;
// data is never silently reinterpreted.
[[nodiscard]] GltfAnimationData
read_gltf_animations(const std::filesystem::path &path,
                     const skeletal::Limits &limits,
                     std::stop_token stop = {});

// Per-node animated local components at real seconds (clamped to the clip).
// Only paths present in the clip set their flag; untouched components keep
// the file's static node transform (caller composes).
struct NodeOverride {
  skeletal::Transform local{};
  bool translation{}, rotation{}, scale{};
};

[[nodiscard]] std::vector<NodeOverride>
evaluate_clip(const GltfAnimationData &data, std::size_t clip, double seconds,
              std::stop_token stop = {});

} // namespace formaview::gltf
