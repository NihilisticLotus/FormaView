#pragma once
#include "formaview/contracts.hpp"
#include "formaview/skeletal.hpp"
#include "formaview/skeleton_compare.hpp"
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace formaview::skeletal {

// Root lock bases per the reviewed contract. AnimationFirstFrame evaluates
// the real clip start (never the playhead). ReferencePose requires explicit
// reference data (e.g. a paired PSK); it is never substituted by a frame.
// ZeroWorld is the zero translation/identity rotation base in the declared
// skeleton coordinate space.
enum class RootBaseKind { AnimationFirstFrame, ReferencePose, ZeroWorld };

enum class RootLockStatus {
  Effective,
  MissingReference, // ReferencePose without explicit reference data.
  NonInvertibleBase, // Base transform is singular; lock cannot apply.
  InvalidRoot,
  InvalidPose, // Incomplete pose or invalid/cyclic parent chain.
};

struct RootLockRequest {
  std::uint32_t root{};
  RootBaseKind kind{RootBaseKind::AnimationFirstFrame};
  std::optional<Transform> reference_local; // Required for ReferencePose.
  // Proposal pending approval: translation + rotation locked, scale kept.
  bool lock_translation{true};
  bool lock_rotation{true};
};

struct RootLockResult {
  RootLockStatus status{RootLockStatus::InvalidRoot};
  Pose pose;          // Processed pose; equals the source on failure.
  Transform base_local{};
  std::string note;   // Rule source shown beside the readouts.
  double base_time{}; // Real seconds the base was taken at, when applicable.
};

// Evaluates the base local transform for the request. The start pose for
// AnimationFirstFrame is evaluated at the clip's real start time.
[[nodiscard]] RootLockResult evaluate_root_base(const ActorAsset &asset,
                                                std::size_t clip,
                                                const RootLockRequest &request,
                                                std::stop_token stop = {});

// Applies the lock to a source pose: the root's local translation/rotation
// are replaced by the base (scale always keeps the source value), and the
// world chain is recomputed. The source pose is never mutated; lock-off is
// simply using the unmodified source at the same time.
[[nodiscard]] RootLockResult apply_root_lock(const Pose &source,
                                             const ActorAsset &asset,
                                             const RootLockRequest &request,
                                             const Transform &base_local);

// Stable digest over the target-relevant subset of an ActorAsset: bone
// identity/hierarchy/default transforms, bind matrices and the complete raw
// weight list and point/wedge/face counts. Used for before/after
// immutability assertions and as a mapping-cache fingerprint.
[[nodiscard]] std::string actor_target_digest(const ActorAsset &asset,
                                              std::stop_token stop = {});

// Mapping cache keyed by dependency fingerprints per the reviewed contract:
// source/target digests, coordinate convention and policy versions. Different
// dependencies must re-verify; per-frame lookups only read stored results.
struct MappingCacheKey {
  std::string source_digest, target_digest;
  int coordinates_version{};
  std::uint32_t policy_version{};
  bool operator==(const MappingCacheKey &other) const;
};

} // namespace formaview::skeletal

template <>
struct std::hash<formaview::skeletal::MappingCacheKey> {
  size_t operator()(
      const formaview::skeletal::MappingCacheKey &key) const noexcept {
    size_t seed = std::hash<std::string>()(key.source_digest);
    seed ^= std::hash<std::string>()(key.target_digest) +
            0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    seed ^= std::hash<int>()(key.coordinates_version) +
            0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    seed ^= std::hash<std::uint32_t>()(key.policy_version) +
            0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    return seed;
  }
};

namespace formaview::skeletal {

class MappingCache final {
public:
  explicit MappingCache(std::size_t max_entries = 16);
  // The returned pointer stays valid until the next store/eviction.
  [[nodiscard]] const CompareReport *find(const MappingCacheKey &key);
  void store(const MappingCacheKey &key, CompareReport report);
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::uint64_t hits() const;
  [[nodiscard]] std::uint64_t misses() const;

private:
  std::size_t max_entries_;
  std::uint64_t hits_{}, misses_{};
  struct Entry {
    CompareReport report;
    std::uint64_t used{};
  };
  std::unordered_map<MappingCacheKey, Entry> entries_;
  std::uint64_t clock_{};
};

// Transform comparison for the "directly applicable" dimension: local
// transforms of matched pairs under explicit conventions and configurable
// tolerance. Rotation delta is 1 - |dot| of the quaternions (0 = identical).
struct TransformDelta {
  double translation{}, rotation{}, scale{};
  std::uint32_t worst_bone{};
};
[[nodiscard]] TransformDelta
compare_transforms(const std::vector<Transform> &a,
                   const std::vector<Transform> &b,
                   const std::vector<BonePair> &mapping);

} // namespace formaview::skeletal
