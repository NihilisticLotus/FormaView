#pragma once
#include "formaview/fbx.hpp"
#include "formaview/render_item.hpp"
#include "formaview/skeletal.hpp"
#include <QHash>
#include <QVariantList>
#include <QVector3D>
#include <functional>
#include "formaview/gltf_anim.hpp"
#include "formaview/geometry_cache.hpp"
#include <map>
#include <stop_token>
#include <cmath>

struct ModelWeight {
  int bone;
  double value;
};
struct ModelSkin {
  QString name, id, method;
  QStringList bones;
  QVariantList palette;
  std::vector<std::vector<ModelWeight>> weights;
};
struct ModelSkinCluster {
  std::uint32_t node{};
  std::array<double, 16> geometryToBone{};
};
// Per-point slot into the mesh cluster list; the cluster matrix is computed
// once per frame per cluster, not per point slot.
using ModelSkinSlot = std::pair<std::uint32_t, double>; // cluster index, weight
struct ModelAnimationClip {
  QString name;
  int source{-1}; // External-drive source index (-1 native).
  double start{}, end{}; // Real seconds from the source time domain.
  double fps{30};
  int lastFrame{};       // Last sample index for display mapping.
  bool hasAnimatedChannels{true};
  [[nodiscard]] bool playable() const {
    return hasAnimatedChannels && std::isfinite(start) && std::isfinite(end) &&
           end > start && std::isfinite(end - start) &&
           std::isfinite(fps) && fps > 0;
  }
};
struct ModelMorphTarget {
  std::vector<formaview::skeletal::Vec3> positionDeltas; // Geometry-local.
  std::vector<formaview::skeletal::Vec3> normalDeltas;   // Local; empty without base normals.
};
struct ModelClassification {
  QString group;
  int level{};
  bool collision{};
  bool operator==(const ModelClassification &) const = default;
};
struct ModelMesh {
  QString name, id, nodeId;
  bool uvTopOrigin{};
  bool unlit{};
  bool skinDeformationValid{true}; // Invalid raw weights remain inspectable, never animated.
  ModelClassification classification;
  QString classificationEvidence, classificationCandidate;
  QVector3D boundsLow, boundsHigh;
  std::vector<formaview::skeletal::Vec3> raw;
  // Animated morph weights (glTF weights channels): targets are applied per
  // pose instead of the static defaults baked at load.
  bool morphAnimated{};
  std::vector<ModelMorphTarget> morphTargets;
  // Unmorphed per-point base normals, local space (animated meshes only).
  std::vector<formaview::skeletal::Vec3> morphBaseNormals;
  // Local geometry used for animation, including static morph defaults.
  // Raw source positions and decoded static display data remain unchanged.
  std::vector<formaview::skeletal::Vec3> animationPositions;
  std::vector<QVector3D> animationNormals;
  std::vector<QVector3D> points;
  std::vector<std::uint32_t> corners, triangles;
  std::vector<std::uint64_t> faces;
  std::vector<std::uint64_t> triangleFaces;
  std::vector<std::array<std::uint64_t, 3>> triangleCorners;
  QStringList uvNames;
  std::vector<std::vector<QPointF>> uvs;
  std::vector<std::array<double, 4>> uvBounds;
  std::vector<bool> invalidUVs;
  std::vector<QVector3D> normals, colours;
  std::vector<float> alphas;
  std::vector<int> alphaModes;
  std::vector<float> alphaCutoffs;
  std::vector<ModelSkin> skins;
  QStringList materials, references, texturePaths;
  std::vector<int> materialUv;
  std::vector<std::array<float, 5>>
      uvTransforms; // offset U/V, scale U/V, rotation.
  std::vector<std::array<float, 6>> uvAffine;
  std::vector<std::uint32_t> cornerMaterial;
  std::vector<std::shared_ptr<const formaview::render::Texture>> textures;
};
// Immutable, process-local proof input. Never persisted as a reusable verdict.
struct ModelDecoderSnapshot {
  std::vector<QString> nodeNames; // full raw domain; unique names may establish a candidate bijection
  QByteArray metadata;
  // Full decoded local morph inputs; empty vector means a legacy TRS-only snapshot.
  std::vector<QByteArray> morphInputs;
  std::vector<std::size_t> morphTargetCounts;
  QStringList excludedShadingExtensions;
  QString sourceSha;
  std::shared_ptr<const formaview::gltf::GltfAnimationData> animations;
};
// Caller-supplied contract for the fixed-decoder proof. explicitMapping[i] is the
// target node of source node i and must be a full bijection; names are then not consulted.
// Budgets may only be tightened: values above the built-in limits are clamped.
struct FixedDecoderOptions {
  std::vector<std::size_t> explicitMapping;
  std::size_t channelBudget{1048576};
  std::size_t scalarBudget{67108864};
};
QVariantMap compareFixedDecoder(const std::shared_ptr<const ModelDecoderSnapshot> &a,
    const std::shared_ptr<const ModelDecoderSnapshot> &b, std::stop_token stop = {},
    const FixedDecoderOptions &options = {});
