#pragma once
#include <QQuickWindow>
#include <QSGTexture>
#include <d3d11.h>

namespace videoOutput {
bool hdrDesktop(QQuickWindow *window);
QVariantMap presentationInfo(QQuickWindow *window);
bool pqSwapchain(QQuickWindow *window);
QSGTexture *importTexture(QQuickWindow *window,ID3D11Texture2D *texture,QSize size,bool pq);
}
