#ifdef FORMAVIEW_VIDEO_TEST_MARKER
#include "video_test_marker.hpp"
#endif
#include "video_session.hpp"
#include "video_statistics.hpp"
#include "video_output.hpp"
#include <QQmlEngine>
#include <QScreen>
#ifdef FORMAVIEW_INDEXED_VIDEO
#include "video_frames.hpp"
#include "video_index.hpp"
#include "flv_seek_index.hpp"
#include <mpv/stream_cb.h>
#endif
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLibrary>
#include <QLoggingCategory>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>
#include <QtQuick/qsgtexture_platform.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <d3d11.h>
#include <mpv/client.h>
#include <mpv/render_d3d11.h>
#include <mutex>
#include <wrl/client.h>

namespace {
struct SavedWindowPlacement {
  WINDOWPLACEMENT native{sizeof(WINDOWPLACEMENT)};
  HMONITOR monitor{};
  UINT dpi{};
};
Q_LOGGING_CATEGORY(seekTrace, "formaview.video.seek", QtInfoMsg)
void traceSeek(const char *phase, double target) {
  if (seekTrace().isDebugEnabled())
    qCDebug(seekTrace).nospace() << phase << " t_us="
        << std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count()
        << " target=" << target;
}
struct Api {
  QLibrary library{QCoreApplication::applicationDirPath() + "/libmpv-2.dll"};
#define FV_MPV(X)                                                              \
  X(create)                                                                    \
  X(initialize)                                                                \
  X(set_option_string) X(get_property_async) X(get_property_string) X(free)     \
  X(observe_property) X(wait_event) X(command_async) X(terminate_destroy)      \
  X(hook_add) X(hook_continue) X(set_property_string)                         \
      X(error_string) X(render_context_create) X(render_context_render)        \
          X(render_context_update) X(render_context_get_info) X(render_context_free)                      \
              X(render_context_set_update_callback)
#define FIELD(n) decltype(&mpv_##n) n = nullptr;
  FV_MPV(FIELD)
#undef FIELD
  bool ready = false;
  Api() {
#define RESOLVE(n)                                                             \
  n = reinterpret_cast<decltype(n)>(library.resolve("mpv_" #n));               \
  if (!n)                                                                      \
    return;
    FV_MPV(RESOLVE)
#undef RESOLVE
    ready = true;
  }
};
Api &api() {
  static Api instance;
  return instance;
}
QVariant variant(const mpv_node &node) {
  switch (node.format) {
  case MPV_FORMAT_STRING:
    return QString::fromUtf8(node.u.string);
  case MPV_FORMAT_FLAG:
    return bool(node.u.flag);
  case MPV_FORMAT_INT64:
    return qint64(node.u.int64);
  case MPV_FORMAT_DOUBLE:
    return node.u.double_;
  case MPV_FORMAT_NODE_ARRAY: {
    QVariantList result;
    for (int i = 0; i < node.u.list->num; ++i)
      result.append(variant(node.u.list->values[i]));
    return result;
  }
  case MPV_FORMAT_NODE_MAP: {
    QVariantMap result;
    for (int i = 0; i < node.u.list->num; ++i)
      result.insert(QString::fromUtf8(node.u.list->keys[i]),
                    variant(node.u.list->values[i]));
    return result;
  }
  default:
    return {};
  }
}
} // namespace

struct VideoEngine {
#ifdef FORMAVIEW_INDEXED_VIDEO
  std::shared_ptr<VideoIndex> index;
  VideoStream stream;
  VideoSeekPlan plan;
  double warmAnchor=-100;
  double diagnosticFloor=0;
  std::unique_ptr<VideoFrames> worker;
#endif
  QVariantMap presentation;
  bool samplePresentation = false;
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  mpv_handle *handle = nullptr;
  bool loaded = false;
  std::mutex callbackLock;
  VideoSession *view = nullptr;
  ~VideoEngine() {
    if (handle)
      api().terminate_destroy(handle);
  }
};

namespace {
class Surface : public QSGSimpleTextureNode {
public:
  std::shared_ptr<VideoEngine> engine;
  mpv_render_context *renderer = nullptr;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> target;
  QSize size;
  bool pq=false;
  QSGSimpleTextureNode *overlay = nullptr;
  struct Cached {
    qint64 tick=-1;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> native;
    std::unique_ptr<QSGTexture> texture;
    bool sampled=false;
  };
  std::vector<Cached> history;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  quint64 epoch=0;
  void hideReview(){
    if(overlay){removeChildNode(overlay);delete overlay;overlay=nullptr;}
  }
  void invalidate(){hideReview();history.clear();staging.Reset();}
  void retain(QQuickWindow *win,ID3D11Device *device){
    if(pq || !target)return;
    Cached item;
    if(history.size()==8){
      item=std::move(history.front());history.erase(history.begin());
    }
    if(!item.native){
      D3D11_TEXTURE2D_DESC desc{};target->GetDesc(&desc);
      if(FAILED(device->CreateTexture2D(&desc,nullptr,&item.native)))return;
      item.texture.reset(videoOutput::importTexture(win,item.native.Get(),size,false));
      if(!item.texture)return;
    }
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    context->CopyResource(item.native.Get(),target.Get());
    item.tick=-1;item.sampled=false;history.push_back(std::move(item));
  }
#ifdef FORMAVIEW_INDEXED_VIDEO
  qint64 identify(ID3D11Device *device,Cached &candidate){
    if(!candidate.native||!engine->worker)return -1;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;device->GetImmediateContext(&ctx);
    if(!staging){
      D3D11_TEXTURE2D_DESC desc{};target->GetDesc(&desc);desc.Height=8;
      desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
      if(FAILED(device->CreateTexture2D(&desc,nullptr,&staging)))return -1;
    }
    auto area=QSize(3840,1632).scaled(size,Qt::KeepAspectRatio);
    int left=(size.width()-area.width())/2,top=(size.height()-area.height())/2;
    for(int y=0;y<8;++y){
      UINT row=UINT(top+(y+2)*area.height()/12);
      D3D11_BOX region{0,row,0,UINT(size.width()),row+1,1};
      ctx->CopySubresourceRegion(staging.Get(),0,0,UINT(y),0,candidate.native.Get(),0,&region);
    }
    D3D11_MAPPED_SUBRESOURCE map{};
    if(FAILED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&map)))return -1;
    VideoSignature sig{};
    for(int y=0;y<8;++y)for(int x=0;x<24;++x){
      auto *rgb=static_cast<const quint8*>(map.pData)+y*map.RowPitch+4*(left+(x+1)*area.width()/26);
      sig[24*y+x]=.2126f*rgb[0]+.7152f*rgb[1]+.0722f*rgb[2];
    }
    ctx->Unmap(staging.Get(),0);
    return engine->worker->identify(sig);
  }
#endif
  void showReview(QQuickWindow *win, ID3D11Device *device, qint64 tick,
                  const VideoReviewFrame *review, QRectF viewport, bool allowNative) {
    if (tick < 0 || !review) {
      hideReview();
      return;
    }
#ifdef FORMAVIEW_INDEXED_VIDEO
    if(!engine->worker)return;
    if(allowNative){
      for(auto &slot:history){
        if(slot.native&&!slot.sampled){slot.tick=identify(device,slot);slot.sampled=true;}
      }
    }
    auto it=std::find_if(history.begin(),history.end(),[&](const auto &x){return x.tick==tick && (!x.native||allowNative);});
    if(it==history.end()){
      auto image = VideoFrames::image(*review);
      if (image.isNull()) {
        hideReview();
        return;
      }
#ifdef FORMAVIEW_VIDEO_TEST_MARKER
      markReferenceFrame(image,tick);
#endif
      auto tex=std::unique_ptr<QSGTexture>(win->createTextureFromImage(image));
      if (!tex) {
        hideReview();
        return;
      }
      if(!overlay){overlay=new QSGSimpleTextureNode;appendChildNode(overlay);}
      overlay->setTexture(tex.get());
      if(history.size()==8)history.erase(history.begin());
      history.push_back({tick,{},std::move(tex),true});it=history.end()-1;
    }
    if(!overlay){overlay=new QSGSimpleTextureNode;appendChildNode(overlay);}
    overlay->setTexture(it->texture.get());
    if(it->native)overlay->setRect(viewport);
    else{
      auto fit=QSizeF(it->texture->textureSize()).scaled(viewport.size(),Qt::KeepAspectRatio);
      overlay->setRect(QRectF(viewport.center()-QPointF(fit.width()/2,fit.height()/2),fit));
    }
#else
    Q_UNUSED(win);Q_UNUSED(device);Q_UNUSED(viewport);Q_UNUSED(allowNative);
#endif
  }
  explicit Surface(std::shared_ptr<VideoEngine> e) : engine(std::move(e)) {}
  ~Surface() override {
    if (renderer) {
      api().render_context_set_update_callback(renderer, nullptr, nullptr);
      api().render_context_free(renderer);
    }
  }
  bool resize(QQuickWindow *win, ID3D11Device *device, QSize requested,bool hdr) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = UINT(requested.width());
    desc.Height = UINT(requested.height());
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = hdr?DXGI_FORMAT_R10G10B10A2_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> next;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &next)))
      return false;
    auto *qtTexture = videoOutput::importTexture(win,next.Get(),requested,hdr);
    if (!qtTexture)
      return false;
    setTexture(qtTexture);
    setOwnsTexture(true);
    target = std::move(next);
    invalidate();
    size = requested;pq=hdr;
    return true;
  }
};
} // namespace

