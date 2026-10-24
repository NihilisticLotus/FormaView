#include "window_capture.hpp"
#include "desktop.hpp"
#include "desktop_trace.hpp"
#include "window_chrome.hpp"
#include <QFileInfo>
#include <QSettings>
#include <QScopeGuard>
#include <QThread>
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
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStyleHints>
#include <QTimer>
#ifdef QT_QML_DEBUG
#include <QCryptographicHash>
#include <QDir>
#include <QPointer>
#include <QQmlContext>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtCore/private/qabstractfileengine_p.h>
#include <QtCore/private/qfsfileengine_p.h>
#include <filesystem>
#include <fstream>
#endif
#include <atomic>
#include <chrono>
#include <iostream>
#include <string_view>

#ifdef QT_QML_DEBUG
namespace {
class PreviewAssetFiles final : public QAbstractFileEngineHandler {
public:
  explicit PreviewAssetFiles(QString module) : module_(std::move(module)) {}
  std::uint64_t requests() const { return requests_.load(); }
  std::uint64_t archives() const { return archives_.load(); }
  std::unique_ptr<QAbstractFileEngine>
  create(const QString &name) const override {
    if (name.startsWith(':') || name.startsWith("qrc:"))
      return {};
    std::error_code error;
    const auto path = std::filesystem::absolute(
        std::filesystem::path(name.toStdWString()), error);
    if (error)
      return {};
    const auto normalized = QDir::fromNativeSeparators(
        QString::fromStdWString(path.lexically_normal().wstring()));
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    if (normalized.compare(module_, sensitivity) == 0 ||
        normalized.startsWith(module_ + '/', sensitivity))
      return {};
    // Only development QML belongs on the preview connection. In particular,
    // statting an asset must not make Qt's preview engine read its whole body.
    ++requests_;
    if (normalized.endsWith(".rar", Qt::CaseInsensitive) ||
        normalized.endsWith(".zip", Qt::CaseInsensitive) ||
        normalized.endsWith(".7z", Qt::CaseInsensitive))
      ++archives_;
    return std::make_unique<QFSFileEngine>(name);
  }

private:
  QString module_;
  mutable std::atomic<std::uint64_t> requests_{}, archives_{};
};
} // namespace
#endif

namespace {
// Named functions avoid the stateless-functor template path that traps in
// Qt 6.12 headers with clang-cl 19 UBSan; keep the same signal semantics.
void installCreatedWindowChrome(QObject *object, const QUrl &) {
  if (auto *quick = qobject_cast<QQuickWindow *>(object))
    installWindowChrome(quick);
}
void failRootCreation() { QCoreApplication::exit(2); }
} // namespace

class DesktopApplication final : public QGuiApplication {
public:
  using QGuiApplication::QGuiApplication;
  DesktopTrace *trace{};
  bool notify(QObject *receiver,QEvent *event) override {
    if (QThread::currentThread()!=thread() || !trace)
      return QGuiApplication::notify(receiver,event);
    trace->beginDispatch(receiver,event);
    const auto restore=qScopeGuard([this]{trace->endDispatch();});
    return QGuiApplication::notify(receiver,event);
  }
};

int main(int argc, char *argv[]) {
  const auto applicationStarted = std::chrono::steady_clock::now();
#if defined(QT_QML_DEBUG) && defined(FORMAVIEW_PREVIEW_PLUGIN_DIR)
  QFile plugin(QStringLiteral(FORMAVIEW_PREVIEW_PLUGIN_DIR) +
               "/qmltooling/qmldbg_preview.dll");
  if (!plugin.open(QIODevice::ReadOnly) ||
      QCryptographicHash::hash(plugin.readAll(), QCryptographicHash::Sha256).toHex() !=
          QByteArray(FORMAVIEW_PREVIEW_PLUGIN_SHA256)) {
    std::cerr << "Isolated QML preview plugin fingerprint mismatch\n";
    return 7;
  }
  qputenv("QT_PLUGIN_PATH", QByteArray(FORMAVIEW_PREVIEW_PLUGIN_DIR) + ';' +
                                qgetenv("QT_PLUGIN_PATH"));
#endif
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
  auto app = DesktopApplication{argc, argv};
  app.styleHints()->setColorScheme(Qt::ColorScheme::Dark);
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
  // Development preview sessions must not overwrite the user's settings or
  // jobs.
  const auto previewRoot = app.applicationDirPath() + "/preview-state";
  QDir().mkpath(previewRoot);
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, previewRoot);
  qputenv("FORMAVIEW_IMAGE_STATE_ROOT",
          (previewRoot + "/image-journal").toUtf8());
