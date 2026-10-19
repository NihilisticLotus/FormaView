#include "model_view.hpp"
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickGraphicsConfiguration>
#include <QQuickWindow>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <windows.h>

#include <psapi.h>

namespace {
using Clock = std::chrono::steady_clock;
double milliseconds(Clock::duration value) {
  return std::chrono::duration<double, std::milli>(value).count();
}
QJsonObject distribution(std::vector<double> values) {
  if (values.empty())
    return {{"count", 0}};
  std::sort(values.begin(), values.end());
  const auto at = [&](double p) {
    return values[static_cast<std::size_t>(std::ceil(p * values.size())) - 1];
  };
  return {{"count", static_cast<int>(values.size())},
          {"p95_ms", at(.95)},
          {"p99_ms", at(.99)},
          {"max_ms", values.back()},
          {"over_100ms",
           static_cast<int>(std::count_if(values.begin(), values.end(),
                                          [](double v) { return v > 100; }))}};
}
class Application : public QGuiApplication {
public:
  using QGuiApplication::QGuiApplication;
  bool measuring{};
  std::vector<double> events;
  bool notify(QObject *object, QEvent *event) override {
    if (!measuring)
      return QGuiApplication::notify(object, event);
    const auto start = Clock::now();
    const auto result = QGuiApplication::notify(object, event);
    events.push_back(milliseconds(Clock::now() - start));
    return result;
  }
};
std::uint64_t rss() {
  PROCESS_MEMORY_COUNTERS_EX memory{};
  memory.cb = sizeof(memory);
  if (!GetProcessMemoryInfo(
          GetCurrentProcess(),
          reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory)))
    throw std::runtime_error("Process memory query failed");
  return memory.WorkingSetSize;
}
void waitFor(const std::function<bool()> &predicate, int timeout = 60000) {
  if (predicate())
    return;
  QEventLoop loop;
  QTimer poll, deadline;
  poll.setInterval(1);
  deadline.setSingleShot(true);
  QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
    if (predicate())
      loop.quit();
  });
  QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
  poll.start();
  deadline.start(timeout);
  loop.exec();
  if (!predicate())
    throw std::runtime_error("Timed out waiting for actual model/frame state");
}
} // namespace