VideoSession::VideoSession(QQuickItem *parent) : QQuickItem(parent) {
  setFlag(ItemHasContents);
  auto watchPlaybackWindow = [this](QQuickWindow *next) {
    if (shortcutWindow_)
      shortcutWindow_->removeEventFilter(this);
    shortcutWindow_ = next;
    if (next)
      next->installEventFilter(this);
  };
  connect(this, &QQuickItem::windowChanged, this, watchPlaybackWindow);
  watchPlaybackWindow(window());
  alignmentTimer_.setSingleShot(true);
  alignmentTimer_.setInterval(180);
  connect(&alignmentTimer_, &QTimer::timeout, this, &VideoSession::alignReview);
  prefetchTimer_.setSingleShot(true);
  connect(&prefetchTimer_, &QTimer::timeout, this, [this] {
    // Wait for the foreground exact seek before admitting a second UHD decoder.
    // A frame-step explicitly requests preparation and bypasses this idle timer.
    prefetchDeferred_ = pendingSeekTarget_.has_value();
    if (!prefetchDeferred_)
      prepareReviewFrames();
  });
  timer_.setInterval(16);
  connect(&timer_, &QTimer::timeout, this, &VideoSession::events);
}
bool VideoSession::eventFilter(QObject *watched, QEvent *event) {
  if (watched == shortcutWindow_ && playbackKeysEnabled_ &&
      (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress ||
       event->type() == QEvent::KeyRelease)) {
    auto *key = static_cast<QKeyEvent *>(event);
    const auto combination = key->key() == Qt::Key_Backtab
        ? QKeyCombination(key->modifiers() | Qt::ShiftModifier, Qt::Key_Tab) : key->keyCombination();
    const auto sequence = QKeySequence(combination).toString(QKeySequence::PortableText);
    const auto action = playbackBindings_.value(sequence).toString();
    if (!action.isEmpty()) {
      const bool repeatable = action == "seekBack" || action == "seekForward" ||
          action == "volumeUp" || action == "volumeDown" || action == "slower" || action == "faster";
      if (event->type() == QEvent::KeyPress && (!key->isAutoRepeat() || repeatable))
        emit shortcutPressed(action);
      key->accept();
      return true;
    }
  }
  return QQuickItem::eventFilter(watched, event);
}
void VideoSession::fail(const QString &message) {
  if (error_ != message) {
    error_ = message;
    emit errorChanged();
  }
}
static QString resumeKey(const QString &path) {
  QFileInfo file(path);
  auto identity = file.absoluteFilePath().toUtf8() + "|" +
                  QByteArray::number(file.size()) + "|" +
                  QByteArray::number(file.lastModified().toMSecsSinceEpoch());
  return "Video/Resume/" +
         QString::fromLatin1(
             QCryptographicHash::hash(identity, QCryptographicHash::Sha256)
                 .toHex());
}
VideoSession::~VideoSession() {
  disconnect(this, &QQuickItem::windowChanged, this, nullptr);
  if (shortcutWindow_)
    shortcutWindow_->removeEventFilter(this);
  releaseSession();
  if (hdrWindow_)
    hdrWindow_->deleteLater();
}
void VideoSession::releaseSession() {
  prefetchTimer_.stop();
  prefetchDeferred_ = false;
  if (!engine_)
    return;
#ifdef FORMAVIEW_INDEXED_VIDEO
  engine_->stream.cancelled = true;
#endif
  {
    std::lock_guard lock(engine_->callbackLock);
    engine_->view = nullptr;
  }
  if (status_.contains("duration")) {
    auto key = resumeKey(source_);
    double t = status_.value("visible-time", status_.value("time-pos")).toDouble(),
           duration = status_.value("duration").toDouble();
    QSettings prefs;
    if (t >= 5 && duration - t > 5)
      prefs.setValue(key, t);
    else
      prefs.remove(key);
  }
  command({"stop"});
}
void VideoSession::setSource(const QString &path) {
  if (path == source_)
    return;
  if (hdrWindow_)
    hdrWindow_->hide();
  openSession(path, false, false);
}
void VideoSession::openSession(const QString &path, bool switchingWindow, bool hdr) {
  QMap<QByteArray, QByteArray> options;
  QStringList subtitles;
  if (switchingWindow && engine_) {
    for (auto key : {"time-pos", "pause", "volume", "mute", "speed", "aid", "sid",
                     "sub-visibility", "sub-delay", "brightness", "contrast", "saturation",
                     "sharpen", "loop-file", "audio-spdif", "video-unscaled", "ab-loop-a", "ab-loop-b"}) {
      if (auto value = api().get_property_string(engine_->handle, key)) {
        options.insert(qstrcmp(key,"time-pos")==0 ? "start" : key, value);
        api().free(value);
      }
    }
    if (status_.contains("visible-time"))
      options["start"] = QByteArray::number(status_.value("visible-time").toDouble(),'f',6);
    for (const auto &track : status_.value("track-list").toList()) {
      const auto info = track.toMap();
      if (info.value("type")=="sub" && info.value("external").toBool())
        subtitles.append(info.value("external-filename").toString());
    }
  }
  timer_.stop();
  releaseSession();
  clearReview();
  if (!switchingWindow) {
    customOutput_ = false;
    outputAdjustments_.clear();
  }
  requestedStep_ = -1;
  source_ = path;
  engine_.reset();
  status_.clear();
  clearPlaybackInfo();
  pendingPlaybackOptions_.clear();
  screenshots_.clear();
  fail({});
  if (!switchingWindow) emit sourceChanged();
  emit statusChanged();
  update();
  if (path.isEmpty())
    return;
  if (!api().ready) {
    fail(tr("无法加载视频运行库 libmpv-2.dll"));
    return;
  }
  auto e = std::make_shared<VideoEngine>();
  e->view = this;
  e->handle = api().create();
  if (!e->handle) {
    fail(tr("无法创建视频播放器"));
    return;
  }
  auto option = [&](const char *n, const char *v) {
    return api().set_option_string(e->handle, n, v);
  };
  auto start = QByteArray::number(
      QSettings().value(resumeKey(path), 0).toDouble(), 'f', 6);
  option("start", start.constData());
#ifdef FORMAVIEW_INDEXED_VIDEO
  e->index = VideoIndex::open(path);
  if (e->index) {
    e->stream.path = path;
    auto reg = api().library.resolve("mpv_stream_cb_add_ro");
    if (!e->stream.attach(e->handle, reinterpret_cast<void *>(reg)))
      e->index.reset();
    else {
      e->worker = std::make_unique<VideoFrames>(e->index);
      option("cache", "no");
      option("sub-auto", "no");
      option("demuxer-seekable-cache", "no");
    }
  }
#endif
  option("config", "no");
  if (seekTrace().isDebugEnabled()) {
    const auto args = QCoreApplication::arguments();
    const auto trace = args.indexOf("--trace");
    if (trace >= 0 && trace + 1 < args.size()) {
      const auto log = (args[trace + 1] + ".mpv.log").toUtf8();
      option("log-file", log.constData());
      option("msg-level", "all=warn,cplayer=trace,demux=debug,vd=debug,ao/wasapi=debug,vo/libmpv=debug");
    }
  }
  option("load-scripts", "no");
  option("vo", background_ ? "null" : "libmpv");
  if (background_) {
    option("ao", "null");
    option("stop-screensaver", "no");
    option("pause", "yes");
    option("sub-auto", "no");
    option("audio-file-auto", "no");
  }
  if (background_ && QGuiApplication::platformName() == "windows") {
    // Let the decoder retain preroll frames on the device. A hidden parent
    // keeps the embedded output off the desktop; PNG requests still read
    // back the selected source frame. Fall back on hosts without D3D11.
    if (!backgroundTarget_) {
      backgroundTarget_ = std::make_unique<QWindow>();
      backgroundTarget_->setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
      backgroundTarget_->resize(64, 64);
      backgroundTarget_->create();
    }
    if (const auto wid = backgroundTarget_->winId()) {
      const auto handle = QByteArray::number(qulonglong(wid));
      option("wid", handle.constData());
      option("gpu-api", "d3d11");
      option("gpu-context", "d3d11");
      option("screenshot-sw", "yes");
      option("vo", "gpu,null");
    }
  }
  option("hwdec", "auto-safe");
#ifdef FORMAVIEW_VIDEO_TEST_MARKER
  option("hwdec","d3d11va");option("vf",measurementGraph(0).constData());
#endif
  option("vd-lavc-threads","6");
  option("hr-seek-framedrop","yes");
  option("keep-open", "yes");
  option("osc", "no");
  option("osd-level", "0");
  option("terminal", "no");
  option("audio-display", "no");
  // Keep the source bit depth. A fast lossless PNG preset reduces capture
  // wait time on UHD frames without changing decoded pixels.
  option("screenshot-png-compression", "1");
  option("screenshot-png-filter", "1");
  for (auto it=options.cbegin(); it!=options.cend(); ++it)
    option(it.key().constData(), it.value().constData());
  for (const auto &sub : subtitles)
    option("sub-files-append", sub.toUtf8().constData());
  option("target-trc", hdr ? "pq" : "srgb");
  option("target-prim", hdr ? "bt.2020" : "bt.709");
  if (hdr) option("target-peak", "1000");
  if (api().initialize(e->handle) < 0) {
    fail(tr("视频播放器初始化失败"));
    return;
  }
#ifdef FORMAVIEW_INDEXED_VIDEO
  if (path.endsWith(".flv", Qt::CaseInsensitive) && QFileInfo(path).size() >= minIndexedFlvSize) {
    e->stream.path = path;
    e->stream.flv = e->stream.attach(e->handle,
        reinterpret_cast<void *>(api().library.resolve("mpv_stream_cb_add_ro"))) &&
        api().hook_add(e->handle, 0, "on_load", 0) >= 0;
    if (e->stream.flv) e->stream.prepareFlvIndex();
  }
#endif
  for (const char *name : {"time-pos",
                           "duration",
                           "pause",
                           "volume",
                           "mute",
                           "speed",
                           "track-list",
                           "chapter-list",
                           "chapter",
                           "loop-file",
                           "aid",
                           "sid",
                           "sub-visibility",
                           "sub-delay",
                           "video-params",
                           "hwdec-current",
                           "audio-codec-name",
                           "audio-out-params",
                           "vf", "brightness", "contrast", "saturation", "sharpen",
                             "sub-text",
                           "ab-loop-a",
                           "ab-loop-b",
                           "eof-reached"})
    api().observe_property(e->handle, 0, name, MPV_FORMAT_NODE);
#ifdef FORMAVIEW_INDEXED_VIDEO
  status_.insert("indexed-review",bool(e->index));
#endif
  engine_ = std::move(e);
  if (background_) loadVideo();
  timer_.start();
  update();
}
void VideoSession::loadVideo() {
  engine_->loaded = true;
  QByteArray name = source_.toUtf8();
#ifdef FORMAVIEW_INDEXED_VIDEO
  if (engine_->index) name = "fvmedia://source.mp4";
#endif
  command({"loadfile", name, "replace"});
}
bool VideoSession::command(const QList<QByteArray> &args, quint64 id) {
  if (!engine_)
    return false;
  QList<const char *> argv;
  for (auto &arg : args)
    argv.append(arg.constData());
  argv.append(nullptr);
  int rc = api().command_async(engine_->handle, id, argv.data());
  if (rc < 0)
    fail(QString::fromUtf8(api().error_string(rc)));
  return rc >= 0;
}
void VideoSession::clearReview() {
  alignmentTimer_.stop();alignRequest_=0;alignAck_=alignPainted_=false;
  pendingSeekTarget_.reset();
  pendingSeekRequest_ = 0;
  positionQuery_ = 0;

  reviewTick_ = alignTick_ = -1;
  reviewFrame_.reset();
  resumeAfterAlign_ = false;
  pendingShots_.clear();
  status_.remove("visible-time");
  status_.remove("review-caption");
  update();
}
void VideoSession::prepareReviewFrames() {
  prefetchTimer_.stop();
  prefetchDeferred_ = false;
  if (background_) return; // No Qt viewport consumes a review texture.
#ifdef FORMAVIEW_INDEXED_VIDEO
  if (!engine_ || !engine_->worker || !engine_->plan.valid())
    return;
  const auto dpi = window() ? window()->effectiveDevicePixelRatio() : 1.;
  engine_->worker->prepare(engine_->warmAnchor, QSize(qRound(width()*dpi), qRound(height()*dpi)), engine_->plan);
#endif
}
void VideoSession::alignReview() {
#ifdef FORMAVIEW_INDEXED_VIDEO
  if (reviewTick_ < 0 || !engine_ || !engine_->plan.valid())
    return;
  if (alignTick_ == reviewTick_) {
    reviewReady();
    return;
  }
  alignTick_ = reviewTick_;
  alignAck_=alignPainted_=false;alignRequest_=++serial_;
#ifdef FORMAVIEW_VIDEO_TEST_MARKER
  if(double(alignTick_)/engine_->plan.scale<engine_->diagnosticFloor){
    engine_->diagnosticFloor=double(alignTick_)/engine_->plan.scale-.0001;
    command({"vf","set",measurementGraph(engine_->diagnosticFloor)});
  }
#endif
  command({"seek",
           QByteArray::number(double(alignTick_) / engine_->plan.scale, 'f', 9),
           "absolute+exact"},alignRequest_);
#endif
}
void VideoSession::reviewReady() {
#ifdef FORMAVIEW_INDEXED_VIDEO
  if (!alignPainted_ || !engine_ || alignTick_ < 0 || alignTick_ != reviewTick_ ||
      !engine_->plan.valid())
    return;
  if (!status_.contains("time-pos") ||
      std::abs(qRound64(status_.value("time-pos").toDouble() *
                        engine_->plan.scale) -
               alignTick_) > 1)
    return;
  const auto shots = std::exchange(pendingShots_, {});
  for (const auto &shot : shots) {
    const auto saved = reviewTick_;
    reviewTick_ = -1;
    screenshot(shot.first, shot.second);
    reviewTick_ = saved;
  }
  if (resumeAfterAlign_) {
    clearReview();
    command({"set", "pause", "no"});
    emit statusChanged();
  }
#endif
}
QByteArray VideoSession::windowPlacement(QQuickWindow *window) const {
  if (!window || !window->handle())
    return {};
  const auto hwnd = reinterpret_cast<HWND>(window->winId());
  SavedWindowPlacement saved{};
  if (!GetWindowPlacement(hwnd, &saved.native))
    return {};
  saved.monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  saved.dpi = GetDpiForWindow(hwnd);
  return QByteArray(reinterpret_cast<const char *>(&saved), sizeof(saved));
}

