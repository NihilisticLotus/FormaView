#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace formaview::skeletal::transform {
using Vec3 = std::array<double, 3>;
using Quat = std::array<double, 4>; // x,y,z,w; normalized, sign invariant.
using Matrix = std::array<double, 16>; // column-major, affine, column vectors.
enum class Format { Fbx, Gltf, Actor };
enum class Verdict { Compatible, Incompatible, Insufficient, Cancelled };
enum class Reason { None, Identity, MissingEvidence, UnsupportedFormat,
  InvalidInput, MappingConflict, CoordinateConflict, EvaluationConflict,
  RestConflict, BindingConflict, BudgetExceeded, Cancelled };
struct Trs {
  Vec3 translation{};
  Quat rotation{0,0,0,1};
  Vec3 scale{1,1,1};
};
struct Evaluation {
  // Adapter supplies source-declared semantics, never guessed defaults.
  std::uint8_t rotationOrder{}, inheritance{};
  bool rotationSpace{true};
  Quat preRotation{0,0,0,1}, postRotation{0,0,0,1};
  Vec3 rotationPivot{}, scalingPivot{}, rotationOffset{}, scalingOffset{};
};
struct Coordinates {
  double metresPerUnit{};
  // Signed permutation of canonical axes; orientation includes handedness.
  std::array<int,3> axes{};
};
struct Node {
  std::uint32_t fileIndex{};
  std::optional<std::size_t> parent; // array offset, not fileIndex
  std::optional<Trs> restLocal;
  std::optional<Evaluation> evaluation;
  std::optional<std::size_t> inheritScaleParent; // full array domain
  bool syntheticRoot{}; // only the format reader may declare this
  std::optional<Matrix> restMatrix; // static glTF matrix; never decomposed
};
struct Binding {
  std::size_t joint{};
  // Source-declared true binding matrices, NOT node default matrices.
  Matrix bindWorld{}, geometryToBone{}, geometryWorld{};
  std::size_t instance{}; // full node-array domain, never inferred from joint
  std::uint8_t witness{}; // 0 = declared BindPose, 1 = cluster link record
};
struct NativeSkinBinding {
  std::size_t instance{}, joint{};
  Matrix inverseBind{}; // glTF format-native operator, not a claimed bind pose
};
// Diagnostic only: locates the first binding evidence behind Reason::BindingConflict.
// It never alters a verdict, a reason or any threshold. Indices are array offsets of the
// respective evidence; for MissingPeer the target fields name the expected (absent) key.
enum class BindingFault : std::uint8_t { None, ClosureResidual, CountMismatch, MissingPeer, OperatorResidual };
enum class BindingMatrix : std::uint8_t { None, BindWorld, GeometryToBone, GeometryWorld, InverseBind };
struct BindingDiagnostic {
  BindingFault fault{BindingFault::None};
  BindingMatrix matrix{BindingMatrix::None};
  bool native{};                // glTF inverse-bind operator evidence rather than an FBX witness
  std::uint8_t closureSide{};   // ClosureResidual only: 0 = source file, 1 = target file
  std::uint8_t witness{};       // FBX witness class: 0 declared BindPose, 1 cluster link
  std::optional<std::size_t> sourceInstance, sourceJoint, targetInstance, targetJoint;
  std::size_t element{};        // column-major index of the offending matrix element
  double residual{};            // that element's relative residual
  std::size_t sourceCount{}, targetCount{}; // CountMismatch only
};
struct Evidence {
  Format format{Format::Fbx};
  std::string sha256, adapterVersion;
  bool fullNodeDomain{};
  std::optional<Coordinates> coordinates;
  std::vector<Node> nodes;
  // nullopt = unavailable; empty = explicitly complete pure-animation input.
  // Adapter must enumerate every skin/instance binding, including unweighted nodes.
  std::optional<std::vector<Binding>> bindings;
  std::optional<std::vector<NativeSkinBinding>> nativeSkinBindings;
};
struct Pair { std::size_t source{}, target{}; };
struct Policy {
  std::size_t maxNodes{65536}, maxBindings{262144};
  // Provisional core budget in physical units, separate from display-space GPU gates.
  double translationMetres{1e-7}, rotationRadians{1e-7}, relativeScale{1e-9};
  double bindingResidual{1e-9};
};
struct Report {
  Verdict verdict{Verdict::Insufficient};
  Reason reason{Reason::MissingEvidence};
  std::optional<std::size_t> sourceNode, targetNode;
  std::string sourceSha, targetSha, sourceAdapter, targetAdapter;
  Policy policy;
  std::vector<Pair> mapping; // bounded, validated bijection; cache hashing is an adapter concern
  std::optional<BindingDiagnostic> bindingFault; // set only together with Reason::BindingConflict
  std::uint32_t evidenceVersion{3};
  double maxTranslationMetres{}, maxRotationRadians{}, maxScaleResidual{}, maxBindingResidual{};
};
// Pure evidence kernel. No parsing, retargeting, caching or UI publication.
// Adapters must prove completeness/provenance before constructing Evidence.
// Full bijective mapping only; partial/ambiguous host conclusions remain outside.
[[nodiscard]] Report validate(const Evidence &source, const Evidence &target,
  std::span<const Pair> mapping, const Policy &policy = {}, std::stop_token stop = {});
} // namespace formaview::skeletal::transform
