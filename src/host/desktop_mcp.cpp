#include "desktop_mcp.hpp"
#include "desktop.hpp"
#include "image_jobs.hpp"
#include "image_session.hpp"
#include "video_session.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLockFile>
#include <QSettings>
#include <QGuiApplication>
#include <QScreen>
#include <QtGui/qscreen_platform.h>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <QWinEventNotifier>
#include <windows.h>
#include <cmath>
#include <future>
#include <map>
#include <iostream>

namespace {
using namespace formaview;
using namespace formaview::imaging;
using Reply = ImageEndpoint::Reply;
using Result = std::optional<QJsonObject>;

QString displayId(QScreen *screen) {
  if (screen) {
    auto *native=screen->nativeInterface<QNativeInterface::QWindowsScreen>();
    MONITORINFOEXW info{};
    info.cbSize=sizeof(info);
    if (native && GetMonitorInfoW(native->handle(), &info))
      return QString::fromWCharArray(info.szDevice);
  }
  return {};
}

void require(bool condition, ErrorCode code, const char *message) {
  if (!condition) throw skeletal::Failure(code, message);
}
QJsonObject failure(ErrorCode code, const char *message) {
  return image_error_json(skeletal::Failure(code, message));
}
double numeric(const QJsonObject &r, const char *field) {
  require(r[field].isDouble() && std::isfinite(r[field].toDouble()),
          ErrorCode::InvalidArgument, "A finite numeric value is required");
  return r[field].toDouble();
}
double position(const QVariantMap &state) {
  return state.value("visible-time", state.value("time-pos", -1)).toDouble();
}
const QStringList adjustments{"brightness", "contrast", "saturation", "sharpen"};
QJsonObject filterValues(const QVariantMap &s, bool background) {
  QJsonObject result;
  for (const auto &name : adjustments) result[name] = background ? 0 : s.value(name).toDouble();
  if (background) for (const auto &entry : s.value("vf").toList()) {
    const auto label = entry.toMap().value("label").toString();
    for (const auto &name : adjustments) {
      const auto prefix = "fv-" + name + "_";
      if (label.startsWith(prefix)) result[name] = label.mid(prefix.size()).replace('p','.').toDouble();
    }
  }
  return result;
}
QJsonValue loopTime(const QVariantMap &s, const QString &key) {
  bool ok = false;
  const auto value = s.value(key).toDouble(&ok);
  return ok ? QJsonValue(value) : QJsonValue(QJsonValue::Null);
}
QVariantMap selectedTrack(const QVariantMap &s, const QString &type) {
  for (const auto &row : s.value("track-list").toList()) {
    const auto t = row.toMap();
    if (t.value("type") == type && t.value("selected").toBool()) return t;
  }
  return {};
}

class Session final : public QObject {
  Desktop &desktop;
  QPointer<QQuickWindow> window;
  VideoSession *background;
  AccessScope scope;
  QString assetId, path, kind;
  quint64 revision = 0;
  std::optional<InputIdentityLease> input;
  std::map<QString, InputIdentityLease> subtitles;
  QTimer timer;
  QElapsedTimer clock;
  Reply reply;
  std::function<Result()> poll;

