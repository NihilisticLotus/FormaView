#pragma once
#include <QMatrix4x4>
#include <QQuickRhiItem>
#include <cstdint>
#include <functional>
#include <array>
#include <memory>
#include <vector>

namespace formaview::render {
// Bytes kept by every live renderer for the largest Params uniform block.
// Observation::owned_gpu_bytes includes this allocation even for preview frames
// that upload only the 80-byte non-skin prefix.
inline constexpr std::uint64_t renderer_uniform_bytes = 176;
inline constexpr std::uint64_t vertex_upload_budget = 256ULL * 1024 * 1024;
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
  int mesh{-1}; // Source mesh of a resident static draw (GPU geometry only).
};
struct Vertex {
  float x{}, y{}, z{}, u{}, v{}, weight{};
  float nx{}, ny{}, nz{1}, r{1}, g{1}, b{1};
  float alpha{1}, alphaMode{}, alphaCutoff{.5F};
};
struct SkinData {
  // Source-point ranges are integer attributes; influences retain every slot.
  std::vector<std::array<std::uint32_t, 2>> ranges;
  std::vector<float> influences; // RGBA texels: cluster, weight, cluster, weight.
  QMatrix4x4 display;
  // Animated morph targets (glTF weights channels): per-point delta pairs in
  // a static texture; per-vertex range (texel base, count | group << 16).
  // Weights ride the palette each pose (groups x stride texels, stable size).
  std::vector<float> morph; // [dPos.xyz,_],[dNorm.xyz,_] per (point,target).
  std::uint32_t morphStride{}; // Max targets of any mesh (row pitch).
  std::uint32_t morphPaletteBase{}; // Palette texel where the group rows start.
  std::vector<std::array<std::uint32_t, 2>> morphRanges; // Per VBO vertex.
  // ActorX reference semantics: normals follow the weighted bone rotation
  // instead of the blended matrix inverse transpose.
  bool boneRotationNormals{};
};
struct Frame {
  std::vector<Vertex> vertices;
  // Geometry owns no pose. Pose frames hold this immutable static payload and
  // a small row-major palette (four RGBA texels per matrix).
  std::shared_ptr<const Frame> geometry;
  std::shared_ptr<const SkinData> skin;
  std::vector<float> palette;
  std::vector<std::array<double,16>> worlds; // Presented pose snapshot for on-demand CPU picking.
  std::vector<std::array<std::uint32_t, 2>> sourceCorners;
  double seconds{};
  int clip{-1};
  std::vector<Vertex> guides;
  std::uint32_t gridVertices{};
  std::uint32_t selectionVertices{};
  std::vector<float>
      rgba; // Selected source subresource converted explicitly to RGBA32F.
  std::vector<std::shared_ptr<const Texture>> textures;
  std::vector<DrawRange> draws;
  // Pose frames of resident geometry list exactly the ranges to draw, even when none.
  bool explicitDraws{};
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
  int clip{-1};
  double seconds{};
  bool gpu_skinning{};
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
  bool showGrid{}, showAxes{}, wireframe{};
  int skinProbe{}; // Diagnostics only: 1 position, 2 normal, same vertex shader.
  // Callbacks run on the render thread. Do not touch GUI objects directly.
  std::function<void(Observation)> observe;
  std::function<void(QString, std::shared_ptr<const Frame>)> skinFailure;
  std::function<void(QString, std::shared_ptr<const Frame>)> frameFailure;
  std::function<void(std::shared_ptr<const Frame>)> frameSubmitted;
  // One-shot diagnostics; use a new Item for another request. Empty bytes and
  // size report unavailable geometry or a readback exceeding 120 render passes.
  std::function<void(QByteArray, QSize)> readback;

protected:
  QQuickRhiItemRenderer *createRenderer() override;
};
} // namespace formaview::render
