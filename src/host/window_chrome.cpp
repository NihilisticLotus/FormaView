#include "window_chrome.hpp"
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQmlContext>
#include <QQmlProperty>
#include <QPointer>
#include <memory>

#ifdef Q_OS_WIN
#include <QAbstractNativeEventFilter>
#include <QtCore/qt_windows.h>
#include <windowsx.h>

namespace {
class WindowChrome final : public QObject, public QAbstractNativeEventFilter {
public:
  explicit WindowChrome(QQuickWindow *window) : QObject(window), window_(window) {
    setObjectName(QStringLiteral("nativeWindowChrome"));
    overlay_ = QQmlProperty(window, QStringLiteral("Overlay.overlay"), qmlContext(window)).read().value<QQuickItem *>();
    qGuiApp->installNativeEventFilter(this);
  }
  ~WindowChrome() override {
    if (qGuiApp)
      qGuiApp->removeNativeEventFilter(this);
  }

  bool nativeEventFilter(const QByteArray &, void *message, qintptr *result) override {
    const auto *msg = static_cast<MSG *>(message);
    if (!window_ || msg->message != WM_NCHITTEST ||
        msg->hwnd != reinterpret_cast<HWND>(window_->winId()) ||
        !window_->flags().testFlag(Qt::ExpandedClientAreaHint))
      return false;
    auto *region = window_->findChild<QQuickItem *>(QStringLiteral("windowTitleDrag"));
    if (!region || !region->isVisible() || !region->isEnabled())
      return false;
    if (overlay_)
      for (auto *item : overlay_->childItems())
        if (item->isVisible() && item->width() > 0 && item->height() > 0)
          return false;

    POINT point{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
    RECT rect{};
    if (!GetWindowRect(msg->hwnd, &rect))
      return false;
    if (!IsZoomed(msg->hwnd)) {
      const auto dpi = GetDpiForWindow(msg->hwnd);
      const auto border = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
                          GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
      if (point.x < rect.left + border || point.x >= rect.right - border ||
          point.y < rect.top + border || point.y >= rect.bottom - border)
        return false;
    }
    if (!ScreenToClient(msg->hwnd, &point))
      return false;
    const QPointF scene(point.x / window_->devicePixelRatio(),
                        point.y / window_->devicePixelRatio());
    if (!region->contains(region->mapFromScene(scene)))
      return false;
    // Labels also accept the left mouse button. Only button controls expose
    // "pressed"; let the caption beneath labels and empty space remain draggable.
    if (const auto *bar = window_->findChild<QQuickItem *>(QStringLiteral("topBar")))
      for (const auto *control : bar->childItems())
        if (control->isVisible() && control->property("pressed").isValid() &&
            control->contains(control->mapFromScene(scene)))
          return false;
    // Use native caption hit testing so Windows handles move, snap, double-click
    // and system-menu gestures without a queued startSystemMove request.
    *result = HTCAPTION;
    return true;
  }
private:
  QPointer<QQuickWindow> window_;
  QPointer<QQuickItem> overlay_;
};
}
#endif

void installWindowChrome(QQuickWindow *window) {
#ifdef Q_OS_WIN
  if (window && !window->findChild<QObject *>(QStringLiteral("nativeWindowChrome")))
    std::make_unique<WindowChrome>(window).release(); // QObject parent owns the filter.
#else
  Q_UNUSED(window);
#endif
}
