#include "formaview/geometry_cache.hpp"
#include "formaview/preview.hpp"
#include "formaview/pro_image.hpp"
#include "formaview/render_item.hpp"
// clang-format off
#include <Windows.h>
#include <Psapi.h>
// clang-format on
#include <QElapsedTimer>
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
#include <cmath>
#include <cstring>
#include <dxgi1_4.h>
#include <limits>
#include <mutex>
#include <thread>
#include <wrl/client.h>

namespace {
QJsonObject stats(std::vector<double> v) {
  if (v.empty())
    return {};
  std::sort(v.begin(), v.end());
  const auto at = [&](double p) {
    return v[static_cast<std::size_t>(
                 std::ceil(p * static_cast<double>(v.size()))) -
             1];
  };
  return {{"n", static_cast<int>(v.size())},
          {"p95_ms", at(.95)},
          {"p99_ms", at(.99)},
          {"max_ms", v.back()},
          {"over_33ms",
           static_cast<int>(std::count_if(v.begin(), v.end(),
                                          [](double x) { return x > 33.3; }))},
          {"over_100ms",
           static_cast<int>(std::count_if(v.begin(), v.end(),
                                          [](double x) { return x > 100; }))}};
}
quint64 rss() {
  PROCESS_MEMORY_COUNTERS_EX p{};
  GetProcessMemoryInfo(GetCurrentProcess(),
                       reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&p),
                       sizeof(p));
  return p.WorkingSetSize;
}
quint64 gpuUsage() {
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
  DXGI_QUERY_VIDEO_MEMORY_INFO info{};
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
      FAILED(factory->EnumAdapters1(0, &adapter)) ||
      FAILED(adapter.As(&adapter3)) ||
      FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                                            &info)))
    return 0;
  return info.CurrentUsage;
}
struct Metrics {
  std::mutex mutex;
  std::vector<double> prepare, record, frames, heartbeat, gpu;
  quint64 gpu_peak{}, gpu_release{}, owned_peak{}, rss_peak{}, rss_start{},
      rss_end{};
  bool released{}, readback_ok{}, finishing{};
  int count{};
  double first_ms{}, first_prepare_ms{};
  QString error;
};
} // namespace
int main(int argc, char **argv) {
  QGuiApplication app(argc, argv);
  if (app.arguments().size() < 3)
    return 2;
  const auto output = app.arguments()[1], mode = app.arguments()[2];
  const auto arguments = app.arguments();
  QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
  const auto gpu_start = gpuUsage();
  auto window_owner = std::make_unique<QQuickWindow>();
  auto &window = *window_owner;
  window.resize(1920, 1080);
  window.setTitle("FormaView M03 technical validation");
  window.setPersistentGraphics(false);
  window.setPersistentSceneGraph(false);
  QQuickGraphicsConfiguration graphics;
  graphics.setTimestamps(true);
  window.setGraphicsConfiguration(graphics);
  auto item = std::make_unique<formaview::render::Item>(window.contentItem());
  item->setSize({1920, 1080});
  item->setFixedColorBufferWidth(1920);
  item->setFixedColorBufferHeight(1080);
  auto metrics = std::make_shared<Metrics>();
  metrics->rss_start = rss();
  QElapsedTimer total;
  total.start();
  QElapsedTimer frame_timer;
  frame_timer.start();
  QObject::connect(
      &window, &QQuickWindow::frameSwapped, &window,
      [&] {
        std::lock_guard lock(metrics->mutex);
        if (metrics->count > 1 && !metrics->finishing)
          metrics->frames.push_back(
              static_cast<double>(frame_timer.nsecsElapsed()) / 1e6);
        frame_timer.restart();
      },
      Qt::DirectConnection);
  QElapsedTimer heartbeat_timer;
  heartbeat_timer.start();
  QTimer heartbeat;
  heartbeat.setInterval(5);
  QObject::connect(&heartbeat, &QTimer::timeout, &app, [&] {
    std::lock_guard lock(metrics->mutex);
    if (metrics->count > 1 && !metrics->finishing)
      metrics->heartbeat.push_back(
          static_cast<double>(heartbeat_timer.nsecsElapsed()) / 1e6);
    heartbeat_timer.restart();
  });
  heartbeat.start();
  std::atomic<bool> finish_started = false;
  std::vector<std::shared_ptr<const formaview::render::Frame>> sequence;
  std::size_t sequence_index{};
  QTimer sequence_timer;
  sequence_timer.setInterval(16);
  QObject::connect(&sequence_timer, &QTimer::timeout, &app, [&] {
    if (item && !sequence.empty())
      item->setFrame(sequence[++sequence_index % sequence.size()]);
  });
  const auto finish = [&] {
    if (finish_started.exchange(true))
      return;
    sequence_timer.stop();
    sequence.clear();
    {
      std::lock_guard lock(metrics->mutex);
      metrics->finishing = true;
    }
    auto screenshot = window.grabWindow();
    item.reset();
    window.hide();
    window.releaseResources();
    window_owner.reset();
    // Artifact file I/O is outside both GUI interaction and measured rendering.
    auto image_writer = std::make_shared<std::jthread>(
        [screenshot, output] { screenshot.save(output + "/viewport.png"); });
    QTimer::singleShot(700, &app, [&, image_writer] {
      heartbeat.stop();
      std::lock_guard lock(metrics->mutex);
      metrics->rss_end = rss();
      QJsonObject result{
          {"backend", "D3D11 / QQuickRhiItem / Qt 6.8.3"},
          {"mode", mode},
          {"width", 1920},
          {"height", 1080},
          {"prepare", stats(metrics->prepare)},
          {"record", stats(metrics->record)},
          {"frame_intervals", stats(metrics->frames)},
          {"main_heartbeat", stats(metrics->heartbeat)},
          {"gpu", stats(metrics->gpu)},
          {"first_render_ms", metrics->first_ms},
          {"owned_gpu_peak", static_cast<double>(metrics->owned_peak)},
          {"process_gpu_peak", static_cast<double>(metrics->gpu_peak)},
          {"rss_start", static_cast<double>(metrics->rss_start)},
          {"rss_peak", static_cast<double>(metrics->rss_peak)},
          {"rss_end", static_cast<double>(metrics->rss_end)},
          {"renderer_released", metrics->released},
          {"float_readback_ok", metrics->readback_ok},
          {"error", metrics->error}};
      result["first_upload_prepare_ms"] = metrics->first_prepare_ms;
      result["process_gpu_after_adapter_release"] =
          static_cast<double>(metrics->gpu_release);
      result["measurement_scope"] =
          "First render separately; stable intervals exclude startup and "
          "screenshot/teardown; GPU times unavailable if empty";
      result["process_gpu_before_window"] = static_cast<double>(gpu_start);
      result["process_gpu_after_window_release"] =
          static_cast<double>(gpuUsage());
      QFile file(output + "/render.json");
      if (!file.open(QIODevice::WriteOnly)) {
        app.exit(3);
        return;
      }
      file.write(QJsonDocument(result).toJson());
      app.exit(metrics->error.isEmpty() && metrics->released &&
                       (mode != "precision" || metrics->readback_ok)
                   ? 0
                   : 1);
    });
  };
  item->observe = [&, metrics](formaview::render::Observation o) {
    std::lock_guard lock(metrics->mutex);
    if (o.released) {
      metrics->released = true;
      metrics->gpu_release = o.process_gpu_bytes;
      return;
    }
    if (metrics->finishing)
      return;
    metrics->prepare.push_back(o.prepare_ms);
    metrics->record.push_back(o.record_ms);
    if (o.gpu_seconds > 0)
      metrics->gpu.push_back(o.gpu_seconds * 1000);
    metrics->owned_peak =
        std::max(metrics->owned_peak, static_cast<quint64>(o.owned_gpu_bytes));
    metrics->gpu_peak =
        std::max(metrics->gpu_peak, static_cast<quint64>(o.process_gpu_bytes));
    metrics->rss_peak = std::max(metrics->rss_peak, rss());
    if (metrics->count++ == 0) {
      metrics->first_ms = static_cast<double>(total.nsecsElapsed()) / 1e6;
      metrics->first_prepare_ms = o.prepare_ms;
    }
    if (metrics->count == 360)
      QMetaObject::invokeMethod(&app, finish, Qt::QueuedConnection);
  };
  if (mode == "precision")
    item->readback = [metrics](QByteArray data, QSize size) {
      std::lock_guard lock(metrics->mutex);
      const auto offset =
          (static_cast<qsizetype>(size.height() / 2) * size.width() +
           size.width() / 2) *
          16;
      if (data.size() >= offset + 16) {
        std::array<float, 4> pixel{};
        std::memcpy(pixel.data(), data.data() + offset, 16);
        metrics->readback_ok =
            pixel == std::array<float, 4>{-2.5F, 1.000000119F, 65536.F, .25F};
      }
    };
  window.show();
  std::jthread loader([&](std::stop_token stop) {
    try {
      auto frame = std::make_shared<formaview::render::Frame>();
      frame->revision = 1;
      frame->rgba = {-2.5F, 1.000000119F, 65536.F, .25F};
      frame->matrix.setToIdentity();
      std::vector<std::shared_ptr<const formaview::render::Frame>> prepared;
      if (mode == "mesh") {
        if (arguments.size() != 4)
          throw std::runtime_error("Mesh path required");
        auto source = formaview::skeletal::read_actor(
            std::filesystem::path(arguments[3].toStdWString()), {.stop = stop});
        auto pose = formaview::skeletal::default_pose(source, stop);
        auto evaluated = formaview::preview::evaluate_mesh(
            source, pose.world, {}, formaview::preview::WeightPolicy::Normalize,
            stop);
        formaview::preview::WeightIndex index(*source.mesh,
                                              source.bones.size());
        auto weights = index.select(source.bones.size() - 1);
        frame->rgba = {1, 1, 1, 1};
        frame->heat = true;
        QVector3D low(1e30F, 1e30F, 1e30F), high(-1e30F, -1e30F, -1e30F);
        for (const auto &v : evaluated.corners) {
          const auto x = static_cast<float>(v.position.x),
                     y = static_cast<float>(v.position.y),
                     z = static_cast<float>(v.position.z);
          low = {std::min(low.x(), x), std::min(low.y(), y),
                 std::min(low.z(), z)};
          high = {std::max(high.x(), x), std::max(high.y(), y),
                  std::max(high.z(), z)};
          frame->vertices.push_back(
              {x, y, z, static_cast<float>(v.uv.x), static_cast<float>(v.uv.y),
               weights[v.point].has_data
                   ? static_cast<float>(weights[v.point].sum)
                   : std::numeric_limits<float>::quiet_NaN()});
        }
        const auto center = (low + high) * .5F;
        const auto extent = high - low;
        const auto scale =
            1.7F / std::max({extent.x(), extent.y(), extent.z(), .001F});
        frame->matrix.scale(scale);
        frame->matrix.translate(-center);
        auto alternate = std::make_shared<formaview::render::Frame>(*frame);
        alternate->revision = 2;
        alternate->matrix.rotate(15, 0, 1, 0);
        const auto other = index.select(0);
        for (std::size_t i = 0; i < evaluated.corners.size(); ++i)
          alternate->vertices[i].weight =
              other[evaluated.corners[i].point].has_data
                  ? static_cast<float>(other[evaluated.corners[i].point].sum)
                  : std::numeric_limits<float>::quiet_NaN();
        prepared = {frame, alternate};
      } else if (mode == "abc") {
        if (arguments.size() != 4)
          throw std::runtime_error("ABC path required");
        auto archive = formaview::cache::Archive::open(
            std::filesystem::path(arguments[3].toStdWString()), {}, stop);
        if (archive.meshes().empty() || archive.meshes()[0].times.empty())
          throw std::runtime_error("ABC mesh required");
        for (double t : {archive.meshes()[0].times.front(),
                         archive.meshes()[0]
                             .times[archive.meshes()[0].times.size() / 2]}) {
          auto sample = archive.read(0, t, stop);
          auto f = std::make_shared<formaview::render::Frame>();
          f->rgba = {.2F, .6F, .9F, 1};
          f->revision = prepared.size() + 1;
          for (auto n : sample.face_sizes)
            if (n != 3)
              throw std::runtime_error(
                  "Technical ABC renderer requires triangulated source");
          QVector3D low(1e30F, 1e30F, 1e30F), high(-1e30F, -1e30F, -1e30F);
          for (auto index : sample.corner_points) {
            const auto &v = sample.positions[static_cast<std::size_t>(index)];
            const auto x = static_cast<float>(v.x), y = static_cast<float>(v.y),
                       z = static_cast<float>(v.z);
            low = {std::min(low.x(), x), std::min(low.y(), y),
                   std::min(low.z(), z)};
            high = {std::max(high.x(), x), std::max(high.y(), y),
                    std::max(high.z(), z)};
            f->vertices.push_back({x, y, z, 0, 0, 0});
          }
          const auto extent = high - low;
          f->matrix.scale(
              1.7F / std::max({extent.x(), extent.y(), extent.z(), .001F}));
          f->matrix.translate(-(low + high) * .5F);
          prepared.push_back(f);
        }
        frame = std::make_shared<formaview::render::Frame>(*prepared.front());
      } else {
        frame->vertices = {{-1, -1, 0, 0, 1, 0}, {1, -1, 0, 1, 1, 0},
                           {-1, 1, 0, 0, 0, 0},  {-1, 1, 0, 0, 0, 0},
                           {1, -1, 0, 1, 1, 0},  {1, 1, 0, 1, 0, 0}};
        if (mode == "image") {
          if (arguments.size() != 4)
            throw std::runtime_error("Image path required");
          auto file = formaview::imaging::ImageFile::open(
              std::filesystem::path(arguments[3].toStdWString()), {}, stop);
          auto source = file.decode(0, stop);
          formaview::imaging::DisplayOptions options;
          options.meaning = formaview::ImageMeaning::Colour;
          options.exposure = -2;
          options.apply_srgb = true;
          frame->rgba = formaview::imaging::display_rgba(source, options);
          frame->width = static_cast<int>(source.data_window.width);
          frame->height = static_cast<int>(source.data_window.height);
        }
      }
      QMetaObject::invokeMethod(
          &app,
          [&, frame, prepared] {
            if (item) {
              item->setFrame(frame);
              sequence = prepared;
              if (!sequence.empty())
                sequence_timer.start();
            }
          },
          Qt::QueuedConnection);
    } catch (const std::exception &e) {
      {
        std::lock_guard lock(metrics->mutex);
        metrics->error = QString::fromUtf8(e.what());
      }
      QMetaObject::invokeMethod(&app, finish, Qt::QueuedConnection);
    }
  });
  QTimer::singleShot(30000, &app, [&] {
    if (!finish_started) {
      std::lock_guard lock(metrics->mutex);
      metrics->error = "Render timeout";
      QMetaObject::invokeMethod(&app, finish, Qt::QueuedConnection);
    }
  });
  return app.exec();
}
