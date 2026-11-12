#include "image_view.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QQuickWindow>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <rhi/qrhi.h>

namespace {
QShader shader(const char *path) {
  QFile f(QString::fromLatin1(path));
  if (!f.open(QIODevice::ReadOnly))
    return {};
  return QShader::fromSerialized(f.readAll());
}
class ImageRenderer final : public QQuickRhiItemRenderer {
public:
  void synchronize(QQuickRhiItem *item) override {
    auto *v = static_cast<ImageView *>(item);
    view_ = v;
    pixels_ = v->session() ? v->session()->pixels() : nullptr;
    display_ = v->session() ? v->session()->display() : QVariantMap{};
    rectangle_ = v->imageRect();
    size_ = v->size();
  }
  void initialize(QRhiCommandBuffer *) override {
    if (owner_ != rhi()) {
      release();
      owner_ = rhi();
    }
  }
  void render(QRhiCommandBuffer *cb) override {
    if (!pixels_) {
      release();
      cb->beginPass(renderTarget(), QColor(34, 35, 38), {1, 0});
      cb->endPass();
      return;
    }
    auto *updates = rhi()->nextResourceUpdateBatch();
    const QSize dimensions(
        static_cast<int>(pixels_->source.data_window.width),
        static_cast<int>(pixels_->source.data_window.height));
    const auto format = pixels_->byteUpload() ? QRhiTexture::RGBA8
                                             : QRhiTexture::RGBA32F;
    if (!texture_ || texture_->pixelSize() != dimensions ||
        texture_->format() != format) {
      release();
      texture_.reset(rhi()->newTexture(format, dimensions));
      uniform_.reset(rhi()->newBuffer(QRhiBuffer::Dynamic,
                                      QRhiBuffer::UniformBuffer, 128));
      sampler_.reset(rhi()->newSampler(
          QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
          QRhiSampler::Repeat, QRhiSampler::Repeat));
      if (!texture_->create() || !uniform_->create() || !sampler_->create()) {
        updates->release();
        failed(cb, "gpu_resource_allocation_failed");
        return;
      }
      bindings_.reset(rhi()->newShaderResourceBindings());
      bindings_->setBindings({QRhiShaderResourceBinding::uniformBuffer(
                                  0,
                                  QRhiShaderResourceBinding::VertexStage |
                                      QRhiShaderResourceBinding::FragmentStage,
                                  uniform_.get()),
                              QRhiShaderResourceBinding::sampledTexture(
                                  1, QRhiShaderResourceBinding::FragmentStage,
                                  texture_.get(), sampler_.get())});
      if (!bindings_->create()) {
        updates->release();
        failed(cb, "gpu_shader_binding_failed");
        return;
      }
    }
    if (uploaded_ != pixels_) {
      updates->uploadTexture(
          texture_.get(), QRhiTextureUploadDescription({QRhiTextureUploadEntry(
                              0, 0,
                              QRhiTextureSubresourceUploadDescription(
                                  pixels_->uploadData(),
                                  static_cast<quint32>(pixels_->uploadBytes())))}));
      uploaded_ = pixels_;
    }
    if (!pipeline_) {
      pipeline_.reset(rhi()->newGraphicsPipeline());
      pipeline_->setShaderStages(
          {{QRhiShaderStage::Vertex, shader(":/shaders/image.vert.qsb")},
           {QRhiShaderStage::Fragment, shader(":/shaders/image.frag.qsb")}});
      pipeline_->setShaderResourceBindings(bindings_.get());
      pipeline_->setRenderPassDescriptor(
          renderTarget()->renderPassDescriptor());
      pipeline_->setSampleCount(renderTarget()->sampleCount());
      if (!pipeline_->create()) {
        updates->release();
        failed(cb, "gpu_pipeline_creation_failed");
        return;
      }
    }
    std::array<float, 32> data{};
    const auto matrix = rhi()->clipSpaceCorrMatrix();
    std::memcpy(data.data(), matrix.constData(), 64);
    data[16] = static_cast<float>(rectangle_.x());
    data[17] = static_cast<float>(rectangle_.y());
    data[18] = static_cast<float>(rectangle_.width());
    data[19] = static_cast<float>(rectangle_.height());
    data[20] = static_cast<float>(size_.width());
    data[21] = static_cast<float>(size_.height());
    data[22] = display_["tiled"].toBool() ? 1.F : 0.F;
    data[23] = static_cast<float>(display_["channel"].toInt());
    data[24] = static_cast<float>(display_["exposure"].toDouble());
    data[25] = display_["colour"].toBool() ? 1.F : 0.F;
    data[26] = display_["srgb"].toBool() ? 1.F : 0.F;
    data[27] = static_cast<float>(display_["alpha"].toInt());
    data[28] =
        display_["composite"].toBool() && pixels_->mapping[3] >= 0 ? 1.F : 0.F;
    data[29] = static_cast<float>(display_["background"].toInt());
    updates->updateDynamicBuffer(uniform_.get(), 0, 128, data.data());
    cb->beginPass(renderTarget(), QColor(34, 35, 38), {1, 0}, updates);
    cb->setGraphicsPipeline(pipeline_.get());
    cb->setViewport({0, 0,
                     static_cast<float>(renderTarget()->pixelSize().width()),
                     static_cast<float>(renderTarget()->pixelSize().height())});
    cb->setShaderResources();
    cb->draw(3);
    cb->endPass();
    report({});
  }

private:
  void release() {
    pipeline_.reset();
    bindings_.reset();
    texture_.reset();
    sampler_.reset();
    uniform_.reset();
    uploaded_.reset();
  }
  void report(const QString &message) {
    if (status_ == message && reportedPixels_.lock() == pixels_)
      return;
    status_ = message;
    reportedPixels_ = pixels_;
    if (auto *application = QCoreApplication::instance())
      QMetaObject::invokeMethod(
          application,
          [view = view_, pixels = pixels_, message] {
            if (view)
              view->reportRenderError(message, pixels);
          },
          Qt::QueuedConnection);
  }
  void failed(QRhiCommandBuffer *cb, const QString &message) {
    release();
    cb->beginPass(renderTarget(), QColor(90, 30, 40), {1, 0});
    cb->endPass();
    report(message);
  }
  QPointer<ImageView> view_;
  QString status_;
  std::weak_ptr<const ImagePixels> reportedPixels_;
  QRhi *owner_{};
  std::shared_ptr<const ImagePixels> pixels_, uploaded_;
  QVariantMap display_;
  QRectF rectangle_;
  QSizeF size_;
  std::unique_ptr<QRhiTexture> texture_;
  std::unique_ptr<QRhiBuffer> uniform_;
  std::unique_ptr<QRhiSampler> sampler_;
  std::unique_ptr<QRhiShaderResourceBindings> bindings_;
  std::unique_ptr<QRhiGraphicsPipeline> pipeline_;
};
} // namespace
ImageView::ImageView(QQuickItem *parent) : QQuickRhiItem(parent) {
  setAlphaBlending(false);
}
void ImageView::setSession(ImageSession *s) {
  if (session_ == s)
    return;
  if (session_)
    disconnect(session_, nullptr, this, nullptr);
  session_ = s;
  if (s) {
    connect(s, &ImageSession::changed, this, &ImageView::refresh);
    connect(s, &ImageSession::displayChanged, this, &ImageView::refresh);
  }
  refresh();
  emit sessionChanged();
}
void ImageView::refresh() {
  const auto next = session_ ? session_->pixels() : nullptr;
  const bool different = !next || !previous_ || next->path != previous_->path;
  if (next != previous_ && !renderError_.isEmpty()) {
    renderError_.clear();
    emit renderErrorChanged();
  }
  previous_ = next;
  if (different) {
    fit_ = true;
    actual_ = false;
  }
  if (fit_)
    fit();
  update();
}
void ImageView::fit() {
  fit_ = true;
  actual_ = false;
  pan_ = {};
  if (previous_) {
    const auto display = session_->display()["displayWindow"].toBool();
    const auto &w = display ? previous_->source.display_window
                            : previous_->source.data_window;
    zoom_ = std::max(.0001, std::min(width() / static_cast<double>(w.width),
                                     height() / static_cast<double>(w.height)) *
                                .92);
  }
  emit viewChanged();
  update();
}
void ImageView::actual() {
  fit_ = false;
  actual_ = true;
  pan_ = {};
  zoom_ = 1. / (window() ? window()->effectiveDevicePixelRatio() : 1.);
  emit viewChanged();
  update();
}
void ImageView::setZoom(double z) {
  if (!std::isfinite(z))
    return;
  fit_ = false;
  actual_ = false;
  zoom_ = std::clamp(z, .0001, 256.);
  emit viewChanged();
  update();
}
void ImageView::setPan(QPointF p) {
  if (!std::isfinite(p.x()) || !std::isfinite(p.y()))
    return;
  fit_ = false;
  pan_ = p;
  emit viewChanged();
  update();
}
void ImageView::zoomAt(double factor, double x, double y) {
  if (!std::isfinite(factor) || factor <= 0)
    return;
  const auto old = zoom_;
  setZoom(zoom_ * factor);
  const QPointF centre(width() / 2, height() / 2), cursor(x, y);
  pan_ = cursor - centre - (cursor - centre - pan_) * (zoom_ / old);
  emit viewChanged();
  update();
}
QRectF ImageView::imageRect() const {
  if (!previous_)
    return {};
  const auto &data = previous_->source.data_window;
  const auto &w = session_->display()["displayWindow"].toBool()
                      ? previous_->source.display_window
                      : data;
  const auto x = (static_cast<double>(data.x) - static_cast<double>(w.x) -
                  static_cast<double>(w.width) / 2) *
                 zoom_;
  const auto y = (static_cast<double>(data.y) - static_cast<double>(w.y) -
                  static_cast<double>(w.height) / 2) *
                 zoom_;
  return {width() / 2 + pan_.x() + x, height() / 2 + pan_.y() + y,
          static_cast<double>(data.width) * zoom_,
          static_cast<double>(data.height) * zoom_};
}
bool ImageView::pick(double x, double y) {
  // The sample and its lock share a session lifetime. Hover events from a
  // replacement pane or a window resize cannot overwrite a pinned sample.
  if (session_ && session_->samplePinned())
    return false;
  return pickAt(x, y, false);
}
bool ImageView::pinAt(double x, double y) { return pickAt(x, y, true); }
bool ImageView::pickAt(double x, double y, bool pin) {
  if (!previous_ || !session_ || session_->busy() ||
      !session_->error().isEmpty() || !std::isfinite(x) || !std::isfinite(y) ||
      x < 0 || y < 0 || x >= width() || y >= height())
    return false;
  const auto r = imageRect();
  const auto &w = previous_->source.data_window;
  auto px = (x - r.x()) / zoom_, py = (y - r.y()) / zoom_;
  int tx = 0, ty = 0;
  if (session_->display()["tiled"].toBool()) {
    tx = static_cast<int>(std::floor(px / static_cast<double>(w.width)));
    ty = static_cast<int>(std::floor(py / static_cast<double>(w.height)));
    px -= tx * static_cast<double>(w.width);
    py -= ty * static_cast<double>(w.height);
  }
  // Leaving the image is not a new sample. Keep the last valid readout;
  // session loading/clearing still invalidates samples from the old image.
  if (px < 0 || py < 0 || px >= static_cast<double>(w.width) ||
      py >= static_cast<double>(w.height))
    return false;
  session_->sample(std::floor(px) + static_cast<double>(w.x),
                   std::floor(py) + static_cast<double>(w.y), tx, ty);
  if (pin)
    session_->setSamplePinned(true);
  return session_->pixel().value("valid").toBool();
}
void ImageView::geometryChange(const QRectF &now, const QRectF &old) {
  QQuickRhiItem::geometryChange(now, old);
  if (fit_)
    fit();
}
QQuickRhiItemRenderer *ImageView::createRenderer() { return new ImageRenderer; }
double ImageView::pixelScale() const {
  return zoom_ * (window() ? window()->effectiveDevicePixelRatio() : 1.);
}
void ImageView::itemChange(ItemChange change, const ItemChangeData &data) {
  QQuickRhiItem::itemChange(change, data);
  if (change == ItemDevicePixelRatioHasChanged || change == ItemSceneChange) {
    if (actual_)
      zoom_ = 1. / (window() ? window()->effectiveDevicePixelRatio() : 1.);
    emit viewChanged();
    update();
  }
}
void ImageView::reportRenderError(
    const QString &message, const std::shared_ptr<const ImagePixels> &pixels) {
  if (pixels != previous_ || renderError_ == message)
    return;
  renderError_ = message;
  emit renderErrorChanged();
}
