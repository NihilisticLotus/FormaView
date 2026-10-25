#include "window_chrome.hpp"
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQmlContext>
#include <QQmlProperty>
#include <QPointer>
#include <cmath>
#include <memory>
#include <utility>

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
    updateCaptionInset();
    connect(window, &QWindow::screenChanged, this, [this] {
      QMetaObject::invokeMethod(this, [this] { updateCaptionInset(); }, Qt::QueuedConnection);
    });
  }
  ~WindowChrome() override {
    if (qGuiApp)
      qGuiApp->removeNativeEventFilter(this);
  }

  bool nativeEventFilter(const QByteArray &, void *message, qintptr *result) override {
    const auto *msg = static_cast<MSG *>(message);
    // winId() creates a platform window. Never call it from messages emitted
    // during that same creation: the platform handle is not installed yet.
    if (!window_ || !window_->handle() ||
        msg->hwnd != reinterpret_cast<HWND>(window_->winId()) ||
        !window_->flags().testFlag(Qt::ExpandedClientAreaHint))
      return false;
    const auto captionButton = [](WPARAM hit) {
      return hit == HTCLOSE || hit == HTMINBUTTON || hit == HTMAXBUTTON;
    };
    // Expanded-client caption buttons must follow press/release semantics.
    // Capture the gesture before the platform's press handler can execute it.
    if ((msg->message == WM_NCLBUTTONDOWN || msg->message == WM_NCLBUTTONDBLCLK) &&
        captionButton(msg->wParam) && IsWindowEnabled(msg->hwnd)) {
      pressedCaption_ = static_cast<int>(msg->wParam);
      SetCapture(msg->hwnd);
      if (result) *result = 0;
      return true;
    }
    if (pressedCaption_) {
      if (msg->message == WM_CANCELMODE || msg->message == WM_CAPTURECHANGED ||
          (msg->message == WM_KEYDOWN && msg->wParam == VK_ESCAPE) ||
          (msg->message == WM_ACTIVATE && LOWORD(msg->wParam) == WA_INACTIVE)) {
        pressedCaption_ = 0;
        if (GetCapture() == msg->hwnd)
          ReleaseCapture();
      } else if (msg->message == WM_LBUTTONUP || msg->message == WM_NCLBUTTONUP) {
        POINT point{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
        if (msg->message == WM_LBUTTONUP)
          ClientToScreen(msg->hwnd, &point);
        const auto pressed = std::exchange(pressedCaption_, 0);
        if (GetCapture() == msg->hwnd)
          ReleaseCapture();
        const auto hit = SendMessageW(msg->hwnd, WM_NCHITTEST, 0,
                                       MAKELPARAM(point.x, point.y));
        if (hit == pressed) {
          const WPARAM command = pressed == HTCLOSE ? SC_CLOSE
              : pressed == HTMINBUTTON ? SC_MINIMIZE
              : IsZoomed(msg->hwnd) ? SC_RESTORE : SC_MAXIMIZE;
          PostMessageW(msg->hwnd, WM_SYSCOMMAND, command, 0);
        }
        if (result) *result = 0;
        return true;
      } else if (msg->message == WM_MOUSEMOVE) {
        if (result) *result = 0;
        return true;
      }
    }
    if (msg->message == WM_DPICHANGED || msg->message == WM_SIZE || msg->message == WM_SHOWWINDOW)
      QMetaObject::invokeMethod(this, [this] { updateCaptionInset(); }, Qt::QueuedConnection);
    if (msg->message != WM_NCHITTEST)
      return false;
    // All valid expanded-client queries are pure: delegating even a client
    // strip can synchronously synthesize input or execute a caption action.
    const auto clientHit = [result] {
      if (result) *result = HTCLIENT;
      return true;
    };
    // Qt 6.12's expanded-area hit test performs close/min/max immediately
    // when GetAsyncKeyState reports a pressed mouse button. Return pure hit
    // codes here, before reaching that handler. Match Qt's painted geometry
    // using the current window DPI; the outer resize border keeps priority.
    POINT buttonPoint{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
    RECT windowRect{}, clientRect{};
    if (GetWindowRect(msg->hwnd, &windowRect) && GetClientRect(msg->hwnd, &clientRect)) {
      const auto dpi = GetDpiForWindow(msg->hwnd);
      const int thickness = GetSystemMetricsForDpi(SM_CXSIZEFRAME,dpi) +
                            GetSystemMetricsForDpi(SM_CXPADDEDBORDER,dpi);
      const int border = IsZoomed(msg->hwnd) ? 0 : thickness;
      const bool resize = buttonPoint.x < windowRect.left+border ||
          buttonPoint.x >= windowRect.right-border ||
          buttonPoint.y < windowRect.top+border || buttonPoint.y >= windowRect.bottom-border;
      if (resize && window_->visibility() != QWindow::FullScreen) {
        // Returning false here enters Qt's expanded-area hit test, which
        // samples asynchronous mouse state before its own border handling.
        // Keep resize queries pure just like the caption-button queries.
        const bool left = buttonPoint.x < windowRect.left+border;
        const bool right = buttonPoint.x >= windowRect.right-border;
        const bool top = buttonPoint.y < windowRect.top+border;
        const bool bottom = buttonPoint.y >= windowRect.bottom-border;
        const int hit = top ? (left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP)
            : bottom ? (left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM)
            : left ? HTLEFT : HTRIGHT;
        if (result) *result = hit;
        return true;
      }
      if (!resize && ScreenToClient(msg->hwnd,&buttonPoint) &&
          window_->visibility() != QWindow::FullScreen) {
        // Maximized native rectangles include invisible pixels outside the
        // client area. Such queries cannot target a caption action.
        if (buttonPoint.x < 0 || buttonPoint.y < 0 ||
            buttonPoint.x >= clientRect.right || buttonPoint.y >= clientRect.bottom) {
          if (result) *result = HTNOWHERE;
          return true;
        }
        const int height = thickness + GetSystemMetricsForDpi(SM_CYCAPTION,dpi);
        const int width = static_cast<int>(height * 1.5);
        int right = clientRect.right;
        const auto flags = window_->flags();
        for (const auto &[flag,hit] : {
                 std::pair{Qt::WindowCloseButtonHint, HTCLOSE},
                 std::pair{Qt::WindowMaximizeButtonHint, HTMAXBUTTON},
                 std::pair{Qt::WindowMinimizeButtonHint, HTMINBUTTON}}) {
          if (!flags.testFlag(flag)) continue;
          if (buttonPoint.y >= 0 && buttonPoint.y < height &&
              buttonPoint.x >= right-width && buttonPoint.x < right) {
            if (result) *result = hit;
            return true;
          }
          right -= width;
        }
      }
    }
    auto *region = window_->findChild<QQuickItem *>(QStringLiteral("windowTitleDrag"));
    if (!region || !region->isVisible() || !region->isEnabled())
      return clientHit();

    POINT point{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
    RECT rect{};
    if (!GetWindowRect(msg->hwnd, &rect))
      return clientHit();
    if (!IsZoomed(msg->hwnd)) {
      const auto dpi = GetDpiForWindow(msg->hwnd);
      const auto border = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
                          GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
      if (point.x < rect.left + border || point.x >= rect.right - border ||
          point.y < rect.top + border || point.y >= rect.bottom - border)
        return clientHit();
    }
    if (!ScreenToClient(msg->hwnd, &point))
      return clientHit();
    const QPointF scene(point.x / window_->devicePixelRatio(),
                        point.y / window_->devicePixelRatio());
    if (!region->contains(region->mapFromScene(scene)))
      return clientHit();
    // Qt's expanded-client fallback samples GetAsyncKeyState and may
    // synchronously dispatch mouse/window actions from WM_NCHITTEST.
    // Own the client hit as well as the caption hit: a query must not
    // become a nested close, popup, or scene-graph mutation.
    if (overlay_)
      for (auto *item : overlay_->childItems())
        if (item->isVisible() && item->width() > 0 && item->height() > 0)
          return clientHit();
    // Labels also accept the left mouse button. Only button controls expose
    // "pressed"; let the caption beneath labels and empty space remain draggable.
    if (const auto *bar = window_->findChild<QQuickItem *>(QStringLiteral("topBar")))
      for (const auto *control : bar->childItems())
        if (control->isVisible() && control->property("pressed").isValid() &&
            control->contains(control->mapFromScene(scene)))
          return clientHit();
    // Use native caption hit testing so Windows handles move, snap, double-click
    // and system-menu gestures without a queued startSystemMove request.
    if (result) *result = HTCAPTION;
    return true;
  }
private:
  void updateCaptionInset() {
    if (!window_ || !window_->handle() ||
        window_->metaObject()->indexOfProperty("nativeCaptionInset") < 0)
      return;
    const auto dpi = GetDpiForWindow(reinterpret_cast<HWND>(window_->winId()));
    if (!dpi) return;
    const auto thickness = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
                           GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    const auto height = thickness + GetSystemMetricsForDpi(SM_CYCAPTION, dpi);
    const auto width = static_cast<int>(height * 1.5);
    const auto flags = window_->flags();
    const auto count = int(flags.testFlag(Qt::WindowCloseButtonHint)) +
                       int(flags.testFlag(Qt::WindowMaximizeButtonHint)) +
                       int(flags.testFlag(Qt::WindowMinimizeButtonHint));
    // Native caption pixels follow Windows DPI, while QML may additionally
    // use QT_SCALE_FACTOR. Reserve the painted/hit-tested native strip in
    // QML coordinates, including a small gap beside the application menu.
    window_->setProperty("nativeCaptionInset",
        std::ceil(width * count / window_->devicePixelRatio()) + 8.);
  }
  QPointer<QQuickWindow> window_;
  QPointer<QQuickItem> overlay_;
  int pressedCaption_{};
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
