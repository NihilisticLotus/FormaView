#pragma once
#include "formaview/skeletal.hpp"
#include <list>
#include <memory>

namespace formaview::preview {
enum class WeightPolicy { Raw, Normalize };
struct Vertex {
  skeletal::Vec3 position;
  skeletal::Vec2 uv;
  std::uint32_t point{}, wedge{};
  // Unit skinned normal when the source provides point normals; otherwise
  // zero and callers fall back to flat shading.
  skeletal::Vec3 normal{};
};
struct MeshFrame {
  std::vector<Vertex> corners;
};
class SkinEvaluator {
public:
  // Source must outlive this worker-local evaluator and remain immutable.
  explicit SkinEvaluator(const skeletal::ActorAsset &);
  [[nodiscard]] const MeshFrame &
  evaluate(std::span<const Matrix4> world, std::string_view uv_set = {},
           WeightPolicy policy = WeightPolicy::Normalize,
           std::stop_token stop = {});

private:
  const skeletal::ActorAsset &source_;
  std::vector<Matrix4> inverse_bind_, matrices_;
  std::vector<skeletal::Vec3> points_;
  std::vector<skeletal::Vec3> skinnedNormals_;
  MeshFrame frame_;
};
[[nodiscard]] MeshFrame
evaluate_mesh(const skeletal::ActorAsset &source,
              std::span<const Matrix4> world, std::string_view uv_set = {},
              WeightPolicy policy = WeightPolicy::Normalize,
              std::stop_token stop = {});
struct WeightValue {
  double sum{};
  bool has_data{};
};
class WeightIndex {
public:
  explicit WeightIndex(const skeletal::Mesh &source, std::size_t bones);
  [[nodiscard]] std::vector<WeightValue> select(std::size_t bone) const;

private:
  std::vector<std::vector<std::pair<std::uint32_t, double>>> by_bone_;
  std::vector<bool> has_data_;
};
struct Hit {
  std::size_t face{}, corner{};
  std::uint32_t source_point{}, source_wedge{};
  double distance{};
};
[[nodiscard]] std::optional<Hit>
pick(const MeshFrame &frame, skeletal::Vec3 origin, skeletal::Vec3 direction);
// Exclusive ownership makes eviction release allocations. Borrowed results are
// valid only until the next mutation. All calls must be on the same worker.
template <class Key, class Value> class BoundedCache {
public:
  explicit BoundedCache(std::size_t budget) : budget_(budget) {}
  bool put(Key key, std::unique_ptr<Value> value, std::size_t bytes) {
    if (!value || bytes > budget_)
      return false;
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (it->key == key) {
        used_ -= it->bytes;
        entries_.erase(it);
        break;
      }
    }
    while (bytes > budget_ - used_) {
      used_ -= entries_.back().bytes;
      entries_.pop_back();
    }
    entries_.push_front({std::move(key), std::move(value), bytes});
    used_ += bytes;
    return true;
  }
  const Value *get(const Key &key) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (it->key == key) {
        entries_.splice(entries_.begin(), entries_, it);
        return entries_.front().value.get();
      }
    }
    return nullptr;
  }
  void clear() noexcept {
    entries_.clear();
    used_ = 0;
  }
  [[nodiscard]] std::size_t bytes() const noexcept { return used_; }

private:
  struct Entry {
    Key key;
    std::unique_ptr<Value> value;
    std::size_t bytes;
  };
  std::list<Entry> entries_;
  std::size_t budget_{}, used_{};
};
} // namespace formaview::preview
