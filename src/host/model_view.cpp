#include "model_view.hpp"
#include <QtMath>
#include <QCoreApplication>
#include <QQuickWindow>
#include <algorithm>
#include <cmath>

ModelView::ModelView(QQuickItem *parent) : Item(parent) {
  setSampleCount(4);
  presented_=std::make_shared<std::atomic<std::shared_ptr<const formaview::render::Frame>>>();
  submitted_=std::make_shared<std::atomic<std::shared_ptr<const formaview::render::Frame>>>();
  frameSubmitted=[snapshot=submitted_](auto frame) { snapshot->store(std::move(frame)); };
  connect(this,&QQuickItem::windowChanged,this,&ModelView::bindPresentedWindow);
  bindPresentedWindow(window());
}
void ModelView::bindPresentedWindow(QQuickWindow *value) {
  disconnect(swapConnection_);
  if(session_)session_->attachRenderWindow(value);
  if(value)swapConnection_=connect(value,&QQuickWindow::frameSwapped,this,[submitted=submitted_,presented=presented_] {
    presented->store(submitted->load());
  },Qt::DirectConnection);
}
void ModelView::setSession(ModelSession *value) {
  if (session_ == value)
    return;
  if (session_)
    disconnect(session_, nullptr, this, nullptr);
  session_ = value;
  if (session_) { session_->enableGpuSkinning(true); session_->attachRenderWindow(window()); }
  const QPointer<ModelSession> weak=session_;
  skinFailure=[weak](QString reason,std::shared_ptr<const formaview::render::Frame> failed) {
    QMetaObject::invokeMethod(QCoreApplication::instance(),[weak,reason,failed] { if (weak) weak->gpuSkinningFailed(reason,failed); },Qt::QueuedConnection);
  };
  frameFailure=[weak](QString reason,std::shared_ptr<const formaview::render::Frame> failed) {
    QMetaObject::invokeMethod(QCoreApplication::instance(),[weak,reason,failed] {
      if (weak) weak->renderingFailed(reason,failed);
    },Qt::QueuedConnection);
  };
  if (session_) {
    connect(session_, &ModelSession::frameChanged, this, &ModelView::refresh);
    connect(session_, &ModelSession::documentChanged, this, [this] {
      presented_->store({}); submitted_->store({});
      saved_[0] = {};
      saved_[1] = {};
      fit();
    });
  }
  refresh();
  fit();
  emit sessionChanged();
}
void ModelView::refresh() {
  const bool uv = representation_ == 1 ||
                  (representation_ < 0 && session_ && session_->mode() == 1);
  const auto previousGeometry=frame?frame->geometry:nullptr;
  setFrame(session_ ? (representation_ < 0 ? session_->frame()
                                           : session_->frameFor(uv))
                    : nullptr);
  if (uvMode_ != uv) {
    saved_[uvMode_ ? 1 : 0] = {target_, yaw_, pitch_, distance_};
    uvMode_ = uv;
    const auto &state = saved_[uvMode_ ? 1 : 0];
    target_ = state.target;
    yaw_ = state.yaw;
    pitch_ = state.pitch;
    distance_ = state.distance;
  }
  if (automatic_ && session_ && session_->available() &&
      !(previousGeometry && frame && previousGeometry==frame->geometry))
    fit(fitSelected_);
  else
    camera();
}
void ModelView::camera() {
  const auto aspect =
      static_cast<float>(std::max(width(), 1.0) / std::max(height(), 1.0));
  QMatrix4x4 projection, view;
  if (uvMode_) {
    const float span = static_cast<float>(distance_ / 3.2 * 1.15);
    projection.ortho(-span * aspect, span * aspect, -span, span, -10, 10);
    view.translate(-target_);
  } else {
    projection.perspective(40, aspect, .001F, 1000);
    const double y = qDegreesToRadians(yaw_), p = qDegreesToRadians(pitch_);
    const QVector3D offset(static_cast<float>(std::sin(y) * std::cos(p)),
                           static_cast<float>(std::sin(p)),
                           static_cast<float>(std::cos(y) * std::cos(p)));
    view.lookAt(target_ + offset * static_cast<float>(distance_), target_,
                {0, 1, 0});
  }
  matrix_ = projection * view;
  setViewMatrix(matrix_);
  emit cameraChanged(cameraState());
}
void ModelView::geometryChange(const QRectF &n, const QRectF &o) {
  Item::geometryChange(n, o);
  if (automatic_ && session_ && session_->available())
    fit(fitSelected_);
  else
    camera();
}
void ModelView::orbit(double dx, double dy) {
  if (uvMode_)
    return;
  automatic_ = false;
  yaw_ -= dx * .4;
  pitch_ = std::clamp(pitch_ + dy * .4, -89.0, 89.0);
  camera();
}
void ModelView::panBy(double dx, double dy) {
  automatic_ = false;
  bool ok = false;
  const auto inv = matrix_.inverted(&ok);
  if (!ok)
    return;
  const float z = (matrix_ * QVector4D(target_, 1)).toVector3DAffine().z();
  const auto a = inv.map(QVector3D(0, 0, z)),
             b = inv.map(QVector3D(
                 static_cast<float>(2 * dx / std::max(width(), 1.0)),
                 static_cast<float>(-2 * dy / std::max(height(), 1.0)), z));
  target_ -= b - a;
  camera();
}
void ModelView::zoomBy(double factor) {
  if (!std::isfinite(factor) || factor <= 0)
    return;
  automatic_ = false;
  distance_ = std::clamp(distance_ / factor, .002, uvMode_ ? 1e9 : 300.0);
  camera();
}
void ModelView::fit(bool selected) {
  automatic_ = true;
  fitSelected_ = selected;
  target_ = {};
  distance_ = 3.2;
  yaw_ = 25;
  pitch_ = 12;
  if (session_ && !uvMode_) {
    const auto [center, radius] = session_->focusBounds(selected);
    target_ = center;
    const double aspect = std::max(width(), 1.0) / std::max(height(), 1.0);
    const double half =
        std::min(qDegreesToRadians(20.0),
                 std::atan(std::tan(qDegreesToRadians(20.0)) * aspect));
    distance_ =
        std::max(static_cast<double>(radius) * 1.05 / std::sin(half), .002);
  } else if (session_ && uvMode_) {
    const auto [center, radius] = session_->uvBounds(!selected);
    target_ = center;
    const double aspect = std::max(width(), 1.0) / std::max(height(), 1.0);
    distance_ = std::max(static_cast<double>(radius) * 1.05 *
                             std::max(1.0, 1.0 / aspect) * 3.2 / 1.15,
                         .002);
  }
  camera();
}
void ModelView::pickAt(double x, double y) {
  if (session_ && width() > 0 && height() > 0)
    session_->pick(matrix_, {2 * x / width() - 1, 1 - 2 * y / height()},
                   uvMode_,presented_->load());
}
void ModelView::restoreCamera(const QVariantMap &state) {
  if (state.isEmpty())
    return;
  for (const auto &v : state)
    if (!std::isfinite(v.toDouble()))
      return;
  yaw_ = state.value("yaw", 25).toDouble();
  pitch_ = std::clamp(state.value("pitch", 12).toDouble(), -89.0, 89.0);
  distance_ = std::clamp(state.value("distance", 3.2).toDouble(), .002, 1e9);
  target_ = {state.value("x").toFloat(), state.value("y").toFloat(),
             state.value("z").toFloat()};
  automatic_ = state.value("automatic", false).toBool();
  fitSelected_ = state.value("selected", false).toBool();
  if (automatic_)
    fit(fitSelected_);
  else
    camera();
}
