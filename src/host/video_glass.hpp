#pragma once
#include <QSGGeometryNode>

class QSGTexture;
class VideoGlass final : public QSGGeometryNode {
public:
  VideoGlass();
  void setFrame(const QRectF &panel, const QRectF &videoFrame, QSGTexture *texture);
private:
  QSGGeometry geometry_{QSGGeometry::defaultAttributes_TexturedPoint2D(), 4};
  QRectF panel_;
};
