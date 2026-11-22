#include "video_glass.hpp"
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGTexture>
#include <array>
#include <cstring>

namespace {
class GlassMaterial final : public QSGMaterial {
public:
  GlassMaterial() {
    setFlag(Blending);
    setFlag(NoBatching);
  }
  QSGMaterialType *type() const override {
    static QSGMaterialType type;
    return &type;
  }
  QSGMaterialShader *createShader(QSGRendererInterface::RenderMode) const override;
  QSGTexture *frame = nullptr;
  std::array<float, 8> parameters{};
};

class GlassShader final : public QSGMaterialShader {
public:
  GlassShader() {
    setShaderFileName(VertexStage, QStringLiteral(":/shaders/video_glass.vert.qsb"));
    setShaderFileName(FragmentStage, QStringLiteral(":/shaders/video_glass.frag.qsb"));
  }
  bool updateUniformData(RenderState &state, QSGMaterial *next, QSGMaterial *) override {
    auto *bytes = state.uniformData()->data();
    const auto matrix = state.combinedMatrix();
    std::memcpy(bytes, matrix.constData(), 64);
    const float opacity = state.opacity();
    std::memcpy(bytes + 64, &opacity, sizeof(opacity));
    const auto *glass = static_cast<GlassMaterial *>(next);
    std::memcpy(bytes + 80, glass->parameters.data(), sizeof(glass->parameters));
    return true;
  }
  void updateSampledImage(RenderState &state, int binding, QSGTexture **texture,
                          QSGMaterial *next, QSGMaterial *) override {
    if (binding != 1) return;
    *texture = static_cast<GlassMaterial *>(next)->frame;
    (*texture)->setFiltering(QSGTexture::Linear);
    (*texture)->commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
  }
};

QSGMaterialShader *GlassMaterial::createShader(QSGRendererInterface::RenderMode) const {
  return new GlassShader;
}
} // namespace

VideoGlass::VideoGlass() {
  setGeometry(&geometry_);
  setMaterial(new GlassMaterial);
  setFlag(OwnsMaterial);
}

void VideoGlass::setFrame(const QRectF &panel, const QRectF &videoFrame, QSGTexture *texture) {
  if (panel != panel_) {
    panel_ = panel;
    QSGGeometry::updateTexturedRectGeometry(&geometry_, panel, QRectF(0, 0, 1, 1));
    markDirty(DirtyGeometry);
  }
  auto *glass = static_cast<GlassMaterial *>(material());
  glass->frame = texture;
  glass->parameters = {
      float((panel.x() - videoFrame.x()) / videoFrame.width()),
      float((panel.y() - videoFrame.y()) / videoFrame.height()),
      float(panel.width() / videoFrame.width()),
      float(panel.height() / videoFrame.height()),
      float(panel.width()), float(panel.height()), 22.0f, 0.0f
  };
  markDirty(DirtyMaterial);
}