class ModelAbcSource;
struct ModelDocument {
  std::shared_ptr<const ModelDecoderSnapshot> decoderSnapshot;
  // Alembic geometry cache: every document is one immutable timeline sample
  // of the shared archive (never animated through worldAt/clips). The source
  // never references documents, so sample caches cannot form cycles.
  std::shared_ptr<const ModelAbcSource> geometryCache;
  std::size_t cacheSample{};                  // Index into the merged timeline.
  std::vector<std::size_t> cacheMeshSamples;  // Per-mesh source sample index.
  std::vector<formaview::cache::SampleKeys> cacheMeshKeys; // Content digests.
  std::vector<formaview::Matrix4> cacheMeshWorlds;           // World per mesh.
  std::vector<std::size_t> cacheMeshFaces;                   // Faces per mesh.
  std::uint64_t cacheFaces{}, cacheCorners{}; // Source polygon statistics.
  // Immutable source evidence; never derived from a displayed or animated frame.
  std::function<formaview::skeletal::transform::Evidence(std::stop_token)> transformEvidence;
  std::vector<ModelMesh> meshes;
  QVariantList nodes, resources;
  QHash<QString, QVariantMap> nodeById;
  QStringList warnings;
  bool linearColour{};
  QString hash, coordinates, name;
  QMatrix4x4 displayBasis;
  QVector3D center;
  float radius{1};
  std::uint64_t points{}, triangles{};
  // Animation channel (M07). skeletonOnly documents (standalone PSA,
  // animation-only FBX) carry no meshes and render as skeleton lines.
  bool skeletonOnly{};
  // Session-reference compatibility outcome for THIS file: rejected means
  // the remembered/user reference did not fit and the document previews
  // unpaired with an explicit warning (never a silently wrong skeleton).
  bool referenceRejected{};
  QString referenceRejectReason;
  // Paired PSK for optional skinned-model display: the PSK asset (kept
  // alive for CPU skinning) and mesh-bone -> animation-bone index map.
  std::shared_ptr<const formaview::skeletal::ActorAsset> pairSource;
  std::vector<std::uint32_t> pairBoneMap;
  QString referencePath, companionPath;
  std::vector<ModelAnimationClip> animationClips;
  QVariantList clips; // QML-facing clip maps.
  QStringList boneNames;
  std::vector<std::int32_t> boneParents{}; // -1 root, animation bone order.
  // Evaluates per-bone world matrices for (clip, real seconds). The closure
  // owns its source assets; the caller never mutates source data.
  std::function<std::vector<formaview::Matrix4>(std::size_t, double)>
      worldAt;
  // Animated morph weights per mesh (clip, seconds -> one vector per mesh;
  // empty for meshes without animated targets). Linear/Step interpolation.
  std::function<std::vector<std::vector<double>>(std::size_t, double)>
      morphWeightsAt;
  // Per-bone LOCAL matrices for the Force Root Lock domain: lock replaces
  // the root local translation/rotation with a base while keeping source
  // scale, then worlds are re-chained. Empty when the format has no local
  // evaluator wired yet.
  std::function<std::vector<formaview::Matrix4>(std::size_t, double)>
      localAt;
  // Verified PSK reference locals (matrix form) when a pairing supplies
  // them; enables the Reference Pose base.
  std::vector<formaview::Matrix4> referenceLocals;
  // External FBX animation drive channel (attached at runtime, session
  // only): the target keeps its own meshes/materials/skeleton/skins; the
  // external source only contributes evaluated local poses for MAPPED
  // bones. Unmapped target bones keep their static local (constant-node
  // semantics); nothing of the target is ever overwritten.
  bool externallyDriven{};
  // Native self-contained animation (glTF/GLB/FBX files whose own clips drive
  // their meshes): unlike externallyDriven nothing is attached at runtime
  // and no mapping exists — the file's channels address its own nodes.
  bool nativeAnimated{};
  // FBX native clips may coexist with mapped external FBX clips.
  bool nativeFbxAnimated{};
  decltype(localAt) nativeFbxLocalAt, nativeFbxWorldAt;
  QString externalPath, externalStatus, externalNotice;
  std::vector<std::int32_t> animNodeParents;       // full target node domain
  std::vector<formaview::Matrix4> animNodeLocals;  // static target locals
  // Skeleton roots derived from weighted cluster ancestor chains, excluding
  // the synthetic scene root and unrelated mesh/scene nodes.
  std::vector<std::int32_t> animRootNodes;
  std::vector<std::int32_t> externalToTarget;       // ext node -> target node/-1
  // Multi-source drive: clips reference their source holder/map by index.
  std::vector<std::shared_ptr<const formaview::skeletal::FbxDocument>>
      externalHolders;
  std::vector<std::vector<std::int32_t>> externalMaps;
  // PSK-first drive reuses verified PSA/PSK pairings. Each source owns its
  // evaluator/config; poses are reordered into the immutable target PSK domain.
  std::vector<std::shared_ptr<const ModelDocument>> externalActorSources;
  // [mesh] clusters and [mesh][point] slots (first skin binding), plus
  // averaged smooth point normals derived from the source corner normals.
  std::vector<std::vector<ModelSkinCluster>> skinClusters;
  std::vector<std::vector<std::vector<ModelSkinSlot>>> skinSlots;
  std::vector<std::vector<formaview::skeletal::Vec3>> pointNormals;
  std::vector<std::uint32_t> meshInstanceNodes; // rigid fallback per mesh
  [[nodiscard]] bool canAttachExternal() const {
    return !meshes.empty() && (!nativeAnimated || nativeFbxAnimated) &&
           ((pairSource && !skeletonOnly && !pairSource->bones.empty()) ||
            (!animNodeParents.empty() &&
             animNodeLocals.size() == animNodeParents.size())) && !externallyDriven;
  }
  [[nodiscard]] bool animated() const {
    return worldAt && !animationClips.empty() &&
           (skeletonOnly || externallyDriven || nativeAnimated);
  }
  [[nodiscard]] int firstPlayableAnimation() const {
    if (animated())
      for (std::size_t i = 0; i < animationClips.size(); ++i)
        if (animationClips[i].playable()) return static_cast<int>(i);
    return -1;
  }
  // Full-node animation domain (external drive or native glTF clips): parent
  // chains, root candidates and lock worlds use it instead of boneParents.
  [[nodiscard]] bool nodeAnimDomain() const {
    return (externallyDriven || nativeAnimated) && !animNodeParents.empty();
  }
  // GPU skinning domain: exactly the documents whose CPU pose takes the
  // mesh-cluster path (native glTF clips, FBX-target external drive), plus a
  // paired PSK actor driven by attached PSA clips (ActorAsset bind palette).
  [[nodiscard]] bool gpuSkinDomain() const {
    return (nodeAnimDomain() && !meshes.empty() && !pairSource &&
            skinClusters.size() == meshes.size()) || actorSkinDomain();
  }
  // PSK-first drive: one source mesh, worlds in the PSK bone (boneParents)
  // domain, mesh bone i reading world pairBoneMap[i] like the CPU path.
  [[nodiscard]] bool actorSkinDomain() const {
    return externallyDriven && pairSource && pairSource->mesh && !skeletonOnly &&
           meshes.size() == 1 && !externalActorSources.empty() &&
           pairBoneMap.size() == pairSource->bones.size() && !boneParents.empty();
  }
  // PSA-first preview of the paired reference PSK mesh (shown on request):
  // no ModelMesh, worlds in animation bone order, the same ActorAsset skinning.
  [[nodiscard]] bool pairPreviewDomain() const {
    return skeletonOnly && meshes.empty() && pairSource && pairSource->mesh &&
           pairBoneMap.size() == pairSource->bones.size() && !boneParents.empty() &&
           worldAt && !animationClips.empty();
  }
};

