#include "scene_gltf.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace formaview::scene_export {
namespace {
using Values = std::vector<double>;
struct Track {
  const cgltf_animation_sampler &source;
  const std::vector<float> &times, &values;
  size_t components;
  bool rotation;
  Values at(double time) const {
    const size_t stride =
        components *
        (source.interpolation == cgltf_interpolation_type_cubic_spline ? 3 : 1);
    const size_t offset =
        source.interpolation == cgltf_interpolation_type_cubic_spline
            ? components
            : 0;
    const auto key = std::upper_bound(times.begin(), times.end(), time);
    const size_t a =
        key == times.begin() ? 0 : static_cast<size_t>(key - times.begin() - 1);
    const size_t b = std::min(a + 1, times.size() - 1);
    const double dt = static_cast<double>(times[b]) - times[a];
    const double t = a == b ? 0 : std::clamp((time - times[a]) / dt, 0.0, 1.0);
    Values out(components);
    if (source.interpolation == cgltf_interpolation_type_step || a == b ||
        key == times.begin()) {
      for (size_t i = 0; i < components; ++i)
        out[i] = values[a * stride + offset + i];
    } else if (source.interpolation == cgltf_interpolation_type_cubic_spline) {
      const double t2 = t * t, t3 = t2 * t;
      for (size_t i = 0; i < components; ++i)
        out[i] =
            (2 * t3 - 3 * t2 + 1) * values[a * stride + components + i] +
            (t3 - 2 * t2 + t) * dt * values[a * stride + components * 2 + i] +
            (-2 * t3 + 3 * t2) * values[b * stride + components + i] +
            (t3 - t2) * dt * values[b * stride + i];
    } else if (rotation) {
      double dot{};
      for (size_t i = 0; i < 4; ++i)
        dot += static_cast<double>(values[a * stride + i]) *
               values[b * stride + i];
      const double sign = dot < 0 ? -1 : 1;
      dot = std::clamp(std::abs(dot), 0.0, 1.0);
      const auto angle = std::acos(dot), denominator = std::sin(angle);
      const auto wa =
          denominator < 1e-7 ? 1 - t : std::sin((1 - t) * angle) / denominator;
      const auto wb =
          denominator < 1e-7 ? t : std::sin(t * angle) / denominator;
      for (size_t i = 0; i < 4; ++i)
        out[i] =
            wa * values[a * stride + i] + wb * sign * values[b * stride + i];
    } else
      for (size_t i = 0; i < components; ++i)
        out[i] = (1 - t) * values[a * stride + i] + t * values[b * stride + i];
    if (rotation) {
      double length{};
      for (auto v : out)
        length += v * v;
      if (length < 1e-20)
        invalid("glTF animation contains a zero quaternion");
      for (auto &v : out)
        v /= std::sqrt(length);
    }
    return out;
  }
};
Values encoded(const Track &track, double time, const Values &reference = {}) {
  auto value = track.at(time);
  if (!track.rotation)
    return value;
  FbxAMatrix matrix;
  matrix.SetQ(FbxQuaternion(value[0], value[1], value[2], value[3]));
  const auto r = matrix.GetR();
  Values result{r[0], r[1], r[2]};
  if (!reference.empty())
    for (size_t i = 0; i < 3; ++i)
      result[i] += 360 * std::round((reference[i] - result[i]) / 360);
  return result;
}
double error(const Track &track, double time, const Values &left,
             const Values &right, double fraction) {
  const auto expected = track.at(time);
  if (track.rotation) {
    FbxAMatrix matrix;
    matrix.SetR(FbxVector4(left[0] + (right[0] - left[0]) * fraction,
                           left[1] + (right[1] - left[1]) * fraction,
                           left[2] + (right[2] - left[2]) * fraction));
    const auto q = matrix.GetQ();
    double dot{};
    for (size_t i = 0; i < 4; ++i)
      dot += q[static_cast<int>(i)] * expected[i];
    return 2 * std::acos(std::clamp(std::abs(dot), 0.0, 1.0));
  }
  double worst{};
  for (size_t i = 0; i < left.size(); ++i)
    worst =
        std::max(worst, std::abs(expected[i] -
                                 (left[i] + (right[i] - left[i]) * fraction)) /
                            std::max(1.0, std::abs(expected[i])));
  return worst;
}
struct Sample {
  double time;
  Values value;
};
void refine(Scene &s, const Track &track, double a, const Values &left,
            double b, const Values &right, std::vector<Sample> &out,
            unsigned depth) {
  s.check();
  bool split{};
  for (const double fraction : {.25, .5, .75})
    if (error(track, a + (b - a) * fraction, left, right, fraction) > 1e-5) {
      split = true;
      break;
    }
  if (split) {
    if (depth >= 20 || b - a < 1e-9)
      throw skeletal::Failure(
          ErrorCode::Unsupported,
          "Cannot represent glTF animation within the FBX bake tolerance");
    const double mid = (a + b) / 2;
    const auto middle = encoded(track, mid, left);
    refine(s, track, a, left, mid, middle, out, depth + 1);
    const auto midpoint = out.back().value;
    const auto aligned = encoded(track, b, midpoint);
    refine(s, track, mid, midpoint, b, aligned, out, depth + 1);
  } else {
    if (out.size() >= s.limits.elements)
      throw skeletal::Failure(ErrorCode::ResourceLimit,
                              "glTF animation bake sample budget exceeded");
    s.reserve(64 + right.size() * sizeof(double));
    out.push_back({b, right});
  }
}
void curve(FbxAnimCurve *curve, const std::vector<Sample> &samples,
           size_t component, bool step, double scale = 1) {
  if (!curve)
    invalid("Cannot create FBX animation curve");
  curve->KeyModifyBegin();
  for (const auto &sample : samples) {
    FbxTime time;
    time.SetSecondDouble(sample.time);
    const auto key = curve->KeyAdd(time);
    curve->KeySetValue(key,
                       static_cast<float>(sample.value[component] * scale));
    curve->KeySetInterpolation(key,
                               step ? FbxAnimCurveDef::eInterpolationConstant
                                    : FbxAnimCurveDef::eInterpolationLinear);
    if (step)
      curve->KeySetConstantMode(key, FbxAnimCurveDef::eConstantStandard);
  }
  curve->KeyModifyEnd();
}
} // namespace
void gltf_animations(Scene &s, const cgltf_data &data, GltfAccessors &access,
                     const std::vector<FbxNode *> &nodes,
                     const GltfMorphs &morphs) {
  std::uint64_t samples_written{};
  for (size_t ai = 0; ai < data.animations_count; ++ai) {
    s.check();
    const auto &animation = data.animations[ai];
    auto *stack = FbxAnimStack::Create(
        s.scene, animation.name ? animation.name
                                : ("Animation_" + std::to_string(ai)).c_str());
    auto *layer = FbxAnimLayer::Create(s.scene, "BaseLayer");
    stack->AddMember(layer);
    double start = std::numeric_limits<double>::infinity(), end = -start;
    std::set<std::pair<const cgltf_node *, cgltf_animation_path_type>> targets;
    for (size_t ci = 0; ci < animation.channels_count; ++ci) {
      const auto &channel = animation.channels[ci];
      if (!channel.target_node || !channel.sampler)
        invalid("Missing glTF animation target/sampler");
      if (!targets.insert({channel.target_node, channel.target_path}).second)
        invalid("Duplicate glTF animation target");
      if (channel.target_node->has_matrix)
        invalid("Animated glTF node must use TRS rather than matrix");
      const auto &sampler = *channel.sampler;
      if (!sampler.input || sampler.input->type != cgltf_type_scalar ||
          !sampler.output || !sampler.input->count)
        invalid("Invalid glTF animation accessor");
      const auto &times = access.read(sampler.input),
                 &values = access.read(sampler.output);
      for (size_t i = 0; i < times.size(); ++i)
        if (times[i] < 0 || (i && times[i] <= times[i - 1]))
          invalid("glTF animation times must increase");
      const bool rotation =
                     channel.target_path == cgltf_animation_path_type_rotation,
                 weights =
                     channel.target_path == cgltf_animation_path_type_weights;
      const size_t multiplier =
          sampler.interpolation == cgltf_interpolation_type_cubic_spline ? 3
                                                                         : 1;
      size_t components = rotation ? 4 : 3;
      if (weights) {
        const auto it = morphs.find(channel.target_node);
        if (it == morphs.end())
          invalid("glTF weights animation has no morph target");
        components = it->second.size();
        if (sampler.output->type != cgltf_type_scalar)
          invalid("glTF morph animation requires scalar values");
      } else if (sampler.output->type !=
                 (rotation ? cgltf_type_vec4 : cgltf_type_vec3))
        invalid("Invalid glTF animation value shape");
      if (!components ||
          values.size() != times.size() * components * multiplier)
        invalid("glTF animation value count mismatch");
      if (sampler.interpolation == cgltf_interpolation_type_cubic_spline &&
          times.size() < 2)
        invalid("Cubic glTF animation requires two keys");
      if (sampler.interpolation != cgltf_interpolation_type_step &&
          sampler.interpolation != cgltf_interpolation_type_linear &&
          sampler.interpolation != cgltf_interpolation_type_cubic_spline)
        invalid("Unknown glTF interpolation");
      Track track{sampler, times, values, components, rotation};
      std::vector<Sample> samples{
          {times.front(), encoded(track, times.front())}};
      const bool step = sampler.interpolation == cgltf_interpolation_type_step;
      for (size_t i = 1; i < times.size(); ++i) {
        // Copy before recursive appends: vector growth invalidates references.
        const auto previous = samples.back().value;
        const auto next = encoded(track, times[i], previous);
        if (step) {
          s.reserve(64 + next.size() * sizeof(double));
          samples.push_back({times[i], next});
        } else
          refine(s, track, times[i - 1], previous, times[i], next, samples, 0);
      }
      samples_written += samples.size() * components;
      if (samples_written > s.limits.elements)
        throw skeletal::Failure(ErrorCode::ResourceLimit,
                                "glTF animation key budget exceeded");
      auto *node = nodes[static_cast<size_t>(channel.target_node - data.nodes)];
      if (weights)
        for (size_t i = 0; i < components; ++i)
          for (auto *morph : morphs.at(channel.target_node)[i])
            curve(morph->DeformPercent.GetCurve(layer, true), samples, i, step,
                  100);
      else {
        auto *property =
            channel.target_path == cgltf_animation_path_type_translation
                ? &node->LclTranslation
            : rotation ? &node->LclRotation
            : channel.target_path == cgltf_animation_path_type_scale
                ? &node->LclScaling
                : nullptr;
        if (!property)
          invalid("Unsupported glTF animation channel");
        const char *names[]{FBXSDK_CURVENODE_COMPONENT_X,
                            FBXSDK_CURVENODE_COMPONENT_Y,
                            FBXSDK_CURVENODE_COMPONENT_Z};
        for (size_t i = 0; i < 3; ++i)
          curve(property->GetCurve(layer, names[i], true), samples, i, step);
      }
      start = std::min(start, static_cast<double>(times.front()));
      end = std::max(end, static_cast<double>(times.back()));
    }
    if (!std::isfinite(start))
      invalid("glTF animation has no channels");
    FbxTime begin, finish;
    begin.SetSecondDouble(start);
    finish.SetSecondDouble(end);
    FbxTimeSpan span(begin, finish);
    stack->SetLocalTimeSpan(span);
    stack->SetReferenceTimeSpan(span);
  }
  s.metadata["animation_bake_keys"] = static_cast<qint64>(samples_written);
  s.metadata["animation_bake_tolerance"] = 1e-5;
  if (samples_written)
    s.loss("gltf_animation_bake",
           "Quaternion and cubic animation is adaptively baked to FBX "
           "Euler/linear curves (local angular/relative component tolerance "
           "1e-5); original clip times and independent stacks are retained.");
}
} // namespace formaview::scene_export
