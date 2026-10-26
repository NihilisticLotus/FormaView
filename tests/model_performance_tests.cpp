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
#include <QThread>
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
  QJsonArray raw;
  for (const auto value : values)
    raw.append(value);
  std::sort(values.begin(), values.end());
  const auto at = [&](double p) {
    return values[static_cast<std::size_t>(std::ceil(p * values.size())) - 1];
  };
  return {{"count", static_cast<int>(values.size())},
          {"samples_ms", raw},
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
    if (QThread::currentThread() != thread() || !measuring)
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
  evidence["forced_geometry_copy"]=qEnvironmentVariableIsSet("FORMAVIEW_PERF_FORCE_GEOMETRY_COPY");
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
  std::uint64_t previousRowUpload{};
  const auto begin = [&] {
    std::lock_guard guard(shared.mutex);
    shared.prepare.clear();
    shared.record.clear();
    shared.gpu.clear();
    shared.intervals.clear();
    shared.swapTime = {};
    shared.uploadBytes = 0;
    previousRowUpload = 0;
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
  const auto residencyRow = [&](int cycle, const std::shared_ptr<const formaview::render::Frame> &before,
                                std::uint64_t buildsBefore) {
    const auto frame = session.frame();
    // The pinned LOD fixture has a small palette that occupies one RGBA32F row.
    // Refuse a different shape rather than silently widening the upload budget.
    if (frame->geometry && (frame->palette.empty() || frame->palette.size()>4096 || frame->palette.size()%4))
      throw std::runtime_error("Residency fixture must use a nonempty single-row palette");
    const auto dynamicBytes = frame->geometry ?
        frame->palette.size()*sizeof(float)+formaview::render::renderer_uniform_bytes+
          (frame->selectionVertices ? frame->guides.size()*sizeof(formaview::render::Vertex) : 0) : 0;
    bool subsetMatches=true, collisionAlphaMatches=true, meshMembershipMatches=true;
    if(frame->geometry) {
      const auto state=session.renderStateForTest();
      const auto cpu=buildModelFrames(*session.documentSnapshot(),state,1,{},nullptr,false).model;
      std::uint64_t shown=0, blended=0, cpuBlended=0;
      std::vector<std::uint64_t> meshVertices(state.classes.size());
      for(const auto &draw:frame->draws) {
        shown+=draw.count;if(draw.blend)blended+=draw.count;
        if(draw.mesh>=0)meshVertices.at(static_cast<std::size_t>(draw.mesh))+=draw.count;
        if(draw.mesh>=0 && !state.classes.empty() && state.classes.at(static_cast<std::size_t>(draw.mesh)).collision) {
          for(std::size_t vi=draw.first;vi<static_cast<std::size_t>(draw.first)+draw.count;++vi) {
            const auto &vertex=frame->geometry->vertices.at(vi);
            collisionAlphaMatches=collisionAlphaMatches &&
              vertex.alpha==(state.collisionMode==1?.35F:1.F) &&
              vertex.alphaMode==(state.collisionMode==1?2.F:0.F);
          }
        }
      }
      // Independently derive membership from classification, not state.visible:
      // CPU and GPU builders must not agree on the same erroneous visibility.
      const auto document=session.documentSnapshot();
      for(std::size_t mi=0;mi<state.classes.size();++mi) {
        const auto &classification=state.classes[mi];
        const auto level=state.levels.find(classification.group);
        const bool atLevel=classification.group.isEmpty() ||
          (level!=state.levels.end() && level->second==classification.level);
        const bool expected=atLevel && (!state.isolated || mi==static_cast<std::size_t>(state.mesh)) &&
          (classification.collision ? state.collisionMode!=0 : state.collisionMode!=2);
        meshMembershipMatches=meshMembershipMatches &&
          meshVertices[mi]==(expected?document->meshes[mi].triangles.size():0);
      }
      for(const auto &draw:cpu->draws)if(draw.blend)cpuBlended+=draw.count;
      subsetMatches=shown==cpu->vertices.size() && blended==cpuBlended &&
                    frame->sourceTriangles==cpu->sourceTriangles && frame->sourcePoints==cpu->sourcePoints;
    }
    std::lock_guard guard(shared.mutex);
    const auto uploadDelta=shared.uploadBytes-previousRowUpload;
    previousRowUpload=shared.uploadBytes;
    return QJsonObject{
        {"cycle", cycle}, {"gpu", shared.last.gpu_skinning},
        {"geometrySame", frame->geometry && frame->geometry == before->geometry},
        {"geometryRevision", static_cast<qint64>(frame->geometry ? frame->geometry->revision : 0)},
        {"geometryBuilds", static_cast<qint64>(modelGpuGeometryBuildCount()-buildsBefore)},
        {"frameRevision", static_cast<qint64>(frame->revision)},
        {"observedRevision", static_cast<qint64>(shared.last.frame_revision)},
        {"paletteFloats", static_cast<qint64>(frame->palette.size())},
        {"sourceSubsetMatchesCpu", subsetMatches}, {"collisionAlphaMatches", collisionAlphaMatches},
        {"meshMembershipMatches", meshMembershipMatches},
        {"expectedDynamicBytes", static_cast<qint64>(dynamicBytes)},
        {"frameCacheable", frame->cacheable},
        {"uploadedBytes", static_cast<qint64>(shared.last.uploaded_bytes)},
        {"uploadedSincePreviousRow", static_cast<qint64>(uploadDelta)},
        {"residentHit", shared.last.resident_hit}};
  };
  const auto verifyResidency = [&](const QJsonObject &result, const QJsonArray &rows) {
    if (!session.frame()->geometry) {
      if (result["uploaded_bytes"].toInteger()!=0 || result["resident_hits"].toInteger()<rows.size()-1)
        throw std::runtime_error("CPU frame-cache residency must reuse buffers without re-upload");
      return;
    }
    qint64 expected=0;
    for (const auto &value:rows) {
      const auto row=value.toObject();
      expected+=row["expectedDynamicBytes"].toInteger();
      if (!row["gpu"].toBool() || !row["geometrySame"].toBool() || row["geometryBuilds"].toInteger()!=0 ||
          row["frameRevision"]!=row["observedRevision"] || row["uploadedSincePreviousRow"]!=row["expectedDynamicBytes"] ||
          !row["sourceSubsetMatchesCpu"].toBool() || !row["collisionAlphaMatches"].toBool() ||
          !row["meshMembershipMatches"].toBool())
        throw std::runtime_error("Resident GPU geometry changed, rebuilt, mismatched its frame, or uploaded more than pose/guide data");
    }
    if (result["uploaded_bytes"].toInteger()!=expected)
      throw std::runtime_error("Resident GPU workload contains additional unaccounted uploads");
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
    QJsonArray lodRows;
    for (int i = 0; i < 80; ++i) {
      const auto beforeFrame = session.frame();
      const auto buildsBefore = modelGpuGeometryBuildCount();
      const auto start = Clock::now();
      session.requestLod(levels[(i + 1) % 2].toMap().value("level").toInt());
      ready();
      if (i==2 && qEnvironmentVariableIsSet("FORMAVIEW_PERF_FORCE_GEOMETRY_COPY") && session.frame()->geometry) {
        // Negative control: an equivalent new geometry object must be detected as a re-upload.
        auto copy=std::make_shared<formaview::render::Frame>(*session.frame());
        copy->geometry=std::make_shared<formaview::render::Frame>(*copy->geometry);
        view->setFrame(copy);
      }
      warm.push_back(milliseconds(presented() - start));
      lodRows.append(residencyRow(i,beforeFrame,buildsBefore));
    }
    const auto resident = finish(warm);
    evidence["resident_lod"] = resident;
    evidence["resident_lod_rows"] = lodRows;
    evidence["residency_boundary"]="GPU: immutable geometry identity and build count unchanged; matching presented revision; exact palette/uniform/selection-guide upload budget, no extra uploads. CPU: original frame-cache zero-upload/hit-count requirement.";
    verifyResidency(resident,lodRows);
    // Session-only explicit classification measures the actual collision view
    // path without claiming this FBX exported collision or a PhysicsAsset.
    session.selectMesh(0);
    session.classifyMesh("", 0, true);
    ready();
    QJsonArray firstCollision;
    for (int mode : {1, 2, 0}) {
      const auto start = Clock::now();
      session.setCollisionMode(mode);
      ready();
      const auto frame=session.frame();
      const bool drawsGeometry=frame->geometry ? std::any_of(frame->draws.begin(),frame->draws.end(),[](const auto &draw){return draw.count>0;}) : !frame->vertices.empty();
      if (mode == 2 && !drawsGeometry)
        throw std::runtime_error("Collision benchmark must draw actual source geometry");
      firstCollision.append(milliseconds(presented() - start));
    }
    evidence["first_collision_switch_ms"] = firstCollision;
    begin();
    std::vector<double> collision;
    QJsonArray collisionRows;
    for (int i = 0; i < 81; ++i) {
      const auto beforeFrame=session.frame();
      const auto buildsBefore=modelGpuGeometryBuildCount();
      const auto start = Clock::now();
      session.setCollisionMode((i + 1) % 3);
      ready();
      collision.push_back(milliseconds(presented() - start));
      collisionRows.append(residencyRow(i,beforeFrame,buildsBefore));
    }
    const auto collisionResult = finish(collision);
    evidence["resident_collision"] = collisionResult;
    evidence["resident_collision_rows"] = collisionRows;
    evidence["collision_boundary"] = "Explicit session-only correction of an actual source mesh; hidden/overlay/collision-only. This source is not claimed to contain exported collision.";
    if (collisionResult.value("command_to_swap").toObject().value("p95_ms").toDouble() > 100)
      throw std::runtime_error("Resident collision switch exceeds 100 ms P95");
    verifyResidency(collisionResult,collisionRows);
    session.restoreClassification();
    session.setCollisionMode(0);
    ready();
    QJsonArray release;
    for (int i = 0; i < 6; ++i) {
      load(i % 2 ? lod : cloud);
      session.clear();
      presented();
      std::lock_guard guard(shared.mutex);
      if (shared.last.owned_gpu_bytes > formaview::render::renderer_uniform_bytes)
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
