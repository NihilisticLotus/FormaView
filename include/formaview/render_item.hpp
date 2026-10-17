#pragma once
#include <QMatrix4x4>
#include <QQuickRhiItem>
#include <functional>
#include <memory>
#include <vector>

namespace formaview::render {
struct Vertex {
  float x{}, y{}, z{}, u{}, v{}, weight{};
};
struct Frame {
  std::vector<Vertex> vertices;
  std::vector<float>
      rgba; // Selected source subresource converted explicitly to RGBA32F.
  int width{1}, height{1};
  QMatrix4x4 matrix;
  bool heat{};
  std::uint64_t revision{};
};
struct Observation {
  double prepare_ms{}, record_ms{}, gpu_seconds{};
  std::uint64_t owned_gpu_bytes{}, process_gpu_bytes{};
  bool released{};
};
class Item final : public QQuickRhiItem {
public:
  explicit Item(QQuickItem *parent = nullptr);
  // GUI-thread only; immutable payload transferred during synchronize(). No I/O
  // here.
  void setFrame(std::shared_ptr<const Frame>);
  std::shared_ptr<const Frame> frame;
  // Callbacks run on the render thread. Do not touch GUI objects directly.
  std::function<void(Observation)> observe;
  std::function<void(QByteArray, QSize)> readback;

protected:
  QQuickRhiItemRenderer *createRenderer() override;
};
} // namespace formaview::render
