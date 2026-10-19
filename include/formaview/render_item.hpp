#pragma once
#include <QMatrix4x4>
#include <QQuickRhiItem>
#include <functional>
#include <memory>
#include <vector>

namespace formaview::render {
struct Texture {
  std::vector<float> rgba;
  int width{}, height{};
  int wrapU{}, wrapV{}; // 0 repeat, 1 clamp, 2 mirror.
  bool linearMin{}, linearMag{}, mipmaps{}, linearMip{};
};
struct DrawRange {
  std::uint32_t first{}, count{};
  int texture{-1};
  bool blend{};
  QVector3D center;
};
struct Vertex {
  float x{}, y{}, z{}, u{}, v{}, weight{};
  float nx{}, ny{}, nz{1}, r{1}, g{1}, b{1};
  float alpha{1}, alphaMode{}, alphaCutoff{.5F};
};
struct Frame {
  std::vector<Vertex> vertices;
  std::vector<Vertex> guides;
  std::uint32_t gridVertices{};
  std::uint32_t selectionVertices{};
  std::vector<float>
      rgba; // Selected source subresource converted explicitly to RGBA32F.
  std::vector<std::shared_ptr<const Texture>> textures;
  std::vector<DrawRange> draws;
  int width{1}, height{1};
  QMatrix4x4 matrix;
  bool heat{};
  bool lit{}, lines{};
  bool uv{};
  std::uint32_t backgroundVertices{};
  int backgroundTexture{-1};
  std::uint64_t sourcePoints{}, sourceTriangles{};
  QVector3D displayLow, displayHigh;
  bool hasDisplayBounds{};
  bool linearColour{};
  bool cacheable{};
  std::uint64_t revision{};
};
struct Observation {
  double prepare_ms{}, record_ms{}, gpu_seconds{};
  std::uint64_t owned_gpu_bytes{}, process_gpu_bytes{};
  bool released{};
  std::uint64_t frame_revision{}, uploaded_bytes{};
  std::uint32_t resident_frames{};
  bool resident_hit{};
};
class Item : public QQuickRhiItem {
  Q_OBJECT
public:
  explicit Item(QQuickItem *parent = nullptr);
  // GUI-thread only; immutable payload transferred during synchronize(). No I/O
  // here.
  void setFrame(std::shared_ptr<const Frame>);
  void setViewMatrix(const QMatrix4x4 &value) {
    viewMatrix = value;
    useViewMatrix = true;
    update();
  }
  std::shared_ptr<const Frame> frame;
  QMatrix4x4 viewMatrix;
  bool useViewMatrix{};
  bool showGrid{}, showAxes{};
  // Callbacks run on the render thread. Do not touch GUI objects directly.
  std::function<void(Observation)> observe;
  std::function<void(QByteArray, QSize)> readback;

protected:
  QQuickRhiItemRenderer *createRenderer() override;
};
} // namespace formaview::render