bool VideoSession::restoreWindowPlacement(QQuickWindow *window,
                                          const QByteArray &saved) const {
  SavedWindowPlacement placement{};
  if (!window || !window->handle() || saved.size() != sizeof(placement))
    return false;
  std::memcpy(&placement, saved.constData(), sizeof(placement));
  const auto hwnd = reinterpret_cast<HWND>(window->winId());
  MONITORINFO monitor{sizeof(MONITORINFO)};
  if (!GetMonitorInfoW(placement.monitor, &monitor)) {
    const auto current = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    const auto dpi = GetDpiForWindow(hwnd);
    if (!dpi || !placement.dpi || !GetMonitorInfoW(current, &monitor))
      return false;
    auto &rect = placement.native.rcNormalPosition;
    const LONG workWidth = monitor.rcWork.right - monitor.rcWork.left;
    const LONG workHeight = monitor.rcWork.bottom - monitor.rcWork.top;
    const LONG width = qMin<LONG>(workWidth, MulDiv(rect.right - rect.left, dpi, placement.dpi));
    const LONG height = qMin<LONG>(workHeight, MulDiv(rect.bottom - rect.top, dpi, placement.dpi));
    // WINDOWPLACEMENT uses workspace coordinates, excluding the taskbar offset.
    const LONG left = monitor.rcMonitor.left + (workWidth - width) / 2;
    const LONG top = monitor.rcMonitor.top + (workHeight - height) / 2;
    rect = {left, top, left + width, top + height};
    placement.native.ptMaxPosition = {-1, -1};
  }
  return SetWindowPlacement(hwnd, &placement.native);
}

