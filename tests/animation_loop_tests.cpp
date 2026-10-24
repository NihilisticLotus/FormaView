// M07 minimal vertical animation loop probe: real PSA -> real-second
// evaluation -> immutable pose snapshot -> generation-checked publication ->
// line-topology render Frame -> presentation. Measures the product-pattern
// scheduling cost (Scheduler + LatestRequest + 20 ms poll, mirroring
// ModelSession) against an inline loop before any 60 fps claim is made.
#include "formaview/render_item.hpp"
#include "formaview/scheduler.hpp"
#include "formaview/skeletal.hpp"
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
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <numbers>
#include <windows.h>

#include <psapi.h>

namespace {
using Clock = std::chrono::steady_clock;
using formaview::Matrix4;
using formaview::skeletal::ActorAsset;
using formaview::skeletal::Pose;
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
          {"p50_ms", at(.50)},
          {"p95_ms", at(.95)},
          {"p99_ms", at(.99)},
          {"max_ms", values.back()},
          {"over_16_7ms",
           static_cast<int>(std::count_if(values.begin(), values.end(),
                                          [](double v) { return v > 16.7; }))},
          {"over_33_3ms",
           static_cast<int>(std::count_if(values.begin(), values.end(),
                                          [](double v) { return v > 33.3; }))}};
}
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
  QObject::connect(&poll, &QTimer::timeout, &loop,
                   [&] { if (predicate()) loop.quit(); });
  QObject::connect(&deadline, &QTimer::timeout, &loop,
                   [&] { loop.quit(); });
  poll.start();
  deadline.start(timeout);
  loop.exec();
  if (!predicate())
    throw std::runtime_error("Timed out waiting for animation loop state");
}

struct PoseSnapshot {
  std::uint64_t request{};
  std::size_t clip{};
  double requestedTime{}, evaluatedTime{};
  Pose pose; // Immutable once published.
};

double maxAbs(const Matrix4 &a, const Matrix4 &b) {
  double worst{};
  for (std::size_t i = 0; i < 16; ++i)
    worst = std::max(worst, std::abs(a[i] - b[i]));
  return worst;
}
double chainError(const ActorAsset &asset, const Pose &pose) {
  double worst{};
  for (std::size_t i = 0; i < asset.bones.size(); ++i) {
    const auto &bone = asset.bones[i];
    if (!bone.parent)
      continue;
    const auto composed =
        formaview::skeletal::multiply(pose.world[*bone.parent],
                                      formaview::skeletal::matrix(pose.local[i]));
    worst = std::max(worst, maxAbs(composed, pose.world[i]));
  }
  return worst;
}
bool finite(const Pose &pose) {
  for (const auto &m : pose.world)
    for (double v : m)
      if (!std::isfinite(v))
        return false;
  for (const auto &t : pose.local) {
    const std::array<double, 7> values{
        t.translation.x, t.translation.y, t.translation.z, t.rotation.x,
        t.rotation.y, t.rotation.z, t.rotation.w};
    for (double v : values)
      if (!std::isfinite(v))
        return false;
  }
  return true;
}
double frobeniusDelta(const Pose &a, const Pose &b) {
  if (a.world.size() != b.world.size())
    return std::numeric_limits<double>::infinity();
  double sum{};
  for (std::size_t i = 0; i < a.world.size(); ++i)
    for (std::size_t e = 0; e < 16; ++e) {
      const double d = a.world[i][e] - b.world[i][e];
      sum += d * d;
    }
  return std::sqrt(sum);
}

