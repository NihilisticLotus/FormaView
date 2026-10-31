#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace formaview {
inline constexpr std::uint32_t api_version = 1;

template<class Tag> struct Id {
    std::string value;
    auto operator<=>(const Id&) const = default;
};
using AssetId = Id<struct AssetTag>;
using ObjectId = Id<struct ObjectTag>;
using PlanId = Id<struct PlanTag>;
using ItemId = Id<struct ItemTag>;
using JobId = Id<struct JobTag>;
[[nodiscard]] AssetId asset_id_from_sha256(std::string_view digest);

// Content identity is independent of display name, path and adapter array order.
// object_key must be an adapter's persistent source locator, never a display label.
struct SourceIdentity {
    std::string sha256;
    std::string adapter;
    std::string adapter_version;
    std::string object_key;
    [[nodiscard]] ObjectId stable_id() const;
};
struct InputSnapshot {
    AssetId asset;
    std::filesystem::path requested_path;
    std::filesystem::path resolved_path;
    std::uint64_t size_bytes{};
    std::int64_t modified_ticks{};
    std::string content_sha256;
    std::string file_identity;
};
enum class Axis { Unknown, PositiveX, NegativeX, PositiveY, NegativeY, PositiveZ, NegativeZ };
enum class Handedness { Unknown, Left, Right };
struct Coordinates {
    Axis up{Axis::Unknown};
    Axis forward{Axis::Unknown};
    Handedness handedness{Handedness::Unknown};
    std::optional<double> metres_per_unit;
    std::string source_convention;
};
struct Rational { std::int64_t numerator{}; std::int64_t denominator{1}; };
enum class EndBoundary { Inclusive, Exclusive, Unknown };
struct TimeDomain {
    double source_start_seconds{};
    double source_end_seconds{};
    EndBoundary end_boundary{EndBoundary::Unknown};
    std::optional<Rational> frame_rate;
    std::vector<double> sample_times; // Supports nonuniform geometry sampling.
};
using Matrix4 = std::array<double, 16>; // Row-major storage; adapter declares source convention.
struct SourceMetadata {
    SourceIdentity identity;
    std::string original_name;
    Coordinates coordinates;
    std::map<std::string, std::string> properties;
};
enum class ScalarType { UInt8, UInt16, UInt32, Int16, Int32, Half, Float32, Float64 };
struct Buffer {
    ScalarType scalar{ScalarType::UInt8};
    std::uint32_t components{1};
    std::uint64_t element_count{};
    std::vector<std::byte> bytes;
};
enum class AlphaMode { Unknown, Absent, Straight, Premultiplied };
enum class ImageMeaning { Unknown, Colour, Data };
struct ImageWindow { std::int64_t x{}, y{}; std::uint64_t width{}, height{}; };
enum class SampleEncoding { Numeric, UNorm, SNorm };
struct ImageChannel { std::string name; Buffer samples; SampleEncoding encoding{SampleEncoding::Numeric}; };
struct ImageSubresource {
    std::uint32_t part{}, mip{}, array_layer{}, cube_face{}, depth_slice{};
    std::string part_name;
    ImageWindow data_window, display_window;
    std::vector<ImageChannel> channels;
    std::optional<std::string> block_encoding;
    std::vector<std::byte> encoded_blocks;
};
struct ImageData {
    SourceMetadata source;
    AlphaMode alpha{AlphaMode::Unknown};
    ImageMeaning meaning{ImageMeaning::Unknown};
    std::optional<std::string> colour_space;
    std::vector<ImageSubresource> subresources;
};
struct Influence { ObjectId joint; double source_weight{}; };
struct SourceVertexMapping {
    std::uint64_t source_point{}, source_face{}, source_corner{}, render_vertex{};
};
struct Geometry {
    SourceMetadata source;
    Buffer positions;
    std::vector<std::uint64_t> face_sizes, corner_points;
    std::map<std::string, Buffer> corner_attributes; // Named UV sets, normals, colours.
    std::vector<std::vector<Influence>> point_influences; // Complete, unnormalised; no four-weight limit.
    std::vector<SourceVertexMapping> render_mapping;
};
struct Joint {
    SourceMetadata source;
    std::optional<ObjectId> parent;
    std::optional<Matrix4> default_local, bind_world;
};
struct SkinBinding {
    ObjectId mesh, skeleton;
    std::vector<ObjectId> palette;
    std::vector<Matrix4> inverse_bind;
    std::string source_skinning_method;
};
struct SkeletonData { SourceMetadata source; std::vector<Joint> joints; };
struct AnimationClip {
    SourceMetadata source;
    ObjectId skeleton;
    TimeDomain time;
    std::optional<Matrix4> first_frame_root;
};
struct SceneNode {
    SourceMetadata source;
    std::optional<ObjectId> parent, geometry;
    std::optional<Matrix4> source_local;
};
struct SceneData {
    SourceMetadata source;
    std::vector<SceneNode> nodes;
    std::vector<Geometry> geometries;
    std::vector<SkeletonData> skeletons;
    std::vector<SkinBinding> bindings;
    std::vector<AnimationClip> clips;
};
enum class Topology { Unknown, Fixed, Varying };
struct GeometryCacheDescriptor { SourceMetadata source; TimeDomain time; Topology topology{Topology::Unknown}; };
struct GeometrySample { ObjectId cache; double source_seconds{}; Geometry geometry; }; // One requested sample, not all frames.