QQuickWindow *VideoSession::prepareHdrWindow(){
  const auto gamma=status_.value("video-params").toMap().value("gamma").toString();
  if(!engine_||(gamma!="pq" && gamma!="hlg")){
    fail(tr("当前视频不是 PQ 或 HLG"));return nullptr;
  }
  if(!videoOutput::hdrDesktop(window())){fail(tr("当前屏幕未启用 Windows HDR，已使用 SDR 映射"));return nullptr;}
  if(!hdrWindow_){
    hdrWindow_=new QQuickWindow;
    QQmlEngine::setObjectOwnership(hdrWindow_,QQmlEngine::CppOwnership);
    hdrWindow_->setColor(Qt::black);
    hdrWindow_->setProperty("_qt_sg_hdr_format","hdr10");
    hdrWindow_->setTitle(tr("FormaView · HDR10"));
    hdrWindow_->setFlags(Qt::Window|Qt::FramelessWindowHint);
  }
  hdrWindow_->setScreen(window()->screen());
  hdrWindow_->setGeometry(window()->screen()->geometry());
  openSession(source_,true,true);
  return engine_ ? hdrWindow_.data() : nullptr;
}
void VideoSession::leaveHdr(){
  if(engine_) openSession(source_,true,false);
  if(hdrWindow_)hdrWindow_->hide();
  status_.remove("hdr-output");emit statusChanged();
}
void VideoSession::togglePause() {
  requestedStep_ = -1;
  if (reviewTick_ >= 0) {
    resumeAfterAlign_ = true;
    alignmentTimer_.stop();
    alignReview();
    return;
  }
  if (status_.value("eof-reached").toBool() && !pendingSeekTarget_) {
    // keep-open retains the final frame. Rewind the same session so speed,
    // selected tracks and subtitles also apply to the replay.
    seek(0., false);
    command({"set", "pause", "no"});
    return;
  }
  command({"cycle", "pause"});
}
void VideoSession::seek(double seconds, bool relative) {
  requestedStep_ = -1;
  if (!std::isfinite(seconds) || !engine_)
    return;
  if (relative)
    seconds += pendingSeekTarget_.value_or(
        status_.value("visible-time", status_.value("time-pos")).toDouble());
  traceSeek("request", seconds);
  clearReview();++videoEpoch_;
  prefetchTimer_.stop();
  prefetchDeferred_ = false;
  emit statusChanged();
  seconds =
      std::clamp(seconds, 0.0, qMax(0.0, status_.value("duration").toDouble()));
#ifdef FORMAVIEW_INDEXED_VIDEO
  if (engine_->index) {
    if (engine_->worker)
      engine_->worker->cancel();
    engine_->plan = engine_->index->plan(seconds);
    engine_->stream.apply(engine_->plan);
#ifdef FORMAVIEW_VIDEO_TEST_MARKER
    engine_->diagnosticFloor=engine_->plan.valid()?double(engine_->plan.ticks[qMax(0,engine_->plan.prefix-4)])/engine_->plan.scale-.0001:0;
    command({"vf","set",measurementGraph(engine_->diagnosticFloor)});
#endif
    if(engine_->worker){
      engine_->warmAnchor=seconds;
      if (engine_->plan.valid())
        prefetchTimer_.start(engine_->plan.prefix > 320 ? 100 : 240);
    }
  }
#endif
  requestExactSeek(seconds);
}
void VideoSession::requestExactSeek(double seconds) {
  const auto request = ++serial_;
  if (command({"seek", QByteArray::number(seconds, 'f', 9), "absolute+exact"}, request)) {
    traceSeek("queued", seconds);
    pendingSeekTarget_ = seconds;
    pendingSeekRequest_ = request;
  }
}
void VideoSession::step(bool forward) {
  const auto seekingTo = std::exchange(pendingSeekTarget_, std::nullopt);
  pendingSeekRequest_ = 0;
  positionQuery_ = 0;
#ifdef FORMAVIEW_INDEXED_VIDEO
  if (engine_ && engine_->index && engine_->worker && !customOutput_ &&
      status_.value("pause").toBool() && status_.contains("time-pos")) {
    const double seconds = requestedStep_ >= 0 ? requestedStep_ : seekingTo.value_or(
        status_.value("visible-time", status_.value("time-pos")).toDouble());
    if (std::abs(seconds-engine_->warmAnchor)>.65 || !engine_->plan.valid() ||
        qRound64(seconds * engine_->plan.scale) < engine_->plan.firstSafe - 1 ||
        qRound64(seconds * engine_->plan.scale) > engine_->plan.ticks.back() + 1) {
      engine_->warmAnchor=seconds;engine_->plan = engine_->index->plan(seconds);
      engine_->stream.apply(engine_->plan);
      prepareReviewFrames();
    }
    const auto &plan = engine_->plan;
    if (plan.valid()) {
      qint64 current = qRound64(seconds * plan.scale);
      auto it = std::lower_bound(plan.ticks.begin(), plan.ticks.end(), current);
      // An exact seek can report the requested time one stream tick away
      // from the picture's source PTS. Match that picture before stepping.
      if (it != plan.ticks.begin() && (it == plan.ticks.end() || *it != current) &&
          *(it - 1) == current - 1)
        --it;
      if (it != plan.ticks.end() && std::abs(*it - current) <= 1 &&
          (forward ? it + 1 != plan.ticks.end() : it != plan.ticks.begin())) {
        qint64 tick = forward ? *(it + 1) : *(it - 1);
        requestedStep_ = double(tick) / plan.scale;
        if (tick >= plan.firstSafe) {
          auto review = engine_->worker->frame(tick);
          std::optional<QString> caption = QString();
          if (review &&
              status_.value("sub-visibility", true).toBool()) {
            for (auto t : status_.value("track-list").toList()) {
              auto m = t.toMap();
              if (m.value("selected").toBool() && m.value("type") == "sub") {
                caption = m.value("external").toBool()
                              ? std::nullopt
                              : engine_->index->caption(
                                    m.value("ff-index", -1).toInt(),
                                    double(tick) / plan.scale -
                                        status_.value("sub-delay").toDouble());
                break;
              }
            }
          }
          if (review && caption) {
            reviewTick_ = tick;
            reviewFrame_ = std::move(review);
            resumeAfterAlign_ = false;
            pendingShots_.clear();
            status_.insert("visible-time", double(tick) / plan.scale);
            status_.insert("review-caption", *caption);
            alignmentTimer_.start();
            emit statusChanged();
            update();
            return;
          }
          clearReview();
          // A missing neighbor still needs the pending decode for later steps.
          if (prefetchTimer_.isActive() || prefetchDeferred_)
            prepareReviewFrames();
#ifdef FORMAVIEW_VIDEO_TEST_MARKER
          if (double(tick) / plan.scale < engine_->diagnosticFloor) {
            engine_->diagnosticFloor = double(tick) / plan.scale - .0001;
            command({"vf","set",measurementGraph(engine_->diagnosticFloor)});
          }
#endif
          requestExactSeek(double(tick) / plan.scale);
          emit statusChanged();
          return;
        }
        // Rebuild dependencies for the requested picture when it falls outside
        // the current decoder window. Seeking the anchor would repeat a frame.
        const double target = requestedStep_;
        seek(target, false);
        requestedStep_ = target;
        return;
      }
    }
    if (!forward && plan.valid() && std::abs(qRound64(seconds*plan.scale)-plan.ticks.front())<=1 && seconds>0) {
      const auto prior = engine_->index->plan(seconds - 1.0 / plan.scale);
      if (prior.valid() && prior.ticks.back() < plan.ticks.front()) {
        const double target = double(prior.ticks.back()) / prior.scale;
        seek(target, false);
        requestedStep_ = target;
        return;
      }
    }
    requestedStep_ = -1;
    if (reviewTick_ >= 0) {
      auto t = seconds;
      clearReview();
      command({"seek", QByteArray::number(t, 'f', 9), "absolute+exact"});
    }
    engine_->stream.apply({});
    engine_->plan = {};
  }
#endif
  requestedStep_ = -1;
  command({forward ? "frame-step" : "frame-back-step"});
}
void VideoSession::setOption(const QString &name, const QVariant &value) {
  if (name == "pause" && value.toString() == "yes")
    resumeAfterAlign_ = false;
  static const QStringList allowed = {
      "pause",   "volume",     "mute",           "speed",
      "sid",     "aid",        "sub-visibility", "sub-delay",
      "chapter", "brightness", "contrast",       "saturation",
      "sharpen", "loop-file",  "audio-spdif",    "video-unscaled"};
  if (name == "chapter") {
    bool ok = false;
    const int chapter = value.toInt(&ok);
    if (!ok || chapter < 0 || chapter >= status_.value("chapter-list").toList().size())
      return;
    requestedStep_ = -1;
    clearReview();
    ++videoEpoch_;
    prefetchTimer_.stop();
    prefetchDeferred_ = false;
#ifdef FORMAVIEW_INDEXED_VIDEO
    if (engine_) {
      if (engine_->worker)
        engine_->worker->cancel();
      engine_->plan = {};
      engine_->stream.apply(engine_->plan);
    }
#endif
    emit statusChanged();
  }
  if (name == "volume" || name == "speed") {
    const auto request = ++serial_;
    if (command({"set", name.toUtf8(), value.toString().toUtf8()}, request)) {
      // Consecutive keys build on the latest requested value while mpv applies it.
      pendingPlaybackOptions_.insert(name, request);
      status_.insert(name, value);
      emit statusChanged();
    }
    return;
  }
  if (name == "brightness" || name == "contrast" || name == "saturation" ||
      name == "sharpen" || name == "video-unscaled") {
    if (background_ && name != "video-unscaled") {
      // Source screenshots include these software filters. Download decoded
      // frames only while at least one of those filters is active.
      const auto v = value.toDouble();
      auto hasFrameFilter = [this] {
        return outputAdjustments_.size() > 1 ||
               (!outputAdjustments_.isEmpty() && !outputAdjustments_.contains("video-unscaled"));
      };
      const bool hadFilter = hasFrameFilter();
      if (v != 0 && !hadFilter && backgroundTarget_)
        command({"set", "hwdec", "auto-copy"});
      const auto prefix = "fv-" + name + "_";
      for (const auto &entry : status_.value("vf").toList()) {
        const auto label = entry.toMap().value("label").toString();
        if (label.startsWith(prefix))
          command({"vf", "remove", ("@" + label).toUtf8()});
      }
      if (v != 0) {
        const auto key = QByteArray::number(v, 'g', 12);
        QByteArray graph;
        if (name == "sharpen") graph = "unsharp=5:5:" + key;
        else if (name == "saturation") graph = "hue=s=" + QByteArray::number(1 + v/100);
        else if (name == "brightness") graph = "lutyuv=y=val+maxval*(" + QByteArray::number(v/100) + ")";
        else graph = "lutyuv=y=(val-maxval/2)*" + QByteArray::number(1 + v/100) + "+maxval/2";
        // Dots are accepted when creating labels but not by mpv's removal parser.
        const auto labelValue = QByteArray(key).replace('.', 'p');
        command({"vf", "add", "@" + prefix.toUtf8() + labelValue + ":lavfi=[" + graph + "]"});
      }
      if (v == 0) outputAdjustments_.remove(name); else outputAdjustments_.insert(name);
      if (hadFilter && !hasFrameFilter() && backgroundTarget_)
        command({"set", "hwdec", "auto-safe"});
      customOutput_ = !outputAdjustments_.isEmpty();
      return;
    }
    if (name=="video-unscaled" ? value.toString()=="yes" : value.toDouble()!=0)
      outputAdjustments_.insert(name);
    else
      outputAdjustments_.remove(name);
    customOutput_ = !outputAdjustments_.isEmpty();
  }
  if (reviewTick_ >= 0 && (name == "brightness" || name == "contrast" ||
      name == "saturation" || name == "sharpen" || name == "video-unscaled" ||
      name == "sid" || name == "sub-visibility" || name == "sub-delay"))
    seek(status_.value("visible-time").toDouble(), false);
  if (allowed.contains(name))
    command({"set", name.toUtf8(), value.toString().toUtf8()});
}
void VideoSession::loop(double a, double b) {
  if (std::isfinite(a) && std::isfinite(b) && a >= 0 && b > a) {
    command({"set", "ab-loop-a", QByteArray::number(a, 'f', 6)});
    command({"set", "ab-loop-b", QByteArray::number(b, 'f', 6)});
  }
}
void VideoSession::clearLoop() {
  command({"set", "ab-loop-b", "no"});
  command({"set", "ab-loop-a", "no"});
}
void VideoSession::addSubtitle(const QUrl &url) {
  if (url.isLocalFile()) {
    if (reviewTick_ >= 0)
      seek(status_.value("visible-time").toDouble(), false);
    command({"sub-add", url.toLocalFile().toUtf8(), "select"});
  }
}
void VideoSession::screenshot(const QUrl &directory, bool subtitles) {
  if (reviewTick_ >= 0) {
    pendingShots_.append({directory, subtitles});
    alignReview();
    return;
  }
  const QString dest =
      directory.isLocalFile()
          ? directory.toLocalFile()
          : QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) +
                "/FormaView";
  if (!QDir().mkpath(dest)) {
    fail(tr("无法创建截图目录"));
    return;
  }
  const auto name =
      QFileInfo(source_).completeBaseName() + "-" +
      QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz") + "-" +
      QUuid::createUuid().toString(QUuid::WithoutBraces).left(8) + ".png";
  auto file = QDir(dest).filePath(name);
  const auto request = ++serial_;
  if (command({"screenshot-to-file", file.toUtf8(), subtitles ? "subtitles" : "video"}, request))
    screenshots_.insert(request, file);
  else
    emit screenshotFailed(file, error_);
}
void VideoSession::clearPlaybackInfo() {
  infoRequest_ = 0;
  infoReplies_ = 0;
  statistics_.reset();
  pendingInfo_.clear();
  playbackInfo_.clear();
  emit playbackInfoChanged();
}

