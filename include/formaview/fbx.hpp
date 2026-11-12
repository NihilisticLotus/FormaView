#pragma once
#include "formaview/transform_validation.hpp"
#include "formaview/skeletal.hpp"
#include "formaview/camera.hpp"
#include <memory>

namespace formaview::skeletal {
struct FbxMaterial {
  std::string name, colour_texture, colour_resolved, colour_uv;
  std::shared_ptr<const std::vector<std::byte>> colour_content;
  std::array<double, 6> colour_uv_transform{1, 0, 0, 0, 1, 0};
  int wrap_u{}, wrap_v{};
  std::array<double, 3> colour{.7, .7, .7};
  double opacity{1};
};
struct FbxNode {
  SourceMetadata source;
  std::optional<std::uint32_t> parent;
  bool bone{};
  bool implicit_scene_root{}; // Parser container, never an authored skeleton joint.
  Matrix4 default_local{}, default_world{}, geometry_world{};
  std::vector<FbxMaterial> materials;
  struct LodLevel {
    std::uint32_t child{};
    double distance{};
    std::uint32_t display{};
  };
  std::vector<LodLevel> lod_levels;
  bool relative_lod_distances{};
};
struct FbxCluster {
  SourceMetadata source;
  std::uint32_t bone_node{};
  Matrix4 geometry_to_bone{}, bind_to_world{};
  std::vector<std::uint32_t> points;
  std::vector<double>
      weights; // Raw cluster order; no cleanup, truncation or normalization.
};
struct FbxSkin {
  SourceMetadata source;
  std::uint32_t method{};
  std::vector<FbxCluster> clusters;
};
struct FbxMesh {
  Geometry geometry;
  std::vector<std::uint32_t> instance_nodes, face_materials;
  std::vector<std::uint32_t> triangle_corners; // ufbx triangulation; indices
                                               // into the source corner domain.
  using Material = FbxMaterial;
  std::vector<Material> materials;
  std::vector<FbxSkin> skins;
};
struct FbxClip {
  SourceMetadata source;
  double start{}, end{}, fps{};
  bool hasAnimatedChannels{};
};
struct FbxData {
  SourceMetadata source;
  std::uint32_t version{};
  bool ascii{};
  double metres_per_unit{}, fps{};
  std::array<std::int32_t, 3>
      raw_axes{}; // ufbx right/up/front enum values, unconverted.
  std::vector<FbxNode>
      nodes; // Includes auxiliary parents and the synthetic scene root.
  std::vector<FbxMesh> meshes;
  std::vector<formaview::CameraInfo> cameras;
  std::vector<FbxClip> clips;
  struct Resource {
    std::string reference, resolved, detail;
    bool loaded{};
  };
  std::vector<Resource> resources;
};
struct MatrixPose {
  std::vector<Matrix4> local, world;
  std::vector<formaview::CameraSample> cameras;
};
class FbxDocument final {
public:
  [[nodiscard]] static FbxDocument read(const std::filesystem::path &path,
                                        const ReadOptions &options = {});
  [[nodiscard]] static FbxDocument read_obj(const std::filesystem::path &path,
                                            const ReadOptions &options = {});
  [[nodiscard]] const FbxData &data() const noexcept;
  // Bounded source evidence, extracted on demand from the retained scene.
  [[nodiscard]] transform::Evidence transform_evidence(std::stop_token stop = {}) const;
  [[nodiscard]] MatrixPose evaluate(std::size_t clip, double seconds,
                                    std::stop_token stop = {}) const;

private:
  struct Impl;
  static FbxDocument read_impl(const std::filesystem::path &,
                               const ReadOptions &, bool obj);
  explicit FbxDocument(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
};
enum class FbxAxisPreset { Source, Maya, Houdini, Unreal, Max, Cinema4D, Blender, Unity };
struct FbxWriteOptions {
  // Explicit convention only. Initial writer supports a verified
  // Z-up/X-forward, right-handed numerical space and centimetres. No implicit
  // axis guessing.
  Limits limits;
  std::stop_token stop;
  FbxAxisPreset output_axes{FbxAxisPreset::Source};
  double output_metres_per_unit{0.01};
  PairingPolicy pairing_policy{PairingPolicy::StrictRecords};
  const PsaConfig *psa_config{}; // Explicit companion, valid only with a paired
                                 // PSK reference.
  bool include_mesh{
      true}; // A reference PSK can also supply a skeleton-only export.
  double max_world_matrix_error{
      0.0005}; // Absolute component error, audited within every bake interval.
  std::uint32_t max_sample_subdivisions{
      0}; // Zero: refine until accurate, bounded by memory/time representation.
          // Nonzero power of two: explicit caller work limit (not accuracy).
};
struct FbxWriteResult {
  std::uint32_t sample_subdivisions{};
  double source_fps{}, bake_fps{}, audited_world_matrix_error{};
  std::uint64_t samples{};
};
// Optional Autodesk SDK target; only ActorX conversion, not general FBX
// transcoding. Writes to a new path, never overwrites. Caller owns
// output-directory authorization.
FbxWriteResult write_fbx_clip(const ActorAsset &animation, std::size_t clip,
                              const std::filesystem::path &output,
                              const ActorAsset *paired_mesh = nullptr,
                              const FbxWriteOptions &options = {});
// Pure ActorX model export; never creates an animation stack.
void write_fbx_model(const ActorAsset &model, const std::filesystem::path &output,
                     const FbxWriteOptions &options = {});
// Bounded encoding for the shared output transaction; no temporary files.
[[nodiscard]] std::vector<std::byte> encode_fbx_model(const ActorAsset &model,
                                                     const FbxWriteOptions &options = {});
[[nodiscard]] std::vector<std::byte> encode_fbx_clip(const ActorAsset &animation, std::size_t clip,
    const ActorAsset *paired_mesh = nullptr, const FbxWriteOptions &options = {},
    FbxWriteResult *result = nullptr);
// One file, one independent animation stack per selected source clip.
[[nodiscard]] std::vector<std::byte> encode_fbx_clips(const ActorAsset &animation,
    std::span<const std::size_t> clips, const ActorAsset *paired_mesh = nullptr,
    const FbxWriteOptions &options = {}, std::vector<FbxWriteResult> *results = nullptr);
[[nodiscard]] std::string fbx_writer_version();
// Validate source, pairing and initial resource requirements without baking.
void validate_fbx_export(const ActorAsset &, std::span<const std::size_t> clips,
                         const ActorAsset *paired, const FbxWriteOptions &);
} // namespace formaview::skeletal