  VideoSession *video() const {
    return background ? background : window ? window->findChild<VideoSession *>("videoSession") : nullptr;
  }
  QJsonObject state() const {
    QJsonObject result{{"asset_id", assetId}, {"kind", kind}, {"path", path},
                       {"host_pid", QCoreApplication::applicationPid()},
                       {"mode", background ? "background" : "window"}};
    if (kind == "video") {
      const auto s = video()->status();
      result["position"] = position(s);
      result["duration"] = s.value("duration").toDouble();
      result["paused"] = s.value("pause").toBool();
      result["speed"] = s.value("speed").toDouble();
      result["volume"] = s.value("volume").toDouble();
      result["muted"] = s.value("mute").toBool();
      result["subtitles_visible"] = s.value("sub-visibility").toBool();
      result["subtitle_delay_ms"] = qRound64(s.value("sub-delay").toDouble() * 1000);
      result["repeat_file"] = s.value("loop-file").toString() == "inf";
      result["chapter"] = s.value("chapter", -1).toInt();
      result["indexed"] = s.value("indexed-review").toBool();
      result["filters"] = filterValues(s, background);
      result["loop_a"] = loopTime(s, "ab-loop-a");
      result["loop_b"] = loopTime(s, "ab-loop-b");
      result["audio_id"] = selectedTrack(s, "audio").value("id", 0).toInt();
      result["subtitle_id"] = selectedTrack(s, "sub").value("id", 0).toInt();
    }
    return result;
  }
  void current() const {
    require(input && revision == desktop.sessionRevision() && desktop.asset().value("path") == path,
            ErrorCode::InputChanged, "The active asset changed; open it again");
    if (kind == "video")
      require(video() && video()->source() == path, ErrorCode::InputChanged, "Player source changed");
  }
  QObject *pane() const {
    return window ? window->findChild<QObject *>("videoPlayerPane") : nullptr;
  }
  QJsonObject windowState() const {
    auto rect = [](const QRect &r) {
      return QJsonObject{{"x",r.x()},{"y",r.y()},{"width",r.width()},{"height",r.height()}};
    };
    QJsonArray displays;
    for (auto *screen : QGuiApplication::screens())
      displays.append(QJsonObject{{"id",displayId(screen)}, {"name",screen->name()}, {"geometry",rect(screen->geometry())},
        {"available",rect(screen->availableGeometry())},{"scale",screen->devicePixelRatio()},
        {"primary",screen==QGuiApplication::primaryScreen()}});
    QJsonObject res{{"screens",displays},{"has_window",bool(window)},{"host_pid",QCoreApplication::applicationPid()}};
    if (!window) return res;
    const auto *playerPane = pane();
    const auto mode = playerPane ? playerPane->property("mode").toString() : QStringLiteral("normal");
    auto *w = window.data();
    if (mode == "hdr" && playerPane) {
      if (auto *hdr = qobject_cast<QQuickWindow *>(playerPane->property("hdrHost").value<QObject *>()))
        w = hdr;
    }
    res["mode"] = mode == "full" ? "fullscreen" : mode;
    res["screen_id"] = displayId(w->screen());
    res["visible"] = w->isVisible();
    res["fullscreen"] = w->visibility() == QWindow::FullScreen;
    const auto hwnd = w->handle() ? reinterpret_cast<HWND>(w->winId()) : nullptr;
    res["maximized"] = hwnd && IsZoomed(hwnd);
    res["minimized"] = hwnd && IsIconic(hwnd);
    res["topmost"] = hwnd && (GetWindowLongPtrW(hwnd,GWL_EXSTYLE) & WS_EX_TOPMOST);
    RECT outer{};
    if (hwnd && GetWindowRect(hwnd,&outer))
      res["native_outer_px"] = rect(QRect(outer.left,outer.top,outer.right-outer.left,outer.bottom-outer.top));
    res["geometry"] = rect(w->geometry());
    res["frame_geometry"] = rect(w->frameGeometry());
    return res;
  }
  void windowControl(const QJsonObject &r) {
    require(window, ErrorCode::InvalidArgument, "Open the video with open_asset(window=True) to control its window");
    require(!assetId.isEmpty() && r["asset_id"].toString()==assetId, ErrorCode::InputChanged, "Video asset ID is stale");
    current();
    auto *p = pane();
    require(kind=="video" && p, ErrorCode::InvalidArgument, "Window controls require a video");
    const auto mode = p->property("mode").toString();
    require(mode!="hdr",ErrorCode::InvalidArgument,"Exit dedicated HDR output before changing window placement");
    const auto action = r["action"].toString();
    std::function<bool(const QJsonObject &)> settled;
    if (action=="normal" || action=="fullscreen" || action=="mini") {
      const auto target = action=="fullscreen" ? QStringLiteral("full") : action;
      if (mode!=target)
        require(QMetaObject::invokeMethod(p,"changeMode",Q_ARG(QVariant,QVariant(target))),
                ErrorCode::Io,"Player mode switch unavailable");
      settled = [action](const auto &s) { return s["mode"]==action && s["visible"].toBool() &&
        (action!="mini" || s["topmost"].toBool()) && (action=="fullscreen")==s["fullscreen"].toBool(); };
    } else if (action=="move") {
      QScreen *dest = nullptr;
      const auto id = r["screen_id"].toString();
      for (auto *s : QGuiApplication::screens()) if (!id.isEmpty() && displayId(s)==id) dest=s;
      require(dest, ErrorCode::InvalidArgument, "Unknown display ID");
      if (window->screen()!=dest) {
        const bool maximized = window->visibility()==QWindow::Maximized;
        if (maximized) window->showNormal();
        const auto available = dest->availableGeometry();
        const auto size = window->size().boundedTo(available.size());
        window->setScreen(dest);
        if (mode=="full") window->setGeometry(dest->geometry());
        else {
          window->setGeometry(QRect(available.center()-QPoint(size.width()/2,size.height()/2),size));
          if (maximized) window->showMaximized();
        }
      }
      settled = [id,mode](const auto &s) { return s["screen_id"]==id &&
        (mode!="full" || s["fullscreen"].toBool()) && (mode!="mini" || s["topmost"].toBool()); };
    } else {
      require(action=="resize" && mode=="mini",ErrorCode::InvalidArgument,"Resize requires mini mode");
      const QSize size(r["width"].toInt(-1),r["height"].toInt(-1));
      const auto available=window->screen()->availableGeometry();
      require(size.width()>=320 && size.height()>=180 && available.size().expandedTo(size)==available.size(),
              ErrorCode::InvalidArgument,"Mini client size must fit this screen and be at least 320 by 180 logical pixels");
      window->resize(size);
      settled = [size](const auto &s) { const auto g=s["geometry"].toObject();
        return g["width"].toInt()==size.width() && g["height"].toInt()==size.height(); };
    }
    poll = [this,settled]() -> Result {
      current();
      const auto s=windowState();
      if (settled(s)) return s;
      return {};
    };
  }
  void tick() {
    Result result;
    try {
      result = poll();
      if (!result && clock.elapsed() >= 15000)
        result = failure(ErrorCode::Cancelled, "Timed out waiting for the player or asset");
    } catch (const skeletal::Failure &e) { result = image_error_json(e); }
      catch (const std::exception &e) { result = failure(ErrorCode::Io, e.what()); }
    if (!result) return;
    timer.stop();
    poll = {};
    auto done = std::move(reply);
    done(*result);
  }
  void open(const QJsonObject &r, std::optional<bool> paused = {}) {
    const auto requested = r["path"].toString();
    auto lease = InputIdentityLease::open(scope, requested.toStdWString());
    const auto file = QDir::fromNativeSeparators(QString::fromStdWString(lease.snapshot().resolved_path.wstring()));
    const auto type = Desktop::assetKind(file);
    require(type == "video" || type == "image", ErrorCode::InvalidArgument,
            "open_asset supports local video and image assets");
    input.emplace(std::move(lease));
    if (path != file) subtitles.clear();
    path = file;
    kind = type;
    assetId.clear();
    const auto previous = desktop.sessionRevision();
    desktop.openPath(path);
    if (background && !paused.has_value()) paused = true;
    poll = [this, previous, paused, pauseRequested = false]() mutable -> Result {
      if (desktop.sessionRevision() == previous) {
        require(desktop.error().isEmpty(), ErrorCode::Io, "Desktop could not open the requested asset");
        return {};
      }
      require(desktop.asset().value("path") == path, ErrorCode::InputChanged, "Asset open was superseded");
      if (background) background->setSource(kind == "video" ? path : QString());
      if (kind == "video") {
        auto *v = video();
        if (!v || v->source() != path) return {};
        require(v->error().isEmpty(), ErrorCode::Io, qPrintable(v->error()));
        const auto s = v->status();
        if (s.value("video-params").toMap().isEmpty() || !s.contains("time-pos") ||
            !s.contains("pause") || s.value("duration").toDouble() <= 0) return {};
        if (paused && s.value("pause").toBool() != *paused) {
          if (!pauseRequested) v->setOption("pause", *paused ? "yes" : "no");
          pauseRequested = true;
          return {};
        }
      } else {
        auto *image = static_cast<ImageSession *>(desktop.image());
        require(image->error().isEmpty(), ErrorCode::Io, qPrintable(image->error()));
        if (image->busy() || image->path() != path) return {};
      }
      revision = desktop.sessionRevision();
      assetId = QUuid::createUuid().toString(QUuid::WithoutBraces);
      return state();
    };
  }
  void capture(VideoSession *v, const QJsonObject &r) {
    require(r["output"].isString() && QFileInfo(r["output"].toString()).suffix().toLower() == "png"
            && r["subtitles"].isBool(), ErrorCode::InvalidArgument, "Supply a PNG output path and subtitles flag");
    const auto output = std::filesystem::path(r["output"].toString().toStdWString());
    const auto snapshot = input->snapshot();
    require(!inspect_output(scope, output, snapshot, ConflictPolicy::Skip).exists,
            ErrorCode::Conflict, "Screenshot output already exists");
    auto dir = std::make_shared<QTemporaryDir>();
    require(dir->isValid(), ErrorCode::Io, "Cannot create screenshot staging directory");
    auto saved = std::make_shared<QString>();
    auto error = std::make_shared<QString>();
    // Limit the completion signal to this request, including concurrent UI screenshots.
    auto guard = std::make_shared<QObject>();
    connect(v, &VideoSession::screenshotSaved, guard.get(), [dir, saved](const QString &file) {
      if (QFileInfo(file).absolutePath() == dir->path()) *saved = file;
    });
    connect(v, &VideoSession::screenshotFailed, guard.get(), [dir, error](const QString &file, const QString &reason) {
      if (QFileInfo(file).absolutePath() == dir->path()) *error = reason;
    });
    v->screenshot(QUrl::fromLocalFile(dir->path()), r["subtitles"].toBool());
    auto worker = std::make_shared<std::future<QJsonObject>>();
    poll = [this, saved, error, dir, guard, output, snapshot, worker]() -> Result {
      current();
      require(error->isEmpty(), ErrorCode::Io, qPrintable(*error));
      if (saved->isEmpty()) return {};
      if (!worker->valid()) {
        *worker = std::async(std::launch::async, [s = scope, file = *saved, dir, output, snapshot] {
          QFile png(file);
          require(png.open(QIODevice::ReadOnly) && png.size() > 0 && png.size() <= 256 * 1024 * 1024,
                  ErrorCode::Io, "Cannot read bounded screenshot");
          const auto bytes = png.readAll();
          require(bytes.size() == png.size() && bytes.startsWith("\x89PNG\r\n\x1a\n"),
                  ErrorCode::Io, "Incomplete PNG screenshot");
          OutputTransaction transaction(s, output, snapshot, ConflictPolicy::Skip);
          transaction.write(std::as_bytes(std::span(bytes.constData(), size_t(bytes.size()))));
          const auto commit = transaction.commit();
          require(!commit.skipped, ErrorCode::Conflict, "Screenshot destination was taken");
          return QJsonObject{{"output", QString::fromStdWString(commit.path.wstring())}, {"bytes", bytes.size()}};
        });
      }
      if (worker->wait_for(std::chrono::seconds(0)) != std::future_status::ready) return {};
      return worker->get();
    };
  }
  void playback(const QString &op, const QJsonObject &r) {
    require(!assetId.isEmpty() && r["asset_id"].toString() == assetId, ErrorCode::InputChanged,
            "Use the asset_id returned by open_asset");
    current();
    require(kind == "video", ErrorCode::InvalidArgument, "This operation requires an open video");
    auto *v = video();
    if (op == "desktop.state") { poll = [this] { return state(); }; return; }
    if (op == "desktop.capture") { capture(v, r); return; }
    const auto before = v->status();
    const auto start = position(before);
    const auto action = r["action"].toString();
    std::function<bool(const QVariantMap &)> ready;
    if (op == "desktop.playlist") {
      const auto *list = static_cast<const FileList *>(desktop.videos());
      const auto at = list->indexOf(path);
      if (action == "list") {
        const int offset = r["offset"].toInt(-1);
        require(offset >= 0, ErrorCode::InvalidArgument, "Invalid playlist offset");
        poll = [this, list, offset]() -> Result {
          current();
          const auto index = list->indexOf(path);
          if (index < 0 && desktop.busy()) return {};
          require(index >= 0, ErrorCode::InvalidArgument, "Current folder playlist is unavailable");
          QJsonArray entries;
          for (int i = offset; i < list->rowCount() && entries.size() < 100; ++i)
            entries.append(QJsonObject{{"index", i}, {"path", list->at(i)->path}});
          return QJsonObject{{"items", entries}, {"current", index}, {"total", list->rowCount()}};
        };
        return;
      }
      require(at >= 0, ErrorCode::InvalidArgument, "List the current video folder first");
      require(action == "previous" || action == "next" || action == "select", ErrorCode::InvalidArgument, "Invalid playlist action");
      const int to = action == "select" ? r["index"].toInt(-1) : at + (action == "next" ? 1 : -1);
      require(list->at(to), ErrorCode::InvalidArgument, "Playlist index outside the current folder");
      open({{"path", list->at(to)->path}}, before.value("pause").toBool());
      return;
    } else if (op == "desktop.tracks") {
      QJsonArray tracks;
      for (const auto &entry : before.value("track-list").toList()) {
        const auto t = entry.toMap();
        if (t.value("type") != "audio" && t.value("type") != "sub") continue;
        QJsonObject row;
        for (auto key : {"type", "id", "title", "lang", "codec", "selected", "external"})
          if (t.contains(key)) row[key] = QJsonValue::fromVariant(t.value(key));
        tracks.append(row);
      }
      poll = [tracks]{ return QJsonObject{{"tracks", tracks}}; }; return;
    } else if (op == "desktop.track") {
      const auto type = r["type"].toString();
      const int id = r["id"].toInt(-1);
      require(type == "audio" || type == "sub", ErrorCode::InvalidArgument, "Invalid track type");
      bool found = id == 0;
      for (const auto &entry : before.value("track-list").toList()) {
        const auto t = entry.toMap();
        if (t.value("type") == type && t.value("id").toInt() == id) found = true;
      }
      require(found && id >= 0, ErrorCode::InvalidArgument, "Unknown track ID");
      v->setOption(type == "audio" ? "aid" : "sid", id ? QString::number(id) : QStringLiteral("no"));
      ready = [type,id](const auto &s){return selectedTrack(s,type).value("id",0).toInt() == id;};
    } else if (op == "desktop.subtitle") {
      auto lease = InputIdentityLease::open(scope, r["path"].toString().toStdWString());
      const auto file = QDir::fromNativeSeparators(QString::fromStdWString(lease.snapshot().resolved_path.wstring()));
      require(QStringList{"srt","ass","ssa","vtt"}.contains(QFileInfo(file).suffix().toLower()) &&
              lease.snapshot().size_bytes <= 16*1024*1024, ErrorCode::InvalidArgument, "Expected a local text subtitle up to 16 MiB");
      int existing = 0;
      for (const auto &entry : before.value("track-list").toList()) {
        const auto t = entry.toMap();
        if (t.value("type") == "sub" && QFileInfo(t.value("external-filename").toString()) == QFileInfo(file))
          existing = t.value("id").toInt();
      }
      require(subtitles.contains(file) || subtitles.size() < 32, ErrorCode::ResourceLimit, "External subtitle limit reached");
      subtitles.insert_or_assign(file,std::move(lease));
      if (existing) v->setOption("sid",existing); else v->addSubtitle(QUrl::fromLocalFile(file));
      ready = [file](const auto &s){return QFileInfo(selectedTrack(s,"sub").value("external-filename").toString()) == QFileInfo(file);};
    } else if (op == "desktop.loop") {
      if (action == "clear") {
        v->clearLoop();
        ready = [](const auto &s){return loopTime(s,"ab-loop-a").isNull() && loopTime(s,"ab-loop-b").isNull();};
      } else {
        const double a = numeric(r,"a"), b = numeric(r,"b");
        require(action == "set" && a >= 0 && b-a >= .000001 && b <= before.value("duration").toDouble(),
                ErrorCode::InvalidArgument, "Expected 0 <= A < B <= duration");
        v->loop(a,b);
        ready = [a,b](const auto &s){return std::abs(loopTime(s,"ab-loop-a").toDouble(-1)-a)<.000001 &&
                                            std::abs(loopTime(s,"ab-loop-b").toDouble(-1)-b)<.000001;};
      }
    } else if (op == "desktop.filter") {
      const auto name = r["name"].toString();
      const auto value = numeric(r,"value");
      require(adjustments.contains(name) && (name == "sharpen" ? value >= 0 && value <= 2 :
              value >= -100 && value <= 100 && value == std::round(value)), ErrorCode::InvalidArgument, "Invalid filter or value");
      const bool changed = filterValues(before,background)[name].toDouble() != value;
      if (changed) v->setOption(name,value);
      ready = [this,v,name,value,start, refresh = changed && background && before.value("pause").toBool()](const auto &s) mutable {
        if (std::abs(filterValues(s,background)[name].toDouble()-value)>1e-6) return false;
        if (refresh) {
          refresh = false;
          v->seek(start,false);
          return false;
        }
        return !v->seekPending();
      };
    } else if (op == "desktop.chapters") {
      const auto chapters = before.value("chapter-list").toList();
      if (action == "list") {
        poll = [chapters, before] { return QJsonObject{
          {"chapters", QJsonArray::fromVariantList(chapters)}, {"current", before.value("chapter", -1).toInt()}}; };
        return;
      }
      const int index = r["index"].toInt(-1);
      require(action == "select" && index >= 0 && index < chapters.size(),
              ErrorCode::InvalidArgument, "Chapter index outside the current video");
      v->seek(chapters.at(index).toMap().value("time").toDouble(), false);
      ready = [v,index](const auto &s) { return !v->seekPending() && s.value("chapter", -1).toInt() == index; };
    } else if (op == "desktop.toggle") {
      const auto name = r["name"].toString();
      require((name == "mute" || name == "sub-visibility" || name == "loop-file") && r["enabled"].isBool(),
              ErrorCode::InvalidArgument, "Expected a playback switch and boolean");
      const bool enabled = r["enabled"].toBool();
      v->setOption(name, enabled ? (name == "loop-file" ? "inf" : "yes") : "no");
      ready = [name,enabled](const auto &s) {
        return s.contains(name) && (name == "loop-file" ? s.value(name).toString() == "inf" : s.value(name).toBool()) == enabled;
      };
    } else if (op == "desktop.control" && action == "subtitle_delay_ms") {
      const auto value = numeric(r, "value");
      require(value >= -60000 && value <= 60000 && value == std::round(value),
              ErrorCode::InvalidArgument, "Subtitle delay must be an integer in milliseconds from -60000 to 60000");
      v->setOption("sub-delay", value / 1000);
      ready = [value](const auto &s) { return s.contains("sub-delay") && qRound64(s.value("sub-delay").toDouble()*1000) == value; };
    } else if (op == "desktop.step") {
      require(before.value("pause").toBool(), ErrorCode::InvalidArgument, "Pause before frame stepping");
      const auto forward = r["direction"].toString() == "next";
      require(forward || r["direction"] == "previous", ErrorCode::InvalidArgument, "Invalid frame direction");
      require(forward ? !before.value("eof-reached").toBool() : start > 0, ErrorCode::InvalidArgument, "At video boundary");
      v->step(forward);
      const auto target = v->indexedStepTarget();
      ready = [start, forward, target, v](const auto &s) {
        const auto seconds = position(s);
        // mpv can publish the old PTS again with a different double rounding.
        // An indexed step is complete only at the requested source frame.
        if (target >= 0) return !v->seekPending() && std::abs(seconds - target) <= .000001;
        return (forward ? seconds - start : start - seconds) > .000001;
      };
    } else if (op == "desktop.control" && (action == "play" || action == "pause")) {
      const bool pause = action == "pause";
      if (before.value("pause").toBool() != pause) v->togglePause();
      ready = [pause](const auto &s) { return s.value("pause").toBool() == pause; };
    } else if (op == "desktop.control" && (action == "seek" || action == "seek_relative")) {
      const auto offset = numeric(r, "value");
      const auto seconds = action == "seek_relative" ? start + offset : offset;
      require(seconds >= 0 && seconds < before.value("duration").toDouble(), ErrorCode::InvalidArgument, "Seek outside duration");
      v->seek(seconds, false);
      ready = [v](const auto &) { return !v->seekPending(); };
    } else if (op == "desktop.control" && (action == "speed" || action == "volume")) {
      const auto value = numeric(r, "value");
      const bool rate = action == "speed";
      require(rate ? value >= .1 && value <= 8 : value >= 0 && value <= 100,
              ErrorCode::InvalidArgument, "Speed must be 0.1 to 8 or volume 0 to 100");
      v->setOption(action, value);
      ready = [v, action, value](const auto &s) {
        return !v->optionPending(action) && std::abs(s.value(action).toDouble() - value) < .0001;
      };
    } else throw skeletal::Failure(ErrorCode::InvalidArgument, "Unknown playback operation");
    poll = [this, ready, previousError = v->error()]() -> Result {
      current();
      const auto error = video()->error();
      require(error.isEmpty() || error == previousError, ErrorCode::Io, qPrintable(error));
      if (!ready(video()->status())) return {};
      return state();
    };
  }
public:
  Session(Desktop &d, QQuickWindow *w, VideoSession *v, AccessScope policy)
      : desktop(d), window(w), background(v), scope(std::move(policy)) {
    timer.setInterval(10);
    connect(&timer, &QTimer::timeout, this, [this] { tick(); });
    connect(&desktop, &Desktop::changed, this, [this] {
      if (!assetId.isEmpty() && revision != desktop.sessionRevision()) {
        assetId.clear();
        input.reset();
        subtitles.clear();
      }
    });
  }
  void request(const QJsonObject &r, Reply done) {
    if (reply) { done(failure(ErrorCode::Conflict, "Another desktop operation is pending")); return; }
    try {
      require(r["version"] == 1, ErrorCode::InvalidArgument, "Desktop request version must be 1");
      const auto op = r["operation"].toString();
      if (op == "desktop.close" && (background || QCoreApplication::arguments().contains("--mcp-window"))) {
        done({{"closing", true}});
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        return;
      }
      if (op == "desktop.open") open(r);
      else if (op == "desktop.window_state") {
        if (!r["asset_id"].toString().isEmpty()) {
          require(r["asset_id"]==assetId,ErrorCode::InputChanged,"Asset ID is stale");
          current();
        }
        poll = [this]{ return windowState(); };
      }
      else if (op == "desktop.window_control") windowControl(r);
      else playback(op, r);
      reply = std::move(done);
      clock.start();
      timer.start();
      tick();
    } catch (const skeletal::Failure &e) { done(image_error_json(e)); }
      catch (const std::exception &e) { done(failure(ErrorCode::Io, e.what())); }
  }
};
}

