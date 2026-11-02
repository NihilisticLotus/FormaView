// M07 animation post-processing tests: root lock bases/states, target
// immutability digest, dependency-keyed mapping cache and transform
// comparison. Synthetic cases run without arguments (ctest-safe); a real
// PSA path adds sample-backed checks:
//   animation_post_tests <psa> <output.json>
#include "formaview/animation_post.hpp"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>

namespace {
using formaview::skeletal::ActorAsset;
using formaview::skeletal::BoneRef;
using formaview::skeletal::CompareInput;
using formaview::skeletal::CompareReport;
using formaview::skeletal::MappingCache;
using formaview::skeletal::MappingCacheKey;
using formaview::skeletal::Pose;
using formaview::skeletal::RootBaseKind;
using formaview::skeletal::RootLockRequest;
using formaview::skeletal::RootLockStatus;
using formaview::skeletal::Transform;

int failures = 0;
void check(const std::string &name, bool condition,
           const std::string &detail = {}) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL " << name << (detail.empty() ? "" : ": " + detail)
              << "\n";
  } else {
    std::cout << "pass " << name << "\n";
  }
}

// Synthetic chain: root -> spine -> head with root motion along X.
ActorAsset syntheticAsset() {
  ActorAsset asset;
  asset.animation = true;
  formaview::skeletal::Bone root, spine, head;
  root.source.original_name = "root";
  root.default_local.translation = {0, 0, 0};
  spine.source.original_name = "spine";
  spine.parent = 0;
  spine.default_local.translation = {0, 30, 0};
  head.source.original_name = "head";
  head.parent = 1;
  head.default_local.translation = {0, 30, 0};
  asset.bones = {root, spine, head};
  formaview::skeletal::Sequence sequence;
  sequence.group = "move";
  sequence.frame_count = 5;
  sequence.fps = 10.0;
  sequence.time.source_start_seconds = 0.0;
  sequence.time.source_end_seconds = 0.4;
  asset.sequences = {sequence};
  // Frame-major bone-minor keys; uniform delta_time is required by the
  // evaluator. Root slides along +X across the five frames.
  for (int frame = 0; frame < 5; ++frame) {
    const double rootX[5] = {0, 1, 2, 3, 4};
    asset.keys.push_back({{rootX[frame], 0, 0}, {}, 0.1});
    asset.keys.push_back({{0, 30, 0}, {}, 0.1});
    asset.keys.push_back({{0, 30, 0}, {}, 0.1});
  }
  return asset;
}

double component(const formaview::skeletal::Vec3 &v, std::size_t i) {
  return i == 0 ? v.x : i == 1 ? v.y : v.z;
}
double chainError(const ActorAsset &asset, const Pose &pose) {
  double worst{};
  for (std::size_t i = 0; i < asset.bones.size(); ++i) {
    const auto &bone = asset.bones[i];
    if (!bone.parent)
      continue;
    const auto composed = formaview::skeletal::multiply(
        pose.world[*bone.parent], formaview::skeletal::matrix(pose.local[i]));
    for (std::size_t e = 0; e < 16; ++e)
      worst = std::max(worst, std::abs(composed[e] - pose.world[i][e]));
  }
  return worst;
}