void VideoSession::samplePlaybackInfo() {
  if (!engine_ || !engine_->handle || infoRequest_)
    return;
  if (!statistics_)
    statistics_ = std::make_unique<VideoStatistics>();
  pendingInfo_ = statistics_->sample(engine_->adapter.Get());
  engine_->samplePresentation = true;
  update();
  if (window() && window()->screen())
    pendingInfo_.insert("screen-hz", window()->screen()->refreshRate());
  infoRequest_ = ++serial_;
  for (auto name : {"mpv-version", "ffmpeg-version", "video-codec", "video-dec-params",
                    "video-out-params", "container-fps", "estimated-vf-fps", "video-bitrate",
                    "estimated-frame-number", "estimated-frame-count", "decoder-frame-drop-count",
                    "frame-drop-count", "vo-delayed-frame-count", "mistimed-frame-count",
                    "avsync", "vsync-jitter", "audio-codec", "audio-params", "audio-bitrate",
                    "current-ao", "audio-buffer", "demuxer-cache-duration"}) {
    if (api().get_property_async(engine_->handle, infoRequest_, name, MPV_FORMAT_NODE) >= 0)
      ++infoReplies_;
  }
  if (!infoReplies_) infoRequest_ = 0;
}

void VideoSession::events() {
  if (!engine_)
    return;
  const auto query = [this](const char *name, mpv_format format) -> quint64 {
    const auto id = ++serial_;
    return api().get_property_async(engine_->handle, id, name, format) >= 0 ? id : 0;
  };
  bool changed = false;
  while (auto *ev = api().wait_event(engine_->handle, 0)) {
    if (ev->event_id == MPV_EVENT_NONE)
      break;
#ifdef FORMAVIEW_INDEXED_VIDEO
    if (ev->event_id == MPV_EVENT_HOOK && engine_->stream.flv) {
      // Preserve the original playlist path, subtitle discovery and title;
      // only the demuxer's read handle uses the indexed byte view.
      const auto id = static_cast<mpv_event_hook *>(ev->data)->id;
      api().set_property_string(engine_->handle, "stream-open-filename", "fvmedia://source.flv");
      api().hook_continue(engine_->handle, id);
      continue;
    }
#endif
    if (ev->event_id == MPV_EVENT_PLAYBACK_RESTART && seekTrace().isDebugEnabled())
      traceSeek("restart", status_.value("time-pos", -1).toDouble());
    if (ev->event_id == MPV_EVENT_PROPERTY_CHANGE) {
      auto *p = static_cast<mpv_event_property *>(ev->data);
      if (pendingPlaybackOptions_.contains(QString::fromUtf8(p->name)))
        continue;
      auto v = p->format == MPV_FORMAT_NODE && p->data
                   ? variant(*static_cast<mpv_node *>(p->data))
                   : QVariant();
      if (status_.value(p->name) != v) {
        status_.insert(p->name, v);
        changed = true;
      }
    } else if(ev->event_id==MPV_EVENT_PLAYBACK_RESTART && alignAck_ && alignRequest_ && window()){
      const auto id=alignRequest_;
      connect(window(),&QQuickWindow::frameSwapped,this,[this,id]{
        if(id==alignRequest_){alignPainted_=true;reviewReady();}
      },Qt::ConnectionType(Qt::QueuedConnection|Qt::SingleShotConnection));
      update();
    } else if (ev->event_id == MPV_EVENT_END_FILE) {
      auto *end = static_cast<mpv_event_end_file *>(ev->data);
      if (end->reason == MPV_END_FILE_REASON_ERROR)
        fail(QString::fromUtf8(api().error_string(end->error)));
    } else if (ev->event_id == MPV_EVENT_GET_PROPERTY_REPLY) {
      const auto *p = static_cast<const mpv_event_property *>(ev->data);
      const bool valid = ev->error >= 0 && p && p->data;
      if (infoRequest_ && ev->reply_userdata == infoRequest_) {
        if (valid && p->format == MPV_FORMAT_NODE)
          pendingInfo_.insert(p->name, variant(*static_cast<const mpv_node *>(p->data)));
        if (--infoReplies_ == 0) {
          infoRequest_ = 0;
          for (auto name : {"time-pos", "visible-time", "duration", "speed", "track-list", "video-params",
                            "hwdec-current", "audio-out-params", "hdr-output"})
            if (status_.contains(name)) pendingInfo_.insert(name, status_.value(name));
          pendingInfo_.insert(engine_->presentation);
          playbackInfo_ = std::move(pendingInfo_);
          emit playbackInfoChanged();
        }
        continue;
      }
      if (positionQuery_ && ev->reply_userdata == positionQuery_) {
        positionQuery_ = 0;
        if (valid && p->format == MPV_FORMAT_FLAG) {
          if (!*static_cast<const int *>(p->data)) {
            positionQuery_ = query("time-pos", MPV_FORMAT_DOUBLE);
            if (!positionQuery_)
              pendingSeekTarget_.reset();
          }
        } else {
          pendingSeekTarget_.reset();
          if (valid && p->format == MPV_FORMAT_DOUBLE) {
            status_.insert("time-pos", *static_cast<const double *>(p->data));
            if (prefetchDeferred_)
              prepareReviewFrames();
            changed = true;
          }
        }
      } else {
        const auto option = pendingPlaybackOptions_.key(ev->reply_userdata);
        if (!option.isEmpty()) {
          pendingPlaybackOptions_.remove(option);
          if (valid && p->format == MPV_FORMAT_DOUBLE)
            status_.insert(option, *static_cast<const double *>(p->data));
          else
            status_.remove(option);
          changed = true;
        }
      }
    } else if (ev->event_id == MPV_EVENT_COMMAND_REPLY) {
      if (ev->reply_userdata) {
        if (ev->reply_userdata == pendingSeekRequest_) {
          traceSeek("reply", pendingSeekTarget_.value_or(-1));
          pendingSeekRequest_ = 0;
          if (ev->error < 0)
            pendingSeekTarget_.reset();
        }
        const auto option = pendingPlaybackOptions_.key(ev->reply_userdata);
        if (!option.isEmpty()) {
          const auto id = query(option.toUtf8().constData(), MPV_FORMAT_DOUBLE);
          if (id)
            pendingPlaybackOptions_.insert(option, id);
          else {
            pendingPlaybackOptions_.remove(option);
            status_.remove(option);
            changed = true;
          }
        }
      }
      if(alignRequest_ && ev->reply_userdata==alignRequest_ && ev->error>=0)alignAck_=true;
      auto file = screenshots_.take(ev->reply_userdata);
      if (ev->error < 0) {
        fail(QString::fromUtf8(api().error_string(ev->error)));
        if (!file.isEmpty()) emit screenshotFailed(file, error_);
      } else if (!file.isEmpty())
        emit screenshotSaved(file);
    }
  }
  // A command reply acknowledges the seek before decoding necessarily ends.
  // Use the requested position for consecutive keys until mpv has settled.
  // The mpv core may be waiting for Qt to render the target picture. A
  // synchronous property read here stalls both sides until mpv's watchdog.
  if (pendingSeekTarget_ && !pendingSeekRequest_ && !positionQuery_) {
    positionQuery_ = query("seeking", MPV_FORMAT_FLAG);
    if (!positionQuery_)
      pendingSeekTarget_.reset();
  }
#ifdef FORMAVIEW_INDEXED_VIDEO
  if(engine_->worker && status_.contains("time-pos"))engine_->worker->track(status_.value("time-pos").toDouble());
#endif
  reviewReady();
  if (changed)
    emit statusChanged();
}
QSGNode *VideoSession::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
  auto *node = static_cast<Surface *>(old);
  if (node && node->engine != engine_) {
    delete node;
    node = nullptr;
  }
  if (!engine_ || width() < 1 || height() < 1)
    return node;
  auto *win = window();
  auto *ri = win->rendererInterface();
  auto *device = ri->graphicsApi() == QSGRendererInterface::Direct3D11
                     ? static_cast<ID3D11Device *>(ri->getResource(
                           win, QSGRendererInterface::DeviceResource))
                     : nullptr;
  auto notifyError = [this] {
    QMetaObject::invokeMethod(
        this, [this] { fail(tr("无法创建 D3D11 视频画面")); },
        Qt::QueuedConnection);
  };
  if (!device) {
    notifyError();
    return node;
  }
  if (engine_->samplePresentation) {
    engine_->presentation = videoOutput::presentationInfo(win);
    engine_->samplePresentation = false;
  }
  if (!node) {
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))))
      dxgi->GetAdapter(engine_->adapter.ReleaseAndGetAddressOf());
    auto next = std::make_unique<Surface>(engine_);
    mpv_d3d11_init_params init{device};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE,
         const_cast<char *>(MPV_RENDER_API_TYPE_D3D11)},
        {MPV_RENDER_PARAM_D3D11_INIT_PARAMS, &init},
        {}};
    if (api().render_context_create(&next->renderer, engine_->handle, params) <
        0) {
      notifyError();
      return nullptr;
    }
    api().render_context_set_update_callback(
        next->renderer,
        [](void *p) {
          auto *e = static_cast<VideoEngine *>(p);
          std::lock_guard lock(e->callbackLock);
          if (e->view)
            QMetaObject::invokeMethod(e->view, "update", Qt::QueuedConnection);
        },
        engine_.get());
    node = next.release();
  }
  const QSize size(
      qBound(1, qRound(width() * win->effectiveDevicePixelRatio()), 8192),
      qBound(1, qRound(height() * win->effectiveDevicePixelRatio()), 8192));
  const bool hdr=win==hdrWindow_ && videoOutput::pqSwapchain(win);
  if(win==hdrWindow_&&!hdr){
    QMetaObject::invokeMethod(this,[this]{if(error_.isEmpty())fail(tr("无法建立 HDR10 交换链，请返回 SDR 主窗"));},Qt::QueuedConnection);
    return node;
  }
  if ((node->size != size || node->pq!=hdr) && !node->resize(win, device, size,hdr)) {
    notifyError();
    delete node;
    return nullptr;
  }
  if (!engine_->loaded) loadVideo();
  node->setRect(boundingRect());
  if(node->epoch!=videoEpoch_){node->invalidate();node->epoch=videoEpoch_;}
  node->showReview(win, device, reviewTick_, reviewFrame_.get(), boundingRect(),
                   status_.value("review-caption").toString().isEmpty());
  mpv_d3d11_fbo fbo{node->target.Get()};
  int blockForDeadline = 0;
  mpv_render_param params[] = {
      {MPV_RENDER_PARAM_D3D11_FBO, &fbo},
      {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &blockForDeadline},
      {}};
  win->beginExternalCommands();
  auto changes=api().render_context_update(node->renderer);
  mpv_render_frame_info frame{};
  mpv_render_param query{MPV_RENDER_PARAM_NEXT_FRAME_INFO,&frame};
  api().render_context_get_info(node->renderer,query);
  const int renderResult=api().render_context_render(node->renderer, params);
  if(hdr && renderResult>=0 && (frame.flags&MPV_RENDER_FRAME_INFO_PRESENT) &&
     !status_.value("hdr-output").toBool() && !status_.value("video-params").toMap().isEmpty()){
    QMetaObject::invokeMethod(this,[this,active=engine_]{if(engine_==active && window()==hdrWindow_){status_.insert("hdr-output",true);emit statusChanged();}},Qt::QueuedConnection);
  }
#ifdef FORMAVIEW_INDEXED_VIDEO
  // Only uninterrupted playback contributes to the native history. After a
  // paused seek, sparse displayed frames can spuriously match a different
  // neighbor in the still-filling CPU cache. Use its timestamped frames there.
  if(engine_->index && reviewTick_<0 && !customOutput_ && !status_.value("pause", true).toBool() &&
     (changes&MPV_RENDER_UPDATE_FRAME) &&
     (frame.flags&MPV_RENDER_FRAME_INFO_PRESENT) && !(frame.flags&(MPV_RENDER_FRAME_INFO_REDRAW|MPV_RENDER_FRAME_INFO_REPEAT)))
    node->retain(win,device);
#else
  Q_UNUSED(changes);
#endif
  win->endExternalCommands();
  node->markDirty(QSGNode::DirtyMaterial);
  return node;
}
