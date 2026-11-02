#include "caption_evidence.hpp"
#include "window_capture.hpp"
#include "stress_trace.hpp"
#include "desktop.hpp"
#include "window_chrome.hpp"
#include "formaview/telemetry.hpp"
#include "thumbnail.hpp"
#include <QImageReader>
#ifdef FORMAVIEW_MODEL_UI
#include "model_session.hpp"
#include "model_view.hpp"
#include "model_document.hpp"
#endif
#ifdef FORMAVIEW_IMAGE_UI
#include "image_jobs.hpp"
#ifdef FORMAVIEW_MODEL_EXPORT
#include "formaview/fbx.hpp"
#endif
#include "image_session.hpp"
#include "image_view.hpp"
#include <cstring>
#endif
#include <QBuffer>
#include <QClipboard>
#include <QDataStream>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLocalSocket>
#include <QMimeData>
#include <QMetaProperty>
#include <QProcess>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlIncubationController>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSaveFile>
#include <QScopeGuard>
#include <QUuid>
#include <QAbstractNativeEventFilter>
#include <QScreen>
#include <QSettings>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QWheelEvent>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <thread>
#include <stdexcept>
namespace {
class MissingFixture final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
QString requireFixture(const char *variable, bool optional = false) {
  const auto value = QDir::fromNativeSeparators(qEnvironmentVariable(variable));
  if (value.isEmpty() && optional) return {};
  if (value.isEmpty())
    throw MissingFixture(std::string("Missing fixture environment: ") + variable);
  for (const auto &path : value.split(';', Qt::SkipEmptyParts))
    if (!QFileInfo::exists(path))
      throw MissingFixture(std::string(variable) + ": missing fixture " + path.toStdString());
  return value;
}
bool workflowHeadless() {
  return qEnvironmentVariableIsSet("FORMAVIEW_TEST_HEADLESS");
}
#ifdef FORMAVIEW_IMAGE_UI
// These waits establish correctness states for the same 64 inputs. ASan
// instrumentation is not subject to the Release validation deadlines;
// throughput and latency targets are measured separately.
#if defined(__SANITIZE_ADDRESS__)
constexpr int reviewPreflightWaitMs = 90000;
constexpr int reviewProducerSignalWaitMs = 135000;
#else
constexpr int reviewPreflightWaitMs = 30000;
constexpr int reviewProducerSignalWaitMs = 45000;
#endif
constexpr int reviewProducerPreflightWaitMs = reviewPreflightWaitMs + 5000;
#endif
}
#ifdef Q_OS_WIN
#include <d3d11.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>
#endif

class MeasuredApplication final : public QGuiApplication {
public:
  using QGuiApplication::QGuiApplication;
  bool measure{};
  double maxEventMs{};
  int events{}, over100ms{};
  QJsonArray slowEvents;
  qint64 nativeDownNs{};
  stress_trace::Trace *stressTrace{}; // GUI-only; owned by the active test scope.
  bool notify(QObject *receiver, QEvent *event) override {
    if (QThread::currentThread() != thread() || !measure)
      return QGuiApplication::notify(receiver, event);
    const auto started = std::chrono::steady_clock::now();
    const auto eventType = static_cast<int>(event->type());
    const QPointer<QObject> guardedReceiver(receiver);
    auto *const trace=stressTrace;
    const auto token=trace ? trace->enter(receiver,eventType,started) : stress_trace::Trace::Token{};
    auto unwind=qScopeGuard([&] {
      if(trace) trace->leave(token,std::chrono::steady_clock::now(),true);
    });
    if (!nativeDownNs && event->type()==QEvent::KeyPress &&
        static_cast<QKeyEvent *>(event)->key()==Qt::Key_Down)
      nativeDownNs=std::chrono::duration_cast<std::chrono::nanoseconds>(started.time_since_epoch()).count();
    const auto result = QGuiApplication::notify(receiver, event);
    const auto finished=std::chrono::steady_clock::now();
    if(trace) trace->leave(token,finished);
    unwind.dismiss();
    const auto ms = std::chrono::duration<double, std::milli>(
                        finished - started)
                        .count();
    maxEventMs = std::max(maxEventMs, ms);
    ++events;
    if (ms > 100)
      ++over100ms;
    if (ms > 10) {
      // DeferredDelete and event handlers may destroy their receiver. The
      // diagnostic must not dereference it (or the event) after dispatch.
      const QJsonObject sample{{"ms",ms},{"type",eventType},{"call_id",qint64(token.id)},
        {"receiver",guardedReceiver ? guardedReceiver->metaObject()->className() : "<destroyed>"},
        {"object",guardedReceiver ? guardedReceiver->objectName() : QString{}}};
      if(slowEvents.size()<128) slowEvents.append(sample);
      else {
        int smallest=0;
        for(int i=1;i<slowEvents.size();++i) if(slowEvents[i].toObject().value("ms").toDouble()<slowEvents[smallest].toObject().value("ms").toDouble()) smallest=i;
        if(ms>slowEvents[smallest].toObject().value("ms").toDouble()) slowEvents[smallest]=sample;
      }
    }
    return result;
  }
};
QJsonArray gpuMemory() {
  auto result = QJsonArray{};
#ifdef Q_OS_WIN
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    return result;
  auto seen = QSet<QString>{};
  for (UINT i = 0;; ++i) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
      break;
    if (!adapter)
      break;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> advanced;
    if (FAILED(adapter.As(&advanced)))
      continue;
    DXGI_ADAPTER_DESC1 description{};
    if (FAILED(adapter->GetDesc1(&description)))
      continue;
    const auto luid = QString::number(description.AdapterLuid.HighPart) + ":" +
                      QString::number(description.AdapterLuid.LowPart);
    if (seen.contains(luid))
      continue;
    seen.insert(luid);
    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
    if (FAILED(advanced->QueryVideoMemoryInfo(
            0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)))
      continue;
    if (FAILED(advanced->QueryVideoMemoryInfo(
            0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared)))
      continue;
    result.append(QJsonObject{
        {"adapter", QString::fromWCharArray(description.Description)},
        {"luid", luid},
        {"process_local_bytes", static_cast<qint64>(local.CurrentUsage)},
        {"process_shared_bytes", static_cast<qint64>(shared.CurrentUsage)}});
  }
#endif
  return result;
}

class DesktopTests final : public QObject {
  Q_OBJECT
  QString root_, fixture_, small_;
  std::unique_ptr<QTemporaryDir> stateDirectory_, fixtureDirectory_;
  std::unique_ptr<Desktop> desktop_;
  std::unique_ptr<QQmlApplicationEngine> engine_;
  QQuickWindow *window_{};
  QJsonObject evidence_;
  QTimer gpuSampler_;
  QString renderLuid_;
  std::atomic<bool> renderDeviceRecorded_{false};
  qint64 peakGpuLocal_{};
#ifdef Q_OS_WIN
  Microsoft::WRL::ComPtr<IDXGIAdapter3> renderAdapter_;
#endif
  void initializeGpuSampler() {
#ifdef Q_OS_WIN
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    QVERIFY(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    for (UINT i = 0;; ++i) {
      Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
      if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
        break;
      if (!adapter)
        break;
      DXGI_ADAPTER_DESC1 desc{};
      if (FAILED(adapter->GetDesc1(&desc)))
        continue;
      const auto luid = QString::number(desc.AdapterLuid.HighPart) + ":" +
                        QString::number(desc.AdapterLuid.LowPart);
      if (luid == renderLuid_) {
        QVERIFY(SUCCEEDED(adapter.As(&renderAdapter_)));
        break;
      }
    }
    QVERIFY(renderAdapter_);
#endif
  }
  void sampleGpu() {
#ifdef Q_OS_WIN
    DXGI_QUERY_VIDEO_MEMORY_INFO local{};
    if (renderAdapter_ && SUCCEEDED(renderAdapter_->QueryVideoMemoryInfo(
                              0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)))
      peakGpuLocal_ =
          std::max(peakGpuLocal_, static_cast<qint64>(local.CurrentUsage));
#endif
  }
  QList<int> receivedKeys_;
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (event->type() == QEvent::KeyPress)
      receivedKeys_.append(static_cast<QKeyEvent *>(event)->key());
    return QObject::eventFilter(watched, event);
  }
  bool idle() {
    QCoreApplication::processEvents();
    return desktop_->activeTasks() == 0 && !desktop_->busy();
  }
  bool modelPanelsReady() const {
    for(const auto *name:{"modelPaneLoader","modelInspectorLoader"}) {
      const auto *loader=window_->findChild<QObject *>(name);
      if(!loader || loader->property("status").toInt()!=1) return false;
    }
    return true;
  }
  int delegateCount(QQuickItem *item) const {
    auto n = item->objectName() == "fileDelegate" ? 1 : 0;
    for (auto *child : item->childItems())
      n += delegateCount(child);
    return n;
  }
  QQuickItem *item(const char *name) {
    const auto visible = visualItems(window_->contentItem(), QString::fromUtf8(name));
    if (!visible.isEmpty())
      return visible.front();
    for (auto *found : window_->findChildren<QQuickItem *>(name))
      if (found->isVisible())
        return found;
    return nullptr;
  }
  QList<QQuickItem *> visualItems(QQuickItem *root, const QString &name) {
    QList<QQuickItem *> result;
    if (root->objectName() == name && root->isVisible())
      result.append(root);
    for (auto *child : root->childItems())
      result.append(visualItems(child, name));
    return result;
  }
  QImage captureWindow(QQuickWindow *window) {
    const auto image=captureExposedWindow(window);
    if (image.isNull())
      QTest::qFail(qPrintable(QString("Window capture unavailable: window=%1 handle=%2 visible=%3 exposed=%4 visibility=%5 sceneGraph=%6 size=%7x%8")
          .arg(window != nullptr).arg(window && window->handle()).arg(window && window->isVisible())
          .arg(window && window->isExposed()).arg(window ? int(window->visibility()) : -1)
          .arg(window && window->isSceneGraphInitialized()).arg(window ? window->width() : 0)
          .arg(window ? window->height() : 0)), __FILE__, __LINE__);
    return image;
  }
#ifdef FORMAVIEW_IMAGE_UI
  void restorePageReviewUi() {
    auto *workspace = item("workspace");
    auto *service = qobject_cast<ImageJobs *>(desktop_->imageJobs());
    service->setMcpEnabled(false);
    if (auto *settings = window_->findChild<QObject *>("mcpSettingsDialog")) {
      // A hidden dirty draft can make close() open another modal. Reset it
      // through the normal open path and cancel any pending leave callback.
      if (auto *unsaved = window_->findChild<QObject *>("unsavedSettingsDialog"))
        QMetaObject::invokeMethod(unsaved, "close");
      workspace->setProperty("settingsVisible", false);
      QMetaObject::invokeMethod(settings, "open", Q_ARG(QVariant, "general"));
      QMetaObject::invokeMethod(settings, "close");
    }
    workspace->setProperty("tasksVisible", false);
    workspace->setProperty("settingsVisible", false);
    workspace->setProperty("conversionVisible", false);
    workspace->setProperty("focusMode", false);
    window_->resize(1600, 900);
    desktop_->setLanguage("zh-CN");
  }
#endif
  void save(const QString &name) {
    const auto pixels=captureWindow(window_);
    QVERIFY(pixels.save(root_ + "/" + name + ".png"));
    const auto geometry=window_->geometry();
    QJsonObject capture{{"name",name},{"logical_width",geometry.width()},{"logical_height",geometry.height()},
                        {"physical_png_width",pixels.width()},{"physical_png_height",pixels.height()},
                        {"dpr",window_->devicePixelRatio()},{"x",geometry.x()},{"y",geometry.y()}};
    if(auto *screen=window_->screen()) {
      capture["screen"]=screen->name();
      capture["available_width"]=screen->availableGeometry().width();
      capture["available_height"]=screen->availableGeometry().height();
    }
#ifdef Q_OS_WIN
    RECT client{};
    if(GetClientRect(reinterpret_cast<HWND>(window_->winId()),&client)) {
      capture["native_client_width"]=static_cast<int>(client.right-client.left);
      capture["native_client_height"]=static_cast<int>(client.bottom-client.top);
    }
#endif
    auto captures=evidence_["captures"].toArray(); captures.append(capture); evidence_["captures"]=captures;
    sampleGpu();
  }
private slots:
  void initTestCase() {
    root_ = QDir::fromNativeSeparators(
        qEnvironmentVariable("FORMAVIEW_UI_TEST_ROOT"));
    QVERIFY(!root_.isEmpty());
    QVERIFY(QDir().mkpath(root_));
    stateDirectory_ = std::make_unique<QTemporaryDir>(root_ + "/state-XXXXXX");
    QVERIFY(stateDirectory_->isValid());
    qputenv("FORMAVIEW_IMAGE_STATE_ROOT", (stateDirectory_->path() + "/image-journal").toUtf8());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       stateDirectory_->path() + "/settings");
    QVERIFY2(QSettings{}.allKeys().isEmpty(), "GUI run inherited settings from a previous run");
    fixtureDirectory_ = std::make_unique<QTemporaryDir>(root_ + "/fixtures-XXXXXX");
    QVERIFY(fixtureDirectory_->isValid());
    fixture_ = fixtureDirectory_->path() + "/中文目录";
    small_ = fixtureDirectory_->path() + "/其他目录";
    evidence_["state_directory"] = stateDirectory_->path();
    QVERIFY(QDir().mkpath(fixture_));
    QVERIFY(QDir().mkpath(small_));
    for (auto i = 0; i < 10000; ++i) {
      auto file = QFile(fixture_ + QString("/asset%1.obj").arg(i));
      QVERIFY(file.open(QIODevice::WriteOnly));
      QCOMPARE(file.write("# metadata test\n"), 16);
    }
    auto png = QImage(64, 64, QImage::Format_RGBA8888);
    png.fill(Qt::cyan);
    for (auto i = 0; i < 24; ++i)
      QVERIFY(png.save(fixture_ + QString("/图片%1.png").arg(i)));
    QVERIFY(png.save(small_ + "/唯一图片.png"));
    desktop_ = std::make_unique<Desktop>();
    engine_ = std::make_unique<QQmlApplicationEngine>();
    engine_->addImageProvider("thumb",
                              std::make_unique<ThumbnailProvider>().release());
    engine_->setInitialProperties(
        {{"controller", QVariant::fromValue(desktop_.get())}});
    engine_->loadFromModule("FormaView.TestUI", "Main");
    QVERIFY(!engine_->rootObjects().isEmpty());
    window_ = qobject_cast<QQuickWindow *>(engine_->rootObjects().front());
    QVERIFY(window_);
    if (auto *screen = QGuiApplication::primaryScreen()) {
      window_->setScreen(screen);
      window_->setPosition(screen->availableGeometry().topLeft()+QPoint(24,24));
    }
    if (qEnvironmentVariableIsSet("FORMAVIEW_TEST_PASSIVE_WINDOW")) {
      window_->setFlags(window_->flags() | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
      window_->setFlag(Qt::WindowStaysOnTopHint, false);
    }
    if (!workflowHeadless()) installWindowChrome(window_);
#ifdef Q_OS_WIN
    if (!workflowHeadless()) {
    connect(
        window_, &QQuickWindow::beforeRendering, window_,
        [this] {
          if (renderDeviceRecorded_.exchange(true))
            return;
          auto *device = static_cast<ID3D11Device *>(
              window_->rendererInterface()->getResource(
                  window_, QSGRendererInterface::DeviceResource));
          if (!device)
            return;
          Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
          Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
          DXGI_ADAPTER_DESC desc{};
          if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) ||
              FAILED(dxgi->GetAdapter(&adapter)) ||
              FAILED(adapter->GetDesc(&desc)))
            return;
          const auto name = QString::fromWCharArray(desc.Description);
          const auto luid = QString::number(desc.AdapterLuid.HighPart) + ":" +
                            QString::number(desc.AdapterLuid.LowPart);
          QMetaObject::invokeMethod(
              this,
              [this, name, luid] {
                renderLuid_ = luid;
                evidence_["render_adapter"] = QJsonObject{
                    {"name", name}, {"luid", luid}, {"api", "D3D11"}};
              },
              Qt::QueuedConnection);
        },
        Qt::DirectConnection);
    }
#endif
    window_->show();
    QVERIFY(QTest::qWaitForWindowExposed(window_));
    if (!workflowHeadless()) {
      QTRY_VERIFY(!renderLuid_.isEmpty());
      initializeGpuSampler();
      connect(&gpuSampler_, &QTimer::timeout, this, &DesktopTests::sampleGpu);
      gpuSampler_.start(250);
    } else {
      QCOMPARE(QGuiApplication::platformName(), QString("offscreen"));
      QCOMPARE(window_->rendererInterface()->graphicsApi(), QSGRendererInterface::Software);
      evidence_["render_mode"] = "offscreen software; workflow assertions only, no GPU/native-window/visual acceptance";
    }
    evidence_["fixture"] = fixture_;
    evidence_["source"] = "generated directory; 10000 OBJ marker files, 24 "
                          "PNGs; not model compatibility evidence";
  }
  void scanSortFilterAndSelection() {
    auto time = QElapsedTimer{};
    time.start();
    desktop_->navigate(fixture_);
    QTRY_COMPARE_WITH_TIMEOUT(desktop_->count(), 10024, 30000);
    QTRY_VERIFY(idle());
    evidence_["directory_complete_ms"] = time.elapsed();
    const auto delegates = delegateCount(window_->contentItem());
    evidence_["delegates_for_10024_items"] = delegates;
    QVERIFY(delegates > 0 && delegates < 100);
    auto *model = desktop_->files();
    QCOMPARE(model->index(2, 0).data(FileList::Name).toString(),
             QString("asset2.obj"));
    desktop_->select(2);
    QTRY_COMPARE(desktop_->asset().value("name").toString(),
                 QString("asset2.obj"));
    const auto session = desktop_->sessionRevision();
    desktop_->select(3, Qt::ControlModifier);
    QCOMPARE(desktop_->selectedCount(), 2);
    QCOMPARE(desktop_->sessionRevision(), session);
    desktop_->filter("图片", 1, 0);
    QTRY_COMPARE(desktop_->count(), 24);
    QTRY_VERIFY(idle());
    desktop_->filter("", 0, 1);
    QTRY_COMPARE(desktop_->count(), 10024);
    QTRY_VERIFY(idle());
    QVERIFY(model->index(0, 0).data(FileList::Name).toString() !=
            QString("asset0.obj"));
    desktop_->filter("", 0, 0);
    QTRY_VERIFY(idle());
    save("01-files");
  }
  void folderMenuIsolationAndPaths() {
    const auto base = root_ + "/目录导航/Model";
    const auto texture = base + "/克劳德/Texture";
    QVERIFY(QDir().mkpath(texture));
    QVERIFY(QDir().mkpath(base + "/场景"));
    QVERIFY(QDir().mkpath(base + "/道具"));
    desktop_->navigate(small_);
    QTRY_COMPARE(desktop_->directory(), small_);
    QTRY_VERIFY(idle());
    desktop_->select(0);
    QTRY_VERIFY(!desktop_->asset().isEmpty());
    QTRY_VERIFY(idle());
    const auto session = desktop_->sessionRevision();
    const auto selection = desktop_->currentIndex();
    const auto tasks = desktop_->tasks().size();
    desktop_->saveView(3);
    desktop_->listFolders(base);
    QTRY_VERIFY(!desktop_->folderMenu().value("busy").toBool());
    QCOMPARE(desktop_->folderMenu().value("entries").toList().size(), 3);
    QCOMPARE(desktop_->directory(), small_);
    QCOMPARE(desktop_->currentIndex(), selection);
    QCOMPARE(desktop_->sessionRevision(), session);
    QCOMPARE(desktop_->scrollPosition(), 3.0);
    QCOMPARE(desktop_->tasks().size(), tasks);
    for (auto i = 0; i < 8; ++i) {
      desktop_->listFolders(fixture_);
      desktop_->listFolders(base + "/克劳德");
    }
    QTRY_VERIFY(!desktop_->folderMenu().value("busy").toBool());
    QCOMPARE(desktop_->folderMenu().value("path").toString(), base + "/克劳德");
    QCOMPARE(desktop_->folderMenu().value("entries").toList().size(), 1);
    QCOMPARE(desktop_->folderMenu().value("entries").toList()[0].toMap()["path"].toString(), texture);
    desktop_->listFolders(fixture_);
    desktop_->closeFolderMenu();
    QTest::qWait(200);
    QVERIFY(desktop_->folderMenu().isEmpty());
    desktop_->listFolders(base + "/不存在");
    QTRY_VERIFY(!desktop_->folderMenu().value("busy").toBool());
    QVERIFY(!desktop_->folderMenu().value("error").toString().isEmpty());
    QVERIFY(desktop_->error().isEmpty());
    QCOMPARE(desktop_->sessionRevision(), session);
    desktop_->listFolders(base);
    desktop_->navigate(texture);
    QTRY_COMPARE(desktop_->directory(), texture);
    QTRY_VERIFY(idle());
    QVERIFY(desktop_->folderMenu().isEmpty());
    QCOMPARE(desktop_->sessionRevision(), session);
    const auto fixtureDrive = QDir::fromNativeSeparators(QFileInfo(root_).absoluteFilePath()).left(3);
    const auto drive = desktop_->breadcrumbs(fixtureDrive + "Model/克劳德/Texture/");
    QCOMPARE(drive.size(), 4);
    QCOMPARE(drive[0].toMap()["path"].toString(), fixtureDrive);
    QCOMPARE(drive[1].toMap()["path"].toString(), fixtureDrive + "Model");
    const auto unc = desktop_->breadcrumbs("\\\\server\\share\\中文\\Texture");
    QCOMPARE(unc.size(), 3);
    QCOMPARE(unc[0].toMap()["path"].toString(), QString("//server/share"));
    QCOMPARE(unc.last().toMap()["path"].toString(), QString("//server/share/中文/Texture"));
    QCOMPARE(desktop_->breadcrumbs("C:/").size(), 1);
    QVERIFY(desktop_->breadcrumbs("").isEmpty());
    desktop_->history(-1);
    QTRY_COMPARE(desktop_->directory(), small_);
    QTRY_VERIFY(idle());
    QCOMPARE(desktop_->currentIndex(), selection);
    QCOMPARE(desktop_->selectedCount(), 1);
  }
  void folderNavigationInteraction() {
    QVERIFY(QDir().mkpath(root_ + "/目录导航/Model/克劳德/Texture"));
    QVERIFY(QDir().mkpath(root_ + "/目录导航/Model/场景"));
    QVERIFY(QDir().mkpath(root_ + "/目录导航/Model/道具"));
    window_->resize(1800, 1000);
    auto *workspace = item("workspace");
    QVERIFY(workspace);
    workspace->setProperty("tasksVisible", false);
    workspace->setProperty("settingsVisible", false);
    workspace->setProperty("conversionVisible", false);
    workspace->setProperty("focusMode", false);
    desktop_->preference("filesVisible", true);
    desktop_->navigate(root_ + "/目录导航/Model/克劳德/Texture");
    QTRY_COMPARE(desktop_->directory(), root_ + "/目录导航/Model/克劳德/Texture");
    QTRY_VERIFY(idle());
    auto *navigation = item("folderNavigation");
    QVERIFY(navigation);
    const auto findPath = [&](const char *name, const QString &path) -> QQuickItem * {
      for (auto *candidate : visualItems(window_->contentItem(), name))
        if (candidate->property("folderPath").toString() == path) return candidate;
      return nullptr;
    };
    const auto click = [&](QQuickItem *target) {
      QVERIFY(target);
      const auto targetName=target->objectName();
      const auto targetPath=target->property("folderPath").toString();
      const auto before=target->mapToScene(QPointF(target->width()/2,target->height()/2));
      QPointer<QQuickItem> initialTarget=target;
      QSignalSpy readyFrame(window_,&QQuickWindow::frameSwapped);
      window_->update();
      QTRY_VERIFY(!readyFrame.isEmpty());
      QQuickItem *ready=nullptr;
      const auto nameBytes=targetName.toUtf8();
      for(auto *candidate:visualItems(window_->contentItem(),nameBytes.constData()))
        if(candidate->isVisible() && candidate->isEnabled() && candidate->property("folderPath").toString()==targetPath) { ready=candidate; break; }
      QVERIFY(ready);
      const auto position=ready->mapToScene(QPointF(ready->width()/2,ready->height()/2));
      const bool initialDestroyed=initialTarget.isNull();
      QPointer<QQuickItem> readyTarget=ready;
      QTest::mousePress(window_,Qt::LeftButton,Qt::NoModifier,position.toPoint());
      auto *receiver=window_->mouseGrabberItem();
      const auto receiverName=receiver ? receiver->objectName() : QString{};
      while(receiver && receiver!=readyTarget.data()) receiver=receiver->parentItem();
      const bool hitTarget=receiver && readyTarget;
      QTest::mouseRelease(window_,Qt::LeftButton,Qt::NoModifier,position.toPoint());
      auto clicks=evidence_["folder_navigation_clicks"].toArray();
      clicks.append(QJsonObject{{"name",targetName},{"path",targetPath},{"before_x",before.x()},{"before_y",before.y()},{"ready_x",position.x()},{"ready_y",position.y()},{"initial_destroyed",initialDestroyed},{"receiver",receiverName},{"hit_target",hitTarget}});
      evidence_["folder_navigation_clicks"]=clicks;
      QVERIFY(hitTarget);
    };
    const auto model = root_ + "/目录导航/Model";
    const auto session = desktop_->sessionRevision();
    QTest::qWait(100);
    save("folder-before-model-arrow");
    auto *arrow = findPath("breadcrumbChildren", model);
    QTRY_VERIFY((arrow = findPath("breadcrumbChildren", model)));
    QVERIFY(arrow->width() >= 32 && arrow->height() >= 32);
    auto *preview = item("previewPane");
    const auto previewHeight = preview ? preview->height() : 0;
    QElapsedTimer menuClock; menuClock.start();
    click(arrow);
    QTRY_COMPARE(desktop_->folderMenu().value("entries").toList().size(), 3);
    QTRY_VERIFY(!desktop_->folderMenu().value("busy").toBool());
    const auto entriesReadyMs=menuClock.elapsed();
    QCOMPARE(desktop_->sessionRevision(), session);
    if (preview) QCOMPARE(preview->height(), previewHeight);
    auto *sibling = findPath("folderChild", model + "/场景");
    QTRY_VERIFY((sibling = findPath("folderChild", model + "/场景")));
    QCOMPARE(sibling->height(), 36.0);
    const auto delegateReadyMs=menuClock.elapsed();
    QPointer<QQuickItem> initialSibling=sibling;
    const auto positionBeforeLayout=sibling->mapToScene(QPointF(sibling->width()/2,sibling->height()/2));
    QSignalSpy menuFrame(window_,&QQuickWindow::frameSwapped);
    window_->update();
    QTRY_VERIFY(!menuFrame.isEmpty());
    const auto frameReadyMs=menuClock.elapsed();
    // ListView can recycle/recreate an initial delegate during its first layout.
    // Resolve the requested path again after presentation, then click it.
    QTRY_VERIFY((sibling=findPath("folderChild",model + "/场景")));
    const auto positionAfterLayout=sibling->mapToScene(QPointF(sibling->width()/2,sibling->height()/2));
    const auto initialPathAfterFrame=initialSibling ? initialSibling->property("folderPath").toString() : QString("destroyed");
    QTest::mousePress(window_,Qt::LeftButton,Qt::NoModifier,positionAfterLayout.toPoint());
    auto *grabber=window_->mouseGrabberItem();
    while(grabber && grabber->objectName()!="folderChild") grabber=grabber->parentItem();
    const auto hitPath=grabber ? grabber->property("folderPath").toString() : QString{};
    QTest::mouseRelease(window_,Qt::LeftButton,Qt::NoModifier,positionAfterLayout.toPoint());
    evidence_["folder_menu_layout"]=QJsonObject{{"entries_ready_ms",entriesReadyMs},{"delegate_ready_ms",delegateReadyMs},{"frame_ready_ms",frameReadyMs},{"before_x",positionBeforeLayout.x()},{"before_y",positionBeforeLayout.y()},{"after_x",positionAfterLayout.x()},{"after_y",positionAfterLayout.y()},{"pressed_folder",hitPath},{"initial_delegate_after_frame",initialPathAfterFrame}};
    QCOMPARE(hitPath,model + "/场景");
    QTRY_COMPARE(desktop_->directory(), model + "/场景");
    QTRY_VERIFY(idle());
    auto *childrenPopup=window_->findChild<QObject *>("folderChildrenPopup");
    QVERIFY(childrenPopup);
    QTRY_VERIFY(!childrenPopup->property("visible").toBool());
    QSignalSpy navigationFrame(window_,&QQuickWindow::frameSwapped);
    window_->update();
    QTRY_VERIFY(!navigationFrame.isEmpty());
    auto *name = findPath("breadcrumbName", model);
    QVERIFY(name);
    click(name);
    QTRY_COMPARE(desktop_->directory(), model);
    QTRY_VERIFY(idle());
    auto *back = item("folderBack");
    QVERIFY(back);
    click(back);
    QTRY_COMPARE(desktop_->directory(), model + "/场景");
    QTRY_VERIFY(idle());
    click(item("folderForward"));
    QTRY_COMPARE(desktop_->directory(), model);
    QTRY_VERIFY(idle());
    click(item("folderBack"));
    QTRY_COMPARE(desktop_->directory(), model + "/场景");
    QTRY_VERIFY(idle());
    click(item("folderUp"));
    QTRY_COMPARE(desktop_->directory(), model);
    QTRY_VERIFY(idle());
    click(item("copyFolderPath"));
    QCOMPARE(QGuiApplication::clipboard()->text(), desktop_->directory());
    QMetaObject::invokeMethod(workspace, "openAddress");
    QTRY_VERIFY(item("address") && item("address")->hasActiveFocus());
    auto *address = item("address");
    QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
    QInputMethodEvent commit; commit.setCommitString(small_);
    QCoreApplication::sendEvent(address, &commit);
    QTest::keyClick(window_, Qt::Key_Return);
    QTRY_COMPARE(desktop_->directory(), small_);
    QTRY_VERIFY(idle());
    // Edit/paste an absolute path on a different drive through the same field.
    QTest::keyClick(window_, Qt::Key_L, Qt::ControlModifier);
    QTRY_VERIFY(item("address") && item("address")->hasActiveFocus());
    QGuiApplication::clipboard()->setText("C:/");
    QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(window_, Qt::Key_V, Qt::ControlModifier);
    QTest::keyClick(window_, Qt::Key_Return);
    QTRY_COMPARE(desktop_->directory(), QString("C:/"));
    QTRY_VERIFY(idle());
    QTest::keyClick(window_, Qt::Key_Left, Qt::AltModifier);
    QTRY_COMPARE(desktop_->directory(), small_);
    QTRY_VERIFY(idle());
    auto longPath = root_ + "/长目录";
    for (auto i = 0; i < 5; ++i) longPath += "/非常长的中文文件夹名称" + QString::number(i);
    QVERIFY(QDir().mkpath(longPath));
    desktop_->navigate(longPath);
    QTRY_COMPARE(desktop_->directory(), longPath);
    QTRY_VERIFY(idle());
    QTRY_VERIFY(item("hiddenAncestors"));
    QVERIFY(item("hiddenAncestors")->width() >= 32);
    QVERIFY(item("hiddenAncestors")->height() >= 32);
    auto *ancestors=window_->findChild<QObject *>("ancestorMenu");
    QVERIFY(ancestors);
    QCOMPARE(ancestors->property("count").toInt(),0);
    click(item("hiddenAncestors"));
    QTRY_VERIFY(ancestors->property("visible").toBool());
    QTRY_VERIFY(ancestors->property("count").toInt()>0);
    QVERIFY(QMetaObject::invokeMethod(ancestors,"close"));
    QTRY_VERIFY(!ancestors->property("visible").toBool());
    QTRY_COMPARE(ancestors->property("count").toInt(),0);
    QCOMPARE(navigation->height(), 38.0);
    window_->resize(1280, 800);
    QMetaObject::invokeMethod(workspace, "openAddress");
    QTRY_VERIFY(item("address") && item("address")->hasActiveFocus());
    QCOMPARE(item("address")->property("text").toString(), longPath);
    QTest::keyClick(window_, Qt::Key_Home);
    QCOMPARE(item("address")->property("cursorPosition").toInt(), 0);
    QTest::keyClick(window_, Qt::Key_End);
    QCOMPARE(item("address")->property("cursorPosition").toInt(), longPath.size());
    QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(window_, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(QGuiApplication::clipboard()->text(), longPath);
    QTest::keyClick(window_, Qt::Key_Escape);
    QTRY_VERIFY(!item("address"));
    QVERIFY(window_->activeFocusItem());
    QTest::keyClick(window_, Qt::Key_Tab);
    QVERIFY(window_->activeFocusItem());
    QTest::keyClick(window_, Qt::Key_Escape);
    auto *drawer = window_->findChild<QObject *>("fileDrawer");
    QVERIFY(drawer);
    QTRY_VERIFY(!drawer->property("visible").toBool());
    window_->resize(1800, 1000);
    desktop_->navigate(small_);
    QTRY_COMPARE(desktop_->directory(), small_);
    QTRY_VERIFY(idle());
    evidence_["folder_navigation"] = "Name/arrow separate clicks; sibling, ancestor, history, clipboard, inline IME path, long-path overflow and narrow keyboard";
  }
  void driveBreadcrumbSelection() {
    QTest::failOnWarning(QRegularExpression(".*DelegateModel::cancel.*"));
    desktop_->navigate(small_); desktop_->filter("",0,0);
    QTRY_COMPARE(desktop_->directory(),small_); QTRY_VERIFY(idle());
    const auto currentRoot = desktop_->breadcrumbs(small_).front().toMap()["path"].toString();
    const auto drives = desktop_->drives();
    QStringList actual, system;
    for (const auto &drive : drives) actual << drive.toMap()["path"].toString();
    for (const auto &drive : QDir::drives()) system << QDir::fromNativeSeparators(drive.absoluteFilePath());
    QCOMPARE(actual,system);
    QVERIFY(actual.contains(currentRoot));
    const auto findPath = [&](const char *name,const QString &path) -> QQuickItem * {
      for(auto *entry:visualItems(window_->contentItem(),name))
        if(entry->property("folderPath").toString()==path) return entry;
      return nullptr;
    };
    const auto click = [&](QQuickItem *target) {
      if(target) QTest::mouseClick(window_,Qt::LeftButton,Qt::NoModifier,target->mapToScene(QPointF(target->width()/2,target->height()/2)).toPoint());
    };
    auto *name=findPath("breadcrumbName",currentRoot);
    QTRY_VERIFY((name=findPath("breadcrumbName",currentRoot)));
    click(name);
    auto *popup=window_->findChild<QObject *>("folderDrivesPopup"); QVERIFY(popup);
    QTRY_VERIFY(popup->property("opened").toBool());
    QCOMPARE(desktop_->directory(),small_);
    QStringList choices;
    for(auto *entry:visualItems(window_->contentItem(),"folderDriveOption"))
      if(entry->isVisible()) choices << entry->property("folderPath").toString();
    QCOMPARE(choices,actual);
    save("drive-breadcrumb-menu");
    QTest::keyClick(window_,Qt::Key_Escape);
    QTRY_VERIFY(!popup->property("visible").toBool());
    auto *arrow=findPath("breadcrumbChildren",currentRoot); QVERIFY(arrow); click(arrow);
    QTRY_VERIFY(popup->property("opened").toBool());
    QVERIFY(desktop_->folderMenu().isEmpty());
    QTest::keyClick(window_,Qt::Key_Escape);
    QTRY_VERIFY(!popup->property("visible").toBool());
    QTRY_VERIFY(arrow->hasActiveFocus());
    for(int repeat=0;repeat<30;++repeat) {
      click(findPath("breadcrumbName",currentRoot));
      QTRY_VERIFY(popup->property("opened").toBool());
      QTest::keyClick(window_,Qt::Key_Escape);
      QTRY_VERIFY(!popup->property("visible").toBool());
    }
    QString missing;
    for(char letter='A'; letter<='Z'; ++letter) {
      const auto candidate=QString(QChar(letter))+":/";
      if(!actual.contains(candidate)) { missing=candidate; break; }
    }
    if(!missing.isEmpty()) {
      desktop_->navigate(missing);
      QTRY_VERIFY(!desktop_->error().isEmpty()); QTRY_VERIFY(idle());
      QCOMPARE(desktop_->directory(),small_);
      desktop_->dismissError();
    }
    QStringList alternatives=actual;
    alternatives.removeAll(currentRoot);
    if(alternatives.isEmpty()) QSKIP("No second system drive is present; menu and unavailable-drive checks completed");
    // Prefer the already accessible temporary-files volume if it differs,
    // without assuming a particular Windows drive letter. Readiness remains
    // an asynchronous navigation result, not a blocking volume probe.
    const auto tempCrumbs=desktop_->breadcrumbs(QDir::tempPath());
    if(!tempCrumbs.isEmpty()) {
      const auto tempRoot=tempCrumbs.front().toMap()["path"].toString();
      if(alternatives.removeAll(tempRoot)) alternatives.prepend(tempRoot);
    }
    QString chosen;
    for(const auto &candidate:alternatives) {
      click(findPath("breadcrumbName",currentRoot));
      QTRY_VERIFY(popup->property("opened").toBool());
      auto *option=findPath("folderDriveOption",candidate);
      if(!option) { QTest::keyClick(window_,Qt::Key_Escape); continue; }
      QTest::qWait(50); // Menu row layout must finish before hit testing.
      click(option);
      QTRY_VERIFY(!popup->property("visible").toBool());
      QTRY_VERIFY(idle());
      if(!desktop_->error().isEmpty()) {
        QCOMPARE(desktop_->directory(),small_);
        desktop_->dismissError();
        continue;
      }
      QCOMPARE(desktop_->directory(),candidate);
      chosen=candidate;
      break;
    }
    if(chosen.isEmpty()) QSKIP("No second enumerated drive is accessible; failure paths kept the original directory");
    evidence_["drive_menu_selected_actual_root"]=chosen;
    desktop_->history(-1); QTRY_COMPARE(desktop_->directory(),small_); QTRY_VERIFY(idle());
  }
  void imageAliasClassification() {
    const auto dir = root_ + "/image-aliases";
    QVERIFY(QDir().mkpath(dir));
    const QStringList extensions{"png", "jpg", "jpeg", "jpe",  "jfif", "bmp",
                                 "dib", "gif", "tif",  "tiff", "tga",  "WEBP",
                                 "ico", "dds", "hdr",  "rgbe", "exr"};
    for (const auto &extension : extensions) {
      QFile file(dir + "/asset." + extension);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("classification fixture only\n");
    }
    desktop_->navigate(dir);
    QTRY_COMPARE(desktop_->directory(), dir);
    QTRY_COMPARE(desktop_->count(), extensions.size());
    QTRY_VERIFY(idle());
    desktop_->filter("", 1, 0);
    QTRY_VERIFY(idle());
    QCOMPARE(desktop_->count(), extensions.size());
    for (int i = 0; i < desktop_->count(); ++i)
      QCOMPARE(desktop_->files()->index(i, 0).data(FileList::Kind).toString(),
               QString("image"));
    // Classification is not a decode capability claim.
    desktop_->filter("", 0, 0);
    QTRY_VERIFY(idle());
  }
  void staleCancellationAndFailure() {
    for (auto i = 0; i < 20; ++i) {
      desktop_->navigate(fixture_);
      desktop_->navigate(small_);
    }
    QTRY_COMPARE_WITH_TIMEOUT(desktop_->directory(), small_, 30000);
    QTRY_COMPARE(desktop_->count(), 1);
    QTRY_VERIFY(idle());
    desktop_->openPath(small_ + "/唯一图片.png");
    QTRY_COMPARE(desktop_->asset().value("name").toString(),
                 QString("唯一图片.png"));
    const auto session = desktop_->sessionRevision();
    desktop_->openPath(small_ + "/不存在.fbx");
    QTRY_VERIFY(!desktop_->error().isEmpty());
    QCOMPARE(desktop_->sessionRevision(), session);
    QCOMPARE(desktop_->asset().value("name").toString(),
             QString("唯一图片.png"));
    save("02-missing-file");
    desktop_->dismissError();
    desktop_->navigate(fixture_);
    const auto tasks = desktop_->tasks();
    QVERIFY(!tasks.isEmpty());
    desktop_->cancel(tasks.front().toMap()["id"].toString());
    QTRY_VERIFY(idle());
    const auto completed = desktop_->tasks();
    QVERIFY(std::any_of(completed.begin(), completed.end(),
                        [](const QVariant &row) {
                          return row.toMap()["state"] == "cancelled";
                        }));
  }
  void uiImeStateAndLanguages() {
    desktop_->navigate(fixture_);
    QTRY_COMPARE(desktop_->count(), 10024);
    QTRY_VERIFY(idle());
    auto *search = item("search");
    QVERIFY(search);
    search->forceActiveFocus();
    QInputMethodEvent preedit(QString::fromUtf8("图片"), {});
    QCoreApplication::sendEvent(search, &preedit);
    QTest::qWait(200);
    QCOMPARE(desktop_->query(), QString{});
    QTest::keyClick(window_, Qt::Key_Left, Qt::AltModifier);
    QTest::qWait(50);
    QCOMPARE(desktop_->directory(), fixture_);
    QInputMethodEvent commit;
    commit.setCommitString(QString::fromUtf8("图片"));
    QCoreApplication::sendEvent(search, &commit);
    QTRY_COMPARE(desktop_->query(), QString::fromUtf8("图片"));
    QTRY_COMPARE(desktop_->count(), 24);
    desktop_->select(0);
    QTRY_VERIFY(!desktop_->asset().isEmpty());
    QTRY_VERIFY(idle());
    const auto session = desktop_->sessionRevision();
    auto *tasks = item("tasksButton");
    QVERIFY(tasks);
    QMetaObject::invokeMethod(tasks, "clicked");
    QTest::qWait(100);
    save("03-tasks");
    QVERIFY(item("nav-assets"));
    QMetaObject::invokeMethod(item("nav-assets"), "clicked");
    QCOMPARE(desktop_->sessionRevision(), session);
    desktop_->preference("thumbnails", true);
    QTest::qWait(400);
    save("04-thumbnails");
    for (const auto &language :
         {QString("en"), QString("zh-TW"), QString("zh-CN")}) {
      desktop_->setLanguage(language);
      QTest::qWait(60);
      save("05-" + language);
      QCOMPARE(desktop_->sessionRevision(), session);
    }
    window_->resize(1280, 800);
    QTest::qWait(100);
    save("06-narrow");
    auto *rail = item("fileRail");
    QVERIFY(rail);
    QMetaObject::invokeMethod(rail, "clicked");
    auto *drawer = window_->findChild<QObject *>("fileDrawer");
    QVERIFY(drawer);
    QTRY_VERIFY(drawer->property("opened").toBool());
    QTest::qWait(60);
    save("06b-narrow-files");
    auto popupNumber = 0;
    for (auto *candidate : QGuiApplication::allWindows())
      if (auto *popup = qobject_cast<QQuickWindow *>(candidate);
          popup && popup != window_ && popup->isVisible())
        QVERIFY(captureWindow(popup).save(
            root_ + "/06b-popup-" + QString::number(popupNumber++) + ".png"));
    evidence_["drawer_popup_type"] = drawer->property("popupType").toInt();
    QTest::keyClick(window_, Qt::Key_Escape);
    QVERIFY(drawer->property("opened").toBool());
    QTest::keyClick(window_, Qt::Key_Escape);
    QTRY_VERIFY(!drawer->property("opened").toBool());
    QTRY_VERIFY(rail->hasActiveFocus());
    window_->resize(1600, 900);
    auto *workspace = item("workspace");
    QVERIFY(workspace);
    workspace->setProperty("focusMode", true);
    QTest::qWait(60);
    save("06c-focus");
    workspace->setProperty("focusMode", false);
    QCOMPARE(desktop_->sessionRevision(), session);
    desktop_->copy("中文路径与只读值");
    QCOMPARE(QGuiApplication::clipboard()->text(), QString("中文路径与只读值"));
    desktop_->preference("thumbnails", false);
    desktop_->filter("", 0, 0);
    QTRY_COMPARE(desktop_->count(), 10024);
    QTRY_VERIFY(idle());
  }
  void explainedLossAndErrorText() {
    auto *explain = engine_->singletonInstance<QObject *>("FormaView.TestUI", "Explain");
    QVERIFY(explain);
    const auto table = [&](const char *name) {
      return explain->property(name).value<QJSValue>().toVariant().toMap();
    };
    const auto call = [&](const char *method, const QString &a, const QString &b = {}) {
      QString out;
      if (std::strcmp(method, "lossText") == 0)
        QMetaObject::invokeMethod(explain, method, Q_RETURN_ARG(QString, out), Q_ARG(QString, a), Q_ARG(QString, b));
      else
        QMetaObject::invokeMethod(explain, method, Q_RETURN_ARG(QString, out), Q_ARG(QVariant, a));
      return out;
    };
    const auto hasCjk = [](const QString &s) {
      return std::any_of(s.begin(), s.end(), [](QChar c) { return c.unicode() >= 0x3400 && c.unicode() <= 0x9fff; });
    };
    const auto losses = table("losses");
    QVERIFY(losses.size() >= 50);
    const auto codes = table("errorCodes");
    QCOMPARE(codes.size(), 12);
    for (const auto &[tableName, entries] : {std::pair{"losses", losses}, std::pair{"errorCodes", codes}})
      for (auto it = entries.begin(); it != entries.end(); ++it) {
        const auto row = it.value().toStringList();
        QVERIFY2(row.size() == 3, qPrintable(it.key()));
        QVERIFY2(hasCjk(row[0]) && hasCjk(row[2]), qPrintable(it.key()));
        QVERIFY2(!row[1].isEmpty() && !hasCjk(row[1]), qPrintable(it.key()));
        QVERIFY2(row[0] != row[1] && row[1] != row[2], qPrintable(it.key()));
        // Every explanation says something beyond the technical code itself.
        QVERIFY2(row[1].size() > 12, qPrintable(it.key()));
        Q_UNUSED(tableName);
      }
    for (const auto &language : {QString("zh-CN"), QString("en"), QString("zh-TW")}) {
      desktop_->setLanguage(language);
      QTest::qWait(30);
      QString channel = call("lossText", "omitted_channel", "Source channel omitted: B");
      QVERIFY2(channel.contains("B"), qPrintable(language + channel));
      QVERIFY2(!channel.contains("omitted_channel") && !channel.contains("Source channel omitted:"), qPrintable(channel));
      QCOMPARE(hasCjk(channel), language != "en");
      const auto changed = call("errorText", "input_changed: Input changed after preflight");
      QCOMPARE(hasCjk(changed), language != "en");
      QVERIFY2(!changed.contains("Input changed after preflight"), qPrintable(changed));
      QVERIFY2(call("errorText", "").isEmpty(), "empty stays empty");
      // Unknown messages still get a localized headline; the raw text stays technical.
      const auto unknown = call("errorText", "io: Weird upstream failure 42");
      QCOMPARE(hasCjk(unknown), language != "en");
      QVERIFY2(!unknown.contains("Weird"), qPrintable(unknown));
      const auto fallback = call("errorText", "Something nobody mapped");
      QCOMPARE(hasCjk(fallback), language != "en");
      const auto jpeg = call("lossText", "lossy_codec", "JPEG recompression");
      QCOMPARE(hasCjk(jpeg), language != "en");
      QVERIFY2(jpeg.contains("JPEG"), qPrintable(jpeg));
      // Two-letter service codes (io) must match the bundled headline, and
      // producer details containing replace() patterns must not corrupt text.
      const auto generic = call("errorText", "internal: definitely-unmapped-message-xyz");
      const auto pairing = call("errorText", "invalid_argument: PSK pairing failed: PSA parent records differ; paired evaluation uses the reference PSK hierarchy");
      QVERIFY2(pairing.contains(language == "en" ? "Use reference skeleton" : language == "zh-TW" ? "使用參考模型骨架" : "使用参考模型骨架"), qPrintable(pairing));
      for (const auto *message : {
             "TIFF WhiteIsZero grayscale requires an explicit photometric conversion; convert to BlackIsZero in an image editor before importing",
             "ICC colour model does not match output channels; choose metadata drop or a matching profile",
             "glTF inverse bind matrix cannot be retained by FBX cluster API",
             "Duplicate glTF animation target channel",
             "Invalid glTF sparse animation indices", "Sparse animation value read failed",
             "Animation times must be finite", "Animation key count mismatch", "Non-finite animation value",
             "Cannot open input", "Cannot hold parent directory", "Cannot inspect file handle",
             "Journal already has an owner", "Journal directory contains unrelated entries",
             "Corrupt journal record", "Invalid journal record contract",
             "Journal exceeds 100 preserved temporary records"}) {
        const auto raw = QString("internal: ") + message;
        const auto localized = call("errorText", raw);
        QVERIFY2(localized != generic && !localized.contains(message), qPrintable(raw + " => " + localized));
        QCOMPARE(hasCjk(localized), language != "en");
        QCOMPARE(call("errorTechnical", raw), raw);
      }
      const auto io = call("errorText", "io: Cannot read bounded scope configuration");
      QVERIFY2(io != generic && !io.contains("Cannot read"), qPrintable(io + " | " + generic));
      const auto ambiguous = call("errorText", "out_of_scope: Ambiguous path component rejected");
      const auto nameReason = language == "en" ? QString("trailing space/dot") : language == "zh-TW" ? QString("末尾空格/句點") : QString("末尾空格/句点");
      QVERIFY2(ambiguous.contains(nameReason), qPrintable(ambiguous));
      QVERIFY(ambiguous != call("errorText", "out_of_scope: Only absolute local drive paths are authorized"));
      const auto copied_label = language == "en" ? QStringLiteral("Metadata not copied: ")
                                  : language == "zh-TW" ? QStringLiteral("中繼資料未複製：")
                                                        : QStringLiteral("元数据未复制：");
      QCOMPARE(call("lossText", "metadata_field", "Not copied: key=$&{d}"), copied_label + "key=$&{d}");

      const auto note = call("noteText", "Body: base colour has no usable UV set; source image reference retained without inventing coordinates");
      QCOMPARE(hasCjk(note), language != "en");
      QVERIFY2(note.contains("Body"), qPrintable(note));
      const auto mapping = call("noteText", "Verified mapping: 12/14 bones");
      QCOMPARE(hasCjk(mapping), language != "en");
      QVERIFY2(mapping.contains("12/14"), qPrintable(mapping));
      const auto batch = call("noteText", "Verified mapping: 3/4 bones · 1 of 2 file(s) NOT applied: other.fbx");
      QVERIFY2(batch.contains("3/4") && batch.contains("other.fbx") && !batch.contains("NOT applied") == (language != "en"), qPrintable(batch));
      const auto axes = call("noteText", "FBX · source axes preserved · 0.01 m/unit");
      QCOMPARE(hasCjk(axes), language != "en");
      QVERIFY2(axes.contains("0.01"), qPrintable(axes));
    }
    desktop_->setLanguage("zh-CN");
  }
  void convertOnlyInputsAndFutureFormats() {
    const auto dir = root_ + "/convert-only-list";
    QVERIFY(QDir().mkpath(dir));
    for (const auto *name : {"a.pmx", "b.vrm", "c.abc", "e.fbx", "f.txt"}) {
      QFile file(dir + "/" + name);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("x");
    }
    QImage(8, 8, QImage::Format_RGBA8888).save(dir + "/d.png");
    desktop_->navigate(dir);
    QTRY_COMPARE(desktop_->count(), 4); // pmx, vrm, fbx, png; ABC is not a previewable type in this release
    QTRY_VERIFY(idle());
    desktop_->filter("", 2, 0);
    QTRY_COMPARE(desktop_->count(), 3); // models plus conversion-only inputs
    QTRY_VERIFY(idle());
    desktop_->filter("", 5, 0);
    QTRY_COMPARE(desktop_->count(), 6);
    QTRY_VERIFY(idle());
    auto *model = desktop_->files();
    int pmx = -1, abc = -1;
    for (int row = 0; row < model->rowCount(); ++row) {
      const auto name = model->index(row, 0).data(FileList::Name).toString();
      if (name == "a.pmx") pmx = row;
      if (name == "c.abc") abc = row;
    }
    QVERIFY(pmx >= 0 && abc >= 0);
    desktop_->select(pmx);
    QTRY_COMPARE(desktop_->asset().value("kind").toString(), QString("convert"));
    QCOMPARE(desktop_->selectedModels(), QStringList{dir + "/a.pmx"});
    desktop_->select(abc);
    QTRY_COMPARE(desktop_->asset().value("name").toString(), QString("c.abc"));
    QVERIFY(desktop_->selectedModels().isEmpty());
    desktop_->filter("", 0, 0);
    QTRY_VERIFY(idle());
  }
  void viewHistoryAndResourceRecovery() {
    desktop_->navigate(fixture_);
    QTRY_COMPARE(desktop_->count(), 10024);
    QTRY_VERIFY(idle());
    desktop_->filter("asset", 2, 0);
    QTRY_COMPARE(desktop_->count(), 10000);
    QTRY_VERIFY(idle());
    desktop_->select(5);
    QTRY_VERIFY(idle());
    desktop_->saveView(12);
    desktop_->navigate(small_);
    QTRY_COMPARE(desktop_->count(), 1);
    QTRY_VERIFY(idle());
    desktop_->history(-1);
    QTRY_COMPARE(desktop_->count(), 10000);
    QTRY_VERIFY(idle());
    QCOMPARE(desktop_->query(), QString("asset"));
    QCOMPARE(desktop_->typeFilter(), 2);
    QCOMPARE(desktop_->currentIndex(), 5);
    QCOMPARE(desktop_->scrollPosition(), 12.0);
    auto *list = item("fileList");
    QVERIFY(list);
    QTRY_VERIFY(std::abs(list->property("contentY").toDouble() - 720) < 2);
    auto *workspace = item("workspace");
    QVERIFY(workspace);
    const auto session = desktop_->sessionRevision();
    QMetaObject::invokeMethod(workspace, "toggleFiles");
    QTest::qWait(30);
    QMetaObject::invokeMethod(workspace, "toggleFiles");
    QTest::qWait(60);
    QCOMPARE(desktop_->sessionRevision(), session);
    QTRY_VERIFY(std::abs(list->property("contentY").toDouble() - 720) < 2);
    desktop_->preference("thumbnails", true);
    QTest::qWait(60);
    auto *grid = item("fileGrid");
    QVERIFY(grid);
    const auto columns = std::max(1, static_cast<int>(grid->width() / 128));
    QTRY_VERIFY(std::abs(grid->property("contentY").toDouble() -
                        12.0 / columns * 116) < 2);
    desktop_->preference("thumbnails", false);
    QTest::qWait(60);
    QVERIFY(
        !desktop_->reportMissingResource(session - 1, "stale", "missing.png"));
    QVERIFY(desktop_->reportMissingResource(session, "diffuse",
                                            fixture_ + "/missing.png"));
    desktop_->applyResource("diffuse");
    QCOMPARE(desktop_->resourceIssues().front().toMap()["state"].toString(),
             QString("missing"));
    desktop_->validateResource("diffuse", fixture_ + "/missing.png");
    QTRY_VERIFY(idle());
    QCOMPARE(desktop_->resourceIssues().front().toMap()["state"].toString(),
             QString("missing"));
    desktop_->validateResource("diffuse", small_ + "/唯一图片.png");
    QTRY_VERIFY(idle());
    QCOMPARE(desktop_->resourceIssues().front().toMap()["state"].toString(),
             QString("checked"));
    save("07-resource-candidate");
    desktop_->applyResource("diffuse");
    QCOMPARE(desktop_->resourceIssues().front().toMap()["state"].toString(),
             QString("mapped"));
    QCOMPARE(desktop_->sessionRevision(), session);
    desktop_->clearAsset();
    QVERIFY(desktop_->resourceIssues().isEmpty());
    desktop_->filter("", 0, 0);
    QTRY_COMPARE(desktop_->count(), 10024);
    QTRY_VERIFY(idle());
  }
  void keyboardMouseDropAndReadOnly() {
    desktop_->navigate(fixture_);
    QTRY_COMPARE(desktop_->count(), 10024);
    QTRY_VERIFY(idle());
    auto *list = item("fileList");
    QVERIFY(list);
    list->setProperty("contentY", 0);
    QTest::qWait(60);
    desktop_->select(0);
    QTRY_VERIFY(idle());
    const auto session = desktop_->sessionRevision();
    QQuickItem *target = nullptr;
    for (auto *child : visualItems(window_->contentItem(), "fileDelegate"))
      if (child->isVisible() && child->property("index").toInt() == 2) {
        target = child;
        break;
      }
    QVERIFY(target);
    QTest::mouseClick(window_, Qt::LeftButton, Qt::ControlModifier,
                      target->mapToScene(QPointF(30, 15)).toPoint());
    QCOMPARE(desktop_->selectedCount(), 2);
    QCOMPARE(desktop_->sessionRevision(), session);
    QQuickItem *value = nullptr;
    for (auto *child : visualItems(window_->contentItem(), "readoutValue"))
      if (child->isVisible() &&
          child->property("text") == desktop_->asset()["path"]) {
        value = child;
        break;
      }
    QVERIFY(value);
    const auto text = value->property("text").toString();
    value->forceActiveFocus();
    QTest::keyClick(window_, Qt::Key_9);
    QCOMPARE(value->property("text").toString(), text);
    QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(window_, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(QGuiApplication::clipboard()->text(), text);
    auto *filterButton = item("filterButton");
    QVERIFY(filterButton);
    QMetaObject::invokeMethod(filterButton, "clicked");
    QTest::qWait(40);
    QTest::keyClick(window_, Qt::Key_Escape);
    QTRY_VERIFY(filterButton->hasActiveFocus());
    QTest::keyClick(window_, Qt::Key_L, Qt::ControlModifier);
    auto *address = item("address");
    QTRY_VERIFY((address = item("address")));
    QTRY_VERIFY(address->hasActiveFocus());
    QTest::keyClick(window_, Qt::Key_Escape);
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(small_ + "/唯一图片.png")});
    QDragEnterEvent enter(QPointF(600, 400), Qt::CopyAction, &mime,
                          Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window_, &enter);
    QVERIFY(enter.isAccepted());
    QDropEvent drop(QPointF(600, 400), Qt::CopyAction, &mime, Qt::LeftButton,
                    Qt::NoModifier);
    QCoreApplication::sendEvent(window_, &drop);
    QVERIFY(drop.isAccepted());
    QTRY_COMPARE(desktop_->directory(), small_);
    QTRY_COMPARE(desktop_->count(), 1);
    QTRY_VERIFY(idle());
    // The location field is only populated while editing (folder navigation
    // plan 2 shows breadcrumbs otherwise): opening it must show the new folder.
    QTest::keyClick(window_, Qt::Key_L, Qt::ControlModifier);
    QTRY_VERIFY((address = item("address")) && address->isVisible() && address->hasActiveFocus());
    QCOMPARE(address->property("text").toString(), small_);
    QTest::keyClick(window_, Qt::Key_Escape);
    QTRY_VERIFY(!item("address") || !item("address")->isVisible());
    desktop_->navigate(fixture_);
    desktop_->filter("", 0, 0);
    QTRY_COMPARE(desktop_->count(), 10024);
    QTRY_VERIFY(idle());
  }
  void gridDirectionalNavigation() {
    desktop_->navigate(fixture_);
    desktop_->filter("", 0, 0);
    QTRY_COMPARE(desktop_->count(), 10024);
    QTRY_VERIFY(idle());
    desktop_->preference("thumbnails", true);
    QTest::qWait(60);
    auto *grid = item("fileGrid");
    QVERIFY(grid);
    desktop_->select(0);
    QTRY_VERIFY(idle());
    grid->forceActiveFocus();
    const auto session = desktop_->sessionRevision();
    QTest::keyClick(window_, Qt::Key_Right, Qt::ControlModifier);
    QCOMPARE(desktop_->currentIndex(), 1);
    QCOMPARE(desktop_->selectedCount(), 2);
    QCOMPARE(desktop_->sessionRevision(), session);
    QTest::keyClick(window_, Qt::Key_Right, Qt::ShiftModifier);
    QCOMPARE(desktop_->currentIndex(), 2);
    QCOMPARE(desktop_->selectedCount(), 2);
    QTest::keyClick(window_, Qt::Key_Left,
                    Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(desktop_->currentIndex(), 1);
    QCOMPARE(desktop_->selectedCount(), 2);
    const auto columns = std::max(1, static_cast<int>(grid->width() / 128));
    QTest::keyClick(window_, Qt::Key_Down);
    QCOMPARE(desktop_->currentIndex(), 1 + columns);
    QTest::keyClick(window_, Qt::Key_Up);
    QCOMPARE(desktop_->currentIndex(), 1);
    QTest::keyClick(window_, Qt::Key_Left);
    QCOMPARE(desktop_->currentIndex(), 0);
    QTRY_VERIFY(idle());
    const auto firstSession = desktop_->sessionRevision();
    QTest::keyClick(window_, Qt::Key_Left);
    QTest::keyClick(window_, Qt::Key_Up);
    QTRY_VERIFY(idle());
    QCOMPARE(desktop_->currentIndex(), 0);
    QCOMPARE(desktop_->sessionRevision(), firstSession);
    QTest::keyClick(window_, Qt::Key_End);
    QTRY_VERIFY(idle());
    const auto lastSession = desktop_->sessionRevision();
    QTest::keyClick(window_, Qt::Key_Right);
    QTest::keyClick(window_, Qt::Key_Down);
    QTRY_VERIFY(idle());
    QCOMPARE(desktop_->currentIndex(), desktop_->count() - 1);
    QCOMPARE(desktop_->sessionRevision(), lastSession);
    desktop_->preference("thumbnails", false);
    QTest::qWait(60);
    desktop_->select(0);
    QTRY_VERIFY(idle());
    auto *list = item("fileList");
    QVERIFY(list);
    list->forceActiveFocus();
    QTest::keyClick(window_, Qt::Key_Right);
    QCOMPARE(desktop_->currentIndex(), 0);
    QTest::keyClick(window_, Qt::Key_Down);
    QCOMPARE(desktop_->currentIndex(), 1);
    QTRY_VERIFY(idle());
  }
  void windowCaptureLifecycle() {
    QQuickWindow probe;
    const auto retire=qScopeGuard([&]{probe.hide();probe.releaseResources();});
    probe.resize(320,240);
    QSignalSpy swaps(&probe,&QQuickWindow::frameSwapped);
    probe.show();
    QVERIFY(QTest::qWaitForWindowExposed(&probe));
    QTRY_VERIFY(swaps.count()>0);
    QVERIFY(!captureExposedWindow(&probe).isNull());
    probe.hide();
    QVERIFY(captureExposedWindow(&probe).isNull());
    probe.show();
    QVERIFY(QTest::qWaitForWindowExposed(&probe));
    probe.showMinimized();
    QTRY_COMPARE(probe.visibility(),QWindow::Minimized);
    QVERIFY(captureExposedWindow(&probe).isNull());
    probe.showNormal();
    QVERIFY(QTest::qWaitForWindowExposed(&probe));
    QVERIFY(probe.close());
    QVERIFY(!probe.handle());
    QVERIFY(captureExposedWindow(&probe).isNull());
    QVERIFY(!probe.handle()); // Guard must not recreate the native window.
    probe.create();
    QVERIFY(captureExposedWindow(&probe).isNull());
    const auto before=swaps.count();
    probe.show();
    QVERIFY(QTest::qWaitForWindowExposed(&probe));
    QTRY_VERIFY(swaps.count()>before);
    QVERIFY(!captureExposedWindow(&probe).isNull());
  }
  void nativeCaptionHitPurity() {
#ifdef Q_OS_WIN
    window_->showNormal();
    window_->resize(1600,900);
    QTest::qWait(100);
    auto *chrome=window_->findChild<QObject *>("nativeWindowChrome");
    auto *filter=dynamic_cast<QAbstractNativeEventFilter *>(chrome);
    QVERIFY(filter);
    auto *bar=item("topBar");QVERIFY(bar);
    const auto pureClient=[&](const QPointF &scene) {
      const auto hwnd=reinterpret_cast<HWND>(window_->winId());
      POINT point{qRound(scene.x()*window_->devicePixelRatio()),qRound(scene.y()*window_->devicePixelRatio())};
      if(!ClientToScreen(hwnd,&point))return false;
      MSG message{};message.hwnd=hwnd;message.message=WM_NCHITTEST;
      message.lParam=MAKELPARAM(point.x,point.y);
      qintptr result=-999;
      // A client control must be handled here, not delegated to Qt's
      // expanded-caption hit test (which samples physical button state and
      // synchronously dispatches mouse events/window actions).
      return filter->nativeEventFilter("windows_generic_MSG",&message,&result)&&result==HTCLIENT;
    };
    int controls=0;
    for(auto *control:bar->childItems())
      if(control->isVisible()&&control->property("pressed").isValid()) {
        ++controls;
        QVERIFY2(pureClient(control->mapToScene(QPointF(control->width()/2,control->height()/2))),
          "QML title-bar controls must return handled HTCLIENT without platform side effects");
      }
    QVERIFY(controls>0);
    auto *menu=item("applicationMenuButton");QVERIFY(menu);
    const auto dismiss=qScopeGuard([&]{
      // This test inspects hit-test purity, not keyboard dismissal. The
      // preceding capture probe may own activation, so Escape is unreliable.
      for (auto *child : menu->children())
        if (child->property("opened").isValid())
          QMetaObject::invokeMethod(child, "close");
    });
    QVERIFY(QMetaObject::invokeMethod(menu,"clicked"));
    QTRY_VERIFY(pureClient(bar->mapToScene(QPointF(400,bar->height()/2))));
#else
    QSKIP("Windows native hit purity");
#endif
  }
  void nativeWindowCaptionHitTesting() {
#ifdef Q_OS_WIN
    window_->showNormal();
    window_->resize(1600, 900);
    QTest::qWait(60);
    // Escape is delivered to a focused popup, not an inactive window.
    // Exposure and popup visibility do not establish keyboard focus.
    window_->requestActivate();
    QTRY_VERIFY(window_->isActive());
    auto *bar = item("topBar");
    auto *menu = item("applicationMenuButton");
    QVERIFY(bar);
    QVERIFY(menu);
    QObject *popup=nullptr;
    for(auto *child:menu->children())
      if(child->property("opened").isValid()) { popup=child;break; }
    QVERIFY(popup);
    QJsonArray focusTrace;
    const auto recordFocus=[&](const char *stage) {
      auto *focus=window_->activeFocusItem();
      focusTrace.append(QJsonObject{{"stage",stage},{"windowActive",window_->isActive()},
        {"focusClass",focus?focus->metaObject()->className():"null"},
        {"focusName",focus?focus->objectName():QString()},
        {"popupVisible",popup->property("visible").toBool()},
        {"popupOpened",popup->property("opened").toBool()},
        {"popupActiveFocus",popup->property("activeFocus").toBool()}});
    };
    bool captionPassed=false;
    const auto focusEvidence=qScopeGuard([&] {
      recordFocus("final");
      evidence_["caption_popup_focus"]=focusTrace;
      if(!captionPassed)
        qWarning().noquote()<<"CAPTION_POPUP_FAILURE_EVIDENCE"<<QJsonDocument(focusTrace).toJson(QJsonDocument::Compact);
    });
    const auto hit = [this](const QPointF &scene) {
      const auto hwnd = reinterpret_cast<HWND>(window_->winId());
      POINT point{qRound(scene.x() * window_->devicePixelRatio()),
                  qRound(scene.y() * window_->devicePixelRatio())};
      if (!ClientToScreen(hwnd, &point))
        return LRESULT{-1};
      return SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
    };
    const auto caption = [&] { return bar->mapToScene(QPointF(400, bar->height() / 2)); };
    QCOMPARE(hit(caption()), LRESULT{HTCAPTION});
    for (auto *control : bar->childItems()) {
      if (control->isVisible() && control->property("pressed").isValid())
        // A DPI/size change can still have a pending layout polish. Verify
        // the same client hit after that bounded settling period.
        QTRY_VERIFY2(hit(control->mapToScene(QPointF(control->width() / 2, control->height() / 2))) == LRESULT{HTCLIENT},
            qPrintable(QString("control=%1 x=%2 y=%3 width=%4 height=%5 dpr=%6")
                .arg(control->objectName()).arg(control->x()).arg(control->y())
                .arg(control->width()).arg(control->height()).arg(window_->devicePixelRatio())));
    }
    // A popup must receive the outside click rather than starting a window drag.
    QVERIFY(QMetaObject::invokeMethod(menu, "clicked"));
    QTRY_COMPARE(hit(caption()), LRESULT{HTCLIENT});
    recordFocus("opened-before-focus-check");
    QTRY_VERIFY(popup->property("opened").toBool());
    QTRY_VERIFY(popup->property("activeFocus").toBool());
    recordFocus("before-escape");
    QTest::keyClick(window_, Qt::Key_Escape);
    recordFocus("after-escape");
    QTRY_COMPARE(hit(caption()), LRESULT{HTCAPTION});
    window_->showMaximized();
    QTRY_COMPARE(window_->visibility(), QWindow::Maximized);
    QCOMPARE(hit(caption()), LRESULT{HTCAPTION});
    window_->showNormal();
    QTRY_COMPARE(window_->visibility(), QWindow::Windowed);
    QCOMPARE(hit(caption()), LRESULT{HTCAPTION});
    // Reinstalling after discovery of a preview window is harmless.
    installWindowChrome(window_);
    QCOMPARE(window_->findChildren<QObject *>("nativeWindowChrome").size(), 1);
    captionPassed=true;
#else
    QSKIP("Windows native caption handling");
#endif
  }
  void nativeCaptionResizeHitPurity() {
#ifdef Q_OS_WIN
    QQuickWindow probe;
    const auto retire=qScopeGuard([&]{probe.hide();probe.releaseResources();});
    probe.setFlags(window_->flags());probe.resize(1280,800);probe.show();
    installWindowChrome(&probe);
    QVERIFY(QTest::qWaitForWindowExposed(&probe));
    auto *filter=dynamic_cast<QAbstractNativeEventFilter *>(probe.findChild<QObject *>("nativeWindowChrome"));
    QVERIFY(filter);
    const auto hwnd=reinterpret_cast<HWND>(probe.winId());RECT r{};
    QVERIFY(GetWindowRect(hwnd,&r));
    const int middleX=(r.left+r.right)/2,middleY=(r.top+r.bottom)/2;
    const QList<QPair<QPoint,int>> cases={
      {{r.left+1,r.top+1},HTTOPLEFT},{{middleX,r.top+1},HTTOP},
      {{r.right-2,r.top+1},HTTOPRIGHT},{{r.left+1,middleY},HTLEFT},
      {{r.right-2,middleY},HTRIGHT},{{r.left+1,r.bottom-2},HTBOTTOMLEFT},
      {{middleX,r.bottom-2},HTBOTTOM},{{r.right-2,r.bottom-2},HTBOTTOMRIGHT}};
    for(const auto &entry:cases){
      MSG message{};message.hwnd=hwnd;message.message=WM_NCHITTEST;
      message.lParam=MAKELPARAM(entry.first.x(),entry.first.y());qintptr hit=0;
      QVERIFY2(filter->nativeEventFilter("windows_generic_MSG",&message,&hit),
        "Resize hit queries must not reach Qt's stateful expanded-client fallback");
      QCOMPARE(hit,qintptr(entry.second));
      QCOMPARE(SendMessageW(hwnd,WM_NCHITTEST,0,message.lParam),LRESULT(entry.second));
    }
    RECT client{};QVERIFY(GetClientRect(hwnd,&client));POINT origin{};
    QVERIFY(ClientToScreen(hwnd,&origin));const UINT dpi=GetDpiForWindow(hwnd);
    const int height=GetSystemMetricsForDpi(SM_CXSIZEFRAME,dpi)+
        GetSystemMetricsForDpi(SM_CXPADDEDBORDER,dpi)+GetSystemMetricsForDpi(SM_CYCAPTION,dpi);
    const int width=int(height*1.5);
    for(int i=1;i<=3;++i){
      MSG message{};message.hwnd=hwnd;message.message=WM_NCHITTEST;
      message.lParam=MAKELPARAM(origin.x+client.right-width*i,origin.y+height/2);qintptr hit=0;
      QVERIFY2(filter->nativeEventFilter("windows_generic_MSG",&message,&hit),
        "Adjacent caption rectangles must not leave a stateful one-pixel gap");
      QVERIFY(hit==HTCLOSE||hit==HTMAXBUTTON||hit==HTMINBUTTON);
    }
    // Cover the entire caption/client seam, including pixels beyond the
    // button rectangle. A point without a QML title region is ordinary client.
    for(int y=height-2;y<=height+12;y+=2)
      for(int x=client.right-width*3;x<client.right-width/2;x+=3){
        MSG message{};message.hwnd=hwnd;message.message=WM_NCHITTEST;
        message.lParam=MAKELPARAM(origin.x+x,origin.y+y);qintptr hit=0;
        QVERIFY2(filter->nativeEventFilter("windows_generic_MSG",&message,&hit),
          "The complete caption/client seam must bypass Qt's stateful fallback");
        QVERIFY(hit==HTCLIENT||hit==HTCLOSE||hit==HTMAXBUTTON||hit==HTMINBUTTON);
      }
    probe.showMaximized();QTRY_VERIFY(IsZoomed(hwnd));
    QVERIFY(GetWindowRect(hwnd,&r));origin={0,0};QVERIFY(ClientToScreen(hwnd,&origin));
    if(r.top<origin.y){
      MSG message{};message.hwnd=hwnd;message.message=WM_NCHITTEST;
      message.lParam=MAKELPARAM((r.left+r.right)/2,r.top);qintptr hit=0;
      QVERIFY(filter->nativeEventFilter("windows_generic_MSG",&message,&hit));
      QCOMPARE(hit,qintptr(HTNOWHERE));
    }
#else
    QSKIP("Windows resize hit purity");
#endif
  }
  void nativeCaptionButtonsRelease() {
#ifdef Q_OS_WIN
    QQuickWindow probe;
    const auto retireProbe=qScopeGuard([&] {
      // Also runs on every QTRY/QVERIFY early return. Stop render/exposure
      // work before tearing down the temporary native-window probe.
      probe.hide();
      probe.releaseResources();
      window_->requestActivate();
    });
    probe.setFlags(window_->flags());
    probe.resize(1280, 800);
    probe.show();
    probe.requestActivate();
    installWindowChrome(&probe);
    struct CloseGuard : QObject {
      int closes{};
      bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() != QEvent::Close) return false;
        ++closes;
        event->ignore();
        return true;
      }
    } guard;
    probe.installEventFilter(&guard);
    QTest::qWait(100);
    const auto hwnd = reinterpret_cast<HWND>(probe.winId());
    auto *nativeChrome=dynamic_cast<QAbstractNativeEventFilter *>(probe.findChild<QObject *>("nativeWindowChrome"));
    QVERIFY(nativeChrome);
    const auto ownedHit=[&](QPoint point){
      MSG message{};message.hwnd=hwnd;message.message=WM_NCHITTEST;
      message.lParam=MAKELPARAM(point.x(),point.y());qintptr hit=0;
      // A blank probe has no QML drag region. Candidate scans must not enter
      // Qt's default hit test, which samples physical buttons and dispatches
      // synchronous mouse/window actions. Verify the chosen center natively.
      return nativeChrome->nativeEventFilter("windows_generic_MSG",&message,&hit)?int(hit):0;
    };
    CaptionEvidence evidence(hwnd);
    const auto publishFailure=qScopeGuard([&]{
      if(QTest::currentTestFailed())
        qInfo().noquote()<<"CAPTION_FAILURE_EVIDENCE"<<evidence.json();
    });
    const auto buttonPoint = [&](int wanted) {
      RECT rect{};
      MONITORINFO monitor{}; monitor.cbSize=sizeof(monitor);
      if (!GetWindowRect(hwnd,&rect) ||
          !GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor))
        return QPoint(-1,-1);
      // Maximized HWND rectangles include invisible frame pixels outside the
      // monitor. Choose the center of the visible native hit region, not its
      // first border pixel; keep fractional display scaling intact.
      const qreal scale=probe.devicePixelRatio();
      const int left=std::max(int(monitor.rcMonitor.left),int(rect.right)-qCeil(180*scale));
      const int right=std::min(int(monitor.rcMonitor.right),int(rect.right));
      const int top=std::max(int(monitor.rcMonitor.top),int(rect.top));
      const int bottom=std::min(int(monitor.rcMonitor.bottom),int(rect.top)+qCeil(48*scale));
      QRect hits;
      for (int y=top; y<bottom; y+=3)
        for (int x=left; x<right; x+=3)
          if (ownedHit(QPoint(x,y))==wanted)
            hits=hits.united(QRect(x,y,1,1));
      const auto center=hits.center();
      if (hits.isEmpty() || SendMessageW(hwnd,WM_NCHITTEST,0,
          MAKELPARAM(center.x(),center.y()))!=wanted)
        return QPoint(-1,-1);
      return center;
    };
    const auto down = [&](int hit, QPoint point) {
      evidence.checkpoint("before-down",point,ownedHit(point));
      // Native input passes the dispatcher filter as a queued message.
      // SendMessage bypasses that filter and enters Windows' modal tracking.
      PostMessageW(hwnd, WM_NCLBUTTONDOWN, hit, MAKELPARAM(point.x(),point.y()));
      QTest::qWait(10);
    };
    const auto up = [&](QPoint screen) {
      evidence.checkpoint("before-up",screen,ownedHit(screen));
      POINT client{screen.x(),screen.y()};
      ScreenToClient(hwnd,&client);
      PostMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(client.x,client.y));
      QTest::qWait(10);
    };
    for (const int hit : {HTCLOSE, HTMAXBUTTON, HTMINBUTTON}) {
      const auto point = buttonPoint(hit);
      QVERIFY2(point != QPoint(-1,-1), qPrintable(QString("No native caption hit %1").arg(hit)));
      down(hit, point);
      QTest::qWait(50);
      QCOMPARE(guard.closes, 0);
      QVERIFY(!IsZoomed(hwnd) && !IsIconic(hwnd));
      QCOMPARE(GetCapture(), hwnd);
      // Dragging out and releasing cancels all three buttons.
      RECT rect{};
      GetWindowRect(hwnd,&rect);
      up(QPoint(rect.left+300, rect.top+200));
      QTest::qWait(30);
      QCOMPARE(guard.closes, 0);
      QVERIFY(!IsZoomed(hwnd) && !IsIconic(hwnd));
      // Losing capture/cancelling must not leave an armed button.
      down(hit, point);
      SendMessageW(hwnd, WM_CANCELMODE, 0, 0);
      up(point);
      QTest::qWait(30);
      QCOMPARE(guard.closes, 0);
      QVERIFY(!IsZoomed(hwnd) && !IsIconic(hwnd));
    }
    auto point = buttonPoint(HTMAXBUTTON);
    QVERIFY(point != QPoint(-1,-1));
    down(HTMAXBUTTON,point); up(point);
    QTRY_VERIFY(IsZoomed(hwnd));
    point = buttonPoint(HTMAXBUTTON);
    QVERIFY(point != QPoint(-1,-1));
    MONITORINFO monitor{};monitor.cbSize=sizeof(monitor);
    QVERIFY(GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor));
    const POINT physical{point.x(),point.y()};
    QVERIFY2(PtInRect(&monitor.rcMonitor,physical),
      "Caption input must target a physically reachable point, not the invisible maximized frame");
    down(HTMAXBUTTON,point);
    QTest::qWait(30);
    QVERIFY(IsZoomed(hwnd));
    up(point);
    QTRY_VERIFY(!IsZoomed(hwnd));
    point = buttonPoint(HTMINBUTTON);
    QVERIFY(point != QPoint(-1,-1));
    down(HTMINBUTTON,point); up(point);
    QTRY_VERIFY(IsIconic(hwnd));
    probe.showNormal();
    QTRY_VERIFY(!IsIconic(hwnd));
    point = buttonPoint(HTCLOSE);
    QVERIFY(point != QPoint(-1,-1));
    down(HTCLOSE,point);
    QTest::qWait(30);
    QCOMPARE(guard.closes,0);
    up(point);
    QTRY_COMPARE(guard.closes,1);
    probe.removeEventFilter(&guard);
#else
    QSKIP("Windows caption buttons");
#endif
  }
  void paneFocusRecovery() {
    window_->resize(1600, 900);
    desktop_->preference("thumbnails", false);
    desktop_->navigate(fixture_);
    QTRY_COMPARE(desktop_->count(), 10024);
    QTRY_VERIFY(idle());
    auto *workspace = item("workspace");
    QVERIFY(workspace);
    workspace->setProperty("focusMode", false);
    workspace->setProperty("tasksVisible", false);
    workspace->setProperty("filesVisible", true);
    QTest::qWait(60);
    const auto collapseButton = [&]() -> QQuickItem * {
      for (auto *candidate : window_->findChildren<QQuickItem *>())
        if (candidate->isVisible() &&
            (candidate->objectName() == "collapseFiles" || candidate->objectName() == "inspectorRail"))
          return candidate;
      return nullptr;
    };
    auto *collapse = collapseButton();
    QVERIFY(collapse);
    collapse->forceActiveFocus();
    QMetaObject::invokeMethod(collapse, "clicked");
    auto *rail = item("fileRail");
    QVERIFY(rail);
    QTRY_VERIFY(rail->hasActiveFocus());
    QMetaObject::invokeMethod(rail, "clicked");
    QTest::qWait(100);
    save("files-address-restored");
    auto *address = item("address");
    QTRY_VERIFY((address = item("address")));
    QTRY_VERIFY(address->hasActiveFocus());
    address->setProperty("text", "Z:/unsubmitted-location");
    QTest::keyClick(window_, Qt::Key_Escape);
    collapse = collapseButton();
    QVERIFY(collapse);
    QTRY_VERIFY(window_->activeFocusItem() && window_->activeFocusItem()->objectName() == "breadcrumbName");
    QCOMPARE(desktop_->directory(), fixture_);
    QMetaObject::invokeMethod(collapse, "clicked");
    QTRY_VERIFY(rail->hasActiveFocus());
    QMetaObject::invokeMethod(rail, "clicked");
    QTRY_VERIFY(address->hasActiveFocus());
    QCOMPARE(address->property("text").toString(), fixture_);
    window_->resize(1280, 800);
    QTRY_VERIFY(rail->isVisible());
    auto *drawer = window_->findChild<QObject *>("fileDrawer");
    QVERIFY(drawer);
    QMetaObject::invokeMethod(rail, "clicked");
    QTRY_VERIFY(drawer->property("opened").toBool());
    address = item("address");
    QVERIFY(address);
    QTRY_VERIFY(address->hasActiveFocus());
    address->setProperty("text", "Z:/unsubmitted-location");
    QTest::keyClick(window_, Qt::Key_Escape);
    QVERIFY(drawer->property("opened").toBool());
    collapse = collapseButton();
    QVERIFY(collapse);
    QTRY_VERIFY(window_->activeFocusItem() && window_->activeFocusItem()->objectName() == "breadcrumbName");
    QCOMPARE(desktop_->directory(), fixture_);
    QTest::keyClick(window_, Qt::Key_Escape);
    QTRY_VERIFY(!drawer->property("opened").toBool());
    QTRY_VERIFY(!drawer->property("visible").toBool());
    QTRY_VERIFY(rail->hasActiveFocus());
    QMetaObject::invokeMethod(rail, "clicked");
    QTRY_VERIFY(drawer->property("opened").toBool());
    collapse = collapseButton();
    QVERIFY(collapse);
    collapse->forceActiveFocus();
    QMetaObject::invokeMethod(collapse, "clicked");
    QTRY_VERIFY(!drawer->property("opened").toBool());
    QTRY_VERIFY(!drawer->property("visible").toBool());
    QTRY_VERIFY(rail->hasActiveFocus());
    const auto session = desktop_->sessionRevision();
    for (auto width : {1600, 1280}) {
      window_->resize(width, 900);
      workspace->setProperty("focusMode", true);
      QTest::qWait(60);
      QVERIFY(window_->activeFocusItem() &&
              window_->activeFocusItem()->isVisible());
      QVERIFY(!item("navigationRail"));
      QVERIFY(!item("fileRail"));
      QTest::keyClick(window_, Qt::Key_L, Qt::ControlModifier);
      if (width == 1280)
        QTRY_VERIFY(drawer->property("opened").toBool());
      address = item("address");
      QTRY_VERIFY((address = item("address")));
      QTRY_VERIFY(address->hasActiveFocus());
      QCOMPARE(desktop_->sessionRevision(), session);
      if (width == 1280) {
        QTest::keyClick(window_, Qt::Key_Escape);
        QTest::keyClick(window_, Qt::Key_Escape);
        QTRY_VERIFY(!drawer->property("opened").toBool());
    QTRY_VERIFY(!drawer->property("visible").toBool());
      }
    }
    window_->resize(1600, 900);
    workspace->setProperty("focusMode", true);
    QTest::qWait(60);
    auto *exitFocus = item("globalFocusPreview");
    QVERIFY(exitFocus);
    exitFocus->forceActiveFocus();
    QTest::keyClick(window_, Qt::Key_Escape);
    QTRY_VERIFY(!workspace->property("focusMode").toBool());
    QTRY_VERIFY(window_->activeFocusItem() &&
                window_->activeFocusItem()->isVisible());
    QCOMPARE(desktop_->sessionRevision(), session);
  }
  void focusRestoresFileScroll_data() {
    QTest::addColumn<int>("entries");
    QTest::newRow("short-overflow") << 9;
    QTest::newRow("long-overflow") << 16;
  }
  void focusRestoresFileScroll() {
    QFETCH(int, entries);
    window_->resize(1600, 908);
    auto *workspace = item("workspace");
    QVERIFY(workspace);
    workspace->setProperty("focusMode", false);
    workspace->setProperty("tasksVisible", false);
    workspace->setProperty("conversionVisible", false);
    workspace->setProperty("filesVisible", true);
    desktop_->preference("thumbnails", false);
    const auto folder = root_ + QString("/focus-scroll-%1").arg(entries);
    QVERIFY(QDir().mkpath(folder));
    QImage image(8, 8, QImage::Format_RGBA8888);
    image.fill(Qt::cyan);
    for (int i = 0; i < entries; ++i)
      QVERIFY(image.save(folder + QString("/sample%1.png").arg(i)));
    desktop_->navigate(folder);
    QTRY_COMPARE(desktop_->count(), entries);
    QTRY_VERIFY(idle());
    auto *list = item("fileList");
    QVERIFY(list);
    QTRY_VERIFY(list->property("contentHeight").toDouble() > list->height() + 1);
    // Directory restoration completes on the first rendered frame. Decoder idle
    // alone does not mean the saved scroll offset has been applied.
    QTRY_VERIFY(!item("filesPane")->property("restorePending").toBool());
    for (int round = 0; round < 3; ++round) {
      QSignalSpy layoutFrame(window_, &QQuickWindow::frameSwapped);
      window_->update();
      QTRY_VERIFY(!layoutFrame.isEmpty());
      QVERIFY(QMetaObject::invokeMethod(list, "forceLayout"));
      QTRY_VERIFY(list->property("contentHeight").toDouble() > list->height() + 1);
      const auto position = list->property("originY").toDouble()
                          + list->property("contentHeight").toDouble() - list->height();
      list->setProperty("contentY", position);
      QTest::qWait(50);
      const auto before = list->property("contentY").toDouble();
      QVERIFY(before > list->property("originY").toDouble() + 1);
      QVERIFY(QMetaObject::invokeMethod(item("globalFocusPreview"), "clicked"));
      QTRY_VERIFY(workspace->property("focusMode").toBool());
      QTest::qWait(50);
      QVERIFY(QMetaObject::invokeMethod(item("globalFocusPreview"), "clicked"));
      QTRY_VERIFY(!workspace->property("focusMode").toBool());
      QTRY_VERIFY(!item("filesPane")->property("restorePending").toBool());
      QCOMPARE(list->property("contentY").toDouble(), before);
    }
  }
  void drawerHistoryShortcuts() {
    window_->resize(1280, 800);
    auto *workspace = item("workspace");
    QVERIFY(workspace);
    workspace->setProperty("focusMode", false);
    const auto parent = root_ + "/drawer-history";
    const auto child = parent + "/child";
    QVERIFY(QDir().mkpath(child));
    desktop_->navigate(parent);
    QTRY_COMPARE(desktop_->directory(), parent);
    QTRY_VERIFY(idle());
    desktop_->navigate(child);
    QTRY_COMPARE(desktop_->directory(), child);
    QTRY_VERIFY(idle());
    QVERIFY(QMetaObject::invokeMethod(workspace, "openAddress"));
    auto *drawer = window_->findChild<QObject *>("fileDrawer");
    QVERIFY(drawer);
    const auto cleanup = qScopeGuard([&] {
      QMetaObject::invokeMethod(drawer, "close");
      QTRY_VERIFY(!drawer->property("visible").toBool());
      window_->resize(1600, 900);
    });
    QTRY_VERIFY(drawer->property("opened").toBool());
    auto *address = item("address");
    QVERIFY(address);
    QTRY_VERIFY(address->hasActiveFocus());
    QTest::keyClick(window_, Qt::Key_Escape);
    QTRY_VERIFY(!address->hasActiveFocus());
    QVERIFY(drawer->property("opened").toBool());
    QTest::keyClick(window_, Qt::Key_Left, Qt::AltModifier);
    QTRY_COMPARE(desktop_->directory(), parent);
    QTRY_VERIFY(idle());
    QTest::keyClick(window_, Qt::Key_Right, Qt::AltModifier);
    QTRY_COMPARE(desktop_->directory(), child);
    QTRY_VERIFY(idle());
    QTest::keyClick(window_, Qt::Key_Up, Qt::AltModifier);
    QTRY_COMPARE(desktop_->directory(), parent);
  }
  void parentNavigationKeepsSession() {
    const auto parent = root_ + "/parent-navigation";
    const auto current = parent + "/current";
    QVERIFY(QDir().mkpath(current));
    QVERIFY(QDir().mkpath(parent + "/sibling"));
    QImage image(64, 64, QImage::Format_RGBA8888);
    image.fill(Qt::cyan);
    QVERIFY(image.save(current + "/sample.png"));
    desktop_->navigate(current);
    QTRY_COMPARE(desktop_->directory(), current);
    QTRY_COMPARE(desktop_->count(), 1);
    desktop_->openPath(current + "/sample.png");
    QTRY_VERIFY(idle());
    const auto session = desktop_->sessionRevision();
    desktop_->parentDirectory();
    QTRY_COMPARE(desktop_->directory(), parent);
    QTRY_COMPARE(desktop_->count(), 2);
    QTRY_VERIFY(idle());
    QCOMPARE(desktop_->sessionRevision(), session);
    desktop_->history(-1);
    QTRY_COMPARE(desktop_->directory(), current);
    QTRY_COMPARE(desktop_->count(), 1);
    QCOMPARE(desktop_->sessionRevision(), session);
    desktop_->navigate(fixture_);
    QTRY_COMPARE(desktop_->count(), 10024);
    QTRY_VERIFY(idle());
  }
#ifdef FORMAVIEW_IMAGE_UI
  void pixelCompressionDescription_data() {
    QTest::addColumn<QString>("format");
    QTest::addColumn<bool>("blockCompressed");
    QTest::newRow("tga-raw") << QString("tga") << false;
    QTest::newRow("tga-rle") << QString("tga-rle") << false;
    QTest::newRow("dds-raw") << QString("dds") << false;
    QTest::newRow("dds-bc1") << QString("dds") << true;
  }
  void pixelCompressionDescription() {
    QFETCH(QString, format);
    QFETCH(bool, blockCompressed);
    // Independent, constant-red fixtures: no production encoder involved.
    QByteArray bytes;
    if (format.startsWith("tga")) {
      bytes = QByteArray::fromHex("000002000000000000000000040004001820");
      if (format == "tga-rle") {
        bytes[2] = 10;
        for (int row = 0; row < 4; ++row)
          bytes += QByteArray::fromHex("830000ff");
      } else {
        for (int i = 0; i < 16; ++i) bytes += QByteArray::fromHex("0000ff");
      }
    } else {
      QDataStream header(&bytes, QIODevice::WriteOnly);
      header.setByteOrder(QDataStream::LittleEndian);
      header << quint32(0x20534444) << quint32(124)
             << quint32(blockCompressed ? 0x81007 : 0x100f)
             << quint32(4) << quint32(4) << quint32(blockCompressed ? 8 : 16)
             << quint32(0) << quint32(0);
      for (int i = 0; i < 11; ++i) header << quint32(0);
      header << quint32(32) << quint32(blockCompressed ? 4 : 0x41)
             << quint32(blockCompressed ? 0x31545844 : 0)
             << quint32(blockCompressed ? 0 : 32)
             << quint32(blockCompressed ? 0 : 0xff)
             << quint32(blockCompressed ? 0 : 0xff00)
             << quint32(blockCompressed ? 0 : 0xff0000)
             << quint32(blockCompressed ? 0 : 0xff000000)
             << quint32(0x1000) << quint32(0) << quint32(0) << quint32(0) << quint32(0);
      QCOMPARE(bytes.size(), 128);
      if (blockCompressed) bytes += QByteArray::fromHex("00f8000000000000");
      else for (int i = 0; i < 16; ++i) bytes += QByteArray::fromHex("ff0000ff");
    }
    const auto path = small_ + "/" + QTest::currentDataTag() +
                      (format.startsWith("tga") ? ".tga" : ".dds");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
    file.close();
    auto *session = qobject_cast<ImageSession *>(desktop_->image());
    QVERIFY(session);
    desktop_->openPath(path);
    QTRY_VERIFY_WITH_TIMEOUT((!session->busy() && session->path() == path) ||
                                !session->error().isEmpty(), 10000);
    QVERIFY2(session->error().isEmpty(), qPrintable(session->error()));
    QCOMPARE(session->path(), path);
    session->setting("channel", -1);
    session->setting("colour", false);
    session->setting("composite", false);
    session->sample(0, 0);
    QVERIFY(session->pixel()["valid"].toBool());
    const auto channels = session->pixel()["source"].toList();
    const auto display = session->pixel()["display"].toList();
    for (int component = 0; component < 3; ++component) {
      const auto name = QStringList{"R", "G", "B"}[component];
      const auto found = std::find_if(channels.begin(), channels.end(), [&](const auto &c) {
        return c.toMap()["name"].toString() == name;
      });
      QVERIFY(found != channels.end());
      QCOMPARE(found->toMap()["source"].toString().toDouble(),
               component == 0 ? (blockCompressed ? 1. : 255.) : 0.);
      QCOMPARE(found->toMap()["interpreted"].toString().toDouble(), component == 0 ? 1. : 0.);
      QCOMPARE(display[component].toString().toDouble(), component == 0 ? 1. : 0.);
    }
    auto *toggle = item("pixelToggle");
    QVERIFY(toggle);
    toggle->setProperty("checked", true);
    auto *description = item("pixelSourceDescription");
    QVERIFY(description);
    QTRY_VERIFY(description->isVisible());
    QTest::qWait(60);
    save(QString("pixel-compression-") + QTest::currentDataTag());
    evidence_[QString("pixel-compression-") + QTest::currentDataTag()] =
        QJsonObject{{"pixel", QJsonObject::fromVariantMap(session->pixel())},
                    {"label", description->property("text").toString()},
                    {"block", session->info()["block"].toString()}};
    QCOMPARE(description->property("text").toString().contains("BC"), blockCompressed);
    QCOMPARE(!session->info()["block"].toString().isEmpty(), blockCompressed);
    toggle->setProperty("checked", false);
  }
  void imageSwitchKeepsViewportGeometry() {
    auto *workspace = item("workspace");
    QVERIFY(workspace);
    workspace->setProperty("settingsVisible", false);
    workspace->setProperty("conversionVisible", false);
    workspace->setProperty("tasksVisible", false);
    workspace->setProperty("focusMode", false);
    window_->resize(1600, 900);
    auto *session = qobject_cast<ImageSession *>(desktop_->image());
    QVERIFY(session);
    const auto first = small_ + "/switch-first.bmp";
    const auto second = small_ + "/switch-second.bmp";
    QImage image(2048, 2048, QImage::Format_RGB888);
    image.fill(Qt::blue);
    QVERIFY(image.save(first));
    image.fill(Qt::green);
    QVERIFY(image.save(second));
    desktop_->openPath(first);
    QTRY_VERIFY_WITH_TIMEOUT(session->path() == first && !session->busy(), 10000);
    auto *view = qobject_cast<ImageView *>(item("imageViewport"));
    QVERIFY(view);
    QTest::qWait(80);
    const auto height = view->height();
    const auto zoom = view->zoom();
    QList<qreal> heights;
    const auto connection = connect(view, &QQuickItem::heightChanged, view,
                                    [&] { heights.append(view->height()); });
    const auto disconnectHeight = qScopeGuard([&] { disconnect(connection); });
    for (const auto &path : {second, first, second}) {
      session->open(path);
      QVERIFY(session->busy());
      session->sample(0, 0);
      QVERIFY(!session->pixel()["valid"].toBool());
      QTRY_VERIFY_WITH_TIMEOUT(!session->busy(), 10000);
      QCOMPARE(session->path(), path);
      QTest::qWait(40);
      QCOMPARE(view->height(), height);
      QCOMPARE(view->zoom(), zoom);
    }
    for (const auto measured : heights)
      QVERIFY2(qAbs(measured - height) < .5,
               qPrintable(QString("Transient viewport resize: %1 -> %2").arg(height).arg(measured)));
  }
  void pixelHoverKeepsReadoutAndGeometry() {
    auto *session = qobject_cast<ImageSession *>(desktop_->image());
    QVERIFY(session);
    desktop_->openPath(small_ + "/唯一图片.png");
    QTRY_VERIFY(session->available() && !session->busy());
    auto *view = qobject_cast<ImageView *>(item("imageViewport"));
    QVERIFY(view);
    item("pixelToggle")->setProperty("checked", true);
    QTest::qWait(100);
    auto *pane = view->parentItem()->parentItem();
    session->setSamplePinned(false);
    view->fit();
    const auto height = view->height();
    const auto zoom = view->zoom();
    auto move = [&](QPointF p) {
      QTest::mouseMove(window_, view->mapToScene(p).toPoint());
      QTest::qWait(30);
    };
    for (int repeat = 0; repeat < 12; ++repeat) {
      move(view->imageRect().center());
      QTRY_VERIFY(session->pixel()["valid"].toBool());
      const auto sample = session->pixel();
      move({1, 1});
      QCOMPARE(session->pixel(), sample);
      QVERIFY(qAbs(view->height() - height) < .5);
      QVERIFY(qAbs(view->zoom() - zoom) < .00001);
    }
    const auto sample = session->pixel();
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
                      view->mapToScene({1, 1}).toPoint());
    QVERIFY(!pane->property("fixedSample").toBool());
    QCOMPARE(session->pixel(), sample);
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
                      view->mapToScene(view->imageRect().center()).toPoint());
    QVERIFY(pane->property("fixedSample").toBool());
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
                      view->mapToScene({1, 1}).toPoint());
    QVERIFY(pane->property("fixedSample").toBool());
    QCOMPARE(session->pixel(), sample);
    view->forceActiveFocus();
    QTest::keyClick(window_, Qt::Key_Escape);
    QVERIFY(!pane->property("fixedSample").toBool());
    // Loading another image must still invalidate the previous image's sample.
    session->open(fixture_ + "/图片0.png");
    QVERIFY(!session->pixel()["valid"].toBool());
    QTRY_VERIFY(!session->busy());
    save("pixel-hover-stable");
  }
  void pinnedSampleSurvivesPaneRecreation() {
    auto *session = qobject_cast<ImageSession *>(desktop_->image());
    desktop_->openPath(small_ + "/唯一图片.png");
    QTRY_VERIFY(session->available() && !session->busy());
    auto *view = qobject_cast<ImageView *>(item("imageViewport"));
    QVERIFY(view);
    QTest::qWait(100);
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
                     view->mapToScene(view->imageRect().center()).toPoint());
    auto *pane = view->parentItem()->parentItem();
    QVERIFY(pane->property("fixedSample").toBool());
    const auto sample = session->pixel();
    auto *loader = pane->parentItem();
    QVERIFY(loader->metaObject()->indexOfProperty("active") >= 0);
    QPointer<QQuickItem> oldPane = pane;
    QVERIFY(loader->setProperty("active", false));
    QTRY_VERIFY(oldPane.isNull());
    QCOMPARE(session->pixel(), sample);
    QVERIFY(loader->setProperty("active", true));
    QTRY_VERIFY(item("imageViewport"));
    view = qobject_cast<ImageView *>(item("imageViewport"));
    pane = view->parentItem()->parentItem();
    QVERIFY2(pane->property("fixedSample").toBool(),
             "A replacement image pane must retain the session's pinned sample");
    QTest::mouseMove(window_, view->mapToScene(view->imageRect().topLeft() + QPointF(4, 4)).toPoint());
    QCOMPARE(session->pixel(), sample);
    save("pinned-pane-recreated");
  }
  void pinnedSampleRepickAndWindowStates() {
    auto *session = qobject_cast<ImageSession *>(desktop_->image());
    desktop_->openPath(small_ + "/唯一图片.png");
    QTRY_VERIFY(session->available() && !session->busy());
    auto *view = qobject_cast<ImageView *>(item("imageViewport"));
    QVERIFY(view);
    QTest::qWait(100);
    auto click = [&](QPointF point) {
      QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
                       view->mapToScene(point).toPoint());
    };
    click(view->imageRect().center());
    auto *pane = view->parentItem()->parentItem();
    QVERIFY(pane->property("fixedSample").toBool());
    const auto first = session->pixel();
    click(view->imageRect().center() + QPointF(20, 10));
    QVERIFY2(pane->property("fixedSample").toBool(),
             "Clicking a second pixel must repin, not silently resume hover sampling");
    QVERIFY(session->pixel() != first);
    const auto sample = session->pixel();
    for (int repeat = 0; repeat < 3; ++repeat) {
      window_->showMaximized();
      QTRY_COMPARE(window_->visibility(), QWindow::Maximized);
      QTest::qWait(60);
      QTest::mouseMove(window_, view->mapToScene(view->imageRect().center()).toPoint());
      QCOMPARE(session->pixel(), sample);
      QVERIFY(pane->property("fixedSample").toBool());
      window_->showNormal();
      QTRY_COMPARE(window_->visibility(), QWindow::Windowed);
      window_->resize(repeat % 2 ? QSize(1280, 800) : QSize(1600, 900));
      QTest::qWait(60);
      QCOMPARE(session->pixel(), sample);
      QVERIFY(pane->property("fixedSample").toBool());
    }
    view->forceActiveFocus();
    QTest::keyClick(window_, Qt::Key_Escape);
    QVERIFY(!pane->property("fixedSample").toBool());
    session->open(fixture_ + "/图片0.png");
    QVERIFY(!session->pixel()["valid"].toBool());
    QTRY_VERIFY(!session->busy());
    QVERIFY(!pane->property("fixedSample").toBool());
    save("pinned-window-states");
  }
  void pixelInspectionUsesContentHeight() {
    auto *workspace = item("workspace");
    QVERIFY(workspace);
    workspace->setProperty("settingsVisible", false);
    workspace->setProperty("conversionVisible", false);
    workspace->setProperty("tasksVisible", false);
    workspace->setProperty("focusMode", false);
    auto *session = qobject_cast<ImageSession *>(desktop_->image());
    QVERIFY(session);
    desktop_->openPath(small_ + "/唯一图片.png");
    QTRY_VERIFY(idle());
    QTRY_VERIFY(session->available() && !session->busy());
    session->sample(0, 0);
    QTRY_VERIFY(item("pixelToggle"));
    auto *toggle = item("pixelToggle");
    toggle->setProperty("checked", true);
    QTRY_VERIFY(item("pixelPanel"));
    auto *panel = item("pixelPanel");
    auto *view = item("imageViewport");
    QVERIFY(toggle && panel && view);
    for (const auto size : {QSize(1600, 900), QSize(1280, 800)}) {
      window_->resize(size);
      toggle->setProperty("checked", false);
      QTest::qWait(80);
      const auto closedHeight = view->height();
      for (int repeat = 0; repeat < 3; ++repeat) {
        toggle->setProperty("checked", true);
        QTRY_VERIFY(panel->isVisible() && panel->implicitHeight() > 60);
        QTest::qWait(80);
        const auto expected = qMin(panel->implicitHeight(), panel->parentItem()->height() * .35);
        QVERIFY2(qAbs(panel->height() - expected) < 1,
                 qPrintable(QString("Pixel panel reserves %1 for %2 content (cap %3)")
                                .arg(panel->height()).arg(panel->implicitHeight()).arg(expected)));
        toggle->setProperty("checked", false);
        QTRY_VERIFY(qAbs(view->height() - closedHeight) < 1);
      }
    }
    save("pixel-content-height-restored");
  }
  void pixelInspectionOverflowAndLanguages() {
    using namespace formaview;
    auto source = ImageSubresource{};
    source.data_window = {0, 0, 2, 2};
    source.display_window = source.data_window;
    for (int i = 0; i < 16; ++i) {
      ImageChannel channel;
      channel.name = i < 4 ? std::array<std::string, 4>{"R", "G", "B", "A"}[i]
                           : "layer" + std::to_string(i) + ".value";
      channel.samples.scalar = ScalarType::Float32;
      channel.samples.element_count = 4;
      const float value = i * .125F;
      channel.samples.bytes.resize(4 * sizeof(value));
      for (int p = 0; p < 4; ++p)
        std::memcpy(channel.samples.bytes.data() + p * sizeof(value), &value, sizeof(value));
      source.channels.push_back(std::move(channel));
    }
    const auto bytes = imaging::encode_professional(source, {});
    const auto path = small_ + "/pixel-sixteen-channels.exr";
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(reinterpret_cast<const char *>(bytes.data()), qint64(bytes.size())), qint64(bytes.size()));
    file.close();
    auto *session = qobject_cast<ImageSession *>(desktop_->image());
    QVERIFY(session);
    QJsonArray measurements;
    for (const auto size : {QSize(1600, 900), QSize(1280, 800)}) {
      window_->resize(size);
      for (const auto &language : {QString("zh-CN"), QString("en"), QString("zh-TW")}) {
        desktop_->setLanguage(language);
        for (const auto &image : {small_ + "/唯一图片.png", path}) {
          session->open(image);
          QTRY_VERIFY(session->path() == image && !session->busy());
          auto *toggle = item("pixelToggle");
          QVERIFY(toggle);
          toggle->setProperty("checked", true);
          QTRY_VERIFY(item("pixelPanel"));
          auto *panel = item("pixelPanel");
          // Empty readouts reserve channel rows so first hover does not resize the image.
          for (bool valid : {false, true}) {
            session->sample(valid ? 0 : -999, valid ? 0 : -999);
            QCOMPARE(session->pixel()["valid"].toBool(), valid);
            QTest::qWait(80);
            const auto cap = panel->parentItem()->height() * .35;
            QVERIFY(qAbs(panel->height() - qMin(panel->implicitHeight(), cap)) < 1);
            measurements.append(QJsonObject{{"width",window_->width()},{"language",language},{"image",image},
              {"valid",valid},{"panel_height",panel->height()},{"content_height",panel->implicitHeight()},{"cap",cap}});
          }
          if (image == path) {
            QCOMPARE(session->pixel()["source"].toList().size(), 16);
            QVERIFY(panel->implicitHeight() > panel->height());
          }
          auto *copy = item("copyPixelValues");
          QVERIFY(copy);
          copy->forceActiveFocus();
          QTest::qWait(40);
          const auto bottom = copy->mapToItem(panel, QPointF(0, copy->height())).y();
          QVERIFY2(bottom <= panel->height() + 1 && bottom > 0, "Copy button must scroll into view on keyboard focus");
          QTest::keyClick(window_, Qt::Key_Space);
          QCOMPARE(QGuiApplication::clipboard()->text(), session->pixelReport());
          save("pixel-" + QString::number(size.width()) + "-" + language + (image == path ? "-overflow" : "-compact"));
          toggle->setProperty("checked", false);
        }
      }
    }
    desktop_->setLanguage("zh-CN");
    evidence_["pixel_content_geometry"] = measurements;
  }
  void pixelInspectionMouseAndKeyboard() {
    auto *session = qobject_cast<ImageSession *>(desktop_->image());
    QVERIFY(session);
    desktop_->openPath(small_ + "/唯一图片.png");
    QTRY_VERIFY_WITH_TIMEOUT(session->available() && !session->busy(), 10000);
    auto *toggle = item("pixelToggle");
    QVERIFY(toggle);
    toggle->setProperty("checked", false);
    QTest::qWait(80);
    const auto centre = toggle->mapToScene({toggle->width()/2, toggle->height()/2});
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centre.toPoint());
    QTRY_VERIFY(toggle->property("checked").toBool());
    auto panel = [&]() -> QQuickItem * {
      for (auto *candidate : window_->findChildren<QQuickItem *>())
        if (QString(candidate->metaObject()->className()).startsWith("PixelPanel_") && candidate->isVisible()) return candidate;
      return nullptr;
    };
    QTRY_VERIFY(panel() && panel()->height() > 60 && panel()->width() > 100);
    auto *view = qobject_cast<ImageView *>(item("imageViewport"));
    QVERIFY(view);
    QTest::qWait(60);
    const auto samplePoint = view->mapToScene({view->width()/2, view->height()/2});
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, samplePoint.toPoint());
    QTRY_VERIFY(session->pixel()["valid"].toBool());
    QVERIFY(!session->pixel()["source"].toList().isEmpty());
    QVERIFY(!session->pixelReport().isEmpty());
    QTest::qWait(60);
    QVERIFY(view->height() > 0);
    QVERIFY(qAbs(panel()->height() - qMin(panel()->implicitHeight(), panel()->parentItem()->height() * .35)) < 1);
    save("pixel-click-expanded");
    toggle->forceActiveFocus();
    QTest::keyClick(window_, Qt::Key_Space);
    QTRY_VERIFY(!toggle->property("checked").toBool());
    QTRY_VERIFY(!panel());
    QTest::keyClick(window_, Qt::Key_Space);
    QTRY_VERIFY(panel() && panel()->height() > 60);
    toggle->setProperty("checked", false);
  }
  void imagePrecisionAndViewport() {
    using namespace formaview;
    auto *session = qobject_cast<ImageSession *>(desktop_->image());
    QVERIFY(session);
    auto source = ImageSubresource{};
    source.data_window = {-3, 7, 16, 8};
    source.display_window = {-10, -5, 32, 32};
    const std::array<float, 4> values{.125F, .25F, 2.F, .5F};
    for (std::size_t c = 0; c < 4; ++c) {
      ImageChannel channel;
      channel.name = std::array<std::string, 4>{"R", "G", "B", "A"}[c];
      channel.samples.scalar = ScalarType::Float32;
      channel.samples.element_count = 128;
      channel.samples.bytes.resize(128 * sizeof(float));
      for (std::size_t p = 0; p < 128; ++p)
        std::memcpy(channel.samples.bytes.data() + p * 4, &values[c], 4);
      source.channels.push_back(std::move(channel));
    }
    const auto bytes = imaging::encode_professional(source, {});
    const auto path = small_ + "/显示精度.exr";
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(reinterpret_cast<const char *>(bytes.data()),
                        static_cast<qint64>(bytes.size())),
             static_cast<qint64>(bytes.size()));
    file.close();
    auto *workspace = item("workspace");
    QVERIFY(workspace);
    workspace->setProperty("tasksVisible", false);
    workspace->setProperty("focusMode", false);
    window_->resize(1600, 900);
    desktop_->openPath(path);
    QTRY_VERIFY_WITH_TIMEOUT(session->available() && session->path() == path &&
                                 !session->busy(),
                             10000);
    auto *view = qobject_cast<ImageView *>(item("imageViewport"));
    QVERIFY(view);
    QCOMPARE(view->session(), session);
    QCOMPARE(session->info()["dataWindow"].toMap()["x"].toLongLong(), -3LL);
    session->sample(-3, 7);
    QVERIFY(session->pixel()["valid"].toBool());
    const auto sourceRows = session->pixel()["source"].toList();
    QCOMPARE(sourceRows.size(), 4);
    session->setting("colour", true);
    session->setting("srgb", true);
    session->setting("exposure", 1.);
    session->setting("composite", false);
    session->setting("alpha", 2);
    auto *exposure = item("imageExposure");
    QVERIFY(exposure);
    auto *exposureInput =
        qvariant_cast<QQuickItem *>(exposure->property("contentItem"));
    QVERIFY(exposureInput);
    exposureInput->forceActiveFocus();
    QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
    QInputMethodEvent exposureCommit;
    exposureCommit.setCommitString("0.75");
    QCoreApplication::sendEvent(exposureInput, &exposureCommit);
    QTest::keyClick(window_, Qt::Key_Return);
    QTRY_COMPARE(session->display()["exposure"].toDouble(), .75);
    session->setting("exposure", 1.);
    auto displayed = session->pixel()["display"].toList();
    QVERIFY(std::abs(displayed[0].toString().toDouble() - .53709873048) < 1e-9);
    QVERIFY(std::abs(displayed[1].toString().toDouble() - .73535698305) < 1e-9);
    QVERIFY(std::abs(displayed[2].toString().toDouble() - 1.82479629528) <
            1e-9);
    QCOMPARE(displayed[3].toString(), QString("0.5"));
    const auto position =
        view->mapToScene(QPointF(view->width() / 2, view->height() / 2));
    auto centrePixel = [&] {
      const auto frame = captureWindow(window_);
      const auto dpr = frame.devicePixelRatio();
      return frame.pixelColor(qRound(position.x() * dpr),
                              qRound(position.y() * dpr));
    };
    QTRY_VERIFY(std::abs(centrePixel().red() - 137) <= 2);
    QVERIFY(std::abs(centrePixel().green() - 188) <= 2);
    QCOMPARE(centrePixel().blue(), 255);
    session->setting("alpha", 3);
    session->setting("composite", true);
    // Composite exposed linear RGB before OETF/SDR clipping. Either checker
    // square yields red 144..149 and saturated blue; clipping before blending
    // incorrectly dims the HDR blue channel.
    QTRY_VERIFY(centrePixel().red() >= 144 && centrePixel().red() <= 149);
    QCOMPARE(centrePixel().blue(), 255);
    session->setting("background", 1);
    QTRY_VERIFY(std::abs(centrePixel().red() - 137) <= 2);
    session->setting("background", 2);
    QTRY_VERIFY(std::abs(centrePixel().red() - 225) <= 2);
    session->setting("background", 0);
    QVERIFY(view->renderError().isEmpty());
    session->setting("alpha", 2);
    session->setting("composite", false);
    view->forceActiveFocus();
    QTest::keyClick(window_, Qt::Key_A);
    QCOMPARE(session->display()["channel"].toInt(), 3);
    QTRY_VERIFY(std::abs(centrePixel().red() - 128) <= 2);
    QCOMPARE(centrePixel().red(), centrePixel().green());
    QVERIFY2(view->hasActiveFocus(),
             qPrintable(QString("Viewport focus moved to %1 (%2)")
                            .arg(window_->activeFocusItem() ? window_->activeFocusItem()->objectName() : "null",
                                 window_->activeFocusItem() ? window_->activeFocusItem()->metaObject()->className() : "null")));
    QTest::keyClick(window_, Qt::Key_C);
    QCOMPARE(session->display()["channel"].toInt(), -1);
    view->actual();
    QVERIFY(std::abs(view->zoom() * window_->effectiveDevicePixelRatio() - 1) <
            1e-9);
    QCOMPARE(view->pixelScale(), 1.);
    session->setting("tiled", true);
    const auto rect = view->imageRect();
    view->pick(rect.x() - view->zoom() * .5, rect.y() - view->zoom() * .5);
    QCOMPARE(session->pixel()["tileX"].toInt(), -1);
    QCOMPARE(session->pixel()["tileY"].toInt(), -1);
    QCOMPARE(session->pixel()["x"].toInt(), 12);
    QCOMPARE(session->pixel()["y"].toInt(), 14);
    session->setting("tiled", false);
    session->sample(-4, 7);
    QVERIFY(!session->pixel()["valid"].toBool());
    view->fit();
    auto *toggle = item("pixelToggle");
    QVERIFY(toggle);
    toggle->setProperty("checked", true);
    session->sample(-3, 7);
    for (const auto &language :
         {QString("zh-CN"), QString("en"), QString("zh-TW")}) {
      desktop_->setLanguage(language);
      QTest::qWait(80);
      save("image-float-" + language);
    }
    desktop_->setLanguage("zh-CN");
    session->open(small_ + "/missing.exr");
    QTRY_VERIFY(!session->busy());
    QVERIFY(!session->error().isEmpty());
    QCOMPARE(session->path(), path);
    session->sample(-3, 7);
    QVERIFY(!session->pixel()["valid"].toBool());
    // Two-channel DDS must not fabricate blue or source alpha.
    source.channels.resize(2);
    source.data_window = {0, 0, 16, 8};
    source.display_window = source.data_window;
    imaging::ProfessionalEncodeOptions options;
    options.target = imaging::ProfessionalTarget::Dds;
    options.dds_format = 83;
    options.allow_lossy = true;
    options.alpha = AlphaMode::Absent;
    const auto dds = imaging::encode_professional(source, options);
    const auto ddsPath = small_ + "/two-channel.dds";
    QFile ddsFile(ddsPath);
    QVERIFY(ddsFile.open(QIODevice::WriteOnly));
    QCOMPARE(ddsFile.write(reinterpret_cast<const char *>(dds.data()),
                           static_cast<qint64>(dds.size())),
             static_cast<qint64>(dds.size()));
    ddsFile.close();
    session->open(ddsPath);
    QTRY_VERIFY(!session->busy());
    QVERIFY2(session->error().isEmpty(), qPrintable(session->error()));
    QCOMPARE(session->channels().size(), 2);
    QCOMPARE(session->display()["channel"].toInt(), 0);
    for (const auto *name :
         {"imageChannel0", "imageChannel3", "imageChannel4"}) {
      const auto controls = visualItems(window_->contentItem(), name);
      QVERIFY2(controls.size() == 1, name);
      QVERIFY(!controls.front()->isEnabled());
    }
    session->sample(0, 0);
    QCOMPARE(session->pixel()["display"].toList()[3].toString(), QString("1"));
    save("image-bc5");
    desktop_->clearAsset();
    QTRY_VERIFY(!item("imageViewport"));
    QVERIFY(!session->available());
  }
  void imageCacheInvalidation() {
    auto *session = qobject_cast<ImageSession *>(desktop_->image());
    QVERIFY(session);
    const auto path = small_ + "/cache.bmp";
    QImage image(32, 32, QImage::Format_RGB888);
    image.fill(Qt::red);
    QVERIFY(image.save(path));
    const QFileInfo before(path);
    const auto modified = before.lastModified();
    const auto size = before.size();
    session->open(path);
    QTRY_VERIFY(!session->busy());
    QVERIFY2(session->error().isEmpty(), qPrintable(session->error()));
    const auto old = session->pixels();
    const auto digest = session->info().value("sha256").toString();
    QCOMPARE(digest.size(), 64);
    session->open(path);
    QTRY_VERIFY(!session->busy());
    QCOMPARE(session->pixels(), old);
#ifdef Q_OS_WIN
    {
      const auto writer =
          CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                      OPEN_EXISTING, 0, nullptr);
      QVERIFY(writer != INVALID_HANDLE_VALUE);
      const auto closeWriter = qScopeGuard([writer] { CloseHandle(writer); });
      session->open(path);
      QTRY_VERIFY(!session->busy());
      QVERIFY(!session->error().isEmpty());
      // Even a matching cache key cannot bless an input currently being edited.
      QCOMPARE(session->pixels(), old);
      session->sample(0, 0);
      QVERIFY(!session->pixel().value("valid").toBool());
    }
#endif
    image.fill(Qt::green);
    QVERIFY(image.save(path));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadWrite));
    QVERIFY(file.setFileTime(modified, QFileDevice::FileModificationTime));
    file.close();
    QCOMPARE(QFileInfo(path).size(), size);
    QCOMPARE(QFileInfo(path).lastModified(), modified);
    session->open(path);
    QTRY_VERIFY(!session->busy());
    QVERIFY2(session->error().isEmpty(), qPrintable(session->error()));
    QVERIFY(session->pixels() != old);
    QVERIFY(session->info().value("sha256").toString() != digest);
    session->sample(0, 0);
    const auto report =
        QJsonDocument::fromJson(session->pixelReport().toUtf8()).object();
    QCOMPARE(report["path"].toString(), path);
    QCOMPARE(report["sha256"].toString(),
             session->info().value("sha256").toString());
    QCOMPARE(report["display"].toArray()[1].toString(), QString("1"));
    const std::weak_ptr<const ImagePixels> released = session->pixels();
    for (int i = 0; i < 12; ++i)
      session->clear();
    QTRY_VERIFY(released.expired());
  }
  void imageFormatMatrix() {
    auto *session = qobject_cast<ImageSession *>(desktop_->image());
    QVERIFY(session);
    const auto directory = root_ + "/fixtures/conversion";
    QFile png(directory + "/source.png");
    QVERIFY(png.open(QIODevice::ReadOnly));
    const auto payload = png.readAll();
    QByteArray secondPayload;
    QBuffer secondBuffer(&secondPayload);
    QVERIFY(secondBuffer.open(QIODevice::WriteOnly));
    QImage secondIcon(4, 4, QImage::Format_RGB32);
    secondIcon.fill(Qt::red);
    QVERIFY(secondIcon.save(&secondBuffer, "PNG"));
    QFile ico(directory + "/converted.ico");
    QVERIFY(ico.open(QIODevice::WriteOnly));
    QDataStream stream(&ico);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << quint16(0) << quint16(1) << quint16(2) << quint8(8) << quint8(8)
           << quint8(0) << quint8(0) << quint16(1) << quint16(32)
           << quint32(payload.size()) << quint32(38);
    stream << quint8(4) << quint8(4) << quint8(0) << quint8(0) << quint16(1)
           << quint16(24) << quint32(secondPayload.size())
           << quint32(38 + payload.size());
    QCOMPARE(ico.write(payload), payload.size());
    QCOMPARE(ico.write(secondPayload), secondPayload.size());
    ico.close();
    QFile gif(directory + "/converted.gif");
    QVERIFY(gif.open(QIODevice::WriteOnly));
    const auto gifBytes =
        QByteArray::fromHex("474946383961010001008000002060a000000021f904010000"
                            "01002c00000000010001000002024401003b");
    QCOMPARE(gif.write(gifBytes), gifBytes.size());
    gif.close();
    QJsonArray formats;
    for (const auto *extension : {"png", "jpeg", "bmp", "tga", "tiff", "webp",
                                  "dds", "hdr", "exr", "gif", "ico"}) {
      const auto path = directory + "/converted." + extension;
      desktop_->openPath(path);
      QTRY_VERIFY_WITH_TIMEOUT(session->available() && !session->busy() &&
                                   session->path() == path,
                               10000);
      QVERIFY2(session->error().isEmpty(), qPrintable(session->error()));
      session->sample(0, 0);
      QVERIFY(session->pixel()["valid"].toBool());
      const auto channels = session->pixel()["source"].toList();
      for (int component = 0; component < 3; ++component) {
        const auto name = QStringList{"R", "G", "B"}[component];
        const auto found =
            std::find_if(channels.begin(), channels.end(), [&](const auto &v) {
              return v.toMap()["name"].toString() == name;
            });
        QVERIFY(found != channels.end());
        const auto sample = found->toMap()["interpreted"].toString().toDouble();
        QVERIFY2(std::abs(sample - (32 + component * 64) / 255.) < .025,
                 extension);
      }
      session->setting("channel", -1);
      session->setting("colour", false);
      session->setting("composite", false);
      auto *view = qobject_cast<ImageView *>(item("imageViewport"));
      QVERIFY(view);
      const auto point =
          view->mapToScene({view->width() / 2, view->height() / 2});
      const auto frame = captureWindow(window_);
      const auto colour =
          frame.pixelColor(qRound(point.x() * frame.devicePixelRatio()),
                           qRound(point.y() * frame.devicePixelRatio()));
      QVERIFY2(std::abs(colour.red() - 32) <= 6 &&
                   std::abs(colour.green() - 96) <= 6 &&
                   std::abs(colour.blue() - 160) <= 6,
               extension);
      QVERIFY(view->renderError().isEmpty());
      formats.append(extension);
      if (QString(extension) == "ico" || QString(extension) == "gif")
        save(QString("image-format-") + extension);
    }
    evidence_["image_gui_read_formats"] = formats;
    QCOMPARE(session->subresourceCount(), 2);
    auto *iconEntries = item("imageSubresource");
    QVERIFY(iconEntries);
    iconEntries->forceActiveFocus();
    QTest::keyClick(window_, Qt::Key_Down);
    QTRY_VERIFY(!session->busy() && session->selected() == 1);
    QCOMPARE(session->info()["dataWindow"].toMap()["width"].toInt(), 4);
    QCOMPARE(session->info()["metadata"]
                 .toMap()["entry.1.directory_bits_per_pixel"]
                 .toString(),
             QString("24"));
    session->sample(0, 0);
    QCOMPARE(session->pixel()["display"].toList()[0].toString(), QString("1"));
    save("image-ico-second-entry");
    QTest::keyClick(window_, Qt::Key_Up);
    QTRY_VERIFY(!session->busy() && session->selected() == 0);
    QCOMPARE(session->info()["dataWindow"].toMap()["width"].toInt(), 8);
    QImage quadrants(2, 2, QImage::Format_RGB32);
    quadrants.setPixelColor(0, 0, Qt::red);
    quadrants.setPixelColor(1, 0, Qt::green);
    quadrants.setPixelColor(0, 1, Qt::blue);
    quadrants.setPixelColor(1, 1, Qt::white);
    const auto asymmetric = directory + "/orientation.png";
    QVERIFY(quadrants.save(asymmetric));
    desktop_->openPath(asymmetric);
    QTRY_VERIFY_WITH_TIMEOUT(!session->busy() && session->path() == asymmetric,
                             10000);
    auto *view = qobject_cast<ImageView *>(item("imageViewport"));
    QVERIFY(view);
    const auto rectangle = view->imageRect();
    const auto frame = captureWindow(window_);
    for (int y = 0; y < 2; ++y)
      for (int x = 0; x < 2; ++x) {
        const QPointF local(rectangle.x() + (x + .5) * view->zoom(),
                            rectangle.y() + (y + .5) * view->zoom());
        const auto point = view->mapToScene(local) * frame.devicePixelRatio();
        QCOMPARE(frame.pixelColor(qFloor(point.x()), qFloor(point.y())),
                 quadrants.pixelColor(x, y));
        view->pick(local.x(), local.y());
        QCOMPARE(session->pixel()["x"].toInt(), x);
        QCOMPARE(session->pixel()["y"].toInt(), y);
      }
    save("image-orientation");
    view->setZoom(view->zoom() / 4);
    session->setting("tiled", true);
    const auto tileRect = view->imageRect();
    const QPointF negativeTile(tileRect.x() - view->zoom() / 2,
                               tileRect.y() - view->zoom() / 2);
    view->pick(negativeTile.x(), negativeTile.y());
    QCOMPARE(session->pixel()["tileX"].toInt(), -1);
    QCOMPARE(session->pixel()["tileY"].toInt(), -1);
    QCOMPARE(session->pixel()["x"].toInt(), 1);
    QCOMPARE(session->pixel()["y"].toInt(), 1);
    const auto tiledFrame = captureWindow(window_);
    const auto tilePoint =
        view->mapToScene(negativeTile) * tiledFrame.devicePixelRatio();
    QCOMPARE(
        tiledFrame.pixelColor(qFloor(tilePoint.x()), qFloor(tilePoint.y())),
        QColor(Qt::white));
    session->setting("tiled", false);
    desktop_->clearAsset();
  }
  void imageStructuresAndLayers() {
    using namespace formaview;
    auto *session = qobject_cast<ImageSession *>(desktop_->image());
    QVERIFY(session);
    desktop_->openPath(root_ + "/fixtures/image/subresources-0.dds");
    QTRY_VERIFY_WITH_TIMEOUT(
        session->available() && !session->busy() &&
            session->path() == root_ + "/fixtures/image/subresources-0.dds",
        10000);
    QVERIFY2(session->error().isEmpty(), qPrintable(session->error()));
    QCOMPARE(session->dimensions()["array"].toList().size(), 2);
    QCOMPARE(session->dimensions()["face"].toList().size(), 6);
    QCOMPARE(session->dimensions()["mip"].toList().size(), 4);
    session->selectDimension("array", 1);
    QTRY_VERIFY(!session->busy());
    session->selectDimension("face", 5);
    QTRY_VERIFY(!session->busy());
    session->selectDimension("mip", 2);
    QTRY_VERIFY(!session->busy());
    auto current = session->dimensions()["current"].toMap();
    QCOMPARE(current["array"].toInt(), 1);
    QCOMPARE(current["face"].toInt(), 5);
    QCOMPARE(current["mip"].toInt(), 2);
    session->sample(0, 0);
    QCOMPARE(session->pixel()["source"]
                 .toList()[0]
                 .toMap()["source"]
                 .toString()
                 .toDouble(),
             session->selected() + .25);
    save("image-dds-cube-array");
    session->open(root_ + "/fixtures/image/subresources-1.dds");
    QTRY_VERIFY(!session->busy());
    QCOMPARE(session->dimensions()["slice"].toList().size(), 4);
    session->selectDimension("slice", 3);
    QTRY_VERIFY(!session->busy());
    session->selectDimension("mip", 2);
    QTRY_VERIFY(!session->busy());
    current = session->dimensions()["current"].toMap();
    QCOMPARE(current["mip"].toInt(), 2);
    QCOMPARE(current["slice"].toInt(), 0);
    QCOMPARE(session->dimensions()["slice"].toList().size(), 1);
    save("image-dds-volume");
    const auto multipart = root_ + "/fixtures/image/multipart.exr";
    desktop_->openPath(multipart);
    QTRY_VERIFY(session->available() && !session->busy() &&
                session->path() == multipart);
    QCOMPARE(session->subresourceCount(), 2);
    auto *parts = item("imageSubresource");
    QVERIFY(parts);
    parts->forceActiveFocus();
    QTest::keyClick(window_, Qt::Key_Down);
    QTRY_VERIFY(!session->busy() && session->selected() == 1);
    QCOMPARE(session->subresources()[1].toMap()["name"].toString(),
             QString("depth"));
    session->sample(1, 0);
    QVERIFY(session->pixel()["valid"].toBool());
    QCOMPARE(session->pixel()["source"].toList()[0].toMap()["name"].toString(),
             QString("Z"));
    QCOMPARE(
        session->pixel()["source"].toList()[0].toMap()["source"].toString(),
        QString("7.5"));
    save("image-exr-selected-part");
    ImageSubresource source;
    source.data_window = source.display_window = {0, 0, 1, 1};
    for (const auto &[name, value] :
         std::array<std::pair<const char *, float>, 7>{{{"beauty.R", .125F},
                                                        {"beauty.G", .25F},
                                                        {"beauty.B", .5F},
                                                        {"beauty.A", .75F},
                                                        {"utility.Z", 1.5F},
                                                        {"mono.Y", .25F},
                                                        {"mono.A", .5F}}}) {
      ImageChannel channel;
      channel.name = name;
      channel.samples.scalar = ScalarType::Float32;
      channel.samples.element_count = 1;
      channel.samples.bytes.resize(sizeof(value));
      std::memcpy(channel.samples.bytes.data(), &value, sizeof(value));
      source.channels.push_back(std::move(channel));
    }
    const auto bytes = imaging::encode_professional(source, {});
    const auto path = small_ + "/actual-layers.exr";
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(reinterpret_cast<const char *>(bytes.data()),
                        static_cast<qint64>(bytes.size())),
             static_cast<qint64>(bytes.size()));
    file.close();
    session->open(path);
    QTRY_VERIFY(!session->busy());
    QVERIFY2(session->error().isEmpty(), qPrintable(session->error()));
    QCOMPARE(session->info()["layers"].toStringList(),
             QStringList({"beauty", "mono", "utility"}));
    QCOMPARE(session->info()["mapping"].toList()[0].toString(),
             QString("beauty.R"));
    QCOMPARE(session->display()["channel"].toInt(), -1);
    session->setting("colour", true);
    session->setting("exposure", .5);
    session->selectLayer("utility");
    QTRY_VERIFY(!session->busy());
    QCOMPARE(session->info()["mapping"].toList()[0].toString(),
             QString("utility.Z"));
    QCOMPARE(session->display()["exposure"].toDouble(), .5);
    session->sample(0, 0);
    QCOMPARE(session->pixel()["display"].toList()[0].toString(),
             QString("1.5"));
    session->selectLayer("mono");
    QTRY_VERIFY(!session->busy());
    QCOMPARE(session->info()["mapping"].toStringList(),
             QStringList({"mono.Y", "mono.Y", "mono.Y", "mono.A"}));
    session->selectLayer("beauty");
    QTRY_VERIFY(!session->busy());
    QCOMPARE(session->info()["mapping"].toList()[3].toString(),
             QString("beauty.A"));
    session->setting("channel", -1);
    for (const auto &language :
         {QString("zh-CN"), QString("en"), QString("zh-TW")}) {
      desktop_->setLanguage(language);
      QTest::qWait(60);
      save("image-exr-layers-" + language);
    }
    desktop_->setLanguage("zh-CN");
    desktop_->clearAsset();
  }
  void imageApprovedOutputsAndSelectedSemantics() {
    try {
    const auto fixtures =
        requireFixture("FORMAVIEW_TEST_BOUNDARY_FIXTURES");
    if (fixtures.isEmpty())
      QSKIP("Set external independently generated boundary fixtures to run "
            "this regression");
    auto *session = qobject_cast<ImageSession *>(desktop_->image());
    QVERIFY(session);
    ImageJobs service;
    const auto output = root_ + "/gui-boundary-output";
    QVERIFY(QDir().mkpath(output));
    const QVariantMap scope{{"input_roots", QStringList{fixtures}},
                            {"output_roots", QStringList{output}},
                            {"allow_overwrite", false}};
    QJsonArray pixels, records;
    for (const auto &format : {QString("tiff"), QString("ico")}) {
      const auto source = fixtures + "/mixed." + format;
      desktop_->openPath(source);
      QTRY_VERIFY_WITH_TIMEOUT(session->available() && !session->busy() &&
                                   session->path() == source,
                               10000);
      for (int page = 0; page < 2; ++page) {
        session->selectSubresource(page);
        QTRY_VERIFY(!session->busy() && session->selected() == page);
        const auto alpha = page == 0          ? "straight"
                           : format == "tiff" ? "premultiplied"
                                              : "absent";
        QCOMPARE(session->info()["alpha"].toInt(), page == 0          ? 2
                                                   : format == "tiff" ? 3
                                                                      : 1);
        QCOMPARE(session->info()["containerAlpha"].toInt(), 0);
        QCOMPARE(!session->info()["iccProfileSha256"].toString().isEmpty(),
                 page == 0);
        const auto target = format == "tiff" ? "tiff" : "png";
        const auto filename = format + QString::number(page) + "." + target;
        QFile occupied(output + '/' + filename);
        QVERIFY(occupied.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        QCOMPARE(occupied.write("external-owner"), 14LL);
        occupied.close();
        service.preflight({source}, scope,
                          {{"target", target}, {"subresource", page}}, filename,
                          "rename");
        QTRY_VERIFY_WITH_TIMEOUT(!service.busy() && !service.plan().isEmpty() &&
                                     service.plan().value("state") !=
                                         "planning",
                                 15000);
        QVERIFY2(service.plan().value("state") == "ready",
                 qPrintable(service.error()));
        const auto entry = service.plan()["items"].toList()[0].toMap();
        QCOMPARE(entry["request"]
                     .toMap()["options"]
                     .toMap()["source_alpha"]
                     .toString(),
                 QString(alpha));
        const auto selected = entry["preflight"].toMap()["output"].toString();
        QCOMPARE(QFileInfo(selected).fileName(),
                 format + QString::number(page) + " (1)." + target);
        QVERIFY(!QFile::exists(selected));
        records.append(QJsonObject::fromVariantMap(service.plan()));
        service.submit(true);
        QTRY_VERIFY_WITH_TIMEOUT(
            !service.busy() && service.job()["state"] == "succeeded", 15000);
        const auto result =
            service.job()["items"].toList()[0].toMap()["result"].toMap();
        QCOMPARE(result["committed_output"].toString(), selected);
        QVERIFY(occupied.open(QIODevice::ReadOnly));
        QCOMPARE(occupied.readAll(), QByteArray("external-owner"));
        QJsonObject check{{"path", selected},
                          {"sha256", result["output_sha256"].toString()},
                          {"icc", page == 0}};
        if (format == "tiff") {
          check["alpha_tag"] = page == 0 ? 2 : 1;
          check["value"] = page == 0 ? 128 : 64;
        } else {
          check["pixel"] =
              page == 0 ? QJsonArray{120, 30, 20, 128} : QJsonArray{4, 6, 8};
        }
        pixels.append(check);
        save("image-boundary-" + format + QString::number(page));
      }
    }
    QFile manifest(root_ + "/gui-boundary-pixels.json");
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(QJsonDocument(pixels).toJson());
    QFile plans(root_ + "/gui-boundary-plans.json");
    QVERIFY(plans.open(QIODevice::WriteOnly));
    plans.write(QJsonDocument(records).toJson());
    desktop_->clearAsset();
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void generalSettingsDraftAndNavigation() {
    auto *workspace = item("workspace");
    auto *settings = window_->findChild<QObject *>("mcpSettingsDialog");
    QVERIFY(workspace && settings);
    const auto preferences = desktop_->preferences();
    const auto language = desktop_->language();
    const auto restore = qScopeGuard([&] {
      workspace->setProperty("settingsVisible", false);
      for (const auto &key : {"density", "restoreLayout", "defaultFilesVisible", "inspectorMode"})
        desktop_->preference(key, preferences.value(key));
      desktop_->setLanguage(language);
      window_->resize(1600, 900);
    });
    desktop_->preference("density", "standard");
    auto click = [&](const char *name) {
      auto *control = item(name);
      QVERIFY(control && control->isEnabled());
      control->forceActiveFocus(Qt::TabFocusReason);
      QTest::qWait(80);
      const auto point = control->mapToScene({control->width()/2, control->height()/2});
      QVERIFY(QRectF(0,0,window_->width(),window_->height()).contains(point));
      QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, point.toPoint());
      QTest::qWait(40);
    };
    QVERIFY(QMetaObject::invokeMethod(settings, "open", Q_ARG(QVariant, "general")));
    QTRY_VERIFY(item("settingsDensityCompact"));
    QVERIFY(!settings->property("dirty").toBool());
    click("settingsDensityCompact");
    QVERIFY(settings->property("dirty").toBool());
    QCOMPARE(desktop_->preferences().value("density").toString(), QString("standard"));
    click("nav-cache");
    QCOMPARE(settings->property("section").toString(), QString("cache"));
    QVERIFY(settings->property("dirty").toBool());
    QTest::keyClick(window_, Qt::Key_Escape);
    QTRY_VERIFY(item("keepEditingSettings"));
    click("keepEditingSettings");
    QVERIFY(settings->property("visible").toBool());
    click("nav-general");
    click("saveGeneralSettings");
    QCOMPARE(desktop_->preferences().value("density").toString(), QString("compact"));
    QVERIFY(!settings->property("dirty").toBool());
    click("settingsDensityStandard");
    click("discardGeneralSettings");
    QCOMPARE(desktop_->preferences().value("density").toString(), QString("compact"));
    QVERIFY(item("settingsDensityCompact")->property("checked").toBool());
    click("settingsDensityStandard");
    click("saveGeneralSettings");
    for (const auto size : {QSize(1600,900), QSize(1280,800)}) {
      window_->resize(size);
      for (const auto &locale : {QString("zh-CN"), QString("en"), QString("zh-TW")}) {
        desktop_->setLanguage(locale);
        QVERIFY(QMetaObject::invokeMethod(settings, "close"));
        QVERIFY(QMetaObject::invokeMethod(settings, "open", Q_ARG(QVariant, "general")));
        QTest::qWait(100);
        save("cc-general-" + QString::number(size.width()) + "-" + locale);
        QVERIFY(item("saveGeneralSettings"));
        const auto footer = item("saveGeneralSettings");
        QVERIFY(footer->mapToScene({footer->width(),footer->height()}).y() < size.height() - 40);
      }
    }
    click("settingsDensityCompact");
    // Closing the application must keep a dirty draft available for review.
    QVERIFY(!window_->close());
    QTRY_VERIFY(item("keepEditingSettings"));
    click("keepEditingSettings");
    QTest::keyClick(window_, Qt::Key_L, Qt::ControlModifier);
    QTRY_VERIFY(item("discardAndLeaveSettings"));
    click("discardAndLeaveSettings");
    QTRY_VERIFY(!settings->property("visible").toBool());
    QCOMPARE(desktop_->preferences().value("density").toString(), QString("standard"));
    if (auto *drawer = window_->findChild<QObject *>("fileDrawer"))
      QMetaObject::invokeMethod(drawer, "close");
  }

  void settingsDirtyLeaveCallbacks() {
    auto *workspace = item("workspace");
    auto *settings = window_->findChild<QObject *>("mcpSettingsDialog");
    QVERIFY(workspace && settings);
    const auto preferences = desktop_->preferences();
    const auto restore = qScopeGuard([&] {
      workspace->setProperty("settingsVisible", false);
      workspace->setProperty("tasksVisible", false);
      workspace->setProperty("conversionVisible", false);
      desktop_->preference("density", preferences.value("density"));
    });
    desktop_->preference("density", "standard");
    for (const auto &destination : {QString("tasks"), QString("conversion")}) {
      QVERIFY(QMetaObject::invokeMethod(settings, "open", Q_ARG(QVariant, "general")));
      QTRY_VERIFY(item("settingsDensityCompact"));
      QVERIFY(QMetaObject::invokeMethod(item("settingsDensityCompact"), "clicked"));
      QVERIFY(settings->property("dirty").toBool());
      QTRY_VERIFY(item("settingsDraftStatus"));
      QCOMPARE(item("settingsDraftStatus")->property("text").toString(), QString("有未保存的更改"));
      // Visibility/text are updated before the layout polish places the label.
      QTRY_VERIFY(item("settingsDraftStatus")->mapToScene({0,0}).y() >= window_->height() - 44);
      QVERIFY(QMetaObject::invokeMethod(workspace, "navigate", Q_ARG(QVariant, destination)));
      QTRY_VERIFY(item("keepEditingSettings"));
      QCOMPARE(workspace->property("currentPage").toString(), QString("settings"));
      QVERIFY(QMetaObject::invokeMethod(item("keepEditingSettings"), "clicked"));
      QVERIFY(settings->property("dirty").toBool());
      QCOMPARE(desktop_->preferences().value("density").toString(), QString("standard"));
      QVERIFY(QMetaObject::invokeMethod(workspace, "navigate", Q_ARG(QVariant, destination)));
      QTRY_VERIFY(item("discardAndLeaveSettings"));
      QVERIFY(QMetaObject::invokeMethod(item("discardAndLeaveSettings"), "clicked"));
      QTRY_COMPARE(workspace->property("currentPage").toString(), destination);
      QVERIFY(!settings->property("dirty").toBool());
      QVERIFY(!workspace->property("settingsDirty").toBool());
      QCOMPARE(desktop_->preferences().value("density").toString(), QString("standard"));
    }
  }


#ifdef FORMAVIEW_MODEL_EXPORT
  void sceneMalformedPreviewStorage() {
    try {
    const auto fixtures = requireFixture("FORMAVIEW_SCENE_REVIEW_FIXTURES");
    if (fixtures.isEmpty()) QSKIP("Set FORMAVIEW_SCENE_REVIEW_FIXTURES to review fixtures");
    for (const auto *name : {"sparse-outside-buffer.gltf", "offset-overflow.gltf", "deep-hierarchy.gltf"}) {
      const auto path = fixtures + "/" + name;
      QVERIFY(QFile::exists(path));
      QVERIFY_THROWS_EXCEPTION(formaview::skeletal::Failure, readModelDocument(path, {}));
    }
    // Positive controls: the guard must not disable valid sparse data or GLB.
    for (const auto *name : {"source.gltf", "valid-sparse.gltf", "source.glb"}) {
      QVERIFY(QFile::exists(fixtures + "/" + name));
      const auto model = readModelDocument(fixtures + "/" + name, {});
      QVERIFY2(model, name);
    }
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void sceneExportGuiLifecycle() {
    try {
    const auto fixtures = requireFixture("FORMAVIEW_SCENE_EXPORT_FIXTURES");
    if (fixtures.isEmpty()) QSKIP("Set FORMAVIEW_SCENE_EXPORT_FIXTURES to scene_export_tests synthetic fixtures");
    auto *service = qobject_cast<ImageJobs *>(desktop_->imageJobs());
    auto *workspace = item("workspace"); QVERIFY(service && workspace);
    desktop_->navigate(fixtures); QTRY_VERIFY(desktop_->directory() == fixtures && idle());
    desktop_->filter("", 0, 0); QTRY_VERIFY(idle());
    const auto index = static_cast<FileList *>(desktop_->files())->indexOf(fixtures + "/source.glb");
    QVERIFY(index >= 0); desktop_->select(index); QTRY_VERIFY(idle());
    QVERIFY(QMetaObject::invokeMethod(workspace, "openConversion", Q_ARG(QVariant, 0), Q_ARG(QVariant, "")));
    QTRY_VERIFY(item("modelExportContent"));
    auto *pairing = window_->findChild<QQuickItem *>("modelExportPairing");
    QVERIFY(pairing);
    QCOMPARE(pairing->property("currentIndex").toInt(), 1);
    const auto cleanup = qScopeGuard([&] { if (auto *conversion = window_->findChild<QObject *>("conversionWorkspace")) QMetaObject::invokeMethod(conversion, "close"); });
    const auto edit = [&](const char *name, const QString &text) {
      auto *field = item(name); QVERIFY(field); field->forceActiveFocus();
      QVERIFY(QMetaObject::invokeMethod(field, "selectAll"));
      QInputMethodEvent event; event.setCommitString(text); QCoreApplication::sendEvent(field, &event);
      QCOMPARE(field->property("text").toString(), text);
    };
    const auto choose = [&](const char *name, int value) {
      auto *choice = item(name); QVERIFY(choice); choice->forceActiveFocus(); QTest::keyClick(window_, Qt::Key_Home);
      for (int n = 0; n < value; ++n) QTest::keyClick(window_, Qt::Key_Down);
      QCOMPARE(choice->property("currentIndex").toInt(), value);
    };
    const auto click = [&](const char *name) {
      auto *control = item(name); QVERIFY(control && control->isEnabled()); control->forceActiveFocus(); QTest::qWait(80);
      QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, control->mapToScene({control->width()/2,control->height()/2}).toPoint());
    };
    const auto output = root_ + "/scene-export"; QVERIFY(QDir().mkpath(output)); edit("conversionOutputRoot", output);
    choose("modelExportContent", 0);
    auto *pairedControl = window_->findChild<QQuickItem *>("modelExportPaired");
    QVERIFY(pairedControl); QVERIFY(!pairedControl->isVisible());
    click("modelExportInspect"); QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && !service->modelSources().isEmpty(), 10000);
    QCOMPARE(service->modelSources().front().toMap().value("clips").toList().size(), 2);
    QJsonArray jobs;
    for (int mode = 0; mode < 2; ++mode) {
      if (mode) click("editConversionSettings");
      choose("modelExportOrganization", mode == 0 ? 1 : 0);
      QCOMPARE(item("conversionNamingTemplate")->property("text").toString(), mode == 0 ? QString("{name}-animations.{ext}") : QString("{name}-{clip}.{ext}"));
      const auto previousJob = service->job().value("id").toString();
      save("scene-settings-" + QString::number(mode)); click("createImagePlan");
      // Model export starts a direct job. These fixtures have declared losses:
      // follow the UI's retry/review path before acknowledging and submitting.
      QTRY_VERIFY_WITH_TIMEOUT(!service->busy() &&
          !service->job().value("id").toString().isEmpty() &&
          service->job().value("id").toString() != previousJob &&
          service->job().value("state") == "failed", 20000);
      for (const auto &v : service->job().value("items").toList()) {
        const auto entry = v.toMap();
        QCOMPARE(entry.value("error").toMap().value("code").toString(), QString("invalid_argument"));
        QVERIFY(entry.value("error").toMap().value("message").toString().contains("loss acknowledgement"));
        QVERIFY(!QFile::exists(entry.value("request").toMap().value("output").toString()));
      }
      click("retryImageJob");
      QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && service->plan().value("state") == "ready", 20000);
      QCOMPARE(service->plan().value("items").toList().size(), mode == 0 ? 1 : 2);
      save("scene-preflight-" + QString::number(mode));
      if (auto *ack = item("acknowledgeImageLosses"); ack && !ack->property("checked").toBool()) click("acknowledgeImageLosses");
      click("submitImagePlan"); QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && service->job().value("state") == "succeeded", 20000);
      for (const auto &v : service->job().value("items").toList()) {
        const auto result = v.toMap().value("result").toMap();
        const auto doc = formaview::skeletal::FbxDocument::read(std::filesystem::path(result.value("committed_output").toString().toStdWString()));
        QCOMPARE(doc.data().clips.size(), mode == 0 ? 2u : 1u); QCOMPARE(doc.data().meshes.size(), 1u);
        for (const auto &clip : doc.data().clips) {
          QCOMPARE(clip.start, clip.source.original_name == "Move" ? 2.0 : 7.0);
          QCOMPARE(clip.end, clip.source.original_name == "Move" ? 4.0 : 9.0);
        }
      }
      jobs.append(QJsonObject::fromVariantMap(service->job()));
      item("conversionSteps")->setProperty("currentIndex", 1);
    }
    QStringList sources;
    for (const auto *suffix : {"obj", "gltf", "glb", "pmx", "vrm"}) sources.append(fixtures + "/source." + suffix);
    QVariantMap scope{{"input_roots", QStringList{fixtures}}, {"output_roots", QStringList{output}}, {"allow_overwrite", false}};
    service->preflight(sources, scope, {{"target", "fbx"}, {"export_content", "model"}}, "batch-{index}.{ext}", "skip");
    QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && service->plan().value("state") == "ready", 20000);
    QCOMPARE(service->plan().value("items").toList().size(), 5);
    // R08: the same preflight losses read in all three languages while the
    // technical code stays available as a copyable second line.
    item("conversionSteps")->setProperty("currentIndex", 1);
    for (const auto &language : {QString("zh-CN"), QString("en"), QString("zh-TW")}) {
      desktop_->setLanguage(language);
      QTest::qWait(150);
      const auto losses = visualItems(window_->contentItem(), "conversionItemLoss");
      QVERIFY2(!losses.isEmpty(), "preflight shows loss rows");
      for (auto *loss : losses) {
        auto text = loss->property("explanation").toString();
        text.remove(loss->property("detail").toString().section(": ", 0, 0)); // source names (e.g. morph names) are data, not wording
        QVERIFY2(!text.isEmpty(), qPrintable(loss->property("code").toString()));
        const bool cjk = std::any_of(text.begin(), text.end(), [](QChar c) { return c.unicode() >= 0x3400 && c.unicode() <= 0x9fff; });
        QVERIFY2(cjk == (language != "en"), qPrintable(language + ": " + text));
        QVERIFY2(loss->property("technical").toString().startsWith(loss->property("code").toString()), "technical code kept");
      }
      save("scene-batch-losses-" + language);
    }
    desktop_->setLanguage("zh-CN");
    service->submit(true); QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && service->job().value("state") == "succeeded", 20000);
    for (const auto &v : service->job().value("items").toList()) {
      const auto doc = formaview::skeletal::FbxDocument::read(std::filesystem::path(v.toMap().value("result").toMap().value("committed_output").toString().toStdWString()));
      QCOMPARE(doc.data().meshes.size(), 1u); QVERIFY(doc.data().clips.empty());
    }
    jobs.append(QJsonObject::fromVariantMap(service->job())); evidence_["scene_export_gui"] = jobs;
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void preflightCombinedFilters() {
    QVariantList entries;
    for (int i = 0; i < 474; ++i) {
      const auto state = i < 200 ? "ready" : i < 300 ? "failed" : i < 400 ? "pending" : "cancelled";
      const auto format = i < 200 ? "png" : i < 300 ? "jpeg" : i < 400 ? "fbx" : "exr";
      entries.append(QVariantMap{{"status", state},
          {"request", QVariantMap{{"input", QString("C:/source/alpha-%1.psa").arg(i, 3, 10, QChar('0'))},
              {"output", QString("C:/output/result-%1.%2").arg(i).arg(format)},
              {"options", QVariantMap{{"target", format}}}}},
          {"preflight", QVariantMap{{"losses", i < 50 ? QVariantList{QVariantMap{{"code", "alpha_dropped"}, {"detail", "Alpha channel omitted"}}} : QVariantList{}}}}});
    }
    QVariantMap snapshot{{"id", "filter-fixture"}, {"state", "ready"}, {"total", 474},
                         {"approval_digest", "fixture-digest"}, {"items", entries}};
    QSignalSpy warnings(engine_.get(), &QQmlEngine::warnings);
    QQmlComponent component(engine_.get());
    component.loadFromModule("FormaView.TestUI", "ConversionResults");
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> object(component.createWithInitialProperties(
        {{"controller", QVariant::fromValue(desktop_.get())}, {"snapshot", snapshot}}));
    auto *panel = qobject_cast<QQuickItem *>(object.get()); QVERIFY(panel);
    QQuickWindow probe;
    probe.setColor(QColor("#1c1c1e")); probe.resize(1060, 760);
    panel->setParentItem(probe.contentItem()); panel->setPosition({20, 20}); panel->setSize({1020, 720});
    probe.show(); QVERIFY(QTest::qWaitForWindowExposed(&probe));
    const auto find = [&](const char *name) { return panel->findChild<QQuickItem *>(name); };
    auto *filters = find("preflightFilters"), *list = find("preflightResultsList");
    auto *search = find("preflightSearch"), *kind = find("preflightKind"), *format = find("preflightFormat");
    QVERIFY(filters && list && search && kind && format);
    const auto reset = [&] { return QMetaObject::invokeMethod(find("clearPreflightFilters"), "clicked"); };
    const auto state = [&](const char *name) {
      const auto matches = visualItems(panel, "preflightState-" + QString::fromUtf8(name));
      if (matches.isEmpty()) return false;
      auto *check = matches.front(); check->setProperty("checked", !check->property("checked").toBool());
      return QMetaObject::invokeMethod(check, "toggled");
    };
    QTRY_COMPARE(list->property("count").toInt(), 474);
    QVERIFY(state("ready")); QTRY_COMPARE(list->property("count").toInt(), 200);
    QVERIFY(state("failed")); QTRY_COMPARE(list->property("count").toInt(), 300);
    find("preflightWarnings")->setProperty("checked", true);
    QTRY_COMPARE(list->property("count").toInt(), 50);
    search->setProperty("text", "ALPHA-004"); QTRY_COMPARE(list->property("count").toInt(), 1);
    QVERIFY(find("acknowledgeImageLosses")->isVisible()); QVERIFY(!find("submitImagePlan")->isEnabled());
    QTest::qWait(80); QVERIFY(captureWindow(&probe).save(root_ + "/filters-combined.png"));
    QVERIFY(reset()); QTRY_COMPARE(list->property("count").toInt(), 474);
    kind->setProperty("currentIndex", 2); QTRY_COMPARE(list->property("count").toInt(), 100);
    // Open the real popup and select the last entry through keyboard input.
    probe.requestActivate();
    QTRY_COMPARE(QGuiApplication::focusWindow(), &probe);
    QTest::mouseClick(&probe, Qt::LeftButton, Qt::NoModifier,
                      format->mapToScene(QPointF(format->width()/2, format->height()/2)).toPoint());
    auto *popup = format->property("popup").value<QObject *>(); QVERIFY(popup);
    QTRY_VERIFY(popup->property("visible").toBool());
    QTest::keyClick(&probe, Qt::Key_End);
    QTest::keyClick(&probe, Qt::Key_Return);
    QTRY_COMPARE(format->property("currentIndex").toInt(), 4);
    QTRY_COMPARE(list->property("count").toInt(), 0);
    QVERIFY(reset()); QTRY_COMPARE(list->property("count").toInt(), 474);
    QCOMPARE(format->property("currentIndex").toInt(), 0);
    search->setProperty("text", "RESULT-473.EXR"); QTRY_COMPARE(list->property("count").toInt(), 1);
    QVERIFY(!find("submitImagePlan")->isEnabled()); // Hidden losses still require whole-plan acknowledgement.
    QVERIFY(reset());
    QTRY_COMPARE(list->property("count").toInt(), 474);
    QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtEnd"));
    QTRY_VERIFY(list->property("contentY").toDouble() > 1000);
    auto details = visualItems(panel, "conversionItemDetails"); QVERIFY(!details.isEmpty());
    QVERIFY(QMetaObject::invokeMethod(details.back(), "clicked"));
    search->setProperty("text", "alpha-000"); QTRY_COMPARE(list->property("count").toInt(), 1);
    QTRY_VERIFY(list->property("contentY").toDouble() < 1);
    QTest::qWait(80);
    details = visualItems(panel, "conversionItemDetails"); QVERIFY(!details.isEmpty());
    QCOMPARE(details.front()->property("text").toString(), QString::fromUtf8("详情"));
    QVERIFY(reset()); QVERIFY(state("pending")); QTRY_COMPARE(list->property("count").toInt(), 100);
    for (int i = 300; i < 400; ++i) { auto row = entries[i].toMap(); row["status"] = "ready"; entries[i] = row; }
    snapshot["items"] = entries; QVERIFY(panel->setProperty("snapshot", snapshot));
    QTRY_COMPARE(list->property("count").toInt(), 0);
    QVERIFY(state("pending")); QTRY_COMPARE(list->property("count").toInt(), 474);
    search->setProperty("text", "missing"); QTRY_COMPARE(list->property("count").toInt(), 0);
    probe.resize(720, 760); panel->setWidth(680); QTest::qWait(80);
    QVERIFY(captureWindow(&probe).save(root_ + "/filters-narrow-empty.png"));
    snapshot["id"] = "next-plan"; QVERIFY(panel->setProperty("snapshot", snapshot));
    QTRY_COMPARE(list->property("count").toInt(), 474); QVERIFY(!filters->property("active").toBool());
    QCOMPARE(panel->property("snapshot").toMap().value("items").toList(), entries);
    QTest::qWait(80); QVERIFY(captureWindow(&probe).save(root_ + "/filters-all.png"));
    QCOMPARE(warnings.size(), 0);
    evidence_["preflight_filters"] = "474 items; status OR, warning/kind/format/path AND; reset, empty, live states, details reuse, count, whole-plan losses and narrow layout";
    panel->setParentItem(nullptr);
  }
  void modelScopeValidation() {
    try {
    const auto fixtures = requireFixture("FORMAVIEW_MODEL_EXPORT_FIXTURES");
    if (fixtures.isEmpty()) QSKIP("Set FORMAVIEW_MODEL_EXPORT_FIXTURES to synthetic ActorX fixtures");
    auto *service = qobject_cast<ImageJobs *>(desktop_->imageJobs());
    auto *workspace = item("workspace");
    QVERIFY(service && workspace);
    const auto input = root_ + "/scope-input", output = root_ + "/scope-output";
    QVERIFY(QDir().mkpath(input)); QVERIFY(QDir().mkpath(output));
    QVERIFY(QFile::copy(fixtures + "/source.psk", input + "/reference.psk"));
    QStringList sources;
    for (int i = 0; i < 65; ++i) {
      const auto source = input + QString("/animation-%1.psa").arg(i);
      QVERIFY(QFile::copy(fixtures + "/source.psa", source)); sources.append(source);
    }
    QMap<QString, QByteArray> originalSources;
    for (const auto &path : sources + QStringList{input + "/reference.psk"}) {
      QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); originalSources[path] = file.readAll();
    }
    QVERIFY(QMetaObject::invokeMethod(workspace, "openConversion", Q_ARG(QVariant, 0), Q_ARG(QVariant, "")));
    auto *conversion = window_->findChild<QObject *>("conversionWorkspace");
    QVERIFY(conversion);
    const auto cleanup = qScopeGuard([&] { QMetaObject::invokeMethod(conversion, "close"); });
    QVERIFY(conversion->setProperty("modelInputs", sources));
    QVERIFY(conversion->setProperty("modelMode", true));
    QTRY_VERIFY(item("conversionOutputRoot"));
    QTRY_VERIFY(item("conversionPngCompression"));
    QCOMPARE(item("conversionPngCompression")->property("value").toInt(), 0);
    QVERIFY(!item("conversionInputRoot"));
    item("conversionOutputRoot")->setProperty("text", "");
    item("modelExportPaired")->setProperty("text", input + "/reference.psk");
    item("conversionTaskName")->setProperty("text", "角色动作批次");
    item("conversionTaskCategory")->setProperty("text", "回归验证");
    auto *preflight = item("createImagePlan"); QVERIFY(preflight);
    QTRY_VERIFY(!preflight->isEnabled());
    const QVariantMap options{{"target", "fbx"}, {"export_content", "animation"},
        {"paired_file", input + "/reference.psk"}, {"file_organization", "per-clip"}};
    item("conversionOutputRoot")->setProperty("text", output);
    QTRY_VERIFY(preflight->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(preflight, "clicked"));
    QTRY_VERIFY2_WITH_TIMEOUT(!service->busy() && service->plan().value("state") == "ready",
                             qPrintable(service->error()), 30000);
    QCOMPARE(service->plan().value("items").toList().size(), 130);
    evidence_["model_scope_ready"] = QJsonObject::fromVariantMap(service->plan());
    service->submit(true);
    QTRY_VERIFY(!service->busy() && !service->job().value("id").toString().isEmpty());
    item("conversionSteps")->setProperty("currentIndex", 2);
    QTRY_VERIFY(item("pauseImageJob"));
    QVERIFY(QMetaObject::invokeMethod(item("pauseImageJob"), "clicked"));
    QTRY_COMPARE_WITH_TIMEOUT(service->job().value("state").toString(), QString("paused"), 15000);
    const auto pausedItems = service->job().value("items");
    QTest::qWait(200); QCOMPARE(service->job().value("items"), pausedItems);
    save("task-paused");
    QVERIFY(QMetaObject::invokeMethod(item("pauseImageJob"), "clicked"));
    QTRY_VERIFY2_WITH_TIMEOUT(!service->busy() && service->job().value("state") == "succeeded",
                             qPrintable(service->error()), 60000);
    const auto outputs = service->job().value("items").toList(); QCOMPARE(outputs.size(), 130);
    QCOMPARE(service->job().value("name").toString(), QString("角色动作批次"));
    QCOMPARE(service->job().value("category").toString(), QString("回归验证"));
    item("conversionSteps")->setProperty("currentIndex", 2);
    QTRY_VERIFY(item("imageJobChoice"));
    QTRY_VERIFY(item("imageJobChoice")->property("displayText").toString().contains("角色动作批次"));
    QTRY_VERIFY(item("conversionBatchList"));
    QTRY_VERIFY(item("selectedBatchHeading")->property("text").toString().contains("角色动作批次"));
    const auto batchCardName = QString("conversionBatch_") + service->job().value("id").toString();
    QTRY_VERIFY(item(qPrintable(batchCardName)));
    QTRY_VERIFY(item(qPrintable(batchCardName))->property("checked").toBool());
    QVERIFY(item("jobCategoryFilter"));
    item("jobCategoryFilter")->setProperty("currentIndex", 1);
    QVERIFY(QMetaObject::invokeMethod(item("jobCategoryFilter"), "activated", Q_ARG(int, 1)));
    QTRY_VERIFY(item("imageJobChoice")->property("displayText").toString().contains("回归验证"));
    save("named-model-batch");
    QTRY_VERIFY(QFileInfo::exists(root_ + "/image-journal/desktop-history.json"));
    {
      ImageJobs reopened;
      QVERIFY(!reopened.jobs().isEmpty());
      const auto restored = reopened.jobs().last().toMap();
      QCOMPARE(restored.value("name").toString(), QString("角色动作批次"));
      QCOMPARE(restored.value("items").toList().size(), 130);
    }
    for (const auto &v : outputs) {
      const auto result = v.toMap().value("result").toMap();
      const auto path = result.value("committed_output").toString();
      QVERIFY(!path.isEmpty());
      const auto fbx = formaview::skeletal::FbxDocument::read(std::filesystem::path(path.toStdWString()));
      QVERIFY(fbx.data().meshes.empty()); QCOMPARE(fbx.data().clips.size(), 1u);
      const auto expectedClip = result.value("clips").toList().front().toMap();
      QVERIFY(std::abs(fbx.data().clips.front().start - expectedClip.value("start").toDouble()) < 1e-6);
      QVERIFY(std::abs(fbx.data().clips.front().end - expectedClip.value("end").toDouble()) < 1e-6);
    }
    evidence_["model_scope_export"] = QJsonObject::fromVariantMap(service->job());
    QJsonArray singleExports;
    const QVariantMap scope{{"input_roots", QStringList{input}}, {"output_roots", QStringList{output}}};
    for (const bool paired : {false, true}) {
      auto singleOptions = options;
      if (!paired) singleOptions.remove("paired_file");
      service->preflight({sources.front()}, scope, singleOptions,
                         paired ? "single-paired-{clip}.{ext}" : "single-auto-{clip}.{ext}", "skip");
      QTRY_VERIFY2_WITH_TIMEOUT(!service->busy() && service->plan().value("state") == "ready",
                               qPrintable(service->error()), 30000);
      QCOMPARE(service->plan().value("items").toList().size(), 2);
      service->submit(true);
      QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && service->job().value("state") == "succeeded", 30000);
      for (const auto &value : service->job().value("items").toList()) {
        const auto result = value.toMap().value("result").toMap();
        const auto doc = formaview::skeletal::FbxDocument::read(std::filesystem::path(result.value("committed_output").toString().toStdWString()));
        QVERIFY(doc.data().meshes.empty()); QCOMPARE(doc.data().clips.size(), 1u);
        const auto expected = result.value("clips").toList().front().toMap();
        QVERIFY(std::abs(doc.data().clips.front().end - expected.value("end").toDouble()) < 1e-6);
      }
      singleExports.append(QJsonObject::fromVariantMap(service->job()));
    }
    evidence_["model_scope_single_exports"] = singleExports;
    const auto outside = root_ + "/outside-reference.psk";
    QVERIFY(QFile::copy(fixtures + "/source.psk", outside));
    auto denied = options; denied["paired_file"] = outside;
    service->preflight({sources.front()}, scope, denied, "outside-{clip}.{ext}", "skip");
    QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && service->plan().value("state") != "planning", 30000);
    const auto failed = QJsonDocument(QJsonObject::fromVariantMap(service->plan())).toJson();
    QVERIFY2(service->plan().value("state") == "failed", failed.constData());
    QVERIFY2(failed.contains("out_of_scope"), failed.constData());
    QVERIFY(QDir(output).entryList({"outside-*"}, QDir::Files).isEmpty());
    evidence_["model_scope_outside_rejected"] = QJsonObject::fromVariantMap(service->plan());
    for (auto it = originalSources.cbegin(); it != originalSources.cend(); ++it) {
      QFile file(it.key()); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), it.value());
    }
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelExportGuiLifecycle() {
    try {
    const auto fixtures = requireFixture("FORMAVIEW_MODEL_EXPORT_FIXTURES");
    if (fixtures.isEmpty()) QSKIP("Set FORMAVIEW_MODEL_EXPORT_FIXTURES to skeletal_tests fbx synthetic fixtures");
    auto *service = qobject_cast<ImageJobs *>(desktop_->imageJobs());
    auto *workspace = item("workspace");
    QVERIFY(service && workspace);
    const auto source = fixtures + "/source.psk";
    desktop_->navigate(fixtures);
    QTRY_VERIFY(desktop_->directory() == fixtures && idle());
    desktop_->filter("", 0, 0);
    QTRY_VERIFY(idle());
    const auto index = static_cast<FileList *>(desktop_->files())->indexOf(source);
    QVERIFY(index >= 0);
    desktop_->select(index);
    QTRY_VERIFY(idle());
    QVERIFY(QMetaObject::invokeMethod(workspace, "openConversion", Q_ARG(QVariant, 0), Q_ARG(QVariant, "")));
    QTRY_VERIFY(item("modelExportContent"));
    const auto cleanup = qScopeGuard([&] {
      if (auto *conversion = window_->findChild<QObject *>("conversionWorkspace"))
        QMetaObject::invokeMethod(conversion, "close");
    });
    const auto edit = [&](const char *name, const QString &text) {
      auto *field = item(name); QVERIFY(field);
      field->forceActiveFocus();
      QVERIFY(QMetaObject::invokeMethod(field, "selectAll"));
      QInputMethodEvent event; event.setCommitString(text);
      QCoreApplication::sendEvent(field, &event);
      QCOMPARE(field->property("text").toString(), text);
    };
    const auto choose = [&](const char *name, int value) {
      auto *choice = item(name); QVERIFY(choice);
      window_->requestActivate();
      QTRY_COMPARE(QGuiApplication::focusWindow(), window_);
      choice->forceActiveFocus();
      QTRY_VERIFY(choice->hasActiveFocus());
      QTest::keyClick(window_, Qt::Key_Home);
      for (int n = 0; n < value; ++n) QTest::keyClick(window_, Qt::Key_Down);
      QCOMPARE(choice->property("currentIndex").toInt(), value);
    };
    const auto click = [&](const char *name) {
      auto *control = item(name); QVERIFY(control && control->isEnabled());
      window_->requestActivate();
      QTRY_COMPARE(QGuiApplication::focusWindow(), window_);
      QSignalSpy activated(control, SIGNAL(clicked()));
      QVERIFY(activated.isValid());
      control->forceActiveFocus(Qt::TabFocusReason);
      QTRY_VERIFY(control->hasActiveFocus());
      const auto visibleCenter = [&] {
        const auto point = control->mapToScene({control->width()/2, control->height()/2});
        if (!QRectF(0, 0, window_->width(), window_->height()).contains(point)) return false;
        for (auto *ancestor = control->parentItem(); ancestor; ancestor = ancestor->parentItem())
          if (ancestor->clip() && !ancestor->contains(ancestor->mapFromScene(point))) return false;
        return true;
      };
      const auto diagnose = qScopeGuard([&] {
        if (activated.isEmpty()) {
          qWarning() << "Model button input missed" << name << control->mapToScene({0,0})
                     << "visible center" << visibleCenter() << "focus" << control->hasActiveFocus();
          save("model-button-missed-" + QString::fromLatin1(name));
        }
      });
      QTRY_VERIFY_WITH_TIMEOUT(visibleCenter(), 5000);
      // Exercise real keyboard activation after focus-driven scrolling settles.
      // Other conversion flows retain their mouse click coverage.
      QTest::keyClick(window_, Qt::Key_Space);
      QTRY_COMPARE_WITH_TIMEOUT(activated.count(), 1, 2000);
    };
    const auto output = root_ + "/model-export";
    QVERIFY(QDir().mkpath(output));
    edit("conversionOutputRoot", output);
    click("conversionNamingHelp");
    QQuickWindow *help = nullptr;
    QTRY_VERIFY([&] { for (auto *w : QGuiApplication::allWindows()) if (w->objectName() == "conversionNamingWindow") help = qobject_cast<QQuickWindow *>(w); return help != nullptr; }());
    QCOMPARE(help->modality(), Qt::NonModal);
    choose("modelExportContent", 0);
    edit("modelExportPaired", fixtures + "/source.psa");
    QVERIFY(help->isVisible());
    QVERIFY(captureWindow(help).save(root_ + "/model-naming-help.png"));
    help->close();
    click("modelExportInspect");
    QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && !service->modelSources().isEmpty(), 10000);
    QCOMPARE(service->modelSources().front().toMap().value("clips").toList().size(), 2);
    QJsonArray jobs;
    for (int mode = 0; mode < 4; ++mode) {
      if (mode) { auto *steps = item("conversionSteps"); QVERIFY(steps); steps->setProperty("currentIndex", 0); }
      choose("modelExportContent", mode == 0 ? 0 : mode == 2 ? 1 : 2);
      if (mode != 2) choose("modelExportPairing", mode % 2);
      if (mode == 1) {
        choose("modelExportOrganization", 1);
        choose("modelExportAxes", 3);
        choose("modelExportUnit", 2);
        QCOMPARE(item("conversionNamingTemplate")->property("text").toString(), QString("{name}-animations.{ext}"));
      }
      if (mode == 2) edit("conversionNamingTemplate", "{name}-model.{ext}");
      if (mode == 3) {
        choose("modelExportOrganization", 0);
        choose("modelExportClipMode", 1);
        // Under load the synthesized click can occasionally fail to toggle
        // the checkbox (Qt delivery timing). Verify the selection at the QML
        // property level and retry the click when it did not register; the
        // assertion below still requires a registered selection.
        auto *settings = window_->findChild<QQuickItem *>("modelExportSettings");
        QVERIFY(settings);
        // Keyboard activation (focus + Space) instead of a synthesized mouse
        // click: coordinate delivery can miss the delegate under load, while
        // keyboard selection is an equivalent real user path.
        for (int attempt = 0; attempt < 3 &&
                            settings->property("selectedClips").toStringList().isEmpty();
             ++attempt) {
          QTRY_VERIFY_WITH_TIMEOUT(item("modelExportClip") && item("modelExportClip")->isEnabled(), 5000);
          auto *clip = item("modelExportClip");
          clip->forceActiveFocus();
          QTest::qWait(60);
          QTest::keyClick(window_, Qt::Key_Space);
          QTRY_VERIFY_WITH_TIMEOUT(
              !settings->property("selectedClips").toStringList().isEmpty(), 2000);
        }
        QVERIFY2(!settings->property("selectedClips").toStringList().isEmpty(),
                 "selected-clip toggle did not register");
        edit("conversionNamingTemplate", "{name}-selected-{clip}.{ext}");
      }
      if (mode == 1) {
        const auto originalSize = window_->size();
        window_->resize(window_->minimumWidth(), window_->minimumHeight());
        for (const auto &language : {QString("zh-CN"), QString("en"), QString("zh-TW")}) {
          desktop_->setLanguage(language);
          QTest::qWait(80);
          QCOMPARE(item("modelExportContent")->property("currentIndex").toInt(), 2);
          QCOMPARE(item("modelExportOrganization")->property("currentIndex").toInt(), 1);
          QCOMPARE(item("modelExportAxes")->property("currentIndex").toInt(), 3);
          QCOMPARE(item("modelExportUnit")->property("currentIndex").toInt(), 2);
          QCOMPARE(item("conversionCategory")->property("currentIndex").toInt(), 1);
          save("model-narrow-" + language);
        }
        desktop_->setLanguage("zh-CN");
        window_->resize(originalSize);
        QTest::qWait(80);
      }
      save("model-settings-" + QString::number(mode));
      click("createImagePlan");
      QTRY_VERIFY2_WITH_TIMEOUT(!service->busy() && !service->job().value("id").toString().isEmpty() &&
          !(QStringList{"queued", "running", "planning"}).contains(service->job().value("state").toString()), qPrintable(service->error()), 30000);
      QCOMPARE(item("conversionSteps")->property("currentIndex").toInt(), 2);
      const auto items = service->job().value("items").toList();
      save("model-direct-" + QString::number(mode));
      QCOMPARE(items.size(), mode == 0 ? 2 : 1);
      for (const auto &v : items) {
        const auto entry = v.toMap();
        const auto request = entry.value("request").toMap();
        if (mode != 2) QCOMPARE(request.value("options").toMap().value("pairing_policy").toString(),
                               mode % 2 ? QString("reference-skeleton") : QString("strict-records"));
        if (entry.value("status") == "failed") QVERIFY(!QFile::exists(request.value("output").toString()));
        const auto preflight = entry.value("preflight").toMap();
        QCOMPARE(preflight.value("clips").toList().size(), mode == 2 ? 0 : mode == 1 ? 2 : 1);
        QCOMPARE(preflight.value("root_motion").toString(), QString("source"));
      }
      if (service->job().value("state") != "succeeded") {
        click("retryImageJob");
        QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && service->plan().value("state") == "ready", 20000);
        if (auto *ack = item("acknowledgeImageLosses")) {
          if (!ack->property("checked").toBool()) click("acknowledgeImageLosses");
        }
        click("submitImagePlan");
      }
      QTRY_VERIFY2_WITH_TIMEOUT(!service->busy() && service->job().value("state") == "succeeded", qPrintable(service->error()), 20000);
      for (const auto &v : service->job().value("items").toList()) {
        const auto entry = v.toMap();
        const auto result = entry.value("result").toMap();
        const auto file = result.value("committed_output", result.value("output")).toString();
        const auto doc = formaview::skeletal::FbxDocument::read(std::filesystem::path(file.toStdWString()));
        QCOMPARE(doc.data().clips.size(), mode == 2 ? 0u : mode == 1 ? 2u : 1u);
        QCOMPARE(doc.data().meshes.empty(), mode == 1 || mode == 3);
        if (mode == 1) {
          QCOMPARE(doc.data().metres_per_unit, 1.0);
          QVERIFY(doc.data().clips[0].source.original_name != doc.data().clips[1].source.original_name);
          for (size_t c = 0; c < doc.data().clips.size(); ++c) {
            const auto &clip = doc.data().clips[c];
            QCOMPARE(clip.start, 0.0);
            QVERIFY(clip.end > clip.start);
            QVERIFY(!doc.evaluate(c, (clip.start + clip.end)/2).world.empty());
          }
        }
      }
      jobs.append(QJsonObject::fromVariantMap(service->job()));
    }
    evidence_["model_export_gui"] = jobs;
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
#endif

  void exportFolderPicker() {
    try {
    const auto folder = root_ + "/输出 空格 #100%";
    QVERIFY(QDir().mkpath(folder));
    const auto expected = QUrl::fromLocalFile(folder);
#ifdef Q_OS_WIN
    const auto nativeDialog = []() -> HWND {
      HWND result = nullptr;
      EnumWindows([](HWND handle, LPARAM context) -> BOOL {
        DWORD process = 0;
        GetWindowThreadProcessId(handle, &process);
        wchar_t name[32]{};
        if (process == GetCurrentProcessId() && IsWindowVisible(handle) &&
            GetClassNameW(handle, name, 32) && QString::fromWCharArray(name) == "#32770") {
          *reinterpret_cast<HWND *>(context) = handle;
          return FALSE;
        }
        return TRUE;
      }, reinterpret_cast<LPARAM>(&result));
      return result;
    };
#endif
    QCOMPARE(desktop_->existingFolderUrl(folder), expected);
    QCOMPARE(desktop_->existingFolderUrl(folder + "/未创建/子目录"), expected);
    QCOMPARE(desktop_->existingFolderUrl(QDir::toNativeSeparators(folder)), expected);
    QCOMPARE(desktop_->existingFolderUrl(expected.toString()), expected);
    for (const auto &drive : QDir::drives())
      QCOMPARE(desktop_->existingFolderUrl(drive.absoluteFilePath()),
               QUrl::fromLocalFile(drive.isDir() ? drive.absoluteFilePath() : QDir::homePath()));
    auto *workspace = item("workspace"); QVERIFY(workspace);
    desktop_->openPath(small_ + "/唯一图片.png");
    QTRY_COMPARE(desktop_->asset().value("path").toString(), small_ + "/唯一图片.png");
    for (const auto mode : {0, 1, 2}) {
      if (qEnvironmentVariableIsSet("FORMAVIEW_FOLDER_NATIVE_CAPTURE") &&
          mode != qEnvironmentVariableIntValue("FORMAVIEW_FOLDER_NATIVE_MODE")) continue;
      QObject *container = window_;
      QQuickWindow *quick = nullptr;
      if (mode) {
        if (mode == 2) {
          const auto animation = requireFixture("FORMAVIEW_MODEL_EXPORT_FIXTURES") + "/source.psa";
          desktop_->openPath(animation);
          QTRY_COMPARE(desktop_->asset().value("path").toString(), animation);
        }
        QVERIFY(QMetaObject::invokeMethod(workspace, "openQuickExport", Q_ARG(QVariant, mode == 2 ? "animation" : "texture")));
        QTRY_VERIFY(([&] {
          for (auto *w : QGuiApplication::topLevelWindows())
            if (w->objectName() == "quickExportWindow" && w->isVisible()) {
              quick = qobject_cast<QQuickWindow *>(w); return true;
            }
          return false;
        })());
        container = quick;
      } else {
        QVERIFY(QMetaObject::invokeMethod(workspace, "openConversion", Q_ARG(QVariant, 0), Q_ARG(QVariant, "")));
      }
      auto *field = container->findChild<QObject *>("conversionOutputRoot"); QVERIFY(field);
      auto *button = container->findChild<QObject *>("chooseConversionOutput"); QVERIFY(button);
      auto *dialog = container->findChild<QObject *>("conversionOutputDialog"); QVERIFY(dialog);
      const auto cleanup = qScopeGuard([&] {
        QMetaObject::invokeMethod(dialog, "reject");
        if (quick) quick->close();
        else if (auto *conversion = window_->findChild<QObject *>("conversionWorkspace"))
          QMetaObject::invokeMethod(conversion, "close");
      });
      for (const auto &entered : {folder, folder + "/尚未创建/结果"}) {
        QVERIFY(field->setProperty("text", entered));
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        QTRY_VERIFY(dialog->property("visible").toBool());
#ifdef Q_OS_WIN
        // QML visible precedes Qt's deferred native-dialog thread startup.
        // Reject only after the shell window exists, as a real user would.
        QTRY_VERIFY_WITH_TIMEOUT(nativeDialog() != nullptr, 10000);
        const auto nativeHandle = nativeDialog();
        QVERIFY(nativeHandle);
#endif
        QCOMPARE(dialog->property("currentFolder").toUrl(), expected);
        if (entered == folder && qEnvironmentVariableIsSet("FORMAVIEW_FOLDER_NATIVE_CAPTURE")) {
          const auto marker = root_ + "/native-" + QString::number(mode);
          QFile ready(marker + ".ready"); QVERIFY(ready.open(QIODevice::WriteOnly)); ready.write(folder.toUtf8()); ready.close();
          QElapsedTimer wait; wait.start();
          while (!QFileInfo::exists(marker + ".continue") && wait.elapsed() < 60000) QTest::qWait(100);
          QVERIFY2(QFileInfo::exists(marker + ".continue"), "Native dialog observation must complete before resuming");
          QVERIFY(QMetaObject::invokeMethod(dialog, "reject"));
          QTRY_VERIFY(!dialog->property("visible").toBool());
          QCOMPARE(field->property("text").toString(), entered);
#ifdef Q_OS_WIN
          QTRY_VERIFY_WITH_TIMEOUT(!IsWindow(nativeHandle), 10000);
#endif
          break;
        }
        QVERIFY(QMetaObject::invokeMethod(dialog, "reject"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
#ifdef Q_OS_WIN
        QTRY_VERIFY_WITH_TIMEOUT(!IsWindow(nativeHandle), 10000);
#endif
        QCOMPARE(field->property("text").toString(), entered);
      }
    }
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }

#ifdef FORMAVIEW_MODEL_EXPORT
  void modelDirectExportBatch() {
    try {
    auto *service = qobject_cast<ImageJobs *>(desktop_->imageJobs()); QVERIFY(service);
    const auto originalFixtures = requireFixture("FORMAVIEW_MODEL_EXPORT_FIXTURES"); QVERIFY(!originalFixtures.isEmpty());
    const auto fixtures = root_ + "/lossless-fixtures"; QVERIFY(QDir().mkpath(fixtures));
    // Existing synthetic fixtures intentionally contain an unsupported FUTURE
    // chunk to test loss confirmation. This benchmark uses lossless inputs.
    for (const auto &extension : {QString("psk"), QString("psa")}) {
      QFile input(originalFixtures + "/source." + extension); QVERIFY(input.open(QIODevice::ReadOnly));
      const auto data = input.readAll(); QByteArray clean;
      for (qsizetype offset = 0; offset < data.size();) {
        QVERIFY(data.size() - offset >= 32);
        qint32 size{}, count{};
        std::memcpy(&size, data.constData() + offset + 24, 4);
        std::memcpy(&count, data.constData() + offset + 28, 4);
        QVERIFY(size >= 0 && count >= 0);
        const auto length = 32 + qint64(size) * count; QVERIFY(length <= data.size() - offset);
        if (!data.mid(offset, 20).startsWith("FUTURE")) clean.append(data.mid(offset, length));
        offset += length;
      }
      QFile destination(fixtures + "/source." + extension); QVERIFY(destination.open(QIODevice::WriteOnly));
      QCOMPARE(destination.write(clean), clean.size());
    }
    const auto output = root_ + "/direct-55"; QVERIFY(QDir().mkpath(output));
    QStringList inputs;
    for (int i = 0; i < 27; ++i) inputs.append(fixtures + "/source.psa");
    inputs.insert(1, fixtures + "/missing-animation.psa");
    const QVariantMap scope{{"input_roots", QStringList{fixtures}}, {"output_roots", QStringList{output}}, {"allow_overwrite", false}};
    const QVariantMap options{{"target", "fbx"}, {"export_content", "animation"},
      {"file_organization", "per-clip"}, {"paired_file", fixtures + "/source.psk"}, {"pairing_policy", "reference-skeleton"}};
    QElapsedTimer elapsed; elapsed.start();
    service->startExport(inputs, scope, options, "{index}-{clip}.{ext}", "skip", {{"name", "Synthetic 55 outputs"}});
    QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && !service->job().value("id").toString().isEmpty(), 30000);
    const auto preparedMs = elapsed.elapsed();
    QTRY_VERIFY_WITH_TIMEOUT(!QStringList({"queued", "running", "planning"}).contains(service->job().value("state").toString()), 90000);
    QCOMPARE(service->job().value("state").toString(), QString("partially_succeeded"));
    const auto job = service->job();
    const auto items = job.value("items").toList(); QCOMPARE(items.size(), 55);
    int succeeded = 0, failed = 0;
    for (const auto &value : items) {
      const auto item = value.toMap();
      if (item.value("status") == "failed") {
        ++failed; QVERIFY(item.value("request").toMap().value("input").toString().endsWith("missing-animation.psa"));
        QVERIFY(!item.value("error").toMap().value("message").toString().isEmpty());
      } else {
        QCOMPARE(item.value("status").toString(), QString("succeeded")); ++succeeded;
        const auto path = item.value("result").toMap().value("committed_output").toString();
        const auto doc = formaview::skeletal::FbxDocument::read(std::filesystem::path(path.toStdWString()));
        QVERIFY(doc.data().meshes.empty()); QCOMPARE(doc.data().clips.size(), size_t(1));
        QVERIFY(doc.data().clips.front().end > doc.data().clips.front().start);
      }
    }
    QCOMPARE(succeeded, 54); QCOMPARE(failed, 1);
    const auto timing = job.value("timings").toMap();
    QVERIFY(timing.contains("first_output_ms"));
    QVERIFY(timing.value("first_output_ms").toLongLong() < timing.value("last_check_ms").toLongLong());
    evidence_["direct_55"] = QJsonObject::fromVariantMap(job);
    evidence_["direct_55_prepared_ms"] = preparedMs;
    evidence_["direct_55_total_ms"] = elapsed.elapsed();
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
#endif

  void quickExportWorkflow() {
    try {
    auto *service = qobject_cast<ImageJobs *>(desktop_->imageJobs());
    auto *workspace = item("workspace");
    QVERIFY(service && workspace);
    const auto findWindow = []() -> QQuickWindow * {
      for (auto *candidate : QGuiApplication::topLevelWindows())
        if (candidate->objectName() == "quickExportWindow" && candidate->isVisible())
          return qobject_cast<QQuickWindow *>(candidate);
      return nullptr;
    };
    const auto source = small_ + "/唯一图片.png";
    desktop_->openPath(source);
    QTRY_COMPARE(desktop_->asset().value("path").toString(), source);
    auto *shortcut = window_->findChild<QObject *>("quickTextureExport"); QVERIFY(shortcut);
    QVERIFY(QMetaObject::invokeMethod(shortcut, "triggered"));
    QTRY_VERIFY(findWindow());
    auto *quick = findWindow();
    const auto cleanup = qScopeGuard([&] { if (auto *w = findWindow()) w->close(); });
    QCOMPARE(quick->modality(), Qt::NonModal);
    QCOMPARE(workspace->property("currentPage").toString(), QString("assets"));
    QCOMPARE(quick->property("inputPath").toString(), source);
    auto *sourceReadout = quick->findChild<QQuickItem *>("quickExportSource"); QVERIFY(sourceReadout);
    auto *header = quick->findChild<QQuickItem *>("topBar"); QVERIFY(header);
    QTRY_VERIFY(sourceReadout->mapToScene({0, 0}).y() >= header->mapToScene({0, header->height()}).y());
    const auto field = [&](const char *name) { return quick->findChild<QQuickItem *>(name); };
    const auto edit = [&](const char *name, const QString &text) {
      auto *control = field(name); QVERIFY(control);
      QVERIFY(control->setProperty("text", text));
      QVERIFY(QMetaObject::invokeMethod(control, "textEdited"));
    };
    const auto click = [&](const char *name) {
      auto *control = field(name); QVERIFY(control && control->isEnabled());
      QVERIFY(QMetaObject::invokeMethod(control, "clicked"));
    };
    const auto output = root_ + "/quick-export"; QVERIFY(QDir().mkpath(output));
    edit("conversionOutputRoot", output);
    if (!workflowHeadless()) { QTest::qWait(100); QVERIFY(captureWindow(quick).save(root_ + "/quick-texture-settings.png")); }
    // Browsing remains possible and never changes the captured export input.
    desktop_->openPath(fixture_ + "/图片0.png");
    QTRY_COMPARE(desktop_->asset().value("path").toString(), fixture_ + "/图片0.png");
    QCOMPARE(quick->property("inputPath").toString(), source);
    click("createImagePlan");
    QTRY_VERIFY_WITH_TIMEOUT(quick->property("phase") == "job", 20000);
    QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && service->job().value("state") == "succeeded", 20000);
    QCOMPARE(workspace->property("currentPage").toString(), QString("assets"));
    const auto exported = service->job().value("items").toList().front().toMap().value("result").toMap().value("committed_output").toString();
    QCOMPARE(QImage(exported).convertToFormat(QImage::Format_RGBA8888), QImage(source).convertToFormat(QImage::Format_RGBA8888));
    evidence_["quick_texture"] = QJsonObject::fromVariantMap(service->job());
    const auto textureJob = service->job().value("id").toString();
    if (!workflowHeadless()) QVERIFY(captureWindow(quick).save(root_ + "/quick-texture-complete.png"));
    quick->close(); QTRY_VERIFY(!quick->isVisible());
    // Closing during asynchronous preparation cannot submit later in the background.
    const auto jobsBefore = service->jobs().size();
    QVERIFY(QMetaObject::invokeMethod(workspace, "openQuickExport", Q_ARG(QVariant, "texture")));
    QTRY_VERIFY(findWindow()); quick = findWindow();
    edit("conversionOutputRoot", output);
    click("createImagePlan");
    quick->close();
    QTRY_VERIFY_WITH_TIMEOUT(!service->busy(), 20000);
    QTest::qWait(150);
    QCOMPARE(service->jobs().size(), jobsBefore);
    QCOMPARE(workspace->property("currentPage").toString(), QString("assets"));

#ifdef FORMAVIEW_MODEL_EXPORT
    const auto fixtures = requireFixture("FORMAVIEW_MODEL_EXPORT_FIXTURES");
    QVERIFY2(!fixtures.isEmpty(), "Synthetic ActorX fixtures are required for all quick export modes");
    QString inventoryFailedJob;
    for (const auto &mode : {QString("model"), QString("combined"), QString("animation")}) {
      const auto input = fixtures + (mode == "animation" ? "/source.psa" : "/source.psk");
      desktop_->openPath(input);
      QTRY_COMPARE_WITH_TIMEOUT(desktop_->asset().value("path").toString(), input, 20000);
      QVERIFY(QMetaObject::invokeMethod(workspace, "openQuickExport", Q_ARG(QVariant, mode == "animation" ? "animation" : "model")));
      QTRY_VERIFY(findWindow()); quick = findWindow();
      QCOMPARE(quick->modality(), Qt::NonModal);
      const auto destination = output + "/" + mode; QVERIFY(QDir().mkpath(destination));
      edit("conversionOutputRoot", destination);
      auto *content = field("modelExportContent"); QVERIFY(content);
      QCOMPARE(content->property("currentIndex").toInt(), mode == "animation" ? 2 : 1);
      if (mode == "combined") {
        content->setProperty("currentIndex", 0);
        QVERIFY(QMetaObject::invokeMethod(content, "activated", Q_ARG(int, 0)));
        edit("modelExportPaired", fixtures + "/source.psa");
      }
      if (mode == "animation") {
        // Invalid explicit pairing must block here; valid standalone PSA semantics
        // remain the same as the full conversion editor.
        edit("modelExportPaired", fixtures + "/missing-reference.psk");
        click("createImagePlan");
        QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && service->job().value("state") == "failed", 20000);
        QCOMPARE(quick->property("phase").toString(), QString("job"));
        QVERIFY(!service->job().value("items").toList().first().toMap().value("error").toMap().isEmpty());
        auto *jobError = quick->findChild<QObject *>("imageJobError");
        QVERIFY(jobError && !jobError->property("raw").toString().isEmpty());
        inventoryFailedJob = service->job().value("id").toString();
        QVERIFY(!inventoryFailedJob.isEmpty());
        click("quickExportEdit");
        edit("modelExportPaired", fixtures + "/source.psk");
      }
      if (!workflowHeadless()) { QTest::qWait(100); QVERIFY(captureWindow(quick).save(root_ + "/quick-" + mode + "-settings.png")); }
      click("createImagePlan");
      QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && quick->property("phase") == "job" &&
          !(QStringList{"queued", "running", "planning"}).contains(service->job().value("state").toString()), 30000);
      auto losses = false;
      for (const auto &item : service->job().value("items").toList())
        losses |= !item.toMap().value("preflight").toMap().value("losses").toList().isEmpty();
      if (losses) {
        click("retryImageJob");
        QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && service->plan().value("state") == "ready", 20000);
        QCOMPARE(quick->property("phase").toString(), QString("preflight"));
        auto *submit = field("submitImagePlan"); QVERIFY(submit && !submit->isEnabled());
        auto *ack = field("acknowledgeImageLosses"); QVERIFY(ack);
        if (!workflowHeadless()) { QTest::qWait(100); QVERIFY(captureWindow(quick).save(root_ + "/quick-" + mode + "-losses.png")); }
        ack->setProperty("checked", true);
        click("submitImagePlan");
      }
      QTRY_VERIFY_WITH_TIMEOUT(quick->property("phase") == "job", 20000);
      QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && service->job().value("state") == "succeeded", 30000);
      for (const auto &value : service->job().value("items").toList()) {
        const auto path = value.toMap().value("result").toMap().value("committed_output").toString();
        const auto document = formaview::skeletal::FbxDocument::read(std::filesystem::path(path.toStdWString()));
        QCOMPARE(document.data().meshes.empty(), mode == "animation");
        QCOMPARE(document.data().clips.empty(), mode == "model");
      }
      evidence_["quick_" + mode] = QJsonObject::fromVariantMap(service->job());
      const auto ownId = service->job().value("id").toString();
      const auto ownItems = service->job().value("items");
      service->selectJob(textureJob);
      auto *pinned = quick->findChild<QObject *>("quickExportJob"); QVERIFY(pinned);
      QTRY_COMPARE(pinned->property("job").toMap().value("id").toString(), ownId);
      QCOMPARE(pinned->property("job").toMap().value("items"), ownItems);
      QCOMPARE(service->job().value("id").toString(), textureJob);
      QCOMPARE(service->jobSnapshot(ownId).value("items"), ownItems);
      const auto priorError = service->error();
      QVERIFY(service->jobSnapshot("unknown-quick-export-job").isEmpty());
      QCOMPARE(service->error(), priorError);
      {
        ImageJobs restored;
        restored.archivedJobs_.clear();
        restored.archivedJobs_[ownId] = service->jobSnapshot(ownId);
        QCOMPARE(restored.jobSnapshot(ownId).value("items"), ownItems);
        restored.archivedJobs_[ownId] = QVariantMap{{"deleted", true}};
        QVERIFY(restored.jobSnapshot(ownId).isEmpty());
      }
      if (!workflowHeadless()) QVERIFY(captureWindow(quick).save(root_ + "/quick-" + mode + ".png"));
      quick->close(); QTRY_VERIFY(!quick->isVisible());
    }
    // Close immediately after Export while preparation is still on the worker.
    // The captured submission must survive its nonmodal result window.
    const auto countBeforeClose = service->jobs().size();
    QVERIFY(QMetaObject::invokeMethod(workspace, "openQuickExport", Q_ARG(QVariant, "animation")));
    QTRY_VERIFY(findWindow()); quick = findWindow();
    const auto detachedOutput = output + "/detached"; QVERIFY(QDir().mkpath(detachedOutput));
    edit("conversionOutputRoot", detachedOutput);
    edit("modelExportPaired", fixtures + "/source.psk");
    click("createImagePlan");
    QCOMPARE(quick->property("phase").toString(), QString("direct"));
    quick->close(); QTRY_VERIFY(!quick->isVisible());
    QElapsedTimer detachedWait; detachedWait.start();
    const auto detachedDone = [&] { return !service->busy() && service->jobs().size() == countBeforeClose + 1 &&
        !QStringList({"queued", "running", "planning"}).contains(service->job().value("state").toString()); };
    while (!detachedDone() && detachedWait.elapsed() < 30000) QTest::qWait(100);
    QVERIFY2(detachedDone(), qPrintable(QString("busy=%1 jobs=%2 expected=%3 selected=%4 state=%5")
        .arg(service->busy()).arg(service->jobs().size()).arg(countBeforeClose + 1)
        .arg(service->job().value("id").toString()).arg(service->job().value("state").toString())));
    QVERIFY(!service->job().value("cancellation_requested").toBool());
    const auto failedSnapshot = service->jobSnapshot(inventoryFailedJob);
    QCOMPARE(failedSnapshot.value("state").toString(), QString("failed"));
    QVERIFY(!failedSnapshot.value("error").toMap().isEmpty());
    QVERIFY(service->job().value("state") == "succeeded" || service->job().value("state") == "failed");
    QCOMPARE(workspace->property("currentPage").toString(), QString("assets"));
    evidence_["quick_closed_submission"] = QJsonObject::fromVariantMap(service->job());
#endif
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void conversionEditorPlanBinding() {
    auto *service = qobject_cast<ImageJobs *>(desktop_->imageJobs());
    auto *workspace = item("workspace");
    QVERIFY(service && workspace);
    workspace->setProperty("settingsVisible", false);
    workspace->setProperty("tasksVisible", false);
    const auto source = small_ + "/唯一图片.png";
    desktop_->navigate(small_);
    QTRY_VERIFY(desktop_->directory() == small_ && idle());
    desktop_->filter("", 0, 0);
    QTRY_VERIFY(idle());
    const auto index = static_cast<FileList *>(desktop_->files())->indexOf(source);
    QVERIFY(index >= 0);
    desktop_->select(index);
    QTRY_VERIFY(idle());
    QVERIFY(QMetaObject::invokeMethod(workspace, "openConversion", Q_ARG(QVariant, 0), Q_ARG(QVariant, "")));
    const auto close = qScopeGuard([&] {
      if (auto *conversion = window_->findChild<QObject *>("conversionWorkspace"))
        QMetaObject::invokeMethod(conversion, "close");
    });
    QTRY_VERIFY(item("conversionOutputRoot"));
    auto *conversionSettings = window_->findChild<QObject *>("conversionSettings");
    auto *colour = window_->findChild<QObject *>("conversionColour");
    auto *transfer = window_->findChild<QObject *>("conversionTransfer");
    auto *exposure = window_->findChild<QObject *>("conversionExposure");
    QVERIFY(conversionSettings && colour && transfer && exposure);
    colour->setProperty("checked", true);
    transfer->setProperty("currentIndex", 1);
    exposure->setProperty("value", 100);
    colour->setProperty("checked", false);
    QVariant selectedOptions;
    QVERIFY(QMetaObject::invokeMethod(conversionSettings, "options", Q_RETURN_ARG(QVariant, selectedOptions)));
    QCOMPARE(selectedOptions.toMap().value("transfer").toString(), QString("preserve"));
    QCOMPARE(selectedOptions.toMap().value("exposure").toDouble(), 0.0);
    colour->setProperty("checked", true);
    QVERIFY(QMetaObject::invokeMethod(conversionSettings, "options", Q_RETURN_ARG(QVariant, selectedOptions)));
    QCOMPARE(selectedOptions.toMap().value("transfer").toString(), QString("linear_to_srgb"));
    QCOMPARE(selectedOptions.toMap().value("exposure").toDouble(), 1.0);
    colour->setProperty("checked", false);
    // Source validation/import runs on the control worker. The settings page
    // may be visible before its input list and naming example are published.
    QTRY_VERIFY_WITH_TIMEOUT(!service->importing(), 15000);
    QCOMPARE(service->sourcePaths(), QStringList{source});
    QCOMPARE(item("conversionPreflightStatus")->property("value").toString(), QString("待预检"));
    QVERIFY(item("conversionContentSource")->property("value").toString().contains("不套用预览"));
    QCOMPARE(item("conversionNamingExamples")->property("value").toString(), QString("唯一图片-converted.png"));
    const auto examples = service->nameExamples({source}, "{name}-converted.{ext}", "png", {});
    QCOMPARE(examples.value("names").toStringList(), QStringList{"唯一图片-converted.png"});
    QCOMPARE(service->nameExamples({source}, "{name}-{index}.{ext}", "png", {QVariantMap{{"target", "tiff"}}})
                 .value("names").toStringList(), QStringList{"唯一图片-1.tiff"});
    QVERIFY(!service->nameExamples({source}, "{name}_m{mip}.{ext}", "png", {}).value("error").toString().isEmpty());
    QVERIFY(!service->nameExamples({source}, "../{name}.{ext}", "png", {}).value("error").toString().isEmpty());
    const auto firstOutput = root_ + "/editor-plan-a";
    const auto secondOutput = root_ + "/editor-plan-b";
    QVERIFY(QDir().mkpath(firstOutput) && QDir().mkpath(secondOutput));
    auto editOutput = [&](const QString &path) {
      auto *field = item("conversionOutputRoot");
      QVERIFY(field);
      field->forceActiveFocus();
      QVERIFY(QMetaObject::invokeMethod(field, "selectAll"));
      QInputMethodEvent event;
      event.setCommitString(path);
      QCoreApplication::sendEvent(field, &event);
      QCOMPARE(field->property("text").toString(), path);
    };
    const auto click = [&](const char *name) {
      auto *control = item(name);
      QVERIFY(control && control->isEnabled());
      control->forceActiveFocus();
      QTest::qWait(30);
      QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
          control->mapToScene({control->width()/2, control->height()/2}).toPoint());
    };
    editOutput(firstOutput);
    click("createImagePlan");
    QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && service->plan().value("state") == "ready", 15000);
    const auto firstPlan = service->plan();
    const auto firstRequest = firstPlan.value("items").toList().front().toMap().value("request").toMap();
    const auto firstOptions = firstRequest.value("options").toMap();
    QCOMPARE(firstOptions.value("meaning").toString(), QString("data"));
    QCOMPARE(firstOptions.value("transfer").toString(), QString("preserve"));
    QCOMPARE(firstOptions.value("exposure").toDouble(), 0.0);
    QCOMPARE(firstRequest.value("input").toString(), source);
    QCOMPARE(firstRequest.value("output").toString(), firstOutput + "/唯一图片-converted.png");
    QCOMPARE(QFileInfo(firstRequest.value("output").toString()).fileName(), examples.value("names").toStringList().front());
    auto *ack = window_->findChild<QQuickItem *>("acknowledgeImageLosses");
    QVERIFY(ack);
    ack->setProperty("checked", true);
    click("editConversionSettings");
    QTRY_VERIFY(item("conversionOutputRoot"));
    editOutput(secondOutput);
    QTRY_VERIFY(service->plan().isEmpty());
    QVERIFY(!ack->property("checked").toBool());
    const auto jobCount = service->jobs().size();
    service->submit(true);
    QCOMPARE(service->jobs().size(), jobCount);
    click("createImagePlan");
    QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && service->plan().value("state") == "ready", 15000);
    QVERIFY(service->plan().value("id") != firstPlan.value("id"));
    const auto request = service->plan().value("items").toList().front().toMap().value("request").toMap();
    QCOMPARE(request.value("input").toString(), source);
    QCOMPARE(request.value("output").toString(), secondOutput + "/唯一图片-converted.png");
    QVERIFY(!QFile::exists(firstRequest.value("output").toString()));
    QVERIFY(!QFile::exists(request.value("output").toString()));
    click("submitImagePlan");
    QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && service->job().value("state") == "succeeded", 15000);
    QCOMPARE(QImage(request.value("output").toString()).pixelColor(0, 0), QColor(Qt::cyan));
    QVERIFY(!QFile::exists(firstRequest.value("output").toString()));
    evidence_["conversion_editor_binding"] = QJsonObject{{"before", QJsonObject::fromVariantMap(firstRequest)}, {"submitted", QJsonObject::fromVariantMap(request)}};
    // Opening directly on Preflight or Jobs must invalidate a plan whose
    // selected inputs changed, just as opening the Settings page does.
    for (const auto page : {1, 2}) {
      const auto plannedInputs = desktop_->selectedImages();
      service->preflight(plannedInputs,
          {{"input_roots", QStringList{desktop_->directory()}}, {"output_roots", QStringList{secondOutput}}, {"allow_overwrite", false}},
          {{"target", "png"}}, "{name}-changed-selection.{ext}", "skip");
      QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && service->plan().value("state") == "ready", 15000);
      auto *conversion = window_->findChild<QObject *>("conversionWorkspace");
      QVERIFY(conversion);
      QVERIFY(QMetaObject::invokeMethod(conversion, "close"));
      const auto selectedPath = page == 1 ? fixture_ + "/图片0.png" : source;
      desktop_->navigate(QFileInfo(selectedPath).absolutePath());
      QTRY_VERIFY(idle() && desktop_->directory() == QFileInfo(selectedPath).absolutePath());
      const auto selectedIndex = static_cast<FileList *>(desktop_->files())->indexOf(selectedPath);
      QVERIFY(selectedIndex >= 0);
      desktop_->select(selectedIndex);
      QTRY_COMPARE(desktop_->selectedImages(), QStringList{selectedPath});
      QVERIFY(QMetaObject::invokeMethod(workspace, "openConversion", Q_ARG(QVariant, page), Q_ARG(QVariant, "")));
      QTRY_VERIFY_WITH_TIMEOUT(!service->importing(), 15000);
      QCOMPARE(service->sourcePaths(), QStringList{selectedPath});
      QTRY_VERIFY(service->plan().isEmpty());
      QCOMPARE(item("conversionSteps")->property("currentIndex").toInt(), page);
      const auto count = service->jobs().size();
      service->submit(true);
      QCOMPARE(service->jobs().size(), count);
    }
  }

  void imageQueuedPreparationCancel() {
    const auto source = root_ + "/queued-cancel.png";
    QImage image(4, 4, QImage::Format_RGBA8888); image.fill(Qt::red);
    QVERIFY(image.save(source));
    const auto output = root_ + "/queued-cancel-output";
    QVERIFY(QDir().mkpath(output));
    const QVariantMap scope{{"input_roots", QStringList{root_}},
                            {"output_roots", QStringList{output}}};
    for (const bool inspect : {false, true}) {
      ImageJobs jobs;
      std::atomic<bool> entered{}, release{};
      const auto unblock = qScopeGuard([&] { release = true; jobs.control_.shutdown(); });
      formaview::TaskRequest hold;
      hold.work = [&](std::stop_token stop) {
        entered = true;
        while (!release && !stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return formaview::TaskResult{};
      };
      QVERIFY(jobs.control_.submit(std::move(hold)).task);
      QTRY_VERIFY(entered.load());
      if (inspect) jobs.inspectModelInputs({source}, scope, {});
      else jobs.preflight({source}, scope, {{"target", "png"}}, "{name}.{ext}", "skip");
      QVERIFY(jobs.busy());
      QTest::qWait(250); // The preparation must remain observable without a service yet.
      jobs.cancelPreparation();
      QTRY_VERIFY_WITH_TIMEOUT(!jobs.busy(), 5000);
      QVERIFY(!jobs.error().isEmpty());
      QCOMPARE(jobs.control_.stats().queued, std::size_t(0));
    }
    evidence_["queued_preflight_and_inspection_cancel"] = "passed";
  }
  void imageHistoryBeyondTwoPages() {
    const auto journal = root_ + "/history-three-pages";
    QVERIFY(QDir().mkpath(journal));
    for (int i = 0; i < 205; ++i) {
      const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
      QFile f(journal + '/' + id + ".json");
      QVERIFY(f.open(QIODevice::WriteOnly));
      const auto data = QJsonDocument(QJsonObject{{"version", 1}, {"id", id},
          {"kind", "job"}, {"state", "succeeded"}, {"items", QJsonArray{}}}).toJson();
      QCOMPARE(f.write(data), data.size());
    }
    ImageJobs jobs;
    jobs.archivedJobs_.clear();
    jobs.historyPath_ = root_ + "/history-three-pages.json";
    const std::filesystem::path path(journal.toStdWString());
    formaview::AccessScope scope;
    scope.input_roots = {path}; scope.output_roots = {path};
    jobs.service_ = std::make_shared<formaview::imaging::ImageBatchService>(scope, path);
    jobs.poll();
    QCOMPARE(jobs.jobs().size(), 205);
    QCOMPARE(jobs.archivedJobs_.size(), 205);
    QVERIFY(!jobs.activeJobs());
    evidence_["image_history_three_pages"] = "passed";
  }
  void imageHistoryPersistence() {
    const auto path = root_ + "/history-pressure.json";
    const auto read = [](const QString &p) {
      QFile f(p);
      return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object().toVariantMap() : QVariantMap{};
    };
    {
      ImageJobs jobs; jobs.historyPath_ = path;
      std::atomic<bool> entered{}, release{};
      const auto unblock = qScopeGuard([&] { release = true; jobs.control_.shutdown(); });
      formaview::TaskRequest hold;
      hold.work = [&](std::stop_token stop) {
        entered = true;
        while (!release && !stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return formaview::TaskResult{};
      };
      QVERIFY(jobs.control_.submit(std::move(hold)).task);
      QTRY_VERIFY(entered.load());
      for (int i = 0; i < 4; ++i) {
        formaview::TaskRequest filler;
        filler.work = [](std::stop_token) { return formaview::TaskResult{}; };
        QVERIFY(jobs.control_.submit(std::move(filler)).task);
      }
      jobs.archivedJobs_ = {{"batch-one", QVariantMap{{"state", "succeeded"}}}};
      jobs.saveHistory();
      jobs.archivedJobs_["batch-two"] = QVariantMap{{"state", "succeeded"}};
      jobs.saveHistory();
      release = true;
      QTRY_COMPARE_WITH_TIMEOUT(read(path), jobs.archivedJobs_, 10000);
    }
    const auto closingPath = root_ + "/history-close.json";
    {
      auto jobs = std::make_unique<ImageJobs>(); jobs->historyPath_ = closingPath;
      std::atomic<bool> entered{};
      const auto stop = qScopeGuard([&] { if (jobs) jobs->control_.shutdown(); });
      formaview::TaskRequest hold;
      hold.work = [&](std::stop_token token) {
        entered = true;
        while (!token.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return formaview::TaskResult{};
      };
      QVERIFY(jobs->control_.submit(std::move(hold)).task);
      QTRY_VERIFY(entered.load());
      jobs->archivedJobs_ = {{"closed-batch", QVariantMap{{"state", "succeeded"}}}};
      jobs->saveHistory();
      jobs.reset();
      QVERIFY(read(closingPath).contains("closed-batch"));
    }
    {
      ImageJobs jobs; jobs.historyPath_ = root_ + "/history-error.json";
      QVERIFY(QDir().mkpath(jobs.historyPath_)); // A directory cannot be replaced as a file.
      jobs.archivedJobs_ = {{"retry-batch", QVariantMap{{"state", "succeeded"}}}};
      jobs.saveHistory();
      QTRY_VERIFY_WITH_TIMEOUT(!jobs.error().isEmpty(), 10000);
      QVERIFY(QDir().rmdir(jobs.historyPath_));
      jobs.saveHistory();
      QTRY_COMPARE_WITH_TIMEOUT(read(jobs.historyPath_), jobs.archivedJobs_, 10000);
      QTRY_VERIFY(jobs.error().isEmpty());
    }
    evidence_["history_pressure_close_and_error"] = "passed";
  }
  void archivedTaskControls() {
    const auto previous = qgetenv("FORMAVIEW_IMAGE_STATE_ROOT");
    const auto restore = qScopeGuard([&] { qputenv("FORMAVIEW_IMAGE_STATE_ROOT", previous); });
    const auto directory = root_ + "/archived-controls";
    QVERIFY(QDir().mkpath(directory));
    qputenv("FORMAVIEW_IMAGE_STATE_ROOT", directory.toUtf8());
    const auto completed = directory + "/completed.png", pending = directory + "/exact-name.png";
    QVERIFY(QFile::copy(small_ + "/唯一图片.png", completed));
    const auto rawRequest = [&](const QString &output) {
      QVariantList channels;
      for (const auto *channel : {"R", "G", "B", "A"})
        channels.append(QVariantMap{{"source", channel}, {"destination", channel}});
      return QVariantMap{{"input", small_ + "/唯一图片.png"}, {"output", output},
                         {"options", QVariantMap{{"target", "png"}, {"channels", channels}, {"alpha_channel", "A"}, {"source_alpha", "straight"}}}, {"conflict", "skip"}};
    };
    const QVariantMap old{{"id", "old-batch"}, {"name", "恢复测试"}, {"category", "image"},
      {"state", "partially_succeeded"}, {"archived", true},
      {"items", QVariantList{QVariantMap{{"status", "succeeded"}, {"request", rawRequest(completed)}},
                             QVariantMap{{"status", "pending"}, {"request", rawRequest(pending)}}}}};
    QFile history(directory + "/desktop-history.json"); QVERIFY(history.open(QIODevice::WriteOnly));
    history.write(QJsonDocument(QJsonObject{{"old-batch", QJsonObject::fromVariantMap(old)}}).toJson()); history.close();
    QString newId;
    {
      ImageJobs jobs;
      QCOMPARE(jobs.job().value("state").toString(), QString("interrupted"));
      jobs.retry("old-batch");
      QTRY_VERIFY2_WITH_TIMEOUT(!jobs.busy() && jobs.plan().value("state") == "ready", qPrintable(jobs.error()), 15000);
      QCOMPARE(jobs.plan().value("items").toList().size(), 1);
      jobs.submit(true);
      QTRY_VERIFY2_WITH_TIMEOUT(!jobs.busy() && jobs.job().value("state") == "succeeded", qPrintable(jobs.error()), 15000);
      QVERIFY(QFileInfo::exists(pending)); QVERIFY(QFileInfo::exists(completed));
      newId = jobs.job().value("id").toString();
      jobs.removeJob(newId); jobs.removeJob("old-batch");
      QVERIFY(jobs.jobs().isEmpty()); jobs.poll(); QVERIFY(jobs.jobs().isEmpty());
    }
    ImageJobs restored;
    QVERIFY(restored.jobs().isEmpty());
    QVERIFY(QFileInfo::exists(pending)); QVERIFY(QFileInfo::exists(completed));
  }
  void imageJobsCancelAndShutdown() {
    const auto input = root_ + "/shutdown-input";
    QVERIFY(QDir().mkpath(input));
    QImage image(2048, 2048, QImage::Format_RGBA8888);
    image.fill(QColor(73, 127, 201, 255));
    const auto source = input + "/source.png";
    QVERIFY(image.save(source));
    QStringList inputs;
    for (auto i = 0; i < 16; ++i) inputs.append(source);
    QJsonArray observed;
    for (const auto &mode : {QString("cancel"), QString("shutdown")}) {
      const auto output = root_ + "/jobs-" + mode;
      QVERIFY(QDir().mkpath(output));
      const QVariantMap scope{{"input_roots", QStringList{input}},
                              {"output_roots", QStringList{output}},
                              {"allow_overwrite", false}};
      auto jobs = std::make_unique<ImageJobs>();
      jobs->preflight(inputs, scope, {{"target", "png"}, {"png_compression", 9}}, "item-{index}.{ext}", "skip");
      QTRY_VERIFY_WITH_TIMEOUT(!jobs->busy() && jobs->plan().value("state") == "ready", 30000);
      jobs->submit(true);
      QTRY_VERIFY_WITH_TIMEOUT(!jobs->busy() && jobs->job().value("state") == "running", 15000);
      const auto id = jobs->job().value("id").toString();
      QVERIFY(!id.isEmpty());
      if (mode == "cancel") {
        jobs->cancel("job", id);
        QTRY_VERIFY(jobs->job().value("cancellation_requested").toBool());
        QTRY_VERIFY_WITH_TIMEOUT((!QStringList{"queued", "running"}.contains(jobs->job().value("state").toString())), 15000);
        QVERIFY((QStringList{"cancelled", "partially_succeeded"}.contains(jobs->job().value("state").toString())));
        const auto entries = jobs->job().value("items").toList();
        QVERIFY(std::any_of(entries.cbegin(), entries.cend(), [](const auto &entry) { return entry.toMap().value("status") == "cancelled"; }));
      }
      QElapsedTimer closing;
      closing.start();
      jobs.reset();
      const auto closeMs = closing.elapsed();
      ImageJobs recovered;
      recovered.configureScope(scope);
      QTRY_VERIFY_WITH_TIMEOUT(!recovered.busy() && recovered.error().isEmpty(), 15000);
      recovered.selectJob(id);
      QTRY_COMPARE(recovered.job().value("id").toString(), id);
      const auto state = recovered.job().value("state").toString();
      QVERIFY2((QStringList{"cancelled", "partially_succeeded", "interrupted"}.contains(state)), qPrintable(state));
      const auto entries = recovered.job().value("items").toList();
      QVERIFY(std::any_of(entries.cbegin(), entries.cend(), [](const auto &entry) {
        return QStringList{"cancelled", "interrupted"}.contains(entry.toMap().value("status").toString());
      }));
      const auto outputs = QDir(output).entryList(QDir::Files);
      QTest::qWait(200);
      QCOMPARE(QDir(output).entryList(QDir::Files), outputs);
      observed.append(QJsonObject{{"mode", mode}, {"job", QJsonObject::fromVariantMap(recovered.job())},
                                 {"observed_close_ms", closeMs}, {"output_count", outputs.size()}});
    }
    evidence_["cancel_and_normal_shutdown"] = observed;
  }

  void approvedWorkspaceFidelity() {
#ifdef FORMAVIEW_IMAGE_UI
    auto *workspace = item("workspace");
    auto *service = qobject_cast<ImageJobs *>(desktop_->imageJobs());
    QVERIFY(workspace && service);
    const auto restoreWorkspace = qScopeGuard([&] {
      if (auto *conversion = window_->findChild<QObject *>("conversionWorkspace"))
        QMetaObject::invokeMethod(conversion, "close");
      workspace->setProperty("settingsVisible", false);
      workspace->setProperty("tasksVisible", false);
      workspace->setProperty("focusMode", false);
      window_->resize(1600,900);
      desktop_->setLanguage("zh-CN");
    });
    workspace->setProperty("tasksVisible", false);
    workspace->setProperty("conversionVisible", false);
    workspace->setProperty("focusMode", false);
    desktop_->preference("fileWidth", 236);
    desktop_->preference("filesVisible", true);
    desktop_->preference("thumbnails", false);
    desktop_->navigate(small_);
    QTRY_VERIFY(idle());
    desktop_->filter("", 0, 0);
    QTRY_VERIFY(idle());
    const auto source = small_ + "/唯一图片.png";
    const auto index = static_cast<FileList *>(desktop_->files())->indexOf(source);
    QVERIFY(index >= 0);
    desktop_->select(index);
    QTRY_VERIFY(idle());
    desktop_->openPath(source);
    auto *session = qobject_cast<ImageSession *>(desktop_->image());
    QTRY_VERIFY(session->available() && !session->busy());
    auto click = [&](QQuickItem *control) {
      QVERIFY(control && control->isVisible() && control->isEnabled());
      control->forceActiveFocus(Qt::TabFocusReason);
      QTest::qWait(80);
      const auto point = control->mapToScene({control->width()/2, control->height()/2});
      QVERIFY(QRectF(0, 0, window_->width(), window_->height()).contains(point));
      QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, point.toPoint());
      QTest::qWait(60);
    };
    auto clickConvert = [&] {
      window_->requestActivate();
      QTRY_COMPARE(QGuiApplication::focusWindow(), window_);
      // Leaving focus mode makes the navigation rail visible and relayouts
      // it. Wait for a rendered frame before sampling pointer coordinates.
      QSignalSpy rendered(window_, &QQuickWindow::frameSwapped);
      window_->update();
      QTRY_VERIFY_WITH_TIMEOUT(!rendered.isEmpty(), 5000);
      auto *button = item("convertImagesButton");
      QVERIFY(button);
      QSignalSpy activated(button, SIGNAL(clicked()));
      QVERIFY(activated.isValid());
      const auto diagnose = qScopeGuard([&] {
        if (activated.isEmpty()) {
          qWarning() << "Fidelity conversion input missed" << button->mapToScene({0,0})
                     << "active window" << window_->isActive();
          save("fidelity-conversion-input-missed");
        }
      });
      click(button);
      QTRY_COMPARE_WITH_TIMEOUT(activated.count(), 1, 2000);
    };
    auto bounds = [](QQuickItem *control) {
      const auto point = control->mapToScene({0, 0});
      return QJsonObject{{"x",point.x()}, {"y",point.y()}, {"width",control->width()}, {"height",control->height()}};
    };
    QJsonArray frames;
    auto capture = [&](const QString &name) {
      QTest::qWait(100);
      const auto image = captureWindow(window_);
      QVERIFY(image.save(root_ + "/" + name + ".png"));
      QJsonObject frame{{"name", name}, {"logical_width", window_->width()}, {"logical_height", window_->height()}, {"dpr", window_->devicePixelRatio()}, {"physical_width", image.width()}, {"physical_height", image.height()}, {"source", source}, {"font", QGuiApplication::font().family()}};
      for (const auto *region : {"topBar", "statusBar", "filesPane", "previewPane", "inspectorPane", "conversionSources", "conversionContent", "conversionEncoding", "conversionFooter"})
        if (auto *control = item(region)) frame[region] = bounds(control);
      frames.append(frame);
    };
    for (const auto &size : {QSize(1600,900), QSize(1280,800)}) {
      window_->resize(size);
      QTRY_COMPARE(window_->size(), size);
      for (const auto &language : {QString("zh-CN"), QString("en"), QString("zh-TW")}) {
        desktop_->setLanguage(language);
        const auto suffix = QString::number(size.width()) + "-" + language;
        QTest::qWait(100);
        QVERIFY(item("topBar") && item("statusBar"));
        QCOMPARE(qRound(item("topBar")->height()), 60);
        QCOMPARE(qRound(item("statusBar")->height()), 44);
        QCOMPARE(qRound(item("inspectorPane")->width()), size.width() == 1600 ? 312 : 300);
        if (size.width() == 1600) QCOMPARE(qRound(item("filesPane")->width()), 236);
        else QVERIFY(!item("filesPane"));
        QVERIFY(!item("pixelToggle")->property("checked").toBool());
        capture("fidelity-image-" + suffix);
        click(item("applicationMenuButton"));
        click(item("appMenuEntry"));
        capture("fidelity-menu-" + suffix);
        QTest::keyClick(window_, Qt::Key_Escape);
        QTest::keyClick(window_, Qt::Key_Escape);
        if (size.width() == 1280) {
          click(item("fileRail"));
          QTRY_VERIFY(item("address"));
          capture("fidelity-drawer-" + suffix);
          QTest::keyClick(window_, Qt::Key_Escape);
          QTest::keyClick(window_, Qt::Key_Escape);
          QTRY_VERIFY(!item("address"));
        }
        workspace->setProperty("focusMode", true);
        QTest::qWait(80);
        QVERIFY(!item("inspectorPane"));
        capture("fidelity-focus-" + suffix);
        workspace->setProperty("focusMode", false);
        clickConvert();
        QTRY_VERIFY(item("conversionOutputRoot"));
        QVERIFY(!item("previewPane"));
        auto *output = item("conversionOutputRoot");
        const auto destination = root_ + "/fidelity-output-" + suffix;
        QVERIFY(QDir().mkpath(destination));
        output->setProperty("text", destination);
        QTest::qWait(80);
        // Earlier keyboard navigation can leave the settings scrolled. This
        // assertion compares the top-aligned layout, so establish that view.
        auto *settingsScroll = item("conversionSettingsScroll");
        QVERIFY(settingsScroll);
        auto *settingsContent = qvariant_cast<QQuickItem *>(settingsScroll->property("contentItem"));
        QVERIFY(settingsContent);
        settingsContent->setProperty("contentY", 0);
        QTest::qWait(60);
        auto *left = item("conversionSources");
        auto *right = item("conversionSummary");
        QVERIFY(left && right);
        const auto leftPoint = left->mapToScene({0,0});
        const auto rightPoint = right->mapToScene({0,0});
        qInfo() << "fidelity alignment" << leftPoint << rightPoint
                << item("conversionSettingsScroll")->property("contentItem");
        QVERIFY(qAbs(leftPoint.y() - rightPoint.y()) < 2);
        QVERIFY(rightPoint.x() > leftPoint.x() + left->width());
        QVERIFY(rightPoint.x() + right->width() <= size.width() - 20);
        auto *footer = item("conversionFooter");
        QVERIFY(footer);
        QVERIFY(footer->mapToScene({0,footer->height()}).y() < size.height() - 25);
        QVERIFY(!item("conversionQuantize"));
        QVERIFY(item("conversionAlphaAction"));
        capture("fidelity-settings-" + suffix);
        click(item("conversionAdvancedSettings"));
        auto *advanced = item("conversionAdvanced");
        auto *scroll = item("conversionSettingsScroll");
        QVERIFY(advanced && scroll);
        // Narrow windows legitimately scroll the optional section below the footer.
        // Keyboard focus must reveal it before pointer activation is attempted.
        advanced->forceActiveFocus();
        QTRY_VERIFY(advanced->mapToScene({0, advanced->height()}).y() <=
                    scroll->mapToScene({0, scroll->height()}).y() + 1);
        QSignalSpy expandedClick(advanced, SIGNAL(clicked()));
        click(advanced);
        QCOMPARE(expandedClick.count(), 1);
        QTRY_VERIFY(item("conversionAdvanced")->property("checked").toBool());
        auto *lastRule = item("conversionMetadata");
        QVERIFY(lastRule);
        lastRule->forceActiveFocus();
        QTRY_VERIFY(lastRule->mapToScene({0, lastRule->height()}).y() <=
                    scroll->mapToScene({0, scroll->height()}).y() + 1);
        capture("fidelity-rule-focus-" + suffix);
        advanced->forceActiveFocus();
        QTRY_VERIFY(advanced->mapToScene({0, 0}).y() >= scroll->mapToScene({0, 0}).y());
        click(item("conversionAdvanced"));
        item("conversionAdvancedSettings")->setProperty("checked", false);
        auto *scrollContent = qvariant_cast<QQuickItem *>(scroll->property("contentItem"));
        QVERIFY(scrollContent);
        scrollContent->setProperty("contentY", 0);
        QTest::qWait(60);
        auto *target = item("conversionTarget");
        click(target);
        capture("fidelity-choice-" + suffix);
        QTest::keyClick(window_, Qt::Key_Escape);
        QVERIFY(item("conversionOutputRoot"));
        click(item("createImagePlan"));
        QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && service->plan().value("state") == "ready", 15000);
        capture("fidelity-preflight-" + suffix);
        click(item("editConversionSettings"));
        QTRY_VERIFY(item("conversionOutputRoot"));
        // Leaving/re-entering the workspace preserves settings and the current plan.
        QTest::keyClick(window_, Qt::Key_Escape);
        QTRY_VERIFY(item("previewPane"));
        clickConvert();
        QTRY_VERIFY(item("conversionOutputRoot"));
        QCOMPARE(item("conversionOutputRoot")->property("text").toString(), destination);
        QCOMPARE(service->plan().value("state").toString(), QString("ready"));
        auto *field = item("conversionOutputRoot");
        field->forceActiveFocus();
        QTest::keyClick(window_, Qt::Key_End);
        QTest::keyClick(window_, Qt::Key_X);
        QTRY_VERIFY(service->plan().isEmpty());
        QTest::keyClick(window_, Qt::Key_Backspace);
        QCOMPARE(field->property("text").toString(), destination);
        click(item("createImagePlan"));
        QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && service->plan().value("state") == "ready", 15000);
        if (auto *ack = item("acknowledgeImageLosses")) click(ack);
        click(item("submitImagePlan"));
        QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && service->job().value("state") == "succeeded", 15000);
        capture("fidelity-jobs-" + suffix);
        QTest::keyClick(window_, Qt::Key_Escape);
        QTRY_VERIFY(item("previewPane"));
        QVERIFY(item("convertImagesButton")->hasActiveFocus());
      }
    }
    window_->resize(1600,900);
    desktop_->setLanguage("zh-CN");
    evidence_["visual_fidelity"] = frames;
#else
    QSKIP("Image UI is not compiled into this build");
#endif
  }

  void imageConversionGuiLifecycle() {
    try {
    auto *service = qobject_cast<ImageJobs *>(desktop_->imageJobs());
    QVERIFY(service);
    auto *workspace = item("workspace");
    QVERIFY(workspace);
    workspace->setProperty("tasksVisible", false);
    workspace->setProperty("focusMode", false);
    desktop_->navigate(small_);
    QTRY_COMPARE(desktop_->directory(), small_);
    QTRY_VERIFY(idle());
    desktop_->filter("", 0, 0);
    QTRY_VERIFY(idle());
    const auto source = small_ + "/唯一图片.png";
    const auto index =
        static_cast<FileList *>(desktop_->files())->indexOf(source);
    QVERIFY(index >= 0);
    desktop_->select(index);
    QTRY_VERIFY(idle());
    QCOMPARE(desktop_->selectedImages(), QStringList{source});
    const auto output = root_ + "/gui-conversion-output";
    QVERIFY(QDir().mkpath(output));
    QPointer<QQuickItem> open = item("convertImagesButton");
    QVERIFY(open);
    QTest::mouseClick(
        window_, Qt::LeftButton, Qt::NoModifier,
        open->mapToScene(QPointF(open->width() / 2, open->height() / 2))
            .toPoint());
    auto find = [&](const QString &name) -> QQuickItem * {
      const auto list = visualItems(window_->contentItem(), name);
      return list.isEmpty() ? nullptr : list.front();
    };
    QTRY_VERIFY(find("conversionOutputRoot"));
    const auto closePopup =
        qScopeGuard([popup = QPointer<QObject>(
                         window_->findChild<QObject *>("conversionWorkspace"))] {
          if (popup)
            QMetaObject::invokeMethod(popup, "close");
        });
    auto *outputField = find("conversionOutputRoot");
    outputField->forceActiveFocus();
    QMetaObject::invokeMethod(outputField, "selectAll");
    QInputMethodEvent outputCommit;
    outputCommit.setCommitString(output);
    QCoreApplication::sendEvent(outputField, &outputCommit);
    QCOMPARE(outputField->property("text").toString(), output);
    save("conversion-settings-zh-CN");
    auto *create = find("createImagePlan");
    QVERIFY(create);
    QVERIFY(create->isEnabled());
    QTest::mouseClick(
        window_, Qt::LeftButton, Qt::NoModifier,
        create->mapToScene(QPointF(create->width() / 2, create->height() / 2))
            .toPoint());
    QTRY_VERIFY_WITH_TIMEOUT(
        !service->busy() &&
            !service->plan().value("state").toString().isEmpty() &&
            service->plan().value("state") != "planning",
        15000);
    QVERIFY2(service->plan().value("state") == "ready",
             qPrintable(QString::fromUtf8(
                 QJsonDocument(QJsonObject::fromVariantMap(service->plan()))
                     .toJson())));
    const auto target = output + "/唯一图片-converted.png";
    QVERIFY(!QFile::exists(target));
    const auto planId = service->plan()["id"].toString();
    QVERIFY(!planId.isEmpty());
    save("conversion-preflight-zh-CN");
    if (auto *ack = find("acknowledgeImageLosses"))
      QTest::mouseClick(
          window_, Qt::LeftButton, Qt::NoModifier,
          ack->mapToScene(QPointF(12, ack->height() / 2)).toPoint());
    auto *submit = find("submitImagePlan");
    QVERIFY(submit);
    QVERIFY(submit->isEnabled());
    QTest::mouseClick(
        window_, Qt::LeftButton, Qt::NoModifier,
        submit->mapToScene(QPointF(submit->width() / 2, submit->height() / 2))
            .toPoint());
    QTRY_VERIFY_WITH_TIMEOUT(!service->busy() &&
                                 service->job().value("state") == "succeeded",
                             15000);
    QVERIFY(QFile::exists(target));
    const auto decoded = QImage(target);
    QCOMPARE(decoded.size(), QSize(64, 64));
    QCOMPARE(decoded.pixelColor(0, 0), QColor(Qt::cyan));
    const auto jobId = service->job()["id"].toString();
    const auto losses = service->plan()["items"]
                            .toList()[0]
                            .toMap()["preflight"]
                            .toMap()["losses"]
                            .toList();
    service->submit(!losses.isEmpty());
    QTRY_VERIFY(!service->busy());
    QVERIFY2(service->error().isEmpty(), qPrintable(service->error()));
    QCOMPARE(service->job()["id"].toString(), jobId);
    for (const auto &language :
         {QString("zh-CN"), QString("en"), QString("zh-TW")}) {
      desktop_->setLanguage(language);
      QTest::qWait(60);
      save("conversion-job-" + language);
    }
    desktop_->setLanguage("zh-CN");
    auto *acknowledgement =
        window_->findChild<QQuickItem *>("acknowledgeImageLosses");
    QVERIFY(acknowledgement);
    acknowledgement->setProperty("checked", true);
    service->invalidatePlan();
    QTRY_VERIFY(!acknowledgement->property("checked").toBool());
    QTest::keyClick(window_, Qt::Key_Escape);
    QTRY_VERIFY(!find("conversionOutputRoot"));
    QVERIFY(open);
    QCOMPARE(item("convertImagesButton"), open.data());
    QTRY_VERIFY(open->hasActiveFocus());
    // A changed setting invalidates approval before any new submission.
    service->invalidatePlan();
    QVERIFY(service->plan().isEmpty());
    const auto oldJobs = service->jobs().size();
    service->submit(true);
    QCOMPARE(service->jobs().size(), oldJobs);
    const auto scope = QVariantMap{{"input_roots", QStringList{small_}},
                                   {"output_roots", QStringList{output}},
                                   {"allow_overwrite", false}};
    const auto options = QVariantMap{{"target", "png"}};
    service->preflight({source}, scope, options, "{name}-converted.{ext}",
                       "skip");
    QTRY_VERIFY_WITH_TIMEOUT(!service->busy(), 15000);
    QVERIFY2(service->error().isEmpty(), qPrintable(service->error()));
    QTRY_VERIFY_WITH_TIMEOUT(service->plan().value("state") != "planning",
                             15000);
    QVERIFY2(service->plan().value("state") == "ready",
             qPrintable(QString::fromUtf8(
                 QJsonDocument(QJsonObject::fromVariantMap(service->plan()))
                     .toJson())));
    service->submit(true);
    QTRY_VERIFY2_WITH_TIMEOUT(!service->busy() &&
                                 service->job().value("plan_id") == service->plan().value("id") &&
                                 service->job().value("state") == "succeeded",
                             qPrintable(service->error()), 15000);
    QCOMPARE(service->job()["items"].toList()[0].toMap()["status"].toString(),
             QString("skipped"));
    service->preflight({source}, scope, options, "../escape.{ext}", "skip");
    QTRY_VERIFY(!service->busy());
    QVERIFY(!service->error().isEmpty());
    QVERIFY(service->plan().isEmpty());
    service->preflight({source}, scope, options, "cancelled.{ext}", "skip");
    service->cancelPreparation();
    QTRY_VERIFY_WITH_TIMEOUT(!service->busy(), 15000);
    QVERIFY(!QFile::exists(output + "/cancelled.png"));
    const auto python = requireFixture("FORMAVIEW_TEST_MCP_PYTHON");
    const auto adapter = requireFixture("FORMAVIEW_TEST_MCP_ADAPTER");
    const auto client = requireFixture("FORMAVIEW_TEST_MCP_CLIENT");
    QVERIFY2(QFile::exists(python) && QFile::exists(adapter) &&
                 QFile::exists(client),
             "Explicit MCP verification runtime and client paths required");
    QVERIFY(!service->mcpEnabled());
    service->setMcpEnabled(true);
    const auto disableEndpoint = qScopeGuard([&] { service->setMcpEnabled(false); });
    QVERIFY(service->mcpEnabled());
    QVERIFY(service->lastClient().isEmpty());
    // The host is idle before the external client starts. Its task list must
    // still observe new MCP jobs without a user selection forcing poll().
    QTest::qWait(250);
    const auto configuration = QJsonDocument::fromJson(
        service->mcpConfiguration(python, adapter).toUtf8());
    QCOMPARE(configuration.object()["mcpServers"]
                 .toObject()["formaview-images"]
                 .toObject()["command"]
                 .toString(),
             python);
    QProcess process;
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) {
          args->flags |= CREATE_NO_WINDOW;
        });
#endif
    const auto report = root_ + "/gui-mcp-report.json";
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(python,
                  {client, "--endpoint", service->endpoint(), "--adapter",
                   adapter, "--source", source, "--output",
                   output + "/from-mcp.png", "--existing-job",
                   service->job()["id"].toString(), "--report", report});
    QTRY_VERIFY_WITH_TIMEOUT(process.state() == QProcess::NotRunning, 20000);
    QVERIFY2(process.exitCode() == 0, process.readAll().constData());
    QFile result(report);
    QVERIFY(result.open(QIODevice::ReadOnly));
    const auto mcpResult = QJsonDocument::fromJson(result.readAll()).object();
    QCOMPARE(mcpResult["capabilities"].toObject().toVariantMap(),
             service->capabilities());
    QCOMPARE(service->lastClient()["name"].toString(),
             QString("FormaView GUI integration"));
    QCOMPARE(service->lastClient()["protocol"].toString(),
             mcpResult["protocol"].toString());
    const auto externalJobVisible = [&] {
      const auto jobs = service->jobs();
      return std::any_of(jobs.cbegin(), jobs.cend(), [&](const QVariant &row) {
        return row.toMap().value("id").toString() ==
               mcpResult["job_id"].toString();
      });
    };
    QTRY_VERIFY_WITH_TIMEOUT(externalJobVisible(), 3000);
    service->selectJob(mcpResult["job_id"].toString());
    QCOMPARE(service->job()["state"].toString(), QString("succeeded"));
    QCOMPARE(QImage(output + "/from-mcp.png").pixelColor(0, 0),
             QColor(Qt::cyan));
    service->setMcpEnabled(false);
    QVERIFY(!service->mcpEnabled());
    service->setMcpEnabled(true);
    QVERIFY(service->mcpEnabled());
    QTest::qWait(250);
    const auto pipeRequest = [&](QJsonObject request, QJsonObject &reply) {
      request["version"] = 1;
      QLocalSocket pipe;
      pipe.connectToServer(service->endpoint());
      QTRY_COMPARE(pipe.state(), QLocalSocket::ConnectedState);
      pipe.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
      QTRY_VERIFY_WITH_TIMEOUT(pipe.canReadLine(), 5000);
      reply = QJsonDocument::fromJson(pipe.readLine()).object();
      QVERIFY2(!reply.contains("error"), qPrintable(QString::fromUtf8(QJsonDocument(reply).toJson())));
    };
    QJsonObject externalPlan;
    QJsonArray externalChannels;
    for (const auto *channel : {"R", "G", "B", "A"})
      externalChannels.append(QJsonObject{{"source", channel}, {"destination", channel}});
    pipeRequest({{"operation","create_plan"},{"items",QJsonArray{QJsonObject{
        {"input",source},{"output",output + "/without-client-metadata.png"},{"conflict","skip"},
        {"options",QJsonObject{{"target","png"},{"channels",externalChannels},
                              {"source_alpha","straight"},{"alpha_channel","A"}}}}}}}, externalPlan);
    QVERIFY(externalPlan.contains("plan_id"));
    QJsonObject externalState;
    for (int attempt = 0; attempt < 100; ++attempt) {
      pipeRequest({{"operation","get_plan"},{"id",externalPlan["plan_id"]}}, externalState);
      if (externalState["state"] == "ready") break;
      QTest::qWait(20);
    }
    QCOMPARE(externalState["state"].toString(), QString("ready"));
    QJsonObject externalJob;
    pipeRequest({{"operation","submit"},{"plan_id",externalPlan["plan_id"]},
        {"approval_digest",externalState["approval_digest"]},
        {"idempotency_key","gui-no-client-metadata"},{"acknowledge_losses",true}}, externalJob);
    QVERIFY(externalJob.contains("job_id"));
    QTRY_VERIFY_WITH_TIMEOUT([&] {
      const auto rows = service->jobs();
      return std::any_of(rows.cbegin(), rows.cend(), [&](const QVariant &row) {
        const auto value = row.toMap();
        return value["id"] == externalJob["job_id"].toString() && value["state"] == "succeeded";
      });
    }(), 5000);
    QVERIFY(service->lastClient().isEmpty());
    QCOMPARE(QImage(output + "/without-client-metadata.png").pixelColor(0,0), QColor(Qt::cyan));
    service->setMcpEnabled(false);
    service->preflight({source, small_ + "/unreadable.png"}, scope,
                       {{"target", "png"}}, "read-error-{index}.{ext}", "skip");
    QTRY_VERIFY_WITH_TIMEOUT(!service->busy() &&
                                 service->plan().value("state") != "planning",
                             15000);
    QVERIFY2(service->error().isEmpty(), qPrintable(service->error()));
    QCOMPARE(service->plan()["items"].toList().size(), 2);
    QCOMPARE(service->plan()["items"].toList()[0].toMap()["status"].toString(),
             QString("ready"));
    QCOMPARE(service->plan()["items"].toList()[1].toMap()["status"].toString(),
             QString("failed"));
    const auto layered = small_ + "/actual-layers.exr";
    // This case also runs independently of imageStructuresAndLayers.
    formaview::ImageSubresource layerSource;
    layerSource.data_window = layerSource.display_window = {0, 0, 1, 1};
    for (const auto &[name, value] : std::array<std::pair<const char *, float>, 7>{{
        {"beauty.R", .125F}, {"beauty.G", .25F}, {"beauty.B", .5F}, {"beauty.A", .75F},
        {"utility.Z", 1.5F}, {"mono.Y", .25F}, {"mono.A", .5F}}}) {
      formaview::ImageChannel channel; channel.name = name; channel.samples.scalar = formaview::ScalarType::Float32;
      channel.samples.element_count = 1; channel.samples.bytes.resize(sizeof(value));
      std::memcpy(channel.samples.bytes.data(), &value, sizeof(value));
      layerSource.channels.push_back(std::move(channel));
    }
    const auto layerBytes = formaview::imaging::encode_professional(layerSource, {});
    QFile layerFile(layered); QVERIFY(layerFile.open(QIODevice::WriteOnly));
    QCOMPARE(layerFile.write(reinterpret_cast<const char *>(layerBytes.data()), static_cast<qint64>(layerBytes.size())),
             static_cast<qint64>(layerBytes.size()));
    layerFile.close();
    service->preflight({source, layered}, scope, {{"target", "png"}},
                       "mapped-{index}.{ext}", "skip");
    QTRY_VERIFY_WITH_TIMEOUT(!service->busy() &&
                                 service->plan().value("state") != "planning",
                             15000);
    QCOMPARE(service->inputInfo().size(), 2);
    QCOMPARE(service->inputInfo()[1].toMap()["channels"].toList().size(), 7);
    QCOMPARE(service->plan()["items"].toList()[1].toMap()["status"].toString(),
             QString("failed"));
    QVariantList channels;
    for (const auto *channel : {"R", "G", "B", "A"})
      channels.append(QVariantMap{{"source", QString("beauty.") + channel},
                                  {"destination", channel}});
    const QVariantList overrides{QVariantMap{},
                                 QVariantMap{{"channels", channels},
                                             {"output_scalar", "uint8"},
                                             {"quantization", "clamp_round"},
                                             {"non_finite", "reject"},
                                             {"source_alpha", "straight"},
                                             {"alpha_channel", "beauty.A"}}};
    service->preflight({source, layered}, scope, {{"target", "png"}},
                       "mapped-{index}.{ext}", "skip", overrides);
    QTRY_VERIFY_WITH_TIMEOUT(!service->busy() &&
                                 service->plan().value("state") != "planning",
                             15000);
    QVERIFY2(service->plan().value("state") == "ready",
             qPrintable(QString::fromUtf8(
                 QJsonDocument::fromVariant(service->plan()).toJson())));
    service->submit(true);
    QTRY_VERIFY_WITH_TIMEOUT(!service->busy() &&
                                 service->job().value("state") == "succeeded",
                             15000);
    QCOMPARE(QImage(output + "/mapped-1.png").pixelColor(0, 0),
             QColor(Qt::cyan));
    QCOMPARE(QImage(output + "/mapped-2.png").pixelColor(0, 0),
             QColor(32, 64, 128, 191));
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
#endif
  void imageGuiWriteMatrix() {
#ifdef FORMAVIEW_IMAGE_UI
    auto *service = qobject_cast<ImageJobs *>(desktop_->imageJobs());
    QVERIFY(service);
    const auto output = root_ + "/gui-write-matrix";
    QVERIFY(QDir().mkpath(output));
    const auto source = small_ + "/唯一图片.png";
    desktop_->navigate(small_);
    QTRY_VERIFY(desktop_->directory() == small_ && idle());
    const auto sourceIndex =
        static_cast<FileList *>(desktop_->files())->indexOf(source);
    QVERIFY(sourceIndex >= 0);
    desktop_->select(sourceIndex);
    const QStringList targets{"png",  "jpeg", "bmp", "tga", "tiff",
                              "webp", "dds",  "hdr", "exr"};
    QJsonArray results;
    for (int i = 0; i < targets.size(); ++i) {
      const auto target = targets[i];
      auto *open = item("convertImagesButton");
      QVERIFY(open && open->isEnabled());
      QTest::mouseClick(
          window_, Qt::LeftButton, Qt::NoModifier,
          open->mapToScene({open->width() / 2, open->height() / 2}).toPoint());
      QTRY_VERIFY(item("conversionTarget"));
      QPointer<QObject> dialog(
          window_->findChild<QObject *>("conversionWorkspace"));
      QVERIFY(dialog);
      const auto close = qScopeGuard([dialog] {
        if (dialog)
          QMetaObject::invokeMethod(dialog, "close");
      });
      item("conversionAdvancedSettings")->setProperty("checked", true);
      QTRY_VERIFY(!qobject_cast<ImageJobs *>(desktop_->imageJobs())->importing());
      QVERIFY(!qobject_cast<ImageJobs *>(desktop_->imageJobs())->sourceRoots().isEmpty());
      item("conversionOutputRoot")->setProperty("text", output);
      item("conversionTarget")->setProperty("currentIndex", i);
      auto *advanced = item("conversionAdvanced");
      QVERIFY(advanced);
      advanced->setProperty("checked", true);
      const bool floating = target == "hdr" || target == "exr";
      item("conversionDepth")->setProperty("currentIndex", floating ? 4 : 0);
      item("conversionAlphaAction")
          ->setProperty(
              "currentIndex",
              target == "jpeg" || target == "bmp" || target == "hdr" ? 1 : 0);
      item("conversionAllowLossy")
          ->setProperty("checked", target == "jpeg" || target == "hdr");
      item("conversionQuantize")->setProperty("checked", floating);
      auto *preflight = item("createImagePlan");
      QVERIFY(preflight && preflight->isEnabled());
      QTest::mouseClick(
          window_, Qt::LeftButton, Qt::NoModifier,
          preflight
              ->mapToScene({preflight->width() / 2, preflight->height() / 2})
              .toPoint());
      QTRY_VERIFY_WITH_TIMEOUT(
          !service->busy() && (!service->error().isEmpty() ||
                               (!service->plan().isEmpty() &&
                                service->plan().value("state") != "planning")),
          15000);
      QVERIFY2(service->error().isEmpty(),
               qPrintable(target + ": " + service->error()));
      QVERIFY2(service->plan().value("state") == "ready",
               qPrintable(QString::fromUtf8(
                   QJsonDocument::fromVariant(service->plan()).toJson())));
      const auto lossItems = service->plan()
                                 .value("items")
                                 .toList()[0]
                                 .toMap()
                                 .value("preflight")
                                 .toMap()
                                 .value("losses")
                                 .toList();
      auto *ack = window_->findChild<QQuickItem *>("acknowledgeImageLosses");
      QVERIFY(ack);
      if (!lossItems.isEmpty()) {
        QVERIFY(ack->isEnabled());
        ack->setProperty("checked", true);
      }
      auto *submit = item("submitImagePlan");
      QVERIFY(submit && submit->isEnabled());
      QTest::mouseClick(
          window_, Qt::LeftButton, Qt::NoModifier,
          submit->mapToScene({submit->width() / 2, submit->height() / 2})
              .toPoint());
      QTRY_VERIFY_WITH_TIMEOUT(!service->busy() &&
                                   service->job().value("state") == "succeeded",
                               15000);
      const auto path = output + "/唯一图片-converted." + target;
      QVERIFY(QFile::exists(path));
      auto document = formaview::imaging::ImageDocument::open(
          std::filesystem::path(path.toStdWString()));
      auto decoded = document.decode(0);
      QCOMPARE(decoded.data_window.width, 64ULL);
      QCOMPARE(decoded.data_window.height, 64ULL);
      for (const auto &channel : decoded.channels)
        if (channel.name == "R" || channel.name == "G" || channel.name == "B")
          QVERIFY2(std::abs(formaview::imaging::interpreted_value(channel, 0) -
                            (channel.name == "R" ? 0. : 1.)) < .025,
                   qPrintable(target));
      results.append(QJsonObject{
          {"target", target},
          {"job", service->job().value("id").toString()},
          {"path", path},
          {"sha256", QString::fromStdString(
                         document.description().source.identity.sha256)}});
      QTest::keyClick(window_, Qt::Key_Escape);
      QTRY_VERIFY(!item("conversionTarget"));
    }
    evidence_["image_gui_write_targets"] = results;
#endif
  }
  void mcpSettingsInteraction() {
    try {
#ifdef FORMAVIEW_IMAGE_UI
    auto *service = qobject_cast<ImageJobs *>(desktop_->imageJobs());
    QVERIFY(service && !service->mcpEnabled());
    const auto scope = service->scope();
    QVERIFY2(scope.value("output_roots").toList().size() == 1,
             "A successfully configured scope is required before UI settings "
             "checks");
    const auto output = scope.value("output_roots").toList()[0].toString();
    const auto python = requireFixture("FORMAVIEW_TEST_MCP_PYTHON");
    const auto adapter = requireFixture("FORMAVIEW_TEST_MCP_ADAPTER");
    desktop_->preference("mcpInputRoot", small_);
    desktop_->preference("mcpOutputRoot", output);
    desktop_->preference("mcpPython", python);
    desktop_->preference("mcpAdapter", adapter);
    auto *dialog = window_->findChild<QObject *>("mcpSettingsDialog");
    QVERIFY(dialog);
    QVERIFY(QMetaObject::invokeMethod(dialog, "open", Q_ARG(QVariant, "permissions")));
    const auto closeSettings = qScopeGuard([popup = QPointer<QObject>(dialog)] {
      if (popup)
        QMetaObject::invokeMethod(popup, "close");
    });
    QTRY_VERIFY(item("mcpEnableButton"));
    auto *input = item("mcpInput");
    QVERIFY(input);
    input->forceActiveFocus();
    QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
    QInputMethodEvent commit;
    commit.setCommitString(small_);
    QCoreApplication::sendEvent(input, &commit);
    QCOMPARE(input->property("text").toString(), small_);
    auto *apply = item("mcpApplyScope");
    QVERIFY(apply && apply->isEnabled());
    QTest::mouseClick(
        window_, Qt::LeftButton, Qt::NoModifier,
        apply->mapToScene({apply->width() / 2, apply->height() / 2}).toPoint());
    QTRY_VERIFY(!service->busy());
    QVERIFY2(service->error().isEmpty(), qPrintable(service->error()));
    // Applying MCP permissions switches the backend's authorization scope.
    // Seed an approved GUI job in that scope, not an archived job from the
    // broader local-export scope used by the preceding matrix test.
    const auto configuredScope = service->scope();
    auto scopedInputs = configuredScope;
    scopedInputs.remove("policy_revision");
    const auto previousScopedJob = service->job().value("id").toString();
    service->preflight({small_ + "/唯一图片.png"}, scopedInputs,
                       {{"target", "png"}}, "settings-existing.{ext}", "skip");
    QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && service->plan().value("state") == "ready", 15000);
    service->submit(true);
    QTRY_VERIFY_WITH_TIMEOUT(!service->busy() &&
        service->job().value("id").toString() != previousScopedJob &&
        service->job().value("state") == "succeeded", 15000);
    QCOMPARE(service->scope(), configuredScope);
    auto *enable = item("mcpEnableButton");
    QVERIFY(enable && enable->isEnabled());
    QTest::mouseClick(
        window_, Qt::LeftButton, Qt::NoModifier,
        enable->mapToScene({enable->width() / 2, enable->height() / 2})
            .toPoint());
    QTRY_VERIFY(service->mcpEnabled());
    QVERIFY(!apply->isEnabled());
    QVERIFY(service->lastClient().isEmpty());
    QProcess process;
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) {
          args->flags |= CREATE_NO_WINDOW;
        });
#endif
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(python, {requireFixture("FORMAVIEW_TEST_MCP_CLIENT"),
                           "--endpoint", service->endpoint(), "--adapter",
                           adapter, "--source", small_ + "/唯一图片.png",
                           "--output", output + "/settings-client.png",
                           "--existing-job", service->job()["id"].toString(),
                           "--report", root_ + "/settings-mcp-report.json"});
    QTRY_VERIFY_WITH_TIMEOUT(process.state() == QProcess::NotRunning, 20000);
    QVERIFY2(process.exitCode() == 0, process.readAll().constData());
    QTRY_VERIFY(!service->lastClient().isEmpty());
    auto *scroll = item("mcpSettingsScroll");
    QVERIFY(scroll);
    auto *flickable =
        qvariant_cast<QQuickItem *>(scroll->property("contentItem"));
    QVERIFY(flickable);
    for (const auto &language :
         {QString("zh-CN"), QString("en"), QString("zh-TW")}) {
      desktop_->setLanguage(language);
      flickable->setProperty("contentY", 0);
      QTest::qWait(60);
      save("mcp-enabled-" + language);
    }
    desktop_->setLanguage("zh-CN");
    flickable->setProperty(
        "contentY",
        std::max(0., flickable->property("contentHeight").toDouble() -
                         flickable->height()));
    QTest::qWait(60);
    auto *copy = item("mcpCopyConfig");
    QVERIFY(copy && copy->isEnabled());
    QGuiApplication::clipboard()->clear();
    QTest::mouseClick(
        window_, Qt::LeftButton, Qt::NoModifier,
        copy->mapToScene({copy->width() / 2, copy->height() / 2}).toPoint());
    QTRY_VERIFY(!QGuiApplication::clipboard()->text().isEmpty());
    const auto config =
        QJsonDocument::fromJson(QGuiApplication::clipboard()->text().toUtf8())
            .object()["mcpServers"]
            .toObject()["formaview-images"]
            .toObject();
    QCOMPARE(config["command"].toString(), python);
    QCOMPARE(config["env"].toObject()["FORMAVIEW_IMAGE_ENDPOINT"].toString(),
             service->endpoint());
    save("mcp-copy-config");
    flickable->setProperty("contentY", 0);
    QTest::qWait(60);
    QTest::mouseClick(
        window_, Qt::LeftButton, Qt::NoModifier,
        enable->mapToScene({enable->width() / 2, enable->height() / 2})
            .toPoint());
    QTRY_VERIFY(!service->mcpEnabled());
    QVERIFY(!copy->isEnabled());
    QTest::keyClick(window_, Qt::Key_Escape);
    QTRY_VERIFY(!dialog->property("visible").toBool());
#endif
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void textureSettingsAndHelp() {
#ifdef FORMAVIEW_IMAGE_UI
    auto *service = qobject_cast<ImageJobs *>(desktop_->imageJobs());
    QVERIFY(service);
    const auto originalSize = window_->size();
    const auto preferences = desktop_->preferences();
    QString originalTemplate;
    int originalPreset = -1;
    const auto cleanup = qScopeGuard([&] {
      if (originalPreset >= 0) {
        if (auto *field = window_->findChild<QObject *>("conversionNamingTemplate")) field->setProperty("text", originalTemplate);
        if (auto *preset = window_->findChild<QObject *>("conversionNamingPreset")) preset->setProperty("currentIndex", originalPreset);
      }
      if (auto *w = window_->findChild<QObject *>("conversionWorkspace")) QMetaObject::invokeMethod(w, "close");
      desktop_->preference("density", preferences.value("density"));
      desktop_->setLanguage("zh-CN"); window_->resize(originalSize);
    });
    QVERIFY(QMetaObject::invokeMethod(item("workspace"), "openConversion", Q_ARG(QVariant, 0), Q_ARG(QVariant, "")));
    auto *workspace = window_->findChild<QObject *>("conversionWorkspace"); QVERIFY(workspace);
    workspace->setProperty("modelMode", false);
    originalTemplate = item("conversionNamingTemplate")->property("text").toString();
    originalPreset = item("conversionNamingPreset")->property("currentIndex").toInt();
    service->setSourceInputs({small_ + "/唯一图片.png"});
    QTRY_VERIFY(!service->importing());
    const auto click = [&](const char *name) {
      window_->requestActivate(); QTRY_COMPARE(QGuiApplication::focusWindow(), window_);
      auto *control = item(name); QVERIFY(control && control->isEnabled());
      control->forceActiveFocus(); QTest::qWait(100);
      QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, control->mapToScene({control->width()/2, control->height()/2}).toPoint());
    };
    auto *target = item("conversionTarget"); QVERIFY(target);
    const auto targets = service->capabilities()["write_targets"].toList();
    target->setProperty("currentIndex", targets.indexOf("png"));
    QTRY_VERIFY(item("pngCompressionExplanation") && item("pngCompressionExplanation")->isVisible());
    QVERIFY(item("pngCompressionExplanation")->property("text").toString().contains("0"));
    click("conversionNamingHelp");
    QPointer<QQuickWindow> help;
    QTRY_VERIFY([&] { for (auto *w : QGuiApplication::allWindows()) if (w->objectName() == "conversionNamingWindow") help = qobject_cast<QQuickWindow *>(w); return help && help->isExposed(); }());
    QCOMPARE(help->modality(), Qt::NonModal); QCOMPARE(help->transientParent(), window_);
    QVERIFY(help->flags().testFlag(Qt::ExpandedClientAreaHint));
#ifdef Q_OS_WIN
    QVERIFY(help->findChild<QObject *>("nativeWindowChrome"));
    const auto helpHandle = reinterpret_cast<HWND>(help->winId());
    POINT p{qRound(70 * help->devicePixelRatio()), qRound(26 * help->devicePixelRatio())};
    QVERIFY(ClientToScreen(helpHandle, &p));
    QCOMPARE(SendMessageW(helpHandle, WM_NCHITTEST, 0, MAKELPARAM(p.x, p.y)), LRESULT(HTCAPTION));
#endif
    click("conversionNamingHelp");
    int helpCount = 0;
    for (auto *w : QGuiApplication::allWindows()) if (w->objectName() == "conversionNamingWindow") ++helpCount;
    QCOMPARE(helpCount, 1);
    window_->requestActivate(); QTRY_VERIFY(window_->isActive());
    auto *pattern = item("conversionNamingTemplate"); QVERIFY(pattern); pattern->forceActiveFocus();
    QVERIFY(QMetaObject::invokeMethod(pattern, "selectAll"));
    QInputMethodEvent edit; edit.setCommitString("{index}-{name}.{ext}"); QCoreApplication::sendEvent(pattern, &edit);
    QCOMPARE(pattern->property("text").toString(), QString("{index}-{name}.{ext}"));
    QVERIFY(help->isVisible());
    window_->resize(1280, 800);
    for (const auto &density : {QString("standard"), QString("compact")}) {
      desktop_->preference("density", density);
      help->resize(density == "compact" ? QSize(440, 340) : QSize(580, 480));
      for (const auto &language : {QString("zh-CN"), QString("en"), QString("zh-TW")}) {
        desktop_->setLanguage(language); QTest::qWait(100);
        const auto png = "texture-settings-" + density + '-' + language;
        save(png);
        QVERIFY(captureWindow(help).save(root_ + '/' + png + "-help.png"));
        for (const auto *name : {"addConversionImages", "addConversionFolder"}) {
          auto *button = item(name); QVERIFY(button && button->isVisible());
          const auto box = button->mapRectToScene(button->boundingRect());
          QVERIFY2(box.right() <= window_->width() && box.left() >= 0, name);
        }
      }
    }
    help->requestActivate(); QTRY_VERIFY(help->isActive());
    QTest::keyClick(help, Qt::Key_Escape); QTRY_VERIFY(!help);
    QCOMPARE(pattern->property("text").toString(), QString("{index}-{name}.{ext}"));
    click("conversionNamingHelp");
    QTRY_VERIFY([&] { for (auto *w : QGuiApplication::allWindows()) if (w->objectName() == "conversionNamingWindow") help = qobject_cast<QQuickWindow *>(w); return !help.isNull(); }());
    QVERIFY(QMetaObject::invokeMethod(workspace, "close")); QTRY_VERIFY(!help);
    QObject *about = nullptr;
    for (auto *object : window_->findChildren<QObject *>())
      if (object->property("title").toString() == "FormaView" && object->property("modal").isValid()) about = object;
    QVERIFY(about && about->property("modal").toBool());
    for (const auto &language : {QString("zh-CN"), QString("en"), QString("zh-TW")}) {
      desktop_->setLanguage(language);
      QVERIFY(QMetaObject::invokeMethod(about, "open")); QTRY_VERIFY(about->property("opened").toBool());
      save("about-theme-" + language);
      window_->requestActivate(); QTRY_COMPARE(QGuiApplication::focusWindow(), window_);
      QTest::keyClick(window_, Qt::Key_Escape); QTRY_VERIFY(!about->property("visible").toBool());
    }
#endif
  }
  void imageJournalPathValidation() {
#ifdef FORMAVIEW_IMAGE_UI
    const auto previousRoot = qgetenv("FORMAVIEW_IMAGE_STATE_ROOT");
    const auto restoreRoot = qScopeGuard([&] {
      qputenv("FORMAVIEW_IMAGE_STATE_ROOT", previousRoot);
    });
    const auto localRoot = root_ + "/journal-path-validation";
    QVERIFY(QDir().mkpath(localRoot + "/input"));
    QVERIFY(QDir().mkpath(localRoot + "/output"));
    const QVariantMap scope{{"input_roots", QStringList{localRoot + "/input"}},
                            {"output_roots", QStringList{localRoot + "/output"}},
                            {"allow_overwrite", false}};
    const QStringList rejectedRoots{
        QStringLiteral("\\\\localhost\\formaview-no-such-share\\journal"),
        QStringLiteral("//localhost/formaview-no-such-share/journal"),
        QStringLiteral("\\\\?\\UNC\\localhost\\formaview-no-such-share\\journal"),
        localRoot + "/must-not-create/../escaped"};
    for (const auto &stateRoot : rejectedRoots) {
      qputenv("FORMAVIEW_IMAGE_STATE_ROOT", stateRoot.toUtf8());
      ImageJobs jobs;
      jobs.configureScope(scope);
      QTRY_VERIFY_WITH_TIMEOUT(!jobs.busy(), 10000);
      QVERIFY2(jobs.error().startsWith("out_of_scope: "), qPrintable(jobs.error()));
      QVERIFY(!jobs.configured());
      QVERIFY(jobs.plan().isEmpty());
    }
    QVERIFY(!QDir(localRoot + "/must-not-create").exists());
    QVERIFY(!QDir(localRoot + "/escaped").exists());
    qputenv("FORMAVIEW_IMAGE_STATE_ROOT", (localRoot + "/valid-state").toUtf8());
    ImageJobs valid;
    valid.configureScope(scope);
    QTRY_VERIFY_WITH_TIMEOUT(!valid.busy(), 10000);
    QVERIFY2(valid.error().isEmpty(), qPrintable(valid.error()));
    QVERIFY(valid.configured());
    QCOMPARE(QDir(localRoot + "/valid-state").entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 2);
#endif
  }
  void imageDirectorySelection() {
#ifdef FORMAVIEW_IMAGE_UI
    auto *service = qobject_cast<ImageJobs *>(desktop_->imageJobs());
    QVERIFY(service && !service->mcpEnabled());
    const auto directory = small_ + "/discovery";
    QVERIFY(QDir().mkpath(directory + "/nested"));
    for (int i = 0; i < 129; ++i)
      QVERIFY(QFile::copy(small_ + "/唯一图片.png", directory + QString("/image-%1.png").arg(i, 3, 10, QLatin1Char('0'))));
    QVERIFY(QFile::copy(small_ + "/唯一图片.png", directory + "/nested/inside.png"));
#ifdef Q_OS_WIN
    const auto ambiguousPath = L"\\\\?\\" + QDir::toNativeSeparators(directory + "/trailing.png.").toStdWString();
    const auto ambiguous = CreateFileW(ambiguousPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    QVERIFY(ambiguous != INVALID_HANDLE_VALUE); CloseHandle(ambiguous);
    // Qt's normal Win32 path cleanup strips the trailing dot. Remove exactly
    // the extended-path fixture we created, including on assertion failure.
    const auto removeAmbiguous = qScopeGuard([&] { QVERIFY(DeleteFileW(ambiguousPath.c_str())); });
#endif
    QVERIFY(QMetaObject::invokeMethod(item("workspace"), "openConversion", Q_ARG(QVariant, 0), Q_ARG(QVariant, "")));
    const auto closeConversion = qScopeGuard([&] {
      if (auto *w = window_->findChild<QObject *>("conversionWorkspace")) QMetaObject::invokeMethod(w, "close");
      service->clearSources();
    });
    QTRY_VERIFY(item("addConversionImages") && item("addConversionImages")->isVisible());
    QVERIFY(item("addConversionFolder")->isVisible());
    QVERIFY(!item("conversionIncludeSubfolders")->property("checked").toBool());
    auto *dialog = window_->findChild<QObject *>("conversionImageFileDialog");
    QVERIFY(dialog); QCOMPARE(dialog->property("fileMode").toInt(), 1);
    service->clearSources();
    // No output directory or configured image service is needed to import.
    service->addSourceFolder(directory, false);
    QTRY_VERIFY_WITH_TIMEOUT(!service->importing(), 10000);
    QCOMPARE(service->sourcePaths().size(), 129);
    QCOMPARE(service->sourceRoots().size(), 1);
    QCOMPARE(service->importResult()["added"].toInt(), 129);
#ifdef Q_OS_WIN
    QCOMPARE(service->importResult()["rejected"].toInt(), 1);
#endif
    service->addSourceFiles({directory + "/image-000.png", directory + "/image-001.png"});
    QTRY_VERIFY_WITH_TIMEOUT(!service->importing(), 10000);
    QCOMPARE(service->sourcePaths().size(), 129);
    QCOMPARE(service->importResult()["duplicates"].toInt(), 2);
    service->addSourceFolder(directory, true);
    QTRY_VERIFY_WITH_TIMEOUT(!service->importing(), 10000);
    QCOMPARE(service->sourcePaths().size(), 130);
    QCOMPARE(service->sourceRoots().size(), 1);
    service->addSourceFiles({small_ + "/唯一图片.png"});
    QTRY_VERIFY_WITH_TIMEOUT(!service->importing(), 10000);
    QCOMPARE(service->sourcePaths().size(), 131);
    QCOMPARE(service->sourceRoots().size(), 1);
    const auto baseline = service->sourcePaths();
    service->addSourceFolder(directory, true); service->cancelImport();
    QTest::qWait(100); QCOMPARE(service->sourcePaths(), baseline);
    service->addSourceFolder(directory + "/missing", false);
    QTRY_VERIFY_WITH_TIMEOUT(!service->importing(), 10000);
    QVERIFY(service->importResult().contains("error")); QCOMPARE(service->sourcePaths(), baseline);
    QVERIFY(QDir().mkpath(directory + "/empty"));
    service->addSourceFolder(directory + "/empty", false);
    QTRY_VERIFY_WITH_TIMEOUT(!service->importing(), 10000);
    QVERIFY(service->importResult()["empty"].toBool()); QCOMPARE(service->sourcePaths(), baseline);
    service->removeSource(small_ + "/唯一图片.png");
    QCOMPARE(service->sourcePaths().size(), 130);
    service->removeSource(directory + "/nested/inside.png");
    QCOMPARE(service->sourcePaths().size(), 129);
    const auto output = root_ + "/discovery-output"; QVERIFY(QDir().mkpath(output));
    item("conversionOutputRoot")->setProperty("text", output);
    service->preflight(service->sourcePaths(), {{"input_roots", service->sourceRoots()}, {"output_roots", QStringList{output}}}, {{"target", "tga"}}, "{index}-{name}.{ext}", "skip");
    QTRY_VERIFY_WITH_TIMEOUT(service->plan()["state"] == "ready", reviewPreflightWaitMs);
    QCOMPARE(service->plan()["total"].toInt(), 129);
    QCOMPARE(service->plan()["batch_count"].toInt(), 3);
    QVERIFY(QDir(output).entryList(QDir::Files).isEmpty());
    service->submit(true);
    QTRY_VERIFY_WITH_TIMEOUT(service->job()["state"] == "succeeded", reviewPreflightWaitMs);
    QCOMPARE(service->job()["items"].toList().size(), 129);
    item("conversionSteps")->setProperty("currentIndex", 2);
    QTRY_VERIFY(item("imageGroupProgress"));
    QVERIFY(item("imageGroupProgress")->property("text").toString().contains("129 / 129"));
    QVERIFY(QFileInfo::exists(output + "/129-image-128.tga"));
    for (const auto &language : {QString("zh-CN"), QString("en"), QString("zh-TW")}) {
      desktop_->setLanguage(language); QTest::qWait(60);
      save("conversion-import-" + language);
    }
    desktop_->setLanguage("zh-CN");
    service->clearSources();
    QStringList boundary;
    for (int i = 0; i < 4096; ++i) boundary.append(directory + QString("/limit-%1.png").arg(i));
    service->addSourceFiles(boundary); QTRY_VERIFY(!service->importing());
    QCOMPARE(service->sourcePaths().size(), 4096);
    service->addSourceFiles({directory + "/limit-extra.png"}); QTRY_VERIFY(!service->importing());
    QCOMPARE(service->sourcePaths().size(), 4097); QVERIFY(!service->importResult().contains("error"));
    service->clearSources();
    QStringList roots;
    for (int i = 0; i < 64; ++i) roots.append(directory + QString("/root-%1/image.png").arg(i));
    service->addSourceFiles(roots); QTRY_VERIFY(!service->importing());
    QCOMPARE(service->sourceRoots().size(), 64);
    service->addSourceFiles({directory + "/root-extra/image.png"}); QTRY_VERIFY(!service->importing());
    QCOMPARE(service->sourcePaths().size(), 65); QCOMPARE(service->sourceRoots().size(), 65);
    QVERIFY(!service->importResult().contains("error"));
#endif
  }
  void approvedPageReviewCleanup() {
#ifdef FORMAVIEW_IMAGE_UI
    auto *workspace = item("workspace");
    auto *settings = window_->findChild<QObject *>("mcpSettingsDialog");
    auto *unsaved = window_->findChild<QObject *>("unsavedSettingsDialog");
    QVERIFY(workspace && settings && unsaved && desktop_->imageJobs());
    desktop_->setLanguage("zh-CN");
    const auto preferences = desktop_->preferences();
    const auto rescue = qScopeGuard([&] {
      QMetaObject::invokeMethod(unsaved, "close");
      settings->setProperty("afterLeave", QVariant::fromValue(QJSValue(QJSValue::NullValue)));
      workspace->setProperty("settingsVisible", false);
      workspace->setProperty("focusMode", false);
    });
    for (const bool modalAlreadyOpen : {false, true}) {
      QVERIFY(QMetaObject::invokeMethod(settings, "open", Q_ARG(QVariant, "general")));
      QTRY_VERIFY(settings->property("visible").toBool() && !settings->property("dirty").toBool());
      const auto density = desktop_->preferences().value("density").toString();
      auto *choice = item(density == "compact" ? "settingsDensityStandard" : "settingsDensityCompact");
      QVERIFY(choice && QMetaObject::invokeMethod(choice, "clicked"));
      QVERIFY(settings->property("dirty").toBool());
      if (modalAlreadyOpen) {
        // This also leaves a pending window-close callback, which cleanup
        // must cancel rather than execute while discarding the draft.
        QVERIFY(!window_->close());
        QTRY_VERIFY(unsaved->property("opened").toBool());
      }
      workspace->setProperty("settingsVisible", false);
      QVERIFY(settings->property("dirty").toBool());
      const auto leaveEarly = [&] {
        const auto cleanup = qScopeGuard([&] { restorePageReviewUi(); });
        return; // Same scope unwinding as a QTest assertion before MCP setup.
      };
      leaveEarly();
      QTRY_VERIFY(!unsaved->property("visible").toBool());
      QVERIFY(!settings->property("dirty").toBool());
      QVERIFY(!settings->property("visible").toBool());
      QVERIFY(window_->isVisible());
      QTRY_VERIFY(window_->isExposed());
      QCOMPARE(desktop_->preferences(), preferences);
      auto *focus = item("globalFocusPreview");
      QVERIFY(focus && focus->isVisible());
      QTest::qWait(100); // Allow the restored page's layout to polish before input.
      QSignalSpy clicked(focus, SIGNAL(clicked()));
      QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
          focus->mapToScene({focus->width()/2, focus->height()/2}).toPoint());
      QCOMPARE(clicked.count(), 1);
      QVERIFY(workspace->property("focusMode").toBool());
      QVERIFY(QMetaObject::invokeMethod(focus, "clicked"));
      QVERIFY(!workspace->property("focusMode").toBool());
    }
#else
    QSKIP("Image UI is not compiled into this build");
#endif
  }
  void approvedPageReview() {
#ifdef FORMAVIEW_IMAGE_UI
    auto *workspace = item("workspace");
    auto *session = qobject_cast<ImageSession *>(desktop_->image());
    auto *service = qobject_cast<ImageJobs *>(desktop_->imageJobs());
    QVERIFY(workspace && session && service);
    // QTest assertions return early. Leave the shared window in the same
    // normal-preview state on failure as on successful completion.
    const auto restoreReviewUi = qScopeGuard([&] { restorePageReviewUi(); });
    QJsonObject validationWaits{{"cancel_preflight_budget_ms", reviewPreflightWaitMs},
      {"producer_preflight_budget_ms", reviewProducerPreflightWaitMs},
      {"producer_signal_budget_ms", reviewProducerSignalWaitMs}};
    const auto recordWaits = qScopeGuard([&] { evidence_["review_validation_waits"] = validationWaits; });
    workspace->setProperty("tasksVisible", false);
    workspace->setProperty("settingsVisible", false);
    workspace->setProperty("conversionVisible", false);
    workspace->setProperty("focusMode", false);
    const auto captureMatrix = [&](const QString &page) {
      const auto previousSize = window_->size();
      const auto previousLanguage = desktop_->language();
      for (const auto &size : {QSize(1600,900), QSize(1280,800)}) {
        window_->resize(size);
        for (const auto &language : {QString("zh-CN"), QString("en"), QString("zh-TW")}) {
          desktop_->setLanguage(language);
          QTest::qWait(80);
          const auto name = "review-" + page + "-" + QString::number(size.width()) + "-" + language;
          QVERIFY(captureWindow(window_).save(root_ + "/" + name + ".png"));
        }
      }
      window_->resize(previousSize);
      desktop_->setLanguage(previousLanguage);
      QTest::qWait(80);
    };
    const QList<QPair<QString, QString>> samples{
      {"rgba", small_ + "/唯一图片.png"},
      {"tile", small_ + "/two-channel.dds"},
      {"exr", small_ + "/显示精度.exr"},
      {"dds", root_ + "/fixtures/image/subresources-0.dds"},
      {"part", root_ + "/fixtures/image/multipart.exr"},
      {"layers", small_ + "/actual-layers.exr"}};
    QJsonArray sources;
    for (const auto &[page, path] : samples) {
      QVERIFY2(QFile::exists(path), qPrintable(path));
      desktop_->openPath(path);
      QTRY_VERIFY_WITH_TIMEOUT(!session->busy() && session->available() && session->path() == path, 15000);
      QVERIFY2(session->error().isEmpty(), qPrintable(session->error()));
      session->setting("tiled", page == "tile");
      if (page == "exr") {
        session->setting("colour", true);
        session->setting("exposure", 1.);
        session->setting("srgb", true);
        session->sample(-3,7);
      }
      if (page == "dds") {
        session->selectDimension("array", 1); QTRY_VERIFY(!session->busy());
        session->selectDimension("face", 5); QTRY_VERIFY(!session->busy());
        session->selectDimension("mip", 2); QTRY_VERIFY(!session->busy());
      }
      if (page == "part") { session->selectSubresource(1); QTRY_VERIFY(!session->busy()); }
      item("pixelToggle")->setProperty("checked", page == "exr" || page == "tile");
      if (auto *section = item("imageChannelSection")) section->setProperty("checked", page == "layers");
      if (auto *section = item("imageMetadataSection")) section->setProperty("checked", page == "part");
      sources.append(QJsonObject{{"page", page}, {"path",path}, {"sha256", session->info()["sha256"].toString()}, {"selected",session->selected()}, {"info",QJsonObject::fromVariantMap(session->info())}});
      captureMatrix(page);
    }
    desktop_->openPath(small_ + "/review-missing.exr");
    QTRY_VERIFY(!desktop_->busy() && !desktop_->error().isEmpty());
    captureMatrix("failed-retained");
    desktop_->dismissError();
    desktop_->clearAsset();
    captureMatrix("empty");
    auto *settings = window_->findChild<QObject *>("mcpSettingsDialog");
    QVERIFY(settings);
    QVERIFY(QMetaObject::invokeMethod(settings, "open", Q_ARG(QVariant, "permissions")));
    QTRY_VERIFY(item("mcpEnableButton"));
    captureMatrix("mcp-off");
    service->setMcpEnabled(true);
    QTRY_VERIFY(service->mcpEnabled());
    captureMatrix("mcp-endpoint");
    service->setMcpEnabled(false);
    QVERIFY(QMetaObject::invokeMethod(settings, "close"));
    // A real source change between preflight and execution produces partial success.
    auto reviewScope = service->scope();
    reviewScope.remove("policy_revision");
    const auto input = reviewScope.value("input_roots").toList().front().toString();
    for (const auto *name : {"review-valid.png", "review-changed.png"})
      QVERIFY(QFile::copy(small_ + "/唯一图片.png", input + "/" + name));
    service->preflight({input + "/review-valid.png", input + "/review-changed.png"}, reviewScope,
       {{"target","png"}}, "{name}-review.{ext}", "skip");
    QTRY_VERIFY2_WITH_TIMEOUT(!service->busy() && service->plan().value("state") == "ready", qPrintable(service->error()), 15000);
    QFile changed(input + "/review-changed.png");
    QVERIFY(changed.open(QIODevice::WriteOnly));
    changed.write("changed after preflight"); changed.close();
    service->submit(true);
    QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && service->job().value("state") == "partially_succeeded", 15000);
    workspace->setProperty("tasksVisible", true);
    QTRY_VERIFY(item("imageJobsTable"));
    const auto verifyJobChoice = [&] {
      auto *choice = item("imageJobChoice");
      QVERIFY(choice);
      const auto jobs = service->jobs();
      const auto id = service->job().value("id");
      int selected = -1;
      for (qsizetype i = 0; i < jobs.size(); ++i)
        if (jobs[i].toMap().value("id") == id) selected = static_cast<int>(i);
      QVERIFY(selected >= 0);
      QTRY_COMPARE(choice->property("currentIndex").toInt(), selected);
      QTRY_COMPARE(item("conversionBatchList")->property("currentIndex").toInt(), choice->property("currentIndex").toInt());
      QTRY_VERIFY(item("selectedBatchHeading")->property("text").toString().contains(
          service->job().value("name").toString().isEmpty() ? id.toString() : service->job().value("name").toString()));
      const auto state = service->job().value("state").toString();
      const auto expected = state == "partially_succeeded" ? QString("部分成功") : state == "cancelled" ? QString("已取消") : QString("已中断");
      QTRY_VERIFY(choice->property("displayText").toString().endsWith(expected));
    };
    desktop_->setLanguage("zh-CN");
    verifyJobChoice();
    captureMatrix("partial-tasks");
    const auto partialJob = QJsonObject::fromVariantMap(service->job());
    // Submit actual work and cancel through the visible control. No journal or
    // UI state is fabricated for the cancelled-state evidence.
    QImage cancelImage(2048, 2048, QImage::Format_RGBA8888);
    cancelImage.fill(QColor(73, 127, 201, 255));
    const auto cancelSource = input + "/review-cancel.png";
    QVERIFY(cancelImage.save(cancelSource));
    QStringList cancelInputs;
    for (int i = 0; i < 64; ++i) cancelInputs.append(cancelSource);
    QElapsedTimer cancelPreflightElapsed; cancelPreflightElapsed.start();
    service->preflight(cancelInputs, reviewScope,
       {{"target","png"}, {"png_compression",9}}, "cancel-{index}.{ext}", "skip");
    QTRY_VERIFY2_WITH_TIMEOUT(!service->busy() && service->plan().value("state") == "ready", qPrintable(service->error()), reviewPreflightWaitMs);
    validationWaits["cancel_preflight_elapsed_ms"] = cancelPreflightElapsed.elapsed();
    validationWaits["input_count"] = cancelInputs.size();
    const auto previousJob = service->job().value("id");
    service->submit(true);
    QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && service->job().value("id") != previousJob, 15000);
    auto *cancelControl = item("cancelImageJob");
    QVERIFY(cancelControl && cancelControl->isVisible() && cancelControl->isEnabled());
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
      cancelControl->mapToScene({cancelControl->width()/2, cancelControl->height()/2}).toPoint());
    QTRY_VERIFY(service->job().value("cancellation_requested").toBool());
    QTRY_VERIFY_WITH_TIMEOUT(service->job().value("state") == "cancelled" || service->job().value("state") == "partially_succeeded", 15000);
    const auto cancelledJob = QJsonObject::fromVariantMap(service->job());
    const auto cancelledItems = service->job().value("items").toList();
    QVERIFY(std::any_of(cancelledItems.cbegin(), cancelledItems.cend(), [](const auto &entry) { return entry.toMap().value("status") == "cancelled"; }));
    verifyJobChoice();
    captureMatrix("cancelled-tasks");
    evidence_["review_jobs"] = QJsonObject{{"partial",partialJob}, {"cancelled",cancelledJob}};
    // A separate, owned process performs real conversion. Terminating it while
    // running leaves its actual journal for the GUI service to recover.
    const auto recoveryInput = root_ + "/recovery-input";
    const auto recoveryOutput = root_ + "/recovery-output";
    const auto recoverySignal = root_ + "/recovery-producer-ready.json";
    QVERIFY(QDir().mkpath(recoveryInput) && QDir().mkpath(recoveryOutput));
    QVERIFY(QFile::copy(cancelSource, recoveryInput + "/source.png"));
    QProcess producer;
#ifdef Q_OS_WIN
    producer.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
#endif
    const auto stopProducer = qScopeGuard([&] { if (producer.state() != QProcess::NotRunning) { producer.kill(); producer.waitForFinished(5000); } });
    QElapsedTimer producerSignalElapsed; producerSignalElapsed.start();
    producer.start(QCoreApplication::applicationFilePath(), {"--recovery-producer", recoveryInput, recoveryOutput, recoverySignal});
    QTRY_VERIFY2_WITH_TIMEOUT(QFile::exists(recoverySignal), producer.readAll().constData(), reviewProducerSignalWaitMs);
    QFile signal(recoverySignal);
    // Windows may expose the committed name before a transient exclusive
    // handle is released. Wait for readability as well as atomic publication.
    QTRY_VERIFY2_WITH_TIMEOUT(signal.isOpen() || signal.open(QIODevice::ReadOnly), qPrintable(signal.errorString()), 5000);
    QJsonParseError signalError;
    const auto runningJob = QJsonDocument::fromJson(signal.readAll(), &signalError).object();
    signal.close();
    QVERIFY2(signalError.error == QJsonParseError::NoError, qPrintable(signalError.errorString()));
    QCOMPARE(runningJob.value("state").toString(), QString("running"));
    validationWaits["producer_signal_elapsed_ms"] = producerSignalElapsed.elapsed();
    producer.kill();
    QTRY_COMPARE(producer.state(), QProcess::NotRunning);
    QCOMPARE(producer.exitStatus(), QProcess::CrashExit);
    service->configureScope({{"input_roots",QStringList{recoveryInput}}, {"output_roots",QStringList{recoveryOutput}}, {"allow_overwrite",false}});
    QTRY_VERIFY2_WITH_TIMEOUT(!service->busy() && service->error().isEmpty(), qPrintable(service->error()), 15000);
    service->selectJob(runningJob.value("id").toString());
    QTRY_COMPARE(service->job().value("state").toString(), QString("interrupted"));
    const auto interruptedItems = service->job().value("items").toList();
    QVERIFY(std::any_of(interruptedItems.cbegin(), interruptedItems.cend(), [](const auto &entry) { return entry.toMap().value("status") == "interrupted"; }));
    verifyJobChoice();
    const auto outputCount = QDir(recoveryOutput).entryList(QDir::Files).size();
    captureMatrix("recovered-tasks");
    QCOMPARE(QDir(recoveryOutput).entryList(QDir::Files).size(), outputCount);
    evidence_["review_recovery"] = QJsonObject{{"before_termination",runningJob}, {"recovered",QJsonObject::fromVariantMap(service->job())}, {"output_files",outputCount}, {"producer_exit_status",static_cast<int>(producer.exitStatus())}};
    workspace->setProperty("tasksVisible", false);
    window_->resize(1600,900); desktop_->setLanguage("zh-CN");
    evidence_["review_sources"] = sources;
#else
    QSKIP("Image UI is not compiled into this build");
#endif
  }

  void imageMixedPerformance() {
#if defined(FORMAVIEW_IMAGE_UI) && defined(Q_OS_WIN)
    auto *session = qobject_cast<ImageSession *>(desktop_->image());
    auto *service = qobject_cast<ImageJobs *>(desktop_->imageJobs());
    QVERIFY(session && service);
    auto *workspace = item("workspace");
    workspace->setProperty("tasksVisible", false);
    workspace->setProperty("focusMode", false);
    window_->resize(1600, 900);
    desktop_->navigate(fixture_);
    QTRY_COMPARE(desktop_->directory(), fixture_);
    QTRY_VERIFY(idle());
    QStringList paths;
    std::array<std::shared_ptr<const ImagePixels>, 2> cached;
    for (int i = 0; i < 2; ++i) {
      QImage image(1024, 1024, QImage::Format_RGB32);
      image.fill(i == 0 ? Qt::yellow : Qt::cyan);
      paths.append(small_ + QString("/cache-performance-%1.bmp").arg(i));
      QVERIFY(image.save(paths.back()));
      desktop_->openPath(paths.back());
      QTRY_VERIFY_WITH_TIMEOUT(
          !session->busy() && session->path() == paths.back(), 10000);
      QVERIFY2(session->error().isEmpty(), qPrintable(session->error()));
      cached[static_cast<std::size_t>(i)] = session->pixels();
    }
    auto *view = qobject_cast<ImageView *>(item("imageViewport"));
    QVERIFY(view);
    const auto source = small_ + "/mixed-load.bmp";
    {
      QImage image(2048, 2048, QImage::Format_RGB32);
      quint32 state = 0x4d3035U;
      for (int y = 0; y < image.height(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
          state = state * 1664525U + 1013904223U;
          row[x] = 0xff000000U | (state & 0xffffffU);
        }
      }
      QVERIFY(image.save(source));
    }
    const auto output = root_ + "/mixed-output";
    QVERIFY(QDir().mkpath(output));
    const QVariantMap scope{{"input_roots", QVariantList{small_}},
                            {"output_roots", QVariantList{output}},
                            {"allow_overwrite", false}};
    QStringList inputs;
    for (int i = 0; i < 8; ++i)
      inputs.append(source);
    service->invalidatePlan();
    service->preflight(inputs, scope,
                       {{"target", "png"}, {"png_compression", 9}},
                       "mixed-{index}.{ext}", "skip");
    QTRY_VERIFY_WITH_TIMEOUT(!service->busy() &&
                                 service->plan().value("state") != "planning",
                             30000);
    QVERIFY2(service->error().isEmpty(), qPrintable(service->error()));
    QVERIFY2(service->plan().value("state") == "ready",
             qPrintable(QString::fromUtf8(
                 QJsonDocument::fromVariant(service->plan()).toJson())));
    view->forceActiveFocus();
    window_->requestActivate();
    const auto hwnd = reinterpret_cast<HWND>(window_->winId());
    const auto foregroundThread =
        GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const bool attached =
        foregroundThread != GetCurrentThreadId() &&
        AttachThreadInput(GetCurrentThreadId(), foregroundThread, TRUE);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    if (attached)
      AttachThreadInput(GetCurrentThreadId(), foregroundThread, FALSE);
    const auto restoreTopmost = qScopeGuard([&] {
      SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    });
    QTRY_VERIFY(GetForegroundWindow() == hwnd && view->hasActiveFocus());
    service->submit(true);
    QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && !service->job().isEmpty(),
                             10000);
    QVERIFY2(service->error().isEmpty(), qPrintable(service->error()));
    auto *measured =
        static_cast<MeasuredApplication *>(QCoreApplication::instance());
    measured->maxEventMs = 0;
    measured->events = measured->over100ms = 0;
    measured->slowEvents = {};
    measured->measure = true;
    const auto stopMeasurement =
        qScopeGuard([&] { measured->measure = false; });
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    bool pending = false, ready = false, native = false;
    int expected = -1;
    quint64 sequence = 0;
    std::atomic<quint64> changed{0}, synchronized{0}, swapped{0};
    auto started = std::chrono::steady_clock::now();
    std::vector<double> cacheTimes, inputTimes;
    auto mark = [&] {
      if (pending &&
          ((!native && !session->busy() &&
            session->pixels() == cached[static_cast<std::size_t>(expected)]) ||
           (native && session->display().value("channel").toInt() == expected)))
        changed.store(sequence);
    };
    const auto loaded = connect(session, &ImageSession::changed, this, mark);
    const auto display =
        connect(session, &ImageSession::displayChanged, this, mark);
    const auto synced = connect(
        window_, &QQuickWindow::afterSynchronizing, window_,
        [&] { synchronized.store(changed.load()); }, Qt::DirectConnection);
    const auto directlySwapped = connect(
        window_, &QQuickWindow::frameSwapped, window_,
        [&] { swapped.store(synchronized.load()); }, Qt::DirectConnection);
    const auto frame = connect(
        window_, &QQuickWindow::frameSwapped, this,
        [&] {
          if (pending && swapped.load() == sequence) {
            (native ? inputTimes : cacheTimes)
                .push_back(std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - started)
                               .count());
            ready = true;
            pending = false;
            loop.quit();
          }
        },
        Qt::QueuedConnection);
    const auto disconnectAll = qScopeGuard([&] {
      disconnect(loaded);
      disconnect(display);
      disconnect(synced);
      disconnect(directlySwapped);
      disconnect(frame);
    });
    int cacheOverlap = 0, inputOverlap = 0;
    for (int i = 0; i < 160; ++i) {
      if (i == 40) {
        // Cache setup can outlive foreground ownership. Prepare the native
        // phase outside its timed samples; never send keys to another app.
        measured->measure = false;
        view->forceActiveFocus();
        window_->requestActivate();
        const auto currentForegroundThread =
            GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        const bool inputAttached = currentForegroundThread != GetCurrentThreadId() &&
            AttachThreadInput(GetCurrentThreadId(), currentForegroundThread, TRUE);
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        if (inputAttached)
          AttachThreadInput(GetCurrentThreadId(), currentForegroundThread, FALSE);
        QTRY_VERIFY(GetForegroundWindow() == hwnd && view->hasActiveFocus());
        measured->measure = true;
      }
      native = i >= 40;
      expected = i % 2;
      if (i % 20 == 0)
        desktop_->refresh();
      if (service->job().value("state") == "running" ||
          service->job().value("state") == "queued")
        ++(native ? inputOverlap : cacheOverlap);
      ready = false;
      ++sequence;
      started = std::chrono::steady_clock::now();
      pending = true;
      if (!native)
        session->open(paths[expected]);
      else {
        QVERIFY2(GetForegroundWindow() == hwnd && view->hasActiveFocus(),
                 "Native input interrupted: test window lost foreground or image focus");
        INPUT events[2]{};
        events[0].type = INPUT_KEYBOARD;
        events[0].ki.wVk = expected == 0 ? 'R' : 'G';
        events[1] = events[0];
        events[1].ki.dwFlags = KEYEVENTF_KEYUP;
        QCOMPARE(SendInput(2, events, sizeof(INPUT)), UINT(2));
      }
      timeout.start(3000);
      if (!ready)
        loop.exec();
      timeout.stop();
      if (!ready) {
        evidence_["image_mixed_timeout"] = QJsonObject{
            {"iteration", i}, {"native", native}, {"expected", expected},
            {"changed", static_cast<qint64>(changed.load())},
            {"synchronized", static_cast<qint64>(synchronized.load())},
            {"sequence", static_cast<qint64>(sequence)},
            {"channel", session->display().value("channel").toInt()},
            {"cache_pointer_matches", session->pixels() == cached[static_cast<std::size_t>(expected)]},
            {"busy", session->busy()}, {"visible", view->isVisible()},
            {"active_focus", view->hasActiveFocus()},
            {"focus_item", window_->activeFocusItem() ? window_->activeFocusItem()->objectName() : QString()},
            {"foreground", GetForegroundWindow() == hwnd}};
      }
      QVERIFY2(ready, "Image change did not reach a synchronized frame");
    }
    const auto summarize = [](std::vector<double> values) {
      QJsonArray raw;
      for (const auto value : values)
        raw.append(value);
      std::sort(values.begin(), values.end());
      return QJsonObject{
          {"samples", static_cast<int>(values.size())},
          {"samples_ms", raw},
          {"p95_ms",
           values[static_cast<std::size_t>(std::ceil(values.size() * .95)) -
                  1]},
          {"p99_ms",
           values[static_cast<std::size_t>(std::ceil(values.size() * .99)) -
                  1]},
          {"max_ms", values.back()}};
    };
    const auto cacheSummary = summarize(cacheTimes),
               inputSummary = summarize(inputTimes);
    evidence_["image_mixed_load"] = QJsonObject{
        {"cache", cacheSummary},
        {"native_input", inputSummary},
        {"cache_samples_during_observed_running_job", cacheOverlap},
        {"input_samples_during_observed_running_job", inputOverlap},
        {"main_thread_max_ms", measured->maxEventMs},
        {"main_thread_over_100ms", measured->over100ms},
        {"slow_events", measured->slowEvents},
        {"dpr", window_->effectiveDevicePixelRatio()},
        {"boundary",
         "40 verified cache-pointer hits (1024x1024 RGB) and 120 native R/G "
         "inputs to frameSwapped after generation sync, including GUI "
         "callback; concurrent eight 2048x2048 PNG writes and eight "
         "10,024-entry directory rescans; not GPU presentation time"}};
    measured->measure = false;
    QTRY_VERIFY_WITH_TIMEOUT(service->job().value("state") == "succeeded",
                             30000);
    QCOMPARE(service->job().value("items").toList().size(), 8);
    for (const auto &entry : service->job().value("items").toList())
      QCOMPARE(entry.toMap().value("status").toString(), QString("succeeded"));
    QVERIFY(cacheOverlap > 0 && inputOverlap > 0);
    QVERIFY2(cacheSummary.value("p95_ms").toDouble() <= 150,
             "Cached image switch P95 exceeds 150ms");
    QVERIFY2(inputSummary.value("p95_ms").toDouble() <= 50,
             "Image native input P95 exceeds 50ms");
    QVERIFY2(inputSummary.value("p99_ms").toDouble() <= 100,
             "Image native input P99 exceeds 100ms");
    QCOMPARE(measured->over100ms, 0);
    desktop_->clearAsset();
#endif
  }
  void thumbnailBudget() {
    auto provider = ThumbnailProvider{};
    for (auto i = 0; i < 800; ++i) {
      const auto id = QString::fromLatin1(
                          QUrl::toPercentEncoding(small_ + "/唯一图片.png").toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)) +
                      "?version=" + QString::number(i);
      const auto response = std::unique_ptr<QQuickImageResponse>(provider.requestImageResponse(id, QSize(8192,8192)));
      QSignalSpy done(response.get(), &QQuickImageResponse::finished);
      QVERIFY(done.wait(10000));
      QVERIFY2(response->errorString().isEmpty(), qPrintable(response->errorString()));
      const auto factory = std::unique_ptr<QQuickTextureFactory>(response->textureFactory());
      const auto image = factory->image();
      QVERIFY(!image.isNull());
      QVERIFY(image.width() <= 160 && image.height() <= 120);
      QVERIFY(provider.cacheKiB() <= 32768);
    }
    evidence_["thumbnail_cache_after_800_revisions_kib"] = provider.cacheKiB();
    provider.clear();
    QCOMPARE(provider.cacheKiB(), 0);
    const auto invalid = std::unique_ptr<QQuickImageResponse>(provider.requestImageResponse(
        QString::fromLatin1(QUrl::toPercentEncoding(small_ + "/missing.png").toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)), {}));
    QSignalSpy done(invalid.get(), &QQuickImageResponse::finished);
    QVERIFY(done.wait(10000));
    QVERIFY(!invalid->errorString().isEmpty());
  }
  void imageThumbnailsWorkflow() {
    const auto folder = root_ + "/thumbnail-ui";
    QVERIFY(QDir().mkpath(folder));
    QImage image(4096,4096,QImage::Format_RGBA8888);
    image.fill(QColor(40,170,80,180));
    const auto source = folder + "/01 中文 #100%25.png";
    QVERIFY(image.save(source));
    image = {};
    QFile bad(folder + "/02 broken.png"); QVERIFY(bad.open(QIODevice::WriteOnly)); bad.write("broken"); bad.close();
    QFile model(folder + "/03 model.obj"); QVERIFY(model.open(QIODevice::WriteOnly)); model.write("# synthetic"); model.close();
    desktop_->navigate(folder); desktop_->filter("",0,0);
    QTRY_COMPARE(desktop_->count(),3); QTRY_VERIFY(idle());
    desktop_->preference("thumbnails",true);
    auto delegate = [&](const QString &name) -> QQuickItem * {
      for (auto *entry : visualItems(window_->contentItem(),"fileDelegate"))
        if (entry->property("fileName").toString() == name) return entry;
      return nullptr;
    };
    QTRY_VERIFY(delegate("01 中文 #100%25.png"));
    auto *first = delegate("01 中文 #100%25.png");
    QCOMPARE(first->property("fileKind").toString(),QString("image"));
    auto thumbs = visualItems(first,"fileThumbnail"); QVERIFY(!thumbs.empty());
    QTRY_COMPARE_WITH_TIMEOUT(thumbs.front()->property("status").toInt(),1,10000);
    QTRY_VERIFY(delegate("02 broken.png"));
    auto failed = visualItems(delegate("02 broken.png"),"thumbnailFailure"); QVERIFY(!failed.empty());
    QTRY_VERIFY(failed.front()->isVisible());
    auto models = visualItems(delegate("03 model.obj"),"fileThumbnail"); QVERIFY(!models.empty());
    QCOMPARE(models.front()->property("source").toUrl(),QUrl{});
    if (!workflowHeadless()) save("thumbnails-ready-and-failed");
    // Switching views releases the grid's images, then returning creates only
    // the current folder's requests. List selection/navigation remain usable.
    desktop_->preference("thumbnails",false);
    QTRY_VERIFY(item("fileList")->isVisible());
    desktop_->select(0); QCOMPARE(desktop_->selectedCount(),1);
    desktop_->preference("thumbnails",true);
    // Earlier source-order cases add inputs to small_; this navigation oracle
    // needs its own directory containing exactly the one expected thumbnail.
    const auto singleFolder = root_ + "/thumbnail-single";
    QVERIFY(QDir().mkpath(singleFolder));
    QVERIFY(QFile::copy(small_ + "/唯一图片.png", singleFolder + "/唯一图片.png"));
    desktop_->navigate(singleFolder);
    QTRY_COMPARE(desktop_->directory(),singleFolder); QTRY_COMPARE(desktop_->count(),1);
    QTRY_VERIFY(delegate("唯一图片.png"));
    auto current = visualItems(delegate("唯一图片.png"),"fileThumbnail"); QVERIFY(!current.empty());
    QTRY_COMPARE_WITH_TIMEOUT(current.front()->property("status").toInt(),1,10000);
    desktop_->preference("thumbnails",false);
  }
  void measuredDeferredDeleteIsSafe() {
    struct SlowDelete final : QObject { ~SlowDelete() override { QThread::msleep(12); } };
    auto *measured=static_cast<MeasuredApplication *>(QCoreApplication::instance());
    const auto wasMeasuring=measured->measure;
    const auto restore=qScopeGuard([&] { measured->measure=wasMeasuring; });
    measured->slowEvents={};
    measured->measure=true;
    QPointer<QObject> object=new SlowDelete;
    object->deleteLater();
    QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    QVERIFY(object.isNull());
    QVERIFY(std::any_of(measured->slowEvents.begin(),measured->slowEvents.end(),[](const auto &value) {
      const auto row=value.toObject();
      return row.value("type").toInt()==QEvent::DeferredDelete && row.value("receiver")=="<destroyed>";
    }));
  }
  void nativeInputPerformance() {
#ifdef Q_OS_WIN
    const auto rescanText=qEnvironmentVariable("FORMAVIEW_NATIVE_RESCANS");
    const int rescans=rescanText.isEmpty()?8:rescanText.toInt();
    QVERIFY(rescans==0 || rescans==1 || rescans==8);
    auto *workspace = item("workspace");
    QVERIFY(workspace);
    workspace->setProperty("tasksVisible", false);
    workspace->setProperty("focusMode", false);
    workspace->setProperty("filesVisible", true);
    window_->resize(1600, 900);
    desktop_->navigate(fixture_);
    QTRY_COMPARE_WITH_TIMEOUT(desktop_->directory(), fixture_, 10000);
    desktop_->filter("", 0, 0);
    QTRY_COMPARE_WITH_TIMEOUT(desktop_->count(), 10024, 10000);
    desktop_->select(0);
    QTRY_VERIFY(idle());
    auto *list = item("fileList");
    QVERIFY(list);
    list->forceActiveFocus();
    window_->requestActivate();
    QTRY_VERIFY(list->hasActiveFocus());
    qInfo() << "native focus" << list << list->isVisible() << desktop_->count()
            << desktop_->currentIndex();
    const auto hwnd = reinterpret_cast<HWND>(window_->winId());
    const auto foregroundThread =
        GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const auto attached =
        foregroundThread != GetCurrentThreadId() &&
        AttachThreadInput(GetCurrentThreadId(), foregroundThread, TRUE);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    SetFocus(hwnd);
    if (attached)
      AttachThreadInput(GetCurrentThreadId(), foregroundThread, FALSE);
    const auto restoreTopmost = qScopeGuard([&] {
      SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    });
    QTRY_VERIFY_WITH_TIMEOUT(GetForegroundWindow() == hwnd, 3000);
    window_->installEventFilter(this);
    QTest::keyClick(window_, Qt::Key_Down);
    QTRY_COMPARE(desktop_->currentIndex(), 1);
    auto *measured =
        static_cast<MeasuredApplication *>(QCoreApplication::instance());
    measured->measure = true;
    const auto stopMeasurement =
        qScopeGuard([measured] { measured->measure = false; });
    std::vector<double> samples;
    QJsonArray stageRows;
    auto pending = false;
    auto frameReady = false;
    auto previous = -1;
    std::atomic<quint64> changed{0}, synchronized{0};
    quint64 sequence{};
    using Clock = std::chrono::steady_clock;
    const auto nowNs=[] {return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();};
    struct FeedbackTimes {std::mutex mutex;quint64 expected{};qint64 syncNs{},swapNs{},selectNs{};};
    const auto feedback=std::make_shared<FeedbackTimes>();
    bool beginActive{},beginExposed{};
    auto start = Clock::now();
    QEventLoop feedbackLoop;
    QTimer feedbackTimeout;
    feedbackTimeout.setSingleShot(true);
    connect(&feedbackTimeout, &QTimer::timeout, &feedbackLoop,
            &QEventLoop::quit);
    auto stateConnection =
        connect(desktop_.get(), &Desktop::changed, this, [&] {
          if (pending && desktop_->currentIndex() != previous) {
            std::lock_guard guard(feedback->mutex);
            if(!feedback->selectNs)feedback->selectNs=nowNs();
            changed.store(sequence);
          }
        });
    auto syncConnection = connect(
        window_, &QQuickWindow::afterSynchronizing, window_,
        [&] { const auto value=changed.load();synchronized.store(value);
          std::lock_guard guard(feedback->mutex);
          if(value && value==feedback->expected && !feedback->syncNs)feedback->syncNs=nowNs();
        }, Qt::DirectConnection);
    const auto directSwapConnection=connect(window_,&QQuickWindow::frameSwapped,window_,[&] {
      std::lock_guard guard(feedback->mutex);
      if(feedback->expected && synchronized.load()==feedback->expected && !feedback->swapNs)feedback->swapNs=nowNs();
    },Qt::DirectConnection);
    auto connection = connect(
        window_, &QQuickWindow::frameSwapped, this,
        [&] {
          if (pending && synchronized.load() == sequence) {
            std::lock_guard feedbackGuard(feedback->mutex);
            // A queued notification for an older frame can arrive after the
            // next generation synchronizes. Require its actual matching swap.
            if(!feedback->swapNs)return;
            samples.push_back(
                std::chrono::duration<double, std::milli>(Clock::now() - start)
                    .count());
            const auto base=std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch()).count();
            const auto offset=[&](qint64 stamp){return stamp?QJsonValue(double(stamp-base)/1e6):QJsonValue(QJsonValue::Null);};
            stageRows.append(QJsonObject{{"iteration",static_cast<int>(sequence)-1},
              {"dispatch_ms",offset(measured->nativeDownNs)}, {"selection_ms",offset(feedback->selectNs)},
              {"synchronized_ms",offset(feedback->syncNs)}, {"swap_signal_ms",offset(feedback->swapNs)},
              {"gui_callback_ms",samples.back()}, {"active_begin",beginActive},{"active_end",window_->isActive()},
              {"exposed_begin",beginExposed},{"exposed_end",window_->isExposed()}});
            pending = false;
            frameReady = true;
            feedbackLoop.quit();
          }
        },
        Qt::QueuedConnection);
    const auto disconnectAll = qScopeGuard([&] {
      disconnect(connection);
      disconnect(syncConnection);
      disconnect(stateConnection);
      disconnect(directSwapConnection);
    });
    // Native OS injection to QML selection repaint, not event-loop timer
    // latency.
    for (auto i = 0; i < 160; ++i) {
      if (rescans && i % (160/rescans) == 0)
        desktop_->refresh();
      previous = desktop_->currentIndex();
      frameReady = false;
      ++sequence;
      measured->nativeDownNs=0;
      {std::lock_guard guard(feedback->mutex);feedback->selectNs=0;feedback->syncNs=0;feedback->swapNs=0;feedback->expected=sequence;}
      beginActive=window_->isActive();beginExposed=window_->isExposed();
      start = Clock::now();
      pending = true;
      INPUT events[2]{};
      events[0].type = INPUT_KEYBOARD;
      events[0].ki.wVk = VK_DOWN;
      events[1] = events[0];
      events[1].ki.dwFlags = KEYEVENTF_KEYUP;
      QCOMPARE(SendInput(2, events, sizeof(INPUT)), UINT(2));
      // Use the normal continuous event dispatcher. QTest's polling waits
      // sleep between processEvents calls and can add artificial input delay.
      feedbackTimeout.start(3000);
      if (!frameReady)
        feedbackLoop.exec();
      feedbackTimeout.stop();
      QVERIFY2(
          desktop_->currentIndex() != previous,
          qPrintable(
              QString("iteration=%1 previous=%2 current=%3 keys=%4 focus=%5")
                  .arg(i)
                  .arg(previous)
                  .arg(desktop_->currentIndex())
                  .arg(receivedKeys_.size())
                  .arg(window_->activeFocusItem()
                           ? window_->activeFocusItem()->objectName()
                           : "null")));
      QVERIFY(frameReady);
    }
    disconnect(connection);
    disconnect(syncConnection);
    disconnect(stateConnection);
    disconnect(directSwapConnection);
    QJsonArray rawSamples;
    for (const auto value : samples)
      rawSamples.append(value);
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&](double p) {
      return samples[static_cast<size_t>(
                         std::ceil(p * static_cast<double>(samples.size()))) -
                     1];
    };
    evidence_["native_input"] = QJsonObject{
        {"samples", static_cast<int>(samples.size())},
        {"samples_ms", rawSamples},
        {"rescan_requests", rescans},
        {"stage_rows", stageRows},
        {"p95_ms", percentile(.95)},
        {"p99_ms", percentile(.99)},
        {"max_ms", samples.back()},
        {"boundary",
         "SendInput start to frameSwapped queued notification after selection "
         "generation synchronized; configured rescan requests scheduled across 160 inputs; includes "
         "GUI callback delivery; not GPU presentation timestamp"}};
    evidence_["main_thread_events"] = QJsonObject{
        {"observations", measured->events},
        {"max_ms", measured->maxEventMs},
        {"over_100ms", measured->over100ms},
        {"boundary",
         "QGuiApplication::notify wall time during native input workload; "
         "excludes idle wait; nested events may overlap"}};
    evidence_["slow_event_diagnostics"] = measured->slowEvents;
    evidence_["gpu_before_release"] = gpuMemory();
    // Preserve measured failures; ordinary hardware is not represented by this
    // host.
    QVERIFY2(percentile(.95) <= 50, "Native input P95 exceeds 50ms");
    QVERIFY2(percentile(.99) <= 100, "Native input P99 exceeds 100ms");
    QCOMPARE(measured->over100ms, 0);
#endif
  }
#ifdef FORMAVIEW_MODEL_UI
  void modelOpenKeepsPreviewPane() {
    try {
    auto *session = qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    desktop_->clearAsset();
    QTRY_VERIFY(!item("modelPane"));
    // Fixture contract: readable OBJ markers with no geometry, so parsing fails.
    for (int i = 0; i < 2; ++i) {
      QFile marker(fixture_ + QString("/asset%1.obj").arg(i));
      QVERIFY(marker.open(QIODevice::ReadOnly));
      QCOMPARE(marker.readAll(), QByteArray("# metadata test\n"));
    }
    desktop_->openPath(fixture_ + "/asset0.obj");
    QTRY_COMPARE(session->path(),fixture_ + "/asset0.obj");
    QTRY_VERIFY_WITH_TIMEOUT(!session->busy() && !session->error().isEmpty(), 10000);
    auto *loader=window_->findChild<QObject *>("modelPaneLoader");
    QVERIFY(loader);
    QTRY_COMPARE(loader->property("status").toInt(),1); // Loader.Ready: Connections installed.
    QTRY_VERIFY(item("modelPane"));
    const QPointer<QQuickItem> pane = item("modelPane");
    QVERIFY(pane);
    auto *view = window_->findChild<ModelView *>("modelViewport");
    QVERIFY(view);
    for (int i = 0; i < 10; ++i) {
      const auto path = fixture_ + QString("/asset%1.obj").arg(i % 2);
      pane->setProperty("wireframe", true);
      pane->setProperty("showGrid", false);
      pane->setProperty("showAxes", false);
      pane->setProperty("showUV", false);
      view->orbit(20, 10);
      view->zoomBy(2);
      session->open(path);
      QVERIFY(session->busy());
      QVERIFY2(pane, "A new model open must not destroy the preview during reset");
      QCOMPARE(item("modelPane"), pane.data());
      QVERIFY(!pane->property("wireframe").toBool());
      QVERIFY(pane->property("showGrid").toBool());
      QVERIFY(pane->property("showAxes").toBool());
      QVERIFY(pane->property("showUV").toBool());
      QVERIFY(view->cameraState().value("automatic").toBool());
      QCOMPARE(view->cameraState().value("yaw").toDouble(), 25.0);
      QCOMPARE(session->path(), path);
      QTRY_VERIFY_WITH_TIMEOUT(!session->busy() && !session->error().isEmpty(), 10000);
      QCOMPARE(item("modelPane"), pane.data());
    }
    const QString sampleRoot = (requireFixture("FORMAVIEW_PSK_FIXTURES") + "/");
    for (const auto &name : {QString("Suzanne.psk"), QString("CS_Sarge_S0_Skelmesh.pskx")}) {
      view->panBy(20, 10);
      view->zoomBy(2);
      session->open(sampleRoot + name);
      QVERIFY(session->busy());
      QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(), 10000);
      QVERIFY(session->error().isEmpty());
      QCOMPARE(item("modelPane"), pane.data());
      QVERIFY(view->cameraState().value("automatic").toBool());
      QCOMPARE(view->cameraState().value("yaw").toDouble(), 25.0);
    }
    session->clear();
    QTRY_VERIFY(pane.isNull());
    session->open(sampleRoot + "Suzanne.psk");
    QVERIFY(session->busy());
    session->open(QString{});
    QVERIFY(!session->busy() && !session->available() && session->path().isEmpty());
    QTest::qWait(100);
    QVERIFY(!session->busy() && !session->available() && session->path().isEmpty());
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelFocusRestoreRails() {
    try {
    const QDir fixtures(requireFixture("FORMAVIEW_MODEL_FIXTURES"));
    auto *session = qobject_cast<ModelSession *>(desktop_->model());
    auto *workspace = item("workspace");
    QVERIFY(session && workspace && fixtures.exists("external.gltf"));
    workspace->setProperty("focusMode", false);
    desktop_->setLanguage("zh-CN");
    desktop_->preference("filesVisible", true);
    desktop_->preference("inspectorMode", "auto");
    desktop_->openPath(fixtures.filePath("external.gltf"));
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(), 10000);
    // Inspection and directory scanning finish independently. Capture the
    // focus-restoration baseline only after both refer to the target asset.
    QTRY_COMPARE(session->path(), fixtures.filePath("external.gltf"));
    QTRY_VERIFY(idle() && desktop_->directory() == fixtures.absolutePath());
    session->setMode(0);
    auto *view = qobject_cast<ModelView *>(item("modelViewport"));
    QVERIFY(view);
    view->orbit(27, -13);
    view->panBy(18, -9);
    view->zoomBy(1.2);
    const auto camera = view->cameraState();
    const auto path = session->path();
    const auto directory = desktop_->directory();
    const auto revision = desktop_->sessionRevision();
    const auto mesh = session->meshIndex(), binding = session->bindingIndex();
    const auto restore = qScopeGuard([&] {
      workspace->setProperty("focusMode", false);
      desktop_->preference("inspectorMode", "auto");
      window_->resize(1600, 900);
      desktop_->clearAsset();
    });
    const auto click = [&](const char *name) {
      auto *control = item(name);
      QVERIFY(control && control->isEnabled());
      QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
                        control->mapToScene({control->width()/2, control->height()/2}).toPoint());
    };
    for (const auto width : {1600, 1280}) {
      window_->resize(width, 900);
      QTest::qWait(100);
      if (window_->width() != width) window_->resize(width, 900);
      for (const auto *railName : {"focusFilesRail", "focusInspectorRail"}) {
        click("globalFocusPreview");
        QTRY_VERIFY(workspace->property("focusMode").toBool());
        QTRY_VERIFY(item("focusFilesRail") && item("focusInspectorRail"));
        // Visibility changes precede the RowLayout polish; wait for the
        // requested rail width instead of observing Tool's implicit width.
        QTRY_COMPARE(item("focusFilesRail")->width(), 36.0);
        QTRY_COMPARE(item("focusInspectorRail")->width(), 36.0);
        QVERIFY(item("focusFilesRail")->height() > 600);
        QVERIFY(!item("navigationRail") && !item("inspectorPane"));
        save(QString("focus-rails-%1-%2").arg(width).arg(railName));
        click(railName);
        QTRY_VERIFY(!workspace->property("focusMode").toBool());
        if (QString::fromLatin1(railName) == "focusFilesRail" && width == 1280) {
          auto *drawer = window_->findChild<QObject *>("fileDrawer");
          QVERIFY(drawer);
          QTRY_VERIFY(drawer->property("opened").toBool());
          QMetaObject::invokeMethod(drawer, "close");
          QTRY_VERIFY(!drawer->property("visible").toBool());
        }
        QTRY_VERIFY(item("inspectorPane"));
        QCOMPARE(view->cameraState(), camera);
        QCOMPARE(session->path(), path);
        QCOMPARE(desktop_->directory(), directory);
        QCOMPARE(desktop_->sessionRevision(), revision);
        QCOMPARE(session->meshIndex(), mesh);
        QCOMPARE(session->bindingIndex(), binding);
        save(QString("focus-restored-%1-%2").arg(width).arg(railName));
      }
    }
    desktop_->preference("inspectorMode", "hidden");
    click("globalFocusPreview");
    click("focusInspectorRail");
    QTRY_VERIFY(item("inspectorPane"));
    QCOMPARE(view->cameraState(), camera);
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelDisplayControls() {
    try {
    const QDir fixtures(requireFixture("FORMAVIEW_MODEL_FIXTURES"));
    auto *session=qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session && fixtures.exists("external.gltf"));
    desktop_->setLanguage("zh-CN");
    desktop_->openPath(fixtures.filePath("external.gltf"));
    QTRY_VERIFY_WITH_TIMEOUT(!session->busy() && session->inspectable() &&
        session->path() == fixtures.filePath("external.gltf"),10000);
    QTRY_VERIFY(modelPanelsReady());
    window_->requestActivate();
    QTRY_COMPARE(QGuiApplication::focusWindow(), window_);
    QSignalSpy presented(window_, &QQuickWindow::frameSwapped);
    window_->update();
    QTRY_VERIFY(!presented.isEmpty());
    auto *view=qobject_cast<ModelView *>(item("modelViewport"));
    QVERIFY(view);
    const auto click=[&](QQuickItem *control) {
      QVERIFY(control && control->isVisible() && control->isEnabled());
      QTest::mouseClick(window_,Qt::LeftButton,Qt::NoModifier,control->mapToScene({control->width()/2,control->height()/2}).toPoint());
    };
    const auto frame=view->frame;
    const auto counts=session->info();
    const auto camera=view->cameraState();
    QVERIFY(view->gridVisible() && view->axesVisible());
    click(item("modelGroundGrid"));
    QTRY_VERIFY(!view->gridVisible());
    QVERIFY(view->axesVisible());
    click(item("modelOriginAxes"));
    QTRY_VERIFY(!view->axesVisible());
    QCOMPARE(view->frame,frame);
    QCOMPARE(view->cameraState(),camera);
    QCOMPARE(session->info().value("visibleTriangles"),counts.value("visibleTriangles"));
    save("model-guides-hidden");
    click(item("modelGroundGrid")); click(item("modelOriginAxes"));
    QTRY_VERIFY(view->gridVisible() && view->axesVisible());
    save("model-guides-visible");
    const auto start=view->mapToScene({view->width()/2,view->height()/2}).toPoint();
    const auto yaw=view->cameraState().value("yaw").toDouble();
    QTest::mousePress(window_,Qt::LeftButton,Qt::NoModifier,start);
    QTest::mouseMove(window_,start+QPoint(80,0),30);
    QTest::mouseRelease(window_,Qt::LeftButton,Qt::NoModifier,start+QPoint(80,0));
    QTRY_VERIFY(view->cameraState().value("yaw").toDouble()<yaw);
    const auto rightYaw=view->cameraState().value("yaw").toDouble();
    QTest::mousePress(window_,Qt::LeftButton,Qt::NoModifier,start);
    QTest::mouseMove(window_,start-QPoint(80,0),30);
    QTest::mouseRelease(window_,Qt::LeftButton,Qt::NoModifier,start-QPoint(80,0));
    QTRY_VERIFY(view->cameraState().value("yaw").toDouble()>rightYaw);
    view->fit();
    const auto &vertices=view->frame->vertices;
    QVERIFY(vertices.size()>=3);
    QVector3D centre;
    for (int i=0;i<3;++i) centre+=QVector3D(vertices[i].x,vertices[i].y,vertices[i].z)/3;
    const auto ndc=view->viewMatrix.map(centre);
    const auto hit=view->mapToScene({(ndc.x()+1)*view->width()/2,(1-ndc.y())*view->height()/2});
    QTest::mouseClick(window_,Qt::LeftButton,Qt::NoModifier,hit.toPoint());
    QTRY_VERIFY(!session->picked().isEmpty());
    QVERIFY(item("globalOpenFolderButton") && !item("chooseBrowseFolder"));
    QVERIFY(item("editLocation") && item("editLocation")->property("iconName").toString().isEmpty());
    save("model-left-click-source-inspection");
    desktop_->clearAsset();
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void genericResourcesRespectCollapse() {
    const auto restore=qScopeGuard([&] { desktop_->clearAsset(); });
    desktop_->openPath(fixture_+"/asset0.obj");
    QTRY_VERIFY(idle() && !desktop_->asset().isEmpty());
    auto *model=qobject_cast<ModelSession *>(desktop_->model());
    QTRY_VERIFY(!model->busy() && !model->preparing());
    QTRY_VERIFY(item("genericResourceSection"));
    auto *section=item("genericResourceSection");
    QVERIFY(desktop_->reportMissingResource(desktop_->sessionRevision(),"review-fold","missing.png"));
    QTRY_VERIFY(section->property("checked").toBool() && item("genericResourceList"));
    const auto point=section->mapToScene({section->width()/2,section->height()/2}).toPoint();
    QTest::mouseClick(window_,Qt::LeftButton,Qt::NoModifier,point);
    QTRY_VERIFY(!section->property("checked").toBool());
    QTRY_VERIFY(!window_->findChild<QQuickItem *>("genericResourceList")->isVisible());
    QCOMPARE(desktop_->resourceIssues().size(),1);
    desktop_->validateResource("review-fold",fixture_+"/still-missing.png");
    QTRY_VERIFY(idle());
    QVERIFY(!section->property("checked").toBool());
    save("review-resources-collapsed-with-report");
    QTest::mouseClick(window_,Qt::LeftButton,Qt::NoModifier,point);
    QTRY_VERIFY(section->property("checked").toBool() && item("genericResourceList"));
  }
  void modelInspectorCollapseRestoresGeometry() {
    try {
    QTest::failOnWarning(QRegularExpression(".*(ReferenceError|TypeError|Binding loop detected).*"));
    auto *session=qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    const auto restore=qScopeGuard([&] { desktop_->clearAsset(); });
    desktop_->setLanguage("zh-CN");
    window_->resize(1600,900);
    desktop_->openPath(requireFixture("FORMAVIEW_ANIMATION_TARGET"));
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),30000);
    QTRY_VERIFY(modelPanelsReady());
    QSignalSpy panelsFrame(window_,&QQuickWindow::frameSwapped);
    window_->update();
    QTRY_VERIFY(!panelsFrame.isEmpty());
    QTRY_VERIFY(item("modelInspector"));
    item("modelInspector")->setProperty("tab",0);
    QTRY_VERIFY(item("modelNodeMatrices"));
    auto *header=item("modelNodeMatrices");
    auto *details=window_->findChild<QQuickItem *>("modelNodeDetails");
    auto *column=item("modelSceneColumn");
    auto *statistics=item("modelStatistics");
    auto *resources=item("modelSourceResources");
    QVERIFY(header && details && column && statistics && resources);
    QTRY_VERIFY(!header->property("checked").toBool());
    QTRY_VERIFY(!details->isVisible());
    QTest::qWait(80);
    const auto initialHeight=column->height();
    const auto initialStatistics=statistics->y();
    const auto initialResources=resources->y();
    save("inspector-collapse-before");
    for(int cycle=0;cycle<3;++cycle) {
      const auto point=header->mapToScene({header->width()/2,header->height()/2}).toPoint();
      QTest::mouseClick(window_,Qt::LeftButton,Qt::NoModifier,point);
      QTRY_VERIFY(header->property("checked").toBool() && details->isVisible());
      QTRY_VERIFY(column->height()>initialHeight+100);
      if(cycle==0) save("inspector-collapse-expanded");
      QTest::mouseClick(window_,Qt::LeftButton,Qt::NoModifier,point);
      QTRY_VERIFY(!header->property("checked").toBool() && !details->isVisible());
      QTRY_VERIFY(std::abs(column->height()-initialHeight)<1);
      QTRY_VERIFY(std::abs(statistics->y()-initialStatistics)<1);
      QTRY_VERIFY(std::abs(resources->y()-initialResources)<1);
    }
    save("inspector-collapse-after");
    session->setMode(3);
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),10000);
    QTRY_VERIFY(window_->findChild<QQuickItem *>("modelLodDetails")->isVisible());
    session->setMode(0);
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),10000);
    QTRY_VERIFY(!window_->findChild<QQuickItem *>("modelLodDetails")->isVisible());
    QTRY_VERIFY(std::abs(column->height()-initialHeight)<1);
    desktop_->clearAsset();
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelSkeletonNavigation() {
    try {
    QTest::failOnWarning(QRegularExpression(".*(ReferenceError|TypeError|Binding loop detected|Unable to assign).*"));
    auto *session=qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    window_->resize(1600,900);
    desktop_->openPath(requireFixture("FORMAVIEW_ANIMATION_TARGET"));
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),30000);
    QTRY_VERIFY(modelPanelsReady());
    QSignalSpy panelsFrame(window_,&QQuickWindow::frameSwapped);
    window_->update();
    QTRY_VERIFY(!panelsFrame.isEmpty());
    QTRY_VERIFY(item("modelInspectorTab1"));
    const auto click=[&](QQuickItem *control) {
      QVERIFY(control && control->isVisible() && control->isEnabled());
      QTest::mouseClick(window_,Qt::LeftButton,Qt::NoModifier,control->mapToScene({control->width()/2,control->height()/2}).toPoint());
    };
    click(item("modelInspectorTab1"));
    QTRY_VERIFY(item("modelBoneTree") && item("modelBoneSearch"));
    auto *tree=item("modelBoneTree"); auto *search=item("modelBoneSearch");
    QVERIFY(tree && search && tree->isVisible());
    QVERIFY(!item("modelBindingMatrices")->property("checked").toBool());
    QVERIFY(session->skeleton()->rowCount()>0);
    auto *view=qobject_cast<ModelView *>(item("modelViewport"));
    QVERIFY(view);
    view->orbit(17,8); view->zoomBy(1.2);
    const auto camera=view->cameraState();
    const auto shoulder=session->skeleton()->match(session->skeleton()->index(0,0),Qt::DisplayRole,"L_Shoulder_a",1,Qt::MatchRecursive | Qt::MatchExactly);
    QVERIFY(!shoulder.isEmpty());
    session->selectSkeletonIndex(session->skeleton()->indexForNode(shoulder.front().data(ModelSkeleton::NodeKey).toString(),shoulder.front().data(ModelSkeleton::JointIndex).toInt()));
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),10000);
    QTest::qWait(100);
    const auto selection=session->skeletonSelection();
    QVERIFY(selection.value("inPalette").toBool());
    QVERIFY(session->frame()->selectionVertices>0);
    QCOMPARE(view->cameraState(),camera);
    const auto shot=captureWindow(window_);
    const auto viewTop=view->mapToScene({0,0})*window_->devicePixelRatio();
    const QRect viewPixels(viewTop.toPoint(),QSize(int(view->width()*window_->devicePixelRatio()),int(view->height()*window_->devicePixelRatio())));
    int orangePixels=0;
    for(int y=viewPixels.top();y<=viewPixels.bottom();++y) for(int x=viewPixels.left();x<=viewPixels.right();++x) {
      const auto colour=shot.pixelColor(x,y);
      if(colour.red()>200 && colour.green()>80 && colour.green()<180 && colour.blue()>20 && colour.blue()<100) ++orangePixels;
    }
    QVERIFY2(orangePixels>5,"Selected source binding segment must actually appear in the real model viewport");
    evidence_["skeleton_selection_orange_pixels"]=orangePixels;
    save("skeleton-real-cloud-selected");
    QQuickItem *candidate=nullptr;
    const auto findRow=[&](auto &&self,QQuickItem *parent)->void {
      for(auto *child:parent->childItems()) {
        const auto key=child->property("nodeKey").toString();
        const auto point=child->mapToItem(tree,{child->width()/2,child->height()/2});
        if (!candidate && child->isVisible() && child->property("jointIndex").isValid() && child->property("jointIndex").toInt()>=0 && key!=selection.value("id").toString() && QRectF(0,0,tree->width(),tree->height()).contains(point)) candidate=child;
        self(self,child);
      }
    };
    findRow(findRow,tree); QVERIFY(candidate);
    const auto clickedId=candidate->property("nodeKey").toString();
    click(candidate);
    QTRY_COMPARE(session->skeletonSelection().value("id").toString(),clickedId);
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),10000);
    QCOMPARE(view->cameraState(),camera);
    session->selectSkeletonIndex(session->skeleton()->indexForNode(selection.value("id").toString(),selection.value("boneIndex").toInt()));
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),10000);
    QVERIFY(QMetaObject::invokeMethod(tree,"reveal",Q_ARG(bool,false)));
    session->skeleton()->setSearchText("not_a_real_bone");
    QCoreApplication::processEvents();
    QCOMPARE(session->skeleton()->rowCount(),0);
    session->skeleton()->setSearchText("");
    QTRY_VERIFY(session->skeleton()->rowCount()>0);
    search->forceActiveFocus(); QInputMethodEvent query; query.setCommitString("not_a_real_bone"); QCoreApplication::sendEvent(search,&query);
    QTRY_COMPARE(session->skeleton()->rowCount(),0);
    QCOMPARE(session->skeletonSelection(),selection);
    click(item("modelBoneLocate"));
    QTRY_VERIFY(session->skeleton()->rowCount()>0);
    QCOMPARE(search->property("text").toString(),QString{});
    click(item("modelBoneHeat"));
    QTRY_VERIFY_WITH_TIMEOUT(session->mode()==2 && session->inspectable(),10000);
    QCOMPARE(session->skeletonSelection(),selection);
    click(item("modelBindingMatrices"));
    QTRY_VERIFY(!visualItems(window_->contentItem(),"modelMatrixValues").isEmpty());
    save("skeleton-real-cloud-advanced");
    auto *bindingDetails=window_->findChild<QQuickItem *>("modelBindingDetailsLoader");
    auto *sourceDetails=window_->findChild<QQuickItem *>("modelSourceDetailsLoader");
    QVERIFY(bindingDetails && sourceDetails && bindingDetails->isVisible());
    QTRY_VERIFY(sourceDetails->y()>100);
    const auto ancestor=session->skeleton()->index(0,0);
    QVERIFY(ancestor.isValid() && ancestor.data(ModelSkeleton::JointIndex).toInt()<0);
    session->selectSkeletonIndex(ancestor);
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),10000);
    QTRY_VERIFY(!bindingDetails->isVisible());
    QTRY_VERIFY(std::abs(sourceDetails->y())<1);
    QVERIFY(!item("modelBoneHeat")->isEnabled());
    save("review-skeleton-ancestor-without-binding-gap");
    session->selectSkeletonIndex(session->skeleton()->indexForNode(selection.value("id").toString(),selection.value("boneIndex").toInt()));
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),10000);
    QTRY_VERIFY(bindingDetails->isVisible() && sourceDetails->y()>100);
    click(item("modelBoneCollapse"));
    QTest::qWait(80);
    save("skeleton-real-cloud-collapsed");
    desktop_->clearAsset();
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelProfessionalUi() {
    try {
    QTest::failOnWarning(QRegularExpression(".*(Binding loop detected|invalid context|Qt.callLater).*"));
    const QDir fixtures(requireFixture("FORMAVIEW_MODEL_FIXTURES"));
    QVERIFY2(fixtures.exists("external.gltf"), "Set FORMAVIEW_MODEL_FIXTURES to the generated M06 fixtures");
    auto *session = qobject_cast<ModelSession *>(desktop_->model());
    auto *workspace = item("workspace");
    QVERIFY(session && workspace);
    workspace->setProperty("settingsVisible", false);
    workspace->setProperty("conversionVisible", false);
    workspace->setProperty("tasksVisible", false);
    workspace->setProperty("focusMode", false);
    desktop_->preference("fileWidth", 236);
    desktop_->preference("filesVisible", true);
    desktop_->preference("inspectorMode", "auto");
    const auto restore = qScopeGuard([&] {
      workspace->setProperty("focusMode", false);
      window_->resize(1600, 900);
      desktop_->setLanguage("zh-CN");
      desktop_->clearAsset();
    });
    const auto click = [&](QQuickItem *control) {
      QVERIFY(control && control->isVisible() && control->isEnabled());
      const auto point = control->mapToScene({control->width()/2, control->height()/2});
      QVERIFY(QRectF(0, 0, window_->width(), window_->height()).contains(point));
      QSignalSpy clicked(control,SIGNAL(clicked()));
      QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, point.toPoint());
      QCOMPARE(clicked.count(),1);
    };
    const auto ready = [&] { return session->inspectable(); };
    desktop_->openPath(fixtures.filePath("external.gltf"));
    QTRY_VERIFY_WITH_TIMEOUT(ready(), 10000);
    QTRY_VERIFY(item("modelPane") && item("modelInspector"));
    const auto originalMesh = session->meshIndex();
    const auto originalBinding = session->bindingIndex();
    QJsonArray screens;
    for (const auto &layout : {QString("default"), QString("focus"), QString("narrow")}) {
      const auto size = layout == "narrow" ? QSize(1280,800) : QSize(1600,900);
      window_->resize(size);
      // Expanded client-area margins settle after the first native resize
      // at fractional DPI. Compare the actual client size after that update.
      QTest::qWait(100);
      if (window_->size() != size) {
        window_->resize(size);
        QTest::qWait(100);
      }
      QCOMPARE(window_->size(), size);
      workspace->setProperty("focusMode", layout == "focus");
      QTest::qWait(80);
      for (const auto &language : {QString("zh-CN"), QString("en"), QString("zh-TW")}) {
        desktop_->setLanguage(language);
        QTest::qWait(100);
        for (int mode = 0; mode < 4; ++mode) {
          const auto name = QByteArray("modelModeTab") + QByteArray::number(mode);
          click(item(name.constData()));
          QTRY_COMPARE(session->mode(), mode);
          QTRY_VERIFY_WITH_TIMEOUT(ready(), 10000);
          QTest::qWait(80);
          QCOMPARE(session->meshIndex(), originalMesh);
          QCOMPARE(session->bindingIndex(), originalBinding);
          QCOMPARE(item("modelPane")->property("narrow").toBool(), layout == "narrow");
          if (mode == 1) {
            QVERIFY(item("uvViewport"));
            QCOMPARE(bool(item("modelViewport")), layout != "narrow");
          }
          const auto screenshot = QString("professional-%1-%2-mode%3").arg(layout,language).arg(mode);
          save(screenshot);
          screens.append(QJsonObject{{"name",screenshot},{"layout",layout},{"language",language},
                                     {"mode",mode},{"width",window_->width()},{"height",window_->height()},
                                     {"dpr",window_->devicePixelRatio()}});
        }
      }
    }
    workspace->setProperty("focusMode", false);
    window_->resize(1600,900);
    desktop_->setLanguage("zh-CN");
    QTest::qWait(100);
    click(item("modelModeTab1"));
    QTRY_VERIFY_WITH_TIMEOUT(ready() && session->mode() == 1, 10000);
    QVERIFY(!session->uvBackground().value("shown").toBool());
    save("professional-uv0-outside-reference");
    auto *uvChoice = item("modelUVChoice");
    QVERIFY(uvChoice);
    uvChoice->forceActiveFocus();
    QTest::keyClick(window_, Qt::Key_Down);
    QTRY_VERIFY_WITH_TIMEOUT(session->uvIndex() == 1 && ready(), 10000);
    QVERIFY(session->uvBackground().value("shown").toBool());
    QCOMPARE(session->frameFor(true)->backgroundVertices, 6U);
    QVERIFY(item("modelUVBackgroundLegend")->property("text").toString().contains("TEXCOORD_1"));
    auto *uvView = qobject_cast<ModelView *>(item("uvViewport"));
    QVERIFY(uvView);
    uvView->panBy(35, -20);
    uvView->zoomBy(1.25);
    const auto manualCamera = uvView->cameraState();
    QVERIFY(!manualCamera.value("automatic").toBool());
    click(item("modelUVMaterialOnly"));
    QTRY_VERIFY_WITH_TIMEOUT(session->materialOnly() && ready(), 10000);
    QCOMPARE(uvView->cameraState(), manualCamera);
    save("professional-uv1-rgb-background");
    desktop_->openPath(fixtures.filePath("two-materials.obj"));
    QTRY_VERIFY_WITH_TIMEOUT(session->path() == fixtures.filePath("two-materials.obj") && ready(), 10000);
    click(item("modelModeTab1"));
    QTRY_VERIFY_WITH_TIMEOUT(session->mode() == 1 && ready(), 10000);
    auto *materialChoice = item("modelUVMaterialChoice");
    QVERIFY(materialChoice);
    materialChoice->forceActiveFocus();
    QTest::keyClick(window_, Qt::Key_End);
    QTRY_VERIFY_WITH_TIMEOUT(session->materialIndex() == session->materials().size() - 1 && ready(), 10000);
    click(item("modelUVMaterialOnly"));
    QTRY_VERIFY_WITH_TIMEOUT(session->materialOnly() && ready(), 10000);
    QCOMPARE(session->info().value("visibleTriangles").toInt(), 1);
    QCOMPARE(session->frameFor(true)->sourceTriangles, 1ULL);
    save("professional-uv-material-range");
    window_->resize(1280,800);
    QTest::qWait(100);
    if (window_->size() != QSize(1280,800)) window_->resize(1280,800);
    QTRY_COMPARE(window_->size(), QSize(1280,800));
    QTRY_VERIFY(item("modelPane")->property("narrow").toBool());
    QVERIFY(item("modelUVMaterialChoice")->isVisible());
    QVERIFY(item("modelUVMaterialOnly")->isVisible());
    save("professional-uv-material-narrow");
    window_->resize(1600,900);
    QTest::qWait(100);
    desktop_->openPath(fixtures.filePath("eight-normalized.gltf"));
    QTRY_VERIFY_WITH_TIMEOUT(session->path()==fixtures.filePath("eight-normalized.gltf") && ready(),10000);
    click(item("modelModeTab2"));
    QTRY_VERIFY_WITH_TIMEOUT(ready() && session->mode()==2,10000);
    session->pick(QMatrix4x4{},QPointF(-.6,-.6));
    QCOMPARE(session->influences().size(),8);
    click(item("modelPointToggle"));
    QTRY_VERIFY(item("modelAllInfluences"));
    QTRY_COMPARE(item("modelAllInfluences")->property("count").toInt(),8);
    auto *influences = item("modelAllInfluences");
    QTRY_VERIFY(influences->height()>=100);
    QTRY_VERIFY(influences->property("contentHeight").toDouble()>influences->height());
    QTest::qWait(100);
    const auto scrollPoint = influences->mapToScene({influences->width()/2,influences->height()/2});
    QWheelEvent scroll(scrollPoint,window_->mapToGlobal(scrollPoint.toPoint()),{},QPoint(0,-960),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
    QCoreApplication::sendEvent(window_,&scroll);
    QTRY_VERIFY(influences->property("contentY").toDouble()>0);
    QTRY_VERIFY(item("modelInfluenceRow7"));
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
      auto *lastRow=item("modelInfluenceRow7");
      if (!lastRow) return false;
      const auto position=lastRow->mapToItem(influences,{0,0});
      return position.y()>=-1 && position.y()+lastRow->height()<=influences->height()+1;
    })(),3000);
    QTest::qWait(60);
    save("professional-all-eight-last-record");
    click(item("modelInspectorTab1"));
    click(item("modelBindingMatrices"));
    QTRY_VERIFY(!visualItems(window_->contentItem(),"modelMatrixValues").isEmpty());
    for (auto *values : visualItems(window_->contentItem(),"modelMatrixValues")) {
      QVERIFY(values->property("readOnly").toBool());
      QCOMPARE(values->property("text").toString().count('\n'),3);
    }
    save("professional-binding-matrices");
    click(item("modelInspectorTab0"));
    click(item("modelNodeMatrices"));
    QTRY_VERIFY(!visualItems(window_->contentItem(),"modelMatrixValues").isEmpty());
    save("professional-node-matrices");
    click(item("modelModeTab1"));
    QTRY_VERIFY_WITH_TIMEOUT(ready() && session->mode()==1,10000);
    QVERIFY(session->uvSets().isEmpty());
    QVERIFY(!session->frameFor(true) || session->frameFor(true)->vertices.empty());
    save("professional-no-uv-source");
    evidence_["professional_ui"] = screens;
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelLodNativeFlow() {
    try {
    QTest::failOnWarning(QRegularExpression(".*(ReferenceError|TypeError|Binding loop detected|invalid context).*"));
    desktop_->setLanguage("zh-CN");
    auto *session=qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    const auto cleanup=qScopeGuard([&]{desktop_->clearAsset();});
    const QString path=requireFixture("FORMAVIEW_LOD_SAMPLE");
    desktop_->openPath(path);
    QTRY_COMPARE_WITH_TIMEOUT(session->path(),path,10000);
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),30000);
    QTRY_VERIFY(modelPanelsReady());
    QSignalSpy panelsFrame(window_,&QQuickWindow::frameSwapped);
    window_->update();
    QTRY_VERIFY(!panelsFrame.isEmpty());
    QVERIFY(session->lodGroups().size()==2 && session->lodLevels().size()==3);
    const auto ensureVisible=[&](QQuickItem *control) {
      QTRY_VERIFY(control->width()>0 && control->height()>0);
      if(auto *scroll=item("modelSceneScroll");scroll && scroll->isAncestorOf(control)) {
        const auto point=control->mapToItem(scroll,{control->width()/2,control->height()/2});
        if(point.y()<0 || point.y()>scroll->height()) {
          auto *flick=scroll->property("contentItem").value<QQuickItem *>();
          QVERIFY(flick);
          flick->setProperty("contentY",flick->property("contentY").toDouble()+point.y()-scroll->height()/2);
          QTest::qWait(50);
        }
      }
    };
    const auto click=[&](const char *name) {
      auto *control=item(name);
      QVERIFY2(control && control->isEnabled(),name);
      ensureVisible(control);
      QSignalSpy clicked(control,SIGNAL(clicked()));
      QTest::mouseClick(window_,Qt::LeftButton,Qt::NoModifier,control->mapToScene({control->width()/2,control->height()/2}).toPoint());
      QCOMPARE(clicked.count(),1);
    };
    click("modelModeTab3");
    QTRY_VERIFY(item("modelLodLevel2"));
    const auto sourceClassification = session->classification();
    for (const auto &language : {QString("zh-CN"), QString("en"), QString("zh-TW")}) {
      desktop_->setLanguage(language);
      click("modelClassifyMesh"); QTRY_VERIFY(item("modelClassificationGroup"));
      save("lod-classification-theme-" + language);
      window_->requestActivate(); QTRY_COMPARE(QGuiApplication::focusWindow(), window_);
      QTest::keyClick(window_, Qt::Key_Escape); QTRY_VERIFY(!item("modelClassificationGroup"));
      QCOMPARE(session->classification(), sourceClassification);
    }
    desktop_->setLanguage("zh-CN");
    click("modelLodLevel2");
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable() && session->lodLevels()[2].toMap().value("current").toBool(),10000);
    const auto levelFrame=session->frame();
    save("lod-native-level2");
    click("modelModeTab1");
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),10000);
    QVERIFY(session->frame()->lines);
    QVERIFY(session->lodLevels()[2].toMap().value("current").toBool());
    click("modelModeTab3");
    QTRY_VERIFY(item("modelCollisionChoice"));
    auto *choice=item("modelCollisionChoice");
    ensureVisible(choice);
    QTest::mouseClick(window_,Qt::LeftButton,Qt::NoModifier,choice->mapToScene({choice->width()/2,choice->height()/2}).toPoint());
    QTest::keyClick(window_,Qt::Key_End);
    QTest::keyClick(window_,Qt::Key_Return);
    QTRY_COMPARE(session->collisionMode(),2);
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),10000);
    const auto hasDrawnGeometry = [&] {
      const auto frame=session->frame();
      return frame->geometry ? std::any_of(frame->draws.begin(),frame->draws.end(),
          [](const auto &draw) { return draw.count > 0; }) : !frame->vertices.empty();
    };
    QVERIFY(!hasDrawnGeometry()); // No source collision: never invent PhysicsAsset.
    save("lod-native-no-exported-collision");
    session->setCollisionMode(0);
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),10000);
    click("modelLodLevel0");
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable() && session->lodLevels()[0].toMap().value("current").toBool(),10000);
    QVERIFY(session->frame()!=levelFrame);
    QVERIFY(hasDrawnGeometry());
    evidence_["lod_native_flow"]=QJsonObject{{"source",path},{"info",QJsonObject::fromVariantMap(session->info())},{"levels",QJsonArray::fromVariantList(session->lodLevels())},{"boundary","Actual QML buttons/ComboBox driven by Qt window mouse/key events; not OS input latency."}};
    desktop_->clearAsset();
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelWeightHeatToggle() {
    try {
    auto *session=qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    const auto path=QDir::fromNativeSeparators(requireFixture("FORMAVIEW_HEAT_FIXTURE"));
    QVERIFY2(QFileInfo::exists(path), "Set FORMAVIEW_HEAT_FIXTURE to a synthetic skinned FBX");
    const auto cleanup=qScopeGuard([&]{desktop_->clearAsset();});
    desktop_->setLanguage("zh-CN");
    const auto payload=[&] {
      const auto frame=session->frame();
      return frame && frame->geometry ? frame->geometry : frame;
    };
    const auto click=[&](const char *name) {
      QSignalSpy presented(window_, &QQuickWindow::frameSwapped);
      window_->update(); QTRY_VERIFY(!presented.isEmpty());
      auto *control=item(name);
      QVERIFY(control && control->isVisible() && control->isEnabled());
      QTest::mouseClick(window_,Qt::LeftButton,Qt::NoModifier,
                       control->mapToScene({control->width()/2,control->height()/2}).toPoint());
    };
    const auto open=[&](const QString &source) {
      desktop_->openPath(source);
      QTRY_COMPARE_WITH_TIMEOUT(session->path(),source,10000);
      QTRY_VERIFY_WITH_TIMEOUT(session->inspectable() && modelPanelsReady(),10000);
    };
    open(path);
    click("modelInspectorTab1");
    QVERIFY(session->skeletonSelection().value("inPalette").toBool());
    const auto normal=payload();
    QVERIFY(normal && !normal->heat && normal->lit);
    for (int i=0;i<2;++i) {
      click("modelBoneHeat");
      QTRY_VERIFY(session->inspectable() && payload()->heat);
      QCOMPARE(session->mode(),2);
      click("modelBoneHeat");
      QTRY_COMPARE(session->mode(),0);
      QTRY_VERIFY(session->inspectable() && !payload()->heat && payload()->lit);
      QVERIFY(!item("modelBoneHeat")->property("checked").toBool());
      QCOMPARE(payload()->vertices.size(),normal->vertices.size());
      QCOMPARE(payload()->rgba,normal->rgba);
      for (std::size_t v=0;v<normal->vertices.size();++v) {
        const auto &before=normal->vertices[v], &after=payload()->vertices[v];
        QCOMPARE(after.r,before.r); QCOMPARE(after.g,before.g); QCOMPARE(after.b,before.b);
        QCOMPARE(after.alpha,before.alpha);
      }
    }
    for (const auto *target : {"modelInspectorTab0", "modelInspectorTab2", "modelModeTab0", "modelModeTab1"}) {
      click("modelInspectorTab1");
      click("modelBoneHeat");
      QTRY_VERIFY(session->inspectable() && payload()->heat);
      QVERIFY(item("modelBoneHeat")->property("checked").toBool());
      QCOMPARE(item("modelBoneHeat")->property("text").toString(),QString("关闭权重热力"));
      click(target);
      QTRY_VERIFY(session->mode()!=2 && session->inspectable() && !payload()->heat);
    }
    click("modelInspectorTab1"); click("modelBoneHeat");
    QTRY_COMPARE(session->mode(),2);
    desktop_->clearAsset();
    QCOMPARE(session->mode(),0);
    QVERIFY(!session->frame());
    open(path);
    QCOMPARE(session->mode(),0);
    click("modelInspectorTab1"); click("modelBoneHeat");
    QTRY_VERIFY(session->inspectable() && payload()->heat);
    const auto unweighted=root_+"/无权重.obj";
    QFile obj(unweighted); QVERIFY(obj.open(QIODevice::WriteOnly));
    obj.write("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"); obj.close();
    open(unweighted);
    QCOMPARE(session->mode(),0);
    QVERIFY(!payload()->heat);
    click("modelInspectorTab1");
    QVERIFY(!item("modelBoneHeat")->isEnabled());
    QVERIFY(item("modelBoneHeatFeedback"));
    click("modelModeTab2");
    QTRY_COMPARE(session->mode(),2);
    QVERIFY(item("modelBoneHeat")->isEnabled());
    click("modelBoneHeat");
    QTRY_VERIFY(session->mode()==0 && session->inspectable() && !payload()->heat);
    save("weight-heat-exited-unweighted");
    open(path);
    click("modelInspectorTab1"); click("modelBoneHeat");
    QTRY_VERIFY(session->inspectable() && payload()->heat);
    session->open(root_+"/missing-model.fbx");
    QTRY_VERIFY(!session->busy());
    QCOMPARE(session->mode(),0);
    QVERIFY(!session->error().isEmpty());
    open(path);
    QVERIFY(!payload()->heat);
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelMultipleBindingsNativeFlow() {
    try {
    auto *session=qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    const auto path=QDir::fromNativeSeparators(requireFixture("FORMAVIEW_MULTIBINDING_FIXTURE"));
    if(path.isEmpty()) QSKIP("Optional two-binding FBX fixture was not configured");
    QVERIFY2(QFileInfo::exists(path),"Set FORMAVIEW_MULTIBINDING_FIXTURE to the independently generated two-skin FBX fixture");
    const auto cleanup=qScopeGuard([&]{desktop_->clearAsset();});
    desktop_->openPath(path);
    QTRY_COMPARE_WITH_TIMEOUT(session->path(),path,10000);
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),30000);
    QTRY_VERIFY(modelPanelsReady());
    QSignalSpy panelsFrame(window_,&QQuickWindow::frameSwapped);
    window_->update();
    QTRY_VERIFY(!panelsFrame.isEmpty());
    QCOMPARE(session->bindings().size(),2);
    QCOMPARE(session->bindingIndex(),-1);
    auto *tab=item("modelInspectorTab1");
    QVERIFY(tab);
    QTest::mouseClick(window_,Qt::LeftButton,Qt::NoModifier,tab->mapToScene({tab->width()/2,tab->height()/2}).toPoint());
    QTRY_VERIFY(item("modelSkinBindingChoice"));
    const auto choose=[&](Qt::Key key) {
      auto *choice=item("modelSkinBindingChoice");
      QVERIFY(choice);
      QTest::mouseClick(window_,Qt::LeftButton,Qt::NoModifier,choice->mapToScene({choice->width()/2,choice->height()/2}).toPoint());
      QTest::keyClick(window_,key); QTest::keyClick(window_,Qt::Key_Return);
    };
    choose(Qt::Key_Home);
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable() && session->bindingIndex()==0,10000);
    const auto firstId=session->bindingInfo().value("id").toString();
    const auto firstFrame=session->frame();
    QVERIFY(!firstId.isEmpty());
    choose(Qt::Key_End);
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable() && session->bindingIndex()==1,10000);
    QVERIFY(session->bindingInfo().value("id").toString()!=firstId);
    QVERIFY(session->frame()!=firstFrame);
    QCOMPARE(session->bones().size(),2);
    save("two-bindings-second-skin");
    session->setMode(2);
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),10000);
    QCOMPARE(session->mode(),2);
    const auto heatPayload=[&] {
      const auto frame=session->frame();
      return frame && frame->geometry ? frame->geometry : frame;
    };
    // GPU poses reference immutable geometry; heat/weights live in that payload.
    QTRY_VERIFY_WITH_TIMEOUT(heatPayload() && heatPayload()->heat,10000);
    const auto secondHeat=heatPayload();
    choose(Qt::Key_Home);
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable() && session->bindingIndex()==0,10000);
    QTRY_VERIFY_WITH_TIMEOUT(heatPayload() && heatPayload()->heat && heatPayload()!=secondHeat,10000);
    QCOMPARE(heatPayload()->vertices.size(),secondHeat->vertices.size());
    QVERIFY(!secondHeat->vertices.empty());
    QVERIFY(std::abs(heatPayload()->vertices[0].weight-secondHeat->vertices[0].weight)>.5F);
    evidence_["multiple_bindings_native"]=QJsonObject{{"source",path},{"first_id",firstId},{"skin_count",2},{"boundary","Actual Qt QML ComboBox selection and heat frames; fixture is an explicitly hand-modified FBX, not claimed as a DCC export."}};
    desktop_->clearAsset();
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelMixedQueuePerformance() {
    try {
#ifdef FORMAVIEW_IMAGE_UI
    auto *session=qobject_cast<ModelSession *>(desktop_->model());
    auto *service=qobject_cast<ImageJobs *>(desktop_->imageJobs());
    QVERIFY(session && service);
    const auto path=QDir::fromNativeSeparators(requireFixture("FORMAVIEW_PERFORMANCE_MODEL"));
    if(path.isEmpty()) QSKIP("Optional typical performance fixture was not configured");
    QVERIFY2(QFileInfo::exists(path),"Set FORMAVIEW_PERFORMANCE_MODEL to the 216k-triangle / 256-joint fixture");
    desktop_->openPath(path);
    QTRY_COMPARE_WITH_TIMEOUT(session->path(),path,10000);
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),30000);
    QTRY_VERIFY(window_->findChild<ModelView *>("modelViewport"));
    auto *view=window_->findChild<ModelView *>("modelViewport");
    QImage image(1024,1024,QImage::Format_RGB32);
    quint32 random=0x4d3036;
    for(int y=0;y<image.height();++y) {
      auto *row=reinterpret_cast<QRgb *>(image.scanLine(y));
      for(int x=0;x<image.width();++x) {random=random*1664525U+1013904223U;row[x]=0xff000000U|(random&0xffffffU);}
    }
    const auto input=small_+"/model-mixed.bmp",output=root_+"/model-mixed-output";
    QVERIFY(image.save(input)); QVERIFY(QDir().mkpath(output));
    const QVariantMap scope{{"input_roots",QVariantList{small_}},{"output_roots",QVariantList{output}},{"allow_overwrite",false}};
    QStringList inputs; for(int i=0;i<8;++i) inputs.append(input);
    service->preflight(inputs,scope,{{"target","png"},{"png_compression",9}},"model-mixed-{index}.{ext}","skip");
    QTRY_VERIFY_WITH_TIMEOUT(!service->busy() && !service->plan().value("state").toString().isEmpty() && service->plan().value("state")!="planning",30000);
    QVERIFY2(service->plan().value("state")=="ready",qPrintable(QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(service->plan())).toJson())));
    struct Signals {std::atomic<quint64> command{},synced{},swapped{};std::atomic<qint64> swapNs{};};
    const auto telemetrySignals=std::make_shared<Signals>();
    const auto sync=connect(window_,&QQuickWindow::afterSynchronizing,window_,[telemetrySignals]{telemetrySignals->synced.store(telemetrySignals->command.load());},Qt::DirectConnection);
    const auto swap=connect(window_,&QQuickWindow::frameSwapped,window_,[telemetrySignals]{telemetrySignals->swapNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());telemetrySignals->swapped.store(telemetrySignals->synced.load());},Qt::DirectConnection);
    const auto disconnectAll=qScopeGuard([&]{disconnect(sync);disconnect(swap);});
    auto *measured=static_cast<MeasuredApplication *>(QCoreApplication::instance());
    measured->events=0;measured->maxEventMs=0;measured->over100ms=0;measured->slowEvents={};measured->measure=true;
    const auto stop=qScopeGuard([&]{measured->measure=false;});
    service->submit(true); // Explicitly acknowledge this synthetic workload's reported conversion losses.
    int concurrent=0;std::vector<double> samples;
    for(quint64 command=1;command<=240;++command) {
      const auto state=service->job().value("state").toString();
      if(state=="running" || state=="queued") ++concurrent;
      const auto started=std::chrono::steady_clock::now();
      telemetrySignals->command.store(command);view->orbit(command%2?-.5:.5,.1);
      QEventLoop loop;QTimer poll,deadline;poll.setInterval(1);deadline.setSingleShot(true);
      connect(&poll,&QTimer::timeout,&loop,[&]{if(telemetrySignals->swapped.load()>=command)loop.quit();});
      connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);
      poll.start();deadline.start(3000);loop.exec();
      QVERIFY(telemetrySignals->swapped.load()>=command);
      const auto startNs=std::chrono::duration_cast<std::chrono::nanoseconds>(started.time_since_epoch()).count();
      samples.push_back(double(telemetrySignals->swapNs.load()-startNs)/1e6);
    }
    QTRY_COMPARE_WITH_TIMEOUT(service->job().value("state").toString(),QString("succeeded"),30000);
    QJsonArray rawSamples;for(const auto value:samples)rawSamples.append(value);
    std::sort(samples.begin(),samples.end());
    const auto p95=samples[227],p99=samples[237];
    evidence_["model_mixed_queue"]=QJsonObject{{"samples",240},{"samples_ms",rawSamples},{"while_job_active",concurrent},{"p95_ms",p95},{"p99_ms",p99},{"max_ms",samples.back()},{"max_main_thread_event_ms",measured->maxEventMs},{"main_thread_over_100ms",measured->over100ms},{"job",QJsonObject::fromVariantMap(service->job())},{"boundary","Qt camera command to first frameSwapped after matching synchronization; actual image conversion queue; not OS input or physical display presentation."}};
    QVERIFY(concurrent>=30); QVERIFY(p95<=50 && p99<=100); QCOMPARE(measured->over100ms,0);
    desktop_->clearAsset();
#endif
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void stressTraceCapturesNestedAndLoss() {
    struct Receiver : QObject {
      bool event(QEvent *e) override {
        if(e->type()==QEvent::User) {
          QEvent child(static_cast<QEvent::Type>(QEvent::User+1));
          QCoreApplication::sendEvent(this,&child);
        }
        return true;
      }
    } receiver;
    auto *measured=static_cast<MeasuredApplication *>(QCoreApplication::instance());
    stress_trace::Trace trace({32,8,8,8});
    const auto restore=qScopeGuard([&]{measured->measure=false;measured->stressTrace=nullptr;});
    measured->stressTrace=&trace; measured->measure=true;
    trace.phase(0,stress_trace::Phase::Setup); trace.publish(42);
    QEvent event(QEvent::User); QCoreApplication::sendEvent(&receiver,&event);
    auto *deleted=new QObject; QPointer<QObject> guarded(deleted);
    deleted->deleteLater(); QCoreApplication::sendPostedEvents(deleted,QEvent::DeferredDelete);
    measured->measure=false; measured->stressTrace=nullptr;
    trace.render(true); trace.render(false); trace.stop();
    QVERIFY(!guarded); QVERIFY(trace.complete()); QCOMPARE(trace.callCount(),std::size_t(3));
    QCOMPARE(trace.call(0).id,std::uint64_t(1)); QCOMPARE(trace.call(0).parent,std::uint64_t(0));
    QCOMPARE(trace.call(1).parent,trace.call(0).id); QCOMPARE(trace.call(1).depth,2u);
    QVERIFY(trace.call(0).enter<=trace.call(1).enter && trace.call(1).exit<=trace.call(0).exit);
    QCOMPARE(trace.call(2).type,int(QEvent::DeferredDelete));
    QCOMPARE(trace.renderRecord(1).sync,trace.renderRecord(0).id);
    QCOMPARE(trace.renderRecord(1).publication,std::uint64_t(1));
    QVERIFY(trace.write(root_+"/stress-trace-calibration.jsonl"));
    stress_trace::Trace tiny({1,1,1,1});
    tiny.phase(0,stress_trace::Phase::Setup); tiny.phase(0,stress_trace::Phase::Open);
    tiny.publish(1); tiny.publish(2); tiny.render(true); tiny.render(false);
    const auto parent=tiny.enter(&receiver,1,stress_trace::Clock::now());
    const auto child=tiny.enter(&receiver,1,stress_trace::Clock::now());
    tiny.leave(child,stress_trace::Clock::now()); tiny.leave(parent,stress_trace::Clock::now());
    tiny.stop(); QVERIFY(!tiny.complete()); QCOMPARE(tiny.callLoss(),std::uint64_t(1));
    const auto lost=tiny.summary();
    QCOMPARE(lost.value("operationLoss").toInteger(),1);
    QCOMPARE(lost.value("publicationLoss").toInteger(),1);
    QCOMPARE(lost.value("renderLoss").toInteger(),1);
    stress_trace::Trace incomplete({1,1,1,1});
    incomplete.enter(&receiver,1,stress_trace::Clock::now()); incomplete.stop();
    QVERIFY(!incomplete.complete());
    stress_trace::Trace unwound({1,1,1,1});
    const auto throwing=unwound.enter(&receiver,1,stress_trace::Clock::now());
    unwound.leave(throwing,stress_trace::Clock::now(),true); unwound.stop();
    QVERIFY(!unwound.complete());
    evidence_["stress_trace_calibration"]=QJsonObject{{"normal",trace.summary()},
      {"boundedLoss",lost},{"incomplete",incomplete.summary()},{"unwound",unwound.summary()}};
  }
  void stressTraceOverhead() {
    struct Receiver : QObject {
      std::uint64_t received{};
      bool event(QEvent *) override { ++received; return true; }
    } receiver;
    auto *measured=static_cast<MeasuredApplication *>(QCoreApplication::instance());
    const auto restore=qScopeGuard([&]{measured->measure=false;measured->stressTrace=nullptr;});
    QEvent event(QEvent::User);
    for(int i=0;i<5000;++i) QCoreApplication::sendEvent(&receiver,&event);
    QJsonArray runs;
    constexpr int count=50000;
    for(int block=0;block<3;++block) for(bool enabled:{false,true,true,false}) {
      stress_trace::Trace trace({count,1,1,1});
      measured->stressTrace=enabled?&trace:nullptr; measured->measure=true;
      const auto started=stress_trace::Clock::now();
      for(int i=0;i<count;++i) QCoreApplication::sendEvent(&receiver,&event);
      const double ns=std::chrono::duration<double,std::nano>(stress_trace::Clock::now()-started).count();
      measured->measure=false; measured->stressTrace=nullptr; trace.stop();
      QVERIFY(trace.complete()); QCOMPARE(trace.callCount(),enabled?std::size_t(count):std::size_t(0));
      runs.append(QJsonObject{{"block",block},{"enabled",enabled},{"events",count},
        {"nsPerEvent",ns/count},{"elapsedMs",ns/1e6},{"trace",trace.summary()}});
    }
    QCOMPARE(receiver.received,std::uint64_t(605000));
    evidence_["stress_trace_overhead"]=QJsonObject{{"runs",runs},
      {"boundary","Same empty synchronous sendEvent receiver, ABBA x3; both use the legacy measured notify path. Buffer allocation and JSON serialization excluded. This is a synthetic observer-cost calibration, not a correction to pressure-test latency or proof of zero workload perturbation."}};
  }
  void recentHistoryPersistsOnClose() {
    QTRY_VERIFY(idle());
    const auto previous=QSettings{}.value("recent");
    const auto restore=qScopeGuard([&]{QSettings{}.setValue("recent",previous);});
    QStringList expected;
    {
      Desktop history;
      for(const auto &path:{small_,fixture_,small_}) {
        history.navigate(path);
        QTRY_COMPARE(history.directory(),path);
        QTRY_VERIFY(!history.busy() && history.activeTasks()==0);
      }
      expected=history.recent();
      QCOMPARE(expected[0],small_);
      QCOMPARE(expected[1],fixture_);
    } // A close drains all ordered snapshots, including the last navigation.
    QCOMPARE(QSettings{}.value("recent").toStringList(),expected);
  }
  void preparedModelTreesTransferOwnership() {
    ModelDocument document;
    document.name="prepared tree";
    document.skeletonOnly=true;
    document.boneNames={"root","child","child"};
    document.boneParents={-1,0,0};
    document.nodes={QVariantMap{{"id","root"},{"name","root"}},
      QVariantMap{{"id","child"},{"name","child"},{"parent","root"}}};
    auto future=std::async(std::launch::async,[&document] {
      return std::make_pair(ModelScene::prepare(&document),ModelSkeleton::prepare(&document,0,-1));
    });
    auto prepared=future.get();
    QVERIFY(prepared.first.root && !prepared.first.root->model());
    for(auto *node:prepared.second.nodes) QVERIFY(node && !node->model());
    ModelScene scene;
    ModelSkeleton skeleton;
    scene.replace(std::move(prepared.first));
    skeleton.replace(std::move(prepared.second));
    QCOMPARE(scene.rowCount(),1);
    const auto root=scene.index(0,0,scene.index(0,0));
    QCOMPARE(root.data(ModelScene::NodeId).toString(),QString("root"));
    QCOMPARE(scene.rowCount(root),1);
    const auto child=skeleton.indexForNode("bone/2",2);
    QVERIFY(child.isValid());
    QCOMPARE(child.data(ModelSkeleton::JointIndex).toInt(),2);
    QVERIFY(child.data().toString().contains("#2"));
    QCOMPARE(skeleton.details("bone/2",2).value("parentIndex").toInt(),0);
    QVERIFY(!skeleton.details("bone/2",2).value("inPalette").toBool());
    skeleton.setSearchText("child");
    QVERIFY(skeleton.indexForNode("bone/2",2).isValid());
    QPersistentModelIndex old=child;
    skeleton.replace(ModelSkeleton::Prepared{});
    scene.replace(ModelScene::Prepared{});
    QVERIFY(!old.isValid());
    QCOMPARE(skeleton.rowCount(),0);
    QCOMPARE(scene.rowCount(),0);
  }
  void folderBreadcrumbsSurviveUnrelatedChanges() {
    desktop_->navigate(small_);
    QTRY_COMPARE(desktop_->directory(),small_);
    QTRY_VERIFY(idle());
    QTest::qWait(50);
    const auto crumbs=visualItems(window_->contentItem(),"breadcrumbName");
    QVERIFY(!crumbs.isEmpty());
    std::vector<QPointer<QQuickItem>> original;
    for(auto *crumb:crumbs) original.emplace_back(crumb);
    auto *navigation=item("folderNavigation");
    QVERIFY(navigation);
    QSignalSpy changed(navigation,SIGNAL(crumbsChanged()));
    // The real shared notifier also fires for task/file state changes while
    // the directory is identical; it must not rebuild the breadcrumb model.
    for(int i=0;i<5;++i) QVERIFY(QMetaObject::invokeMethod(desktop_.get(),"changed",Qt::DirectConnection));
    QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    QCOMPARE(changed.count(),0);
    for(const auto &crumb:original) QVERIFY(crumb);
    QCOMPARE(visualItems(window_->contentItem(),"breadcrumbName"),crumbs);
    desktop_->navigate(root_);
    QTRY_COMPARE(desktop_->directory(),root_);
    QTRY_VERIFY(idle());
    QVERIFY(changed.count()>0);
  }
  void modelPanelIncubationCanBeCancelled() {
    try {
    auto *session=qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    desktop_->clearAsset();
    QTRY_VERIFY(!item("modelPane") && !item("modelInspector"));
    auto *paneLoader=window_->findChild<QQuickItem *>("modelPaneLoader");
    auto *inspectorLoader=window_->findChild<QQuickItem *>("modelInspectorLoader");
    QVERIFY(paneLoader && inspectorLoader);
    QQmlIncubationController controlled;
    auto *original=engine_->incubationController();
    engine_->setIncubationController(&controlled);
    const auto restore=qScopeGuard([&]{
      desktop_->clearAsset();
      engine_->setIncubationController(original);
    });
    const QString compactModel=(requireFixture("FORMAVIEW_PSK_FIXTURES") + "/Suzanne.psk");
    desktop_->openPath(compactModel);
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),30000);
    // Deliberately withhold the incubation budget. Both panels must yield,
    // even though the model worker has already finished loading its data.
    QTRY_COMPARE(paneLoader->property("status").toInt(),2); // Loader.Loading
    QTRY_COMPARE(inspectorLoader->property("status").toInt(),2);
    QVERIFY(controlled.incubatingObjectCount()>0);
    QVERIFY(!item("modelViewport") && !item("modelInspector"));
    desktop_->clearAsset();
    QTRY_COMPARE(controlled.incubatingObjectCount(),0);
    QCOMPARE(paneLoader->property("status").toInt(),0); // Loader.Null
    QCOMPARE(inspectorLoader->property("status").toInt(),0);
    // A superseded request must not resurrect either cancelled component.
    desktop_->openPath(requireFixture("FORMAVIEW_ANIMATION_TARGET"));
    desktop_->openPath(compactModel);
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable() && session->path()==compactModel,30000);
    const auto panelsReady=[&] {
      controlled.incubateFor(5);
      return paneLoader->property("status").toInt()==1 &&
             inspectorLoader->property("status").toInt()==1;
    };
    QTRY_VERIFY_WITH_TIMEOUT(panelsReady(),10000);
    QVERIFY(item("modelViewport") && item("modelInspector"));
    QCOMPARE(session->path(),compactModel);
    QCOMPARE(item("modelPane")->property("session").value<QObject *>(),session);
    QCOMPARE(item("modelInspector")->property("session").value<QObject *>(),session);
    evidence_["model_panel_incubation"]=QJsonObject{{"cancelledWhileLoading",true},
      {"supersededWithoutStalePanel",true},{"panelsReadyAfterBudget",true}};
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelLoadResourceStress() {
    try {
    auto *session=qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    const QString large=requireFixture("FORMAVIEW_ANIMATION_TARGET");
    const QString compactModel=(requireFixture("FORMAVIEW_PSK_FIXTURES") + "/Suzanne.psk");
    auto *measured=static_cast<MeasuredApplication *>(QCoreApplication::instance());
    measured->events=0; measured->maxEventMs=0; measured->over100ms=0; measured->slowEvents={};
    using Phase=stress_trace::Phase;
    const auto trace=std::make_shared<stress_trace::Trace>();
    trace->phase(-1,Phase::Setup);
    const auto publication=connect(session,&ModelSession::frameChanged,this,[trace,session] {
      const auto frame=session->frame(); trace->publish(frame?frame->revision:0);
    });
    const auto synchronized=connect(window_,&QQuickWindow::afterSynchronizing,window_,
      [trace]{trace->render(true);},Qt::DirectConnection);
    const auto swapped=connect(window_,&QQuickWindow::frameSwapped,window_,
      [trace]{trace->render(false);},Qt::DirectConnection);
    measured->stressTrace=trace.get(); measured->measure=true;
    bool finished=false,traceComplete=false;
    const auto finish=[&] {
      if(finished) return;
      finished=true; measured->measure=false; measured->stressTrace=nullptr;
      disconnect(publication); disconnect(synchronized); disconnect(swapped); trace->stop();
      const auto before=stress_trace::Clock::now();
      const auto path=root_+"/stress-trace.jsonl";
      const bool written=trace->write(path);
      const double exportMs=std::chrono::duration<double,std::milli>(stress_trace::Clock::now()-before).count();
      traceComplete=written && trace->complete() && trace->entered()==std::uint64_t(measured->events) && trace->swaps()>0;
      auto summary=trace->summary(); summary["complete"]=traceComplete;
      summary["writeSucceeded"]=written; summary["path"]=path;
      summary["serializationMs"]=exportMs; summary["measuredNotifyEvents"]=measured->events;
      summary["bytesWritten"]=written?QJsonValue(QFileInfo(path).size()):QJsonValue(QJsonValue::Null);
      evidence_["stress_trace"]=summary;
    };
    const auto stop=qScopeGuard([&]{finish();desktop_->clearAsset();});
    QElapsedTimer elapsed; elapsed.start();
    QJsonArray cycles;
    const auto configuredCycles=qEnvironmentVariable("FORMAVIEW_STRESS_CYCLES");
    const auto cycleCount=configuredCycles.isEmpty()?60:configuredCycles.toInt();
    QVERIFY(cycleCount>=60 && cycleCount<=600);
    for(int cycle=0;cycle<cycleCount;++cycle) {
      trace->phase(cycle,Phase::Open);
      desktop_->openPath(large);
      if(cycle%3==0) {
        trace->phase(cycle,Phase::Cancel);
        session->cancel();
        QVERIFY(!session->busy() && !session->frame());
        trace->phase(cycle,Phase::Supersede);
        desktop_->openPath(compactModel);
      } else if(cycle%3==1) {
        trace->phase(cycle,Phase::Supersede); desktop_->openPath(compactModel);
      }
      trace->phase(cycle,Phase::WaitInspect);
      QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),30000);
      QCOMPARE(session->path(),cycle%3==2?large:compactModel);
      trace->phase(cycle,Phase::WaitView);
      QTRY_VERIFY(window_->findChild<ModelView *>("modelViewport"));
      QPointer<ModelView> view=window_->findChild<ModelView *>("modelViewport");
      QTRY_VERIFY(view->isVisible());
      trace->phase(cycle,Phase::Orbit);
      for(int i=0;i<10;++i) view->orbit(i%2?-.5:.5,.1);
      trace->phase(cycle,Phase::WaitRender);
      QTest::qWait(500);
      trace->phase(cycle,Phase::Clear);
      desktop_->clearAsset();
      trace->phase(cycle,Phase::WaitDestroyed);
      QTRY_VERIFY(!view);
      trace->phase(cycle,Phase::WaitClear);
      QTest::qWait(20);
      QVERIFY(!session->busy() && !session->available() && !session->frame());
      trace->phase(cycle,Phase::Resources);
      const auto memory=formaview::process_memory();
      QVERIFY(memory);
      sampleGpu();
      cycles.append(QJsonObject{{"cycle",cycle},{"elapsed_ms",elapsed.elapsed()},{"rss",static_cast<qint64>(memory->working_set_bytes)},{"private",static_cast<qint64>(memory->private_bytes)},{"gpu",gpuMemory()}});
    }
    evidence_["model_load_resource_stress"]=QJsonObject{{"cycles",cycles},{"camera_commands",cycleCount*10},{"events",measured->events},{"max_event_ms",measured->maxEventMs},{"over_100ms",measured->over100ms},{"slow_events",measured->slowEvents},{"boundary","Repeated actual desktop load/cancel/supersede/render/destroy cycles; cycle count is recorded in cycles. Qt input, not OS input. Main-thread event dispatch timing only; process allocators/driver caches need not return RSS to initial baseline."}};
    finish(); // Freeze and export before any assertion can return from the test.
    QCOMPARE(measured->over100ms,0);
    QVERIFY2(traceComplete,"Stress diagnostic records lost, incomplete, unwound, or not saved; cannot count as a pass");
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelPreviewIntegration() {
    try {
    QTest::failOnWarning(QRegularExpression(".*index is not valid.*"));
    auto *session = qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    const QStringList samples{
        requireFixture("FORMAVIEW_ANIMATION_TARGET"),
        (requireFixture("FORMAVIEW_PSK_FIXTURES") + "/Suzanne.psk"),
        (requireFixture("FORMAVIEW_PSK_FIXTURES") + "/CS_Sarge_S0_Skelmesh.pskx")};
    QJsonArray reports;
    for (const auto &path : samples) {
      QVERIFY2(QFileInfo::exists(path), qPrintable(path));
      desktop_->openPath(path);
      QTRY_COMPARE_WITH_TIMEOUT(session->path(), path, 10000);
      QTRY_VERIFY_WITH_TIMEOUT(session->available() || !session->error().isEmpty(), 30000);
      QVERIFY2(session->error().isEmpty(), qPrintable(session->error()));
      QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),10000);
      QVERIFY(session->frame());
      QVERIFY(session->frame()->vertices.size() >= 3);
      QTRY_VERIFY(window_->findChild<ModelView *>("modelViewport"));
      QPointer<ModelView> view = window_->findChild<ModelView *>("modelViewport");
      QVERIFY(view->isVisible());
      QTest::qWait(300);
      QVERIFY(view);
      const auto name = QFileInfo(path).completeBaseName();
      QVERIFY(captureWindow(window_).save(root_ + "/model-" + name + ".png"));
      const auto frame = session->frame();
      const auto oldMesh = session->meshIndex(), oldBinding = session->bindingIndex();
      view->orbit(30, 20); view->panBy(10, -5); view->zoomBy(1.2); view->fit(true);
      QCOMPARE(session->frame(), frame); // Camera never copies/reuploads geometry.
      if (!session->uvSets().isEmpty()) {
        session->setMode(1);
        QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),10000);
        QVERIFY(session->frame()->lines);
        QVERIFY(!session->frame()->vertices.empty());
        QTest::qWait(120);
        QVERIFY(captureWindow(window_).save(root_ + "/uv-" + name + ".png"));
      }
      session->setMode(2);
      QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),10000);
      if (session->bindingIndex() >= 0) {
        QVERIFY(session->frame()->heat);
        QVERIFY(!session->frame()->vertices.empty());
      }
      session->setMode(0);
      QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),10000);
      QCOMPARE(session->meshIndex(), oldMesh);
      QCOMPARE(session->bindingIndex(), oldBinding);
      const auto sourceTextures=session->frame()->textures;
      const auto sourceTexturePath=session->texturePath();
      session->associateTexture(small_ + "/唯一图片.png");
      QTRY_VERIFY_WITH_TIMEOUT(!session->textureBusy(),10000);
      QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),10000);
      QVERIFY2(session->textureError().isEmpty(),qPrintable(session->textureError()));
      QCOMPARE(session->texturePath(),small_ + "/唯一图片.png");
      QVERIFY(!session->frame()->textures.empty());
      QVERIFY(std::any_of(session->frame()->draws.begin(),session->frame()->draws.end(),[](const auto &d){return d.texture>=0;}));
      QTest::qWait(100);
      QVERIFY(captureWindow(window_).save(root_ + "/texture-" + name + ".png"));
      session->removeTexture();
      QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),10000);
      QCOMPARE(session->texturePath(),sourceTexturePath);
      QCOMPARE(session->frame()->textures,sourceTextures);
      reports.append(QJsonObject{{"path",path},{"info",QJsonObject::fromVariantMap(session->info())},
                                 {"meshes",QJsonArray::fromStringList(session->meshes())},
                                 {"uvSets",QJsonArray::fromStringList(session->uvSets())},
                                 {"vertices",static_cast<qint64>(frame->vertices.size())}});
    }
    // Latest request and cancellation must reject previous asynchronous results.
    const auto payload=[](auto write){QByteArray b;QDataStream s(&b,QIODevice::WriteOnly);s.setByteOrder(QDataStream::LittleEndian);s.setFloatingPointPrecision(QDataStream::SinglePrecision);write(s);return b;};
    QByteArray synthetic;
    const auto block=[&](const char *name,int stride,const QByteArray &data){QByteArray n(name);n.resize(20,'\0');synthetic+=n;synthetic+=payload([&](QDataStream &s){s<<qint32(1999801)<<qint32(stride)<<qint32(stride?data.size()/stride:0);});synthetic+=data;};
    block("ACTRHEAD",0,{});
    block("REFSKELT",120,payload([](QDataStream &s){for(int i=0;i<8;++i){QByteArray n="Bone"+QByteArray::number(i);n.resize(64,'\0');s.writeRawData(n.constData(),n.size());s<<qint32(0)<<qint32(i==0?7:0)<<qint32(0);s<<0.F<<0.F<<0.F<<1.F<<0.F<<0.F<<0.F<<1.F<<1.F<<1.F<<1.F;}}));
    block("PNTS0000",12,payload([](QDataStream &s){s<<-1.F<<0.F<<-1.F<<1.F<<0.F<<-1.F<<0.F<<0.F<<1.F;}));
    block("VTXW0000",16,payload([](QDataStream &s){for(quint32 i=0;i<3;++i)s<<i<<float(i)<<-1.F<<quint32(0);}));
    block("FACE0000",12,payload([](QDataStream &s){s<<quint16(0)<<quint16(1)<<quint16(2)<<quint8(0)<<quint8(0)<<quint32(1);}));
    block("MATT0000",88,payload([](QDataStream &s){QByteArray n("Fixture");n.resize(64,'\0');s.writeRawData(n.constData(),n.size());for(int i=0;i<6;++i)s<<qint32(0);}));
    block("RAWWEIGHTS",12,payload([](QDataStream &s){for(quint32 i=0;i<8;++i)s<<float(std::ldexp(1.0,-int(i)-1))<<quint32(0)<<i;s<<0.F<<quint32(1)<<quint32(0);}));
    const auto syntheticPath=root_+"/all-eight-raw.psk";
    QFile fixture(syntheticPath);QVERIFY(fixture.open(QIODevice::WriteOnly));QCOMPARE(fixture.write(synthetic),synthetic.size());fixture.close();
    session->open(syntheticPath);
    QTRY_VERIFY_WITH_TIMEOUT(session->available()||!session->error().isEmpty(),10000);
    QVERIFY2(session->available(),qPrintable(session->error()));
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),10000);
    session->pick(QMatrix4x4{},QPointF(-.6,-.6));
    QCOMPARE(session->picked().value("point").toUInt(),0U);
    QCOMPARE(session->influences().size(),8);
    QCOMPARE(session->picked().value("sum").toDouble(),.99609375);
    session->pick(QMatrix4x4{},QPointF(.6,-.6));
    QCOMPARE(session->picked().value("point").toUInt(),1U);
    QCOMPARE(session->influences().size(),1);
    QCOMPARE(session->influences()[0].toMap().value("weight").toDouble(),0.0);
    session->pick(QMatrix4x4{},QPointF(0,.6));
    QCOMPARE(session->picked().value("point").toUInt(),2U);
    QVERIFY(session->influences().isEmpty());
    for (int i=0;i<6;++i) { session->open(samples[0]); session->open(samples[1]); }
    QTRY_VERIFY_WITH_TIMEOUT(session->available(), 30000);
    QCOMPARE(session->path(),samples[1]);
    session->open(samples[0]); session->cancel();
    QTest::qWait(400);
    QVERIFY(!session->available()); QVERIFY(!session->busy()); QVERIFY(!session->frame());
    session->open(root_ + "/missing.fbx");
    QTRY_VERIFY_WITH_TIMEOUT(!session->busy(), 10000);
    QVERIFY(!session->available()); QVERIFY(!session->error().isEmpty());
    desktop_->clearAsset();
    evidence_["model_preview"] = reports;
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
#endif
  void modelAnimationProperties_data() {
    QTest::addColumn<QString>("sample");
    QTest::newRow("psa") << qEnvironmentVariable("FORMAVIEW_ANIMATION_SAMPLE");
    const auto fbx = qEnvironmentVariable("FORMAVIEW_ANIMATION_FBX_SAMPLE");
    if (!fbx.isEmpty())
      QTest::newRow("animation-only-fbx") << fbx;
  }
  void modelAnimationProperties() {
    QFETCH(QString, sample);
    if (sample.isEmpty())
      QSKIP("Set FORMAVIEW_ANIMATION_SAMPLE to a real PSA sample");
    QVERIFY(QFileInfo::exists(sample));
    // A QML binding may read any exposed property, including those on hidden
    // mesh panels. Inspect a separate session on every notification so a C++
    // exception is reported at its property rather than crossing the QML VM.
    ModelSession session;
    QSignalSpy animationNotifications(&session, &ModelSession::animChanged);
    QStringList failures;
    const auto readProperties = [&] {
      for (int i = QObject::staticMetaObject.propertyCount();
           i < session.metaObject()->propertyCount(); ++i) {
        const auto property = session.metaObject()->property(i);
        if (!property.isReadable())
          continue;
        try {
          (void)property.read(&session);
        } catch (const std::exception &error) {
          failures.append(QString::fromLatin1(property.name()) + ": " +
                          QString::fromUtf8(error.what()));
        }
      }
    };
    connect(&session, &ModelSession::changed, &session, readProperties);
    connect(&session, &ModelSession::animChanged, &session, readProperties);
    readProperties();
    session.open(sample);
    QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 20000);
    QVERIFY2(session.inspectable(), qPrintable(session.error() + session.frameError()));
    QVERIFY(session.animated());
    QVERIFY(session.meshes().isEmpty());
    QVERIFY(animationNotifications.count() >= 2); // Clear + landing notify.
    QVERIFY2(failures.isEmpty(), qPrintable(failures.join('\n')));
    QVERIFY(session.uvBackground().isEmpty());
    QVERIFY(!session.canClearTexture());
    // Mesh-only commands can still be invoked by a stale selection callback.
    session.selectBinding(-1);
    session.removeTexture();
    session.classifyMesh("not-a-mesh", 0, false);
    session.restoreClassification();
    session.seek(session.clipInfo().value("end").toDouble());
    const auto beforeClear = animationNotifications.count();
    session.clear();
    readProperties();
    QVERIFY2(failures.isEmpty(), qPrintable(failures.join('\n')));
    QVERIFY(!session.available());
    QVERIFY(animationNotifications.count() > beforeClear);
    QVERIFY(session.clipInfo().isEmpty());
  }
  void modelAnimationPlayback() {
    try {
    // M07 integration: real animation sample through the product session -
    // clips, inline playback, seek/step/clip switching and the timeline bar.
    // Skipped unless FORMAVIEW_ANIMATION_SAMPLE points at a real PSA/FBX.
    const auto sample =
        QFileInfo(requireFixture("FORMAVIEW_ANIMATION_SAMPLE"));
    if (!sample.exists()) {
      QSKIP("Set FORMAVIEW_ANIMATION_SAMPLE to a real animation sample");
      return;
    }
    auto *session = qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    const auto restore = qScopeGuard([&] {
      // Also forget the session reference memory so later tests in this
      // process start from a clean pairing state.
      if (!session->busy())
        session->setAnimationReference(QString());
      desktop_->clearAsset();
    });
    QSignalSpy qmlWarnings(engine_.get(), &QQmlEngine::warnings);
    desktop_->openPath(sample.absoluteFilePath());
    // Desktop routes the request asynchronously. !busy is also true before
    // the model request starts, so it is not an arrival condition.
    QTRY_COMPARE_WITH_TIMEOUT(session->path(), sample.absoluteFilePath(), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable() ||
                                !session->error().isEmpty() ||
                                !session->frameError().isEmpty(),
                            20000);
    QVERIFY2(session->inspectable(), qPrintable(session->error() + session->frameError()));
    QVERIFY(session->animated());
    const auto reference = requireFixture("FORMAVIEW_ANIMATION_REFERENCE", true);
    if (!reference.isEmpty() && sample.suffix().compare("psa", Qt::CaseInsensitive) == 0) {
      session->setAnimationReference(reference);
      QTRY_VERIFY_WITH_TIMEOUT(!session->busy(), 20000);
      QVERIFY2(session->inspectable(), qPrintable(session->error()));
      const auto companion = requireFixture("FORMAVIEW_ANIMATION_COMPANION", true);
      if (!companion.isEmpty()) {
        session->setAnimationCompanion(companion);
        QTRY_VERIFY_WITH_TIMEOUT(!session->busy(), 20000);
        QVERIFY2(session->inspectable(), qPrintable(session->error()));
      }
    }
    QVERIFY(session->clips().size() >= 1);
    const auto info = session->clipInfo();
    QVERIFY(info.contains("start") && info.contains("lastFrame"));
    QVERIFY(session->frame() && !session->frame()->vertices.empty());
    QTRY_VERIFY(item("modelAnimationBarHost") &&
                item("modelAnimationBarHost")->isVisible());
    if (session->info().value("psa").toBool()) {
      auto *menuButton = item("animPairingMenuButton");
      QVERIFY(menuButton && menuButton->isVisible());
      QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
          menuButton->mapToScene({menuButton->width() / 2, menuButton->height() / 2}).toPoint());
      QTRY_VERIFY(item("animReference") && item("animReference")->isVisible());
      QVERIFY(item("animReference")->isEnabled());
      QCOMPARE(item("animCompanion")->isEnabled(), !session->info().value("referencePath").toString().isEmpty());
      QTest::keyClick(window_, Qt::Key_Escape);
      QTRY_VERIFY(!item("animReference") || !item("animReference")->isVisible());
    }
    QTRY_COMPARE(item("animScrubber")->property("from").toDouble(),
                 info.value("start").toDouble());
    QTRY_COMPARE(item("animScrubber")->property("to").toDouble(),
                 info.value("end").toDouble());
    QVERIFY(!item("animTime")->property("text").toString().contains("undefined"));
    QCOMPARE(qmlWarnings.count(), 0);
    // Inline playback advances real seconds and keeps publishing frames.
    const auto revisionBefore =
        session->frame() ? session->frame()->revision : std::uint64_t{0};
    session->togglePlay();
    QVERIFY(session->playing());
    for (int i = 0; i < 40 && session->playing(); ++i)
      QTest::qWait(25);
    QVERIFY(session->time() > info.value("start").toDouble());
    QVERIFY(session->frame() &&
            session->frame()->revision > revisionBefore);
    session->togglePlay();
    QVERIFY(!session->playing());
    const double paused = session->time();
    QTest::qWait(60);
    auto *guideView = qobject_cast<ModelView *>(item("modelViewport"));
    QVERIFY(guideView);
    QCOMPARE(session->frame()->gridVertices, std::uint32_t(84));
    QCOMPARE(session->frame()->guides.size(), std::size_t(102));
    const auto guideCamera = guideView->cameraState();
    const auto clickGuide = [&](const char *name) {
      auto *control = item(name);
      QVERIFY(control && control->isVisible());
      QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
          control->mapToScene({control->width() / 2, control->height() / 2}).toPoint());
      QTest::qWait(80);
    };
    const auto viewportPixels = [&] {
      const auto snapshot = captureWindow(window_);
      const auto scale = snapshot.width() / double(window_->width());
      const auto top = guideView->mapToScene(QPointF());
      return snapshot.copy(QRect(int(top.x() * scale), int(top.y() * scale),
                                 int(guideView->width() * scale), int(guideView->height() * scale)));
    };
    QVERIFY(guideView->gridVisible() && guideView->axesVisible());
    const auto guidesOn = viewportPixels();
    clickGuide("modelGroundGrid");
    QVERIFY(!guideView->gridVisible() && guideView->axesVisible());
    const auto axesOnly = viewportPixels();
    QVERIFY2(guidesOn != axesOnly, "grid toggle must change actual animation viewport pixels");
    clickGuide("modelOriginAxes");
    QVERIFY(!guideView->gridVisible() && !guideView->axesVisible());
    QVERIFY2(axesOnly != viewportPixels(), "axes toggle must change actual animation viewport pixels");
    QCOMPARE(guideView->cameraState(), guideCamera);
    clickGuide("modelGroundGrid");
    clickGuide("modelOriginAxes");
    const auto image = captureWindow(window_);
    auto *animatedView = item("modelViewport");
    QVERIFY(animatedView);
    const auto top = animatedView->mapToScene(QPointF(0, 0));
    auto *animationBar = item("modelAnimationBarHost");
    QVERIFY(animationBar);
    QVERIFY(animationBar->property("radius").toDouble() > 0);
    const auto barTop = animationBar->mapToScene(QPointF(0, 0)).y();
    const double scaleX = double(image.width()) / window_->width();
    const double scaleY = double(image.height()) / window_->height();
    const auto region = QRectF(top, QSizeF(animatedView->width(),
        std::max(0.0, std::min(animatedView->height(), barTop - top.y()))));
    QRect skeletonPixels;
    int visibleSkeletonPixels = 0;
    for (int y = std::max(0, int(region.top() * scaleY));
         y < std::min(image.height(), int(region.bottom() * scaleY)); ++y)
      for (int x = std::max(0, int(region.left() * scaleX));
           x < std::min(image.width(), int(region.right() * scaleX)); ++x) {
        const auto colour = image.pixelColor(x, y);
        if (colour.red() > 130 && colour.green() > 70 &&
            colour.red() > colour.green() * 1.05 &&
            colour.blue() < colour.green() * .9) {
          ++visibleSkeletonPixels;
          skeletonPixels = skeletonPixels.united(QRect(x, y, 1, 1));
        }
      }
    QVERIFY2(visibleSkeletonPixels > 10 && skeletonPixels.width() > 20 &&
                 skeletonPixels.height() > 20,
             "Animation must produce a visible skeleton, not an empty viewport or one clipped line");
    evidence_["animation_skeleton_pixels"] = visibleSkeletonPixels;
    const auto oldLanguage = desktop_->language();
    const auto oldSize = window_->size();
    for (const auto &language : {QStringLiteral("zh-CN"), QStringLiteral("en"),
                                 QStringLiteral("zh-TW")}) {
      desktop_->setLanguage(language);
      for (const auto &size : {QSize(1600, 900), QSize(1280, 800)}) {
        window_->resize(size);
        QTest::qWait(100);
        auto *pane = item("modelPane");
        auto *grid = item("modelGroundGrid");
        QVERIFY(pane && grid);
        const auto toolbarY = grid->mapToItem(pane, QPointF()).y();
        for (const auto *name : {"modelGroundGrid", "modelOriginAxes", "modelFocus", "modelFrameAll"}) {
          auto *button = item(name);
          QVERIFY2(button && button->isVisible(), name);
          const auto point = button->mapToItem(pane, QPointF());
          QVERIFY2(qAbs(point.y() - toolbarY) < 1, name);
          QVERIFY2(point.x() >= 0 && point.x() + button->width() <= pane->width() + 1, name);
        }
        auto *bar = item("modelAnimationBarHost");
        QVERIFY(bar);
        auto *timeline = item("modelAnimationBar");
        QVERIFY(timeline);
        timeline->setProperty("detailsVisible", false);
        QTest::qWait(60);
        const auto compactHeight = timeline->height();
        QVERIFY(!item("animStart")->isVisible());
        for (const auto *name : {"animPlay", "animSpeed", "animClip", "animRootOptions", "animRootLock"})
          QVERIFY2(item(name) && item(name)->isVisible(), name);
        auto *lock = item("animRootLock"), *arrow = item("animRootOptions");
        QCOMPARE(lock->parentItem(),arrow->parentItem());
        QVERIFY(qAbs(lock->y()-arrow->y()) < 1);
        QVERIFY(qAbs(arrow->x()-(lock->x()+lock->width())) < 1);
        const auto rootControlsPosition=lock->parentItem()->mapToItem(bar,QPointF());
        QVERIFY(rootControlsPosition.x() >= -1 && rootControlsPosition.x()+lock->parentItem()->width() <= bar->width()+1);
        QTest::mouseClick(window_,Qt::LeftButton,Qt::NoModifier,arrow->mapToScene(QPointF(arrow->width()/2,arrow->height()/2)).toPoint());
        QTRY_VERIFY(item("animRootBaseFirst")->isVisible());
        QCOMPARE(item("animRootBaseReference")->isEnabled(),session->rootLockInfo().value("referenceAvailable").toBool());
        QCOMPARE(item("animRootBaseFirst")->property("checked").toBool(),session->rootBase() == 0);
        QTest::keyClick(window_,Qt::Key_Escape);
        timeline->setProperty("detailsVisible", true);
        QTest::qWait(60);
        QVERIFY(timeline->height() > compactHeight);
        for (const auto *name : {"animControls", "animTime", "animScrubber", "animStart", "animEnd"}) {
          auto *control = item(name);
          QVERIFY2(control, name);
          const auto point = control->mapToItem(bar, QPointF(0, 0));
          QVERIFY2(point.x() >= -1 && point.y() >= -1 &&
                       point.x() + control->width() <= bar->width() + 1 &&
                       point.y() + control->height() <= bar->height() + 1,
                   name);
        }
        QVERIFY(item("animEnd")->property("text").toString().contains(
            QString::number(info.value("end").toDouble(), 'f', 3)));
        save("m07-animation-" + language + "-" + QString::number(size.width()));
        timeline->setProperty("detailsVisible", false);
      }
    }
    window_->resize(oldSize);
    desktop_->setLanguage(oldLanguage);
    QTest::qWait(80);
    QCOMPARE(session->time(), paused); // Pause freezes the presented time.
    // Seek clamps to the clip domain; step and clip switching keep state.
    session->seek(1e9);
    QCOMPARE(session->time(), info.value("end").toDouble());
    session->seek(-1.0);
    QCOMPARE(session->time(), info.value("start").toDouble());
    session->seek(paused);
    session->stepFrame(1);
    QVERIFY(session->time() > info.value("start").toDouble());
    if (session->clips().size() > 1) {
      const auto before = session->clipIndex();
      session->setClipIndex(before == 0 ? 1 : 0);
      QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(), 10000);
      QVERIFY(session->clipIndex() != before);
      QCOMPARE(session->time(), session->clipInfo().value("start").toDouble());
    }
    session->setSpeed(2.0);
    QCOMPARE(session->speed(), 2.0);
    session->setLoop(false);
    QVERIFY(!session->loop());
    // The viewport space key toggles playback for animated documents.
    auto *viewport = item("modelViewport");
    QVERIFY(viewport);
    viewport->forceActiveFocus();
    QTest::keyClick(window_, Qt::Key_Space);
    QVERIFY2(session->playing(), QJsonDocument(QJsonObject{
        {"time", session->time()}, {"clip", QJsonObject::fromVariantMap(session->clipInfo())},
        {"viewport_focus", viewport->hasActiveFocus()}, {"speed", session->speed()},
        {"loop", session->loop()}}).toJson(QJsonDocument::Compact).constData());
    QTest::keyClick(window_, Qt::Key_Space);
    QVERIFY(!session->playing());
    // Frame never mutates source data: reopening yields the same digest.
    const auto firstDigest = session->info().value("sha256").toString();
    desktop_->clearAsset();
    desktop_->openPath(sample.absoluteFilePath());
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(), 20000);
    QCOMPARE(session->info().value("sha256").toString(), firstDigest);
    desktop_->clearAsset();
    QCoreApplication::processEvents();
    QCOMPARE(qmlWarnings.count(), 0);
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelPsaSessionReferenceMemory() {
    try {
    // M07 session reference memory + per-PSA companion auto-match:
    // one explicit PSK choice is retried for every later PSA in this
    // process; each file re-verifies compatibility and rejects with a
    // warning instead of silently applying a wrong skeleton. Companions
    // are discovered per file (same dir, same base name) and never
    // inherited from the previous file.
    const auto sampleA =
        QFileInfo(requireFixture("FORMAVIEW_ANIMATION_SAMPLE"));
    const auto reference =
        QFileInfo(requireFixture("FORMAVIEW_ANIMATION_REFERENCE"));
    if (!sampleA.exists() || !reference.exists() ||
        sampleA.suffix().compare("psa", Qt::CaseInsensitive) != 0) {
      QSKIP("Set FORMAVIEW_ANIMATION_SAMPLE (PSA) and "
            "FORMAVIEW_ANIMATION_REFERENCE (PSK)");
      return;
    }
    const auto sampleB =
        QFileInfo(requireFixture("FORMAVIEW_ANIMATION_PSA_B"));
    const auto incompatible =
        QFileInfo(requireFixture("FORMAVIEW_ANIMATION_PSA_INCOMPATIBLE"));
    const auto companionA =
        QFileInfo(sampleA.dir().filePath(sampleA.completeBaseName() + ".config"));
    auto *session = qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    const auto restore = qScopeGuard([&] {
      session->setAnimationReference(QString());
      desktop_->clearAsset();
    });
    // Reopening the same file keeps path()/inspectable() true with the
    // previous document, and the busy window can live entirely inside one
    // event-processing burst: count documentChanged landings instead.
    QSignalSpy documentLoaded(session, &ModelSession::documentChanged);
    const auto openAndWait = [&](const QFileInfo &file) {
      const auto before = documentLoaded.count();
      desktop_->openPath(file.absoluteFilePath());
      QTRY_VERIFY_WITH_TIMEOUT(
          documentLoaded.count() > before && !session->busy() &&
              (session->inspectable() || !session->error().isEmpty()),
          20000);
      QVERIFY2(session->inspectable(),
               qPrintable(session->error() + session->frameError()));
      QVERIFY(session->animated());
    };
    const auto paired = [&] {
      return session->info().value("referencePath").toString();
    };
    const auto companion = [&] {
      return session->info().value("companionPath").toString();
    };
    const auto rejected = [&] {
      return session->info().value("referenceRejected").toBool();
    };

    // 1. First open without a session reference: unpaired raw preview. A
    // discovered companion cannot apply without a reference and says so.
    openAndWait(sampleA);
    QVERIFY(paired().isEmpty());
    QVERIFY(!rejected());
    if (companionA.exists())
      QVERIFY(session->warnings().join('|').contains("NOT applied", Qt::CaseInsensitive));

    // 2. One explicit choice pairs A and applies A's own companion
    // automatically (no manual companion selection).
    session->setAnimationReference(reference.absoluteFilePath());
    QTRY_VERIFY_WITH_TIMEOUT(!session->busy(), 20000);
    QVERIFY2(session->inspectable(), qPrintable(session->error()));
    QCOMPARE(paired(), reference.absoluteFilePath());
    QCOMPARE(companion(),
             companionA.exists() ? companionA.absoluteFilePath() : QString{});

    // 3. Same-folder PSA B reuses the session reference and applies B's own
    // companion - never A's.
    if (sampleB.exists() && sampleB.isDir() == false &&
        sampleB.suffix().compare("psa", Qt::CaseInsensitive) == 0) {
      openAndWait(sampleB);
      QCOMPARE(paired(), reference.absoluteFilePath());
      const auto companionB =
          QFileInfo(sampleB.dir().filePath(sampleB.completeBaseName() + ".config"));
      QCOMPARE(companion(),
               companionB.exists() ? companionB.absoluteFilePath() : QString{});
      if (companionA.exists() && companionB.exists())
        QVERIFY(companion() != companionA.absoluteFilePath() ||
                sampleA.absoluteFilePath() == sampleB.absoluteFilePath());

      // 4. Back to A: still paired with A's companion.
      openAndWait(sampleA);
      QCOMPARE(paired(), reference.absoluteFilePath());
      QCOMPARE(companion(),
               companionA.exists() ? companionA.absoluteFilePath() : QString{});
    }

    // 5. An incompatible PSA warns, stays unpaired and previewable, and
    // does not poison the session for compatible files.
    if (incompatible.exists() &&
        incompatible.suffix().compare("psa", Qt::CaseInsensitive) == 0) {
      openAndWait(incompatible);
      QVERIFY(paired().isEmpty());
      QVERIFY(rejected());
      QVERIFY(!session->info().value("referenceRejectReason").toString().isEmpty());
      QVERIFY(session->warnings().join('|').contains("NOT applied", Qt::CaseInsensitive));
      openAndWait(sampleA);
      QCOMPARE(paired(), reference.absoluteFilePath());
      QVERIFY(!rejected());
    }

    // 5b. An invalid companion (not a track config) only disables the
    // config with a warning; the verified pairing itself stays applied.
    {
      // A binary non-config file (the PSA itself) must fail companion
      // parsing; a key=value .props.txt without [RemoveTracks] is a valid
      // empty config, so it does not count as invalid here.
      if (true) {
        const auto referenceBefore = paired();
        session->setAnimationCompanion(sampleA.absoluteFilePath());
        QTRY_VERIFY_WITH_TIMEOUT(!session->busy(), 20000);
        QVERIFY2(session->inspectable(), qPrintable(session->error()));
        QCOMPARE(paired(), referenceBefore);
        QVERIFY(companion().isEmpty());
        QVERIFY(session->warnings().join('|')
                    .contains("Track config NOT applied", Qt::CaseInsensitive));
        // Back to the per-file auto companion on the next open.
        openAndWait(sampleA);
        QCOMPARE(companion(),
                 companionA.exists() ? companionA.absoluteFilePath()
                                     : QString{});
      }
    }

    // 5c. An externally deleted reference is detected on the next open:
    // every open re-reads and re-verifies the inputs, so a stale pairing is
    // never reused. Uses a private copy; the read-only source stays intact.
    {
      const auto temporary = QFileInfo(
          root_ + "/session-reference-copy.psk");
      QFile::copy(reference.absoluteFilePath(), temporary.absoluteFilePath());
      session->setAnimationReference(temporary.absoluteFilePath());
      QTRY_VERIFY_WITH_TIMEOUT(!session->busy(), 20000);
      QVERIFY2(session->inspectable(), qPrintable(session->error()));
      QCOMPARE(paired(), temporary.absoluteFilePath());
      QFile::remove(temporary.absoluteFilePath());
      openAndWait(sampleA);
      QVERIFY(paired().isEmpty());
      QVERIFY(rejected());
      // An explicit new choice restores the session for the remaining steps.
      session->setAnimationReference(reference.absoluteFilePath());
      QTRY_VERIFY_WITH_TIMEOUT(!session->busy(), 20000);
      QVERIFY2(session->inspectable(), qPrintable(session->error()));
      QCOMPARE(paired(), reference.absoluteFilePath());
    }

    // 6. A non-PSA asset in between does not disturb the session memory.
    const auto fbx = QFileInfo(
        requireFixture("FORMAVIEW_ANIMATION_FBX"));
    if (fbx.exists()) {
      desktop_->openPath(fbx.absoluteFilePath());
      QTRY_VERIFY_WITH_TIMEOUT(session->path().isEmpty() ||
                               !session->busy(), 20000);
      openAndWait(sampleA);
      QCOMPARE(paired(), reference.absoluteFilePath());
    }

    // 7. Explicit clear also forgets the session memory: the next open is
    // unpaired and does not silently re-apply the reference.
    session->setAnimationReference(QString());
    QTRY_VERIFY_WITH_TIMEOUT(!session->busy(), 20000);
    QVERIFY2(session->inspectable(), qPrintable(session->error()));
    QVERIFY(paired().isEmpty());
    QVERIFY(!rejected());
    openAndWait(sampleA);
    QVERIFY(paired().isEmpty());
    QVERIFY(!rejected());
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelRootLockPlayback() {
    try {
    // M07 root lock product flow: toggle locks the root (translation/rotation
    // from the base, scale kept), off restores the source pose at the same
    // time, bases switch, reference base only with verified pairing, and the
    // session memory/pairing integration still holds.
    const auto sample =
        QFileInfo(requireFixture("FORMAVIEW_ANIMATION_SAMPLE"));
    if (!sample.exists() ||
        sample.suffix().compare("psa", Qt::CaseInsensitive) != 0) {
      QSKIP("Set FORMAVIEW_ANIMATION_SAMPLE to a real PSA");
      return;
    }
    auto *session = qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    const auto restore = qScopeGuard([&] {
      session->setRootLock(false);
      session->setAnimationReference(QString());
      desktop_->clearAsset();
    });
    const auto openAndWait = [&](const QFileInfo &file) {
      QSignalSpy documentLoaded(session, &ModelSession::documentChanged);
      const auto before = documentLoaded.count();
      desktop_->openPath(file.absoluteFilePath());
      QTRY_VERIFY_WITH_TIMEOUT(
          documentLoaded.count() > before && !session->busy() &&
              (session->inspectable() || !session->error().isEmpty()),
          20000);
      QVERIFY2(session->inspectable(),
               qPrintable(session->error() + session->frameError()));
    };
    openAndWait(sample);
    const auto candidates = session->rootCandidates();
    QVERIFY(!candidates.isEmpty());

    // Unpaired: reference base unavailable, auto-falls back / refuses.
    QVERIFY(!session->rootLockInfo().value("referenceAvailable").toBool());
    session->setRootBase(1);
    QCOMPARE(session->rootBase(), 0);

    // Enable first-frame lock: the frame must differ from the unlocked one
    // for a root-motion clip, and the status reports locked.
    if (candidates.size() > 1) {
      QVERIFY(!session->rootLockInfo().value("on").toBool());
      session->setRootIndex(
          candidates.front().toMap().value("index").toInt());
    }
    session->seek(session->clipInfo().value("end").toDouble() * 0.5);
    const auto unlocked = session->frame();
    QVERIFY(unlocked);
    session->setRootLock(true);
    QVERIFY(session->rootLockInfo().value("on").toBool());
    QCOMPARE(session->rootLockInfo().value("status").toString(),
             QStringLiteral("locked"));
    const auto locked = session->frame();
    QVERIFY(locked && locked->revision > unlocked->revision);

    // Zero base: root local translation becomes ~zero relative to the pose
    // origin, so the locked pose differs from the first-frame lock.
    session->setRootBase(2);
    QCOMPARE(session->rootBase(), 2);
    const auto zero = session->frame();
    QVERIFY(zero && zero->revision > locked->revision);

    // Off restores the source at the same time: equals the pre-lock frame.
    session->setRootLock(false);
    const auto restored = session->frame();
    QVERIFY(restored);
    QCOMPARE(restored->vertices.size(), unlocked->vertices.size());

    // Paired: reference base becomes available.
    const auto reference =
        QFileInfo(requireFixture("FORMAVIEW_ANIMATION_REFERENCE", true));
    if (reference.exists()) {
      session->setAnimationReference(reference.absoluteFilePath());
      QTRY_VERIFY_WITH_TIMEOUT(!session->busy(), 20000);
      QVERIFY2(session->inspectable(), qPrintable(session->error()));
      QVERIFY(session->rootLockInfo().value("referenceAvailable").toBool());
      session->setRootBase(1);
      QCOMPARE(session->rootBase(), 1);
      session->setRootLock(true);
      QCOMPARE(session->rootLockInfo().value("base").toString(),
               QStringLiteral("reference"));
      session->setRootLock(false);
    }
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelRootLockDegenerateHosts() {
    // P2 review edge case: root lock must not guess a TRS decomposition.
    // lockRootLocal faithfully locks only clean orthogonal positive-scale
    // transforms (base rotation/translation, source per-axis scale). A
    // mirrored, sheared or degenerate source or base must return the source
    // unchanged, so the animation keeps its true motion instead of a
    // corrupted mirror/shear lock.
    const auto toMatrix = [](const QMatrix4x4 &q) {
      formaview::Matrix4 m{};
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
          m[static_cast<std::size_t>(r * 4 + c)] = q(r, c);
      return m;
    };
    const auto maxDiff = [](const formaview::Matrix4 &a,
                            const formaview::Matrix4 &b) {
      double worst = 0;
      for (std::size_t i = 0; i < 16; ++i)
        if (std::abs(a[i] - b[i]) > worst)
          worst = std::abs(a[i] - b[i]);
      return worst;
    };
    const auto columnLength = [](const formaview::Matrix4 &m, int column) {
      return std::hypot(m[static_cast<std::size_t>(column)],
                        m[static_cast<std::size_t>(4 + column)],
                        m[static_cast<std::size_t>(8 + column)]);
    };
    QMatrix4x4 baseQ;
    baseQ.translate(3, 4, 5);
    baseQ.rotate(30.f, 0.f, 0.f, 1.f);
    baseQ.scale(2.f, 2.f, 2.f);
    const auto base = toMatrix(baseQ);

    // Clean positive-scale source: the lock applies - base translation with
    // the source's per-axis scale magnitudes.
    QMatrix4x4 cleanQ;
    cleanQ.translate(-1, 2, 0.5f);
    cleanQ.rotate(77.f, 0.f, 1.f, 0.f);
    cleanQ.scale(3.f, 1.f, 1.5f);
    const auto clean = toMatrix(cleanQ);
    const auto lockedClean = lockRootLocal(clean, base);
    QVERIFY(maxDiff(lockedClean, clean) > 1e-3);
    for (int column = 0; column < 3; ++column)
      QVERIFY(std::abs(columnLength(lockedClean, column) -
                       columnLength(clean, column)) <= 1e-9);
    QVERIFY(std::abs(lockedClean[3] - base[3]) <= 1e-9);
    QVERIFY(std::abs(lockedClean[7] - base[7]) <= 1e-9);
    QVERIFY(std::abs(lockedClean[11] - base[11]) <= 1e-9);

    // Mirrored source (negative X scale): refuse - keep the source.
    QMatrix4x4 mirrorQ;
    mirrorQ.rotate(15.f, 0.f, 0.f, 1.f);
    mirrorQ.scale(-2.f, 2.f, 2.f);
    const auto mirror = toMatrix(mirrorQ);
    QVERIFY(maxDiff(lockRootLocal(mirror, base), mirror) <= 1e-12);

    // Sheared source (non-orthogonal columns): refuse - keep the source.
    formaview::Matrix4 shear{};
    shear[0] = 1;
    shear[4] = 0.4; // column 0 gains a Y component: not orthogonal.
    shear[5] = 1;
    shear[10] = 1;
    shear[15] = 1;
    shear[3] = 1;
    shear[7] = -2;
    shear[11] = 0.5;
    QVERIFY(maxDiff(lockRootLocal(shear, base), shear) <= 1e-12);

    // Zero-scale source column: refuse - keep the source.
    formaview::Matrix4 zeroSource{};
    zeroSource[0] = 2;
    zeroSource[5] = 0;
    zeroSource[10] = 1;
    zeroSource[15] = 1;
    zeroSource[3] = 4;
    QVERIFY(maxDiff(lockRootLocal(zeroSource, base), zeroSource) <= 1e-12);

    // Degenerate base (zero column): refuse - keep the source.
    formaview::Matrix4 degenerateBase{};
    degenerateBase[0] = 0;
    degenerateBase[5] = 1;
    degenerateBase[10] = 1;
    degenerateBase[15] = 1;
    QVERIFY(maxDiff(lockRootLocal(clean, degenerateBase), clean) <= 1e-12);
  }
  void modelGltfAnimationPlayback() {
    try {
    // M07 native glTF animation: a glTF/GLB whose own clips drive its own
    // skinned meshes previews with the standard animation bar — playback,
    // seek, root lock and the UV inspection page — without any external
    // attach, mapping or mutation of the file's bind data.
    const auto sample = QFileInfo(
        requireFixture("FORMAVIEW_GLTF_ANIMATION_SAMPLE"));
    if (!sample.exists() ||
        sample.suffix().compare("gltf", Qt::CaseInsensitive) != 0 &&
            sample.suffix().compare("glb", Qt::CaseInsensitive) != 0) {
      QSKIP("Set FORMAVIEW_GLTF_ANIMATION_SAMPLE to an animated glTF/GLB");
      return;
    }
    auto *session = qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    const auto restore = qScopeGuard([&] {
      session->setRootLock(false);
      session->setMode(0);
      desktop_->clearAsset();
    });
    QSignalSpy documentLoaded(session, &ModelSession::documentChanged);
    const auto before = documentLoaded.count();
    desktop_->openPath(sample.absoluteFilePath());
    QTRY_VERIFY_WITH_TIMEOUT(
        documentLoaded.count() > before && !session->busy() &&
            (session->inspectable() || !session->error().isEmpty()),
        60000);
    QVERIFY2(session->inspectable(),
             qPrintable(session->error() + session->frameError()));
    QVERIFY(session->animated());
    // Native clips are self-contained: no external attach is offered.
    QVERIFY(!session->canAttachExternalAnimation());
    QVERIFY(!session->externalInfo().value("driven").toBool());
    QVERIFY(session->clips().size() >= 1);
    const auto clip = session->clipInfo();
    QVERIFY(clip.value("end").toDouble() > clip.value("start").toDouble());
    QVERIFY(clip.value("fps").toDouble() > 0);
    QTRY_VERIFY_WITH_TIMEOUT(
        session->frame() && !session->frame()->vertices.empty() &&
            session->frameError().isEmpty(),
        30000);

    // Animated geometry, not just state: the mid-clip pose must differ from
    // the first-frame pose on real motion clips.
    const auto maxVertexDelta =
        [](const std::shared_ptr<const formaview::render::Frame> &a,
           const std::shared_ptr<const formaview::render::Frame> &b) {
          if (!a || !b || a->vertices.size() != b->vertices.size() ||
              a->vertices.empty())
            return -1.0;
          double delta = 0;
          for (std::size_t i = 0; i < a->vertices.size(); ++i) {
            const auto &va = a->vertices[i];
            const auto &vb = b->vertices[i];
            delta = std::max({delta, std::abs(double(va.x) - double(vb.x)),
                              std::abs(double(va.y) - double(vb.y)),
                              std::abs(double(va.z) - double(vb.z))});
          }
          return delta;
        };
    const auto firstFrame = session->frame();
    session->seek(clip.value("start").toDouble() +
                  (clip.value("end").toDouble() - clip.value("start").toDouble()) *
                      0.5);
    QTRY_VERIFY_WITH_TIMEOUT(session->frame()->revision > firstFrame->revision,
                             30000);
    QVERIFY2(session->frameError().isEmpty(),
             qPrintable(session->frameError()));
    const auto midFrame = session->frame();
    QVERIFY2(maxVertexDelta(firstFrame, midFrame) > 0.005,
             qPrintable(QString("Mid-clip pose matches the first frame: %1")
                            .arg(maxVertexDelta(firstFrame, midFrame))));

    // Root lock on the native node domain: status, rebuilt frame and exact
    // restore at the same time. Root-motion-free models lock without moving
    // vertices; the restore comparison stays exact either way.
    const auto candidates = session->rootCandidates();
    QVERIFY(!candidates.isEmpty());
    if (candidates.size() > 1)
      session->setRootIndex(candidates.front().toMap().value("index").toInt());
    session->setRootLock(true);
    QCOMPARE(session->rootLockInfo().value("status").toString(),
             QStringLiteral("locked"));
    QTRY_VERIFY_WITH_TIMEOUT(session->frame()->revision > midFrame->revision,
                             30000);
    const auto lockedFrame = session->frame();
    const auto lockDelta = maxVertexDelta(midFrame, lockedFrame);
    session->setRootLock(false);
    QTRY_VERIFY_WITH_TIMEOUT(session->frame()->revision > lockedFrame->revision,
                             30000);
    QCOMPARE(maxVertexDelta(midFrame, session->frame()), 0.0);
    qInfo() << "gltf root lock vertex delta" << lockDelta
            << "root" << session->rootLockInfo().value("rootName").toString();

    // Playback publishes advancing frames on the async skinning path.
    session->setSpeed(1.0);
    session->setLoop(true);
    const auto revisionBefore = session->frame()->revision;
    session->togglePlay();
    QVERIFY(session->playing());
    for (int i = 0; i < 30 && session->playing(); ++i)
      QTest::qWait(25);
    if (session->playing())
      session->togglePlay();
    QTRY_VERIFY_WITH_TIMEOUT(session->frame()->revision > revisionBefore,
                             30000);
    QVERIFY2(session->frameError().isEmpty(),
             qPrintable(session->frameError()));

    // The UV inspection page keeps source UVs while the 3D pose animates.
    session->setMode(1);
    QTRY_VERIFY_WITH_TIMEOUT(session->frame() &&
                                 !session->frame()->vertices.empty(),
                             30000);
    QVERIFY2(session->frameError().isEmpty(),
             qPrintable(session->frameError()));
    session->setMode(0);
    QTRY_VERIFY_WITH_TIMEOUT(session->frame() &&
                                 !session->frame()->vertices.empty(),
                             30000);
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelGltfAnimationVariants() {
    try {
    // M07 glTF variant matrix (handmade minimal samples): animation-only
    // skeleton preview, skins without inverse bind matrices, cubic-spline
    // and step interpolation semantics, and morph-weight channels that must
    // stay explicitly unsupported while sibling TRS channels still drive.
    const QDir variants(
        requireFixture("FORMAVIEW_GLTF_VARIANTS_DIR"));
    if (!variants.exists()) {
      QSKIP("Set FORMAVIEW_GLTF_VARIANTS_DIR to the handmade glTF samples");
      return;
    }
    auto *session = qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    const auto restore = qScopeGuard([&] { desktop_->clearAsset(); });
    const auto openAndWait = [&](const QString &name) {
      QSignalSpy documentLoaded(session, &ModelSession::documentChanged);
      const auto before = documentLoaded.count();
      desktop_->openPath(variants.filePath(name));
      QTRY_VERIFY_WITH_TIMEOUT(
          documentLoaded.count() > before && !session->busy() &&
              (session->inspectable() || !session->error().isEmpty()),
          20000);
      QVERIFY2(session->inspectable(),
               qPrintable(name + ": " + session->error()));
    };
    const auto maxVertexDelta =
        [](const std::shared_ptr<const formaview::render::Frame> &a,
           const std::shared_ptr<const formaview::render::Frame> &b) {
          if (!a || !b || a->vertices.size() != b->vertices.size() ||
              a->vertices.empty())
            return -1.0;
          double delta = 0;
          for (std::size_t i = 0; i < a->vertices.size(); ++i) {
            const auto &va = a->vertices[i];
            const auto &vb = b->vertices[i];
            delta = std::max({delta,
                              std::abs(double(va.x) - double(vb.x)),
                              std::abs(double(va.y) - double(vb.y)),
                              std::abs(double(va.z) - double(vb.z))});
          }
          return delta;
        };

    // Animation-only: no meshes, skeleton-line preview with native clips.
    openAndWait("anim-only.gltf");
    QVERIFY(session->animated());
    QVERIFY(session->clips().size() == 1);
    QTRY_VERIFY_WITH_TIMEOUT(
        session->frame() && !session->frame()->vertices.empty(),
        20000);
    const auto animOnlyStart = session->frame();
    session->seek(1.0);
    QTRY_VERIFY_WITH_TIMEOUT(
        session->frame()->revision > animOnlyStart->revision, 20000);
    QVERIFY2(maxVertexDelta(animOnlyStart, session->frame()) > 0.001,
             "animation-only skeleton lines must follow the clip");

    // No inverse bind matrices: identity bind, skinning still applies.
    openAndWait("no-ibm.gltf");
    QVERIFY(session->animated());
    QTRY_VERIFY_WITH_TIMEOUT(
        session->frame() && !session->frame()->vertices.empty(),
        20000);
    const auto ibmStart = session->frame();
    session->seek(1.0);
    QTRY_VERIFY_WITH_TIMEOUT(session->frame()->revision > ibmStart->revision,
                             20000);
    QVERIFY2(maxVertexDelta(ibmStart, session->frame()) > 0.001,
             "identity-bind skinning must follow the bone");

    // Cubic spline: evaluates without errors and produces motion.
    openAndWait("cubic.gltf");
    QVERIFY(session->animated());
    const auto cubicStart = session->frame();
    QVERIFY(cubicStart);
    session->seek(1.0);
    QTRY_VERIFY_WITH_TIMEOUT(session->frame()->revision > cubicStart->revision,
                             20000);
    QVERIFY2(session->frameError().isEmpty(),
             qPrintable(session->frameError()));
    QVERIFY(maxVertexDelta(cubicStart, session->frame()) > 0.001);

    // Step: between two keys the pose equals the earlier key exactly; at
    // the next key it changes (no interpolation).
    openAndWait("step.gltf");
    QVERIFY(session->animated());
    session->seek(1.0);
    QTRY_VERIFY_WITH_TIMEOUT(
        session->frame() && session->frame()->vertices.size() > 0, 20000);
    const auto stepAtKey = session->frame();
    session->seek(1.5);
    QTRY_VERIFY_WITH_TIMEOUT(session->frame()->revision > stepAtKey->revision,
                             20000);
    QCOMPARE(maxVertexDelta(stepAtKey, session->frame()), 0.0);
    const auto stepMid = session->frame();
    session->seek(2.0);
    QTRY_VERIFY_WITH_TIMEOUT(session->frame()->revision > stepMid->revision,
                             20000);
    QVERIFY2(maxVertexDelta(stepMid, session->frame()) > 0.001,
             "the next STEP key must change the pose");

    // Morph weights channel: SUPPORTED since 2026-10-23 (linear weights on a
    // mesh node drive the preview); no unsupported-path warning remains, and
    // the weights themselves must animate the mesh.
    openAndWait("morph.gltf");
    QVERIFY(session->animated());
    QVERIFY2(!session->warnings().join(';').contains(
                 "Unsupported animation path"),
             qPrintable(session->warnings().join(';')));
    QTRY_VERIFY_WITH_TIMEOUT(
        session->frame() && !session->frame()->vertices.empty(), 20000);
    const auto morphStart = session->frame();
    QVERIFY(morphStart);
    session->seek(1.0);
    QTRY_VERIFY_WITH_TIMEOUT(session->frame()->revision > morphStart->revision,
                             20000);
    QVERIFY(maxVertexDelta(morphStart, session->frame()) > 0.001);
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelAnimationEdgeMatrix() {
    try {
    // M07 edge-case matrix (Blender-made samples, self-authored): multiple
    // skeleton roots, helper (non-bone) parents carrying motion, rigid
    // node-animated meshes without skins, fully disjoint external skeletons
    // refused, and multi-skin glTF driven natively.
    const QDir samples(requireFixture("FORMAVIEW_EDGE_SAMPLES_DIR"));
    if (!samples.exists()) {
      QSKIP("Set FORMAVIEW_EDGE_SAMPLES_DIR to the Blender-made samples");
      return;
    }
    auto *session = qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    const auto restore = qScopeGuard([&] {
      if (session->externalInfo().value("driven").toBool())
        session->detachExternalAnimation();
      desktop_->clearAsset();
    });
    const auto openAndWait = [&](const QString &name) {
      QSignalSpy documentLoaded(session, &ModelSession::documentChanged);
      const auto before = documentLoaded.count();
      desktop_->openPath(samples.filePath(name));
      QTRY_VERIFY_WITH_TIMEOUT(
          documentLoaded.count() > before && !session->busy() &&
              (session->inspectable() || !session->error().isEmpty()),
          20000);
      QVERIFY2(session->inspectable(),
               qPrintable(name + ": " + session->error()));
    };
    const auto maxVertexDelta =
        [](const std::shared_ptr<const formaview::render::Frame> &a,
           const std::shared_ptr<const formaview::render::Frame> &b) {
          if (!a || !b || a->vertices.size() != b->vertices.size() ||
              a->vertices.empty())
            return -1.0;
          double delta = 0;
          for (std::size_t i = 0; i < a->vertices.size(); ++i) {
            const auto &va = a->vertices[i];
            const auto &vb = b->vertices[i];
            delta = std::max({delta,
                              std::abs(double(va.x) - double(vb.x)),
                              std::abs(double(va.y) - double(vb.y)),
                              std::abs(double(va.z) - double(vb.z))});
          }
          return delta;
        };

    // Rigid node animation: a skinned-less mesh must follow its node.
    openAndWait("rigid.gltf");
    QVERIFY(session->animated());
    QTRY_VERIFY_WITH_TIMEOUT(
        session->frame() && !session->frame()->vertices.empty(), 20000);
    const auto rigidStart = session->frame();
    session->seek(1.0);
    QTRY_VERIFY_WITH_TIMEOUT(session->frame()->revision > rigidStart->revision,
                             20000);
    QVERIFY2(maxVertexDelta(rigidStart, session->frame()) > 0.001,
             "rigid node-animated mesh must move with its node");

    // Multi-skin glTF, natively animated: both meshes skin, and both
    // weighted root chains appear as lock candidates.
    openAndWait("multi-skin.gltf");
    QVERIFY(session->animated());
    QTRY_VERIFY_WITH_TIMEOUT(
        session->frame() && !session->frame()->vertices.empty(), 20000);
    const auto multiSkinStart = session->frame();
    session->seek(1.0);
    QTRY_VERIFY_WITH_TIMEOUT(
        session->frame()->revision > multiSkinStart->revision, 20000);
    QVERIFY2(maxVertexDelta(multiSkinStart, session->frame()) > 0.001,
             "multi-skin meshes must follow their clips");
    const auto candidates = session->rootCandidates();
    QVERIFY2(candidates.size() >= 2,
             qPrintable(QString("expected multiple weighted roots, got %1")
                        .arg(candidates.size())));
    session->setRootIndex(candidates.front().toMap().value("index").toInt());
    session->setRootLock(true);
    QCOMPARE(session->rootLockInfo().value("status").toString(),
             QStringLiteral("locked"));
    session->setRootLock(false);

    // Helper (non-bone) parent carrying motion: self-drive stays verified
    // through the full node domain and the helper's animation shows.
    openAndWait("helper-parent.fbx");
    QVERIFY(session->canAttachExternalAnimation());
    session->attachExternalAnimation(samples.filePath("helper-parent.fbx"));
    QTRY_VERIFY_WITH_TIMEOUT((!session->externalBusy() && session->externalInfo().value("driven").toBool()) ||
                                !session->externalInfo()
                                     .value("notice")
                                     .toString()
                                     .isEmpty(),
                             30000);
    QCOMPARE(session->externalInfo().value("status").toString(),
             QStringLiteral("verified"));
    QTRY_VERIFY_WITH_TIMEOUT(
        session->frame() && !session->frame()->vertices.empty(), 30000);
    const auto helperStart = session->frame();
    session->seek(1.0);
    QTRY_VERIFY_WITH_TIMEOUT(
        session->frame()->revision > helperStart->revision, 30000);
    QVERIFY2(maxVertexDelta(helperStart, session->frame()) > 0.001,
             "helper-parent animation must reach the skinned mesh");

    // Disjoint external skeleton: the drive is refused with a reason, and
    // the target document stays inspectable and un-driven.
    openAndWait("pair-target.fbx");
    QVERIFY(session->canAttachExternalAnimation());
    session->attachExternalAnimation(
        samples.filePath("pair-external-unmapped.fbx"));
    QTRY_VERIFY_WITH_TIMEOUT(
        !session->externalBusy() &&
            !session->externalInfo().value("status").toString().isEmpty(),
        30000);
    QCOMPARE(session->externalInfo().value("status").toString(),
             QStringLiteral("refused"));
    QVERIFY(!session->externalInfo().value("driven").toBool());
    QVERIFY2(!session->externalInfo().value("notice").toString().isEmpty(),
             "refusal must explain itself");
    QVERIFY(session->inspectable());

    // Multiple top-level armatures in one FBX: self-drive verifies and the
    // root lock operates on the weighted chain.
    openAndWait("multi-root.fbx");
    QVERIFY(session->canAttachExternalAnimation());
    session->attachExternalAnimation(samples.filePath("multi-root.fbx"));
    QTRY_VERIFY_WITH_TIMEOUT((!session->externalBusy() && session->externalInfo().value("driven").toBool()) ||
                                !session->externalInfo()
                                     .value("notice")
                                     .toString()
                                     .isEmpty(),
                             30000);
    QCOMPARE(session->externalInfo().value("status").toString(),
             QStringLiteral("verified"));
    QVERIFY(!session->rootCandidates().isEmpty());
    const auto multiRootStart = session->frame();
    QVERIFY(multiRootStart);
    session->seek(1.0);
    QTRY_VERIFY_WITH_TIMEOUT(
        session->frame()->revision > multiRootStart->revision, 30000);
    QVERIFY(maxVertexDelta(multiRootStart, session->frame()) > 0.001);
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelFbxNativeAnimation() {
    try {
    const auto paths = requireFixture("FORMAVIEW_NATIVE_FBX_SAMPLES").split(';', Qt::SkipEmptyParts);
    QVERIFY2(!paths.empty(), "Set FORMAVIEW_NATIVE_FBX_SAMPLES to mesh+animation FBX files");
    auto *session = qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    const auto restore = qScopeGuard([&] { desktop_->clearAsset(); });
    for (const auto &path : paths) {
      try {
      std::size_t nativeCount = 0;
      {
        const auto raw = formaview::skeletal::FbxDocument::read(std::filesystem::path(path.toStdWString()));
        qInfo() << "native FBX inventory" << path << "meshes" << raw.data().meshes.size()
                << "clips" << raw.data().clips.size() << "nodes" << raw.data().nodes.size();
        QVERIFY(!raw.data().meshes.empty());
        QVERIFY(!raw.data().clips.empty());
        nativeCount = raw.data().clips.size();
        const auto document = readModelDocument(path, {});
        QVERIFY2(document->animated(), qPrintable(path + " must automatically expose native clips"));
        QVERIFY(document->nativeAnimated);
        QVERIFY(!document->skeletonOnly);
        QVERIFY(document->gpuSkinDomain());
        QVERIFY(document->canAttachExternal());
        QCOMPARE(document->animationClips.size(), nativeCount);
        for (std::size_t c = 0; c < nativeCount; ++c) {
          QCOMPARE(document->animationClips[c].start, raw.data().clips[c].start);
          QCOMPARE(document->animationClips[c].end, raw.data().clips[c].end);
          for (double t : {raw.data().clips[c].start, (raw.data().clips[c].start+raw.data().clips[c].end)*.5}) {
            QCOMPARE(document->localAt(c,t), raw.evaluate(c,t).local);
            QCOMPARE(document->worldAt(c,t), raw.evaluate(c,t).world);
          }
        }
      }
      session->open(path);
      QTRY_VERIFY_WITH_TIMEOUT(!session->busy() && session->inspectable(), 120000);
      QVERIFY(session->animated());
      QVERIFY(session->canAttachExternalAnimation());
      const auto nativeClips = session->clips();
      QCOMPARE(nativeClips.size(), qsizetype(nativeCount));
      const auto digest = session->sourceDataDigest();
      session->setRootLock(false);
      const auto nativeTime = (nativeClips.front().toMap().value("start").toDouble()+nativeClips.front().toMap().value("end").toDouble())*.5;
      const auto initialRevision = session->frame()->revision;
      session->seek(nativeTime);
      QTRY_VERIFY_WITH_TIMEOUT(session->frame()->revision > initialRevision, 30000);
      const auto nativePose = session->frame();
      // An explicitly attached copy remains an external source after native clips.
      session->attachExternalAnimation(path);
      QTRY_VERIFY_WITH_TIMEOUT(!session->externalBusy(), 120000);
      QVERIFY2(session->externalInfo().value("driven").toBool(), qPrintable(session->externalInfo().value("notice").toString()));
      QCOMPARE(session->clips().size(), qsizetype(nativeCount*2));
      QCOMPARE(session->clipIndex(), int(nativeCount));
      for (qsizetype c=0; c<nativeClips.size(); ++c) QCOMPARE(session->clips()[c], nativeClips[c]);
      QCOMPARE(session->sourceDataDigest(), digest);
      const auto attachedRevision = session->frame()->revision;
      session->setClipIndex(0);
      session->seek(nativeTime);
      QTRY_VERIFY_WITH_TIMEOUT(session->frame()->revision > attachedRevision && !session->preparing(), 30000);
      QVERIFY(session->frameError().isEmpty());
      QCOMPARE(session->frame()->vertices.size(), nativePose->vertices.size());
      for (std::size_t v=0; v<nativePose->vertices.size(); ++v) {
        QCOMPARE(session->frame()->vertices[v].x, nativePose->vertices[v].x);
        QCOMPARE(session->frame()->vertices[v].y, nativePose->vertices[v].y);
        QCOMPARE(session->frame()->vertices[v].z, nativePose->vertices[v].z);
      }
      const auto roots = session->rootCandidates();
      QVERIFY(!roots.isEmpty());
      session->setRootIndex(roots.front().toMap().value("index").toInt());
      session->setRootLock(true);
      QVERIFY(session->rootLockInfo().value("rootMapped").toBool());
      QCOMPARE(session->rootLockInfo().value("status").toString(), QString("locked"));
      session->setRootLock(false);
      session->detachExternalAnimation();
      QTRY_VERIFY_WITH_TIMEOUT(!session->busy() && session->animated() && !session->externalInfo().value("driven").toBool(), 120000);
      QCOMPARE(session->clips(), nativeClips);
      QCOMPARE(session->sourceDataDigest(), digest);
      session->clear();
      } catch (const std::exception &e) {
        QFAIL(qPrintable(path + ": " + QString::fromUtf8(e.what())));
      }
    }
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }

  void modelExternalAnimationDrive() {
    try {
    // M07 external FBX animation driving a target FBX: the target keeps its
    // own skeleton/skin/bind data verbatim (digest asserted), the mapping is
    // name+chain based, playback publishes skinned frames, detach restores
    // the static model.
    const auto target =
        QFileInfo(requireFixture("FORMAVIEW_ANIMATION_TARGET"));
    const auto external =
        QFileInfo(requireFixture("FORMAVIEW_ANIMATION_EXTERNAL"));
    if (!target.exists() || !external.exists() ||
        target.suffix().compare("fbx", Qt::CaseInsensitive) != 0 ||
        external.suffix().compare("fbx", Qt::CaseInsensitive) != 0) {
      QSKIP("Set FORMAVIEW_ANIMATION_TARGET (skinned FBX) and "
            "FORMAVIEW_ANIMATION_EXTERNAL (animated FBX)");
      return;
    }
    auto *session = qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    const auto restore = qScopeGuard([&] {
      if (session->externalInfo().value("driven").toBool())
        session->detachExternalAnimation();
      desktop_->clearAsset();
    });
    QSignalSpy documentLoaded(session, &ModelSession::documentChanged);
    const auto openAndWait = [&](const QFileInfo &file) {
      const auto before = documentLoaded.count();
      desktop_->openPath(file.absoluteFilePath());
      QTRY_VERIFY_WITH_TIMEOUT(
          documentLoaded.count() > before && !session->busy() &&
              (session->inspectable() || !session->error().isEmpty()),
          30000);
      QVERIFY2(session->inspectable(),
               qPrintable(session->error() + session->frameError()));
    };
    openAndWait(target);
    const auto expectedNative = formaview::skeletal::FbxDocument::read(std::filesystem::path(target.absoluteFilePath().toStdWString())).data().clips.size();
    const auto nativeClips = session->clips();
    QCOMPARE(nativeClips.size(), qsizetype(expectedNative));
    QCOMPARE(session->animated(), expectedNative > 0);
    QVERIFY(session->canAttachExternalAnimation());
    // Product-surface check: the bar and its attach entry must be visible.
    QTRY_VERIFY(item("modelAnimationBarHost") &&
                item("modelAnimationBarHost")->isVisible());
    QTRY_VERIFY(item("animExternalAttach") &&
                item("animExternalAttach")->isVisible() &&
                item("animExternalAttach")->isEnabled());
    // Keep geometry checks independent from root-lock verification so each
    // regression still runs if the other one fails.
    const auto verifyExternalToolbar = [&]() {
    const auto oldLanguage = desktop_->language();
    const auto oldSize = window_->size();
    const auto restoreToolbar = qScopeGuard([&] {
      window_->resize(oldSize);
      desktop_->setLanguage(oldLanguage);
    });
    for (const auto &language : {QStringLiteral("zh-CN"), QStringLiteral("en"), QStringLiteral("zh-TW")}) {
      desktop_->setLanguage(language);
      for (const auto &size : {QSize(1600, 900), QSize(1280, 800)}) {
        window_->resize(size);
        QTest::qWait(80);
        auto *pane = item("modelPane");
        auto *attach = item("animExternalAttach");
        auto *lastTab = item("modelModeTab3");
        QVERIFY(pane && attach && lastTab);
        const auto position = attach->mapToItem(pane, QPointF());
        const auto tabPosition = lastTab->mapToItem(pane, QPointF());
        QVERIFY(position.x() >= tabPosition.x() + lastTab->width() - 1);
        for (const auto *name : {"modelGroundGrid", "modelOriginAxes", "modelFocus", "modelFrameAll", "modelCompareSkeleton"}) {
          auto *button = item(name);
          QVERIFY(button && button->isVisible());
          const auto point = button->mapToItem(pane, QPointF());
          QVERIFY(qAbs(point.y() - position.y()) < 1);
          QVERIFY(point.x() >= position.x() + attach->width() - 1);
          QVERIFY2(point.x() + button->width() <= pane->width() + 1,
                   qPrintable(QString("%1 %2 window=%3 pane=%4 right=%5")
                       .arg(language, name).arg(size.width()).arg(pane->width()).arg(point.x() + button->width())));
          if (QString::fromLatin1(name) != "modelCompareSkeleton") {
            QVERIFY(button->property("text").toString().isEmpty());
            QVERIFY(!button->property("iconName").toString().isEmpty());
            QVERIFY(!button->property("hint").toString().isEmpty());
            QVERIFY(button->width() <= 33);
          }
        }
        save("external-toolbar-" + language + "-" + QString::number(size.width()));
      }
    }
    };
    verifyExternalToolbar();

    // Digest the target's skeleton/skin identity before the drive: palettes
    // (geometryToBone/bindWorld per bone) and per-point weights.
    const auto skinDigest = [&]() {
      QCryptographicHash hash(QCryptographicHash::Sha256);
      hash.addData(session->sourceDataDigest());
      // Serialize through the session info + a direct document walk via the
      // exposed QVariant surface: bindings(), bones(), influences per point.
      hash.addData(session->bindings().join('|').toUtf8());
      hash.addData(session->bones().join('|').toUtf8());
      for (const auto &meshName : session->meshes())
        hash.addData(meshName.toUtf8());
      for (const auto &influence : session->influences()) {
        const auto map = influence.toMap();
        hash.addData(map.value("bone").toString().toUtf8());
        hash.addData(QByteArray::number(map.value("weight").toDouble(), 'g', 17));
      }
      hash.addData(session->info().value("sha256").toString().toUtf8());
      return hash.result().toHex();
    };
    const auto before = skinDigest();

    session->attachExternalAnimation(external.absoluteFilePath());
    QTRY_VERIFY_WITH_TIMEOUT((!session->externalBusy() && session->externalInfo().value("driven").toBool()) ||
                                 !session->externalInfo()
                                      .value("notice")
                                      .toString()
                                      .isEmpty(),
                             30000);
    const auto info = session->externalInfo();
    QVERIFY2(info.value("driven").toBool(),
             qPrintable(info.value("notice").toString()));
    QVERIFY(info.value("status").toString() == "verified" ||
            info.value("status").toString() == "trial");
    QVERIFY(session->clips().size() >= 1);
    QVERIFY(session->frame() && !session->frame()->vertices.empty());
    QTRY_VERIFY_WITH_TIMEOUT(item("animExternalAttach")->isEnabled(), 10000);

    // Play and confirm advancing frames.
    // Other cases deliberately leave speed=2 and loop=false. Isolate this
    // short clip's playback and await the asynchronous mesh publication.
    session->setSpeed(1.0);
    session->setLoop(true);
    const auto revisionBefore = session->frame()->revision;
    session->togglePlay();
    QVERIFY(session->playing());
    for (int i = 0; i < 30 && session->playing(); ++i)
      QTest::qWait(25);
    if (session->playing())
      session->togglePlay();
    QTRY_VERIFY_WITH_TIMEOUT(session->frame()->revision > revisionBefore ||
                                 !session->frameError().isEmpty(), 10000);
    QVERIFY2(session->frameError().isEmpty(), qPrintable(session->frameError()));
    QVERIFY2(session->frame()->revision > revisionBefore,
             qPrintable(QString("No frame: before=%1 after=%2 time=%3 root=%4 base=%5 on=%6")
                 .arg(revisionBefore).arg(session->frame()->revision).arg(session->time())
                 .arg(session->rootLockInfo().value("rootName").toString())
                 .arg(session->rootBase()).arg(session->rootLockOn())));

    // Target immutability: the whole bind/weight surface is unchanged.
    QCOMPARE(skinDigest(), before);

    // A second attach (same source) accumulates sources instead of hiding
    // the entry: the button stays visible and the clip list grows. A third
    // file queued IMMEDIATELY (multi-select semantics) also lands.
    const auto clipsBefore = session->clips().size();
    const auto external2 = QFileInfo(
        requireFixture("FORMAVIEW_ANIMATION_EXTERNAL2"));
    if (external2.exists()) {
      const auto secondClipCount = formaview::skeletal::FbxDocument::read(std::filesystem::path(external2.absoluteFilePath().toStdWString())).data().clips.size();
      const auto expectedClips = clipsBefore + (clipsBefore-nativeClips.size()) + qsizetype(secondClipCount);
      session->attachExternalAnimation(external.absoluteFilePath());
      session->attachExternalAnimation(external2.absoluteFilePath());
      QTRY_VERIFY_WITH_TIMEOUT(
          !session->busy() && !session->externalBusy() &&
              session->clips().size() == expectedClips,
          30000);
    } else {
      session->attachExternalAnimation(external.absoluteFilePath());
      QTRY_VERIFY_WITH_TIMEOUT(!session->busy() && !session->externalBusy() &&
                                   session->clips().size() == clipsBefore + (clipsBefore-nativeClips.size()),
                               30000);
    }
    QVERIFY(item("animExternalAttach") && item("animExternalAttach")->isVisible());
    QVERIFY(item("animExternalAttach")->isEnabled());
    QCOMPARE(skinDigest(), before);

    // Verify visible geometry, not just the UI's lock status. Keep the
    // clock paused and compare the exact same source time with a fixed camera.
    {
      const auto source = formaview::skeletal::FbxDocument::read(
          std::filesystem::path(external.absoluteFilePath().toStdWString()));
      const auto targetDoc = readModelDocument(target.absoluteFilePath(), {});
      const auto sample = source.data().clips.front();
      const auto t = sample.start + (sample.end - sample.start) * 0.75;
      const auto first = source.evaluate(0, sample.start);
      const auto later = source.evaluate(0, t);
      for (std::size_t i = 0; i < source.data().nodes.size(); ++i) {
        const auto &node = source.data().nodes[i];
        if (i < 8) {
          double delta = 0;
          for (std::size_t e = 0; e < 16; ++e)
            delta = std::max(delta, std::abs(first.local[i][e] - later.local[i][e]));
          qInfo() << "root investigation source" << i
                  << QString::fromStdString(node.source.original_name)
                  << "parent" << (node.parent ? int(*node.parent) : -1)
                  << "bone" << node.bone << "local delta" << delta;
        }
      }
      for (int i = 0; i < std::min(8, int(targetDoc->nodes.size())); ++i)
        qInfo() << "root investigation target" << i
                << targetDoc->nodes[i].toMap().value("name")
                << "parent" << targetDoc->animNodeParents[i]
                << "bone" << targetDoc->nodes[i].toMap().value("bone");
      qInfo() << "root investigation candidates" << session->rootCandidates();
      QCOMPARE(session->rootCandidates().size(), 1);
      QCOMPARE(session->rootLockInfo().value("rootName").toString(), QStringLiteral("Trans"));
      QVERIFY(session->rootLockInfo().value("rootMapped").toBool());
      auto *view = qobject_cast<ModelView *>(item("modelViewport"));
      QVERIFY(view);
      view->orbit(0, 0); // Freeze camera to expose root motion in pixel comparisons.
      const auto camera = view->cameraState();
      const auto pixels = [&] {
        const auto image = captureWindow(window_);
        const auto scale = image.width() / double(window_->width());
        const auto point = view->mapToScene({});
        return image.copy(QRect(int(point.x()*scale), int(point.y()*scale),
                                int(view->width()*scale), int(view->height()*scale)));
      };
      session->setClipIndex(int(nativeClips.size()));
      QCOMPARE(session->clipInfo().value("name").toString(), QString::fromStdString(source.data().clips.front().source.original_name));
      session->setRootLock(false);
      session->setRootBase(0);
      auto revision = session->frame()->revision;
      session->seek(t);
      QTRY_VERIFY_WITH_TIMEOUT(session->frame()->revision > revision, 10000);
      // Drain any prior frame and request t once more before taking baseline.
      QTest::qWait(300);
      revision = session->frame()->revision;
      session->seek(t);
      QTRY_VERIFY_WITH_TIMEOUT(session->frame()->revision > revision, 10000);
      const auto unlocked = session->frame();
      QTest::qWait(80);
      auto *bar = item("modelAnimationBarHost");
      QVERIFY(bar);
      for (const auto *name : {"animExternalStatus", "animExternalRemove", "animScrubber", "animRootStatus"}) {
        auto *control = item(name);
        QVERIFY(control);
        const auto point = control->mapToItem(bar, QPointF());
        QVERIFY2(point.x() >= -1 && point.x() + control->width() <= bar->width() + 1, name);
      }
      const auto unlockedPixels = pixels();
      save("external-root-unlocked");
      revision = unlocked->revision;
      session->setRootLock(true);
      QTRY_VERIFY_WITH_TIMEOUT(session->frame()->revision > revision, 10000);
      const auto locked = session->frame();
      QCOMPARE(locked->vertices.size(), unlocked->vertices.size());
      double maxDelta = 0;
      for (std::size_t i = 0; i < locked->vertices.size(); ++i) {
        const auto &a = unlocked->vertices[i];
        const auto &b = locked->vertices[i];
        maxDelta = std::max(maxDelta, double(QVector3D(a.x-b.x, a.y-b.y, a.z-b.z).length()));
      }
      qInfo() << "root investigation locked" << session->rootLockInfo()
              << "time" << t << "max vertex displacement" << maxDelta;
      QVERIFY2(maxDelta > 0.001, "Root lock must change real skinned model vertices at the same time");
      QTest::qWait(80);
      const auto lockedPixels = pixels();
      QVERIFY(unlockedPixels != lockedPixels);
      QCOMPARE(view->cameraState(), camera);
      save("external-root-locked");
      // An independent world-space correction from source matrices must
      // describe the locked mesh (all weighted bones share Trans here).
      const auto qmatrix = [](const formaview::Matrix4 &m) {
        QMatrix4x4 q;
        for (int r = 0; r < 4; ++r)
          for (int c = 0; c < 4; ++c)
            q(r, c) = float(m[std::size_t(r*4+c)]);
        return q;
      };
      const auto correction = qmatrix(first.world[1]) * qmatrix(later.world[1]).inverted();
      const auto inverseDisplay = targetDoc->displayBasis.inverted();
      double correctionError = 0;
      for (std::size_t i = 0; i < locked->vertices.size(); ++i) {
        const auto &a = unlocked->vertices[i];
        const auto &b = locked->vertices[i];
        const auto raw = inverseDisplay.map(QVector3D(a.x,a.y,a.z) * targetDoc->radius + targetDoc->center);
        const auto expected = (targetDoc->displayBasis.map(correction.map(raw)) - targetDoc->center) / targetDoc->radius;
        correctionError = std::max(correctionError, double((expected - QVector3D(b.x,b.y,b.z)).length()));
      }
      qInfo() << "root independent world correction max error" << correctionError;
      QVERIFY(correctionError < 0.00005);
      revision = locked->revision;
      session->setRootLock(false);
      QTRY_VERIFY_WITH_TIMEOUT(session->frame()->revision > revision, 10000);
      const auto restored = session->frame();
      QCOMPARE(restored->vertices.size(), unlocked->vertices.size());
      for (std::size_t i = 0; i < restored->vertices.size(); ++i) {
        QCOMPARE(restored->vertices[i].x, unlocked->vertices[i].x);
        QCOMPARE(restored->vertices[i].y, unlocked->vertices[i].y);
        QCOMPARE(restored->vertices[i].z, unlocked->vertices[i].z);
      }
      QTest::qWait(80);
      QVERIFY(pixels() == unlockedPixels);
      save("external-root-restored");
      // Burst requests without pumping events guarantee a job is in flight.
      // The final paused state must eventually land, without a playback tick.
      revision = restored->revision;
      session->seek(sample.start);
      session->setRootLock(true);
      session->setRootBase(2);
      session->seek(t * 0.5);
      session->setRootBase(0);
      session->setRootLock(false);
      session->seek(t);
      QTRY_VERIFY_WITH_TIMEOUT(session->frame()->revision > revision, 10000);
      const auto coalesced = session->frame();
      QCOMPARE(coalesced->vertices.size(), unlocked->vertices.size());
      for (std::size_t i = 0; i < coalesced->vertices.size(); ++i) {
        QCOMPARE(coalesced->vertices[i].x, unlocked->vertices[i].x);
        QCOMPARE(coalesced->vertices[i].y, unlocked->vertices[i].y);
        QCOMPARE(coalesced->vertices[i].z, unlocked->vertices[i].z);
      }
      evidence_["external_root_lock"] = QJsonObject{
          {"time", t}, {"root", session->rootLockInfo().value("rootName").toString()},
          {"rootMapped", session->rootLockInfo().value("rootMapped").toBool()},
          {"maxVertexDisplacement", maxDelta}, {"independentCorrectionMaxError", correctionError},
          {"sameCameraViewportPixelsChanged", unlockedPixels != lockedPixels},
          {"unlockRestoresAllVertices", true}, {"pausedBurstLandsLatestState", true}};
    }
    // Root lock is available on the externally driven FBX document.
    {
      const auto candidates = session->rootCandidates();
      QVERIFY(!candidates.isEmpty());
      if (candidates.size() > 1)
        session->setRootIndex(
            candidates.front().toMap().value("index").toInt());
      session->setRootLock(true);
      QTRY_VERIFY_WITH_TIMEOUT(
          session->rootLockInfo().value("on").toBool() &&
              session->rootLockInfo().value("status").toString() ==
                  QStringLiteral("locked"),
          10000);
      // The async frame pipeline still publishes after the lock toggle.
      const auto lockedRevision = session->frame()->revision;
      session->seek(session->clipInfo().value("end").toDouble() * 0.5);
      QTRY_VERIFY_WITH_TIMEOUT(session->frame()->revision > lockedRevision,
                               10000);
      QVERIFY(session->frameError().isEmpty());
      session->setRootLock(false);
    }

    // Detach restores the original native/static model document.
    session->detachExternalAnimation();
    QTRY_VERIFY_WITH_TIMEOUT(!session->busy(), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(session->animated() == (expectedNative > 0) &&
                                 session->canAttachExternalAnimation(),
                             30000);
    QCOMPARE(session->clips(), nativeClips);
    QCOMPARE(skinDigest(), before);
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelExternalAnimationMultiSelect() {
    try {
    // One multi-select dialog submits every file at once (ModelPane loops
    // attachExternalAnimation). None may be silently dropped: each file is
    // either applied or reported, and clips follow the selection order.
    const QFileInfo target(requireFixture("FORMAVIEW_PSK_TARGET"));
    const QFileInfo psa(requireFixture("FORMAVIEW_PSK_PSA"));
    if (!target.exists() || !psa.exists())
      QSKIP("Set FORMAVIEW_PSK_TARGET/PSA to real ActorX inputs");
    QStringList files;
    for (const auto &entry : psa.dir().entryInfoList({"*.psa"}, QDir::Files, QDir::Name))
      files.append(entry.absoluteFilePath());
    QVERIFY2(files.size() > 8, "Multi-select regression needs more than the concurrent job budget");
    auto *session = qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    const auto restore = qScopeGuard([&] { desktop_->clearAsset(); });
    desktop_->openPath(target.absoluteFilePath());
    QTRY_VERIFY_WITH_TIMEOUT(!session->busy() && session->inspectable(), 30000);
    for (const auto &file : files)
      session->attachExternalAnimation(QUrl::fromLocalFile(file).toString());
    QVERIFY(session->externalBusy());
    QTRY_VERIFY_WITH_TIMEOUT(!session->externalBusy(), 600000);
    const auto info = session->externalInfo();
    qInfo().noquote() << "multi-select:" << files.size() << "files," << session->clips().size() << "clips";
    const auto failed = info.value("failedFiles").toStringList();
    QCOMPARE(info.value("requested").toInt(), files.size());
    QCOMPARE(info.value("applied").toInt() + failed.size(), files.size());
    QVERIFY2(info.value("applied").toInt() > 8, qPrintable(info.value("notice").toString()));
    for (const auto &name : failed)
      QVERIFY2(info.value("notice").toString().contains(name), qPrintable(name));
    QVERIFY(session->animated());
    QVERIFY(session->clips().size() >= info.value("applied").toInt());
    // Selection order: the first file's first sequence opens the clip list.
    const auto first = readModelDocument(files.front(), {}, target.absoluteFilePath(),
        psa.dir().filePath(QFileInfo(files.front()).completeBaseName() + ".config"));
    if (!failed.contains(QFileInfo(files.front()).fileName()))
      QCOMPARE(session->clips().front().toMap().value("name"), first->clips[0].toMap().value("name"));
    // FBX-target drive shares the queue but applies through its own holders.
    const QFileInfo fbxTarget(requireFixture("FORMAVIEW_ANIMATION_TARGET"));
    const QFileInfo fbxExternal(requireFixture("FORMAVIEW_ANIMATION_EXTERNAL"));
    if (!fbxTarget.exists() || !fbxExternal.exists())
      return;
    QStringList fbxFiles;
    for (const auto &entry : fbxExternal.dir().entryInfoList({"*.fbx"}, QDir::Files, QDir::Name))
      if (fbxFiles.size() < 12) fbxFiles.append(entry.absoluteFilePath());
    QVERIFY(fbxFiles.size() > 8);
    desktop_->openPath(fbxTarget.absoluteFilePath());
    QTRY_VERIFY_WITH_TIMEOUT(!session->busy() && session->inspectable() && session->canAttachExternalAnimation(), 60000);
    for (const auto &file : fbxFiles)
      session->attachExternalAnimation(QUrl::fromLocalFile(file).toString());
    QTRY_VERIFY_WITH_TIMEOUT(!session->externalBusy(), 600000);
    const auto fbxInfo = session->externalInfo();
    qInfo().noquote() << "multi-select fbx:" << fbxFiles.size() << "files," << session->clips().size() << "clips";
    QCOMPARE(fbxInfo.value("requested").toInt(), fbxFiles.size());
    QCOMPARE(fbxInfo.value("applied").toInt() + fbxInfo.value("failedFiles").toStringList().size(), fbxFiles.size());
    QVERIFY2(fbxInfo.value("applied").toInt() > 8, qPrintable(fbxInfo.value("notice").toString()));
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelPskExternalAnimationDrive() {
    try {
    const QFileInfo target(requireFixture("FORMAVIEW_PSK_TARGET"));
    const QFileInfo psa(requireFixture("FORMAVIEW_PSK_PSA"));
    const QFileInfo psa2(requireFixture("FORMAVIEW_PSK_PSA2"));
    const QFileInfo incompatible(requireFixture("FORMAVIEW_PSK_INCOMPATIBLE"));
    if (!target.exists() || !psa.exists() || !psa2.exists() || !incompatible.exists())
      QSKIP("Set FORMAVIEW_PSK_TARGET/PSA/PSA2/INCOMPATIBLE to real ActorX inputs");
    auto *session = qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    const auto restore = qScopeGuard([&] { desktop_->clearAsset(); });
    desktop_->openPath(target.absoluteFilePath());
    QTRY_VERIFY_WITH_TIMEOUT(!session->busy() && session->inspectable(), 30000);
    QVERIFY(session->canAttachExternalAnimation());
    QCOMPARE(session->externalInfo().value("format").toString(), QString("psa"));
    QTRY_VERIFY(item("animExternalAttach") && item("animExternalAttach")->isVisible());
    QVERIFY(item("animExternalAttach")->property("hint").toString().contains("PSA"));
    auto *dialog = window_->findChild<QObject *>("externalAnimationDialog");
    QVERIFY(dialog);
    QVERIFY(dialog->property("nameFilters").toStringList().join(' ').contains("*.psa"));
    const auto original = session->frame();
    const auto hash = session->info().value("sha256");
    const auto bones = session->bones();
    const auto bindings = session->bindings();
    session->attachExternalAnimation(incompatible.absoluteFilePath());
    QTRY_VERIFY_WITH_TIMEOUT(!session->externalBusy(), 30000);
    QVERIFY(!session->animated());
    QCOMPARE(session->externalInfo().value("status").toString(), QString("refused"));
    // item() intentionally returns only visible items. Bindings can settle
    // after the async completion signal, and an invisible remove is null.
    QTRY_VERIFY(item("animExternalStatus"));
    QVERIFY(!item("animExternalRemove"));
    QCOMPARE(session->frame()->vertices.size(), original->vertices.size());
    // One dialog can queue multiple sources, each with its own config and
    // animation order. The target remains the selected PSK throughout.
    session->attachExternalAnimation(psa.absoluteFilePath());
    session->attachExternalAnimation(psa2.absoluteFilePath());
    QTRY_VERIFY_WITH_TIMEOUT(!session->externalBusy(), 30000);
    QVERIFY2(session->animated(), qPrintable(session->externalInfo().value("notice").toString()));
    session->setRootLock(false);
    QCOMPARE(session->externalInfo().value("status").toString(), QString("verified"));
    const int count = session->clips().size();
    QVERIFY(count >= 2);
    QTRY_VERIFY(item("animClip"));
    QTRY_COMPARE(item("animClip")->property("currentIndex").toInt(), session->clipIndex());
    QCOMPARE(item("animClip")->property("currentText").toString(), session->clipInfo().value("name").toString());
    QCOMPARE(session->info().value("sha256"), hash);
    QCOMPARE(session->bones(), bones);
    QCOMPARE(session->bindings(), bindings);
    QTRY_VERIFY(item("animExternalAttach")->isEnabled());
    session->setLoop(true);
    session->setSpeed(1.0);
    const auto playRevision = session->frame()->revision;
    session->togglePlay();
    QVERIFY(session->playing());
    QTRY_VERIFY_WITH_TIMEOUT(session->frame()->revision > playRevision && session->time() > 0.01, 10000);
    session->togglePlay();
    QVERIFY(!session->playing());
    // Compare against the established PSA-first paired path, preserving the
    // PSK model's normalization, at a nonzero time for BOTH sources.
    const auto targetDoc = readModelDocument(target.absoluteFilePath(), {});
    for (const auto &file : {psa, psa2}) {
      const auto config = file.dir().filePath(file.completeBaseName() + ".config");
      QVERIFY(QFileInfo::exists(config));
      const auto paired = readModelDocument(file.absoluteFilePath(), {},
                                            target.absoluteFilePath(), config);
      QVERIFY(!paired->referenceRejected && paired->pairSource);
      QVERIFY(!paired->companionPath.isEmpty());
      int index = -1;
      for (int i = 0; i < count; ++i)
        if (session->clips()[i].toMap().value("name") == paired->clips[0].toMap().value("name"))
          index = i;
      QVERIFY(index >= 0);
      session->setClipIndex(index);
      const double seconds = paired->animationClips[0].start +
          (paired->animationClips[0].end - paired->animationClips[0].start) * 0.43;
      const auto revision = session->frame()->revision;
      session->seek(seconds);
      QTRY_VERIFY_WITH_TIMEOUT(session->frame()->revision > revision, 30000);
      paired->center = targetDoc->center;
      paired->radius = targetDoc->radius;
      const auto expected = buildAnimationFrame(*paired, 0, seconds, 1, true);
      const auto actual = session->frame();
      QCOMPARE(actual->vertices.size(), expected->vertices.size());
      double error = 0, moved = 0;
      for (std::size_t i = 0; i < actual->vertices.size(); ++i) {
        const auto &a = actual->vertices[i];
        const auto &b = expected->vertices[i];
        error = std::max(error, static_cast<double>(QVector3D(a.x-b.x,a.y-b.y,a.z-b.z).length()));
        if (i < original->vertices.size()) {
          const auto &o = original->vertices[i];
          moved = std::max(moved, static_cast<double>(QVector3D(a.x-o.x,a.y-o.y,a.z-o.z).length()));
        }
      }
      qInfo() << "PSK drive" << file.fileName() << "pose max error" << error << "vertex change" << moved;
      QVERIFY(error < 1e-5);
      QVERIFY(moved > 0.01);
      QVERIFY(session->frameError().isEmpty());
      save("psk-drive-" + file.completeBaseName());
    }
    session->attachExternalAnimation(incompatible.absoluteFilePath());
    QTRY_VERIFY_WITH_TIMEOUT(!session->externalBusy(), 30000);
    QCOMPARE(session->externalInfo().value("status").toString(), QString("refused"));
    QCOMPARE(session->clips().size(), count);
    QVERIFY(session->animated());
    const auto roots = session->rootCandidates();
    QVERIFY(!roots.isEmpty());
    session->setRootIndex(roots.first().toMap().value("index").toInt());
    session->setRootLock(true);
    QVERIFY(session->rootLockInfo().value("rootMapped").toBool());
    QCOMPARE(session->rootLockInfo().value("status").toString(), QString("locked"));
    const auto rootRevision = session->frame()->revision;
    session->setRootBase(1);
    session->seek(session->clipInfo().value("end").toDouble() * 0.5);
    QTRY_VERIFY_WITH_TIMEOUT(session->frame()->revision > rootRevision, 10000);
    QVERIFY(session->frameError().isEmpty());
    session->setRootLock(false);
    QCOMPARE(session->info().value("sha256"), hash);
    session->detachExternalAnimation();
    QTRY_VERIFY_WITH_TIMEOUT(!session->busy() && session->inspectable() && !session->animated(), 30000);
    QVERIFY(session->canAttachExternalAnimation());
    QCOMPARE(session->info().value("sha256"), hash);
    QCOMPARE(session->bones(), bones);
    QCOMPARE(session->frame()->vertices.size(), original->vertices.size());
    for (std::size_t i = 0; i < original->vertices.size(); ++i) {
      QCOMPARE(session->frame()->vertices[i].x, original->vertices[i].x);
      QCOMPARE(session->frame()->vertices[i].y, original->vertices[i].y);
      QCOMPARE(session->frame()->vertices[i].z, original->vertices[i].z);
    }
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void animatedInspectionControls_data() {
    QTest::addColumn<bool>("actor");
    QTest::newRow("fbx") << false;
    QTest::newRow("psk") << true;
  }
  void animatedInspectionControls() {
    try {
    QFETCH(bool, actor);
    const QFileInfo target(requireFixture(actor ? "FORMAVIEW_PSK_TARGET" : "FORMAVIEW_ANIMATION_TARGET"));
    const QFileInfo animation(requireFixture(actor ? "FORMAVIEW_PSK_PSA" : "FORMAVIEW_ANIMATION_EXTERNAL"));
    if (!target.exists() || !animation.exists()) QSKIP("Set real PSK/PSA and FBX animation inputs");
    auto *session = qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    const auto restore = qScopeGuard([&] { desktop_->clearAsset(); });
    window_->showNormal();
    window_->resize(1600,900);
    desktop_->openPath(target.absoluteFilePath());
    QTRY_VERIFY_WITH_TIMEOUT(!session->busy() && session->inspectable(),30000);
    const auto originalClips = session->clips();
    const bool originalAnimated = session->animated();
    session->attachExternalAnimation(animation.absoluteFilePath());
    QTRY_VERIFY_WITH_TIMEOUT(!session->externalBusy() && session->animated(),30000);
    QVERIFY(session->externalInfo().value("driven").toBool());
    session->setMode(0);
    session->setRootLock(false);
    session->setSpeed(1);
    session->setLoop(true);
    if (!session->playing()) session->togglePlay();
    QTRY_VERIFY(item("animSpeed"));
    auto *speed = item("animSpeed");
    speed->forceActiveFocus();
    QTest::keyClick(window_,Qt::Key_A,Qt::ControlModifier);
    QTest::keyClick(window_,Qt::Key_0);
    QTest::keyClick(window_,Qt::Key_Period);
    QTest::qWait(200); // Many animation notifications while the value is partial.
    QCOMPARE(speed->property("text").toString(),QString("0."));
    QTest::keyClick(window_,Qt::Key_5);
    QTest::keyClick(window_,Qt::Key_0);
    QTest::qWait(150);
    QCOMPARE(speed->property("text").toString(),QString("0.50"));
    QTest::keyClick(window_,Qt::Key_Return);
    QTRY_COMPARE(session->speed(),0.5);
    QVERIFY(session->playing());
    const auto started = session->time();
    QTRY_VERIFY(session->time() != started);
    for (const auto &text : {QString("0.1"), QString("8"), QString("0.0001"), QString("1e2")}) {
      speed->forceActiveFocus();
      QTest::keyClick(window_,Qt::Key_A,Qt::ControlModifier);
      QInputMethodEvent input;
      input.setCommitString(text);
      QCoreApplication::sendEvent(speed,&input);
      QCOMPARE(speed->property("text").toString(),text);
      QTest::keyClick(window_,Qt::Key_Return);
      QCOMPARE(session->speed(),text.toDouble());
      QVERIFY(session->playing());
    }
    speed->setFocus(false);
    QTRY_COMPARE(speed->property("text").toString(),QString("100.00"));
    session->setSpeed(0.0001);
    QTRY_COMPARE(speed->property("text").toString(),QString("0.0001"));
    for (double invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
      session->setSpeed(invalid);
      QCOMPARE(session->speed(),0.0001);
    }
    session->setSpeed(std::numeric_limits<double>::max());
    QTest::qSleep(1100); // Delayed tick: finite speed must not produce an infinite clock.
    QCoreApplication::processEvents();
    QVERIFY(std::isfinite(session->time()));
    QVERIFY(session->time()>=session->clipInfo().value("start").toDouble());
    QVERIFY(session->time()<=session->clipInfo().value("end").toDouble());
    QVERIFY(session->playing());
    session->setSpeed(0.5);
    session->togglePlay();
    const double seconds = session->clipInfo().value("start").toDouble() +
        (session->clipInfo().value("end").toDouble()-session->clipInfo().value("start").toDouble())*.43;
    auto revision = session->frame()->revision;
    session->seek(seconds);
    QTRY_VERIFY_WITH_TIMEOUT(session->frame()->revision > revision,10000);
    const auto pose = session->frameFor(false);
    const auto source = readModelDocument(target.absoluteFilePath(),{});
    const auto meshIndex = static_cast<std::size_t>(session->meshIndex());
    const auto &mesh = source->meshes.at(meshIndex);
    std::size_t offset = 0;
    for (std::size_t i=0;i<meshIndex;++i) offset += source->meshes[i].triangles.size();
    const auto clickControl = [&](const char *name) {
      auto *control = item(name);
      QVERIFY(control);
      QTest::mouseClick(window_,Qt::LeftButton,Qt::NoModifier,
          control->mapToScene(QPointF(control->width()/2,control->height()/2)).toPoint());
    };
    clickControl("modelModeTab1");
    QTRY_VERIFY_WITH_TIMEOUT(session->frameFor(true) && session->frameFor(true)->uv && session->mode()==1,10000);
    QVERIFY(session->frameFor(true)->lines);
    QVERIFY(!session->frameFor(true)->vertices.empty());
    QCOMPARE(session->frameFor(false)->vertices.size(),mesh.triangles.size());
    for (std::size_t i=0;i<mesh.triangles.size();i+=97) {
      const auto &a=session->frameFor(false)->vertices[i];
      const auto &b=pose->vertices.at(offset+i);
      QCOMPARE(a.x,b.x); QCOMPARE(a.y,b.y); QCOMPARE(a.z,b.z);
    }
    const auto uv = session->frameFor(true);
    revision = uv->revision;
    session->seek(session->clipInfo().value("end").toDouble()*.71);
    QTRY_VERIFY_WITH_TIMEOUT(session->frameFor(true)->revision > revision,10000);
    QCOMPARE(session->frameFor(true)->vertices.size(),uv->vertices.size());
    for (std::size_t i=0;i<uv->vertices.size();i+=73) {
      const auto &a=session->frameFor(true)->vertices[i], &b=uv->vertices[i];
      QCOMPARE(a.x,b.x); QCOMPARE(a.y,b.y); QCOMPARE(a.u,b.u); QCOMPARE(a.v,b.v);
    }
    save(actor ? "psk-animation-uv" : "fbx-animation-uv");
    session->selectBinding(0);
    std::vector<int> counts(mesh.skins.at(0).bones.size());
    for (const auto &weights : mesh.skins.at(0).weights)
      for (const auto &w : weights) if (w.value>0) ++counts.at(w.bone);
    const int bone = static_cast<int>(std::max_element(counts.begin(),counts.end())-counts.begin());
    session->selectBone(bone);
    clickControl("modelModeTab2");
    session->seek(seconds);
    QTRY_VERIFY_WITH_TIMEOUT(session->frameFor(false)->heat && session->mode()==2,10000);
    QTest::qWait(100);
    const auto heat=session->frameFor(false);
    QCOMPARE(heat->vertices.size(),mesh.triangles.size());
    bool positive = false;
    for (std::size_t i=0;i<mesh.triangles.size();++i) {
      double weight=0;
      const auto &sourceWeights = mesh.skins.at(0).weights.at(mesh.corners.at(mesh.triangles[i]));
      for (const auto &w : sourceWeights)
        if (w.bone==bone) weight+=w.value;
      const auto &a=heat->vertices[i], &b=pose->vertices.at(offset+i);
      if (sourceWeights.empty()) QVERIFY(std::isnan(a.weight));
      else QVERIFY(std::abs(a.weight-weight)<1e-6);
      positive |= weight>0;
      QCOMPARE(a.x,b.x); QCOMPARE(a.y,b.y); QCOMPARE(a.z,b.z);
    }
    QVERIFY(positive);
    save(actor ? "psk-animation-weights" : "fbx-animation-weights");
    clickControl("modelModeTab0");
    QTRY_VERIFY_WITH_TIMEOUT(!session->frameFor(false)->heat,10000);
    auto *view = qobject_cast<ModelView *>(item("modelViewport"));
    QVERIFY(view);
    view->orbit(0,0); // Fix the camera for the renderer comparison.
    view->setGridVisible(false); view->setAxesVisible(false);
    const auto pixels = [&]() {
      QTest::qWait(100);
      const auto image=captureWindow(window_);
      const auto pos=view->mapToScene(QPointF());
      const double scale=static_cast<double>(image.width())/window_->width();
      return image.copy(qRound(pos.x()*scale),qRound(pos.y()*scale),
                        qRound(view->width()*scale),qRound(view->height()*scale));
    };
    const auto solid=pixels();
    clickControl("modelWireframe");
    QTRY_VERIFY(view->wireframeVisible());
    const auto wire=pixels();
    QVERIFY(!wire.isNull() && wire!=solid);
    save(actor ? "psk-animation-wireframe" : "fbx-animation-wireframe");
    clickControl("modelWireframe");
    QTRY_VERIFY(!view->wireframeVisible());
    QCOMPARE(pixels(),solid);
    // A burst of view/pose changes while a worker is active lands on the
    // latest requested view, even paused.
    session->setMode(1); session->setMode(2); session->setMode(0);
    session->seek(seconds);
    QTRY_VERIFY_WITH_TIMEOUT(session->frameFor(false) && !session->frameFor(false)->heat && !session->frameFor(true),10000);
    QVERIFY(session->frameError().isEmpty());
    session->detachExternalAnimation();
    QTRY_VERIFY_WITH_TIMEOUT(!session->busy() && session->inspectable() && session->animated()==originalAnimated,30000);
    QCOMPARE(session->clips(), originalClips);
    // The same wireframe switch also works on the restored static model.
    view = qobject_cast<ModelView *>(item("modelViewport"));
    QVERIFY(view);
    const auto staticSolid=pixels();
    clickControl("modelWireframe");
    QTRY_VERIFY(view->wireframeVisible());
    QVERIFY(pixels()!=staticSolid);
    clickControl("modelWireframe");
    QTRY_VERIFY(!view->wireframeVisible());
    QCOMPARE(pixels(),staticSolid);
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void renderAnimatedTextureResidency() {
    struct Metrics { std::atomic<std::uint64_t> uploaded[6]{}; std::atomic<bool> restored{}; std::atomic<std::uint64_t> restoredUpload{}; };
    const auto metrics = std::make_shared<Metrics>();
    QQuickWindow window;
    window.resize(320,240);
    auto renderer = std::make_unique<formaview::render::Item>(window.contentItem());
    renderer->setSize(QSizeF(320,240));
    renderer->observe = [metrics](formaview::render::Observation o) {
      if (!o.released && o.frame_revision >= 100001 && o.frame_revision <= 100006 && o.uploaded_bytes)
        metrics->uploaded[o.frame_revision-100001].store(o.uploaded_bytes);
      if (o.resident_hit && o.frame_revision==100005) {
        metrics->restoredUpload.store(o.uploaded_bytes); metrics->restored.store(true);
      }
    };
    auto pixels=std::make_shared<formaview::render::Texture>();
    pixels->width=pixels->height=4; pixels->rgba.assign(4*4*4,1.F);
    auto frame=std::make_shared<formaview::render::Frame>();
    frame->vertices={{-.5F,-.5F,0},{.5F,-.5F,0},{0,.5F,0}};
    frame->rgba={1,1,1,1}; frame->textures={pixels}; frame->revision=100001;
    frame->draws={{0,3,0,false,{}}};
    renderer->setFrame(frame); window.show();
    QTRY_VERIFY_WITH_TIMEOUT(metrics->uploaded[0].load()>0,10000);
    const auto image=captureWindow(&window); QVERIFY(!image.isNull());
    auto next=std::make_shared<formaview::render::Frame>(*frame); next->revision=100002;
    renderer->setFrame(next);
    QTRY_VERIFY_WITH_TIMEOUT(metrics->uploaded[1].load()>0,10000);
    QCOMPARE(captureWindow(&window),image);
    const auto textureBytes=pixels->rgba.size()*sizeof(float);
    QCOMPARE(metrics->uploaded[0].load()-metrics->uploaded[1].load(),textureBytes);
    next=std::make_shared<formaview::render::Frame>(*next); next->revision=100003;
    next->textures={std::make_shared<formaview::render::Texture>(*pixels)};
    renderer->setFrame(next);
    QTRY_VERIFY_WITH_TIMEOUT(metrics->uploaded[2].load()>0,10000);
    QCOMPARE(metrics->uploaded[2].load(),metrics->uploaded[0].load());
    next=std::make_shared<formaview::render::Frame>(*next); next->revision=100004;
    next->textures.clear(); next->draws[0].texture=-1;
    renderer->setFrame(next);
    QTRY_VERIFY_WITH_TIMEOUT(metrics->uploaded[3].load()>0,10000);
    QCOMPARE(metrics->uploaded[3].load(),metrics->uploaded[1].load());
    auto cached=std::make_shared<formaview::render::Frame>(*frame);
    cached->revision=100005; cached->cacheable=true;
    renderer->setFrame(cached);
    QTRY_VERIFY_WITH_TIMEOUT(metrics->uploaded[4].load()>0,10000);
    next=std::make_shared<formaview::render::Frame>(*cached);
    next->revision=100006; next->cacheable=false;
    renderer->setFrame(next);
    QTRY_VERIFY_WITH_TIMEOUT(metrics->uploaded[5].load()>0,10000);
    renderer->setFrame(cached);
    QTRY_VERIFY_WITH_TIMEOUT(metrics->restored.load(),10000);
    QCOMPARE(metrics->restoredUpload.load(),std::uint64_t{0});
    renderer.reset(); window.hide(); window.releaseResources();
  }
  void modelReviewContracts() {
    try {
    const QDir samples(requireFixture("FORMAVIEW_REVIEW_SAMPLES"));
    QVERIFY2(!requireFixture("FORMAVIEW_REVIEW_SAMPLES").isEmpty(), "Set FORMAVIEW_REVIEW_SAMPLES");
    for (const auto &name : {QString("negative-scale.gltf"), QString("zero-scale.gltf")}) {
      const auto document = readModelDocument(samples.filePath(name), {});
      QVERIFY(document->animated());
      const auto local = document->localAt(0, .5).at(0);
      QCOMPARE(local[0], name.startsWith("negative") ? -2.0 : 0.0);
      QCOMPARE(local[5], 3.0);
      QCOMPARE(local[10], 4.0);
      QCOMPARE(local[3], .5);
    }
    {
      const auto document = readModelDocument(samples.filePath("multi-morph.gltf"), {});
      QVERIFY(document->animated());
      // Weights channel supported since 2026-10-23: it starts at the static
      // defaults (0.25,0.5) and animates to zero, so at t=0.5 the weights are
      // (0.125,0.25) and the expected point is half the old static default.
      QVERIFY2(!document->warnings.join(';').contains("Unsupported animation path"),
               qPrintable(document->warnings.join(';')));
      const auto frame = buildAnimationFrame(*document, 0, .5, 1);
      const auto point = QVector3D(frame->vertices[0].x,frame->vertices[0].y,frame->vertices[0].z) * document->radius + document->center;
      QVERIFY((point - QVector3D(.5,0,.375)).length() < 1e-5);
    }
    {
      // A nonidentity display basis must be applied exactly once, even when
      // the mesh has no skin weights. The expected source position is known.
      auto document = readModelDocument(samples.filePath("negative-scale.gltf"), {});
      document->displayBasis.rotate(-90,1,0,0);
      for (auto &mesh : document->meshes)
        for (auto &point : mesh.points) point = document->displayBasis.map(point);
      document->center = {}; document->radius = 1;
      const auto frame = buildAnimationFrame(*document, 0, .5, 1);
      const auto &v = frame->vertices[1];
      QVERIFY((QVector3D(v.x,v.y,v.z) - QVector3D(-1.5,0,0)).length() < 1e-5);
      const auto &top = frame->vertices[2];
      QVERIFY((QVector3D(top.x,top.y,top.z) - QVector3D(.5,0,-3)).length() < 1e-5);
    }
    {
      formaview::Matrix4 identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
      std::vector<formaview::Matrix4> locals(20000, identity);
      std::vector<std::int32_t> parents(20000);
      for (int i=0;i<19999;++i) parents[i]=i+1;
      parents.back()=-1;
      QCOMPARE(chainWorlds(locals,parents).front(),identity);
      parents.back()=0;
      QVERIFY_THROWS_EXCEPTION(formaview::skeletal::Failure, (void)chainWorlds(locals,parents));
    }
    {
      const auto path=samples.filePath("invalid-weights.gltf");
      const auto document=readModelDocument(path,{});
      QVERIFY(document->animated());
      QVERIFY(document->warnings.join(';').contains("invalid source skin weights"));
      QVERIFY(!document->meshes.front().skinDeformationValid);
      QVERIFY(document->meshes.front().skins.front().weights[0][0].value<0);
      QVERIFY_THROWS_EXCEPTION(formaview::skeletal::Failure, (void)buildAnimationFrame(*document,0,.5,1));
      ModelSession invalid;
      invalid.open(path);
      QTRY_VERIFY_WITH_TIMEOUT(!invalid.busy() && invalid.animated(),10000);
      invalid.togglePlay();
      QTRY_VERIFY_WITH_TIMEOUT(!invalid.playing() && invalid.frameError().contains("Invalid source skin weights"),10000);
      const auto error=invalid.frameError();
      QTest::qWait(100); QCOMPARE(invalid.frameError(),error);
    }
    ModelSession session;
    {
      ModelDocument data;
      data.meshes.resize(1);
      data.meshes[0].skins.resize(1);
      auto &skin = data.meshes[0].skins[0];
      skin.weights = {{{0, 1.0}}};
      skin.palette = {QVariantMap{{"bind", 1.0}}};
      const auto original = modelSourceDigest(data);
      skin.weights[0][0].value = .75;
      QVERIFY(modelSourceDigest(data) != original);
      skin.weights[0][0].value = 1.0;
      QCOMPARE(modelSourceDigest(data), original);
      skin.palette = {QVariantMap{{"bind", 2.0}}};
      QVERIFY(modelSourceDigest(data) != original);
    }
    for (const auto &source : {requireFixture("FORMAVIEW_ANIMATION_TARGET"),
                               requireFixture("FORMAVIEW_ANIMATION_EXTERNAL")}) {
      QVERIFY(QFileInfo::exists(source));
      session.open(source);
      QTRY_VERIFY_WITH_TIMEOUT(!session.busy() && session.inspectable(), 30000);
      const auto notifiedIdle=std::make_shared<bool>(false);
      const auto comparisonSignal=QObject::connect(&session,&ModelSession::changed,&session,[&session,notifiedIdle] {
        if (!session.compareBusy() && !session.compareResult().isEmpty()) *notifiedIdle=true;
      });
      session.compareSkeletonWith(source);
      QTRY_VERIFY_WITH_TIMEOUT(!session.compareBusy(), 30000);
      QVERIFY2(session.compareError().isEmpty(), qPrintable(session.compareError()));
      QVERIFY(*notifiedIdle);
      QObject::disconnect(comparisonSignal);
      const auto result = session.compareResult();
      QCOMPARE(result.value("structureVerdict").toString(), QString("structuralDirect"));
      QCOMPARE(result.value("applicabilityVerdict").toString(), QString("directApplicable"));
      QCOMPARE(result.value("missingInExternal").toInt(), 0);
      QCOMPARE(result.value("externalOnly").toInt(), 0);
      for (const auto &row : result.value("rows").toList())
        QCOMPARE(row.toMap().value("status").toString(), QString("matched"));
      session.clear();
      QVERIFY(session.compareResult().isEmpty() && session.compareError().isEmpty());
    }
    const QDir edge(requireFixture("FORMAVIEW_EDGE_SAMPLES_DIR"));
    session.open(edge.filePath("multi-skin.gltf"));
    QTRY_VERIFY_WITH_TIMEOUT(!session.busy() && session.frame(), 30000);
    session.selectMesh(0); session.classifyMesh("review", 0, false);
    session.selectMesh(1); session.classifyMesh("review", 1, false);
    session.setMode(0); session.requestLod(0);
    const auto source = readModelDocument(edge.filePath("multi-skin.gltf"),{});
    QTRY_COMPARE_WITH_TIMEOUT(session.frame()->vertices.size(), source->meshes[0].triangles.size(), 10000);
    // Clear with a pose job in flight. No synchronous future wait and no
    // obsolete frame/result may land into the next file.
    session.seek(.8);
    QElapsedTimer clearClock; clearClock.start(); session.clear();
    qInfo() << "review clear ms" << clearClock.nsecsElapsed()/1e6;
    session.open(samples.filePath("negative-scale.gltf"));
    QTRY_VERIFY_WITH_TIMEOUT(!session.busy() && session.frame(), 10000);
    QCOMPARE(session.path(), samples.filePath("negative-scale.gltf"));
    QTest::qWait(150);
    QVERIFY(session.frameError().isEmpty());
    QCOMPARE(session.frame()->vertices.size(), std::size_t(3));
    const auto revision=session.frame()->revision;
    session.seek(.7); session.setMaterialOnly(true);
    QTRY_VERIFY_WITH_TIMEOUT(session.frame()->revision!=revision,10000);
    const auto frame=session.frame();
    QVector3D centroid;
    for (const auto &vertex : frame->vertices) centroid+=QVector3D(vertex.x,vertex.y,vertex.z)/3;
    session.pick(QMatrix4x4{},QPointF(centroid.x(),centroid.y()));
    QVERIFY2(!session.picked().isEmpty(),"Non-UV material edit must not leave the presented-state guard stale");
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelSkeletonCompareFlow() {
    try {
    // M07 skeleton comparison product flow: background compare of the
    // current document against a chosen FBX with separate structure
    // verdict, merged difference rows and a differences-only filter.
    const auto current =
        QFileInfo(requireFixture("FORMAVIEW_ANIMATION_TARGET"));
    const auto other =
        QFileInfo(requireFixture("FORMAVIEW_ANIMATION_EXTERNAL"));
    if (!current.exists() || !other.exists()) {
      QSKIP("Set FORMAVIEW_ANIMATION_TARGET and FORMAVIEW_ANIMATION_EXTERNAL");
      return;
    }
    auto *session = qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    const auto restore = qScopeGuard([&] { desktop_->clearAsset(); });
    desktop_->openPath(current.absoluteFilePath());
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable() ||
                                 !session->error().isEmpty(),
                             30000);
    QVERIFY2(session->inspectable(), qPrintable(session->error()));
    QVERIFY(session->hasComparableSkeleton());
    session->compareSkeletonWith(other.absoluteFilePath());
    QTRY_VERIFY_WITH_TIMEOUT(!session->compareBusy() &&
                                 (session->compareResult().size() > 1 ||
                                  !session->compareError().isEmpty()),
                             30000);
    QVERIFY2(session->compareError().isEmpty(),
             qPrintable(session->compareError()));
    const auto result = session->compareResult();
    QVERIFY(result.contains("structureVerdict"));
    QVERIFY(result.contains("applicabilityVerdict"));
    QVERIFY(result.contains("matched"));
    QVERIFY(result.value("bonesA").toInt() > 0);
    QVERIFY(result.value("bonesB").toInt() > 0);
    // Structure and applicability are separate verdicts: the applicability
    // label must follow the structure outcome plus reference sufficiency.
    const auto structure = result.value("structureVerdict").toString();
    const auto applicability =
        result.value("applicabilityVerdict").toString();
    if (structure == "structuralDirect")
      QVERIFY(applicability == "directApplicable" ||
              applicability == "structureOnly");
    else if (structure == "partial")
      QCOMPARE(applicability, QStringLiteral("trialOnly"));
    else if (structure == "ambiguous")
      QCOMPARE(applicability, QStringLiteral("ambiguousMapping"));
    else
      QCOMPARE(applicability, QStringLiteral("notApplicable"));
    const auto rows = result.value("rows").toList();
    QVERIFY(rows.size() > 0);
    QVERIFY(std::any_of(rows.cbegin(), rows.cend(), [](const QVariant &row) {
      return row.toMap().value("status").toString() == "matched";
    }));
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelR13RealGltfEvidence() {
    try {
    namespace tv=formaview::skeletal::transform;
    const auto path=requireFixture("FORMAVIEW_GLTF_ANIMATION_SAMPLE");QVERIFY(QFileInfo::exists(path));
    const auto document=readModelDocument(path,{});QVERIFY(document->transformEvidence);
    const auto e=document->transformEvidence({});
    QVERIFY(e.fullNodeDomain);QVERIFY(e.nativeSkinBindings.has_value());QVERIFY(!e.nativeSkinBindings->empty());
    std::vector<tv::Pair> pairs;for(std::size_t i=0;i<e.nodes.size();++i)pairs.push_back({i,i});
    const auto report=tv::validate(e,e,pairs);
    QFile proof(root_+"/r13-real-gltf.json");QVERIFY(proof.open(QIODevice::WriteOnly));
    proof.write(QJsonDocument(QJsonObject{{"path",path},{"sha256",QString::fromStdString(e.sha256)},
      {"nodes",static_cast<int>(e.nodes.size())},{"bindings",static_cast<int>(e.nativeSkinBindings->size())},
      {"verdict",static_cast<int>(report.verdict)},{"reason",static_cast<int>(report.reason)}}).toJson());proof.close();
    // Independent source JSON audit (p27-quaternion-audit.json) found 226
    // rotations outside the exact float rounding cell. Preserve rejection;
    // do not expand the unit-quaternion budget to make a real asset pass.
    int outside=0;double worstNorm=0;
    for(const auto &node:e.nodes)if(node.restLocal) {
      double norm=0;for(double q:node.restLocal->rotation)norm+=q*q;
      worstNorm=std::max(worstNorm,std::abs(norm-1));if(std::abs(norm-1)>1e-10)++outside;
    }
    QCOMPARE(outside,226);QVERIFY(worstNorm>1e-6&&worstNorm<1.3e-6);
    QCOMPARE(report.verdict,tv::Verdict::Insufficient);QCOMPARE(report.reason,tv::Reason::InvalidInput);
    QCOMPARE(report.sourceNode,std::optional<std::size_t>{4});
    QVERIFY(!report.targetNode.has_value());
    qInfo()<<"R13 real glTF conservative rejection"<<outside<<"rotations; worst norm squared error"<<worstNorm;
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelR13MalformedDecoderEvidence() {
    try {
    auto source=readModelDocument(requireFixture("FORMAVIEW_R13_SAMPLES")+"/reach-a.gltf",{});
    QVERIFY(source->decoderSnapshot);
    for(int fault=0;fault<4;++fault){
      auto bad=std::make_shared<ModelDecoderSnapshot>(*source->decoderSnapshot);
      auto data=std::make_shared<formaview::gltf::GltfAnimationData>(*bad->animations);
      auto &c=data->clips[0].channels[0];QVERIFY(c.times.size()>1);QVERIFY(!c.values.empty());
      if(fault==0)c.times[1]=c.times[0];
      if(fault==1)c.values[0]=std::numeric_limits<double>::quiet_NaN();
      if(fault==2)c.values.pop_back();
      if(fault==3){c.times.clear();c.values.clear();}
      bad->animations=data;
      QCOMPARE(compareFixedDecoder(bad,bad).value("verdict").toString(),QString("insufficient"));
    }
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelGpuTickBacklog() {
    try {
    // The legacy desktop suite deliberately selects CPU for its CPU oracles.
    // Scope this new GPU-specific contract without changing other cases.
    const bool hadSkinning=qEnvironmentVariableIsSet("FORMAVIEW_SKINNING");
    const auto oldSkinning=qgetenv("FORMAVIEW_SKINNING");
    const auto restoreSkinning=qScopeGuard([&]{if(hadSkinning)qputenv("FORMAVIEW_SKINNING",oldSkinning);else qunsetenv("FORMAVIEW_SKINNING");});
    qunsetenv("FORMAVIEW_SKINNING");
    // Real Anke, deterministic delivery boundary: render notifications can
    // accumulate during a native modal resize, without a new model pose.
    const auto target=requireFixture("FORMAVIEW_PSK_TARGET"),animation=requireFixture("FORMAVIEW_PSK_PSA");
    QVERIFY(QFileInfo::exists(target));QVERIFY(QFileInfo::exists(animation));
    ModelSession session;QQuickWindow first,second;
    const auto cleanup=qScopeGuard([&]{session.attachRenderWindow(nullptr);session.clear();first.hide();second.hide();first.releaseResources();second.releaseResources();});
    session.enableGpuSkinning(true);session.open(target);
    QTRY_VERIFY_WITH_TIMEOUT(session.inspectable(),30000);
    session.attachExternalAnimation(animation);
    QTRY_VERIFY_WITH_TIMEOUT(!session.externalBusy()&&session.animated()&&session.frame()&&session.frame()->geometry,30000);
    session.attachRenderWindow(&first);session.setLoop(true);session.togglePlay();QVERIFY(session.playing());
    QSignalSpy changes(&session,&ModelSession::frameChanged);
    const auto burst=[&](QQuickWindow &window,int count){for(int i=0;i<count;++i)QMetaObject::invokeMethod(&window,"frameSwapped",Qt::DirectConnection);};
    burst(first,128);QThread::msleep(20);
    QCoreApplication::sendPostedEvents(&session,QEvent::MetaCall);
    const int burstPoses=changes.count();evidence_["gpuTickBacklogPoses"]=burstPoses;
    QCOMPARE(burstPoses,1); // Previously 128 unpresented pose evaluations.
    changes.clear();burst(first,32);session.attachRenderWindow(&second);
    QCoreApplication::sendPostedEvents(&session,QEvent::MetaCall);
    QCOMPARE(changes.count(),0); // Already queued callbacks from old window expire.
    burst(second,32);QCoreApplication::sendPostedEvents(&session,QEvent::MetaCall);
    QCOMPARE(changes.count(),1);QVERIFY(session.playing());
    session.togglePlay();changes.clear();burst(second,32);QCoreApplication::sendPostedEvents(&session,QEvent::MetaCall);
    QCOMPARE(changes.count(),0); // Paused playback must remain paused.
    session.togglePlay();changes.clear();burst(second,16);QCoreApplication::sendPostedEvents(&session,QEvent::MetaCall);
    QCOMPARE(changes.count(),1);QVERIFY(session.frameError().isEmpty());
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelMorphStepEndpoint() {
    try {
    const auto directory=requireFixture("FORMAVIEW_R13_STEP");QVERIFY(!directory.isEmpty());
    const auto document=readModelDocument(directory+"/step.gltf",{});
    QFile expected(directory+"/expected.json");QVERIFY(expected.open(QIODevice::ReadOnly));
    const auto last=QJsonDocument::fromJson(expected.readAll()).object().value("last").toArray();
    const auto end=document->animationClips.at(0).end;
    for(double t:{end,end+1.0}){
      const auto weights=document->morphWeightsAt(0,t);QCOMPARE(weights.size(),std::size_t(1));QCOMPARE(weights[0].size(),std::size_t(2));
      for(int i=0;i<last.size();++i)QCOMPARE(weights[0][i],last[i].toDouble());
      const auto worlds=document->worldAt(0,end);
      const auto frame=buildAnimationFrame(*document,0,t,100,false,&worlds);
      const auto &mesh=document->meshes[0];
      for(std::size_t corner=0;corner<mesh.triangles.size();++corner){
        const auto point=mesh.corners[mesh.triangles[corner]];const auto &base=mesh.raw[point];QVector3D p(float(base.x),float(base.y),float(base.z));
        for(int k=0;k<last.size();++k){const auto &delta=mesh.morphTargets[k].positionDeltas[point];p+=QVector3D(float(delta.x),float(delta.y),float(delta.z))*float(last[k].toDouble());}
        p=(document->displayBasis.map(p)-document->center)/std::max(document->radius,1e-6f);
        const auto &actual=frame->vertices[corner];QVERIFY((p-QVector3D(actual.x,actual.y,actual.z)).length()<=1e-4f);
      }
    }
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelR13MorphEvidence() {
    try {
    const auto dir=requireFixture("FORMAVIEW_R13_MORPH");QVERIFY(!dir.isEmpty());
    const auto original=readModelDocument(dir+"/morph-anim.gltf",{});
    QVERIFY(original->decoderSnapshot);
    QCOMPARE(compareFixedDecoder(original->decoderSnapshot,original->decoderSnapshot).value("verdict").toString(),QString("equivalent"));
    QFile input(dir+"/morph-anim.gltf");QVERIFY(input.open(QIODevice::ReadOnly));
    const auto envelope=QJsonDocument::fromJson(input.readAll()).object();
    const auto folder=root_+"/r13-morph";QVERIFY(QDir().mkpath(folder));
    const auto buffers=envelope.value("buffers").toArray();
    for(const auto &b:buffers){const auto uri=b.toObject().value("uri").toString();
      if(!uri.startsWith("data:")){QVERIFY(!uri.contains(".."));QVERIFY(QFile::copy(dir+"/"+uri,folder+"/"+uri));}}
    const auto load=[&](QJsonObject value,const QString &name){
      const auto path=folder+"/"+name+".gltf";QFile file(path);
      if(!file.open(QIODevice::WriteOnly))return std::shared_ptr<ModelDocument>{};
      file.write(QJsonDocument(value).toJson());file.close();return readModelDocument(path,{});
    };
    auto changed=envelope;auto meshes=changed.value("meshes").toArray();auto mesh=meshes[0].toObject();
    mesh.insert("weights",QJsonArray{0.5,0.0});meshes[0]=mesh;changed.insert("meshes",meshes);
    auto defaults=load(changed,"default");QVERIFY(defaults&&defaults->decoderSnapshot);
    QCOMPARE(compareFixedDecoder(original->decoderSnapshot,defaults->decoderSnapshot).value("reason").toString(),QString("mappedMorphConflict"));
    auto shading=envelope;shading.insert("extensionsUsed",QJsonArray{"KHR_materials_unlit"});
    auto allowed=load(shading,"shading");QVERIFY(allowed&&allowed->decoderSnapshot);
    QCOMPARE(compareFixedDecoder(original->decoderSnapshot,allowed->decoderSnapshot).value("verdict").toString(),QString("equivalent"));
    auto unknown=envelope;unknown.insert("extensionsUsed",QJsonArray{"EXT_unknown_animation"});
    auto rejected=load(unknown,"unknown");QVERIFY(rejected);QVERIFY(!rejected->decoderSnapshot);
    std::stop_source cancelled;cancelled.request_stop();
    QCOMPARE(compareFixedDecoder(original->decoderSnapshot,allowed->decoderSnapshot,cancelled.get_token()).value("verdict").toString(),QString("cancelled"));
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelR13ClipCorrespondence() {
    try {
    const auto source=readModelDocument(requireFixture("FORMAVIEW_R13_SAMPLES")+"/reach-a.gltf",{});
    QVERIFY(source->decoderSnapshot);
    auto base=std::make_shared<ModelDecoderSnapshot>(*source->decoderSnapshot);
    auto data=std::make_shared<formaview::gltf::GltfAnimationData>(*base->animations);
    QVERIFY(!data->clips.empty());auto clip=data->clips[0];
    // Exact dyadic times keep this a strict timestamp-translation control.
    for(auto &ch:clip.channels){QVERIFY(ch.times.size()>1);for(std::size_t k=0;k<ch.times.size();++k)ch.times[k]=static_cast<double>(k);}
    clip.start=0;clip.end=static_cast<double>(clip.channels[0].times.size()-1);clip.name="A";
    data->clips={clip,clip};data->clips[1].name="B";base->animations=data;
    auto target=std::make_shared<ModelDecoderSnapshot>(*base);
    auto shifted=std::make_shared<formaview::gltf::GltfAnimationData>(*data);
    std::reverse(shifted->clips.begin(),shifted->clips.end());
    for(auto &c:shifted->clips){c.start+=8;c.end+=8;for(auto &ch:c.channels)for(auto &t:ch.times)t+=8;}
    target->animations=shifted;
    auto proof=compareFixedDecoder(base,target);
    QCOMPARE(proof.value("verdict").toString(),QString("insufficient"));
    QCOMPARE(proof.value("curveCorrespondence").toMap().value("verdict").toString(),QString("exactKeysUnderTimeTranslation"));
    QVERIFY(!proof.value("curveCorrespondence").toMap().value("floatingEvaluationEquivalent").toBool());
    QCOMPARE(proof.value("curveCorrespondence").toMap().value("pairs").toList().size(),qsizetype(2));
    shifted->clips[0].channels[0].values[0]+=.125;
    QCOMPARE(compareFixedDecoder(base,target).value("curveCorrespondence").toMap().value("verdict").toString(),QString("insufficient"));
    shifted->clips[0].channels[0].values[0]-=.125;
    shifted->clips[0].channels[0].times[1]+=.25;
    QCOMPARE(compareFixedDecoder(base,target).value("curveCorrespondence").toMap().value("verdict").toString(),QString("insufficient"));
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }

  void modelR13FixedDecoderCongruence() {
    try {
    // A derived metadata-only envelope is a decoder control, not a second
    // independent asset or a format compatibility proof. Preserve the strict
    // rejection checked by modelR13RealGltfEvidence.
    const auto path=requireFixture("FORMAVIEW_GLTF_ANIMATION_SAMPLE");
    QFile input(path);QVERIFY(input.open(QIODevice::ReadOnly));
    auto rawEnvelope=input.readAll();auto envelope=QJsonDocument::fromJson(rawEnvelope).object();input.close();
    const auto originalGenerator=envelope.value("asset").toObject().value("generator").toString().toUtf8();
    QVERIFY(!originalGenerator.isEmpty());QCOMPARE(rawEnvelope.count(originalGenerator),1);
    rawEnvelope.replace(originalGenerator,"p33 exact textual metadata control");
    auto asset=envelope.value("asset").toObject();
    asset.insert("generator","p32 metadata-only congruence control");envelope.insert("asset",asset);
    const auto folder=root_+"/fixed-decoder-control";QVERIFY(QDir().mkpath(folder));
    const auto buffers=envelope.value("buffers").toArray();QCOMPARE(buffers.size(),1);
    const auto uri=buffers[0].toObject().value("uri").toString();
    QVERIFY(!uri.contains("/")&&!uri.contains("\\")&&!uri.contains(":"));
    QVERIFY(QFile::copy(QFileInfo(path).absolutePath()+"/"+uri,folder+"/"+uri));
    const auto variant=folder+"/variant.gltf";QFile control(variant);QVERIFY(control.open(QIODevice::WriteOnly));
    control.write(rawEnvelope);control.close();
    using Worlds=std::vector<formaview::Matrix4>;
    struct Probe{std::size_t clip;double seconds;Worlds world;};std::vector<Probe> probes;
    auto source=readModelDocument(path,{});QCOMPARE(source->animationClips.size(),std::size_t(120));
    const auto originalEvidence=source->transformEvidence({});const auto staticLocals=source->animNodeLocals;
    for(std::size_t i=0;i<source->animationClips.size();++i){const auto &c=source->animationClips[i];
      for(double f:{0.,.371,1.}){const double t=c.start+(c.end-c.start)*f;probes.push_back({i,t,source->worldAt(i,t)});}}
    const auto decoder=source->decoderSnapshot;
    source.reset();auto target=readModelDocument(variant,{});
    QCOMPARE(compareFixedDecoder(decoder,target->decoderSnapshot).value("verdict").toString(),QString("equivalent"));
    std::stop_source cancelled;cancelled.request_stop();
    QCOMPARE(compareFixedDecoder(decoder,target->decoderSnapshot,cancelled.get_token()).value("verdict").toString(),QString("cancelled"));
    auto altered=std::make_shared<ModelDecoderSnapshot>(*target->decoderSnapshot);altered->metadata.append("different");
    QCOMPARE(compareFixedDecoder(decoder,altered).value("verdict").toString(),QString("insufficient"));
    const auto otherEvidence=target->transformEvidence({});QVERIFY(originalEvidence.sha256!=otherEvidence.sha256);
    QVERIFY(staticLocals==target->animNodeLocals);
    std::size_t matrices=0;for(const auto &p:probes){const auto actual=target->worldAt(p.clip,p.seconds);QVERIFY(p.world==actual);matrices+=actual.size();}
    QCOMPARE(probes.size(),std::size_t(360));QCOMPARE(matrices,std::size_t(90000));
    std::vector<formaview::skeletal::transform::Pair> mapping;for(std::size_t i=0;i<otherEvidence.nodes.size();++i)mapping.push_back({i,i});
    const auto strict=formaview::skeletal::transform::validate(originalEvidence,otherEvidence,mapping);
    QCOMPARE(strict.verdict,formaview::skeletal::transform::Verdict::Insufficient);
    QCOMPARE(strict.reason,formaview::skeletal::transform::Reason::InvalidInput);
    qInfo()<<"P32 fixed decoder 120 clips / 360 poses /"<<matrices<<"exact matrices; format rejection unchanged";
    target.reset();
    const auto reordered=readModelDocument(requireFixture("FORMAVIEW_R13_REAL_REORDER"),{});
    const auto reorderedProof=compareFixedDecoder(decoder,reordered->decoderSnapshot);
    QCOMPARE(reorderedProof.value("verdict").toString(),QString("equivalent"));
    QCOMPARE(reorderedProof.value("mapping").toString(),QString("fullNameBijection"));
    QCOMPARE(reorderedProof.value("mappedPairs").toList().size(),qsizetype(250));
    for(const auto &probe:probes){const auto actual=reordered->worldAt(probe.clip,probe.seconds);
      QCOMPARE(actual.size(),probe.world.size());
      for(std::size_t i=0;i<actual.size();++i)QVERIFY(actual[actual.size()-1-i]==probe.world[i]);}
    qInfo()<<"P34 real reordered 120 clips / 360 poses / 90000 exact mapped matrices";

    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelR13FbxBindingEvidence() {
    try {
    namespace tv=formaview::skeletal::transform;
    const auto folder=requireFixture("FORMAVIEW_R13_FBX");QVERIFY(!folder.isEmpty());
    for(int mode=0;mode<4;++mode) {
      const auto base=folder+"/inherit-"+QString::number(mode);
      const auto a=formaview::skeletal::FbxDocument::read(std::filesystem::path((base+"-a.fbx").toStdWString()));
      const auto b=formaview::skeletal::FbxDocument::read(std::filesystem::path((base+"-b.fbx").toStdWString()));
      const auto ea=a.transform_evidence(),eb=b.transform_evidence();
      QVERIFY(ea.sha256!=eb.sha256);QVERIFY(ea.bindings.has_value());QCOMPARE(ea.bindings->size(),std::size_t(4));
      std::vector<tv::Pair> pairs;for(std::size_t i=0;i<ea.nodes.size();++i)pairs.push_back({i,i});
      const auto r=tv::validate(ea,eb,pairs);
      if(mode==1) {QCOMPARE(r.verdict,tv::Verdict::Incompatible);QCOMPARE(r.reason,tv::Reason::BindingConflict);}
      else {QCOMPARE(r.verdict,tv::Verdict::Compatible);QCOMPARE(r.reason,tv::Reason::None);}
    }
    auto *session=qobject_cast<ModelSession*>(desktop_->model());QVERIFY(session);
    const auto cleanup=qScopeGuard([&]{desktop_->clearAsset();});
    desktop_->openPath(folder+"/inherit-0-a.fbx");
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable()||!session->error().isEmpty(),30000);
    QVERIFY2(session->inspectable(),qPrintable(session->error()));
    session->compareSkeletonWith(folder+"/inherit-0-b.fbx");
    QTRY_VERIFY_WITH_TIMEOUT(!session->compareBusy()&&(!session->compareResult().isEmpty()||!session->compareError().isEmpty()),30000);
    QVERIFY2(session->compareError().isEmpty(),qPrintable(session->compareError()));
    const auto report=session->compareResult();
    QCOMPARE(report.value("applicabilityVerdict").toString(),QString("compatibleNotDrivable"));
    QCOMPARE(report.value("transformValidation").toMap().value("verdict").toString(),QString("compatible"));
    QCOMPARE(report.value("transformValidation").toMap().value("mapped").toInt(),4);
    QFile proof(root_+"/r13-fbx-proof.json");QVERIFY(proof.open(QIODevice::WriteOnly));proof.write(QJsonDocument::fromVariant(report).toJson());
    desktop_->clearAsset();
    desktop_->openPath(folder+"/inherit-3-a.fbx");
    QTRY_VERIFY_WITH_TIMEOUT(!session->busy() && session->path()==folder+"/inherit-3-a.fbx" &&
      (session->inspectable()||!session->error().isEmpty()),30000);
    QVERIFY2(session->inspectable(),qPrintable(session->error()));
    session->compareSkeletonWith(folder+"/inherit-3-b.fbx");
    QTRY_VERIFY_WITH_TIMEOUT(!session->compareBusy()&&(!session->compareResult().isEmpty()||!session->compareError().isEmpty()),30000);
    QVERIFY2(session->compareError().isEmpty(),qPrintable(session->compareError()));
    QCOMPARE(session->compareResult().value("applicabilityVerdict").toString(),QString("directApplicable"));
    QVERIFY(session->compareResult().value("transformValidation").toMap().value("promotionEnabled").toBool());
    QFile normal(root_+"/r13-fbx-normal-proof.json");QVERIFY(normal.open(QIODevice::WriteOnly));normal.write(QJsonDocument::fromVariant(session->compareResult()).toJson());
    auto *pane=item("modelPane");QVERIFY(pane);pane->setProperty("showCompare",true);
    QTest::qWait(100);QVERIFY(captureWindow(window_).save(root_+"/r13-fbx-normal.png"));
    pane->setProperty("showCompare",false);
    // p35: the conflicting pair publishes a located binding fault; verdict/reason stay as before.
    desktop_->clearAsset();
    desktop_->openPath(folder+"/inherit-1-a.fbx");
    QTRY_VERIFY_WITH_TIMEOUT(!session->busy() && session->path()==folder+"/inherit-1-a.fbx" &&
      (session->inspectable()||!session->error().isEmpty()),30000);
    QVERIFY2(session->inspectable(),qPrintable(session->error()));
    session->compareSkeletonWith(folder+"/inherit-1-b.fbx");
    QTRY_VERIFY_WITH_TIMEOUT(!session->compareBusy()&&(!session->compareResult().isEmpty()||!session->compareError().isEmpty()),30000);
    QVERIFY2(session->compareError().isEmpty(),qPrintable(session->compareError()));
    const auto conflictReport=session->compareResult();
    const auto conflict=conflictReport.value("transformValidation").toMap();
    QCOMPARE(conflict.value("verdict").toString(),QString("incompatible"));
    QCOMPARE(conflict.value("reason").toString(),QString("bindingConflict"));
    QVERIFY(!conflict.value("promotionEnabled").toBool());
    QVERIFY(conflictReport.value("applicabilityVerdict").toString()!=QString("directApplicable"));
    const auto fault=conflict.value("bindingFault").toMap();
    QVERIFY2(!fault.isEmpty(),"BindingConflict must publish a located fault");
    QVERIFY(fault.value("fault").toString()!=QString("none"));
    QVERIFY(fault.value("witness").toString()=="bindPose"||fault.value("witness").toString()=="clusterLink");
    QFile faultProof(root_+"/r13-fbx-binding-fault.json");QVERIFY(faultProof.open(QIODevice::WriteOnly));
    faultProof.write(QJsonDocument::fromVariant(conflict).toJson());
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelR13DecoderReordered() {
    try {
    const auto dir=requireFixture("FORMAVIEW_R13_REORDER");QVERIFY(!dir.isEmpty());
    const auto a=readModelDocument(dir+"/base.gltf",{}),b=readModelDocument(dir+"/reordered.gltf",{});
    const auto proof=compareFixedDecoder(a->decoderSnapshot,b->decoderSnapshot);
    QCOMPARE(proof.value("verdict").toString(),QString("equivalent"));
    QCOMPARE(proof.value("mapping").toString(),QString("fullNameBijection"));
    for(double t:{0.,.25,.5,.75,1.}) {
      const auto x=a->worldAt(0,t),y=b->worldAt(0,t);QCOMPARE(x.size(),y.size());
      for(std::size_t i=0;i<x.size();++i)QVERIFY(x[i]==y[x.size()-1-i]);
    }
    for(const auto &file:{"bad-default.gltf","bad-parent.gltf","ambiguous.gltf","bad-binding.gltf"}) {
      const auto bad=readModelDocument(dir+"/"+file,{});
      QCOMPARE(compareFixedDecoder(a->decoderSnapshot,bad->decoderSnapshot).value("verdict").toString(),QString("insufficient"));
    }
    auto *session=qobject_cast<ModelSession*>(desktop_->model());QVERIFY(session);
    const auto cleanup=qScopeGuard([&]{desktop_->clearAsset();});desktop_->openPath(dir+"/base.gltf");
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable()||!session->error().isEmpty(),30000);QVERIFY(session->inspectable());
    session->compareSkeletonWith(dir+"/reordered.gltf");
    QTRY_VERIFY_WITH_TIMEOUT(!session->compareBusy()&&!session->compareResult().isEmpty(),30000);
    QCOMPARE(session->compareResult().value("transformValidation").toMap().value("fixedDecoder").toMap().value("verdict").toString(),QString("equivalent"));
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelR13DecoderCommonCoverage() {
    try {
    const auto dir=requireFixture("FORMAVIEW_R13_REORDER");QVERIFY(!dir.isEmpty());
    const auto a=readModelDocument(dir+"/base.gltf",{}),b=readModelDocument(dir+"/different.gltf",{});
    const auto proof=compareFixedDecoder(a->decoderSnapshot,b->decoderSnapshot);
    QCOMPARE(proof.value("verdict").toString(),QString("insufficient"));
    const auto common=proof.value("commonCoverage").toMap();
    QCOMPARE(common.value("verdict").toString(),QString("sameCommonOverrides"));
    QCOMPARE(common.value("componentCount").toInt(),1);
    QVERIFY(!proof.value("promotionEnabled").toBool());
    QVERIFY(a->worldAt(0,1)[1]!=b->worldAt(0,1)[3]);
    // The condition is identical supplied overrides, not different authored
    // key values. At the shared first key the actual local evaluators agree.
    const auto x=a->worldAt(0,0),y=b->worldAt(0,0);
    for(std::size_t i=0;i<x.size();++i)QVERIFY(x[i]==y[x.size()-1-i]);
    for(const auto &file:{"bad-default.gltf","bad-parent.gltf","ambiguous.gltf","bad-binding.gltf"}) {
      const auto bad=readModelDocument(dir+"/"+file,{});
      QVERIFY(compareFixedDecoder(a->decoderSnapshot,bad->decoderSnapshot).value("commonCoverage").toMap().value("verdict").toString()!=QString("sameCommonOverrides"));
    }
    auto disjoint=std::make_shared<ModelDecoderSnapshot>(*b->decoderSnapshot);
    auto data=std::make_shared<formaview::gltf::GltfAnimationData>(*disjoint->animations);
    data->clips[0].channels[0].path=formaview::gltf::TrackPath::Scale;disjoint->animations=data;
    QCOMPARE(compareFixedDecoder(a->decoderSnapshot,disjoint).value("commonCoverage").toMap().value("reason").toString(),QString("noCommonChannels"));
    std::stop_source stopped;stopped.request_stop();
    QCOMPARE(compareFixedDecoder(a->decoderSnapshot,b->decoderSnapshot,stopped.get_token()).value("verdict").toString(),QString("cancelled"));

    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelR13DecoderExplicitMapping() {
    try {
    const auto dir=requireFixture("FORMAVIEW_R13_REORDER");QVERIFY(!dir.isEmpty());
    const auto a=readModelDocument(dir+"/base.gltf",{}),b=readModelDocument(dir+"/reordered.gltf",{});
    const auto named=compareFixedDecoder(a->decoderSnapshot,b->decoderSnapshot);
    QCOMPARE(named.value("verdict").toString(),QString("equivalent"));
    std::vector<std::size_t> map(a->decoderSnapshot->nodeNames.size());
    for(const auto &pair:named.value("mappedPairs").toList()){const auto p=pair.toMap();map[p.value("source").toUInt()]=p.value("target").toUInt();}
    // Without names or an explicit contract the permutation is never guessed.
    auto unnamed=std::make_shared<ModelDecoderSnapshot>(*b->decoderSnapshot);
    for(auto &name:unnamed->nodeNames)name.clear();
    auto proof=compareFixedDecoder(a->decoderSnapshot,unnamed);
    QCOMPARE(proof.value("verdict").toString(),QString("insufficient"));
    QCOMPARE(proof.value("reason").toString(),QString("mappingConflict"));
    FixedDecoderOptions options;options.explicitMapping=map;
    proof=compareFixedDecoder(a->decoderSnapshot,unnamed,{},options);
    QCOMPARE(proof.value("verdict").toString(),QString("equivalent"));
    QCOMPARE(proof.value("mapping").toString(),QString("explicitBijection"));
    QCOMPARE(proof.value("mappedPairs").toList().size(),qsizetype(map.size()));
    QVERIFY(!proof.value("promotionEnabled").toBool());
    // Names are ignored once a contract is supplied: contradictory names change nothing.
    auto misnamed=std::make_shared<ModelDecoderSnapshot>(*b->decoderSnapshot);
    for(std::size_t i=0;i<misnamed->nodeNames.size();++i)misnamed->nodeNames[i]=QStringLiteral("x%1").arg(i);
    QCOMPARE(compareFixedDecoder(a->decoderSnapshot,misnamed,{},options).value("verdict").toString(),QString("equivalent"));
    // A wrong but well-formed bijection is rejected by the same default/parent proof.
    auto wrong=options;std::swap(wrong.explicitMapping[0],wrong.explicitMapping[1]);
    QCOMPARE(compareFixedDecoder(a->decoderSnapshot,unnamed,{},wrong).value("verdict").toString(),QString("insufficient"));
    QVERIFY(compareFixedDecoder(a->decoderSnapshot,unnamed,{},wrong).value("reason").toString()!=QString("exactMappedDecodedProgram"));
    // Malformed contracts are never repaired: duplicate, out of range, wrong size, empty entries.
    for(int kind=0;kind<3;++kind) {
      auto bad=options;
      if(kind==0)bad.explicitMapping[1]=bad.explicitMapping[0];
      else if(kind==1)bad.explicitMapping[0]=map.size()+10;
      else bad.explicitMapping.pop_back();
      proof=compareFixedDecoder(a->decoderSnapshot,unnamed,{},bad);
      QCOMPARE(proof.value("verdict").toString(),QString("insufficient"));
      QCOMPARE(proof.value("reason").toString(),QString("invalidExplicitMapping"));
    }
    // Duplicate names stay unresolved against a reordered file without a contract (identity fallback needs byte-identical metadata); a supplied identity contract is an explicit statement, not inference.
    const auto amb=readModelDocument(dir+"/ambiguous.gltf",{});
    QVERIFY(compareFixedDecoder(amb->decoderSnapshot,a->decoderSnapshot).value("verdict").toString()!=QString("equivalent"));
    FixedDecoderOptions identity;for(std::size_t i=0;i<amb->decoderSnapshot->nodeNames.size();++i)identity.explicitMapping.push_back(i);
    proof=compareFixedDecoder(amb->decoderSnapshot,amb->decoderSnapshot,{},identity);
    QCOMPARE(proof.value("verdict").toString(),QString("equivalent"));
    QCOMPARE(proof.value("mapping").toString(),QString("explicitBijection"));
    auto swapped=identity;std::swap(swapped.explicitMapping[0],swapped.explicitMapping[1]);
    QCOMPARE(compareFixedDecoder(amb->decoderSnapshot,amb->decoderSnapshot,{},swapped).value("verdict").toString(),QString("insufficient"));
    std::stop_source stopped;stopped.request_stop();
    QCOMPARE(compareFixedDecoder(a->decoderSnapshot,unnamed,stopped.get_token(),options).value("verdict").toString(),QString("cancelled"));
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelR13DecoderForcedOverrideDefaults() {
    try {
    const auto dir=requireFixture("FORMAVIEW_R13_REORDER");QVERIFY(!dir.isEmpty());
    const auto a=readModelDocument(dir+"/base.gltf",{}),b=readModelDocument(dir+"/bad-default.gltf",{});
    // bad-default differs only in the Joint translation default, and the sole clip overrides that exact group.
    const auto proof=compareFixedDecoder(a->decoderSnapshot,b->decoderSnapshot);
    QCOMPARE(proof.value("verdict").toString(),QString("insufficient"));
    QCOMPARE(proof.value("reason").toString(),QString("mappedDefaultOrParentConflict"));
    QVERIFY(!proof.contains("commonCoverage")); // the strict exact-default conclusion is unchanged
    QVERIFY(!proof.value("promotionEnabled").toBool());
    const auto forced=proof.value("forcedOverrideCoverage").toMap();
    QCOMPARE(forced.value("verdict").toString(),QString("sameCommonOverridesDeadDefaults"));
    QCOMPARE(forced.value("reason").toString(),QString("deadDefaultsUnobservable"));
    QCOMPARE(forced.value("componentCount").toInt(),1);
    QCOMPARE(forced.value("deadDefaults").toList().size(),qsizetype(1));
    QVERIFY(!forced.value("authoredActionsEquivalent").toBool());
    QVERIFY(!forced.value("clipPairAligned").toBool());
    QVERIFY(!forced.value("promotionEnabled").toBool());
    // Independent evidence that the dead default is not observable through the real evaluator.
    const auto named=compareFixedDecoder(a->decoderSnapshot,a->decoderSnapshot);
    QCOMPARE(named.value("verdict").toString(),QString("equivalent"));
    const auto joint=std::find(b->decoderSnapshot->nodeNames.begin(),b->decoderSnapshot->nodeNames.end(),QStringLiteral("Joint"))-b->decoderSnapshot->nodeNames.begin();
    const auto jointSource=std::find(a->decoderSnapshot->nodeNames.begin(),a->decoderSnapshot->nodeNames.end(),QStringLiteral("Joint"))-a->decoderSnapshot->nodeNames.begin();
    for(double t:{0.,.3,.5,1.}) {
      const auto x=a->worldAt(0,t),y=b->worldAt(0,t);QVERIFY(x[static_cast<std::size_t>(jointSource)]==y[static_cast<std::size_t>(joint)]);
    }
    // Negative controls: every case leaves the default observable and must not earn the dead-default conclusion.
    const auto edit=[&](auto mutate){auto s=std::make_shared<ModelDecoderSnapshot>(*b->decoderSnapshot);mutate(*s);return s;};
    const auto localOffset=[&](qsizetype node){return qsizetype(8)+node*192+8;};
    const auto putFloat=[&](ModelDecoderSnapshot &s,qsizetype node,qsizetype field,float value){
      std::memcpy(s.metadata.data()+localOffset(node)+field,&value,sizeof(value));};
    const auto expectObservable=[&](const std::shared_ptr<ModelDecoderSnapshot> &changed,const char *label){
      const auto r=compareFixedDecoder(a->decoderSnapshot,changed);
      QVERIFY2(r.value("verdict").toString()==QString("insufficient"),label);
      QVERIFY2(r.value("forcedOverrideCoverage").toMap().value("verdict").toString()!=QString("sameCommonOverridesDeadDefaults"),label);
    };
    expectObservable(edit([&](auto &s){putFloat(s,joint,92,.25f);}),"rotation default of an animated node is read by the evaluator");
    expectObservable(edit([&](auto &s){putFloat(s,joint,108,2.f);}),"scale default is read by the evaluator");
    const auto quaternion=std::find(b->decoderSnapshot->nodeNames.begin(),b->decoderSnapshot->nodeNames.end(),QStringLiteral("QuaternionFrame"))-b->decoderSnapshot->nodeNames.begin();
    expectObservable(edit([&](auto &s){putFloat(s,quaternion,80,5.f);}),"unanimated node default differs");
    expectObservable(edit([&](auto &s){
      auto data=std::make_shared<formaview::gltf::GltfAnimationData>(*s.animations);
      data->clips.push_back(formaview::gltf::AnimationClip{"idle",0,1,{}});s.animations=data;}),"a second clip that leaves the node unanimated reads its static local");
    expectObservable(edit([&](auto &s){
      auto data=std::make_shared<formaview::gltf::GltfAnimationData>(*s.animations);
      data->clips[0].channels[0].path=formaview::gltf::TrackPath::Scale;s.animations=data;}),"the overriding group is no longer the differing group");
    expectObservable(edit([&](auto &s){
      auto data=std::make_shared<formaview::gltf::GltfAnimationData>(*s.animations);
      data->clips[0].channels[0].times.clear();data->clips[0].channels[0].values.clear();s.animations=data;}),"an empty channel is skipped by the evaluator and forces nothing");
    expectObservable(edit([&](auto &s){
      auto data=std::make_shared<formaview::gltf::GltfAnimationData>(*s.animations);
      data->clips[0].channels[0].values.pop_back();s.animations=data;}),"malformed channel shape does not force a group");
    // Parent topology and bindings are never relaxed.
    for(const auto &file:{"bad-parent.gltf","bad-binding.gltf"}) {
      const auto bad=readModelDocument(dir+"/"+file,{});
      const auto r=compareFixedDecoder(a->decoderSnapshot,bad->decoderSnapshot);
      QCOMPARE(r.value("verdict").toString(),QString("insufficient"));
      QVERIFY(!r.contains("forcedOverrideCoverage"));
    }
    // Identical defaults never claim the dead-default conclusion.
    QVERIFY(!compareFixedDecoder(a->decoderSnapshot,a->decoderSnapshot).contains("forcedOverrideCoverage"));
    std::stop_source stopped;stopped.request_stop();
    QCOMPARE(compareFixedDecoder(a->decoderSnapshot,b->decoderSnapshot,stopped.get_token()).value("verdict").toString(),QString("cancelled"));
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelR13DecoderPressure() {
    namespace gl=formaview::gltf;
    // Synthetic chain snapshots exercise cancellation and every budget without a huge fixture.
    const auto build=[](std::size_t n,std::size_t bindings,std::size_t channelsPerNode){
      auto s=std::make_shared<ModelDecoderSnapshot>();
      QByteArray m;const auto put=[&](const auto &v){m.append(reinterpret_cast<const char*>(&v),sizeof(v));};
      put(n);
      for(std::size_t i=0;i<n;++i){const std::int64_t parent=i?static_cast<std::int64_t>(i-1):-1;put(parent);m.append(QByteArray(184,'\0'));
        s->nodeNames.push_back(QStringLiteral("n%1").arg(i));}
      put(bindings);
      for(std::size_t k=0;k<bindings;++k){const std::size_t instance=k/n,joint=k%n;put(instance);put(joint);m.append(QByteArray(128,'\0'));}
      s->metadata=m;s->sourceSha="synthetic";
      auto data=std::make_shared<gl::GltfAnimationData>();data->node_count=static_cast<std::uint32_t>(n);
      gl::AnimationClip clip;clip.name="c";clip.start=0;clip.end=1;
      for(std::size_t i=0;i<n;++i)for(std::size_t p=0;p<channelsPerNode;++p){
        gl::AnimationChannel c;c.node_index=static_cast<std::uint32_t>(i);c.path=static_cast<gl::TrackPath>(p);c.interpolation=gl::Interpolation::Linear;
        c.components=p==1?4:3;c.times={0,1};c.values.assign(2*c.components,double(i%7)*.1);clip.channels.push_back(std::move(c));}
      data->clips.push_back(std::move(clip));s->animations=data;return s;};
    QElapsedTimer phase;phase.start();
    const auto big=build(24000,0,3);const auto same=std::make_shared<ModelDecoderSnapshot>(*big);
    QCOMPARE(compareFixedDecoder(big,same).value("verdict").toString(),QString("equivalent"));
    int cancelled=0,finished=0;
    QElapsedTimer full;full.start();
    QCOMPARE(compareFixedDecoder(big,same).value("verdict").toString(),QString("equivalent"));
    const auto fullMicros=std::max<qint64>(full.nsecsElapsed()/1000,1000);
    for(int k=0;k<32;++k) {
      std::stop_source source;
      // Spread stop requests over 0..1.5x the measured uncancelled duration so both outcomes occur.
      const auto delay=static_cast<qint64>(fullMicros*1.5*k/31.0);
      std::thread requester([&]{std::this_thread::sleep_for(std::chrono::microseconds(delay));source.request_stop();});
      const auto r=compareFixedDecoder(big,same,source.get_token());requester.join();
      const auto verdict=r.value("verdict").toString();
      QVERIFY2(verdict=="cancelled"||verdict=="equivalent",qPrintable(verdict+"/"+r.value("reason").toString()));
      if(verdict=="cancelled"){++cancelled;QVERIFY(!r.contains("commonCoverage")||r.value("reason").toString()=="cancelled");}else ++finished;
    }
    QCOMPARE(cancelled+finished,32);
    qInfo()<<"P35 pressure cancel stress: cancelled"<<cancelled<<"finished"<<finished<<"of 32 randomized stop times; full compare"<<fullMicros<<"us; setup+stress"<<phase.elapsed()<<"ms";
    // Budgets are only ever tightened by callers; default limits stay in force.
    const auto tiny=build(64,0,3);const auto tinyCopy=std::make_shared<ModelDecoderSnapshot>(*tiny);
    FixedDecoderOptions tight;tight.channelBudget=100;
    auto r=compareFixedDecoder(tiny,tinyCopy,{},tight);
    QCOMPARE(r.value("verdict").toString(),QString("insufficient"));QCOMPARE(r.value("reason").toString(),QString("budgetExceeded"));
    tight=FixedDecoderOptions{};tight.scalarBudget=500;
    r=compareFixedDecoder(tiny,tinyCopy,{},tight);
    QCOMPARE(r.value("verdict").toString(),QString("insufficient"));QCOMPARE(r.value("reason").toString(),QString("budgetExceeded"));
    FixedDecoderOptions loose;loose.channelBudget=std::numeric_limits<std::size_t>::max();loose.scalarBudget=std::numeric_limits<std::size_t>::max();
    QCOMPARE(compareFixedDecoder(tiny,tinyCopy,{},loose).value("verdict").toString(),QString("equivalent"));
    qInfo()<<"P35 pressure budgets done at"<<phase.elapsed()<<"ms";
    // Hard structural caps: nodes 65536 and bindings 262144.
    const auto tooManyNodes=build(65537,0,0);
    QCOMPARE(compareFixedDecoder(tooManyNodes,tooManyNodes).value("reason").toString(),QString("invalidSnapshot"));
    const auto tooManyBindings=build(600,262145,0);
    QCOMPARE(compareFixedDecoder(tooManyBindings,tooManyBindings).value("reason").toString(),QString("invalidSnapshot"));
    const auto atLimit=build(600,262144,0);
    QCOMPARE(compareFixedDecoder(atLimit,atLimit).value("verdict").toString(),QString("equivalent"));
    qInfo()<<"P35 pressure structural caps done at"<<phase.elapsed()<<"ms";
    // Trailing garbage, truncation and duplicate channel targets are corruption, never silently accepted.
    auto trailing=std::make_shared<ModelDecoderSnapshot>(*tiny);trailing->metadata.append('x');
    QCOMPARE(compareFixedDecoder(tiny,trailing).value("verdict").toString(),QString("insufficient"));
    auto truncated=std::make_shared<ModelDecoderSnapshot>(*tiny);truncated->metadata.chop(3);
    QCOMPARE(compareFixedDecoder(tiny,truncated).value("verdict").toString(),QString("insufficient"));
    auto duplicate=std::make_shared<ModelDecoderSnapshot>(*tiny);
    auto data=std::make_shared<gl::GltfAnimationData>(*duplicate->animations);data->clips[0].channels.push_back(data->clips[0].channels[0]);duplicate->animations=data;
    QCOMPARE(compareFixedDecoder(tiny,duplicate).value("verdict").toString(),QString("insufficient"));
  }
  void modelR13DecoderStalePublication() {
    try {
    const auto dir=requireFixture("FORMAVIEW_R13_REORDER");QVERIFY(!dir.isEmpty());
    auto *session=qobject_cast<ModelSession*>(desktop_->model());QVERIFY(session);
    const auto cleanup=qScopeGuard([&]{desktop_->clearAsset();});
    for(int round=0;round<9;++round) {
      desktop_->openPath(dir+"/base.gltf");
      QTRY_VERIFY_WITH_TIMEOUT(session->inspectable()||!session->error().isEmpty(),30000);QVERIFY(session->inspectable());
      session->compareSkeletonWith(dir+"/reordered.gltf");
      QVERIFY(session->compareBusy());
      switch(round%3) {
        case 0:desktop_->clearAsset();break;
        case 1:desktop_->openPath(dir+"/different.gltf");
          // Desktop::openPath is asynchronous; the old document stays current (and may legitimately publish)
          // until the new asset lands and clear() supersedes it. Assert only after the landing.
          QTRY_VERIFY_WITH_TIMEOUT(session->path()==dir+"/different.gltf"&&(session->inspectable()||!session->error().isEmpty()),30000);
          break;
        default:QTest::qWait(round);desktop_->clearAsset();break;
      }
      QTRY_VERIFY_WITH_TIMEOUT(!session->compareBusy(),30000);
      QVERIFY2(session->compareResult().isEmpty(),"a superseded comparison must not publish");
      QVERIFY(session->compareError().isEmpty());
    }
    desktop_->openPath(dir+"/base.gltf");
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable()||!session->error().isEmpty(),30000);QVERIFY(session->inspectable());
    session->compareSkeletonWith(dir+"/reordered.gltf");
    QTRY_VERIFY_WITH_TIMEOUT(!session->compareBusy()&&!session->compareResult().isEmpty(),30000);
    QCOMPARE(session->compareResult().value("transformValidation").toMap().value("fixedDecoder").toMap().value("verdict").toString(),QString("equivalent"));
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelFbxDialectOracle() {
    try {
    // Autodesk FBX SDK 2020.2 EvaluateGlobalTransform is the oracle (p35-dialect). 48 chains:
    // 3 inheritance types x uniform/non-uniform root scale x plain/pivot+pre/post x +/- scale x 2 rotation orders.
    const auto dir=requireFixture("FORMAVIEW_FBX_DIALECT");QVERIFY(!dir.isEmpty());
    const auto expanded=requireFixture("FORMAVIEW_FBX_DIALECT_EXTRA").split(';',Qt::SkipEmptyParts);
    QCOMPARE(expanded.size(),qsizetype(3));
    QStringList dirs{dir};dirs.append(expanded);
    for(int domain=0;domain<dirs.size();++domain) {
    const auto current=dirs[domain];
    QFile oracle(current+"/oracle.txt");QVERIFY(oracle.open(QIODevice::ReadOnly));
    QMap<QString,double> worst;QMap<QString,int> samples;QHash<QString,std::shared_ptr<formaview::skeletal::FbxDocument>> documents;
    while(!oracle.atEnd()) {
      const auto parts=QString::fromUtf8(oracle.readLine()).trimmed().split(' ',Qt::SkipEmptyParts);
      if(parts.isEmpty())continue;
      QCOMPARE(parts.size(),19);
      const auto file=parts[0];const double seconds=parts[1].toDouble();
      auto &document=documents[file];
      if(!document)document=std::make_shared<formaview::skeletal::FbxDocument>(formaview::skeletal::FbxDocument::read(std::filesystem::path((current+"/"+file).toStdWString())));
      const auto pose=document->evaluate(0,seconds);std::size_t at=0;
      while(at<document->data().nodes.size()&&QString::fromStdString(document->data().nodes[at].source.original_name)!=parts[2])++at;
      QVERIFY(at<pose.world.size());
      const auto parent=document->data().nodes[at].parent;
      const auto chained=parent?formaview::skeletal::multiply(pose.world[*parent],pose.local[at]):pose.local[at];
      for(std::size_t k=0;k<16;++k)QVERIFY(std::abs(chained[k]-pose.world[at][k])<=1e-7);
      const auto &node=document->data().nodes[at];
      const auto rest=parent?formaview::skeletal::multiply(document->data().nodes[*parent].default_world,node.default_local):node.default_local;
      for(std::size_t k=0;k<16;++k)QVERIFY(std::abs(rest[k]-node.default_world[k])<=1e-7);
      double error=0;for(int c=0;c<4;++c)for(int r=0;r<4;++r)error=std::max(error,std::abs(parts[3+c*4+r].toDouble()-pose.world[at][static_cast<std::size_t>(r*4+c)]));
      worst[file]=std::max(worst.value(file),error);++samples[file];
    }
    QCOMPARE(worst.size(),qsizetype(domain==0?48:(domain==2?1728:576)));
    int matched=0,diverged=0;QJsonArray rows;
    for(auto it=worst.cbegin();it!=worst.cend();++it) {
      QCOMPARE(samples.value(it.key()),15);
      const bool valid=it.value()<=1e-7;
      if(valid)++matched;else ++diverged;
      rows.append(QJsonObject{{"file",it.key()},{"maxError",it.value()},{"matched",valid}});
    }
    QFile proof(root_+"/fbx-dialect-oracle-"+QString::number(domain)+".json");QVERIFY(proof.open(QIODevice::WriteOnly));proof.write(QJsonDocument(rows).toJson());proof.close();
    QCOMPARE(diverged,0);QCOMPARE(matched,worst.size());
    }
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelR13DecoderPublication() {
    try {
    const auto file=requireFixture("FORMAVIEW_R13_SAMPLES")+"/reach-a.gltf";
    auto *session=qobject_cast<ModelSession*>(desktop_->model());QVERIFY(session);
    const auto cleanup=qScopeGuard([&]{desktop_->clearAsset();});
    desktop_->openPath(file);
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable()||!session->error().isEmpty(),30000);
    QVERIFY(session->inspectable());session->compareSkeletonWith(file);
    QTRY_VERIFY_WITH_TIMEOUT(!session->compareBusy()&&!session->compareResult().isEmpty(),30000);
    const auto proof=session->compareResult().value("transformValidation").toMap().value("fixedDecoder").toMap();
    QCOMPARE(proof.value("verdict").toString(),QString("equivalent"));
    QCOMPARE(proof.value("domain").toString(),QString("fixedDecoderProgram"));
    QVERIFY(!proof.value("promotionEnabled").toBool());
    const auto doc=readModelDocument(file,{});QVERIFY(doc->decoderSnapshot);
    for(int kind=0;kind<8;++kind) {
      auto changed=std::make_shared<ModelDecoderSnapshot>(*doc->decoderSnapshot);
      auto data=std::make_shared<formaview::gltf::GltfAnimationData>(*changed->animations);
      QVERIFY(!data->clips.empty());QVERIFY(!data->clips[0].channels.empty());
      auto &channel=data->clips[0].channels[0];QVERIFY(!channel.values.empty());QVERIFY(!channel.times.empty());
      switch(kind) {
        case 0:channel.values[0]+=1;break;
        case 1:channel.times[0]+=.25;break;
        case 2:channel.node_index+=1;break;
        case 3:channel.components+=1;break;
        case 4:channel.interpolation=channel.interpolation==formaview::gltf::Interpolation::Step?formaview::gltf::Interpolation::Linear:formaview::gltf::Interpolation::Step;break;
        case 5:data->clips.pop_back();break;
        case 6:changed->metadata.append("different parent/default/bind program");break;
        case 7:data->node_count+=1;break;
      }
      changed->animations=data;
      QCOMPARE(compareFixedDecoder(doc->decoderSnapshot,changed).value("verdict").toString(),QString("insufficient"));
    }
    QCOMPARE(compareFixedDecoder({},doc->decoderSnapshot).value("reason").toString(),QString("missingEvidence"));

    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelR13FormatComparison() {
    try {
    namespace tv=formaview::skeletal::transform;
    const auto folder=requireFixture("FORMAVIEW_R13_SAMPLES");
    QVERIFY(!folder.isEmpty());
    const auto file=folder+"/reach-a.gltf", other=folder+"/reach-b.gltf";
    QVERIFY(QFileInfo::exists(file));QVERIFY(QFileInfo::exists(other));
    auto *session=qobject_cast<ModelSession*>(desktop_->model());QVERIFY(session);
    const auto cleanup=qScopeGuard([&]{desktop_->clearAsset();});
    desktop_->openPath(file);
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable()||!session->error().isEmpty(),30000);
    QVERIFY2(session->inspectable(),qPrintable(session->error()));
    QVERIFY2(session->hasComparableSkeleton(),"glTF must expose its full-node comparison entry");
    const auto a=readModelDocument(file,{}),b=readModelDocument(other,{});
    const auto ea=a->transformEvidence({}),eb=b->transformEvidence({});
    QVERIFY(ea.nativeSkinBindings.has_value());QCOMPARE(ea.nativeSkinBindings->size(),std::size_t(2));
    QVERIFY(ea.nodes[3].restMatrix.has_value());QVERIFY(!ea.nodes[3].restLocal.has_value());
    std::vector<tv::Pair> pairs;for(std::size_t i=0;i<ea.nodes.size();++i)pairs.push_back({i,i});
    QCOMPARE(tv::validate(ea,eb,pairs).verdict,tv::Verdict::Compatible);
    // Independent analytic oracle authored with the fixture, not product output.
    QFile oracleFile(folder+"/oracle.json");QVERIFY(oracleFile.open(QIODevice::ReadOnly));
    const auto files=QJsonDocument::fromJson(oracleFile.readAll()).object().value("files").toArray();
    int compared=0;double maxError=0;
    for(const auto &entry:files) {
      const auto object=entry.toObject();const auto doc=readModelDocument(folder+"/"+object.value("file").toString(),{});
      const auto evidence=doc->transformEvidence({});QVERIFY(evidence.nativeSkinBindings.has_value());
      QCOMPARE(doc->meshes.size(),std::size_t(1));QCOMPARE(doc->meshes[0].raw.size(),std::size_t(8));
      for(const auto &sample:object.value("samples").toArray()) {
        const auto sampleObject=sample.toObject();const auto worlds=doc->worldAt(0,sampleObject.value("time").toDouble());
        const auto expected=sampleObject.value("positions").toArray();QCOMPARE(expected.size(),8);
        for(std::size_t v=0;v<8;++v) {
          const auto &binding=evidence.nativeSkinBindings->at(v<4?0:1);
          const auto &point=doc->meshes[0].raw[v];const double p[4]={point.x,point.y,point.z,1};
          double local[4]{};for(int r=0;r<4;++r)for(int c=0;c<4;++c)local[r]+=binding.inverseBind[static_cast<std::size_t>(c*4+r)]*p[c];
          const auto values=expected[static_cast<qsizetype>(v)].toArray();
          for(int r=0;r<3;++r) {
            double value=0;for(int c=0;c<4;++c)value+=worlds[binding.joint][static_cast<std::size_t>(r*4+c)]*local[c];
            const double error=std::abs(value-values[r].toDouble());maxError=std::max(maxError,error);QVERIFY(error<=1e-7);++compared;
          }
        }
      }
    }
    qInfo()<<"R13 analytic glTF coordinates"<<compared<<"max error"<<maxError;
    const auto bad=readModelDocument(folder+"/wrong-bind.gltf",{})->transformEvidence({});
    QCOMPARE(tv::validate(ea,bad,pairs).reason,tv::Reason::BindingConflict);
    session->compareSkeletonWith(other);
    QTRY_VERIFY_WITH_TIMEOUT(!session->compareBusy()&&(!session->compareResult().isEmpty()||!session->compareError().isEmpty()),30000);
    QVERIFY2(session->compareError().isEmpty(),qPrintable(session->compareError()));
    const auto report=session->compareResult();
    QCOMPARE(report.value("applicabilityVerdict").toString(),QString("compatibleNotDrivable"));
    QCOMPARE(report.value("structureVerdict").toString(),QString("structuralDirect"));
    QCOMPARE(report.value("transformValidation").toMap().value("verdict").toString(),QString("compatible"));
    QFile proof(root_+"/r13-gltf-proof.json");QVERIFY(proof.open(QIODevice::WriteOnly));
    proof.write(QJsonDocument::fromVariant(report).toJson());
    session->compareSkeletonWith(folder+"/wrong-bind.gltf");
    QTRY_VERIFY_WITH_TIMEOUT(!session->compareBusy()&&(!session->compareResult().isEmpty()||!session->compareError().isEmpty()),30000);
    QVERIFY2(session->compareError().isEmpty(),qPrintable(session->compareError()));
    QCOMPARE(session->compareResult().value("applicabilityVerdict").toString(),QString("notApplicable"));
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelTransformEvidenceAdapters() {
    try {
    namespace tv = formaview::skeletal::transform;
    const auto current=requireFixture("FORMAVIEW_ANIMATION_TARGET");
    QVERIFY(QFileInfo::exists(current));
    const auto fbx=formaview::skeletal::FbxDocument::read(std::filesystem::path(current.toStdWString()));
    const auto f=fbx.transform_evidence();
    QCOMPARE(f.sha256,fbx.data().source.identity.sha256);
    QCOMPARE(f.nodes.size(),fbx.data().nodes.size());
    QVERIFY(f.fullNodeDomain);
    QVERIFY2(std::all_of(f.nodes.begin(),f.nodes.end(),[](const auto &n){return n.evaluation.has_value();}), "R13 must represent native FBX inheritance and declared default rotation semantics");
    QVERIFY(f.coordinates.has_value());
    QCOMPARE(f.coordinates->metresPerUnit,fbx.data().metres_per_unit);
    QVERIFY(!f.bindings.has_value()); // Cloud has skin; current pose is not bind proof.
    for (std::size_t i=0;i<f.nodes.size();++i) {
      QCOMPARE(f.nodes[i].fileIndex,static_cast<std::uint32_t>(i));
      QVERIFY(f.nodes[i].restLocal.has_value());
      QCOMPARE(f.nodes[i].parent.has_value(),fbx.data().nodes[i].parent.has_value());
      if (f.nodes[i].parent) QCOMPARE(*f.nodes[i].parent,*fbx.data().nodes[i].parent);
    }
    std::stop_source cancelled; cancelled.request_stop();
    QVERIFY_THROWS_EXCEPTION(formaview::skeletal::Failure, (void)fbx.transform_evidence(cancelled.get_token()));
    QTemporaryDir temp(root_+"/r13-evidence-XXXXXX");
    QVERIFY(temp.isValid());
    const auto file=temp.filePath("mesh.gltf");
    const auto binary=temp.filePath("mesh.bin");
    const QByteArray json=R"({"asset":{"version":"2.0"},"buffers":[{"uri":"mesh.bin","byteLength":36}],"bufferViews":[{"buffer":0,"byteLength":36}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"nodes":[{"name":"Root","mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
    QFile jf(file); QVERIFY(jf.open(QIODevice::WriteOnly)); QCOMPARE(jf.write(json),json.size()); jf.close();
    float positions[9]={0,0,0,1,0,0,0,1,0};
    const auto writePositions=[&] {
      QFile bf(binary); QVERIFY(bf.open(QIODevice::WriteOnly));
      QCOMPARE(bf.write(reinterpret_cast<const char *>(positions),sizeof(positions)),qint64(sizeof(positions)));
    };
    writePositions();
    const auto a=readModelDocument(file,{});
    QVERIFY(a->transformEvidence);
    const auto ea=a->transformEvidence({});
    QVERIFY(ea.fullNodeDomain); QCOMPARE(ea.nodes.size(),std::size_t(1));
    QVERIFY(ea.coordinates.has_value()); QCOMPARE(ea.coordinates->metresPerUnit,1.);
    QVERIFY(ea.bindings.has_value()); QVERIFY(ea.bindings->empty());
    QVERIFY(ea.nodes[0].evaluation.has_value());
    const std::vector<tv::Pair> pairs{{0,0}};
    QCOMPARE(tv::validate(ea,ea,pairs).verdict,tv::Verdict::Compatible);
    // Same JSON, different external buffer: must not share an identity proof.
    positions[3]=0.5f; writePositions();
    const auto b=readModelDocument(file,{}); const auto eb=b->transformEvidence({});
    QCOMPARE(a->hash,b->hash); QVERIFY(ea.sha256!=eb.sha256);
    QCOMPARE(a->transformEvidence({}).sha256,ea.sha256); // Immutable snapshot.
    QVERIFY_THROWS_EXCEPTION(formaview::skeletal::Failure, (void)a->transformEvidence(cancelled.get_token()));
    // Static matrices retain their raw representation; no TRS decomposition.
    const auto matrixJson=QByteArray(json).replace("\"mesh\":0}","\"mesh\":0,\"matrix\":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]}");
    QVERIFY(matrixJson!=json);
    QVERIFY(jf.open(QIODevice::WriteOnly|QIODevice::Truncate)); QCOMPARE(jf.write(matrixJson),matrixJson.size()); jf.close();
    const auto matrixDoc=readModelDocument(file,{}); const auto matrixEvidence=matrixDoc->transformEvidence({});
    QVERIFY(!matrixEvidence.nodes[0].restLocal.has_value());
    QVERIFY(matrixEvidence.nodes[0].restMatrix.has_value());
    QCOMPARE(tv::validate(matrixEvidence,matrixEvidence,pairs).verdict,tv::Verdict::Compatible);
    QFile proof(root_+"/r13-adapter-proof.json"); QVERIFY(proof.open(QIODevice::WriteOnly));
    proof.write(QJsonDocument(QJsonObject{{"fbxSha",QString::fromStdString(f.sha256)},
        {"fbxNodes",static_cast<int>(f.nodes.size())},{"fbxUnits",f.coordinates->metresPerUnit},
        {"gltfMainSha",a->hash},{"gltfEvidenceA",QString::fromStdString(ea.sha256)},
        {"gltfEvidenceB",QString::fromStdString(eb.sha256)},
        {"crossFileRealPositive",false}}).toJson());
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelSkeletonCompareIdentityRule() {
    try {
    // R13 conservative rule (CR04): identical source content is the only
    // sufficient transform-identity proof, so a self-compare must reach
    // directApplicable, and a structuralDirect compare against a different
    // file must remain structureOnly - never directApplicable on structure
    // alone.
    const auto current =
        QFileInfo(requireFixture("FORMAVIEW_ANIMATION_TARGET"));
    const auto other =
        QFileInfo(requireFixture("FORMAVIEW_ANIMATION_EXTERNAL"));
    if (!current.exists() || !other.exists()) {
      QSKIP("Set FORMAVIEW_ANIMATION_TARGET and FORMAVIEW_ANIMATION_EXTERNAL");
      return;
    }
    auto *session = qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    const auto restore = qScopeGuard([&] { desktop_->clearAsset(); });
    // QTRY_* macros expand to a plain `return;`, so this helper cannot
    // return the result map itself.
    const auto compare = [&](const QFileInfo &against, QVariantMap &out) {
      session->compareSkeletonWith(against.absoluteFilePath());
      QTRY_VERIFY_WITH_TIMEOUT(!session->compareBusy() &&
                                   (session->compareResult().size() > 1 ||
                                    !session->compareError().isEmpty()),
                               30000);
      QVERIFY2(session->compareError().isEmpty(),
               qPrintable(session->compareError()));
      out = session->compareResult();
    };
    desktop_->openPath(current.absoluteFilePath());
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable() ||
                                 !session->error().isEmpty(),
                             30000);
    QVERIFY2(session->inspectable(), qPrintable(session->error()));
    QVERIFY(session->hasComparableSkeleton());
    QVariantMap self;
    compare(current, self);
    QCOMPARE(self.value("structureVerdict").toString(),
             QStringLiteral("structuralDirect"));
    QCOMPARE(self.value("applicabilityVerdict").toString(),
             QStringLiteral("directApplicable"));
    const auto evidence = self.value("transformValidation").toMap();
    QVERIFY2(!evidence.isEmpty(), "R13 worker must publish source-backed transform diagnostics");
    QCOMPARE(evidence.value("verdict").toString(), QStringLiteral("compatible"));
    QCOMPARE(evidence.value("reason").toString(), QStringLiteral("identity"));
    QCOMPARE(evidence.value("sourceSha").toString(), evidence.value("targetSha").toString());
    QVERIFY(!evidence.value("promotionEnabled").toBool());
    QVERIFY(evidence.value("nodesA").toInt() > 0);
    QVariantMap cross;
    compare(other, cross);
    // Different source content can never supply the transform-identity
    // proof, whatever the structure outcome is (this pair compares as
    // "partial" -> trialOnly).
    QVERIFY2(cross.value("applicabilityVerdict").toString() !=
                 QStringLiteral("directApplicable"),
             "Cross-file compare must not claim direct applicability");
    if (cross.value("structureVerdict").toString() == "structuralDirect")
      QCOMPARE(cross.value("applicabilityVerdict").toString(),
               QStringLiteral("structureOnly"));
    QVERIFY(!cross.value("transformValidation").toMap().isEmpty());
    QVERIFY(!cross.value("transformValidation").toMap().value("promotionEnabled").toBool());
    QFile proof(root_+"/r13-comparison-proof.json"); QVERIFY(proof.open(QIODevice::WriteOnly));
    proof.write(QJsonDocument(QJsonObject{{"self",QJsonObject::fromVariantMap(self)},
                                        {"cross",QJsonObject::fromVariantMap(cross)}}).toJson());
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void modelSkeletonCompareTiming() {
    try {
    // M07 budget: skeleton compare is P95 <= 200 ms once data is ready; parse
    // (worker), engine+rows (GUI thread) and end-to-end are recorded apart.
    const auto current =
        QFileInfo(requireFixture("FORMAVIEW_ANIMATION_TARGET"));
    const auto other =
        QFileInfo(requireFixture("FORMAVIEW_ANIMATION_EXTERNAL"));
    if (!current.exists() || !other.exists()) {
      QSKIP("Set FORMAVIEW_ANIMATION_TARGET and FORMAVIEW_ANIMATION_EXTERNAL");
      return;
    }
    auto *session = qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    const auto restore = qScopeGuard([&] { desktop_->clearAsset(); });
    desktop_->openPath(current.absoluteFilePath());
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable() ||
                                 !session->error().isEmpty(),
                             30000);
    QVERIFY2(session->inspectable(), qPrintable(session->error()));
    QVERIFY(session->hasComparableSkeleton());
    std::vector<double> readMs, guiMs, totalMs;
    int bonesA = 0, bonesB = 0;
    constexpr int rounds = 20;
    for (int i = 0; i < rounds; ++i) {
      session->compareSkeletonWith(
          (i % 2 ? other : current).absoluteFilePath());
      QTRY_VERIFY_WITH_TIMEOUT(!session->compareBusy() &&
                                   (session->compareResult().size() > 1 ||
                                    !session->compareError().isEmpty()),
                               30000);
      QVERIFY2(session->compareError().isEmpty(),
               qPrintable(session->compareError()));
      const auto result = session->compareResult();
      const auto timing = result.value("timing").toMap();
      QVERIFY(timing.contains("guiCompareMs"));
      readMs.push_back(timing.value("readMs").toDouble());
      guiMs.push_back(timing.value("guiCompareMs").toDouble());
      totalMs.push_back(timing.value("endToEndMs").toDouble());
      bonesA = std::max(bonesA, result.value("bonesA").toInt());
      bonesB = std::max(bonesB, result.value("bonesB").toInt());
    }
    const auto p95 = [](std::vector<double> v) {
      std::sort(v.begin(), v.end());
      return v[static_cast<std::size_t>(std::ceil(v.size() * 0.95)) - 1];
    };
    const auto pack = [&](const char *name, const std::vector<double> &v) {
      return QJsonObject{{"stage", name}, {"p95Ms", p95(v)},
                         {"maxMs", *std::max_element(v.begin(), v.end())}};
    };
    evidence_["skeleton_compare_timing"] = QJsonObject{
        {"rounds", rounds}, {"bonesA", bonesA}, {"bonesB", bonesB},
        {"parseWorker", pack("parseWorker", readMs)},
        {"engineAndRowsGui", pack("engineAndRowsGui", guiMs)},
        {"endToEnd", pack("endToEnd", totalMs)},
        {"boundary", "GUI-thread engine+rows excludes parsing; end-to-end includes it. Not input latency."}};
    QVERIFY2(p95(guiMs) <= 200.0, "Engine+rows P95 exceeds the 200 ms budget");
    } catch (const MissingFixture &error) {
      QSKIP(error.what());
    } catch (const std::exception &error) {
      QFAIL(error.what());
    }
  }
  void screenCompatibility() {
    if (workflowHeadless()) QSKIP("Requires real Windows screens");
    auto *workspace = item("workspace");
    QVERIFY(workspace);
    auto *originalScreen = window_->screen();
    const auto originalGeometry = window_->geometry();
    const auto originalLanguage = desktop_->language();
    QObject *help = nullptr;
    for (auto *object : window_->findChildren<QObject *>())
      if (object->property("title").toString() == "FormaView" && object->metaObject()->indexOfMethod("open()") >= 0)
        help = object;
    QVERIFY(help);
    const auto restore = qScopeGuard([&] {
      QMetaObject::invokeMethod(help, "close");
      if (auto *drawer = window_->findChild<QObject *>("fileDrawer"))
        QMetaObject::invokeMethod(drawer, "close");
      desktop_->filter("", 0, 0);
      desktop_->setLanguage(originalLanguage);
      window_->setScreen(originalScreen);
      window_->setGeometry(originalGeometry);
    });
    workspace->setProperty("tasksVisible", false);
    workspace->setProperty("settingsVisible", false);
    workspace->setProperty("conversionVisible", false);
    workspace->setProperty("focusMode", false);
    desktop_->navigate(fixture_);
    QTRY_VERIFY(idle() && desktop_->directory() == fixture_);
    QJsonArray observations;
    int screenIndex = 0;
    for (auto *screen : QGuiApplication::screens()) {
      window_->setScreen(screen);
      window_->resize(1280, 800);
      window_->setPosition(screen->availableGeometry().topLeft() + QPoint(24, 24));
      QTRY_COMPARE(window_->screen(), screen);
      QTRY_VERIFY(window_->isExposed());
      QTest::qWait(100);
      const auto firstSize = window_->size();
      // Keep the initial expanded-client geometry discrepancy as evidence.
      // A second explicit resize tests the settled usable window, as the
      // existing model rail test does; this does not validate the first resize.
      if (firstSize != QSize(1280, 800)) window_->resize(1280, 800);
      QTRY_COMPARE(window_->size(), QSize(1280, 800));
      for (const auto &language : {QString("zh-CN"), QString("en"), QString("zh-TW")}) {
        desktop_->setLanguage(language);
        auto *drawer = window_->findChild<QObject *>("fileDrawer");
        QVERIFY(drawer);
        if (auto *rail = item("fileRail")) {
          QVERIFY(QMetaObject::invokeMethod(rail, "clicked"));
          QTRY_VERIFY(drawer->property("opened").toBool());
        }
        auto *search = item("search");
        QVERIFY(search);
        search->forceActiveFocus();
        QTRY_VERIFY(search->hasActiveFocus());
        QVERIFY(QMetaObject::invokeMethod(search, "selectAll"));
        QInputMethodEvent commit;
        commit.setCommitString("图片");
        QCoreApplication::sendEvent(search, &commit);
        QTRY_COMPARE(desktop_->query(), QString("图片"));
        QTRY_COMPARE(desktop_->count(), 24);
        QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(window_, Qt::Key_Backspace);
        QTRY_VERIFY(idle() && desktop_->query().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(drawer, "close"));
        QTRY_VERIFY(!drawer->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(help, "open"));
        QTRY_VERIFY(help->property("opened").toBool());
        QVERIFY(help->property("x").toDouble() >= 0 && help->property("y").toDouble() >= 0);
        QVERIFY(help->property("x").toDouble() + help->property("width").toDouble() <= window_->width());
        QVERIFY(help->property("y").toDouble() + help->property("height").toDouble() <= window_->height());
        QTest::qWait(80);
        const auto capture = captureWindow(window_);
        QVERIFY(!capture.isNull());
        QVERIFY(capture.save(root_ + QString("/screen-%1-help-%2.png").arg(screenIndex).arg(language)));
        observations.append(QJsonObject{{"screen", screen->name()}, {"language", language},
          {"dpr", window_->devicePixelRatio()}, {"logical_width", window_->width()}, {"logical_height", window_->height()},
          {"first_resize_width", firstSize.width()}, {"first_resize_height", firstSize.height()},
          {"available_width", screen->availableGeometry().width()}, {"available_height", screen->availableGeometry().height()},
          {"capture_width", capture.width()}, {"capture_height", capture.height()}});
        QVERIFY(QMetaObject::invokeMethod(help, "close"));
        QTRY_VERIFY(!help->property("visible").toBool());
      }
      ++screenIndex;
    }
    evidence_["screen_compatibility"] = observations;
  }

  void cleanupTestCase() {
    if (const auto mem = formaview::process_memory()) {
      evidence_["working_set_bytes"] =
          static_cast<qint64>(mem->working_set_bytes);
      evidence_["peak_working_set_bytes"] =
          static_cast<qint64>(mem->peak_working_set_bytes);
    }
    gpuSampler_.stop();
    evidence_["gpu_observed_peak_local_bytes"] = workflowHeadless() ? QJsonValue(QJsonValue::Null) : QJsonValue(peakGpuLocal_);
    evidence_["gpu_sampling"] = workflowHeadless() ? "not performed in offscreen workflow mode" :
        "DXGI per-process accounting on a retained actual QQuickWindow D3D11 "
        "adapter, "
        "250ms plus screenshots; transient peaks between samples may be "
        "missed; other enumerated adapters are not summed";
    engine_.reset();
    desktop_.reset();
    QCoreApplication::processEvents();
    QTest::qWait(100);
    if (fixtureDirectory_) {
      const auto path = fixtureDirectory_->path();
      fixtureDirectory_.reset();
      QVERIFY2(!QFileInfo::exists(path), "Generated GUI fixtures were not removed");
    }
    if (stateDirectory_) {
      const auto path = stateDirectory_->path();
      stateDirectory_.reset();
      QVERIFY2(!QFileInfo::exists(path), "GUI run state was not removed");
    }
    if (!workflowHeadless()) evidence_["gpu_after_release"] = gpuMemory();
    if (const auto mem = formaview::process_memory())
      evidence_["after_release_working_set_bytes"] =
          static_cast<qint64>(mem->working_set_bytes);
    QFile report(root_ + "/desktop-evidence.json");
    QVERIFY(report.open(QIODevice::WriteOnly));
    report.write(QJsonDocument(evidence_).toJson());
  }
};
int main(int argc, char **argv) {
  QQuickStyle::setStyle("Basic");
  if (workflowHeadless() && qEnvironmentVariable("QT_QPA_PLATFORM") != "offscreen") {
    qCritical("Headless workflow tests require QT_QPA_PLATFORM=offscreen");
    return 64;
  }
  QQuickWindow::setGraphicsApi(workflowHeadless() ? QSGRendererInterface::Software : QSGRendererInterface::Direct3D11);
  QImageReader::setAllocationLimit(32);
  MeasuredApplication app(argc, argv);
  app.styleHints()->setColorScheme(Qt::ColorScheme::Dark);
  auto font = QFont{};
  font.setFamilies({"Segoe UI", "Microsoft YaHei UI"});
  font.setPixelSize(14);
  app.setFont(font);
  QCoreApplication::setOrganizationName("FormaViewTests");
  QCoreApplication::setApplicationName("Desktop");
  if (workflowHeadless()) {
    const QSet<QString> allowed{"quickExportWorkflow", "settingsDirtyLeaveCallbacks", "conversionEditorPlanBinding", "imageJobsCancelAndShutdown", "folderMenuIsolationAndPaths", "folderNavigationInteraction", "imageHistoryBeyondTwoPages", "preparedModelTreesTransferOwnership"};
    auto selected = 0;
    const auto arguments = app.arguments();
    for (auto i = 1; i < arguments.size(); ++i) {
      if (arguments[i] == "-o" && i + 1 < arguments.size()) { ++i; continue; }
      if (!allowed.contains(arguments[i])) {
        qCritical("Offscreen workflow mode accepts only explicitly selected workflow cases");
        return 64;
      }
      ++selected;
    }
    if (!selected) return 64;
  }
#ifdef FORMAVIEW_IMAGE_UI
  if (app.arguments().value(1) == "--recovery-producer") {
    if (app.arguments().size() != 5) return 20;
    const auto input = app.arguments()[2];
    const auto output = app.arguments()[3];
    ImageJobs service;
    QStringList inputs;
    for (int i = 0; i < 64; ++i) inputs.append(input + "/source.png");
    const auto wait = [](auto predicate, int timeoutMs = 35000) {
      QElapsedTimer deadline; deadline.start();
      while (!predicate() && deadline.elapsed() < timeoutMs) QTest::qWait(10);
      return predicate();
    };
    service.preflight(inputs, {{"input_roots",QStringList{input}}, {"output_roots",QStringList{output}}, {"allow_overwrite",false}}, {{"target","png"}, {"png_compression",9}}, "recovery-{index}.{ext}", "skip");
    if (!wait([&] { return !service.busy() && service.plan().value("state") == "ready"; }, reviewProducerPreflightWaitMs)) return 21;
    service.submit(true);
    if (!wait([&] { return !service.busy() && service.job().value("state") == "running"; })) return 22;
    QSaveFile ready(app.arguments()[4]);
    if (!ready.open(QIODevice::WriteOnly)) return 23;
    // Widen the publication window so the parent exercises an incomplete
    // signal deterministically if this writer ever loses atomic publication.
    QTest::qWait(150);
    const auto signalBytes = QJsonDocument(QJsonObject::fromVariantMap(service.job())).toJson();
    if (ready.write(signalBytes) != signalBytes.size() || !ready.commit()) return 24;
    return app.exec();
  }
#endif
  DesktopTests tests;
  return QTest::qExec(&tests, argc, argv);
}
#include "desktop_tests.moc"
