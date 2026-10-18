#include "desktop.hpp"
#ifdef FORMAVIEW_IMAGE_UI
#include "formaview/image_requests.hpp"
#endif
#include "formaview/telemetry.hpp"
#include "thumbnail.hpp"
#include <QFile>
#include <QFont>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <chrono>
#include <iostream>
#include <string_view>

int main(int argc, char *argv[]) {
  for (auto i = 1; i < argc; ++i) {
    if (std::string_view{argv[i]} == "--capabilities") {
      QCoreApplication app(argc, argv);
#ifdef FORMAVIEW_IMAGE_UI
      std::cout << QJsonDocument(formaview::imaging::image_capabilities())
                       .toJson(QJsonDocument::Compact)
                       .constData()
                << '\n';
#else
      std::cout << "{\"api_version\":1,\"capabilities\":[]}\n";
#endif
      return 0;
    }
  }
  const auto started = std::chrono::steady_clock::now();
  QQuickStyle::setStyle("Basic");
  QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
  QImageReader::setAllocationLimit(32);
  auto app = QGuiApplication{argc, argv};
#ifdef FORMAVIEW_IMAGE_UI
  const auto capabilityCount =
      formaview::imaging::image_capabilities()["capabilities"].toArray().size();
#else
  constexpr auto capabilityCount = 0;
#endif
  auto font = QFont{};
  font.setFamilies({"Segoe UI", "Microsoft YaHei UI"});
  font.setPixelSize(14);
  app.setFont(font);
  QCoreApplication::setOrganizationName("FormaView");
  QCoreApplication::setApplicationName("FormaView");
  auto desktop = Desktop{};
  auto engine = QQmlApplicationEngine{};
  engine.addImageProvider("thumb",
                          std::make_unique<ThumbnailProvider>().release());
  engine.setInitialProperties({{"controller", QVariant::fromValue(&desktop)}});
  auto metrics = formaview::PerformanceRecorder{};
  const auto smoke = app.arguments().contains("--smoke");
  QObject::connect(
      &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
      [] { QCoreApplication::exit(2); }, Qt::QueuedConnection);
  engine.loadFromModule("FormaView.Engineering", "Main");
  if (engine.rootObjects().isEmpty())
    return 2;
  auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().front());
  if (!window)
    return 3;
  auto rendered = false;
  auto previous_frame = started;
  QObject::connect(
      window, &QQuickWindow::frameSwapped, &app,
      [&] {
        const auto now = std::chrono::steady_clock::now();
        if (rendered)
          metrics.record(
              {"frame_interval", formaview::CachePath::NotApplicable},
              now - previous_frame, std::chrono::microseconds{33300});
        previous_frame = now;
        if (!rendered) {
          rendered = true;
          metrics.record({"host_start_to_frame", formaview::CachePath::Cold},
                         now - started);
          std::cout << "HOST_READY rendered_frame=1 qt=" << qVersion()
                    << " capabilities=" << capabilityCount << '\n';
          if (smoke)
            QCoreApplication::quit();
        }
      },
      Qt::QueuedConnection);
  auto timer = QTimer{};
  auto previous_tick = std::chrono::steady_clock::now();
  QObject::connect(&timer, &QTimer::timeout, &app, [&] {
    const auto now = std::chrono::steady_clock::now();
    metrics.record({"event_loop_interval", formaview::CachePath::NotApplicable},
                   now - previous_tick);
    previous_tick = now;
  });
  timer.start(16);
  if (smoke)
    QTimer::singleShot(10000, &app, [window] {
      std::cerr << "HOST_TIMEOUT visible=" << window->isVisible()
                << " exposed=" << window->isExposed()
                << " platform=" << QGuiApplication::platformName().toStdString()
                << '\n';
      QCoreApplication::exit(4);
    });
  window->show(); // Install frame observation before visibility starts the
                  // render thread.
  const auto openIndex = app.arguments().indexOf("--open");
  if (openIndex >= 0 && openIndex + 1 < app.arguments().size())
    desktop.openPath(app.arguments()[openIndex + 1]);
  const auto exit_code = app.exec();
  const auto args = app.arguments();
  const auto metrics_index = args.indexOf("--metrics");
  if (metrics_index >= 0 && metrics_index + 1 < args.size()) {
    auto object = QJsonObject{
        {"boundary",
         "Desktop host; frame/timer observations are not input latency"},
        {"qt", qVersion()},
        {"rendered", rendered}};
    if (const auto summary = metrics.summary(
            {"host_start_to_frame", formaview::CachePath::Cold})) {
      object.insert("start_to_frame_ms", summary->p95_ms);
    }
    for (const auto *name : {"frame_interval", "event_loop_interval"}) {
      if (const auto summary =
              metrics.summary({name, formaview::CachePath::NotApplicable})) {
        object.insert(
            name,
            QJsonObject{
                {"observations", static_cast<qint64>(summary->observations)},
                {"p95_ms", summary->p95_ms},
                {"p99_ms", summary->p99_ms},
                {"max_ms", summary->max_ms},
                {"long_events", static_cast<qint64>(summary->long_events)}});
      }
    }
    if (const auto memory = formaview::process_memory()) {
      object.insert("working_set_bytes",
                    static_cast<qint64>(memory->working_set_bytes));
      object.insert("peak_working_set_bytes",
                    static_cast<qint64>(memory->peak_working_set_bytes));
    }
    auto output = QFile{args[metrics_index + 1]};
    if (!output.open(QIODevice::WriteOnly) ||
        output.write(QJsonDocument{object}.toJson()) < 0)
      return 5;
  }
  return exit_code;
}
