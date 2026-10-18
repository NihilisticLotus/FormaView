#include "desktop.hpp"
#include "formaview/telemetry.hpp"
#include "thumbnail.hpp"
#include <QClipboard>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <atomic>
#include <chrono>
#include <cmath>
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
    if (!measure || QThread::currentThread() != thread())
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
    if (ms > 10 && slowEvents.size() < 128)
      slowEvents.append(
          QJsonObject{{"ms", ms},
                      {"type", static_cast<int>(event->type())},
                      {"receiver", receiver->metaObject()->className()}});
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
    QVERIFY(window_->grabWindow().save(root_ + "/" + name + ".png"));
    sampleGpu();
  }
private slots:
  void initTestCase() {
    root_ = qEnvironmentVariable("FORMAVIEW_UI_TEST_ROOT");
    QVERIFY(!root_.isEmpty());
    QVERIFY(QDir().mkpath(root_));
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
#ifdef Q_OS_WIN
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
#endif
    window_->show();
    QVERIFY(QTest::qWaitForWindowExposed(window_));
    QTRY_VERIFY(!renderLuid_.isEmpty());
    initializeGpuSampler();
    connect(&gpuSampler_, &QTimer::timeout, this, &DesktopTests::sampleGpu);
    gpuSampler_.start(250);
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
    QMetaObject::invokeMethod(tasks, "clicked");
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
    QTRY_VERIFY(std::abs(list->property("contentY").toDouble() - 456) < 2);
    auto *workspace = item("workspace");
    QVERIFY(workspace);
    const auto session = desktop_->sessionRevision();
    QMetaObject::invokeMethod(workspace, "toggleFiles");
    QTest::qWait(30);
    QMetaObject::invokeMethod(workspace, "toggleFiles");
    QTest::qWait(60);
    QCOMPARE(desktop_->sessionRevision(), session);
    QTRY_VERIFY(std::abs(list->property("contentY").toDouble() - 456) < 2);
    desktop_->preference("thumbnails", true);
    QTest::qWait(60);
    auto *grid = item("fileGrid");
    QVERIFY(grid);
    QTRY_VERIFY(std::abs(grid->property("contentY").toDouble() - 696) < 2);
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
    QVERIFY(address);
    QTRY_VERIFY(address->hasActiveFocus());
    QTest::keyClick(window_, Qt::Key_Escape);
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(small_ + "/唯一图片.png")});
    QDragEnterEvent enter(QPoint(600, 400), Qt::CopyAction, &mime,
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
  void cleanupTestCase() {
    if (const auto mem = formaview::process_memory()) {
      evidence_["working_set_bytes"] =
          static_cast<qint64>(mem->working_set_bytes);
      evidence_["peak_working_set_bytes"] =
          static_cast<qint64>(mem->peak_working_set_bytes);
    }
    gpuSampler_.stop();
    evidence_["gpu_observed_peak_local_bytes"] = peakGpuLocal_;
    evidence_["gpu_sampling"] =
        "DXGI per-process accounting on a retained actual QQuickWindow D3D11 "
        "adapter, "
        "250ms plus screenshots; transient peaks between samples may be "
        "missed; other enumerated adapters are not summed";
    engine_.reset();
    desktop_.reset();
    QCoreApplication::processEvents();
    QTest::qWait(100);
    evidence_["gpu_after_release"] = gpuMemory();
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
  QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
  QImageReader::setAllocationLimit(32);
  MeasuredApplication app(argc, argv);
  auto font = QFont{};
  font.setFamilies({"Segoe UI", "Microsoft YaHei UI"});
  font.setPixelSize(14);
  app.setFont(font);
  QCoreApplication::setOrganizationName("FormaViewTests");
  QCoreApplication::setApplicationName("Desktop");
  DesktopTests tests;
  return QTest::qExec(&tests, argc, argv);
}
#include "desktop_tests.moc"
