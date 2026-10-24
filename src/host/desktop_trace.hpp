#pragma once
#include <QObject>
#include <chrono>
#include <memory>

class Desktop;
class QQuickWindow;
class QEvent;

// Explicitly enabled diagnostics. No input injection or asset modification.
class DesktopTrace final : public QObject {
public:
  DesktopTrace(QQuickWindow *, Desktop *, QString,
               std::chrono::steady_clock::time_point);
  ~DesktopTrace() override;
  // Final failure is observable by the diagnostic process exit status.
  bool flush(bool final = false);
  void beginDispatch(QObject *, QEvent *);
  void endDispatch();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
