#include "desktop_trace.hpp"
#include "desktop.hpp"
#ifdef FORMAVIEW_IMAGE_UI
#include "image_session.hpp"
#endif
#ifdef FORMAVIEW_MODEL_UI
#include "model_view.hpp"
#endif
#include <QAbstractNativeEventFilter>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QInputEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSaveFile>
#include <QScreen>
#include <QTimer>
#include <QtCore/qt_windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <map>
#include <psapi.h>

namespace {
using Clock = std::chrono::steady_clock;
double ms(Clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}
QJsonObject distribution(std::vector<double> values) {
  if (values.empty())
    return {{"count", 0}};
  std::sort(values.begin(), values.end());
  const auto percentile = [&](double p) {
    return values[static_cast<std::size_t>(std::ceil(p * values.size())) - 1];
  };
  return {{"count", static_cast<qint64>(values.size())},
          {"p95_ms", percentile(.95)},
          {"p99_ms", percentile(.99)},
          {"max_ms", values.back()}};
}
struct Input {
  quint64 sequence{};
  UINT message{};
  Clock::time_point native, received;
  DWORD timestamp{};
  quint64 cameraCommand{};
};
struct Shared {
  std::mutex mutex;
  Clock::time_point start;
  quint64 next{}, synchronized{}, frames{}, dropped{};
  quint64 lastCameraSequence{};
  quint64 cameraCommands{}, cameraSynchronized{};
  std::deque<Input> pending;
  std::deque<Input> cameraPending;
  Input lastInput;
  Input dispatchInput;
  QJsonArray inputs, cameraInputs, loads;
  std::vector<double> latencies, cameraLatencies, intervals;
  Clock::time_point lastSwap;
  double firstSwap{};
  QString loadingPath;
  Clock::time_point loadingStart;
  quint64 readyRevision{}, renderedReadyRevision{};
  bool loading{};
  bool loadReported{};
  QJsonObject gpu;
  quint64 gpuPoseCount{};
  std::map<QString,QJsonArray> animationPresents;
  QHash<QString,qint64> lastPose;
};
} // namespace

