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
#include <deque>
#include <mutex>
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
    flush();
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
            {"released", observation.released}};
        if (state->readyRevision &&
            observation.frame_revision == state->readyRevision)
          state->renderedReadyRevision = observation.frame_revision;
      };
      view->update();
    }
#endif
  }
  void sample() {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    FILETIME creation{}, exit{}, kernel{}, user{};
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
  void flush() {
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
          {"gpu", state_->gpu}};
    }
    if (window_) {
      const auto geometry = window_->geometry();
      RECT client{}, native{};
      GetClientRect(reinterpret_cast<HWND>(window_->winId()), &client);
      GetWindowRect(reinterpret_cast<HWND>(window_->winId()), &native);
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
    result["image_sampling"] = imageSample();
    result["window_events"] = windowEvents_;
    result["pixel_events"] = pixelEvents_;
    QDir().mkpath(QFileInfo(path_).absolutePath());
    QSaveFile file(path_);
    const auto bytes = QJsonDocument(result).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
        !file.commit())
      qWarning() << "Cannot save desktop trace" << path_ << file.errorString();
  }

private:
  QPointer<QQuickWindow> window_;
  HWND handle_{};
  Desktop *desktop_;
  QString path_;
  std::shared_ptr<Shared> state_;
  QTimer timer_, views_;
  int ticks_{};
  QJsonArray resources_;
  QJsonArray windowEvents_, pixelEvents_;
  std::vector<Input> dispatchStack_;
};

DesktopTrace::DesktopTrace(QQuickWindow *window, Desktop *desktop, QString path,
                           Clock::time_point start)
    : impl_(std::make_unique<Impl>(this, window, desktop, std::move(path),
                                   start)) {}
DesktopTrace::~DesktopTrace() = default;
void DesktopTrace::flush() { impl_->flush(); }
void DesktopTrace::beginDispatch(QObject *receiver, QEvent *event) {
  impl_->beginDispatch(receiver, event);
}
void DesktopTrace::endDispatch() { impl_->endDispatch(); }
