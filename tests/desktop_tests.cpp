#include "desktop.hpp"
#include "window_chrome.hpp"
#include "formaview/telemetry.hpp"
#include "thumbnail.hpp"
#ifdef FORMAVIEW_MODEL_UI
#include "model_session.hpp"
#include "model_view.hpp"
#endif
#ifdef FORMAVIEW_IMAGE_UI
#include "image_jobs.hpp"
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
#include <QMimeData>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QScopeGuard>
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
namespace {
bool workflowHeadless() {
  return qEnvironmentVariableIsSet("FORMAVIEW_TEST_HEADLESS");
}
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
  bool notify(QObject *receiver, QEvent *event) override {
    if (QThread::currentThread() != thread() || !measure)
      return QGuiApplication::notify(receiver, event);
    const auto started = std::chrono::steady_clock::now();
    const auto result = QGuiApplication::notify(receiver, event);
    const auto ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - started)
                        .count();
    maxEventMs = std::max(maxEventMs, ms);
    ++events;
    if (ms > 100)
      ++over100ms;
    if (ms > 10) {
      const QJsonObject sample{{"ms",ms},{"type",static_cast<int>(event->type())},{"receiver",receiver->metaObject()->className()},{"object",receiver->objectName()}};
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
  void save(const QString &name) {
    const auto pixels=window_->grabWindow();
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
    qputenv("FORMAVIEW_IMAGE_STATE_ROOT", (root_ + "/image-journal").toUtf8());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       root_ + "/settings");
    auto temp = QTemporaryDir(root_ + "/fixtures-XXXXXX");
    QVERIFY(temp.isValid());
    temp.setAutoRemove(false);
    fixture_ = temp.path() + "/中文目录";
    small_ = temp.path() + "/其他目录";
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
    const auto drive = desktop_->breadcrumbs("B:/Model/克劳德/Texture/");
    QCOMPARE(drive.size(), 4);
    QCOMPARE(drive[0].toMap()["path"].toString(), QString("B:/"));
    QCOMPARE(drive[1].toMap()["path"].toString(), QString("B:/Model"));
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
      if (target) QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
                                  target->mapToScene(QPointF(target->width()/2, target->height()/2)).toPoint());
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
    click(arrow);
    QTRY_COMPARE(desktop_->folderMenu().value("entries").toList().size(), 3);
    QCOMPARE(desktop_->sessionRevision(), session);
    if (preview) QCOMPARE(preview->height(), previewHeight);
    auto *sibling = findPath("folderChild", model + "/场景");
    QTRY_VERIFY((sibling = findPath("folderChild", model + "/场景")));
    QCOMPARE(sibling->height(), 36.0);
    click(sibling);
    QTRY_COMPARE(desktop_->directory(), model + "/场景");
    QTRY_VERIFY(idle());
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
        QVERIFY(popup->grabWindow().save(
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
    QCOMPARE(address->property("text").toString(), small_);
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
  void nativeWindowCaptionHitTesting() {
#ifdef Q_OS_WIN
    window_->showNormal();
    window_->resize(1600, 900);
    QTest::qWait(60);
    auto *bar = item("topBar");
    auto *menu = item("applicationMenuButton");
    QVERIFY(bar);
    QVERIFY(menu);
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
        QCOMPARE(hit(control->mapToScene(QPointF(control->width() / 2, control->height() / 2))),
                 LRESULT{HTCLIENT});
    }
    // A popup must receive the outside click rather than starting a window drag.
    QVERIFY(QMetaObject::invokeMethod(menu, "clicked"));
    QTRY_COMPARE(hit(caption()), LRESULT{HTCLIENT});
    QTest::keyClick(window_, Qt::Key_Escape);
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
#else
    QSKIP("Windows native caption handling");
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
    pane->setProperty("fixedSample", false);
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
      const auto frame = window_->grabWindow();
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
      const auto frame = window_->grabWindow();
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
    const auto frame = window_->grabWindow();
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
    const auto tiledFrame = window_->grabWindow();
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
    const auto fixtures =
        qEnvironmentVariable("FORMAVIEW_TEST_BOUNDARY_FIXTURES");
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
      QVERIFY(item("settingsDraftStatus")->mapToScene({0,0}).y() >= window_->height() - 44);
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
    QVERIFY(QMetaObject::invokeMethod(workspace, "openConversion", Q_ARG(QVariant, 0)));
    QTRY_VERIFY(item("conversionOutputRoot"));
    QCOMPARE(item("conversionPreflightStatus")->property("value").toString(), QString("待预检"));
    QVERIFY(item("conversionContentSource")->property("value").toString().contains("不套用预览"));
    QCOMPARE(item("conversionNamingExamples")->property("value").toString(), QString("唯一图片-converted.png"));
    const auto examples = service->nameExamples({source}, "{name}-converted.{ext}", "png", {});
    QCOMPARE(examples.value("names").toStringList(), QStringList{"唯一图片-converted.png"});
    QCOMPARE(service->nameExamples({source}, "{name}-{index}.{ext}", "png", {QVariantMap{{"target", "tiff"}}})
                 .value("names").toStringList(), QStringList{"唯一图片-1.tiff"});
    QVERIFY(!service->nameExamples({source}, "{name}_m{mip}.{ext}", "png", {}).value("error").toString().isEmpty());
    QVERIFY(!service->nameExamples({source}, "../{name}.{ext}", "png", {}).value("error").toString().isEmpty());
    const auto close = qScopeGuard([&] {
      if (auto *conversion = window_->findChild<QObject *>("conversionWorkspace"))
        QMetaObject::invokeMethod(conversion, "close");
    });
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
    auto bounds = [](QQuickItem *control) {
      const auto point = control->mapToScene({0, 0});
      return QJsonObject{{"x",point.x()}, {"y",point.y()}, {"width",control->width()}, {"height",control->height()}};
    };
    QJsonArray frames;
    auto capture = [&](const QString &name) {
      QTest::qWait(100);
      const auto image = window_->grabWindow();
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
        click(item("convertImagesButton"));
        QTRY_VERIFY(item("conversionOutputRoot"));
        QVERIFY(!item("previewPane"));
        auto *output = item("conversionOutputRoot");
        const auto destination = root_ + "/fidelity-output-" + suffix;
        QVERIFY(QDir().mkpath(destination));
        output->setProperty("text", destination);
        QTest::qWait(80);
        auto *left = item("conversionSources");
        auto *right = item("conversionSummary");
        QVERIFY(left && right);
        const auto leftPoint = left->mapToScene({0,0});
        const auto rightPoint = right->mapToScene({0,0});
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
        click(item("convertImagesButton"));
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
    QTRY_VERIFY_WITH_TIMEOUT(!service->busy() &&
                                 service->job().value("state") == "succeeded",
                             15000);
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
    const auto python = qEnvironmentVariable("FORMAVIEW_TEST_MCP_PYTHON");
    const auto adapter = qEnvironmentVariable("FORMAVIEW_TEST_MCP_ADAPTER");
    const auto client = qEnvironmentVariable("FORMAVIEW_TEST_MCP_CLIENT");
    QVERIFY2(QFile::exists(python) && QFile::exists(adapter) &&
                 QFile::exists(client),
             "Explicit MCP verification runtime and client paths required");
    QVERIFY(!service->mcpEnabled());
    service->setMcpEnabled(true);
    QVERIFY(service->mcpEnabled());
    QVERIFY(service->lastClient().isEmpty());
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
    service->selectJob(mcpResult["job_id"].toString());
    QCOMPARE(service->job()["state"].toString(), QString("succeeded"));
    QCOMPARE(QImage(output + "/from-mcp.png").pixelColor(0, 0),
             QColor(Qt::cyan));
    service->setMcpEnabled(false);
    QVERIFY(!service->mcpEnabled());
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
      QTRY_VERIFY(item("conversionInputRoot"));
      item("conversionInputRoot")->setProperty("text", small_);
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
#ifdef FORMAVIEW_IMAGE_UI
    auto *service = qobject_cast<ImageJobs *>(desktop_->imageJobs());
    QVERIFY(service && !service->mcpEnabled());
    const auto scope = service->scope();
    QVERIFY2(scope.value("output_roots").toList().size() == 1,
             "A successfully configured scope is required before UI settings "
             "checks");
    const auto output = scope.value("output_roots").toList()[0].toString();
    const auto python = qEnvironmentVariable("FORMAVIEW_TEST_MCP_PYTHON");
    const auto adapter = qEnvironmentVariable("FORMAVIEW_TEST_MCP_ADAPTER");
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
    process.start(python, {qEnvironmentVariable("FORMAVIEW_TEST_MCP_CLIENT"),
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
  }
  void imageDirectorySelection() {
#ifdef FORMAVIEW_IMAGE_UI
    auto *service = qobject_cast<ImageJobs *>(desktop_->imageJobs());
    QVERIFY(service && !service->mcpEnabled());
    const auto directory = small_ + "/discovery";
    const auto listValue = [](const QVariant &value) {
      return value.metaType() == QMetaType::fromType<QJSValue>()
                 ? value.value<QJSValue>().toVariant().toList()
                 : value.toList();
    };
    QVERIFY(QDir().mkpath(directory + "/nested"));
    for (int i = 0; i < 105; ++i)
      QVERIFY(QFile::copy(
          small_ + "/唯一图片.png",
          directory +
              QString("/image-%1.png").arg(i, 3, 10, QLatin1Char('0'))));
    QVERIFY(QFile::copy(small_ + "/唯一图片.png",
                        directory + "/nested/inside.png"));
    const auto output = root_ + "/discovery-output";
    QVERIFY(QDir().mkpath(output));
    const auto click = [&](const char *name) {
      QTest::qWait(40); // Let changed delegate counts settle the scroll layout.
      auto *control = item(name);
      if (!control || !control->isEnabled() || !control->isVisible())
        return false;
      for (auto *parent = control->parentItem(); parent;
           parent = parent->parentItem()) {
        if (parent->metaObject()->indexOfProperty("contentY") < 0)
          continue;
        const auto y = control->mapToItem(parent, {0, 0}).y();
        if (y < 0 || y + control->height() > parent->height())
          parent->setProperty(
              "contentY",
              std::clamp(
                  parent->property("contentY").toDouble() + y -
                      (parent->height() - control->height()) / 2,
                  0.,
                  std::max(0., parent->property("contentHeight").toDouble() -
                                   parent->height())));
      }
      QTest::qWait(30);
      const auto point =
          control->mapToScene({control->width() / 2, control->height() / 2})
              .toPoint();
      if (!QRect(QPoint(0, 0), window_->size()).contains(point))
        return false;
      QSignalSpy clicked(control, SIGNAL(clicked()));
      QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, point);
      if (clicked.count() != 1)
        save(QString("missed-click-") + name);
      return clicked.count() == 1;
    };
    QVERIFY(click("convertImagesButton"));
    QTRY_VERIFY(item("conversionAdvancedSettings"));
    if (!item("conversionAdvancedSettings")->property("checked").toBool())
      QVERIFY(click("conversionAdvancedSettings"));
    QTRY_VERIFY(item("conversionInputRoot"));
    const auto closeConversion =
        qScopeGuard([popup = QPointer<QObject>(
                         window_->findChild<QObject *>("conversionWorkspace"))] {
          if (popup)
            QMetaObject::invokeMethod(popup, "close");
        });
    item("conversionInputRoot")->setProperty("text", directory);
    item("conversionOutputRoot")->setProperty("text", output);
    QVERIFY(click("discoverInputsToggle"));
    QTRY_VERIFY(item("discoverRecursive"));
    item("discoverRecursive")->setProperty("checked", true);
    QVERIFY(click("discoverImagesButton"));
    QTRY_VERIFY_WITH_TIMEOUT(
        !service->busy() && service->discovery().value("total").toInt() == 106,
        15000);
    QVERIFY2(service->error().isEmpty(), qPrintable(service->error()));
    QCOMPARE(service->discovery()["items"].toList().size(), 100);
    const auto snapshot = service->discovery()["id"].toString();
    QVERIFY(click("discoveryNext"));
    QCOMPARE(service->discoveryOffset(), 100);
    QCOMPARE(service->discovery()["items"].toList().size(), 6);
    QVERIFY(QFile::copy(small_ + "/唯一图片.png", directory + "/late.png"));
    QVERIFY(click("discoveryPrevious"));
    QCOMPARE(service->discovery()["id"].toString(), snapshot);
    QCOMPARE(service->discovery()["total"].toInt(), 106);
    auto *filter = item("discoveryFilter");
    QVERIFY(filter);
    const auto first =
        service->discovery()["items"].toList()[0].toMap()["input"].toString();
    filter->setProperty("text", QFileInfo(first).fileName());
    QTRY_COMPARE(item("discoveryList")->property("count").toInt(), 1);
    QVERIFY(click("selectDiscoveredPage"));
    QCOMPARE(listValue(item("conversionInputs")->property("selected")).size(),
             1);
    filter->setProperty("text", "");
    QTRY_COMPARE(item("discoveryList")->property("count").toInt(), 100);
    QVERIFY(click("selectDiscoveredPage"));
    QCOMPARE(listValue(item("conversionInputs")->property("selected")).size(),
             64);
    for (const auto &language :
         {QString("zh-CN"), QString("en"), QString("zh-TW")}) {
      desktop_->setLanguage(language);
      QTest::qWait(60);
      save("conversion-discovery-" + language);
    }
    desktop_->setLanguage("zh-CN");
    QVERIFY(click("useDiscoveredInputs"));
    const QPointer<QObject> dialog(
        window_->findChild<QObject *>("conversionWorkspace"));
    QVERIFY(dialog);
    QCOMPARE(listValue(dialog->property("inputs")).size(), 64);
    QVERIFY(QDir(output).entryList(QDir::Files).isEmpty());
    QTest::keyClick(window_, Qt::Key_Escape);
    QTRY_VERIFY(!dialog || !dialog->property("visible").toBool());
#endif
  }
  void approvedPageReview() {
#ifdef FORMAVIEW_IMAGE_UI
    auto *workspace = item("workspace");
    auto *session = qobject_cast<ImageSession *>(desktop_->image());
    auto *service = qobject_cast<ImageJobs *>(desktop_->imageJobs());
    QVERIFY(workspace && session && service);
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
          QVERIFY(window_->grabWindow().save(root_ + "/" + name + ".png"));
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
    service->preflight(cancelInputs, reviewScope,
       {{"target","png"}, {"png_compression",9}}, "cancel-{index}.{ext}", "skip");
    QTRY_VERIFY2_WITH_TIMEOUT(!service->busy() && service->plan().value("state") == "ready", qPrintable(service->error()), 30000);
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
    producer.start(QCoreApplication::applicationFilePath(), {"--recovery-producer", recoveryInput, recoveryOutput, recoverySignal});
    QTRY_VERIFY2_WITH_TIMEOUT(QFile::exists(recoverySignal), producer.readAll().constData(), 45000);
    QFile signal(recoverySignal);
    QVERIFY(signal.open(QIODevice::ReadOnly));
    const auto runningJob = QJsonDocument::fromJson(signal.readAll()).object();
    signal.close();
    QCOMPARE(runningJob.value("state").toString(), QString("running"));
    producer.kill();
    QTRY_COMPARE(producer.state(), QProcess::NotRunning);
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
    std::atomic<quint64> changed{0}, synchronized{0};
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
    const auto frame = connect(
        window_, &QQuickWindow::frameSwapped, this,
        [&] {
          if (pending && synchronized.load() == sequence) {
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
      std::sort(values.begin(), values.end());
      return QJsonObject{
          {"samples", static_cast<int>(values.size())},
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
    QSize decoded;
    for (auto i = 0; i < 800; ++i) {
      const auto id = QString::fromLatin1(
                          QUrl::toPercentEncoding(small_ + "/唯一图片.png")) +
                      "?version=" + QString::number(i);
      const auto image = provider.requestImage(id, &decoded, QSize(8192, 8192));
      QVERIFY(!image.isNull());
      QVERIFY(decoded.width() <= 160 && decoded.height() <= 120);
      QVERIFY(provider.cacheKiB() <= 32768);
    }
    evidence_["thumbnail_cache_after_800_revisions_kib"] = provider.cacheKiB();
    provider.clear();
    QCOMPARE(provider.cacheKiB(), 0);
    const auto invalid = provider.requestImage(
        QString::fromLatin1(QUrl::toPercentEncoding(small_ + "/missing.png")),
        &decoded, {});
    QVERIFY(invalid.isNull());
  }
  void nativeInputPerformance() {
#ifdef Q_OS_WIN
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
    auto pending = false;
    auto frameReady = false;
    auto previous = -1;
    std::atomic<quint64> changed{0}, synchronized{0};
    quint64 sequence{};
    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();
    QEventLoop feedbackLoop;
    QTimer feedbackTimeout;
    feedbackTimeout.setSingleShot(true);
    connect(&feedbackTimeout, &QTimer::timeout, &feedbackLoop,
            &QEventLoop::quit);
    auto stateConnection =
        connect(desktop_.get(), &Desktop::changed, this, [&] {
          if (pending && desktop_->currentIndex() != previous)
            changed.store(sequence);
        });
    auto syncConnection = connect(
        window_, &QQuickWindow::afterSynchronizing, window_,
        [&] { synchronized.store(changed.load()); }, Qt::DirectConnection);
    auto connection = connect(
        window_, &QQuickWindow::frameSwapped, this,
        [&] {
          if (pending && synchronized.load() == sequence) {
            samples.push_back(
                std::chrono::duration<double, std::milli>(Clock::now() - start)
                    .count());
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
    });
    // Native OS injection to QML selection repaint, not event-loop timer
    // latency.
    for (auto i = 0; i < 160; ++i) {
      if (i % 20 == 0)
        desktop_->refresh();
      previous = desktop_->currentIndex();
      frameReady = false;
      ++sequence;
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
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&](double p) {
      return samples[static_cast<size_t>(
                         std::ceil(p * static_cast<double>(samples.size()))) -
                     1];
    };
    evidence_["native_input"] = QJsonObject{
        {"samples", static_cast<int>(samples.size())},
        {"p95_ms", percentile(.95)},
        {"p99_ms", percentile(.99)},
        {"max_ms", samples.back()},
        {"boundary",
         "SendInput start to frameSwapped queued notification after selection "
         "generation synchronized; 8 concurrent directory rescans; includes "
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
  void modelFocusRestoreRails() {
    const QDir fixtures(qEnvironmentVariable("FORMAVIEW_MODEL_FIXTURES"));
    auto *session = qobject_cast<ModelSession *>(desktop_->model());
    auto *workspace = item("workspace");
    QVERIFY(session && workspace && fixtures.exists("external.gltf"));
    workspace->setProperty("focusMode", false);
    desktop_->setLanguage("zh-CN");
    desktop_->preference("filesVisible", true);
    desktop_->preference("inspectorMode", "auto");
    desktop_->openPath(fixtures.filePath("external.gltf"));
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(), 10000);
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
        QCOMPARE(item("focusFilesRail")->width(), 36.0);
        QCOMPARE(item("focusInspectorRail")->width(), 36.0);
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
  }
  void modelDisplayControls() {
    const QDir fixtures(qEnvironmentVariable("FORMAVIEW_MODEL_FIXTURES"));
    auto *session=qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session && fixtures.exists("external.gltf"));
    desktop_->setLanguage("zh-CN");
    desktop_->openPath(fixtures.filePath("external.gltf"));
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),10000);
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
    QTest::failOnWarning(QRegularExpression(".*(ReferenceError|TypeError|Binding loop detected).*"));
    auto *session=qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    const auto restore=qScopeGuard([&] { desktop_->clearAsset(); });
    desktop_->setLanguage("zh-CN");
    window_->resize(1600,900);
    desktop_->openPath("B:/Model/克劳德/SKM_Cloud.fbx");
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),30000);
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
  }
  void modelSkeletonNavigation() {
    QTest::failOnWarning(QRegularExpression(".*(ReferenceError|TypeError|Binding loop detected|Unable to assign).*"));
    auto *session=qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    window_->resize(1600,900);
    desktop_->openPath("B:/Model/克劳德/SKM_Cloud.fbx");
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),30000);
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
    const auto shot=window_->grabWindow();
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
  }
  void modelProfessionalUi() {
    QTest::failOnWarning(QRegularExpression(".*(Binding loop detected|invalid context|Qt.callLater).*"));
    const QDir fixtures(qEnvironmentVariable("FORMAVIEW_MODEL_FIXTURES"));
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
  }
  void modelLodNativeFlow() {
    QTest::failOnWarning(QRegularExpression(".*(ReferenceError|TypeError|Binding loop detected|invalid context).*"));
    auto *session=qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    const auto cleanup=qScopeGuard([&]{desktop_->clearAsset();});
    const QString path="E:/CodexProject/asset-preview-planning/development/toolchain/M02/ufbx/data/maya_lod_group_7500_ascii.fbx";
    desktop_->openPath(path);
    QTRY_COMPARE_WITH_TIMEOUT(session->path(),path,10000);
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),30000);
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
    QVERIFY(session->frame()->vertices.empty()); // No source collision: never invent PhysicsAsset.
    save("lod-native-no-exported-collision");
    session->setCollisionMode(0);
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),10000);
    click("modelLodLevel0");
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable() && session->lodLevels()[0].toMap().value("current").toBool(),10000);
    QVERIFY(session->frame()!=levelFrame);
    QVERIFY(!session->frame()->vertices.empty());
    evidence_["lod_native_flow"]=QJsonObject{{"source",path},{"info",QJsonObject::fromVariantMap(session->info())},{"levels",QJsonArray::fromVariantList(session->lodLevels())},{"boundary","Actual QML buttons/ComboBox driven by Qt window mouse/key events; not OS input latency."}};
    desktop_->clearAsset();
  }
  void modelMultipleBindingsNativeFlow() {
    auto *session=qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    const auto path=QDir::fromNativeSeparators(qEnvironmentVariable("FORMAVIEW_MULTIBINDING_FIXTURE"));
    if(path.isEmpty()) QSKIP("Optional two-binding FBX fixture was not configured");
    QVERIFY2(QFileInfo::exists(path),"Set FORMAVIEW_MULTIBINDING_FIXTURE to the independently generated two-skin FBX fixture");
    const auto cleanup=qScopeGuard([&]{desktop_->clearAsset();});
    desktop_->openPath(path);
    QTRY_COMPARE_WITH_TIMEOUT(session->path(),path,10000);
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),30000);
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
    QVERIFY(session->frame()->heat);
    const auto secondHeat=session->frame();
    choose(Qt::Key_Home);
    QTRY_VERIFY_WITH_TIMEOUT(session->inspectable() && session->bindingIndex()==0,10000);
    QVERIFY(session->frame()->heat && session->frame()!=secondHeat);
    QCOMPARE(session->frame()->vertices.size(),secondHeat->vertices.size());
    QVERIFY(std::abs(session->frame()->vertices[0].weight-secondHeat->vertices[0].weight)>.5F);
    evidence_["multiple_bindings_native"]=QJsonObject{{"source",path},{"first_id",firstId},{"skin_count",2},{"boundary","Actual Qt QML ComboBox selection and heat frames; fixture is an explicitly hand-modified FBX, not claimed as a DCC export."}};
    desktop_->clearAsset();
  }
  void modelMixedQueuePerformance() {
#ifdef FORMAVIEW_IMAGE_UI
    auto *session=qobject_cast<ModelSession *>(desktop_->model());
    auto *service=qobject_cast<ImageJobs *>(desktop_->imageJobs());
    QVERIFY(session && service);
    const auto path=QDir::fromNativeSeparators(qEnvironmentVariable("FORMAVIEW_PERFORMANCE_MODEL"));
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
    std::sort(samples.begin(),samples.end());
    const auto p95=samples[227],p99=samples[237];
    evidence_["model_mixed_queue"]=QJsonObject{{"samples",240},{"while_job_active",concurrent},{"p95_ms",p95},{"p99_ms",p99},{"max_ms",samples.back()},{"max_main_thread_event_ms",measured->maxEventMs},{"main_thread_over_100ms",measured->over100ms},{"job",QJsonObject::fromVariantMap(service->job())},{"boundary","Qt camera command to first frameSwapped after matching synchronization; actual image conversion queue; not OS input or physical display presentation."}};
    QVERIFY(concurrent>=30); QVERIFY(p95<=50 && p99<=100); QCOMPARE(measured->over100ms,0);
    desktop_->clearAsset();
#endif
  }
  void modelLoadResourceStress() {
    auto *session=qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    const QString large="B:/Model/克劳德/SKM_Cloud.fbx";
    const QString compactModel="E:/CodexProject/asset-preview-planning/development/toolchain/M02/psk_psa_py/tests/data/psk/Suzanne.psk";
    auto *measured=static_cast<MeasuredApplication *>(QCoreApplication::instance());
    measured->events=0; measured->maxEventMs=0; measured->over100ms=0; measured->slowEvents={};
    measured->measure=true;
    const auto stop=qScopeGuard([&]{measured->measure=false;desktop_->clearAsset();});
    QElapsedTimer elapsed; elapsed.start();
    QJsonArray cycles;
    const auto configuredCycles=qEnvironmentVariable("FORMAVIEW_STRESS_CYCLES");
    const auto cycleCount=configuredCycles.isEmpty()?60:configuredCycles.toInt();
    QVERIFY(cycleCount>=60 && cycleCount<=600);
    for(int cycle=0;cycle<cycleCount;++cycle) {
      desktop_->openPath(large);
      if(cycle%3==0) {
        session->cancel();
        QVERIFY(!session->busy() && !session->frame());
        desktop_->openPath(compactModel);
      } else if(cycle%3==1) desktop_->openPath(compactModel);
      QTRY_VERIFY_WITH_TIMEOUT(session->inspectable(),30000);
      QCOMPARE(session->path(),cycle%3==2?large:compactModel);
      QTRY_VERIFY(window_->findChild<ModelView *>("modelViewport"));
      QPointer<ModelView> view=window_->findChild<ModelView *>("modelViewport");
      QTRY_VERIFY(view->isVisible());
      for(int i=0;i<10;++i) view->orbit(i%2?-.5:.5,.1);
      QTest::qWait(500);
      desktop_->clearAsset();
      QTRY_VERIFY(!view);
      QTest::qWait(20);
      QVERIFY(!session->busy() && !session->available() && !session->frame());
      const auto memory=formaview::process_memory();
      QVERIFY(memory);
      sampleGpu();
      cycles.append(QJsonObject{{"cycle",cycle},{"elapsed_ms",elapsed.elapsed()},{"rss",static_cast<qint64>(memory->working_set_bytes)},{"private",static_cast<qint64>(memory->private_bytes)},{"gpu",gpuMemory()}});
    }
    evidence_["model_load_resource_stress"]=QJsonObject{{"cycles",cycles},{"camera_commands",cycleCount*10},{"events",measured->events},{"max_event_ms",measured->maxEventMs},{"over_100ms",measured->over100ms},{"slow_events",measured->slowEvents},{"boundary","Repeated actual desktop load/cancel/supersede/render/destroy cycles; cycle count is recorded in cycles. Qt input, not OS input. Main-thread event dispatch timing only; process allocators/driver caches need not return RSS to initial baseline."}};
    QCOMPARE(measured->over100ms,0);
  }
  void modelPreviewIntegration() {
    QTest::failOnWarning(QRegularExpression(".*index is not valid.*"));
    auto *session = qobject_cast<ModelSession *>(desktop_->model());
    QVERIFY(session);
    const QStringList samples{
        "B:/Model/克劳德/SKM_Cloud.fbx",
        "E:/CodexProject/asset-preview-planning/development/toolchain/M02/psk_psa_py/tests/data/psk/Suzanne.psk",
        "E:/CodexProject/asset-preview-planning/development/toolchain/M02/psk_psa_py/tests/data/psk/CS_Sarge_S0_Skelmesh.pskx"};
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
      QVERIFY(window_->grabWindow().save(root_ + "/model-" + name + ".png"));
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
        QVERIFY(window_->grabWindow().save(root_ + "/uv-" + name + ".png"));
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
      QVERIFY(window_->grabWindow().save(root_ + "/texture-" + name + ".png"));
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
  }
