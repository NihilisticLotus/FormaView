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
#ifdef QT_QML_DEBUG
#include <QDir>
#include <QRegularExpression>
#include <QPointer>
#include <QSettings>
#include <filesystem>
#include <fstream>
#endif
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
  font.setFamilies({"Segoe UI Variable Text", "Microsoft YaHei UI"});
  font.setPixelSize(13);
  app.setFont(font);
  QCoreApplication::setOrganizationName("FormaView");
  QCoreApplication::setApplicationName("FormaView");
#ifdef QT_QML_DEBUG
  // Development preview sessions must not overwrite the user's settings or jobs.
  const auto previewRoot = app.applicationDirPath() + "/preview-state";
  QDir().mkpath(previewRoot);
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, previewRoot);
  qputenv("FORMAVIEW_IMAGE_STATE_ROOT", (previewRoot + "/image-journal").toUtf8());
#endif
  auto desktop = Desktop{};
#ifdef QT_QML_DEBUG
  // The MCP launcher has no --resource argument. Serve a development-only
  // filesystem module so it can watch QML while retaining singleton metadata.
  const auto previewImports = previewRoot + "/qml";
  const auto previewModule = previewImports + "/FormaView/Engineering";
  QDir().mkpath(previewModule);
  QFile moduleSource(":/qt/qml/FormaView/Engineering/qmldir");
  QFile moduleTarget(previewModule + "/qmldir");
  if (!moduleSource.open(QIODevice::ReadOnly) ||
      !moduleTarget.open(QIODevice::WriteOnly))
    return 6;
  for (const auto &line : moduleSource.readAll().split('\n'))
    if (!line.startsWith("prefer "))
      moduleTarget.write(line + '\n');
  moduleTarget.close();
  QTimer previewSync;
  previewSync.setInterval(250);
  const QDir sourceDirectory(QStringLiteral(FORMAVIEW_QML_SOURCE_DIR));
  int previewSyncCount = 0;
  QString previewSyncError;
  const auto syncSources = [&] {
    ++previewSyncCount;
    for (const auto &name : sourceDirectory.entryList({"*.qml", "*.js"}, QDir::Files)) {
      const auto sourcePath = sourceDirectory.filePath(name);
      // QmlPreview intercepts QFile reads; native streams must read the editor's
      // current bytes instead of the preview service's cached copy.
      std::ifstream source(std::filesystem::path(sourcePath.toStdWString()), std::ios::binary);
      const auto targetPath = std::filesystem::path((previewModule + "/" + name).toStdWString());
      if (!source) {
        previewSyncError = sourcePath;
        continue;
      }
      const std::string bytes((std::istreambuf_iterator<char>(source)), {});
      std::ifstream previous(targetPath, std::ios::binary);
      const std::string current((std::istreambuf_iterator<char>(previous)), {});
      previous.close();
      if (current != bytes) {
        std::ofstream target(targetPath, std::ios::binary);
        target.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!target)
          previewSyncError = QString::fromStdWString(targetPath.wstring());
      }
    }
  };
  syncSources();
  QObject::connect(&previewSync, &QTimer::timeout, &app, syncSources);
  previewSync.start();
#endif
  auto engine = QQmlApplicationEngine{};
#ifdef QT_QML_DEBUG
  engine.addImportPath(previewImports);
#endif
  engine.addImageProvider("thumb",
                          std::make_unique<ThumbnailProvider>().release());
  engine.setInitialProperties({{"controller", QVariant::fromValue(&desktop)}});
  auto metrics = formaview::PerformanceRecorder{};
  const auto smoke = app.arguments().contains("--smoke");
  QObject::connect(
      &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
      [] { QCoreApplication::exit(2); }, Qt::QueuedConnection);
#ifdef QT_QML_DEBUG
  engine.load(QUrl::fromLocalFile(previewModule + "/Main.qml"));
#else
  engine.loadFromModule("FormaView.Engineering", "Main");
#endif
  if (engine.rootObjects().isEmpty())
    return 2;
  auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().front());
  if (!window)
    return 3;
#ifdef QT_QML_DEBUG
  window->setTitle("FormaView · QML Preview");
  QPointer<QQuickWindow> previewWindow = window;
  QTimer previewCapture;
  QObject::connect(&previewCapture, &QTimer::timeout, &app, [&] {
    if (!previewWindow) {
      for (auto *candidate : QGuiApplication::topLevelWindows())
        if (auto *quick = qobject_cast<QQuickWindow *>(candidate);
            quick && quick->isVisible() && quick->title().startsWith("FormaView")) {
          previewWindow = quick;
          break;
        }
    }
    if (!previewWindow)
      return;
    const auto requestPath = std::filesystem::path((previewRoot + "/capture.request").toStdWString());
    std::ifstream request(requestPath, std::ios::binary);
    if (!request)
      return;
    const std::string requestedName((std::istreambuf_iterator<char>(request)), {});
    const auto name = QString::fromUtf8(requestedName).trimmed();
    request.close();
    std::error_code removeError;
    std::filesystem::remove(requestPath, removeError);
    static const QRegularExpression safeName("^[a-zA-Z0-9_-]{1,80}$");
    if (!safeName.match(name).hasMatch())
      return;
    const auto captured = previewWindow->grabWindow();
    const auto path = previewRoot + "/" + name;
    const auto saved = captured.save(path + ".png");
    QFile metadata(path + ".json");
    if (metadata.open(QIODevice::WriteOnly))
      metadata.write(QJsonDocument(QJsonObject{
          {"qt", qVersion()}, {"saved", saved},
          {"source_directory", sourceDirectory.absolutePath()},
          {"source_files", sourceDirectory.entryList({"*.qml", "*.js"}, QDir::Files).size()},
          {"sync_count", previewSyncCount}, {"sync_error", previewSyncError},
          {"dpr", previewWindow->devicePixelRatio()},
          {"logical_width", previewWindow->width()}, {"logical_height", previewWindow->height()},
          {"physical_width", captured.width()}, {"physical_height", captured.height()}}).toJson());
  });
  previewCapture.start(250);
#endif
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
