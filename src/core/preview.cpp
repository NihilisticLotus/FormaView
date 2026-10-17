#include "formaview/preview.hpp"
#include <algorithm>
#include <cmath>

namespace formaview::preview {
namespace {
using skeletal::Vec3;
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 transform(const Matrix4 &m, Vec3 p) {
  return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3],
          m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
          m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}
void check(bool ok, const char *message) {
  if (!ok)
    throw skeletal::Failure(ErrorCode::InvalidArgument, message);
}
} // namespace
SkinEvaluator::SkinEvaluator(const skeletal::ActorAsset &source)
    : source_(source) {
  check(source.mesh.has_value(), "Mesh required");
  for (const auto &bone : source.bones) {
    check(bone.bind_world.has_value(), "Verified bind matrix required");
    inverse_bind_.push_back(skeletal::inverse(*bone.bind_world));
  }
  matrices_.resize(source.bones.size());
  points_.resize(source.mesh->points.size());
  frame_.corners.resize(source.mesh->faces.size() * 3);
}
const MeshFrame &SkinEvaluator::evaluate(std::span<const Matrix4> world,
                                         std::string_view uv_set,
                                         WeightPolicy policy,
                                         std::stop_token stop) {
  const auto &source = source_;
  const auto &mesh = *source.mesh;
  check(world.size() == source.bones.size(), "Pose palette mismatch");
  auto &matrices = matrices_;
  for (std::size_t i = 0; i < world.size(); ++i) {
    matrices[i] = skeletal::multiply(world[i], inverse_bind_[i]);
  }
  const std::vector<skeletal::Vec2> *uv = nullptr;
  if (!uv_set.empty()) {
    auto it = mesh.extra_uvs.find(std::string(uv_set));
    check(it != mesh.extra_uvs.end(), "Unknown UV set");
    uv = &it->second;
    check(uv->size() == mesh.wedges.size(), "UV domain mismatch");
  }
  auto &points = points_;
  check(mesh.point_weights.size() == points.size(),
        "Weight index domain mismatch");
  for (std::size_t i = 0; i < points.size(); ++i) {
    if ((i & 1023) == 0 && stop.stop_requested())
      throw skeletal::Failure(ErrorCode::Cancelled,
                              "Mesh evaluation cancelled");
    Vec3 result{};
    double sum = 0;
    for (auto wi : mesh.point_weights[i]) {
      check(wi < mesh.weights.size(), "Weight index out of range");
      const auto &w = mesh.weights[wi];
      check(w.point == i && w.bone < matrices.size() && std::isfinite(w.value),
            "Invalid source influence");
      const auto p = transform(matrices[w.bone], mesh.points[i]);
      result.x += p.x * w.value;
      result.y += p.y * w.value;
      result.z += p.z * w.value;
      sum += w.value;
    }
    if (!mesh.point_weights[i].empty()) {
      if (policy == WeightPolicy::Normalize) {
        check(std::abs(sum) > 1e-15, "Cannot normalize zero-sum weights");
        result.x /= sum;
        result.y /= sum;
        result.z /= sum;
      }
      points[i] = result;
    } else
      points[i] = mesh.points[i];
  }
  auto &result = frame_;
  std::size_t corner = 0;
  for (const auto &face : mesh.faces)
    for (auto wi : face.wedges) {
      check(wi < mesh.wedges.size(), "Wedge out of range");
      const auto &w = mesh.wedges[wi];
      check(w.point < points.size(), "Point out of range");
      result.corners[corner++] = {points[w.point], uv ? (*uv)[wi] : w.uv,
                                  w.point, wi};
    }
  return result;
}
MeshFrame evaluate_mesh(const skeletal::ActorAsset &source,
                        std::span<const Matrix4> world, std::string_view uv_set,
                        WeightPolicy policy, std::stop_token stop) {
  SkinEvaluator evaluator(source);
  return evaluator.evaluate(world, uv_set, policy, stop);
}
WeightIndex::WeightIndex(const skeletal::Mesh &source, std::size_t bones)
    : by_bone_(bones), has_data_(source.points.size()) {
  for (const auto &w : source.weights) {
    check(w.bone < bones && w.point < has_data_.size() &&
              std::isfinite(w.value),
          "Invalid source weight");
    by_bone_[w.bone].emplace_back(w.point, w.value);
    has_data_[w.point] = true;
  }
}
std::vector<WeightValue> WeightIndex::select(std::size_t bone) const {
  check(bone < by_bone_.size(), "Bone out of range");
  std::vector<WeightValue> result(has_data_.size());
  for (std::size_t i = 0; i < result.size(); ++i)
    result[i].has_data = has_data_[i];
  for (const auto &[point, value] : by_bone_[bone])
    result[point].sum += value;
  return result;
}
std::optional<Hit> pick(const MeshFrame &frame, Vec3 origin, Vec3 direction) {
  check(frame.corners.size() % 3 == 0, "Triangle corner domain required");
  const auto length = std::sqrt(dot(direction, direction));
  check(std::isfinite(length) && length > 0, "Finite nonzero ray required");
  direction = {direction.x / length, direction.y / length,
               direction.z / length};
  std::optional<Hit> best;
  for (std::size_t i = 0; i < frame.corners.size(); i += 3) {
    const auto a = frame.corners[i].position;
    const auto e1 = sub(frame.corners[i + 1].position, a),
               e2 = sub(frame.corners[i + 2].position, a);
    const auto p = cross(direction, e2);
    const auto det = dot(e1, p);
    if (std::abs(det) < 1e-12)
      continue;
    const auto t = sub(origin, a);
    const auto u = dot(t, p) / det;
    const auto q = cross(t, e1);
    const auto v = dot(direction, q) / det;
    const auto distance = dot(e2, q) / det;
    if (u < 0 || v < 0 || u + v > 1 || distance < 0 ||
        (best && distance >= best->distance))
      continue;
    const std::array<double, 3> bary{1 - u - v, u, v};
    const auto corner = static_cast<std::size_t>(
        std::max_element(bary.begin(), bary.end()) - bary.begin());
    const auto &vertex = frame.corners[i + corner];
    best = Hit{i / 3, corner, vertex.point, vertex.wedge, distance};
  }
  return best;
}
} // namespace formaview::preview