#endif
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
    const QSet<QString> allowed{"settingsDirtyLeaveCallbacks", "conversionEditorPlanBinding", "imageJobsCancelAndShutdown", "folderMenuIsolationAndPaths", "folderNavigationInteraction"};
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
    const auto wait = [](auto predicate) {
      QElapsedTimer deadline; deadline.start();
      while (!predicate() && deadline.elapsed() < 35000) QTest::qWait(10);
      return predicate();
    };
    service.preflight(inputs, {{"input_roots",QStringList{input}}, {"output_roots",QStringList{output}}, {"allow_overwrite",false}}, {{"target","png"}, {"png_compression",9}}, "recovery-{index}.{ext}", "skip");
    if (!wait([&] { return !service.busy() && service.plan().value("state") == "ready"; })) return 21;
    service.submit(true);
    if (!wait([&] { return !service.busy() && service.job().value("state") == "running"; })) return 22;
    QFile ready(app.arguments()[4]);
    if (!ready.open(QIODevice::WriteOnly)) return 23;
    ready.write(QJsonDocument(QJsonObject::fromVariantMap(service.job())).toJson());
    ready.close();
    return app.exec();
  }
#endif
  DesktopTests tests;
  return QTest::qExec(&tests, argc, argv);
}
#include "desktop_tests.moc"
