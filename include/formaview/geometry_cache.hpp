#pragma once
#include "formaview/skeletal.hpp"
#include "formaview/camera.hpp"
#include <memory>
#include <span>
#include <string_view>
namespace formaview::cache {
struct Limits {
  std::uint64_t input_bytes{4ULL * 1024 * 1024 * 1024},
      sample_bytes{512ULL * 1024 * 1024};
  std::uint32_t objects{65536}, samples{1000000};
};
struct MeshInfo {
  std::string path;
  Topology topology{};
  // True when the schema declares constant topology and positions (one
  // static sample); Fixed/Varying still describe the declared variance.
  bool constant{};
  std::vector<double> times;
  bool uv{}, normals{};
};
// Objects that were recognised but are not previewed (curves, points,
// subdivision surfaces, NURBS, lights, unknown schemas).
// Alembic stores no units. The writing application's documented working
// unit is the only evidence: Maya and Cinema 4D write centimetres, Houdini
// and Blender write metres. Anything else is unknown (metres by convention).
struct UnitConvention {
  double metres_per_unit{1};
  bool known{};
};
[[nodiscard]] UnitConvention unit_convention(std::string_view application);
struct SkippedObject {
  std::string path, schema;
};
// Alembic content digests of one mesh sample's arrays (empty when absent).
// Equal digests identify identical stored data without decoding it.
struct SampleKeys {
  std::string positions, face_indices, face_counts, uv, uv_indices, normals,
      normal_indices;
  bool operator==(const SampleKeys &) const = default;
  [[nodiscard]] bool same_topology(const SampleKeys &other) const {
    return !face_indices.empty() && face_indices == other.face_indices &&
           face_counts == other.face_counts;
  }
  [[nodiscard]] bool same_content(const SampleKeys &other) const {
    return !positions.empty() && *this == other;
  }
};
struct Sample {
  std::size_t index{};
  double seconds{};
  std::vector<skeletal::Vec3> positions, normals;
  std::vector<skeletal::Vec2> uv;
  std::vector<std::int32_t> face_sizes, corner_points;
  int uv_scope{}, normal_scope{};
  Matrix4 world{};
  SampleKeys keys;
  bool topology_reused{}; // Face arrays came from the previous identical read.
};
// Alembic GeomParam scopes as stored in Sample::uv_scope/normal_scope.
enum class Scope { Constant = 0, Uniform = 1, Varying = 2, Vertex = 3,
                   FaceVarying = 4 };
class Archive {
public:
  // Reads are serialised internally (one request at a time); there is no
  // all-frame cache or implicit prefetch at this layer.
  static Archive open(const std::filesystem::path &, Limits = {},
                      std::stop_token = {});
  // Archive over caller-owned bytes (e.g. an exact leased snapshot); `owner`
  // keeps them alive for the archive's lifetime.
  static Archive open(std::span<const char> bytes,
                      std::shared_ptr<const void> owner, Limits = {},
                      std::stop_token = {});
  Archive(Archive &&) noexcept;
  Archive &operator=(Archive &&) noexcept;
  ~Archive();
  [[nodiscard]] const std::vector<MeshInfo> &meshes() const;
  [[nodiscard]] const std::vector<formaview::CameraInfo> &cameras() const;
  [[nodiscard]] formaview::CameraSample camera(std::size_t camera,
                                               double seconds) const;
  [[nodiscard]] const std::vector<SkippedObject> &skipped() const;
  // Sorted distinct sample times of every animated mesh and ancestor
  // transform; a fully static archive has exactly one time.
  [[nodiscard]] const std::vector<double> &times() const;
  // Archive "_ai_Application" metadata (empty when the writer did not set it).
  [[nodiscard]] const std::string &application() const;
  [[nodiscard]] Sample read(std::size_t mesh, double seconds,
                            std::stop_token = {}) const;
  // Cheap queries that decode no geometry: content digests, the world
  // matrix and the per-mesh sample index at `seconds`.
  [[nodiscard]] SampleKeys keys(std::size_t mesh, double seconds) const;
  [[nodiscard]] Matrix4 world(std::size_t mesh, double seconds) const;
  [[nodiscard]] std::size_t sample_index(std::size_t mesh, double seconds) const;
  // Reads whose topology arrays were reused (verification instrument).
  [[nodiscard]] std::uint64_t topology_reuses() const;
  [[nodiscard]] std::uint64_t uv_memo_bytes() const;

private:
  struct Impl;
  explicit Archive(std::unique_ptr<Impl>);
  static Archive index(std::unique_ptr<Impl>, Limits, std::stop_token);
  std::unique_ptr<Impl> impl_;
};
} // namespace formaview::cache
