#pragma once
#include "formaview/contracts.hpp"
#include <span>
#include <stdexcept>
#include <stop_token>

namespace formaview::skeletal {
struct Vec2 {
    double x{}, y{};
};
struct Vec3 {
    double x{}, y{}, z{};
};
struct Quat {
    double x{}, y{}, z{}, w{1};
};
struct Transform {
    Vec3 translation;
    Quat rotation;
    Vec3 scale{1, 1, 1};
};
// Matrices are row-major, act on column vectors; world = parent * local.
[[nodiscard]] Matrix4 matrix(const Transform &transform);
[[nodiscard]] Matrix4 multiply(const Matrix4 &a, const Matrix4 &b);
[[nodiscard]] Matrix4 inverse(const Matrix4 &value);
[[nodiscard]] std::string sha256(std::span<const std::byte> bytes, std::stop_token stop = {});

class Failure final : public std::runtime_error {
  public:
    Failure(ErrorCode code, std::string message);
    Error error;
};
struct Limits {
    std::uint64_t input_bytes{512ULL * 1024 * 1024};
    std::uint64_t decoded_bytes{1024ULL * 1024 * 1024};
    std::uint32_t chunks{4096}, bones{16384}, clips{65536};
    std::uint64_t elements{32000000};
};
enum class WedgeIndex { Full32, Legacy16 }; // Never silently mask invalid high bits.
struct ReadOptions {
    Limits limits;
    std::stop_token stop;
    Coordinates coordinates; // PSK/PSA have no embedded axis/unit declaration.
    WedgeIndex wedge_index{WedgeIndex::Full32};
};
struct Chunk {
    std::string name;
    std::int32_t flags{}, stride{}, count{};
    std::uint64_t offset{};
};
struct Bone {
    SourceMetadata source;
    std::int32_t flags{}, child_count{}, source_parent{};
    std::optional<std::uint32_t> parent;
    Quat raw_rotation;
    Vec3 raw_translation, raw_size;
    double raw_length{};
    Transform default_local;           // Source bone record, never substituted with frame zero.
    bool default_local_valid{true};    // PSA may contain zero reference placeholders, preserved verbatim.
    std::optional<Matrix4> bind_world; // PSK reference only; PSA record is not a verified bind.
};
struct Wedge {
    std::uint32_t point{}, material{};
    Vec2 uv;
};
struct Face {
    std::array<std::uint32_t, 3> wedges{};
    std::uint8_t material{}, auxiliary_material{};
    std::uint32_t smoothing{};
};
struct Weight {
    std::uint32_t point{}, bone{};
    double value{};
};
struct Material {
    std::string name;
    std::array<std::int32_t, 6> properties{};
};
struct Mesh {
    std::vector<Vec3> points;
    std::vector<Wedge> wedges;
    std::vector<Face> faces;
    std::vector<Material> materials;
    std::vector<Weight> weights; // Raw order, duplicates, zero, negative and all influences preserved.
    std::vector<std::vector<std::uint32_t>> point_weights;
    std::map<std::string, std::vector<Vec2>> extra_uvs; // Wedge domain.
    std::vector<std::array<std::uint8_t, 4>> colours;   // Wedge domain.
    std::vector<Vec3> normals;                          // Source point domain.
    std::vector<SourceVertexMapping> render_mapping;    // Face corner -> source point and wedge.
};
struct Key {
    Vec3 translation;
    Quat rotation;
    double delta_time{};
};
struct ScaleKey {
    Vec3 scale;
    double delta_time{};
};
struct Sequence {
    SourceMetadata source;
    std::string group;
    std::int32_t bone_count{}, root_include{}, compression{}, key_quota{}, start_bone{}, first_raw_frame{},
        frame_count{};
    double key_reduction{}, track_time{}, fps{};
    TimeDomain time;               // Clip-local: [0, frame_count/fps), last key at (frame_count-1)/fps.
    bool uniform_key_deltas{true}; // Uninterpretable timing remains inspectable, not silently evaluated.
};
struct ActorAsset {
    SourceMetadata source;
    bool animation{};
    std::vector<std::byte> source_bytes; // Retains unsupported chunk payloads, names and padding verbatim.
    std::vector<Chunk> chunks;
    std::vector<Bone> bones;
    std::optional<Mesh> mesh;
    std::vector<Sequence> sequences;
    std::vector<Key> keys;
    std::vector<ScaleKey> scale_keys;
    std::vector<std::string> warnings;
};
struct Pose {
    std::vector<Transform> local;
    std::vector<Matrix4> world;
};
[[nodiscard]] ActorAsset read_actor(std::span<const std::byte> bytes, const ReadOptions &options = {});
[[nodiscard]] ActorAsset read_actor(const std::filesystem::path &path, const ReadOptions &options = {});
[[nodiscard]] Pose evaluate(const ActorAsset &asset, std::size_t clip, double seconds,
                            std::stop_token stop = {});
[[nodiscard]] Pose default_pose(const ActorAsset &asset, std::stop_token stop = {});
struct Pairing {
    bool compatible{};
    std::vector<std::uint32_t> animation_to_mesh;
    std::vector<std::string> reasons;
    // A topology/name match alone does not certify reference-pose compatibility.
    bool reference_verified{};
    bool hierarchy_verified{}, uses_reference_skeleton{};
    std::string animation_sha256, reference_sha256;
    std::vector<std::optional<std::uint32_t>> parents; // Animation order, supplied by reference PSK.
    std::vector<Transform> reference_local;
    std::vector<std::string> warnings;
};
enum class PairingPolicy { StrictRecords, ReferenceSkeleton };
[[nodiscard]] Pairing pair(const ActorAsset &animation, const ActorAsset &mesh,
                           double reference_tolerance = 1e-4,
                           PairingPolicy policy = PairingPolicy::StrictRecords);
struct PsaConfig {
    SourceMetadata source;
    std::string animation_sha256;
    std::vector<std::byte> source_bytes;
    // Clip then PSA bone order: bit 0 removes translation, bit 1 removes rotation.
    // Removed channels use the paired PSK reference, never the PSA bone record.
    std::vector<std::vector<std::uint8_t>> removed_tracks;
    std::vector<std::string> warnings;
};
// Explicit companion input; no filesystem discovery or implicit access to neighbouring files.
[[nodiscard]] PsaConfig read_psa_config(std::span<const std::byte> bytes, const ActorAsset &animation,
                                        const ReadOptions &options = {});
[[nodiscard]] Pose evaluate_paired(const ActorAsset &animation, const Pairing &pairing, std::size_t clip,
                                   double seconds, const PsaConfig *config = nullptr,
                                   std::stop_token stop = {});
} // namespace formaview::skeletal
