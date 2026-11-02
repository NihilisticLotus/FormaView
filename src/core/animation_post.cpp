#include "formaview/animation_post.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <sstream>

namespace formaview::skeletal {
namespace {

double determinant(const Matrix4 &m) {
  const auto at = [&m](int r, int c) { return m[static_cast<std::size_t>(r) * 4 + c]; };
  double result = 0;
  for (int c = 0; c < 4; ++c) {
    const double sign = (c % 2 == 0) ? 1.0 : -1.0;
    // Cofactor expansion along the first row with 3x3 minors.
    double m3[3][3];
    for (int r = 1; r < 4; ++r) {
      int target = 0;
      for (int cc = 0; cc < 4; ++cc) {
        if (cc == c)
          continue;
        m3[r - 1][target++] = at(r, cc);
      }
    }
    const double minor =
        m3[0][0] * (m3[1][1] * m3[2][2] - m3[1][2] * m3[2][1]) -
        m3[0][1] * (m3[1][0] * m3[2][2] - m3[1][2] * m3[2][0]) +
        m3[0][2] * (m3[1][0] * m3[2][1] - m3[1][1] * m3[2][0]);
    result += sign * at(0, c) * minor;
  }
  return result;
}

bool invertible(const Transform &transform) {
  // A TRS transform is singular exactly when any scale component is zero
  // (rotation is always orthogonal); the determinant check stays as the
  // general guard for degenerate inputs.
  const auto m = matrix(transform);
  const double det = determinant(m);
  return std::isfinite(det) && std::abs(det) > 1e-12;
}

// Iterative validation/composition avoids recursion on source-controlled chains.
bool recomputeWorlds(const ActorAsset &asset, Pose &pose) {
  std::vector<Matrix4> worlds(asset.bones.size());
  std::vector<unsigned char> state(asset.bones.size());
  for (std::size_t start=0; start<asset.bones.size(); ++start) {
    if (state[start]==2) continue;
    std::vector<std::size_t> path;
    std::optional<std::size_t> at=start;
    while (at) {
      if (*at>=asset.bones.size() || state[*at]==1) return false;
      if (state[*at]==2) break;
      state[*at]=1; path.push_back(*at); at=asset.bones[*at].parent;
    }
    for (auto it=path.rbegin();it!=path.rend();++it) {
      const auto local=matrix(pose.local[*it]);
      const auto parent=asset.bones[*it].parent;
      worlds[*it]=parent ? multiply(worlds[*parent],local) : local;
      state[*it]=2;
    }
  }
  pose.world=std::move(worlds);
  return true;
}

void append(std::string &out, double value) {
  char buffer[40];
  std::snprintf(buffer, sizeof(buffer), "%.17g", value);
  out += buffer;
}
void append(std::string &out, const Vec3 &v) {
  append(out, v.x);
  out += ',';
  append(out, v.y);
  out += ',';
  append(out, v.z);
}
void append(std::string &out, const Quat &q) {
  append(out, q.x);
  out += ',';
  append(out, q.y);
  out += ',';
  append(out, q.z);
  out += ',';
  append(out, q.w);
}
void append(std::string &out, const Transform &t) {
  append(out, t.translation);
  out += ';';
  append(out, t.rotation);
  out += ';';
  append(out, t.scale);
}
void append(std::string &out, const Matrix4 &m) {
  for (double value : m) {
    append(out, value);
    out += ',';
  }
}

} // namespace

RootLockResult evaluate_root_base(const ActorAsset &asset,
                                  std::size_t clip,
                                  const RootLockRequest &request,
                                  std::stop_token stop) {
  RootLockResult result;
  result.status = RootLockStatus::InvalidRoot;
  if (request.root >= asset.bones.size())
    return result;
  switch (request.kind) {
  case RootBaseKind::AnimationFirstFrame: {
    if (clip >= asset.sequences.size())
      return result;
    const auto &time = asset.sequences[clip].time;
    const auto pose = evaluate(asset, clip, time.source_start_seconds, stop);
    result.base_local = pose.local[request.root];
    result.base_time = time.source_start_seconds;
    break;
  }
  case RootBaseKind::ReferencePose: {
    if (!request.reference_local) {
      result.status = RootLockStatus::MissingReference;
      result.note =
          "ReferencePose requires explicit reference data; the first frame "
          "is never substituted.";
      return result;
    }
    result.base_local = *request.reference_local;
    break;
  }
  case RootBaseKind::ZeroWorld:
    result.base_local = Transform{};
    result.base_local.rotation = Quat{};
    result.base_local.scale = {1, 1, 1};
    break;
  }
  if (!invertible(result.base_local)) {
    result.status = RootLockStatus::NonInvertibleBase;
    result.note = "Base transform is singular; the lock cannot apply.";
    return result;
  }
  result.status = RootLockStatus::Effective;
  return result;
}

RootLockResult apply_root_lock(const Pose &source, const ActorAsset &asset,
                               const RootLockRequest &request,
                               const Transform &base_local) {
  RootLockResult result;
  result.base_local = base_local;
  result.pose = source; // Failure paths keep the source pose verbatim.
  if (request.root >= asset.bones.size()) {
    result.status = RootLockStatus::InvalidRoot;
    return result;
  }
  if (!invertible(base_local)) {
    result.status = RootLockStatus::NonInvertibleBase;
    result.note = "Base transform is singular; source motion is kept.";
    return result;
  }
  if (source.local.size()!=asset.bones.size() || source.world.size()!=asset.bones.size()) {
    result.status=RootLockStatus::InvalidPose;
    result.note="Pose does not cover the skeleton; source motion is kept.";
    return result;
  }
  auto locked = source.local[request.root];
  if (request.lock_translation)
    locked.translation = base_local.translation;
  if (request.lock_rotation)
    locked.rotation = base_local.rotation;
  // Scale always keeps the source value (proposal pending approval).
  result.pose.local = source.local;
  result.pose.local[request.root] = locked;
  if (!recomputeWorlds(asset,result.pose)) {
    result.pose=source;
    result.status=RootLockStatus::InvalidPose;
    result.note="Invalid or cyclic parent chain; source motion is kept.";
    return result;
  }
  result.status = RootLockStatus::Effective;
  result.note = "Base applied to root local translation/rotation; scale kept "
                "from source; session preview only.";
  return result;
}

std::string actor_target_digest(const ActorAsset &asset, std::stop_token stop) {
  std::string canonical = "actor-target-v2\n";
  canonical += "bones ";
  canonical += std::to_string(asset.bones.size());
  canonical += '\n';
  for (std::size_t i = 0; i < asset.bones.size(); ++i) {
    const auto &bone = asset.bones[i];
    canonical += std::to_string(i);
    canonical += ' ';
    canonical += std::to_string(bone.source.original_name.size());
    canonical += ':';
    canonical += bone.source.original_name;
    canonical += ' ';
    canonical += bone.parent ? std::to_string(*bone.parent) : "-1";
    canonical += ' ';
    append(canonical, bone.default_local);
    canonical += bone.default_local_valid ? " valid" : " placeholder";
    if (bone.bind_world) {
      canonical += " bind ";
      append(canonical, *bone.bind_world);
    }
    canonical += '\n';
  }
  if (asset.mesh) {
    const auto &mesh = *asset.mesh;
    canonical += "points ";
    canonical += std::to_string(mesh.points.size());
    canonical += "\nwedges ";
    canonical += std::to_string(mesh.wedges.size());
    canonical += "\nfaces ";
    canonical += std::to_string(mesh.faces.size());
    canonical += "\nweights ";
    canonical += std::to_string(mesh.weights.size());
    canonical += '\n';
    for (const auto &weight : mesh.weights) {
      canonical += std::to_string(weight.point);
      canonical += ':';
      canonical += std::to_string(weight.bone);
      canonical += ':';
      append(canonical, weight.value);
      canonical += '\n';
    }
  } else {
    canonical += "no-mesh\n";
  }
  return sha256(
      std::span<const std::byte>(
          reinterpret_cast<const std::byte *>(canonical.data()),
          canonical.size()),
      stop);
}

bool MappingCacheKey::operator==(const MappingCacheKey &other) const {
  return source_digest == other.source_digest &&
         target_digest == other.target_digest &&
         coordinates_version == other.coordinates_version &&
         policy_version == other.policy_version;
}

MappingCache::MappingCache(std::size_t max_entries)
    : max_entries_(max_entries == 0 ? 1 : max_entries) {}

const CompareReport *MappingCache::find(const MappingCacheKey &key) {
  const auto found = entries_.find(key);
  if (found == entries_.end()) {
    ++misses_;
    return nullptr;
  }
  ++hits_;
  found->second.used = ++clock_;
  return &found->second.report;
}

void MappingCache::store(const MappingCacheKey &key, CompareReport report) {
  const auto found = entries_.find(key);
  if (found != entries_.end()) {
    found->second.report = std::move(report);
    found->second.used = ++clock_;
    return;
  }
  if (entries_.size() >= max_entries_) {
    auto oldest = entries_.begin();
    for (auto entry = entries_.begin(); entry != entries_.end(); ++entry)
      if (entry->second.used < oldest->second.used)
        oldest = entry;
    entries_.erase(oldest);
  }
  entries_.emplace(key, Entry{std::move(report), ++clock_});
}

std::size_t MappingCache::size() const { return entries_.size(); }
std::uint64_t MappingCache::hits() const { return hits_; }
std::uint64_t MappingCache::misses() const { return misses_; }

TransformDelta compare_transforms(const std::vector<Transform> &a,
                                  const std::vector<Transform> &b,
                                  const std::vector<BonePair> &mapping) {
  TransformDelta delta;
  for (const auto &pair : mapping) {
    if (pair.a >= a.size() || pair.b >= b.size())
      continue;
    const auto &ta = a[pair.a];
    const auto &tb = b[pair.b];
    const double translation =
        std::sqrt((ta.translation.x - tb.translation.x) *
                      (ta.translation.x - tb.translation.x) +
                  (ta.translation.y - tb.translation.y) *
                      (ta.translation.y - tb.translation.y) +
                  (ta.translation.z - tb.translation.z) *
                      (ta.translation.z - tb.translation.z));
    const double dot = std::abs(ta.rotation.x * tb.rotation.x +
                                ta.rotation.y * tb.rotation.y +
                                ta.rotation.z * tb.rotation.z +
                                ta.rotation.w * tb.rotation.w);
    const double rotation = std::max(0.0, 1.0 - dot);
    const double scale =
        std::max({std::abs(ta.scale.x - tb.scale.x),
                  std::abs(ta.scale.y - tb.scale.y),
                  std::abs(ta.scale.z - tb.scale.z)});
    if (translation > delta.translation) {
      delta.translation = translation;
      delta.worst_bone = pair.a;
    }
    if (rotation > delta.rotation) {
      delta.rotation = rotation;
      delta.worst_bone = pair.a;
    }
    if (scale > delta.scale) {
      delta.scale = scale;
      delta.worst_bone = pair.a;
    }
  }
  return delta;
}

} // namespace formaview::skeletal
