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
  std::vector<double> times;
  bool uv{}, normals{};
};
struct Sample {
  std::size_t index{};
  double seconds{};
  std::vector<skeletal::Vec3> positions, normals;
  std::vector<skeletal::Vec2> uv;
  std::vector<std::int32_t> face_sizes, corner_points;
  int uv_scope{}, normal_scope{};
  Matrix4 world{};
};
class Archive {
public:
  // Worker-only, one request at a time. No all-frame cache or implicit
  // prefetch.
  static Archive open(const std::filesystem::path &, Limits = {},
                      std::stop_token = {});
  Archive(Archive &&) noexcept;
  Archive &operator=(Archive &&) noexcept;
  ~Archive();
  [[nodiscard]] const std::vector<MeshInfo> &meshes() const;
  [[nodiscard]] Sample read(std::size_t mesh, double seconds,
                            std::stop_token = {}) const;

private:
  struct Impl;
  explicit Archive(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
};
} // namespace formaview::cache