class DesktopTrace::Impl final : public QAbstractNativeEventFilter {
public:
  Impl(DesktopTrace *owner, QQuickWindow *window, Desktop *desktop,
       QString path, Clock::time_point start)
      : window_(window), desktop_(desktop), path_(std::move(path)),
        state_(std::make_shared<Shared>()) {
    state_->start = start;
    // winId() can create the platform window. Never call it from inside a
    // native filter while CreateWindowEx is itself dispatching messages.
    handle_ = reinterpret_cast<HWND>(window->winId());
    qGuiApp->installNativeEventFilter(this);
#ifdef FORMAVIEW_IMAGE_UI
    if (auto *image = qobject_cast<ImageSession *>(desktop_->image()))
      QObject::connect(image, &ImageSession::pixelChanged, owner, [this] {
        if (pixelEvents_.size() < 2000)
          pixelEvents_.append(QJsonObject{
              {"elapsed_ms", ms(Clock::now() - state_->start)},
              {"sample", imageSample()}});
      });
#endif
    QObject::connect(
        window, &QQuickWindow::afterSynchronizing, owner,
        [state = state_] {
          std::lock_guard lock(state->mutex);
          state->synchronized = state->next;
          state->cameraSynchronized = state->cameraCommands;
        },
        Qt::DirectConnection);
    QObject::connect(
        window, &QQuickWindow::frameSwapped, owner,
        [state = state_] {
          const auto now = Clock::now();
          std::lock_guard lock(state->mutex);
          ++state->frames;
          if (!state->firstSwap)
            state->firstSwap = ms(now - state->start);
          if (state->lastSwap.time_since_epoch().count() &&
              state->intervals.size() < 120000)
            state->intervals.push_back(ms(now - state->lastSwap));
          state->lastSwap = now;
          for(auto it=state->gpu.constBegin();it!=state->gpu.constEnd();++it) {
            const auto sample=it.value().toObject();const auto revision=sample.value("revision").toInteger();
            if(sample.value("released").toBool() || sample.value("clip").toInt(-1)<0 || state->lastPose.value(it.key(),-1)==revision)continue;
            state->lastPose[it.key()]=revision;
            if (sample.value("gpu_skinning").toBool()) ++state->gpuPoseCount;
            auto &records=state->animationPresents[it.key()];
            if(records.size()<20000){auto record=sample;record["swap_elapsed_ms"]=ms(now-state->start);records.append(record);}
          }
          if (!state->loadReported && state->readyRevision &&
              state->renderedReadyRevision == state->readyRevision) {
            state->loads.append(QJsonObject{
                {"path", state->loadingPath},
                {"first_observed_matching_swap_ms",
                 ms(now - state->loadingStart)},
                {"revision", static_cast<qint64>(state->readyRevision)}});
            state->loadReported = true;
          }
          while (!state->pending.empty() &&
                 state->pending.front().sequence <= state->synchronized) {
            const auto input = state->pending.front();
            state->pending.pop_front();
            const auto latency = ms(now - input.native);
            state->latencies.push_back(latency);
            state->inputs.append(QJsonObject{
                {"sequence", static_cast<qint64>(input.sequence)},
                {"message", static_cast<int>(input.message)},
                {"received_ms", ms(input.received - state->start)},
                {"os_queue_age_ms", ms(input.received - input.native)},
                {"swap_ms", ms(now - state->start)},
                {"native_to_swap_ms", latency}});
          }
          while (!state->cameraPending.empty() &&
                 state->cameraPending.front().cameraCommand <=
                     state->cameraSynchronized) {
            const auto input = state->cameraPending.front();
            state->cameraPending.pop_front();
            const auto latency = ms(now - input.native);
            state->cameraLatencies.push_back(latency);
            state->cameraInputs.append(QJsonObject{
                {"sequence", static_cast<qint64>(input.sequence)},
                {"camera_command", static_cast<qint64>(input.cameraCommand)},
                {"message", static_cast<int>(input.message)},
                {"received_ms", ms(input.received - state->start)},
                {"swap_ms", ms(now - state->start)},
                {"native_to_swap_ms", latency}});
          }
        },
        Qt::DirectConnection);
#ifdef FORMAVIEW_MODEL_UI
    auto *model = qobject_cast<ModelSession *>(desktop_->model());
    // This object exists only under --trace. The second gate ensures ordinary
    // traced/manual sessions never play, mutate models, or quit automatically.
    soakEnabled_ = qEnvironmentVariable("FORMAVIEW_PRODUCT_SOAK") == "1";
    if (soakEnabled_) {
      bool ok{};
      const int seconds = qEnvironmentVariableIntValue("FORMAVIEW_PRODUCT_SOAK_SECONDS", &ok);
      soakSeconds_ = ok && seconds > 0 && seconds <= 3600 ? seconds : 300;
      soakPhase_ = "loading";
      if (model) model->setAnimationReference(qEnvironmentVariable("FORMAVIEW_PRODUCT_SOAK_REFERENCE"));
    }
    if (model)
      QObject::connect(model, &ModelSession::changed, owner, [this, model] {
        std::lock_guard lock(state_->mutex);
        if (model->busy() &&
            (!state_->loading || state_->loadingPath != model->path())) {
          state_->loadingPath = model->path();
          state_->loadingStart = Clock::now();
          state_->loadReported = false;
          state_->readyRevision = 0;
          state_->renderedReadyRevision = 0;
        }
        state_->loading = model->busy();
        if (model->inspectable() && model->frame())
          state_->readyRevision = model->frame()->revision;
        else
          state_->readyRevision = 0;
      });
#endif
    QObject::connect(&timer_, &QTimer::timeout, owner, [this] {
      driveSoak();
      sample();
      if (++ticks_ % 5 == 0)
        flush();
    });
    timer_.start(1000);
    // Discover asynchronously-created viewports before most imports finish.
    QObject::connect(&views_, &QTimer::timeout, owner, [this] { bindViews(); });
    views_.start(25);
    bindViews();
    sample();
  }
  ~Impl() override {
    qGuiApp->removeNativeEventFilter(this);
    timer_.stop();
    views_.stop();
    if (!finalized_) flush(true);
  }
  bool nativeEventFilter(const QByteArray &, void *message,
                         qintptr *) override {
    const auto *event = static_cast<MSG *>(message);
    if (!window_ || event->hwnd != handle_)
      return false;
    if (windowEvents_.size() < 2000 &&
        (event->message == WM_LBUTTONDOWN || event->message == WM_LBUTTONUP ||
         event->message == WM_NCLBUTTONDOWN || event->message == WM_NCLBUTTONUP ||
         event->message == WM_NCLBUTTONDBLCLK || event->message == WM_SYSCOMMAND ||
         event->message == WM_SIZE)) {
      POINT client = event->pt;
      ScreenToClient(handle_, &client);
      windowEvents_.append(QJsonObject{
          {"elapsed_ms", ms(Clock::now() - state_->start)},
          {"message", static_cast<int>(event->message)},
          {"wparam", static_cast<qint64>(event->wParam)},
          {"screen_x", static_cast<int>(event->pt.x)},
          {"screen_y", static_cast<int>(event->pt.y)},
          {"client_x", static_cast<int>(client.x)},
          {"client_y", static_cast<int>(client.y)},
          {"width", window_->width()}, {"height", window_->height()},
          {"dpr", window_->devicePixelRatio()}, {"sample", imageSample()}});
    }
    const bool moving =
        event->message == WM_MOUSEMOVE &&
        (event->wParam & (MK_LBUTTON | MK_MBUTTON | MK_RBUTTON));
    const bool button =
        event->message == WM_LBUTTONDOWN || event->message == WM_LBUTTONUP ||
        event->message == WM_RBUTTONDOWN || event->message == WM_RBUTTONUP ||
        event->message == WM_MBUTTONDOWN || event->message == WM_MBUTTONUP;
    if (!moving && !button && event->message != WM_MOUSEWHEEL &&
        event->message != WM_KEYDOWN && event->message != WM_KEYUP &&
        event->message != WM_SYSKEYDOWN)
      return false;
    const auto age = static_cast<DWORD>(GetTickCount64()) - event->time;
    if (age > 5000 || !event->time)
      return false;
    const auto now = Clock::now();
    std::lock_guard lock(state_->mutex);
    if (state_->next >= 20000 || state_->pending.size() >= 2048) {
      ++state_->dropped;
      return false;
    }
    state_->pending.push_back({++state_->next, event->message,
                               now - std::chrono::milliseconds(age), now,
                               event->time});
    state_->lastInput = state_->pending.back();
    return false;
  }
  void beginDispatch(QObject *receiver, QEvent *event) {
    std::lock_guard lock(state_->mutex);
    dispatchStack_.push_back(state_->dispatchInput);
    const auto type = event->type();
    if (type != QEvent::MouseMove && type != QEvent::Wheel &&
        type != QEvent::KeyPress)
      return;
    state_->dispatchInput = {};
    const auto &input = state_->lastInput;
    const bool matching =
        (type == QEvent::MouseMove && input.message == WM_MOUSEMOVE) ||
        (type == QEvent::Wheel && input.message == WM_MOUSEWHEEL) ||
        (type == QEvent::KeyPress && input.message == WM_KEYDOWN);
    const auto *item = qobject_cast<QQuickItem *>(receiver);
    const bool owned =
        receiver == window_ || (item && item->window() == window_);
    if (owned && matching &&
        static_cast<DWORD>(static_cast<QInputEvent *>(event)->timestamp()) ==
            input.timestamp)
      state_->dispatchInput = input;
  }
  void endDispatch() {
    std::lock_guard lock(state_->mutex);
    state_->dispatchInput = dispatchStack_.back();
    dispatchStack_.pop_back();
  }
  void bindViews() {
#ifdef FORMAVIEW_MODEL_UI
    if (!window_)
      return;
    for (auto *view : window_->findChildren<ModelView *>()) {
      if (view->property("traceBound").toBool())
        continue;
      view->setProperty("traceBound", true);
      QObject::connect(view, &ModelView::cameraChanged, &views_,
                       [state = state_](const QVariantMap &) {
                         std::lock_guard lock(state->mutex);
                         auto input = state->dispatchInput;
                         if (input.sequence &&
                             (input.message == WM_MOUSEMOVE ||
                              input.message == WM_MOUSEWHEEL ||
                              input.message == WM_KEYDOWN) &&
                             state->cameraInputs.size() < 20000 &&
                             state->lastCameraSequence != input.sequence) {
                           input.cameraCommand = ++state->cameraCommands;
                           state->cameraPending.push_back(input);
                           state->lastCameraSequence = input.sequence;
                         }
                       });
      const auto name = view->objectName();
      view->observe = [state = state_,
                       name](formaview::render::Observation observation) {
        std::lock_guard lock(state->mutex);
        state->gpu[name] = QJsonObject{
            {"owned_bytes", static_cast<qint64>(observation.owned_gpu_bytes)},
            {"process_bytes",
             static_cast<qint64>(observation.process_gpu_bytes)},
            {"revision", static_cast<qint64>(observation.frame_revision)},
            {"released", observation.released},
            {"clip",observation.clip},{"seconds",observation.seconds},{"gpu_skinning",observation.gpu_skinning},
            {"uploaded_bytes",static_cast<qint64>(observation.uploaded_bytes)},
            {"prepare_ms",observation.prepare_ms},{"record_ms",observation.record_ms},{"gpu_seconds",observation.gpu_seconds}};
        if (state->readyRevision &&
            observation.frame_revision == state->readyRevision)
          state->renderedReadyRevision = observation.frame_revision;
      };
      view->update();
    }
#endif
  }
  void soakAction(const QString &type, QJsonObject details = {}) {
    details["type"] = type;
    details["elapsed_ms"] = ms(Clock::now() - state_->start);
    soakActions_.append(details);
  }
  void driveSoak() {
#ifdef FORMAVIEW_MODEL_UI
    if (!soakEnabled_ || soakFinished_) return;
    auto *model = qobject_cast<ModelSession *>(desktop_->model());
    const auto now = Clock::now();
    const auto fail = [&](const QString &reason) {
      soakError_ = reason; soakPhase_ = "failed"; soakFinished_ = true;
      soakAction("failure", {{"reason", reason}});
      sample(); flush(); QCoreApplication::exit(2);
    };
    if (!model) { fail("model session unavailable"); return; }
    if (!model->error().isEmpty() || !model->frameError().isEmpty()) {
      fail(model->error() + " " + model->frameError()); return;
    }
    if (soakPhase_ == "loading") {
      if (ms(now - state_->start) > 240000) { fail("model readiness timeout"); return; }
      if (!model->inspectable() || model->externalBusy()) return;
      if (!soakAttached_) {
        soakAttached_ = true;
        const auto paths = qEnvironmentVariable("FORMAVIEW_PRODUCT_SOAK_EXTERNALS").split(';', Qt::SkipEmptyParts);
        for (const auto &path : paths) model->attachExternalAnimation(path);
        if (!paths.empty()) { soakAction("attach", {{"requested", paths.size()}}); return; }
      }
      if (!model->externalInfo().value("failedFiles").toStringList().isEmpty()) {
        fail("external animation failed"); return;
      }
      if (!model->animated()) { fail("ready model has no animation"); return; }
      if (qEnvironmentVariable("FORMAVIEW_PRODUCT_SOAK_PAIR_MESH") == "1") {
        model->setPairMeshVisible(true);
        if (!model->pairMeshVisible()) { fail("paired mesh unavailable"); return; }
      }
      model->setLoop(true); model->setSpeed(1);
      if (!model->playing()) model->togglePlay();
      soakPhase_ = "warmup"; soakPhaseStart_ = now;
      soakAction("play", {{"path", model->path()}, {"clips", model->clips().size()}});
      return;
    }
    if (soakPhase_ == "warmup") {
      if (ms(now - soakPhaseStart_) < 5000) return;
      soakPhase_ = "playing"; soakPhaseStart_ = now;
      { std::lock_guard lock(state_->mutex); soakPoseStart_ = state_->gpuPoseCount; }
      soakAction("measurement_start");
    }
    if (soakPhase_ == "playing") {
      const double elapsed = ms(now - soakPhaseStart_);
      if (elapsed >= soakSeconds_ * 1000.0) {
        soakPlaybackMs_ = elapsed;
        { std::lock_guard lock(state_->mutex); soakPoseCount_ = state_->gpuPoseCount - soakPoseStart_; }
        model->clear(); soakPhase_ = "cleared"; soakPhaseStart_ = now;
        soakAction("clear"); return;
      }
      const int step = static_cast<int>(elapsed / 10000);
      if (step > soakStep_) {
        soakStep_ = step;
        model->setClipIndex((model->clipIndex() + 1) % model->clips().size());
        soakAction("clip", {{"index", model->clipIndex()}});
        const auto clip = model->clipInfo();
        model->seek((clip.value("start").toDouble() + clip.value("end").toDouble()) * .5);
        soakAction("seek", {{"time", model->time()}});
        if (step % 2 == 0) {
          constexpr double speeds[]{1, 2, .5, 1};
          model->setSpeed(speeds[(step / 2) % 4]);
          soakAction("speed", {{"value", model->speed()}});
        }
        if (step % 3 == 0) {
          model->setRootLock(!model->rootLockOn());
          soakAction("root_lock", QJsonObject::fromVariantMap(model->rootLockInfo()));
        }
      }
      return;
    }
    if (soakPhase_ == "cleared" && ms(now - soakPhaseStart_) >= 30000) {
      soakFinished_ = true; soakPhase_ = "complete";
      soakAction("complete"); sample(); flush(); QCoreApplication::quit();
    }
#endif
  }
  void sample() {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    FILETIME creation{}, exit{}, kernel{}, user{};
    DWORD handles{};
    const bool handlesOk = GetProcessHandleCount(GetCurrentProcess(), &handles);
    QJsonObject gpu;
    { std::lock_guard lock(state_->mutex); gpu = state_->gpu; }
    DWORD foregroundPid{};
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
    if (K32GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),
            sizeof(memory)) &&
        GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel,
                        &user) &&
        resources_.size() < 36000) {
      const auto ticks = [](FILETIME time) {
        return (quint64(time.dwHighDateTime) << 32) | time.dwLowDateTime;
      };
      resources_.append(QJsonObject{
          {"elapsed_ms", ms(Clock::now() - state_->start)},
          {"handles", handlesOk ? static_cast<qint64>(handles) : -1},
          {"gpu", gpu},
          {"phase", soakPhase_},
          {"foreground", GetForegroundWindow() == handle_},
          {"foreground_pid", static_cast<qint64>(foregroundPid)},
          {"minimized", bool(IsIconic(handle_))},
          {"working_set_bytes", static_cast<qint64>(memory.WorkingSetSize)},
          {"peak_working_set_bytes",
           static_cast<qint64>(memory.PeakWorkingSetSize)},
          {"private_bytes", static_cast<qint64>(memory.PrivateUsage)},
          {"cpu_ms", double(ticks(kernel) + ticks(user)) / 10000.0}});
    }
  }
  QJsonObject imageSample() const {
#ifdef FORMAVIEW_IMAGE_UI
    if (auto *image = qobject_cast<ImageSession *>(desktop_->image()))
      return {{"path", image->path()}, {"pinned", image->samplePinned()},
              {"pixel", QJsonObject::fromVariantMap(image->pixel())}};
#endif
    return {};
  }
  bool flush(bool final = false) {
    if (finalized_) return lastSaveOk_ && statusSaveOk_;
    QJsonObject result;
    {
      std::lock_guard lock(state_->mutex);
      result = {
          {"boundary",
           "Actual Windows MSG timestamps to first frameSwapped after scene "
           "synchronization; timestamp resolution approximately 1 ms, not "
           "photon/display scanout. All coalesced inputs are retained."},
          {"qt", qVersion()},
          {"pid", static_cast<qint64>(QCoreApplication::applicationPid())},
          {"frames", static_cast<qint64>(state_->frames)},
          {"first_shell_swap_ms", state_->firstSwap},
          {"input_count", static_cast<qint64>(state_->next)},
          {"dropped", static_cast<qint64>(state_->dropped)},
          {"pending", static_cast<qint64>(state_->pending.size())},
          {"inputs", state_->inputs},
          {"native_input_to_swap", distribution(state_->latencies)},
          {"camera_native_input_to_swap",
           distribution(state_->cameraLatencies)},
          {"camera_inputs", state_->cameraInputs},
          {"camera_pending", static_cast<qint64>(state_->cameraPending.size())},
          {"camera_boundary",
           "Only matching-timestamp native move/wheel/key inputs whose Qt "
           "event dispatch synchronously emitted "
           "ModelView::cameraChanged; synchronized before the reported swap. "
           "Raw inputs without a camera change remain in inputs and are not "
           "camera response samples."},
          {"frame_intervals", distribution(state_->intervals)},
          {"loads", state_->loads},
          {"model_ready_boundary",
           "First frameSwapped after an observed matching model render; "
           "viewport discovery adds up to 25 ms. OS disk caches are not "
           "flushed."},
          {"animation_presents",[&]{QJsonObject values;for(const auto &[name,records]:state_->animationPresents)values[name]=records;return values;}()},
          {"gpu", state_->gpu}};
    }
    if (window_) {
      const auto geometry = window_->geometry();
      RECT client{}, native{};
      // Diagnostics must not recreate a native window after it was closed.
      if (window_->handle() && IsWindow(handle_)) {
        GetClientRect(handle_, &client);
        GetWindowRect(handle_, &native);
      }
      result["geometry"] = QJsonObject{
          {"logical_x", geometry.x()},
          {"logical_y", geometry.y()},
          {"logical_width", geometry.width()},
          {"logical_height", geometry.height()},
          {"dpr", window_->devicePixelRatio()},
          {"native_client_width", static_cast<int>(client.right - client.left)},
          {"native_client_height",
           static_cast<int>(client.bottom - client.top)},
          {"native_window_width", static_cast<int>(native.right - native.left)},
          {"native_window_height",
           static_cast<int>(native.bottom - native.top)}};
      if (auto *screen = window_->screen())
        result["screen"] = QJsonObject{
            {"name", screen->name()},
            {"available_width", screen->availableGeometry().width()},
            {"available_height", screen->availableGeometry().height()}};
    }
    result["resources"] = resources_;
    result["soak"] = QJsonObject{{"enabled", soakEnabled_}, {"status", soakPhase_},
        {"requested_seconds", soakSeconds_}, {"error", soakError_},
        {"playback_elapsed_ms", soakPlaybackMs_},
        {"gpu_pose_count", static_cast<qint64>(soakPoseCount_)}};
    result["soak_actions"] = soakActions_;
    result["image_sampling"] = imageSample();
    result["window_events"] = windowEvents_;
    result["pixel_events"] = pixelEvents_;
    QDir().mkpath(QFileInfo(path_).absolutePath());
    QSaveFile file(path_);
    const auto bytes = QJsonDocument(result).toJson();
    ++saveAttempts_;
    lastSaveOk_ = file.open(QIODevice::WriteOnly) &&
        file.write(bytes) == bytes.size() && file.commit();
    if (lastSaveOk_) lastSuccessfulSaveMs_ = ms(Clock::now() - state_->start);
    else {
      ++saveFailures_;
      lastSaveError_ = file.errorString();
      qWarning() << "Cannot save desktop trace" << path_ << lastSaveError_;
    }
    // Independent atomic status file remains observable while a reader holds
    // the main trace without FILE_SHARE_DELETE. Never claim complete output
    // from a stale trace; preserve transient failures even after recovery.
    const QJsonObject status{{"version",1},{"trace",path_},
        {"pid",static_cast<qint64>(QCoreApplication::applicationPid())},
        {"final",final},{"lastSaveOk",lastSaveOk_},
        {"attempts",saveAttempts_},{"failures",saveFailures_},
        {"lastError",lastSaveError_},{"lastSuccessfulSaveMs",lastSuccessfulSaveMs_}};
    QSaveFile statusFile(path_ + ".save-status.json");
    const auto statusBytes = QJsonDocument(status).toJson();
    statusSaveOk_ = statusFile.open(QIODevice::WriteOnly) &&
        statusFile.write(statusBytes) == statusBytes.size() && statusFile.commit();
    if (!lastSaveOk_ || !statusSaveOk_) {
      const auto message = (QStringLiteral("FORMAVIEW_TRACE_SAVE_FAILED: ") + path_ +
          QStringLiteral("; trace=") + file.errorString() +
          QStringLiteral("; status=") + statusFile.errorString()).toUtf8();
      std::fprintf(stderr,"%s\n",message.constData()); std::fflush(stderr);
    }
    finalized_ = final;
    return lastSaveOk_ && statusSaveOk_;
  }