enum class ErrorCode {
    Unsupported, InvalidArgument, QueueFull, ResourceLimit, ShuttingDown,
    Cancelled, StaleResult, InputChanged, OutOfScope, Conflict, Io, Internal
};
struct Error {
    ErrorCode code{ErrorCode::Internal};
    std::string message;
    std::map<std::string, std::string> details;
    bool retryable{};
};
enum class ConflictPolicy { Skip, Rename, Overwrite };
struct AccessScope {
    std::vector<std::filesystem::path> input_roots, output_roots;
    bool allow_overwrite{};
    std::string policy_revision;
    // Internal captured dependency graph: reads that were unavailable during
    // inventory stay unavailable for this execution, even if created later.
    std::vector<std::filesystem::path> unavailable_inputs{};
};
struct ResourceEstimate {
    std::optional<std::uint64_t> peak_memory_bytes, peak_gpu_bytes, input_bytes, output_bytes;
    std::string basis;
};
struct PlanItem {
    ItemId id;
    InputSnapshot input;
    std::optional<ObjectId> clip;
    std::filesystem::path output;
    std::map<std::string, std::string> parameters;
    std::vector<std::string> losses, warnings;
    ResourceEstimate estimate;
};
// Value contract. Services must hold approved plans as shared_ptr<const ConversionPlan>.
// Filesystem authorisation, persistence and idempotent submission arrive with real converters.
struct ConversionPlan {
    std::uint32_t version{api_version};
    PlanId id;
    std::string parameter_digest, capability_revision;
    AccessScope scope;
    ConflictPolicy conflict{ConflictPolicy::Skip};
    std::vector<PlanItem> items;
};
struct Submission { std::uint32_t version{api_version}; PlanId plan; std::string idempotency_key, parameter_digest; };
enum class ItemStatus { Succeeded, Failed, Skipped, Cancelled };
struct ItemResult {
    ItemId id;
    ItemStatus status{ItemStatus::Succeeded};
    std::optional<InputSnapshot> input;
    std::optional<ObjectId> clip;
    std::filesystem::path committed_output;
    std::map<std::string, std::string> actual_parameters, dependency_versions;
    std::vector<std::string> warnings;
    std::optional<Error> error;
};
struct TaskResult {
    std::uint32_t version{api_version};
    std::vector<ItemResult> items;
    std::optional<Error> error;
};
enum class TaskState { Queued, Running, Cancelling, Succeeded, PartiallySucceeded, Failed, Cancelled, StaleDiscarded, Interrupted };
[[nodiscard]] bool is_terminal(TaskState state) noexcept;
struct PageRequest { std::uint64_t offset{}; std::uint32_t limit{100}; };
template<class T> struct ResultPage {
    std::uint32_t version{api_version};
    std::vector<T> items;
    std::optional<std::uint64_t> next_offset;
    std::uint64_t total{};
};
struct Progress { std::string stage; std::uint64_t completed{}, discovered{}; std::optional<std::uint64_t> total; };
} // namespace formaview