// Skeleton line geometry: one segment per parent link plus a small cross at
// each joint; line topology reuses the existing render pipeline untouched.
struct SegmentGeometry {
  std::vector<formaview::render::Vertex> vertices;
  QVector3D low{1e30F, 1e30F, 1e30F}, high{-1e30F, -1e30F, -1e30F};
};
SegmentGeometry skeletonSegments(const ActorAsset &asset, const Pose &pose,
                                 double cross) {
  SegmentGeometry geometry;
  const auto position = [&](std::size_t bone) {
    const auto &m = pose.world[bone];
    return QVector3D(static_cast<float>(m[3]), static_cast<float>(m[7]),
                     static_cast<float>(m[11]));
  };
  const auto push = [&](const QVector3D &a, const QVector3D &b) {
    for (const QVector3D *p : {&a, &b}) {
      geometry.low.setX(std::min(geometry.low.x(), p->x()));
      geometry.low.setY(std::min(geometry.low.y(), p->y()));
      geometry.low.setZ(std::min(geometry.low.z(), p->z()));
      geometry.high.setX(std::max(geometry.high.x(), p->x()));
      geometry.high.setY(std::max(geometry.high.y(), p->y()));
      geometry.high.setZ(std::max(geometry.high.z(), p->z()));
    }
    geometry.vertices.push_back({a.x(), a.y(), a.z(), 0, 0, 0});
    geometry.vertices.push_back({b.x(), b.y(), b.z(), 0, 0, 0});
  };
  for (std::size_t i = 0; i < asset.bones.size(); ++i) {
    const auto joint = position(i);
    if (const auto parent = asset.bones[i].parent)
      push(position(*parent), joint);
    push(joint + QVector3D(cross, 0, 0), joint - QVector3D(cross, 0, 0));
    push(joint + QVector3D(0, cross, 0), joint - QVector3D(0, cross, 0));
  }
  return geometry;
}
std::shared_ptr<const formaview::render::Frame>
makeSkeletonFrame(const SegmentGeometry &geometry, std::uint64_t revision) {
  auto frame = std::make_shared<formaview::render::Frame>();
  frame->revision = revision;
  frame->lines = true; // Existing line topology; no renderer change.
  frame->rgba = {0.62F, 0.71F, 0.83F, 1.F};
  frame->vertices = geometry.vertices;
  const auto centre = (geometry.low + geometry.high) * .5F;
  const auto extent = geometry.high - geometry.low;
  frame->matrix.setToIdentity();
  frame->matrix.scale(
      1.7F / std::max({extent.x(), extent.y(), extent.z(), .001F}));
  frame->matrix.translate(-centre);
  frame->cacheable = false; // Per-frame payload never enters the frame cache.
  return frame;
}
} // namespace

