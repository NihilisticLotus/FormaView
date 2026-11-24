#pragma once

#include <QPointF>
#include <QQuickWindow>
#include <QTest>
#include <windows.h>
#include <windowsx.h>
#include <atomic>
#include <chrono>
#include <thread>

namespace monitor_drag {
inline POINT screenPoint(HWND hwnd, QPointF scene) {
  const qreal scale = GetDpiForWindow(hwnd) / 96.;
  POINT pt{qRound(scene.x() * scale), qRound(scene.y() * scale)};
  ClientToScreen(hwnd, &pt);
  return pt;
}

inline bool mouseButton(DWORD flag) {
  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dwFlags = flag;
  return SendInput(1, &input, sizeof(input)) == 1;
}

inline bool click(HWND hwnd, POINT pt) {
  if (GetForegroundWindow() != hwnd || WindowFromPoint(pt) != hwnd ||
      !SetCursorPos(pt.x, pt.y))
    return false;
  return mouseButton(MOUSEEVENTF_LEFTDOWN) && mouseButton(MOUSEEVENTF_LEFTUP);
}

// Exercise Windows' native move loop. The mouse must be released by another
// thread while the Qt GUI thread is handling the non-client button press.
inline bool dragTo(HWND hwnd, HMONITOR target) {
  RECT client{};
  MONITORINFO monitor{sizeof(MONITORINFO)};
  if (!GetClientRect(hwnd, &client) || !GetMonitorInfoW(target, &monitor))
    return false;
  POINT grab{};
  bool found = false;
  for (int x = 25; x < client.right; x += 6) {
    POINT pt{x, MulDiv(23, GetDpiForWindow(hwnd), 96)};
    ClientToScreen(hwnd, &pt);
    if (WindowFromPoint(pt) == hwnd &&
        SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(pt.x, pt.y)) == HTCAPTION) {
      grab = pt;
      found = true;
      break;
    }
  }
  if (!found || GetForegroundWindow() != hwnd)
    return false;
  POINT origin{};
  ClientToScreen(hwnd, &origin);
  const auto work = monitor.rcWork;
  const LONG dx = (work.left + work.right - client.right) / 2 - origin.x;
  const LONG dy = (work.top + work.bottom - client.bottom) / 2 - origin.y;
  std::atomic<bool> done{false};
  bool sent = false;
  std::thread mouse([&] {
    if (SetCursorPos(grab.x, grab.y) && WindowFromPoint(grab) == hwnd &&
        GetForegroundWindow() == hwnd && mouseButton(MOUSEEVENTF_LEFTDOWN)) {
      for (int i = 1; i <= 24; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(18));
        SetCursorPos(grab.x + dx * i / 24, grab.y + dy * i / 24);
      }
      sent = mouseButton(MOUSEEVENTF_LEFTUP);
    }
    done = true;
  });
  while (!done.load())
    QTest::qWait(10);
  mouse.join();
  return sent && MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) == target;
}
}
