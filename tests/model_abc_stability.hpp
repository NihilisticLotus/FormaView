#pragma once
// Opt-in long-run observer. Immutable frame fingerprints and handle snapshots
// run on a bounded diagnostic worker, not the GUI or render thread.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#include <processsnapshot.h>
#include <winternl.h>
#include <QCryptographicHash>
#include <QSaveFile>
#include <deque>
#include <future>
#include <map>
#include <set>
#include <optional>

class AbcStability {
  using Clock=std::chrono::steady_clock;
  using Frame=formaview::render::Frame;
  struct Meta { int epoch{},sample{}; double landed{},requested{-1}; std::weak_ptr<const Frame> frame; };
  struct Event { Meta meta; double time{}; };
  struct Loop {
    double start{},end{}; bool intervened{};
    std::set<int> requests,presented;
    std::vector<double> intervals,latency,heartbeat;
    QJsonArray order;
  };
  struct Recent { double time{},interval{},latency{}; };
  ModelSession &session_;
  QFile raw_,loopsFile_,windows_,resources_,heartbeatFile_;
  QJsonArray loopsSummary_,windowSummary_,actions_,hashes_;
  QJsonObject lastResource_;
  Clock::time_point origin_=Clock::now(),lastHeartbeat_=origin_;
  std::mutex mutex_;
  std::unordered_map<const Frame *,Meta> metadata_;
  std::shared_ptr<const Frame> submittedFrame_;
  std::optional<Meta> submittedMeta_;
  std::vector<Event> pending_;
  formaview::render::Observation observation_{};
  std::map<int,Loop> loops_;
  std::map<std::pair<int,int>,double> requests_;
  std::deque<Recent> recent_;
  std::deque<std::pair<double,double>> recentHeartbeat_;
  QTimer heartbeat_,pump_,resourceTimer_;
  QMetaObject::Connection requestConnection_;
  std::future<QJsonObject> diagnostic_;
  QByteArray initialHash_;
  double lastClock_{},previousPresent_{},lastWindow_{},lastResourceAt_{},lastDiagnostic_{-15000};
  std::uint64_t previousCpu_{};
  int epoch_{},target_{},lastRequested_{-1},lastRequestEpoch_{-1};
  int lastPresentedEpoch_{},lastPresentedSample_{},finalized_{-1},fingerprintFailures_{},payloadFailures_{};
  bool active_{true},suppressWrap_{},paused_{},pauseDone_{},seekDone_{};
  double resumeAt_{};
  QByteArray staged_;
  static QJsonObject stats(std::vector<double> v,bool raw=false) {
    QJsonObject result{{"count",static_cast<qint64>(v.size())}};
    if(v.empty())return result;
    if(raw) result["raw_ms"]=QJsonArray::fromVariantList(QVariantList(v.begin(),v.end()));
    std::sort(v.begin(),v.end());
    for(const auto &[name,p]:std::initializer_list<std::pair<const char*,double>>{
        {"p50_ms",.5},{"p95_ms",.95},{"p99_ms",.99},{"max_ms",1.}})
      result[name]=v[static_cast<std::size_t>(std::ceil(p*v.size()))-1];
    return result;
  }
  static void line(QFile &file,const QJsonObject &o) {
    const auto bytes=QJsonDocument(o).toJson(QJsonDocument::Compact)+'\n';
    if(file.write(bytes)!=bytes.size())throw std::runtime_error("stability output write failed");
  }
  static QByteArray fingerprint(const std::shared_ptr<const Frame> &f) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(f->streamSignature);
    hash.addData(QByteArrayView(reinterpret_cast<const char *>(f->deformation.data()),
        static_cast<qsizetype>(f->deformation.size()*sizeof(formaview::render::DeformedVertex))));
    return hash.result().toHex();
  }
  static QJsonObject handles() {
    const auto begin=Clock::now(); HPSS snapshot{}; HPSSWALK walk{};
    QJsonObject result{{"kind","handles"}};
    const auto status=PssCaptureSnapshot(GetCurrentProcess(),
        static_cast<PSS_CAPTURE_FLAGS>(PSS_CAPTURE_HANDLES|PSS_CAPTURE_HANDLE_NAME_INFORMATION),0,&snapshot);
    result["capture_status"]=static_cast<qint64>(status);
    int files{},total{},typed{};
    QJsonObject types;
    using Query=NTSTATUS (NTAPI *)(HANDLE,OBJECT_INFORMATION_CLASS,PVOID,ULONG,PULONG);
    Query query{};
    const auto symbol=GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryObject");
    static_assert(sizeof(query)==sizeof(symbol));std::memcpy(&query,&symbol,sizeof(query));
    if(status==ERROR_SUCCESS && PssWalkMarkerCreate(nullptr,&walk)==ERROR_SUCCESS) {
      PSS_HANDLE_ENTRY entry{};
      DWORD next{};
      while((next=PssWalkSnapshot(snapshot,PSS_WALK_HANDLES,walk,&entry,sizeof(entry)))==ERROR_SUCCESS) {
        ++total;
        QString type;
        if(query) {
          alignas(void *) std::array<std::byte,4096> buffer{};
          if(query(entry.Handle,ObjectTypeInformation,buffer.data(),static_cast<ULONG>(buffer.size()),nullptr)>=0) {
            const auto &name=reinterpret_cast<PUBLIC_OBJECT_TYPE_INFORMATION *>(buffer.data())->TypeName;
            type=QString::fromWCharArray(name.Buffer,name.Length/sizeof(wchar_t));
          }
        }
        if(!type.isEmpty()) {
          ++typed;
          types[type]=types[type].toInt()+1;
          if(type=="File")++files;
        }
      }
      result["walk_status"]=static_cast<qint64>(next);
      PssWalkMarkerFree(walk);
    }
    if(snapshot)PssFreeSnapshot(GetCurrentProcess(),snapshot);
    result["file_handles"]=files; result["snapshot_handles"]=total; result["typed_handles"]=typed;
    result["object_type_counts"]=types;
    result["duration_ms"]=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
    return result;
  }
  void noteRequest() {
    if(!active_)return;
    const auto c=session_.cacheInfo();
    const auto clock=c.value("clock").toDouble();
    const auto t=elapsed();
    if(!suppressWrap_ && clock<lastClock_-1.) {
      loops_[epoch_].end=t; ++epoch_; loops_[epoch_].start=t;
    }
    lastClock_=clock;
    const auto sample=c.value("requested").toInt();
    if(sample==lastRequested_ && epoch_==lastRequestEpoch_)return;
    lastRequested_=sample;lastRequestEpoch_=epoch_;
    requests_[{epoch_,sample}]=t; loops_[epoch_].requests.insert(sample);
    staged_+=QJsonDocument(QJsonObject{{"event","request"},{"ms",t},{"loop",epoch_},
        {"sample",sample},{"clock",clock}}).toJson(QJsonDocument::Compact)+'\n';
  }
  void finishLoop(int index) {
    auto it=loops_.find(index); if(it==loops_.end())return;
    const auto &l=it->second;
    QJsonArray missing;
    for(int sample:l.requests)if(!l.presented.contains(sample))missing.append(sample);
    const auto duration=l.end-l.start;
    auto o=QJsonObject{{"loop",index},{"start_ms",l.start},{"end_ms",l.end},
        {"duration_ms",duration},{"intervened",l.intervened},
        {"presented_updates",static_cast<qint64>(l.order.size())},
        {"fps",l.order.size()*1000./std::max(duration,1.)},
        {"requested_unique",static_cast<qint64>(l.requests.size())},
        {"missing_requested_indices",missing},{"sample_order",l.order},
        {"intervals",stats(l.intervals,true)},{"request_to_swap",stats(l.latency,true)},
        {"gui_heartbeat",stats(l.heartbeat)}};
    line(loopsFile_,o);loopsFile_.flush();loopsSummary_.append(o);
    line(heartbeatFile_,{{"loop",index},{"intervals",stats(l.heartbeat,true)}});
    std::cout<<"LOOP "<<index<<" fps="<<o["fps"].toDouble()<<" p95="<<o["intervals"].toObject()["p95_ms"].toDouble()
        <<" missing="<<missing.size()<<" private="<<lastResource_["private_bytes"].toInteger()<<'\n'<<std::flush;
    loops_.erase(it);
    std::erase_if(requests_,[index](const auto &p){return p.first.first<=index;});
  }
  void consume() {
    std::vector<Event> events;
    {std::lock_guard lock(mutex_);events.swap(pending_);}
    for(const auto &e:events) {
      const auto &m=e.meta;
      if(m.epoch==lastPresentedEpoch_ && m.sample==lastPresentedSample_)continue;
      const auto interval=previousPresent_>0?e.time-previousPresent_:0.;
      const auto lag=m.requested>=0?e.time-m.requested:-1.;
      auto &l=loops_[m.epoch];l.presented.insert(m.sample);l.order.append(m.sample);
      if(interval>0)l.intervals.push_back(interval);
      if(lag>=0)l.latency.push_back(lag);
      recent_.push_back({e.time,interval,lag});
      staged_+=QJsonDocument(QJsonObject{{"event","present"},{"ms",e.time},{"loop",m.epoch},
          {"sample",m.sample},{"interval_ms",interval},{"request_to_swap_ms",lag},
          {"land_to_swap_ms",e.time-m.landed}}).toJson(QJsonDocument::Compact)+'\n';
      previousPresent_=e.time;lastPresentedEpoch_=m.epoch;lastPresentedSample_=m.sample;
    }
    if(!staged_.isEmpty()) {
      if(raw_.write(staged_)!=staged_.size())throw std::runtime_error("stability event write failed");
      staged_.clear();
    }
    const auto t=elapsed();
    while(!recent_.empty() && recent_.front().time<t-30000)recent_.pop_front();
    while(!recentHeartbeat_.empty() && recentHeartbeat_.front().first<t-30000)recentHeartbeat_.pop_front();
    while(finalized_<epoch_-2)finishLoop(++finalized_);
    if(t-lastWindow_>=5000) {
      lastWindow_=t; std::vector<double> intervals,latency,hb;
      for(const auto &r:recent_){if(r.interval>0)intervals.push_back(r.interval);if(r.latency>=0)latency.push_back(r.latency);}
      for(const auto &r:recentHeartbeat_)hb.push_back(r.second);
      auto o=QJsonObject{{"ms",t},{"window_ms",std::min(t,30000.)},
          {"fps",recent_.size()*1000/std::max(1.,std::min(t,30000.))},
          {"intervals",stats(intervals)},{"request_to_swap",stats(latency)},{"gui_heartbeat",stats(hb)},
          {"resource",lastResource_}};
      line(windows_,o);windows_.flush();windowSummary_.append(o);
    }
    if(diagnostic_.valid() && diagnostic_.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
      auto o=diagnostic_.get();o["ms"]=t;
      if(o["kind"]=="first_frame") {
        if(o["hash"].toString().toLatin1()!=initialHash_)++fingerprintFailures_;
        hashes_.append(o);
      } else {line(resources_,o);lastResource_["file_handles"]=o["file_handles"];lastResource_["handle_snapshot_ms"]=o["duration_ms"];}
    }
    if(!diagnostic_.valid() && t-lastDiagnostic_>=15000) {
      lastDiagnostic_=t;diagnostic_=std::async(std::launch::async,handles);
    }
    // Keep the first 200 loops uninterrupted in the 230-loop acceptance run.
    const int pauseLoop=target_>=230?200:std::max(1,target_/3);
    const int seekLoop=target_>=230?210:std::max(2,target_*2/3);
    if(!pauseDone_ && epoch_>=pauseLoop) {
      pauseDone_=paused_=true;resumeAt_=t+2000;loops_[epoch_].intervened=true;
      actions_.append(QJsonObject{{"ms",t},{"action","pause"},{"loop",epoch_}});session_.togglePlay();
    }
    if(paused_ && t>=resumeAt_) {
      paused_=false;actions_.append(QJsonObject{{"ms",t},{"action","resume"},{"loop",epoch_}});session_.togglePlay();
    }
    if(!seekDone_ && epoch_>=seekLoop && !paused_) {
      seekDone_=true;loops_[epoch_].intervened=true;
      // Spaced deterministic random-access edits retain playing=true.
      for(int i=0;i<4;++i) QTimer::singleShot(i*1500,&pump_,[this,i] {
        constexpr int targets[]{173,12,221,47};
        loops_[epoch_].intervened=true;suppressWrap_=true;
        actions_.append(QJsonObject{{"ms",elapsed()},{"action","seek"},{"sample",targets[i]},{"loop",epoch_}});
        session_.seekSample(targets[i]);lastClock_=session_.cacheInfo().value("clock").toDouble();suppressWrap_=false;
      });
    }
  }
  void resource() {
    const auto begin=Clock::now();const auto t=elapsed();
    struct Memory { PROCESS_MEMORY_COUNTERS_EX base; SIZE_T privateWorking; ULONG64 shared; } memory{};
    memory.base.cb=sizeof(memory);
    const bool extended=GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),sizeof(memory));
    if(!extended) {memory.base.cb=sizeof(memory.base);GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory.base),sizeof(memory.base));}
    FILETIME created{},exited{},kernel{},user{};GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user);
    const auto value=[](FILETIME f){return (std::uint64_t(f.dwHighDateTime)<<32)|f.dwLowDateTime;};
    const auto cpu=value(kernel)+value(user);DWORD totalHandles{};GetProcessHandleCount(GetCurrentProcess(),&totalHandles);
    auto o=QJsonObject::fromVariantMap(session_.performanceInfoForTest());
    const auto cache=session_.cacheInfo();
    o["ms"]=t;o["event"]="resource";
    o["private_bytes"]=static_cast<qint64>(memory.base.PrivateUsage);
    o["working_set_bytes"]=static_cast<qint64>(memory.base.WorkingSetSize);
    o["private_working_set_bytes"]=extended?QJsonValue(static_cast<qint64>(memory.privateWorking)):QJsonValue();
    o["handles"]=static_cast<qint64>(totalHandles);
    o["cpu_one_core_percent"]=previousCpu_?(cpu-previousCpu_)/100./std::max(1.,t-lastResourceAt_):0.;
    o["cpu_capacity_percent"]=o["cpu_one_core_percent"].toDouble()/std::max(1,QThread::idealThreadCount());
    o["document_cache_bytes"]=cache.value("cachedBytes").toLongLong();
    o["document_cache_samples"]=cache.value("cachedSamples").toInt();
    o["file_handles"]=lastResource_["file_handles"];
    {std::lock_guard lock(mutex_);o["renderer_gpu_bytes"]=static_cast<qint64>(observation_.owned_gpu_bytes);
      o["process_gpu_local_bytes"]=static_cast<qint64>(observation_.process_gpu_bytes);o["renderer_resident_frames"]=static_cast<int>(observation_.resident_frames);}
    previousCpu_=cpu;lastResourceAt_=t;
    o["sampling_ms"]=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
    lastResource_=o;line(resources_,o);resources_.flush();
  }
