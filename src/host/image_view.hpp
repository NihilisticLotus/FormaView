#pragma once
#include "image_session.hpp"
#include <QPointer>
#include <QQuickRhiItem>
#include <QtQml/qqmlregistration.h>

class ImageView : public QQuickRhiItem {
  Q_OBJECT
  QML_ELEMENT
  Q_PROPERTY(
      ImageSession *session READ session WRITE setSession NOTIFY sessionChanged)
  Q_PROPERTY(double zoom READ zoom WRITE setZoom NOTIFY viewChanged)
  Q_PROPERTY(QPointF pan READ pan WRITE setPan NOTIFY viewChanged)
  Q_PROPERTY(bool fitMode READ fitMode NOTIFY viewChanged)
  Q_PROPERTY(double pixelScale READ pixelScale NOTIFY viewChanged)
  Q_PROPERTY(QString renderError READ renderError NOTIFY renderErrorChanged)
public:
  explicit ImageView(QQuickItem *parent = nullptr);
  ImageSession *session() const { return session_; }
  void setSession(ImageSession *);
  double zoom() const { return zoom_; }
  QPointF pan() const { return pan_; }
  bool fitMode() const { return fit_; }
  double pixelScale() const;
  QString renderError() const { return renderError_; }
  void reportRenderError(const QString &,
                         const std::shared_ptr<const ImagePixels> &);
  void setZoom(double);
  void setPan(QPointF);
  Q_INVOKABLE void fit();
  Q_INVOKABLE void actual();
  Q_INVOKABLE void zoomAt(double factor, double x, double y);
  Q_INVOKABLE void pick(double x, double y);
  QRectF imageRect() const;
signals:
  void sessionChanged();
  void viewChanged();
  void renderErrorChanged();

protected:
  QQuickRhiItemRenderer *createRenderer() override;
  void geometryChange(const QRectF &, const QRectF &) override;
  void itemChange(ItemChange, const ItemChangeData &) override;

private:
  void refresh();
  QPointer<ImageSession> session_;
  std::shared_ptr<const ImagePixels> previous_;
  double zoom_{1};
  QPointF pan_;
  bool fit_{true};
  bool actual_{};
  QString renderError_;
};