void runSynthetic() {
  const auto asset = syntheticAsset();

  // First-frame base: root local at t=0 is (0,0,0); at t=0.3 it is (3,0,0).
  const auto start = formaview::skeletal::evaluate(asset, 0, 0.0);
  const auto moving = formaview::skeletal::evaluate(asset, 0, 0.3);
  double motion = 0;
  for (std::size_t e = 0; e < 3; ++e)
    motion += std::abs(component(moving.local[0].translation, e) -
                       component(start.local[0].translation, e));
  check("fixture.root_motion", motion > 1.0);
  check("fixture.chain", chainError(asset, moving) < 1e-12);

  RootLockRequest request;
  request.root = 0;
  request.kind = RootBaseKind::AnimationFirstFrame;
  const auto base = formaview::skeletal::evaluate_root_base(asset, 0, request);
  check("base.first_frame_effective", base.status == RootLockStatus::Effective);
  check("base.first_frame_time", base.base_time == 0.0);
  double baseNorm = 0;
  for (std::size_t e = 0; e < 3; ++e)
    baseNorm += std::abs(component(base.base_local.translation, e));
  check("base.first_frame_zero_translation", baseNorm == 0.0);

  const auto locked = formaview::skeletal::apply_root_lock(
      moving, asset, request, base.base_local);
  check("lock.effective", locked.status == RootLockStatus::Effective);
  double lockedDelta = 0, sourceX = 0;
  for (std::size_t e = 0; e < 3; ++e) {
    lockedDelta += std::abs(component(locked.pose.local[0].translation, e) -
                            component(base.base_local.translation, e));
    sourceX += std::abs(component(moving.local[0].translation, e) -
                        component(start.local[0].translation, e));
  }
  check("lock.root_local_from_base", lockedDelta == 0.0);
  check("lock.source_untouched", sourceX > 1.0);
  check("lock.chain_after", chainError(asset, locked.pose) < 1e-12);
  // Descendants follow the locked root instead of the source motion.
  double headShift = 0;
  for (std::size_t e = 0; e < 16; ++e)
    headShift = std::max(headShift,
                         std::abs(locked.pose.world[2][e] - moving.world[2][e]));
  check("lock.descendants_follow", headShift > 1.0);

  // Zero base.
  request.kind = RootBaseKind::ZeroWorld;
  const auto zeroBase = formaview::skeletal::evaluate_root_base(asset, 0, request);
  const auto zeroLocked = formaview::skeletal::apply_root_lock(
      moving, asset, request, zeroBase.base_local);
  double zeroNorm = 0;
  for (std::size_t e = 0; e < 3; ++e)
    zeroNorm += std::abs(component(zeroLocked.pose.local[0].translation, e));
  check("lock.zero_world",
        zeroBase.status == RootLockStatus::Effective && zeroNorm == 0.0);

  // Missing reference: ReferencePose without data never falls back.
  request.kind = RootBaseKind::ReferencePose;
  const auto missing = formaview::skeletal::evaluate_root_base(asset, 0, request);
  check("lock.missing_reference",
        missing.status == RootLockStatus::MissingReference);

  // Non-invertible base (zero scale) keeps the source motion.
  Transform singular;
  singular.scale = {0, 1, 1};
  const auto nonInvertible = formaview::skeletal::apply_root_lock(
      moving, asset, request, singular);
  check("lock.non_invertible",
        nonInvertible.status == RootLockStatus::NonInvertibleBase &&
            &nonInvertible.pose.local[0] != &moving.local[0] &&
            nonInvertible.pose.local.size() == moving.local.size());

  // Invalid root index.
  request.root = 9;
  request.kind = RootBaseKind::AnimationFirstFrame;
  check("lock.invalid_root",
        formaview::skeletal::evaluate_root_base(asset, 0, request).status ==
            RootLockStatus::InvalidRoot);

  // Target digest: stable across copies, sensitive to weight mutation,
  // unchanged by evaluation and locking.
  auto targetA = syntheticAsset();
  auto targetB = syntheticAsset();
  const auto digestA = formaview::skeletal::actor_target_digest(targetA);
  const auto digestB = formaview::skeletal::actor_target_digest(targetB);
  check("digest.stable", digestA == digestB);
  (void)formaview::skeletal::evaluate(targetA, 0, 0.2);
  (void)formaview::skeletal::apply_root_lock(
      formaview::skeletal::evaluate(targetA, 0, 0.2), targetA, request,
      base.base_local);
  check("digest.unaffected_by_use",
        formaview::skeletal::actor_target_digest(targetA) == digestA);
  targetB.bones[1].default_local.translation.x += 0.5;
  check("digest.sensitive",
        formaview::skeletal::actor_target_digest(targetB) != digestA);
  {
    ActorAsset left, right;
    left.bones.resize(2); right.bones.resize(2);
    const std::string boundary = " -1 0,0,0;0,0,0,1;1,1,1 valid\n1 ";
    left.bones[0].source.original_name = "a" + boundary + "b";
    left.bones[1].source.original_name = "c";
    right.bones[0].source.original_name = "a";
    right.bones[1].source.original_name = "b" + boundary + "c";
    const auto leftDigest = formaview::skeletal::actor_target_digest(left);
    const auto rightDigest = formaview::skeletal::actor_target_digest(right);
    check("digest.embedded_record_boundary", leftDigest != rightDigest);
    MappingCache isolated(2);
    isolated.store({leftDigest, leftDigest, 1, 1}, CompareReport{});
    check("cache.distinct_names_miss", !isolated.find({leftDigest, rightDigest, 1, 1}));
  }

  {
    request.root=0;
    auto invalid=asset; invalid.bones[1].parent=999;
    const auto rejected=formaview::skeletal::apply_root_lock(moving,invalid,request,Transform{});
    check("lock.invalid_parent_rejected",rejected.status != RootLockStatus::Effective && rejected.pose.world==moving.world);
    auto shortPose=moving; shortPose.local.clear();
    const auto incomplete=formaview::skeletal::apply_root_lock(shortPose,asset,request,Transform{});
    check("lock.incomplete_pose_rejected",incomplete.status==RootLockStatus::InvalidPose && incomplete.pose.local.empty());
    invalid=asset; invalid.bones[0].parent=2;
    const auto cycle=formaview::skeletal::apply_root_lock(moving,invalid,request,Transform{});
    check("lock.cycle_rejected",cycle.status != RootLockStatus::Effective && cycle.pose.world==moving.world);
  }

  // Mapping cache: fingerprint hit, different-policy miss, LRU eviction and
  // counters. Per-frame lookups only read the stored report.
  MappingCache cache(2);
  MappingCacheKey key{digestA, digestA, 1, 1};
  CompareInput input;
  input.bones = {{0, "root", {}, true}};
  CompareReport report =
      formaview::skeletal::compare_skeletons(input, input);
  cache.store(key, report);
  const auto *hit = cache.find(key);
  check("cache.hit", hit && hit->hierarchy_matches);
  check("cache.identity", hit == cache.find(key));
  const auto before = cache.hits();
  (void)cache.find(key);
  check("cache.per_frame_read_only", cache.hits() == before + 1);
  check("cache.policy_miss",
        cache.find({digestA, digestA, 1, 2}) == nullptr &&
            cache.misses() == 1);
  cache.store({digestA, digestA, 1, 2}, report);
  cache.store({digestA, digestA, 2, 1}, report); // Evicts the LRU entry.
  check("cache.eviction",
        cache.size() == 2 && cache.find(key) == nullptr &&
            cache.find({digestA, digestA, 1, 2}) != nullptr);

  // Transform comparison: identical locals give zero deltas; a translation
  // and a rotation perturbation are reported at the worst bone.
  std::vector<Transform> left{Transform{}, Transform{}, Transform{}},
      right{Transform{}, Transform{}, Transform{}};
  right[1].translation = {0.5, 0, 0};
  right[2].rotation = {0.0, 0.0, 0.070710678118654755,
                       0.99749498660405445}; // ~8.13 degrees about Z.
  const std::vector<formaview::skeletal::BonePair> pairs{
      {0, 0, true, {}}, {1, 1, true, {}}, {2, 2, true, {}}};
  const auto identical =
      formaview::skeletal::compare_transforms(left, left, pairs);
  check("transforms.identical",
        identical.translation == 0 && identical.rotation == 0 &&
            identical.scale == 0);
  const auto delta =
      formaview::skeletal::compare_transforms(left, right, pairs);
  check("transforms.translation", std::abs(delta.translation - 0.5) < 1e-12);
  check("transforms.rotation",
        std::abs(delta.rotation - (1.0 - 0.99749498660405445)) < 1e-12);
}
} // namespace

