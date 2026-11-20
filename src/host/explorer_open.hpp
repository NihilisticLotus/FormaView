#pragma once
#include "desktop.hpp"
#include <QAbstractNativeEventFilter>
#include <QCryptographicHash>
#include <QDir>
#include <QGuiApplication>
#include <QFileInfo>
#include <QLockFile>
#include <QQuickWindow>
#include <QSettings>
#include <QtCore/qt_windows.h>
#include <algorithm>
#include <vector>

// The installed executable and settings profile each own their window set.
class ExplorerOpen final : public QObject, public QAbstractNativeEventFilter {
public:
  ExplorerOpen()
      : group_(QString("FormaView.Open.%1").arg(QString::fromLatin1(
            QCryptographicHash::hash((QCoreApplication::applicationFilePath().toCaseFolded()
                + '|' + QSettings{}.fileName()).toUtf8(), QCryptographicHash::Sha256).toHex())).toStdWString()),
        launchLock_(QDir::temp().filePath(QString::fromStdWString(group_) + ".lock")) {}

  bool forward(const QString &path) {
    const auto key = Desktop::reuseKeyForFile(path);
    if (key.isEmpty() || !QFileInfo(path).isFile() ||
        !QSettings{}.value("explorer/" + key, true).toBool()) return false;
    // Serialize discovery with publication of a cold-start window.
    if (!launchLock_.tryLock(15000)) return false;
    Search search{property(key)};
    EnumWindows(&collect, reinterpret_cast<LPARAM>(&search));
    std::stable_sort(search.windows.begin(), search.windows.end(),
                     [](const auto &a, const auto &b) { return a.first > b.first; });
    COPYDATASTRUCT data{messageId, static_cast<DWORD>(path.size() * sizeof(wchar_t)),
                        const_cast<ushort *>(path.utf16())};
    for (const auto &candidate : search.windows) {
      DWORD pid{};
      GetWindowThreadProcessId(candidate.second, &pid);
      AllowSetForegroundWindow(pid);
      DWORD_PTR accepted{};
      if (SendMessageTimeoutW(candidate.second, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                              SMTO_ABORTIFHUNG | SMTO_BLOCK, 2500, &accepted) && accepted) return true;
    }
    return false;
  }
  void publish(QQuickWindow *window, Desktop *desktop, const QString &startup) {
    window_ = window;
    desktop_ = desktop;
    hwnd_ = reinterpret_cast<HWND>(window->winId());
    qGuiApp->installNativeEventFilter(this);
    const auto update = [this](const QString &key) {
      if (key == key_) return;
      if (!key_.isEmpty()) RemovePropW(hwnd_, property(key_).c_str());
      key_ = key;
      markActive();
    };
    update(Desktop::reuseKeyForFile(startup));
    connect(window, &QWindow::activeChanged, this, [this] {
      if (window_->isActive()) markActive();
    });
    connect(desktop, &Desktop::changed, this, [this, update] {
      const auto asset = desktop_->asset().value("path").toString();
      if (asset != asset_) {
        asset_ = asset;
        update(Desktop::reuseKeyForFile(asset));
      }
    });
    if (launchLock_.isLocked()) launchLock_.unlock();
  }
  ~ExplorerOpen() override {
    if (hwnd_) {
      qGuiApp->removeNativeEventFilter(this);
      if (!key_.isEmpty()) RemovePropW(hwnd_, property(key_).c_str());
    }
  }
private:
  static constexpr ULONG_PTR messageId = 0x46564f31;
  struct Search {
    std::wstring name;
    std::vector<std::pair<ULONG_PTR, HWND>> windows{};
  };
  static BOOL CALLBACK collect(HWND hwnd, LPARAM context) {
    auto &search = *reinterpret_cast<Search *>(context);
    if (const auto stamp = GetPropW(hwnd, search.name.c_str()))
      search.windows.emplace_back(reinterpret_cast<ULONG_PTR>(stamp), hwnd);
    return TRUE;
  }
  std::wstring property(const QString &key) const { return group_ + key.toStdWString(); }
  void markActive() {
    if (!key_.isEmpty()) {
      LARGE_INTEGER now{};
      QueryPerformanceCounter(&now);
      SetPropW(hwnd_, property(key_).c_str(), reinterpret_cast<HANDLE>(now.QuadPart));
    }
  }
  bool nativeEventFilter(const QByteArray &, void *raw, qintptr *result) override {
    const auto &msg = *static_cast<MSG *>(raw);
    if (msg.hwnd != hwnd_ || msg.message != WM_COPYDATA) return false;
    const auto *data = reinterpret_cast<const COPYDATASTRUCT *>(msg.lParam);
    if (!data || data->dwData != messageId) return false;
    *result = 0;
    if (!data->lpData || data->cbData < 2 || data->cbData > 65534 || data->cbData % 2) return true;
    const auto path = QString::fromWCharArray(static_cast<const wchar_t *>(data->lpData), data->cbData / 2);
    if (path.contains(QChar::Null) || !QDir::isAbsolutePath(path) ||
        key_.isEmpty() || Desktop::reuseKeyForFile(path) != key_ || !QFileInfo(path).isFile()) return true;
    desktop_->openPath(path);
    emit desktop_->explorerFileOpened();
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    window_->raise();
    window_->requestActivate();
    markActive();
    *result = 1;
    return true;
  }
  std::wstring group_;
  QLockFile launchLock_;
  HWND hwnd_{};
  QQuickWindow *window_{};
  Desktop *desktop_{};
  QString key_, asset_;
};