int main(int argc, char **argv) {
  QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
  Application app(argc, argv);
  if (app.arguments().size() != 4)
    return 64;
  const auto cloud = app.arguments()[1], lod = app.arguments()[2],
             output = app.arguments()[3];
  QJsonObject evidence{
      {"qt", qVersion()},
      {"boundary",
       "Passive real Qt/D3D11 window; C++ camera command to matching "
       "frameSwapped signal, not OS input or display presentation. Cold parse, "
       "upload and GPU residency hits separated using renderer observations."},
      {"render_width", 1920},
      {"render_height", 1080},
      {"filesystem_cache", "OS caches are not flushed; first load in this "
                           "process is not physical cold-disk I/O"},
      {"msaa", 4}};
  std::atomic<std::uint64_t> generation{}, synchronized{};
  struct Shared {
    std::mutex mutex;
    std::uint64_t swapGeneration{}, frames{}, uploadBytes{}, residentHits{},
        peakOwned{}, peakProcess{};
    Clock::time_point swapTime;
    std::vector<double> intervals, prepare, record, gpu;
    formaview::render::Observation last;
    bool measuring{};
  } shared;
  ModelSession session;
  QQuickWindow window;
  QQuickGraphicsConfiguration graphics;
  graphics.setTimestamps(true);
  window.setGraphicsConfiguration(graphics);
  window.setPersistentGraphics(false);
  window.setPersistentSceneGraph(false);
  window.setFlags(Qt::Window | Qt::WindowDoesNotAcceptFocus |
                  Qt::WindowTransparentForInput);
  window.resize(1280, 720);
  auto *view = new ModelView(window.contentItem());
  view->setWidth(1280);
  view->setHeight(720);
  view->setFixedColorBufferWidth(1920);
  view->setFixedColorBufferHeight(1080);
  view->setSession(&session);
  view->observe = [&](formaview::render::Observation value) {
    std::lock_guard guard(shared.mutex);
    shared.last = value;
    shared.peakOwned = std::max(shared.peakOwned, value.owned_gpu_bytes);
    shared.peakProcess = std::max(shared.peakProcess, value.process_gpu_bytes);
    if (shared.measuring) {
      shared.prepare.push_back(value.prepare_ms);
      shared.record.push_back(value.record_ms);
      if (value.gpu_seconds > 0)
        shared.gpu.push_back(value.gpu_seconds * 1000);
      shared.uploadBytes += value.uploaded_bytes;
      shared.residentHits += value.resident_hit ? 1 : 0;
    }
  };
  QObject::connect(
      &window, &QQuickWindow::afterSynchronizing, &window,
      [&] { synchronized.store(generation.load()); }, Qt::DirectConnection);
  QObject::connect(
      &window, &QQuickWindow::frameSwapped, &window,
      [&] {
        const auto stamp = Clock::now();
        const auto token = synchronized.load();
        std::lock_guard guard(shared.mutex);
        if (shared.measuring && shared.swapTime.time_since_epoch().count())
          shared.intervals.push_back(milliseconds(stamp - shared.swapTime));
        shared.swapTime = stamp;
        shared.swapGeneration = token;
        ++shared.frames;
      },
      Qt::DirectConnection);
  const auto presented = [&] {
    const auto token = ++generation;
    window.update();
    waitFor([&] {
      std::lock_guard guard(shared.mutex);
      return shared.swapGeneration >= token;
    });
    std::lock_guard guard(shared.mutex);
    return shared.swapTime;
  };
  const auto ready = [&] {
    waitFor([&] { return !session.busy() && !session.preparing(); });
    if (!session.inspectable())
      throw std::runtime_error(
          (session.error() + session.frameError()).toStdString());
  };
  const auto load = [&](const QString &path) {
    const auto start = Clock::now();
    session.open(path);
    ready();
    const auto parsed = Clock::now();
    const auto displayed = presented();
    return QJsonObject{
        {"path", path},
        {"parse_and_frame_ms", milliseconds(parsed - start)},
        {"first_actual_frame_ms", milliseconds(displayed - start)},
        {"source", QJsonObject::fromVariantMap(session.info())}};
  };
  const auto begin = [&] {
    std::lock_guard guard(shared.mutex);
    shared.prepare.clear();
    shared.record.clear();
    shared.gpu.clear();
    shared.intervals.clear();
    shared.swapTime = {};
    shared.uploadBytes = 0;
    shared.residentHits = 0;
    shared.measuring = true;
    app.events.clear();
    app.measuring = true;
  };
  const auto finish = [&](const std::vector<double> &latency) {
    app.measuring = false;
    std::lock_guard guard(shared.mutex);
    shared.measuring = false;
    return QJsonObject{
        {"command_to_swap", distribution(latency)},
        {"frame_intervals", distribution(shared.intervals)},
        {"render_prepare", distribution(shared.prepare)},
        {"render_record", distribution(shared.record)},
        {"gpu_completed", distribution(shared.gpu)},
        {"notify_wall", distribution(app.events)},
        {"uploaded_bytes", static_cast<qint64>(shared.uploadBytes)},
        {"resident_hits", static_cast<qint64>(shared.residentHits)},
        {"rss_bytes", static_cast<qint64>(rss())}};
  };
  bool passed = false;
  try {
    evidence["rss_before"] = static_cast<qint64>(rss());
    window.show();
    presented();
    evidence["first_cloud"] = load(cloud);
    evidence["effective_width"] = view->effectiveColorBufferSize().width();
    evidence["effective_height"] = view->effectiveColorBufferSize().height();
    evidence["window_dpr"] = window.devicePixelRatio();
    if (view->effectiveColorBufferSize() != QSize(1920, 1080))
      throw std::runtime_error("Benchmark render target is not actually 1080p");
    QJsonArray interactions;
    for (const int mode : {0, 1, 2}) {
      session.setMode(mode);
      ready();
      if (mode == 2 && session.bindingIndex() < 0)
        throw std::runtime_error(
            "Performance source has no selected skin binding");
      for (int i = 0; i < 30; ++i) {
        if (mode == 1)
          view->panBy(i % 2 ? -1 : 1, 0);
        else
          view->orbit(.5, 0);
        presented();
      }
      begin();
      std::vector<double> latency;
      for (int i = 0; i < 240; ++i) {
        const auto start = Clock::now();
        if (mode == 1)
          view->panBy(i % 2 ? -1 : 1, 0);
        else
          view->orbit(i % 2 ? -.8 : .8, .1);
        latency.push_back(milliseconds(presented() - start));
      }
      auto result = finish(latency);
      result["mode"] = mode;
      result["representation"] = mode == 1   ? "UV panel, original UV wireframe"
                                 : mode == 2 ? "source weight heatmap"
                                             : "shaded source geometry";
      interactions.append(result);
    }
    evidence["camera"] = interactions;
    evidence["first_lod"] = load(lod);
    session.selectLodGroup(0);
    const auto levels = session.lodLevels();
    if (levels.size() < 2)
      throw std::runtime_error("Explicit LOD source requires two levels");
    QJsonArray coldLevels;
    for (int i = 0; i < 2; ++i) {
      const auto start = Clock::now();
      session.requestLod(levels[i].toMap().value("level").toInt());
      ready();
      coldLevels.append(milliseconds(presented() - start));
    }
    evidence["first_lod_switch_ms"] = coldLevels;
    begin();
    std::vector<double> warm;
    for (int i = 0; i < 80; ++i) {
      const auto start = Clock::now();
      session.requestLod(levels[(i + 1) % 2].toMap().value("level").toInt());
      ready();
      warm.push_back(milliseconds(presented() - start));
    }
    const auto resident = finish(warm);
    evidence["resident_lod"] = resident;
    if (resident.value("uploaded_bytes").toInteger() != 0 ||
        resident.value("resident_hits").toInteger() < 79)
      throw std::runtime_error(
          "Warm LOD result must use resident GPU buffers without re-upload");
    QJsonArray release;
    for (int i = 0; i < 6; ++i) {
      load(i % 2 ? lod : cloud);
      session.clear();
      presented();
      std::lock_guard guard(shared.mutex);
      if (shared.last.owned_gpu_bytes > 80)
        throw std::runtime_error(
            "Geometry/texture resources retained after clear");
      release.append(QJsonObject{
          {"cycle", i},
          {"rss", static_cast<qint64>(rss())},
          {"owned_gpu", static_cast<qint64>(shared.last.owned_gpu_bytes)},
          {"process_gpu", static_cast<qint64>(shared.last.process_gpu_bytes)}});
    }
    evidence["clear_cycles"] = release;
    view->setSession(nullptr);
    delete view;
    window.releaseResources();
    window.hide();
    waitFor([&] {
      std::lock_guard guard(shared.mutex);
      return shared.last.released;
    });
    passed = true;
  } catch (const std::exception &error) {
    evidence["failure"] = QString::fromUtf8(error.what());
  }
  app.measuring = false;
  {
    std::lock_guard guard(shared.mutex);
    evidence["peak_owned_gpu_bytes"] = static_cast<qint64>(shared.peakOwned);
    evidence["peak_process_gpu_bytes"] =
        static_cast<qint64>(shared.peakProcess);
    evidence["renderer_released"] = shared.last.released;
  }
  evidence["measurement_completed"] = passed;
  QFile file(output);
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(QJsonDocument(evidence).toJson()) < 0)
    return 74;
  std::cout << (passed ? "MEASURED" : "FAILED")
            << " actual 1080p model workload\n";
  return passed ? 0 : 1;
}