int wmain(int argc, wchar_t **argv) {
  runSynthetic();
  if (argc == 3) {
    QJsonObject evidence;
    try {
      const auto asset = formaview::skeletal::read_actor(
          std::filesystem::path(argv[1]));
      evidence["bones"] = static_cast<int>(asset.bones.size());
      evidence["digest"] =
          QString::fromStdString(formaview::skeletal::actor_target_digest(asset));
      // Real root lock: first-frame base, mid-time lock, chain invariant.
      const auto start = formaview::skeletal::evaluate(
          asset, 0, asset.sequences[0].time.source_start_seconds);
      const auto &domain = asset.sequences[0].time;
      const double midTime =
          (domain.source_start_seconds + domain.source_end_seconds) / 2;
      const auto mid = formaview::skeletal::evaluate(asset, 0, midTime);
      RootLockRequest request;
      request.root = 0;
      request.kind = RootBaseKind::AnimationFirstFrame;
      const auto base = formaview::skeletal::evaluate_root_base(asset, 0, request);
      const auto locked =
          formaview::skeletal::apply_root_lock(mid, asset, request, base.base_local);
      double chain = 0, rootDelta = 0;
      for (std::size_t i = 0; i < asset.bones.size(); ++i) {
        if (!asset.bones[i].parent)
          continue;
        const auto composed = formaview::skeletal::multiply(
            locked.pose.world[*asset.bones[i].parent],
            formaview::skeletal::matrix(locked.pose.local[i]));
        for (std::size_t e = 0; e < 16; ++e)
          chain = std::max(chain,
                           std::abs(composed[e] - locked.pose.world[i][e]));
      }
      rootDelta = std::abs(locked.pose.local[0].translation.x -
                           base.base_local.translation.x) +
                  std::abs(locked.pose.local[0].translation.y -
                           base.base_local.translation.y) +
                  std::abs(locked.pose.local[0].translation.z -
                           base.base_local.translation.z);
      evidence["lock_status"] = static_cast<int>(base.status);
      evidence["locked_chain_max_error"] = chain;
      evidence["locked_root_vs_base_delta"] = rootDelta;
      evidence["start_pose_kept"] =
          start.local.size() == asset.bones.size();
    } catch (const std::exception &failure) {
      evidence["error"] = failure.what();
    }
    QFile output(QString::fromWCharArray(argv[2]));
    if (output.open(QIODevice::WriteOnly))
      output.write(QJsonDocument(evidence).toJson(QJsonDocument::Indented));
  }
  if (failures) {
    std::cerr << failures << " synthetic check(s) failed\n";
    return 1;
  }
  std::cout << "animation post tests complete\n";
  return 0;
}