int main(int argc, char **argv) {
  QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
  QGuiApplication app(argc, argv);
  if (app.arguments().size() != 3) {
    std::cerr << "usage: animation_loop_tests <psa> <output.json>\n";
    return 64;
  }
  const auto source = app.arguments()[1].toStdWString();
  const QString output = app.arguments()[2];
  QJsonObject evidence{
      {"qt", qVersion()},
      {"sample", app.arguments()[1]},
      {"boundary",
       "Real PSA evaluation-to-presentation loop with product-pattern "
       "scheduling (Scheduler + LatestRequest + 20 ms poll) versus an inline "
       "16 ms loop. This probe measures; it does not claim the M07 60 fps "
       "gate passed. Correctness checks are structural invariants "
       "(finiteness, world = parent * local chain, distinctness), not "
       "independent source-key expectations."},
  };
  try {
    // File identity.
    std::ifstream file(source, std::ios::binary);
    if (!file)
      throw std::runtime_error("Sample open failed");
    std::vector<char> raw((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    if (!raw.empty())
      std::memcpy(bytes.data(), raw.data(), raw.size());
    evidence["sample_bytes"] = static_cast<qint64>(bytes.size());
    evidence["sample_sha256"] =
        QString::fromStdString(formaview::skeletal::sha256(bytes));
    const auto parsedStart = Clock::now();
    const ActorAsset asset = formaview::skeletal::read_actor(source);
    evidence["parse_ms"] = milliseconds(Clock::now() - parsedStart);
    if (!asset.animation || asset.sequences.empty())
      throw std::runtime_error("Sample has no animation sequences");
    evidence["bones"] = static_cast<int>(asset.bones.size());
    QJsonArray clipList;
    std::size_t primary = 0;
    for (std::size_t i = 0; i < asset.sequences.size(); ++i) {
      const auto &s = asset.sequences[i];
      clipList.append(QJsonObject{
          {"index", static_cast<int>(i)},
          {"group", QString::fromStdString(s.group)},
          {"frame_count", s.frame_count},
          {"fps", s.fps},
          {"time_start_s", s.time.source_start_seconds},
          {"time_end_s", s.time.source_end_seconds},
          {"uniform_key_deltas", s.uniform_key_deltas}});
      if (s.frame_count > asset.sequences[primary].frame_count)
        primary = i;
    }
    evidence["clips"] = clipList;
    evidence["primary_clip"] = static_cast<int>(primary);
    const auto &clip = asset.sequences[primary];
    const double fps = clip.fps > 0 ? clip.fps : 30.0;
    const double tStart = clip.time.source_start_seconds;
    const double lastKey = tStart + (clip.frame_count - 1) / fps;
    const double mid = (tStart + lastKey) / 2;
    const double offKey = tStart + 0.5 / fps;

    // Phase B: structural invariants on the primary clip.
    QJsonObject checks;
    double worstChain{};
    double worstNorm{};
    for (double t : {tStart, offKey, mid, lastKey}) {
      const auto pose = formaview::skeletal::evaluate(asset, primary, t);
      if (!finite(pose))
        throw std::runtime_error("Non-finite pose matrices in domain");
      worstChain = std::max(worstChain, chainError(asset, pose));
      for (const auto &local : pose.local) {
        const auto &q = local.rotation;
        worstNorm = std::max(worstNorm, std::abs(std::sqrt(q.x * q.x +
                                                           q.y * q.y +
                                                           q.z * q.z +
                                                           q.w * q.w) -
                                                 1.0));
      }
    }
    if (worstChain > 1e-6)
      throw std::runtime_error("world != parent * local chain broken");
    checks["chain_world_parent_local_max"] = worstChain;
    checks["quaternion_norm_max_deviation"] = worstNorm;
    const auto startPose = formaview::skeletal::evaluate(asset, primary, tStart);
    const auto midPose = formaview::skeletal::evaluate(asset, primary, mid);
    const auto motion = frobeniusDelta(startPose, midPose);
    checks["start_mid_frobenius"] = motion;
    if (clip.frame_count > 1 && motion <= 1e-12)
      throw std::runtime_error("Multi-frame clip shows identical start/mid");
    bool endRejected{};
    try {
      (void)formaview::skeletal::evaluate(asset, primary,
                                          clip.time.source_end_seconds);
    } catch (const formaview::skeletal::Failure &) {
      endRejected = true; // Half-open domain rejected; recorded, not asserted.
    }
    checks["exact_end_seconds_rejected"] = endRejected;
    evidence["invariants"] = checks;

    // Window for phases D/E (created before C so the GPU is warm for both).
    QQuickWindow window;
    QQuickGraphicsConfiguration graphics;
    graphics.setTimestamps(true);
    window.setGraphicsConfiguration(graphics);
    window.setPersistentGraphics(false);
    window.setPersistentSceneGraph(false);
    window.setFlags(Qt::Window | Qt::WindowDoesNotAcceptFocus |
                    Qt::WindowTransparentForInput);
    window.resize(1280, 720);
    auto *item = new formaview::render::Item(window.contentItem());
    item->setWidth(1280);
    item->setHeight(720);
    item->setFixedColorBufferWidth(1920);
    item->setFixedColorBufferHeight(1080);
    std::atomic<std::uint64_t> generation{}, synchronized{};
    struct Shared {
      std::mutex mutex;
      std::uint64_t swapGeneration{}, frames{};
      std::uint64_t uploadBytes{}, peakOwned{}, peakProcess{}, lastRevision{};
      Clock::time_point swapTime;
      std::vector<double> intervals, prepare, record, gpu;
      formaview::render::Observation last;
      bool measuring{};
    } shared;
    item->observe = [&](formaview::render::Observation value) {
      std::lock_guard guard(shared.mutex);
      shared.last = value;
      shared.lastRevision = value.frame_revision;
      shared.peakOwned = std::max(shared.peakOwned, value.owned_gpu_bytes);
      shared.peakProcess = std::max(shared.peakProcess, value.process_gpu_bytes);
      if (shared.measuring) {
        shared.prepare.push_back(value.prepare_ms);
        shared.record.push_back(value.record_ms);
        if (value.gpu_seconds > 0)
          shared.gpu.push_back(value.gpu_seconds * 1000);
        shared.uploadBytes += value.uploaded_bytes;
      }
    };
    QObject::connect(
        &window, &QQuickWindow::afterSynchronizing, &window,
        [&] { synchronized.store(generation.load()); }, Qt::DirectConnection);
    QObject::connect(
        &window, &QQuickWindow::frameSwapped, &window,
        [&] {
          const auto stamp = Clock::now();
          std::lock_guard guard(shared.mutex);
          if (shared.measuring && shared.swapTime.time_since_epoch().count())
            shared.intervals.push_back(milliseconds(stamp - shared.swapTime));
          shared.swapTime = stamp;
          shared.swapGeneration = synchronized.load();
          ++shared.frames;
        },
        Qt::DirectConnection);
    const auto presented = [&] {
      const auto token = ++generation;
      window.update();
      item->update();
      waitFor([&] {
        std::lock_guard guard(shared.mutex);
        return shared.swapGeneration >= token;
      });
      std::lock_guard guard(shared.mutex);
      return shared.swapTime;
    };
    window.show();
    presented();
    if (item->effectiveColorBufferSize() != QSize(1920, 1080))
      throw std::runtime_error("Loop render target is not 1080p");

    // Phase C: product-pattern scheduling latency. The pose work mirrors the
    // ModelSession job model: Scheduler worker + 20 ms QTimer poll on the GUI
    // thread + LatestRequest generation arbitration for publication.
    formaview::Scheduler scheduler;
    formaview::LatestRequest requests;
    struct Crawl {
      std::mutex mutex;
      std::optional<formaview::TaskHandle> handle;
      std::optional<formaview::GenerationTicket> ticket;
      std::optional<PoseSnapshot> snapshot;
      Clock::time_point requestedAt, completedAt, pickedAt;
      std::size_t published{}, dropped{};
      bool working{};
    } crawl;
    const auto submitPose = [&](double t, std::uint32_t repetitions = 1) {
      std::lock_guard guard(crawl.mutex);
      crawl.ticket = requests.issue();
      crawl.snapshot.reset();
      crawl.requestedAt = Clock::now();
      crawl.working = true;
      const auto ticket = *crawl.ticket;
      const auto request = crawl.published + crawl.dropped + 1;
      formaview::TaskRequest task;
      task.priority = formaview::Priority::Foreground;
      task.resources.cpu_slots = 1;
      task.generation = ticket;
      task.work = [&, ticket, t, request,
                   repetitions](std::stop_token stop) -> formaview::TaskResult {
        Pose pose;
        for (std::uint32_t r = 0; r < repetitions; ++r)
          pose = formaview::skeletal::evaluate(asset, primary, t, stop);
        const auto done = Clock::now();
        std::lock_guard guard(crawl.mutex);
        crawl.completedAt = done;
        crawl.snapshot = PoseSnapshot{request, primary, t, t, std::move(pose)};
        return {};
      };
      const auto submitted = scheduler.submit(std::move(task));
      if (!submitted.task)
        throw std::runtime_error("Scheduler rejected pose task");
      crawl.handle = *submitted.task;
    };
    QTimer poll;
    poll.setInterval(20); // Mirrors ModelSession's 20 ms poll cadence.
    std::atomic<bool> picked{};
    struct Stream {
      std::mutex mutex;
      std::optional<formaview::TaskHandle> handle;
      std::optional<formaview::GenerationTicket> ticket;
      std::size_t published{}, dropped{}, superseded{};
      Clock::time_point lastPublish{};
      std::vector<double> publishIntervals;
    } stream;
    const auto pickUp = [&]() -> bool {
      std::lock_guard guard(crawl.mutex);
      if (!crawl.working || !crawl.handle)
        return false;
      const auto state = crawl.handle->snapshot().state;
      if (!formaview::is_terminal(state))
        return false;
      crawl.pickedAt = Clock::now();
      crawl.working = false;
      crawl.handle.reset();
      if (crawl.ticket && crawl.ticket->publish([&] {}) &&
          crawl.snapshot) {
        ++crawl.published;
        picked = true;
        return true;
      }
      ++crawl.dropped;
      picked = true;
      return false;
    };
    QObject::connect(&poll, &QTimer::timeout, &poll, [&] {
      pickUp();
      std::optional<formaview::TaskHandle> streamHandle;
      {
        std::lock_guard guard(stream.mutex);
        streamHandle = stream.handle;
      }
      if (streamHandle &&
          formaview::is_terminal(streamHandle->snapshot().state)) {
        std::lock_guard guard(stream.mutex);
        if (stream.handle && stream.ticket) {
          const auto now = Clock::now();
          if (stream.ticket->publish([&] {})) {
            if (stream.lastPublish.time_since_epoch().count())
              stream.publishIntervals.push_back(
                  milliseconds(now - stream.lastPublish));
            stream.lastPublish = now;
            ++stream.published;
          } else {
            ++stream.dropped;
          }
          stream.handle.reset();
        }
      }
    });
    poll.start();
    QJsonObject scheduling;
    {
      // Sequential request->pickup and completion->pickup latency, harvested
      // strictly by the 20 ms poll (never by the 1 ms wait predicate).
      std::vector<double> requestToPickup, completionToPickup, evalOnly;
      for (int i = 0; i < 300; ++i) {
        const double t =
            tStart + std::fmod(i * 0.037, lastKey - tStart);
        const auto evalStart = Clock::now();
        (void)formaview::skeletal::evaluate(asset, primary, t);
        evalOnly.push_back(milliseconds(Clock::now() - evalStart));
        picked = false;
        submitPose(t);
        waitFor([&] { return picked.load(); });
        std::lock_guard guard(crawl.mutex);
        requestToPickup.push_back(milliseconds(crawl.pickedAt -
                                               crawl.requestedAt));
        completionToPickup.push_back(milliseconds(crawl.pickedAt -
                                                  crawl.completedAt));
      }
      scheduling["sequential"] = QJsonObject{
          {"request_to_pickup", distribution(requestToPickup)},
          {"completion_to_pickup_20ms_poll", distribution(completionToPickup)},
          {"evaluate_only", distribution(evalOnly)}};
    }
    {
      // 60 Hz request stream against the 20 ms poll: cadence and staleness.
      // Publication happens only on the GUI thread via the poll, mirroring
      // the product job model; superseded results are counted as dropped.
      QTimer drive;
      drive.setInterval(16);
      int ticks{};
      QObject::connect(&drive, &QTimer::timeout, &drive, [&] {
        ++ticks;
        const double t = tStart + std::fmod(ticks * 0.031, lastKey - tStart);
        std::lock_guard guard(stream.mutex);
        if (stream.handle)
          ++stream.superseded;
        stream.ticket = requests.issue();
        formaview::TaskRequest task;
        task.priority = formaview::Priority::Foreground;
        task.generation = *stream.ticket;
        task.work = [&stream, t, &asset, primary,
                     tStart](std::stop_token stop) -> formaview::TaskResult {
          auto pose =
              formaview::skeletal::evaluate(asset, primary, t, stop);
          (void)pose; // Snapshot identity only; publication is poll-side.
          return {};
        };
        const auto submitted = scheduler.submit(std::move(task));
        if (submitted.task)
          stream.handle = *submitted.task;
      });
      drive.start();
      const auto until = Clock::now() + std::chrono::seconds(6);
      while (Clock::now() < until)
        app.processEvents(QEventLoop::AllEvents, 20);
      drive.stop();
      poll.stop();
      scheduler.shutdown(); // Joins workers before the report reads state.
      std::lock_guard guard(stream.mutex);
      scheduling["stream_16ms_vs_poll_20ms"] = QJsonObject{
          {"ticks", ticks},
          {"published", static_cast<qint64>(stream.published)},
          {"dropped_stale", static_cast<qint64>(stream.dropped)},
          {"superseded_in_flight", static_cast<qint64>(stream.superseded)},
          {"publish_intervals", distribution(stream.publishIntervals)},
          {"boundary",
           "Requests issued every 16 ms; the 20 ms GUI poll is the only "
           "publisher and drops superseded generations."}};
    }
    evidence["scheduling"] = scheduling;

    // Phase D1: inline loop - evaluate and build geometry on the GUI thread
    // every 16 ms, publish an immutable frame, present.
    const auto runRenderLoop = [&](bool inlineEval, int frames,
                                   QJsonObject &report) {
      std::vector<double> evalTimes, geomTimes, commandToSwap;
      std::uint64_t revision{};
      double t = tStart;
      const double step = 1.0 / 60.0;
      {
        std::lock_guard guard(shared.mutex);
        shared.prepare.clear();
        shared.record.clear();
        shared.gpu.clear();
        shared.intervals.clear();
        shared.swapTime = {};
        shared.uploadBytes = 0;
        shared.measuring = true;
      }
      QTimer drive;
      drive.setInterval(16);
      int produced{};
      bool inTick{}; // presented() runs a nested loop; ignore re-entry.
      QObject::connect(&drive, &QTimer::timeout, &drive, [&] {
        if (inTick || produced >= frames) {
          if (produced >= frames)
            drive.stop();
          return;
        }
        inTick = true;
        const auto commandStart = Clock::now();
        Pose pose;
        if (inlineEval) {
          const auto evalStart = Clock::now();
          pose = formaview::skeletal::evaluate(asset, primary, t);
          evalTimes.push_back(milliseconds(Clock::now() - evalStart));
        } else {
          pose = formaview::skeletal::evaluate(asset, primary, t);
        }
        const auto geomStart = Clock::now();
        const auto geometry = skeletonSegments(asset, pose, 0.9);
        const auto frame = makeSkeletonFrame(geometry, ++revision);
        geomTimes.push_back(milliseconds(Clock::now() - geomStart));
        item->setFrame(frame); // GUI thread per render contract.
        window.update();
        item->update();
        commandToSwap.push_back(milliseconds(presented() - commandStart));
        t = tStart + std::fmod((t - tStart) + step, lastKey - tStart);
        ++produced;
        inTick = false;
      });
      drive.start();
      waitFor([&] { return produced >= frames; });
      {
        std::lock_guard guard(shared.mutex);
        shared.measuring = false;
      }
      report = QJsonObject{
          {"mode", inlineEval ? "inline_eval_16ms" : "worker_publish"},
          {"frames", frames},
          {"evaluate_ms", distribution(evalTimes)},
          {"geometry_ms", distribution(geomTimes)},
          {"command_to_swap", distribution(commandToSwap)},
          {"frame_intervals", distribution(shared.intervals)},
          {"render_prepare", distribution(shared.prepare)},
          {"render_record", distribution(shared.record)},
          {"gpu_completed", distribution(shared.gpu)},
          {"uploaded_bytes", static_cast<qint64>(shared.uploadBytes)},
          {"peak_owned_gpu_bytes", static_cast<qint64>(shared.peakOwned)},
          {"rss_bytes", static_cast<qint64>(rss())}};
    };
    QJsonObject inlineReport, warmReport;
    runRenderLoop(true, 600, inlineReport);
    evidence["render_inline"] = inlineReport;
    // Phase D2 (same loop after warm-up; separates first-touch upload cost).
    runRenderLoop(true, 600, warmReport);
    evidence["render_inline_warm"] = warmReport;

    // Phase E: stale publication rejection. A long in-flight evaluation must
    // not publish after its generation was superseded.
    {
      formaview::LatestRequest guard;
      auto ticketA = guard.issue();
      std::atomic<bool> ranA{}, publishedA{};
      formaview::Scheduler stale;
      formaview::TaskRequest task;
      task.generation = ticketA;
      task.work = [ticketA, &ranA, &publishedA,
                   &asset, primary, tStart](std::stop_token stop)
          -> formaview::TaskResult {
        for (int i = 0; i < 400 && !stop.stop_requested(); ++i)
          (void)formaview::skeletal::evaluate(asset, primary, tStart, stop);
        ranA = true;
        publishedA = ticketA.publish([] {});
        return {};
      };
      const auto submitted = stale.submit(std::move(task));
      (void)guard.issue(); // Supersede while the work is still running.
      if (!submitted.task || !submitted.task->wait_for(std::chrono::seconds(60)))
        throw std::runtime_error("Stale probe never completed");
      // Two correct outcomes: the scheduler discards the superseded task
      // before it runs, or the work runs and its ticket fails to publish.
      if (publishedA.load())
        throw std::runtime_error("Stale ticket published after supersede");
      evidence["stale_publication_rejected"] = true;
      evidence["stale_probe_work_ran"] = ranA.load();
      evidence["stale_probe_final_state"] =
          static_cast<int>(submitted.task->snapshot().state);
      stale.shutdown();
    }

    QFile out(output);
    if (!out.open(QIODevice::WriteOnly))
      throw std::runtime_error("Output open failed");
    out.write(QJsonDocument(evidence).toJson(QJsonDocument::Indented));
    std::cout << "animation loop probe complete\n";
    return 0;
  } catch (const std::exception &error) {
    evidence["failure"] = error.what();
    QFile out(output);
    if (out.open(QIODevice::WriteOnly))
      out.write(QJsonDocument(evidence).toJson(QJsonDocument::Indented));
    std::cerr << "animation loop probe failed: " << error.what() << "\n";
    return 1;
  }
}
