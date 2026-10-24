#pragma once
#include <QImage>
#include <QQuickWindow>
#include <QThread>

// Qt 6.12's threaded grab path can retain a scene graph after close(),
// while its swapchain has already been released. Do not let capture create
// a new native handle and submit a grab against that missing swapchain.
// Captures here are explicitly of an exposed on-screen window, never an
// off-screen renderer. The caller must report an empty image as unavailable.
inline QImage captureExposedWindow(QQuickWindow *window) {
  if (!window || QThread::currentThread() != window->thread() ||
      !window->handle() || !window->isVisible() || !window->isExposed() ||
      window->visibility() == QWindow::Minimized ||
      window->width() <= 0 || window->height() <= 0 ||
      !window->isSceneGraphInitialized())
    return {};
  return window->grabWindow();
}