std::shared_ptr<ModelDocument> readGltfModel(const QString &, std::stop_token);
// Shared tail of every reader: classification candidates, UV bounds, display
// basis, statistics, budgets and bounds. allowEmpty admits documents without
// triangles (an empty geometry-cache sample) and gives them unit bounds.
void finalizeModelGeometry(ModelDocument &, std::stop_token,
                           bool allowEmpty = false);
// On-demand audit fingerprint of immutable source geometry and skin data.
QByteArray modelSourceDigest(const ModelDocument &);
std::shared_ptr<ModelDocument> readModelDocument(const QString &,
                                                 std::stop_token,
                                                 const QString &reference = {},
                                                 const QString &companion = {});
struct ModelTextureLink {
  std::shared_ptr<const formaview::render::Texture> pixels;
  QString path;
  int uv{-1};
};
struct ModelRenderState {
  QString highlightedNode;
  int mode{}, mesh{}, bone{}, material{};
  bool materialOnly{};
  bool isolated{};
  int collisionMode{};
  int clip{-1};
  double time{}; // Real seconds of the presented animation pose.
  std::vector<ModelClassification> classes;
  std::map<QString, int> levels;
  std::vector<bool> visible;
  std::vector<int> bindings, uvs;
  std::vector<std::map<int, ModelTextureLink>> textures;
};
struct ModelFrames {
  std::shared_ptr<const formaview::render::Frame> model, uv;
  // Full triangle-order pose for exact picking independent of view filters.
  std::shared_ptr<const formaview::render::Frame> pose;
  int mode{};
};
ModelFrames buildModelFrames(const ModelDocument &, const ModelRenderState &,
                             std::uint64_t, std::stop_token,
                             const formaview::render::Frame *pose = nullptr,
                             bool recordSources = false);
