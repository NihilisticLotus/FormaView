#pragma once
#include "model_session.hpp"
#include <QPointer>
#include <QVector3D>
#include <QtQml/qqmlregistration.h>
struct RenderItemForeign {
  Q_GADGET
  QML_FOREIGN(formaview::render::Item)
  QML_ANONYMOUS
};

class ModelView : public formaview::render::Item {
  Q_OBJECT
  QML_ELEMENT
  Q_PROPERTY(
      ModelSession *session READ session WRITE setSession NOTIFY sessionChanged)
  Q_PROPERTY(int representation READ representation WRITE setRepresentation
                 NOTIFY representationChanged)
  Q_PROPERTY(
      bool showGrid READ gridVisible WRITE setGridVisible NOTIFY guidesChanged)
  Q_PROPERTY(
      bool showAxes READ axesVisible WRITE setAxesVisible NOTIFY guidesChanged)
public:
  explicit ModelView(QQuickItem *parent = nullptr);
  ModelSession *session() const { return session_; }
  int representation() const { return representation_; }
  bool gridVisible() const { return showGrid; }
  bool axesVisible() const { return showAxes; }
  void setGridVisible(bool value) {
    if (showGrid != value) {
      showGrid = value;
      update();
      emit guidesChanged();
    }
  }
  void setAxesVisible(bool value) {
    if (showAxes != value) {
      showAxes = value;
      update();
      emit guidesChanged();
    }
  }
  void setRepresentation(int v) {
    if (v < -1 || v > 1 || v == representation_)
      return;
    representation_ = v;
    refresh();
    emit representationChanged();
  }
  void setSession(ModelSession *);
  Q_INVOKABLE void orbit(double dx, double dy);
  Q_INVOKABLE void panBy(double dx, double dy);
  Q_INVOKABLE void zoomBy(double factor);
  Q_INVOKABLE void fit(bool selected = false);
  Q_INVOKABLE void pickAt(double x, double y);
  Q_INVOKABLE QVariantMap cameraState() const {
    return {{"yaw", yaw_},
            {"pitch", pitch_},
            {"distance", distance_},
            {"x", target_.x()},
            {"y", target_.y()},
            {"z", target_.z()},
            {"automatic", automatic_},
            {"selected", fitSelected_}};
  }
  Q_INVOKABLE void restoreCamera(const QVariantMap &);
signals:
  void sessionChanged();
  void representationChanged();
  void guidesChanged();
  void cameraChanged(QVariantMap state);

protected:
  void geometryChange(const QRectF &, const QRectF &) override;

private:
  void refresh();
  void camera();
  QPointer<ModelSession> session_;
  QMatrix4x4 matrix_;
  QVector3D target_;
  double yaw_{25}, pitch_{12}, distance_{3.2};
  bool uvMode_{};
  bool automatic_{true}, fitSelected_{};
  int representation_{-1};
  struct CameraState {
    QVector3D target;
    double yaw{25}, pitch{12}, distance{3.2};
  };
  CameraState saved_[2];
};