#endif
#ifdef QT_QML_DEBUG
  std::unique_ptr<PreviewAssetFiles> previewAssetFiles;
#endif
  const auto traceArgument = app.arguments().indexOf("--trace");
  QString tracePath;
  if (traceArgument >= 0 && traceArgument + 1 < app.arguments().size()) {
    tracePath = app.arguments()[traceArgument + 1];
    const auto traceRoot = QFileInfo(tracePath).absolutePath();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       traceRoot + "/settings");
    qputenv("FORMAVIEW_IMAGE_STATE_ROOT",
            (traceRoot + "/image-journal").toUtf8());
  }
  auto desktop = Desktop{};
#ifdef QT_QML_DEBUG
  // The MCP launcher has no --resource argument. Serve a development-only
  // filesystem module so it can watch QML while retaining singleton metadata.
  // Windows app-data locations include the executable directory. QmlPreview
  // excludes those paths, so keep live QML in a sibling development directory.
  const auto executableDirectory =
      std::filesystem::path(app.applicationDirPath().toStdWString());
  const auto liveDirectory = executableDirectory.parent_path() /
                             ".formaview-qml-preview" /
                             executableDirectory.filename();
  const auto previewImports = QDir::fromNativeSeparators(
      QString::fromStdWString(liveDirectory.wstring()));
  const auto previewModule = previewImports + "/FormaView/Engineering";
  std::error_code moduleError;
  std::filesystem::create_directories(
      std::filesystem::path(previewModule.toStdWString()), moduleError);
  if (moduleError)
    return 6;
  QFile moduleSource(":/qt/qml/FormaView/Engineering/qmldir");
  std::ofstream moduleTarget(
      std::filesystem::path((previewModule + "/qmldir").toStdWString()),
      std::ios::binary | std::ios::trunc);
  if (!moduleSource.open(QIODevice::ReadOnly) || !moduleTarget)
    return 6;
  for (const auto &line : moduleSource.readAll().split('\n'))
    if (!line.startsWith("prefer ")) {
      moduleTarget.write(line.constData(), line.size());
      moduleTarget.put('\n');
    }
  moduleTarget.close();
  QTimer previewSync;
  previewSync.setInterval(250);
  const QDir sourceDirectory(QStringLiteral(FORMAVIEW_QML_SOURCE_DIR));
  int previewSyncCount = 0;
  int previewSourceFileCount = 0;
  QString previewSyncError;
  QString previewLastSyncError;
  const auto syncSources = [&] {
    ++previewSyncCount;
    previewSourceFileCount = 0;
    previewSyncError.clear();
    // The preview virtualizes QDir as well as QFile; inspect editor files
    // natively.
    std::error_code directoryError;
    const auto sourceRoot =
        std::filesystem::path(sourceDirectory.absolutePath().toStdWString());
    auto entries =
        std::filesystem::directory_iterator(sourceRoot, directoryError);
    std::uint64_t sourceBytes = 0;
    for (; !directoryError && entries != std::filesystem::directory_iterator{};
         entries.increment(directoryError)) {
      const auto &entry = *entries;
      if (!entry.is_regular_file(directoryError))
        continue;
      const auto extension = entry.path().extension().u8string();
      if (extension != u8".qml" && extension != u8".js")
        continue;
      const auto sourcePath = QString::fromStdWString(entry.path().wstring());
      const auto name =
          QString::fromStdWString(entry.path().filename().wstring());
      const auto size = entry.file_size(directoryError);
      if (directoryError)
        break;
      sourceBytes += size;
      if (++previewSourceFileCount > 1024 || size > 2ULL * 1024 * 1024 ||
          sourceBytes > 32ULL * 1024 * 1024) {
        previewSyncError = "Preview source count/size budget exceeded";
        break;
      }
      // QmlPreview intercepts QFile reads; native streams must read the
      // editor's current bytes instead of the preview service's cached copy.
      std::ifstream source(std::filesystem::path(sourcePath.toStdWString()),
                           std::ios::binary);
      const auto targetPath =
          std::filesystem::path((previewModule + "/" + name).toStdWString());
      if (!source) {
        previewSyncError = sourcePath;
        continue;
      }
      const std::string bytes((std::istreambuf_iterator<char>(source)), {});
      std::ifstream previous(targetPath, std::ios::binary);
      const std::string current((std::istreambuf_iterator<char>(previous)), {});
      previous.close();
      if (current != bytes) {
        // Publish complete bytes. The official preview server re-adds a
        // replaced file when its normalized parent directory changes.
        // Native I/O bypasses the preview engine's read-only QFile view.
        auto temporary = targetPath;
        temporary +=
            "." + std::to_string(QCoreApplication::applicationPid()) + ".tmp";
        std::ofstream target(temporary, std::ios::binary | std::ios::trunc);
        target.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        target.close();
        std::error_code error;
        if (target.fail())
          error = std::make_error_code(std::errc::io_error);
        else
          std::filesystem::rename(temporary, targetPath, error);
        if (error) {
          previewSyncError = QString::fromStdWString(targetPath.wstring()) +
                             ": " + QString::fromStdString(error.message());
          std::error_code cleanupError;
          std::filesystem::remove(temporary, cleanupError);
        }
      }
    }
    if (directoryError)
      previewSyncError = QString::fromStdString(directoryError.message());
    if (!previewSyncError.isEmpty())
      previewLastSyncError = previewSyncError;
  };
  syncSources();
  QObject::connect(&previewSync, &QTimer::timeout, &app, syncSources);
  previewSync.start();