private:
  QPointer<QQuickWindow> window_;
  HWND handle_{};
  Desktop *desktop_;
  QString path_;
  bool finalized_{}, lastSaveOk_{}, statusSaveOk_{};
  qint64 saveAttempts_{}, saveFailures_{};
  double lastSuccessfulSaveMs_{-1};
  QString lastSaveError_;
  std::shared_ptr<Shared> state_;
  QTimer timer_, views_;
  int ticks_{};
  bool soakEnabled_{}, soakAttached_{}, soakFinished_{};
  int soakSeconds_{300}, soakStep_{};
  quint64 soakPoseStart_{}, soakPoseCount_{};
  double soakPlaybackMs_{};
  Clock::time_point soakPhaseStart_;
  QString soakPhase_{"disabled"}, soakError_;
  QJsonArray soakActions_;
  QJsonArray resources_;
  QJsonArray windowEvents_, pixelEvents_;
  std::vector<Input> dispatchStack_;
};

DesktopTrace::DesktopTrace(QQuickWindow *window, Desktop *desktop, QString path,
                           Clock::time_point start)
    : impl_(std::make_unique<Impl>(this, window, desktop, std::move(path),
                                   start)) {}
DesktopTrace::~DesktopTrace() = default;
bool DesktopTrace::flush(bool final) { return impl_->flush(final); }
void DesktopTrace::beginDispatch(QObject *receiver, QEvent *event) {
  impl_->beginDispatch(receiver, event);
}
void DesktopTrace::endDispatch() { impl_->endDispatch(); }