ModelFrames buildAbcStreamFrames(const ModelDocument &, const ModelRenderState &,
    std::uint64_t, std::stop_token,
    std::shared_ptr<const formaview::render::Frame> previous = {});
// Animation skeleton frame: per-bone world matrices -> line segments with
// joint crosses, fit to bounds, never cached, never mutating source data.
std::shared_ptr<const formaview::render::Frame>
buildAnimationFrame(const ModelDocument &, std::size_t clip, double seconds,
                    std::uint64_t revision, bool pairMesh = false,
                    const std::vector<formaview::Matrix4> *overrideWorlds = nullptr,
                    std::stop_token stop = {});
// Root-lock helpers (matrix domain): combine base translation/rotation with
// source scale, and chain locals into worlds along boneParents.
[[nodiscard]] formaview::Matrix4 lockRootLocal(const formaview::Matrix4 &source,
                                               const formaview::Matrix4 &base);
[[nodiscard]] std::vector<formaview::Matrix4>
chainWorlds(const std::vector<formaview::Matrix4> &locals,
            const std::vector<std::int32_t> &parents);

struct GpuBounds { QVector3D low, high; bool valid{}; };
// What a pose shows out of resident geometry: draw subset, bounds and statistics.
struct GpuView {
  std::vector<std::uint32_t> draws; // Indices into the resident geometry draws.
  std::vector<GpuBounds> bounds;    // Per cluster, local space, union over shown meshes.
  std::uint64_t sourcePoints{}, sourceTriangles{};
  bool explicitDraws{};
};
struct GpuHighlight { int node{-1}; std::vector<int> edges; };
struct ModelGpuGeometry {
  ModelFrames frames;
  int highlighted{-1};
  std::vector<int> highlightEdges;
  // True when the static payload holds every mesh, so visibility, isolation
  // and LOD changes only select draws (no rebuild, no re-upload).
  bool coversAllMeshes{};
  std::vector<std::uint64_t> meshPoints, meshTriangles;
  GpuView view; // View of the state the geometry was built for.
  std::vector<ModelSkinCluster> clusters;
  std::vector<int> rigidMeshes;
  std::vector<QMatrix4x4> restInverse;
  using Bounds = GpuBounds;
  std::vector<Bounds> bounds;
  std::vector<std::vector<Bounds>> meshBounds;
  // Exact centroid contributions, keyed by cluster, in unnormalized world units.
  std::vector<std::vector<std::pair<std::uint32_t, QVector4D>>> centers;
};
// Number of GPU static geometry builds started in this process (verification instrument).
std::uint64_t modelGpuGeometryBuildCount();
std::shared_ptr<const ModelGpuGeometry> buildModelGpuGeometry(
    const ModelDocument &, const ModelRenderState &, std::uint64_t, std::stop_token);
// Grid and axis guides shared by every model frame (pose independent).
void appendModelGuides(formaview::render::Frame &, const ModelDocument &);
// View (shown draws, bounds, statistics) of `state` over resident geometry.
GpuView makeGpuView(const ModelDocument &, const ModelGpuGeometry &, const ModelRenderState &);
// Bone nodes to highlight for the selected skeleton node (empty when none).
GpuHighlight resolveGpuHighlight(const ModelDocument &, const QString &node);
ModelFrames buildModelGpuPose(const ModelDocument &, const ModelGpuGeometry &,
    std::shared_ptr<const formaview::render::Frame>,
    const std::vector<formaview::Matrix4> &, std::uint64_t, int, double,
    const GpuView *view = nullptr, const GpuHighlight *highlight = nullptr);

struct ModelPickHit {
  int hitMesh{-1};
  std::size_t hitTriangle{}, hitCornerInTriangle{}, overlaps{};
  std::uint32_t hitCorner{};
  float baryU{}, baryV{};
};
ModelPickHit pickModelDocument(const ModelDocument &, const ModelRenderState &,
    const formaview::render::Frame *, const QMatrix4x4 &, QPointF, bool, std::stop_token = {});
