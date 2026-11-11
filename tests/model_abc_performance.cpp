// Opt-in: formaview_model_abc_performance INPUT.abc OUTPUT.json
// INPUT is only read. Output must be outside the input directory.
#include "model_view.hpp"
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QStyleHints>
#include <QtMath>
#include <QQuickGraphicsConfiguration>
#include <QSysInfo>
#include <QThread>
#include <QTimer>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQmlComponent>
#include <QQuickStyle>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <iostream>
#include <mutex>
#include <unordered_map>

namespace {
using Clock = std::chrono::steady_clock;
double ms(Clock::duration d) {
  return std::chrono::duration<double, std::milli>(d).count();
}
QJsonObject distribution(std::vector<double> v) {
  if (v.empty()) return {{"count", 0}};
  const QJsonArray raw = QJsonArray::fromVariantList(QVariantList(v.begin(), v.end()));
  std::sort(v.begin(), v.end());
  const auto at = [&](double p) { return v[static_cast<std::size_t>(std::ceil(p*v.size()))-1]; };
  return {{"count", static_cast<int>(v.size())}, {"raw_ms", raw},
          {"p50_ms", at(.5)}, {"p95_ms", at(.95)}, {"max_ms", v.back()}};
}
void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}
void waitFor(const std::function<bool()> &ready, int timeout = 300000) {
  QEventLoop loop;
  QTimer poll, deadline;
  poll.setInterval(1); deadline.setSingleShot(true);
  QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (ready()) loop.quit(); });
  QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
  poll.start(); deadline.start(timeout);
  if (!ready()) loop.exec();
  require(ready(), "benchmark timed out");
}
ModelRenderState stateFor(const ModelDocument &d) {
  ModelRenderState s;
  s.bindings.assign(d.meshes.size(), -1); s.uvs.resize(d.meshes.size());
  s.textures.resize(d.meshes.size()); s.classes.resize(d.meshes.size());
  return s;
}
}
#include "model_abc_stability.hpp"
int main(int argc, char **argv) {
  QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
  QQuickStyle::setStyle("Basic");
  QGuiApplication app(argc, argv);
  app.styleHints()->setColorScheme(Qt::ColorScheme::Dark);
  try {
    require(argc >= 3 && argc <= 7,
            "Usage: formaview_model_abc_performance INPUT.abc OUTPUT.json [--qml] [--full] [--follow] [--soak=LOOPS]");
    const auto options=app.arguments().mid(3);
    int soakLoops{};
    for(const auto &option:options) {
      require(option=="--qml" || option=="--full" || option=="--follow" || option.startsWith("--soak="),"Unknown benchmark option");
      if(option.startsWith("--soak=")) {soakLoops=option.mid(7).toInt();require(soakLoops>=3 && soakLoops<=1000,"Soak loops must be 3..1000");}
    }
    const bool qml = options.contains("--qml"), full=options.contains("--full");
    const bool follow=options.contains("--follow");
    require(!soakLoops || (qml && follow),"Soak requires full QML and visible geometry");
    const QFileInfo input(app.arguments()[1]), output(app.arguments()[2]);
    require(input.exists() && input.isFile(), "input does not exist");
    require(input.canonicalPath().compare(output.absolutePath(), Qt::CaseInsensitive) != 0,
            "benchmark output must be outside the input directory");
    const auto beforeSize = input.size();
    const auto beforeTime = input.lastModified();
    auto start = Clock::now();
    auto source = ModelAbcSource::open(input.absoluteFilePath(), {});
    const auto openMs = ms(Clock::now()-start);
    auto previous = source->read(0, {});
    const auto display = std::pair(previous->center, previous->radius);
    const auto count = source->times().size();
    require(count > 1, "benchmark requires animated input");
    QJsonObject evidence{{"schema_version",2}, {"full_timeline",full}, {"follow_all_geometry",follow}, {"qt", qVersion()}, {"os", QSysInfo::prettyProductName()},
        {"cpu_arch", QSysInfo::currentCpuArchitecture()},
        {"input", input.absoluteFilePath()}, {"input_bytes", beforeSize},
        {"input_modified", beforeTime.toUTC().toString(Qt::ISODateWithMs)},
        {"sha256", source->hash()}, {"archive_open_ms", openMs},
        {"samples", static_cast<int>(count)}, {"meshes", static_cast<int>(source->meshes().size())},
        {"points", static_cast<qint64>(previous->points)},
        {"triangles", static_cast<qint64>(previous->triangles)},
        {"boundary", "Read-only input; warm OS cache; passive Qt/D3D11 1280x720 MSAA4 viewport. "
         "Presented samples use submitted payload identity at frameSwapped, not timer progress. "
         "Excludes QML inspector overhead and physical monitor scanout."}};
    std::vector<double> reads, frames, streamFrames;
    std::shared_ptr<const formaview::render::Frame> previousFrame;
    int staticBuilds{};
    for (std::size_t i=1; i<std::min<std::size_t>(count, 61); ++i) {
      start=Clock::now();
      auto d=source->read(i, {}, display, previous);
      reads.push_back(ms(Clock::now()-start));
      start=Clock::now();
      auto frame=buildModelFrames(*d, stateFor(*d), i, {});
      frames.push_back(ms(Clock::now()-start));
      start=Clock::now();
      auto streamed=buildAbcStreamFrames(*d,stateFor(*d),i,{},previousFrame).model;
      streamFrames.push_back(ms(Clock::now()-start));
      if (!previousFrame || streamed->streamGeometry!=previousFrame->streamGeometry) ++staticBuilds;
      previousFrame=streamed;
      require(frame.model && frame.model->sourceTriangles == d->triangles,
              "benchmark frame lost source triangles");
      previous=std::move(d);
    }
    evidence["document_read"]=distribution(reads);
    evidence["frame_build"]=distribution(frames);
    evidence["stream_frame_build"]=distribution(streamFrames);
    evidence["stream_static_builds"]=staticBuilds;
    previousFrame.reset();
    {
      auto archive=formaview::cache::Archive::open(std::filesystem::path(input.absoluteFilePath().toStdWString()));
      std::vector<double> decode, finalize;
      for(std::size_t i=0;i<30;++i) {
        start=Clock::now();
        for(std::size_t m=0;m<archive.meshes().size();++m) {
          const auto sample=archive.read(m,archive.times().at(i));
        }
        decode.push_back(ms(Clock::now()-start));
        auto copy=*previous;
        for(auto &m:copy.meshes) { m.uvBounds.clear(); m.invalidUVs.clear(); }
        start=Clock::now();
        finalizeModelGeometry(copy,{},true);
        finalize.push_back(ms(Clock::now()-start));
      }
      evidence["archive_decode"]=distribution(decode);
      evidence["geometry_finalize"]=distribution(finalize);
    }
    previous.reset(); source.reset();

    ModelSession session;
    // The actual production pane and complete inspector use this session;
    // no Desktop/settings/history writer is created by the probe.
    QQmlEngine engine;
    std::unique_ptr<QObject> qmlRoot;
    QQuickWindow window;
    QQuickGraphicsConfiguration graphics; graphics.setTimestamps(true);
    window.setGraphicsConfiguration(graphics);
    window.setFlags(Qt::Window | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
    window.setTitle("FormaView ABC read-only performance probe");
    window.resize(1280,720);
    std::unique_ptr<ModelView> isolatedView;
    ModelView *view{};
    if (qml) {
      window.resize(1600,824);
      engine.rootContext()->setContextProperty("abcSession", &session);
      QQmlComponent component(&engine);
      component.setData(R"(
import QtQuick
import FormaView.AbcPerfUI
Rectangle {
    color: Theme.panel
    width: 1600; height: 824
    QtObject { id: adapter; readonly property var model: abcSession }
    ModelPane { width: 1280; height: parent.height; controller: adapter }
    ModelInspector { x: 1288; width: 312; height: parent.height; controller: adapter }
})", QUrl("qrc:/abc-performance.qml"));
      qmlRoot.reset(component.create());
      if (!qmlRoot) throw std::runtime_error(component.errorString().toStdString());
      auto item=qobject_cast<QQuickItem *>(qmlRoot.get());
      require(item, "QML probe root is not an item");
      item->setParentItem(window.contentItem());
      view=item->findChild<ModelView *>("modelViewport");
      require(view && item->findChild<QObject *>("modelInspector"), "complete QML pane/inspector missing");
      view->setFixedColorBufferWidth(1280); view->setFixedColorBufferHeight(720);
      evidence["boundary"]="Production ModelPane + full ModelInspector (scene tab, default expanded sections) + CacheTimeline; D3D11/MSAA4, fixed 1280x720 render buffer in 1600x824 window; submitted payload at swap; warm OS cache; excludes physical scanout.";
    } else {
      isolatedView=std::make_unique<ModelView>(window.contentItem()); view=isolatedView.get();
      view->setSize({1280,720}); view->setSession(&session);
    }
    evidence["full_qml_inspector"]=qml;
    std::unique_ptr<AbcStability> stability;
    std::atomic<AbcStability *> stabilityObserver{};
    struct Shared {
      std::mutex mutex;
      std::unordered_map<const formaview::render::Frame *, int> samples;
      std::shared_ptr<const formaview::render::Frame> submitted;
      std::shared_ptr<const formaview::render::Frame> presented;
      std::vector<double> intervals, prepare, record, gpu;
      QJsonArray order;
      Clock::time_point last;
      std::uint64_t uploaded{}, peakOwned{};
      int lastSample{-1};
      int lastTracked{};
      QJsonArray trackedOrder;
      bool tracking{};
      bool measuring{};
    } shared;
    QObject::connect(&session,&ModelSession::frameChanged,&session,[&] {
      if (!session.frame()) return;
      if (follow && session.frame()->hasDisplayBounds) {
        const auto &f=*session.frame();
        const auto center=(f.displayLow+f.displayHigh)*.5F;
        const auto radius=std::max((f.displayHigh-f.displayLow).length()*.5F,.0001F);
        const auto aspect=std::max(view->width(),1.)/std::max(view->height(),1.);
        const auto half=std::min(qDegreesToRadians(20.),std::atan(std::tan(qDegreesToRadians(20.))*aspect));
        view->restoreCamera({{"x",center.x()},{"y",center.y()},{"z",center.z()},
            {"distance",radius*1.05/std::sin(half)},{"yaw",25.},{"pitch",12.},{"automatic",false}});
      }
      if(auto *probe=stabilityObserver.load()) {probe->landed(session.frame());return;}
      std::lock_guard lock(shared.mutex);
      shared.samples[session.frame().get()]=session.cacheInfo().value("sample").toInt();
    });
    const auto submitted=view->frameSubmitted;
    view->frameSubmitted=[&,submitted](auto f) {
      submitted(f);
      if(auto *probe=stabilityObserver.load()) {probe->submitted(std::move(f));return;}
      std::lock_guard lock(shared.mutex); shared.submitted=std::move(f);
    };
    view->observe=[&](auto o) {
      if(auto *probe=stabilityObserver.load()) {probe->observe(o);return;}
      std::lock_guard lock(shared.mutex);
      if (!shared.measuring) return;
      shared.prepare.push_back(o.prepare_ms); shared.record.push_back(o.record_ms);
      if(o.gpu_seconds>0) shared.gpu.push_back(o.gpu_seconds*1000);
      shared.uploaded+=o.uploaded_bytes;
      shared.peakOwned=std::max(shared.peakOwned,o.owned_gpu_bytes);
    };
    QObject::connect(&window,&QQuickWindow::frameSwapped,&window,[&] {
      if(auto *probe=stabilityObserver.load()) {probe->swapped();return;}
      std::lock_guard lock(shared.mutex);
      if (!shared.submitted) return;
      shared.presented=shared.submitted;
      const auto it=shared.samples.find(shared.submitted.get());
      if (it==shared.samples.end()) return;
      if (shared.tracking && it->second!=shared.lastTracked) {
        shared.trackedOrder.append(it->second); shared.lastTracked=it->second;
      }
      if (!shared.measuring || it->second==shared.lastSample) return;
      const auto now=Clock::now();
      if (shared.lastSample>=0) shared.intervals.push_back(ms(now-shared.last));
      shared.last=now; shared.lastSample=it->second; shared.order.append(it->second);
    },Qt::DirectConnection);
    window.show(); session.open(input.absoluteFilePath());
    waitFor([&] { return !session.busy() && !session.preparing() && session.frame(); });
    require(session.error().isEmpty() && session.frameError().isEmpty(), "open/frame failed");
    waitFor([&] { std::lock_guard lock(shared.mutex); return bool(shared.submitted); });
    QList<QPointer<QObject>> inspectorRows;
    if (qml) {
      const auto collect=[&](const auto &self,QQuickItem *item)->void {
        if(item->objectName()=="cacheMeshReadout") inspectorRows.append(item);
        for(auto *child:item->childItems()) self(self,child);
      };
      collect(collect,qobject_cast<QQuickItem *>(qmlRoot.get()));
    }
    const auto &times=session.documentSnapshot()->geometryCache->times();
    const double sourceRate=(count-1)/(times.back()-times.front());
    session.setSpeed(30.0/sourceRate); session.setLoop(false);
    QJsonArray runs;
    if(soakLoops) {
      require(QDir().mkpath(output.absolutePath()),"soak output directory creation failed");
      session.setLoop(true);
      stability=std::make_unique<AbcStability>(session,output.absoluteFilePath(),soakLoops);
      stabilityObserver.store(stability.get());view->update();session.togglePlay();
      waitFor([&]{return stability->done() || !session.error().isEmpty() || !session.frameError().isEmpty();},soakLoops*12000+60000);
      require(stability->done() && session.error().isEmpty() && session.frameError().isEmpty(),"soak playback failed");
      if(session.playing())session.togglePlay();
      const auto stop=Clock::now();waitFor([&]{return ms(Clock::now()-stop)>=500 && !session.preparing();});
      evidence["stability"]=stability->finish();stabilityObserver.store(nullptr);
    } else
    for (int run=0;run<2;++run) {
      session.seekSample(0);
      waitFor([&] { return session.cacheInfo().value("framed").toBool() &&
          session.cacheInfo().value("sample").toInt()==0 && !session.preparing(); });
      waitFor([&] { std::lock_guard lock(shared.mutex); return shared.presented==session.frame(); });
      {
        std::lock_guard lock(shared.mutex);
        shared.intervals.clear(); shared.prepare.clear(); shared.record.clear(); shared.gpu.clear();
        shared.order={}; shared.uploaded=shared.peakOwned=0; shared.lastSample=-1; shared.measuring=true;
        shared.trackedOrder={}; shared.lastTracked=0; shared.tracking=true;
      }
      std::vector<double> eventGaps;
      auto heartbeatAt=Clock::now();
      QTimer heartbeat; heartbeat.setInterval(1);
      QObject::connect(&heartbeat,&QTimer::timeout,&session,[&] {
        const auto now=Clock::now(); eventGaps.push_back(ms(now-heartbeatAt)); heartbeatAt=now;
      });
      heartbeat.start();
      start=Clock::now(); session.togglePlay();
      const auto duration=full ? (count-1)*1000.0/30+100 : std::min(6000.0, (count-1)*1000.0/30);
      waitFor([&] { return ms(Clock::now()-start)>=duration || !session.playing(); });
      const auto elapsed=ms(Clock::now()-start);
      const auto stoppedCache=session.cacheInfo();
      heartbeat.stop();
      if(session.playing()) session.togglePlay();
      {
        std::lock_guard lock(shared.mutex); shared.measuring=false;
        const auto expected=std::min(static_cast<int>(count)-1, static_cast<int>(elapsed*30/1000));
        qint64 noninitial{};
        for(const auto &sample:shared.order) if(sample.toInt()!=0) ++noninitial;
        runs.append(QJsonObject{{"elapsed_ms",elapsed}, {"target_fps",30},
            {"expected_updates",expected}, {"presented_updates",shared.order.size()},
            {"missed_updates",std::max<qint64>(0,expected-shared.order.size())},
            {"effective_fps",shared.order.size()*1000.0/elapsed},
            {"noninitial_updates",noninitial},
            {"noninitial_fps",noninitial*1000.0/elapsed},
            {"requested_at_stop",stoppedCache.value("requested").toInt()},
            {"source_clock_drift_ms",elapsed-(stoppedCache.value("clock").toDouble()-times.front())*1000/session.speed()},
            {"gui_heartbeat_intervals",distribution(eventGaps)},
            {"sample_order",shared.order}, {"frame_intervals",distribution(shared.intervals)},
            {"prepare",distribution(shared.prepare)}, {"record",distribution(shared.record)},
            {"gpu",distribution(shared.gpu)}, {"uploaded_bytes",static_cast<qint64>(shared.uploaded)},
            {"peak_owned_gpu_bytes",static_cast<qint64>(shared.peakOwned)},
            {"cache",QJsonObject::fromVariantMap(session.cacheInfo())}});
      }
      // Do not classify an in-flight final sample as skipped. Preserve the
      // fixed-window metrics above, and separately drain the paused request.
      const auto requested=stoppedCache.value("requested").toInt();
      waitFor([&] {
        std::lock_guard lock(shared.mutex);
        const auto it=shared.samples.find(shared.presented.get());
        return !session.preparing() && session.cacheInfo().value("framed").toBool() &&
            it!=shared.samples.end() && it->second==requested;
      });
      auto result=runs.last().toObject();
      result["drain_ms"]=ms(Clock::now()-start)-elapsed;
      result["drained_sample"]=requested;
      {
        std::lock_guard lock(shared.mutex); shared.tracking=false;
        QJsonArray skipped;
        int last{};
        for(const auto &sample:shared.trackedOrder) {
          for(int index=last+1;index<sample.toInt();++index) skipped.append(index);
          last=sample.toInt();
        }
        result["skipped_sample_indices"]=skipped;
        result["presented_order_with_drain"]=shared.trackedOrder;
      }
      runs[runs.size()-1]=result;
    }
    evidence["playback"]=runs;
    if (qml) {
      const auto retained=std::count_if(inspectorRows.begin(),inspectorRows.end(),[](const auto &r){return !r.isNull();});
      evidence["inspector_rows_initial"]=inspectorRows.size();
      evidence["inspector_rows_retained"]=retained;
      std::cout<<"Inspector rows "<<inspectorRows.size()<<", retained "<<retained<<'\n';
      require(retained==inspectorRows.size() && retained>0,"QML inspector recreated its mesh rows");
      require(QDir().mkpath(output.absolutePath()),"screenshot directory creation failed");
      const auto screenshot=output.absolutePath()+"/"+output.completeBaseName()+".png";
      require(window.grabWindow().save(screenshot),"QML screenshot failed");
      evidence["screenshot"]=screenshot;
    }
    qmlRoot.reset(); isolatedView.reset(); window.hide(); window.releaseResources();
    const QFileInfo after(input.absoluteFilePath());
    require(after.size()==beforeSize && after.lastModified()==beforeTime, "input changed during benchmark");
    evidence["input_metadata_unchanged"]=true;
    require(QDir().mkpath(output.absolutePath()), "output directory creation failed");
    QFile out(output.absoluteFilePath()); require(out.open(QIODevice::WriteOnly),"output open failed");
    require(out.write(QJsonDocument(evidence).toJson())>0,"output write failed");
    std::cout<<"PASS read-only ABC benchmark\n";
  } catch(const std::exception &e) { std::cerr<<"FAIL "<<e.what()<<'\n'; return 1; }
}