void attachDesktopMcp(Desktop &desktop, QQuickWindow *window, VideoSession *video) {
  auto *jobs = static_cast<ImageJobs *>(desktop.imageJobs());
  QObject::connect(jobs, &ImageJobs::endpointEnabled, jobs, [=, &desktop](ImageEndpoint *endpoint) {
    auto session = std::make_shared<Session>(desktop, window, video, parse_image_scope(QJsonObject::fromVariantMap(jobs->scope())));
    endpoint->desktopRequest = [session](const auto &request, Reply done) { session->request(request, std::move(done)); };
  });
}

namespace {
QJsonObject readScope(const QString &scopePath) {
  if (!QFileInfo(scopePath).isAbsolute()) return {};
  QFile file(scopePath);
  if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) return {};
  auto policy = QJsonDocument::fromJson(file.readAll()).object();
  // The host assigns the revision from the full policy, just like the settings dialog.
  policy["policy_revision"] = "background";
  try { (void)parse_image_scope(policy); }
  catch (const std::exception &e) { std::cerr << e.what() << '\n'; return {}; }
  policy.remove("policy_revision");
  return policy;
}
void startEndpoint(Desktop &desktop, const QJsonObject &policy) {
  auto *jobs = static_cast<ImageJobs *>(desktop.imageJobs());
  auto enabling = std::make_shared<bool>(false);
  const auto enable = [jobs, enabling] {
    if (*enabling || jobs->busy()) return;
    *enabling = true;
    if (jobs->configured()) jobs->setMcpEnabled(true);
    if (!jobs->mcpEnabled()) {
      std::cerr << jobs->error().toUtf8().constData() << '\n';
      QTimer::singleShot(0, qApp, [] { QCoreApplication::exit(3); });
      return;
    }
    std::cout << QJsonDocument(QJsonObject{{"endpoint", jobs->endpoint()}}).toJson(QJsonDocument::Compact).constData() << std::endl;
  };
  jobs->configureScope(policy.toVariantMap());
  QObject::connect(jobs, &ImageJobs::changed, jobs, enable);
  QTimer::singleShot(0, jobs, enable);
}
}
bool startScopedEndpoint(Desktop &desktop, const QString &scopePath) {
  const auto policy = readScope(scopePath);
  if (policy.isEmpty()) return false;
  startEndpoint(desktop, policy);
  return true;
}
bool watchMcpParent(unsigned long parentPid) {
  const auto parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
  if (!parent) return false;
  auto *notifier = new QWinEventNotifier(parent, qApp);
  QObject::connect(notifier, &QWinEventNotifier::activated, qApp, &QCoreApplication::quit);
  QObject::connect(notifier, &QObject::destroyed, [parent]{ CloseHandle(parent); });
  return true;
}
int runBackgroundMcp(const QString &scopePath, unsigned long parentPid) {
  const auto policy = readScope(scopePath);
  if (policy.isEmpty() || !watchMcpParent(parentPid)) return 2;
  QTemporaryDir settings;
  if (!settings.isValid()) return 2;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
  std::unique_ptr<QLockFile> sessionLock;
  if (qEnvironmentVariableIsEmpty("FORMAVIEW_IMAGE_STATE_ROOT")) {
    auto key = QCryptographicHash::hash(QJsonDocument(policy).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
    QDir root(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/mcp-background/" + key);
    if (!root.mkpath(".")) return 2;
    // MSIX clients can virtualize AppData into the package's LocalCache.
    // Resolve only this host-generated journal path. Input and output paths
    // still go through the normal native scope and reparse checks.
    for (QDir part = root; !part.isRoot(); part = QFileInfo(part.path()).dir()) {
      const QFileInfo info(part.path());
      if (info.isJunction() || info.isSymLink()) return 2;
    }
    std::error_code ec;
    auto actual = std::filesystem::canonical(root.path().toStdWString(), ec).wstring();
    if (ec) return 2;
    if (actual.starts_with(L"\\\\?\\")) actual.erase(0, 4);
    root.setPath(QDir::fromNativeSeparators(QString::fromStdWString(actual)));
    // Each concurrent client gets a separate player and durable job journal.
    // Reuse idle slots to avoid a new history directory on every connection.
    for (int slot = 0; slot < 32; ++slot) {
      const auto dir = root.filePath(QString::number(slot));
      auto lock = std::make_unique<QLockFile>(dir + ".lock");
      lock->setStaleLockTime(0);
      if (lock->tryLock()) {
        qputenv("FORMAVIEW_IMAGE_STATE_ROOT", dir.toUtf8());
        sessionLock = std::move(lock);
        break;
      }
      if (lock->error() != QLockFile::LockFailedError) return 2;
    }
    if (!sessionLock) return 2;
  }
  Desktop desktop;
  VideoSession player;
  player.enableBackgroundPlayback();
  attachDesktopMcp(desktop, nullptr, &player);
  auto *jobs = static_cast<ImageJobs *>(desktop.imageJobs());
  startEndpoint(desktop, policy);
  const auto result = QCoreApplication::exec();
  jobs->setMcpEnabled(false);
  return result;
}
