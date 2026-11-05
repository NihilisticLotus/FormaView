#pragma once
#include "formaview/skeletal.hpp"
#include <memory>
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
// subdivision surfaces, NURBS, cameras, lights, unknown schemas).
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
  Archive(Archive &&) noexcept;
  Archive &operator=(Archive &&) noexcept;
  ~Archive();
  [[nodiscard]] const std::vector<MeshInfo> &meshes() const;
  [[nodiscard]] const std::vector<SkippedObject> &skipped() const;
  // Sorted distinct sample times of every animated mesh and ancestor
  // transform; a fully static archive has exactly one time.
  [[nodiscard]] const std::vector<double> &times() const;
  [[nodiscard]] Sample read(std::size_t mesh, double seconds,
                            std::stop_token = {}) const;
  // Cheap queries that decode no geometry: content digests, the world
  // matrix and the per-mesh sample index at `seconds`.
  [[nodiscard]] SampleKeys keys(std::size_t mesh, double seconds) const;
  [[nodiscard]] Matrix4 world(std::size_t mesh, double seconds) const;
  [[nodiscard]] std::size_t sample_index(std::size_t mesh, double seconds) const;
  // Reads whose topology arrays were reused (verification instrument).
  [[nodiscard]] std::uint64_t topology_reuses() const;

private:
  struct Impl;
  explicit Archive(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
};
} // namespace formaview::cache