public:
  AbcStability(ModelSession &s,const QString &output,int loops):session_(s),target_(loops) {
    const auto base=QFileInfo(output).absolutePath()+"/"+QFileInfo(output).completeBaseName();
    const auto open=[&](QFile &f,const QString &suffix){f.setFileName(base+suffix);if(!f.open(QIODevice::WriteOnly))throw std::runtime_error("stability log open failed");};
    open(raw_,".events.jsonl");open(loopsFile_,".loops.jsonl");open(windows_,".windows.jsonl");open(resources_,".resources.jsonl");open(heartbeatFile_,".heartbeats.jsonl");
    initialHash_=fingerprint(session_.frame());loops_[0].presented.insert(0);
    lastClock_=s.cacheInfo().value("clock").toDouble();
    requestConnection_=QObject::connect(&s,&ModelSession::animChanged,&pump_,[this]{noteRequest();});
    QObject::connect(&heartbeat_,&QTimer::timeout,&pump_,[this]{
      const auto now=Clock::now();const auto gap=std::chrono::duration<double,std::milli>(now-lastHeartbeat_).count();lastHeartbeat_=now;
      loops_[epoch_].heartbeat.push_back(gap);recentHeartbeat_.push_back({elapsed(),gap});
    });heartbeat_.start(1);
    QObject::connect(&pump_,&QTimer::timeout,&pump_,[this]{consume();});pump_.start(50);
    QObject::connect(&resourceTimer_,&QTimer::timeout,&pump_,[this]{resource();});resourceTimer_.start(1000);
    noteRequest();landed(session_.frame());resource();
  }
  ~AbcStability(){QObject::disconnect(requestConnection_);}
  double elapsed()const{return std::chrono::duration<double,std::milli>(Clock::now()-origin_).count();}
  bool done()const{return epoch_>=target_;}
  void landed(const std::shared_ptr<const Frame> &f) {
    noteRequest();const auto sample=session_.cacheInfo().value("sample").toInt();
    const auto document=session_.documentSnapshot();
    if(!f->streamGeometry || f->seconds!=document->geometryCache->times().at(static_cast<std::size_t>(sample)) ||
        f->deformation.size()!=document->triangles*3 || f->sourcePoints!=document->points)++payloadFailures_;
    const auto requested=session_.cacheInfo().value("requested").toInt();
    const auto epoch=epoch_>0 && requested<10 && sample>230?epoch_-1:epoch_;
    const auto it=requests_.find({epoch,sample});
    {std::lock_guard lock(mutex_);
      if(metadata_.size()>=128)std::erase_if(metadata_,[](const auto &p){return p.second.frame.expired();});
      metadata_[f.get()]={epoch,sample,elapsed(),it==requests_.end()?-1.:it->second,f};}
    if(sample==0 && !diagnostic_.valid()) {
      diagnostic_=std::async(std::launch::async,[f,epoch]{return QJsonObject{{"kind","first_frame"},{"loop",epoch},{"hash",QString::fromLatin1(fingerprint(f))}};});
    }
  }
  void submitted(std::shared_ptr<const Frame> f) {
    std::lock_guard lock(mutex_);submittedFrame_=std::move(f);
    const auto it=metadata_.find(submittedFrame_.get());
    submittedMeta_=it==metadata_.end()?std::nullopt:std::optional(it->second);
  }
  void swapped(){std::lock_guard lock(mutex_);if(active_ && submittedMeta_)pending_.push_back({*submittedMeta_,elapsed()});}
  void observe(formaview::render::Observation o){std::lock_guard lock(mutex_);observation_=o;}
  QJsonObject finish() {
    if(session_.playing())session_.togglePlay();consume();
    heartbeat_.stop();pump_.stop();resourceTimer_.stop();QObject::disconnect(requestConnection_);
    {std::lock_guard lock(mutex_);active_=false;}
    while(finalized_<target_-1)finishLoop(++finalized_);
    resource();raw_.flush();heartbeatFile_.flush();
    return {{"elapsed_ms",elapsed()},{"completed_loops",epoch_},{"target_loops",target_},
        {"loops",loopsSummary_},{"sliding_30s_windows",windowSummary_},{"actions",actions_},
        {"first_frame_hash",QString::fromLatin1(initialHash_)},{"first_frame_checks",hashes_},
        {"first_frame_mismatches",fingerprintFailures_},{"final_resource",lastResource_},
        {"payload_sample_mismatches",payloadFailures_},
        {"measurement","Continuous 30 samples/s request speed, production QML, all geometry followed. One-second process/cache samples; 30-second sliding windows every five seconds; asynchronous first-frame hashes and 15-second handle snapshots. Initial 200 loops clean; pause/resume and deterministic seeks thereafter in 230-loop acceptance."}};
  }
};