#endif
  auto engine = QQmlApplicationEngine{};
  QObject::connect(&engine, &QQmlApplicationEngine::objectCreated, &app,
                   &installCreatedWindowChrome);
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
      &failRootCreation, Qt::QueuedConnection);
#ifdef QT_QML_DEBUG
  engine.load(QUrl::fromLocalFile(previewModule + "/Main.qml"));
  // Register after QML's preview service installs its global handler: newest
  // handlers run first, so ordinary assets keep native file semantics.
  previewAssetFiles = std::make_unique<PreviewAssetFiles>(previewModule);
#else
  engine.loadFromModule("FormaView.Engineering", "Main");
#endif
  if (engine.rootObjects().isEmpty())
    return 2;
  auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().front());
  if (!window)
    return 3;
  std::unique_ptr<DesktopTrace> nativeTrace;
  if (!tracePath.isEmpty())
    nativeTrace = std::make_unique<DesktopTrace>(window, &desktop, tracePath,
                                                 applicationStarted);
  app.trace=nativeTrace.get();
#ifdef QT_QML_DEBUG
  window->setTitle("FormaView · QML Preview");
  QPointer<QQuickWindow> previewWindow = window;
  QTimer previewCapture;
  QObject::connect(&previewCapture, &QTimer::timeout, &app, [&] {
    if (!previewWindow) {
      for (auto *candidate : QGuiApplication::topLevelWindows())
        if (auto *quick = qobject_cast<QQuickWindow *>(candidate);
            quick && quick->isVisible() &&
            quick->title().startsWith("FormaView")) {
          previewWindow = quick;
          break;
        }
    }
    if (!previewWindow)
      return;
    const auto requestPath = std::filesystem::path(
        (previewRoot + "/capture.request").toStdWString());
    std::ifstream request(requestPath, std::ios::binary);
    if (!request)
      return;
    const std::string requestedName((std::istreambuf_iterator<char>(request)),
                                    {});
    const auto name = QString::fromUtf8(requestedName).trimmed();
    request.close();
    std::error_code removeError;
    std::filesystem::remove(requestPath, removeError);
    static const QRegularExpression safeName("^[a-zA-Z0-9_-]{1,80}$");
    if (!safeName.match(name).hasMatch())
      return;
    const auto captured = captureExposedWindow(previewWindow);
    const auto path = previewRoot + "/" + name;
    const auto saved = captured.save(path + ".png");
    QJsonObject uiState;
    const auto findVisual = [](auto &&self, QQuickItem *parent,
                               const QString &name) -> QQuickItem * {
      if (parent->objectName() == name)
        return parent;
      for (auto *child : parent->childItems())
        if (auto *found = self(self, child, name))
          return found;
      return nullptr;
    };
    for (const auto *objectName :
         {"workspace", "filesPane", "fileList", "imageViewport", "pixelToggle",
          "navigationRail", "nav-assets", "convertImagesButton", "tasksButton",
          "nav-settings", "folderNavigation", "modelViewport", "uvViewport",
          "modelPointToggle"}) {
      if (auto *object = findVisual(findVisual, previewWindow->contentItem(),
                                    QString::fromLatin1(objectName))) {
        QJsonObject state;
        if (const auto *context = QQmlEngine::contextForObject(object))
          state.insert("source_url", context->baseUrl().toString());
        for (const auto *property :
             {"visible", "width", "height", "focusMode", "currentPage",
              "contentY", "checked", "zoom", "fitMode", "pixelScale", "text",
              "page", "section", "representation"}) {
          const auto value = object->property(property);
          if (value.isValid())
            state.insert(property, QJsonValue::fromVariant(value));
        }
        if (object->property("pan").isValid()) {
          const auto pan = object->property("pan").toPointF();
          state.insert("pan", QJsonArray{pan.x(), pan.y()});
        }
        uiState.insert(objectName, state);
      }
    }
    QFile metadata(path + ".json");
    if (metadata.open(QIODevice::WriteOnly))
      metadata.write(
          QJsonDocument(
              QJsonObject{
                  {"qt", qVersion()},
                  {"saved", saved},
                  {"pid", QCoreApplication::applicationPid()},
                  {"root_address",
                   QString::number(
                       reinterpret_cast<quintptr>(previewWindow.data()), 16)},
                  {"window_visibility",
                   static_cast<int>(previewWindow->visibility())},
                  {"window_x", previewWindow->x()},
                  {"window_y", previewWindow->y()},
                  {"directory", desktop.directory()},
                  {"asset_path", desktop.asset().value("path").toString()},
                  {"session_revision",
                   static_cast<qint64>(desktop.sessionRevision())},
                  {"selection_index", desktop.currentIndex()},
                  {"selection_count", desktop.selectedCount()},
                  {"native_file_requests",
                   static_cast<qint64>(previewAssetFiles->requests())},
                  {"native_archive_requests",
                   static_cast<qint64>(previewAssetFiles->archives())},
                  {"scroll_position", desktop.scrollPosition()},
                  {"ui_state", uiState},
                  {"source_directory", sourceDirectory.absolutePath()},
                  {"live_module", previewModule},
                  {"app_data_locations",
                   QJsonArray::fromStringList(QStandardPaths::standardLocations(
                       QStandardPaths::AppDataLocation))},
                  {"source_files", previewSourceFileCount},
                  {"sync_count", previewSyncCount},
                  {"sync_error", previewSyncError},
                  {"last_sync_error", previewLastSyncError},
                  {"dpr", previewWindow->devicePixelRatio()},
                  {"logical_width", previewWindow->width()},
                  {"logical_height", previewWindow->height()},
                  {"physical_width", captured.width()},
                  {"physical_height", captured.height()}})
              .toJson());
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
  if (smoke || !tracePath.isEmpty() || app.arguments().contains("--metrics"))
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
  auto exit_code = app.exec();
  app.trace=nullptr;
  if (nativeTrace && !nativeTrace->flush(true) && exit_code == 0)
    exit_code = 6; // Requested trace could not be saved completely.
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
