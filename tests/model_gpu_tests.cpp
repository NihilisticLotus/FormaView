#include "model_document.hpp"
#include "model_view.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <imm.h>
#pragma comment(lib,"imm32.lib")
#include <psapi.h>
#include <wct.h>
#pragma comment(lib,"advapi32.lib")
#include <malloc.h>
#include <QScopeGuard>
#include <dbghelp.h>
#pragma comment(lib,"dbghelp.lib")
#include <map>
#include <thread>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QScreen>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QTimer>
#include <QAbstractEventDispatcher>
#include <QEventLoop>
#include <exception>
#include <QUuid>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <iostream>
#include <cmath>
#include <limits>
#include "model_forensics.hpp"
#include "foreground_trace.hpp"
#include <QtGui/private/qrhid3d11_p.h>
using namespace formaview::render;
namespace {
constexpr double frameTimeP95TargetMs=16.7;
// User-approved P95 budget tolerance; the percentile and samples are unchanged.
constexpr double frameTimeP95Tolerance=0.05;
constexpr double frameTimeP95LimitMs=frameTimeP95TargetMs*(1.0+frameTimeP95Tolerance);
constexpr double frameTimeP99LimitMs=33.3;
void recordFrameTimeGate(QJsonObject &report,double p95,double p99,bool eligible) {
  report["frameTimeP95TargetMs"]=frameTimeP95TargetMs;
  report["frameTimeP95TolerancePercent"]=frameTimeP95Tolerance*100.0;
  report["frameTimeP95LimitMs"]=frameTimeP95LimitMs;
  report["frameTimeP99LimitMs"]=frameTimeP99LimitMs;
  report["frameTimeNominalGatePassed"]=eligible && p95<=frameTimeP95TargetMs && p99<=frameTimeP99LimitMs;
  report["frameTimeGatePassed"]=eligible && p95<=frameTimeP95LimitMs && p99<=frameTimeP99LimitMs;
}
bool sogouPyObserved{}, sogouTsfObserved{};
void observeImeModules() {
  sogouPyObserved |= GetModuleHandleW(L"SogouPY.ime") != nullptr;
  sogouTsfObserved |= GetModuleHandleW(L"SogouTSF.ime") != nullptr;
}
void require(bool value,const char *message) { if(!value)throw std::runtime_error(message); }
void settle(const std::function<bool()> &ready,int timeout=60000) {
  QElapsedTimer t;t.start();while(!ready() && t.elapsed()<timeout){QCoreApplication::processEvents();QThread::msleep(1);} require(ready(),"Timeout");
}
ModelRenderState stateFor(const ModelDocument &d) {
  ModelRenderState s;
  for (const auto &m:d.meshes) {s.bindings.push_back(m.skins.empty()?-1:0);s.uvs.push_back(0);s.classes.push_back(m.classification);s.visible.push_back(true);}
  s.textures.resize(d.meshes.size());return s;
}
std::vector<float> read(const std::shared_ptr<const Frame> &frame,int mode) {
  QQuickWindow window;window.setTitle("FormaView GPU numerical readback");window.resize(512,128);
  auto item=std::make_unique<Item>(window.contentItem());item->setSize({512,128});
  const auto count=frame->geometry->vertices.size(); item->setFixedColorBufferWidth(static_cast<int>(count));item->setFixedColorBufferHeight(1);item->skinProbe=mode;
  struct State {std::atomic<bool> done{}; QByteArray data;QSize size;QString error;}; auto s=std::make_shared<State>();
  item->readback=[s](QByteArray bytes,QSize size){s->data=std::move(bytes);s->size=size;s->done=true;};
  item->skinFailure=[s](QString message,std::shared_ptr<const Frame>){s->error=message;s->done=true;};
  item->setFrame(frame);window.show();settle([&]{return s->done.load();},10000);
  observeImeModules();
  require(s->error.isEmpty(),qPrintable(s->error));require(s->data.size()==static_cast<qsizetype>(count*16),"Readback shape");
  std::vector<float> values(count*4);std::memcpy(values.data(),s->data.constData(),values.size()*4);
  item.reset();window.hide();window.releaseResources();QCoreApplication::processEvents();return values;
}
// Static GPU corner -> CPU pose vertex (concatenated triangle slots), independent
// of how the static frame groups draws by material.
std::vector<std::size_t> poseSlots(const ModelDocument &d,const ModelRenderState &state,std::size_t corners) {
  // PSA-first paired preview: GPU and CPU frames both list face corners in order.
  if(d.pairPreviewDomain()){std::vector<std::size_t> identity(corners);for(std::size_t i=0;i<corners;++i)identity[i]=i;return identity;}
  const auto order=buildModelFrames(d,state,1,{},nullptr,true).model->sourceCorners;
  std::vector<std::vector<std::size_t>> slot(d.meshes.size());std::size_t offset=0;
  for(std::size_t mi=0;mi<d.meshes.size();++mi){const auto &m=d.meshes[mi];slot[mi].assign(m.corners.size(),std::numeric_limits<std::size_t>::max());
    for(std::size_t t=0;t<m.triangles.size();++t)if(slot[mi][m.triangles[t]]==std::numeric_limits<std::size_t>::max())slot[mi][m.triangles[t]]=offset+t;offset+=m.triangles.size();}
  std::vector<std::size_t> out;out.reserve(order.size());for(const auto &entry:order)out.push_back(slot.at(entry[0]).at(entry[1]));return out;
}
QJsonObject numerical(const ModelDocument &d,const ModelGpuGeometry &g,const std::vector<std::size_t> &poseSlot,int clip,double seconds,std::uint64_t revision,const std::vector<formaview::Matrix4> *pinned=nullptr) {
  const auto worlds=pinned?*pinned:d.worldAt(static_cast<std::size_t>(clip),seconds);
  auto pose=buildModelGpuPose(d,g,{},worlds,revision,clip,seconds);
  const auto cpu=buildAnimationFrame(d,static_cast<std::size_t>(clip),seconds,revision,d.actorSkinDomain()||d.pairPreviewDomain(),&worlds);
  const auto base=g.frames.model; const auto count=std::min<std::size_t>(128,base->vertices.size());
  auto sampled=std::make_shared<Frame>(); auto skin=std::make_shared<SkinData>(*base->skin);skin->ranges.clear();
  std::vector<std::size_t> indices;
  skin->morphRanges.clear();
  for(std::size_t i=0;i<count;++i){const auto at=i*(base->vertices.size()-1)/std::max<std::size_t>(count-1,1);indices.push_back(at);sampled->vertices.push_back(base->vertices[at]);skin->ranges.push_back(base->skin->ranges[at]);
    skin->morphRanges.push_back(base->skin->morphRanges.empty()?std::array<std::uint32_t,2>{0,0}:base->skin->morphRanges.at(at));}
  sampled->skin=skin;sampled->rgba={1,1,1,1};sampled->revision=revision;
  auto packet=std::make_shared<Frame>(*pose.model);packet->geometry=sampled;packet->draws.clear();packet->explicitDraws=false;
  const auto positions=read(packet,1),normals=read(packet,2);
  double positionError=0,normalAngle=0;int comparedNormals=0;
  for(std::size_t i=0;i<count;++i){const auto &v=cpu->vertices.at(poseSlot.at(indices[i]));const auto at=i*4;
    for(int component=0;component<4;++component)require(std::isfinite(positions[at+component]) && std::isfinite(normals[at+component]),"Non-finite GPU readback");
    require(std::abs(positions[at+3]-1)<1e-6,"GPU point did not rasterize");
    positionError=std::max({positionError,std::abs(double(positions[at])-v.x),std::abs(double(positions[at+1])-v.y),std::abs(double(positions[at+2])-v.z)});
    const QVector3D a(normals[at],normals[at+1],normals[at+2]),b(v.nx,v.ny,v.nz);
    if(a.lengthSquared()>1e-12F && b.lengthSquared()>1e-12F) {
      const double dot=std::clamp(double(QVector3D::dotProduct(a.normalized(),b.normalized())),-1.0,1.0);
      normalAngle=std::max(normalAngle,std::acos(dot)*180/3.141592653589793);++comparedNormals;
    }
  }
  std::cout<<"GPU readback clip "<<clip<<" time "<<seconds<<" position "<<positionError<<" normal degrees "<<normalAngle<<std::endl;
    require(positionError<=1e-4,"GPU position tolerance exceeded");
  return {{"clip",clip},{"seconds",seconds},{"points",static_cast<int>(count)},{"normals",comparedNormals},{"maxPositionError",positionError},{"maxNormalDegrees",normalAngle}};
}
QJsonObject synthetic() {
  auto base=std::make_shared<Frame>();auto skin=std::make_shared<SkinData>();
  base->rgba={1,1,1,1};base->revision=1;
  base->vertices.push_back({.3F,-.2F,.4F,0,0,0,.2F,.7F,.3F});
  skin->ranges.push_back({0,12});skin->display.rotate(37,1,2,3);skin->display.scale(.5F);
  QMatrix4x4 blended;blended.fill(0);auto packet=std::make_shared<Frame>();packet->revision=2;
  for(int i=0;i<12;++i) {
    QMatrix4x4 m;m.translate(.01F*i,-.02F*i,.005F*i);m.rotate(static_cast<float>(i*7),1,2,3);m.scale(i==0?-.5F:1+.03F*i,i==1?0.F:.8F+.02F*i,1.3F);
    const float weight=static_cast<float>(i+1)/78.F;skin->influences.push_back(static_cast<float>(i));skin->influences.push_back(weight);blended+=m*weight;
    for(int r=0;r<4;++r)for(int c=0;c<4;++c)packet->palette.push_back(m(r,c));
  }
  base->skin=skin;packet->geometry=base;
  const auto &v=base->vertices[0];const auto expected=skin->display.map(blended.map(QVector3D(v.x,v.y,v.z)));
  const auto expectedNormal=skin->display.mapVector(blended.inverted().transposed().mapVector(QVector3D(v.nx,v.ny,v.nz))).normalized();
  const auto p=read(packet,1),n=read(packet,2);
  const double error=(QVector3D(p[0],p[1],p[2])-expected).length();const double angle=std::acos(std::clamp(double(QVector3D::dotProduct(QVector3D(n[0],n[1],n[2]).normalized(),expectedNormal)),-1.0,1.0))*180/3.141592653589793;
  require(std::isfinite(error) && std::isfinite(angle) && error<=1e-4 && angle<=.5,"12-influence signed/scaled GPU oracle mismatch");
  return {{"influences",12},{"positionError",error},{"normalDegrees",angle}};
}
// Opens a sample and, for external-drive targets, attaches each animation in order.
// FORMAVIEW_GPU_REFERENCE pairs a PSA with its PSK; FORMAVIEW_GPU_PAIR_MESH=1 shows that mesh.
void openSample(ModelSession &session,const QString &path,const QStringList &externals) {
  const auto reference=qEnvironmentVariable("FORMAVIEW_GPU_REFERENCE");if(!reference.isEmpty())session.setAnimationReference(reference);
  session.open(path);settle([&]{return !session.busy() && session.inspectable() && session.frame();},90000);
  if(qEnvironmentVariable("FORMAVIEW_GPU_PAIR_MESH")=="1"){session.setPairMeshVisible(true);require(session.pairMeshVisible(),"Paired mesh unavailable");}
  for(const auto &external:externals) {
    session.attachExternalAnimation(external);settle([&]{return !session.externalBusy();},120000);
    const auto info=session.externalInfo();require(info.value("driven").toBool() && info.value("status").toString()!="refused","External drive refused");
  }
}
// GUI-thread stall watchdog (diagnostic): stacks are captured with the thread suspended, resolved later.
struct WaitNode {DWORD type{},status{},pid{},tid{},waitTime{},contextSwitches{};QString objectName;};
struct StallSample {double ms{};double stalledMs{};int count{};void *ip[40]{};
 DWORD waitError{},waitCount{},suspendError{};BOOL waitCycle{};WaitNode waitNodes[WCT_MAX_NODE_COUNT]{};
 bool guiSuspended{},renderSuspended{};DWORD contextError{},resumeError{},renderResumeError{};
 double guiAge{},renderAge{};bool renderTriggered{};double captureMs{},resumedMs{},waitStartMs{},waitEndMs{},renderCaptureMs{},renderResumedMs{};int renderCount{};void *renderIp[40]{};DWORD renderId{},renderError{};long long heartbeat{};std::uint64_t waitCandidate{},renderWaitCandidate{},swapchainHandle{};QJsonObject waitObject,renderWaitObject;std::shared_ptr<model_forensics::Snapshot> dispatch;};
struct Watch {std::atomic<long long> beat{0};std::atomic<bool> run{true};std::vector<StallSample> samples;model_forensics::Probe *probe{};std::atomic<DWORD> *renderThread{};std::atomic<std::uint64_t> *swapchainHandle{};std::atomic<long long> *renderBeat{};std::uint64_t checks{},suppressed{};double cpuMs{};};
long long beatNow() {return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
int unwindStack(CONTEXT context,void **out,int capacity) {
  int count=0;
  __try {
    while(count<capacity && context.Rip) {
      const DWORD64 ip=context.Rip;out[count++]=reinterpret_cast<void*>(ip);
      DWORD64 base=0;auto *entry=RtlLookupFunctionEntry(ip,&base,nullptr);
      if(!entry){context.Rip=*reinterpret_cast<DWORD64*>(context.Rsp);context.Rsp+=8;}
      else{PVOID handler=nullptr;DWORD64 frame=0;RtlVirtualUnwind(UNW_FLAG_NHANDLER,base,ip,entry,&context,&handler,&frame,nullptr);}
    }
  } __except(EXCEPTION_EXECUTE_HANDLER) {}
  return count;
}
void watchdogLoop(HANDLE gui,Watch *watch,std::chrono::steady_clock::time_point origin) {
  int taken=0;
  const auto cpuTime=[] {FILETIME created{},exited{},kernel{},user{};
    if(!GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel,&user))return 0.0;
    const auto ticks=[](FILETIME t){return (static_cast<std::uint64_t>(t.dwHighDateTime)<<32)|t.dwLowDateTime;};
    return static_cast<double>(ticks(kernel)+ticks(user))/10000.0;};
  const double cpuStart=cpuTime();
  const auto cpuFinish=qScopeGuard([&]{watch->cpuMs=cpuTime()-cpuStart;});
  const auto waitSession=OpenThreadWaitChainSession(0,nullptr);
  const auto waitSessionError=waitSession?0:GetLastError();
  const auto closeWait=qScopeGuard([&]{if(waitSession)CloseThreadWaitChainSession(waitSession);});
  const auto guiId=GetThreadId(gui);
  const auto ntWait=reinterpret_cast<const unsigned char*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtWaitForSingleObject"));
  const bool knownWaitStub=ntWait&&ntWait[0]==0x4c&&ntWait[1]==0x8b&&ntWait[2]==0xd1&&ntWait[3]==0xb8&&ntWait[18]==0x0f&&ntWait[19]==0x05;
  while(watch->run.load()) {
    Sleep(10);
    ++watch->checks;
    const auto guiBeat=watch->beat.load();if(!guiBeat)continue;
    const auto renderBeat=watch->renderBeat?watch->renderBeat->load():0;
    const double guiAge=(beatNow()-guiBeat)/1e6,renderAge=renderBeat?(beatNow()-renderBeat)/1e6:0;
    const bool renderTriggered=guiAge<60&&renderAge>=60;
    const auto beat=renderTriggered?renderBeat:guiBeat;const double age=renderTriggered?renderAge:guiAge;
    if(age<60){taken=0;continue;}
    if(taken>=80 || watch->samples.size()==watch->samples.capacity()){++watch->suppressed;continue;}
    StallSample sample;sample.guiAge=guiAge;sample.renderAge=renderAge;sample.renderTriggered=renderTriggered;sample.heartbeat=beat;sample.ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-origin).count();sample.stalledMs=age;
    model_forensics::Snapshot dispatch;
    const auto since=[&]{return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-origin).count();};
    sample.captureMs=since();
    if(SuspendThread(gui)!=static_cast<DWORD>(-1)) {
      sample.guiSuspended=true;
      CONTEXT context{};context.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER;
      if(GetThreadContext(gui,&context)) {
        sample.count=unwindStack(context,sample.ip,40);
        if(watch->probe&&knownWaitStub&&context.Rip==reinterpret_cast<std::uintptr_t>(ntWait)+20)sample.waitCandidate=context.R10;
      }else sample.contextError=GetLastError();
      if(watch->probe)dispatch=watch->probe->snapshot();
      if(ResumeThread(gui)==static_cast<DWORD>(-1))sample.resumeError=GetLastError();
    }else sample.suspendError=GetLastError();
    sample.resumedMs=since();
    if(watch->probe)sample.dispatch=std::make_shared<model_forensics::Snapshot>(dispatch);
    if(sample.waitCandidate)sample.waitObject=model_forensics::waitObject(reinterpret_cast<HANDLE>(sample.waitCandidate));
    // Query only AFTER resuming: a suspended GUI thread can hold locks needed
    // by WCT itself. Flags=0 follows this process and reports the first foreign
    // thread without probing that process. Failure is evidence, never bypassed.
    // Capture render thread separately, after GUI resume. Never suspend both.
    sample.renderCaptureMs=since();
    if(watch->probe&&watch->renderThread&&(sample.renderId=watch->renderThread->load())!=0) {
      HANDLE render=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,sample.renderId);
      if(render) {
        if(SuspendThread(render)!=static_cast<DWORD>(-1)) {
          sample.renderSuspended=true;
          CONTEXT context{};context.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER;
          if(GetThreadContext(render,&context)){
            sample.renderCount=unwindStack(context,sample.renderIp,40);
            if(knownWaitStub&&context.Rip==reinterpret_cast<std::uintptr_t>(ntWait)+20)sample.renderWaitCandidate=context.R10;
            if(watch->swapchainHandle)sample.swapchainHandle=watch->swapchainHandle->load();
          }
          else sample.renderError=GetLastError();
          if(ResumeThread(render)==static_cast<DWORD>(-1))sample.renderResumeError=GetLastError();
        } else sample.renderError=GetLastError();
        CloseHandle(render);
      } else sample.renderError=GetLastError();
    }
    sample.renderResumedMs=since();
    if(sample.renderWaitCandidate)sample.renderWaitObject=model_forensics::waitObject(reinterpret_cast<HANDLE>(sample.renderWaitCandidate));
    sample.waitError=waitSessionError;
    sample.waitStartMs=since();
    if(waitSession){
      WAITCHAIN_NODE_INFO nodes[WCT_MAX_NODE_COUNT]{};DWORD count=WCT_MAX_NODE_COUNT;
      if(GetThreadWaitChain(waitSession,0,0,guiId,&count,nodes,&sample.waitCycle)){
        sample.waitCount=std::min<DWORD>(count,WCT_MAX_NODE_COUNT);
        for(DWORD i=0;i<sample.waitCount;++i){auto &out=sample.waitNodes[i];const auto &in=nodes[i];
          out.type=in.ObjectType;out.status=in.ObjectStatus;
          if(in.ObjectType==WctThreadType){out.pid=in.ThreadObject.ProcessId;out.tid=in.ThreadObject.ThreadId;out.waitTime=in.ThreadObject.WaitTime;out.contextSwitches=in.ThreadObject.ContextSwitches;}
          else if(watch->probe)out.objectName=QString::fromWCharArray(in.LockObject.ObjectName,static_cast<int>(wcsnlen_s(in.LockObject.ObjectName,WCT_OBJNAME_LENGTH)));}
      }else sample.waitError=GetLastError();
    }
    sample.waitEndMs=since();
    watch->samples.push_back(std::move(sample));++taken;
  }
}
QString describeAddress(void *address) {
  HMODULE module=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(address),&module);
  wchar_t path[MAX_PATH]{};if(module)GetModuleFileNameW(module,path,MAX_PATH);
  QString text=module?QFileInfo(QString::fromWCharArray(path)).fileName():QStringLiteral("?");
  alignas(SYMBOL_INFOW) char buffer[sizeof(SYMBOL_INFOW)+512*sizeof(wchar_t)]{};auto *symbol=reinterpret_cast<SYMBOL_INFOW*>(buffer);symbol->SizeOfStruct=sizeof(SYMBOL_INFOW);symbol->MaxNameLen=512;DWORD64 displacement=0;
  text+="+0x"+QString::number(reinterpret_cast<std::uintptr_t>(address)-reinterpret_cast<std::uintptr_t>(module),16);
  if(SymFromAddrW(GetCurrentProcess(),reinterpret_cast<DWORD64>(address),&displacement,symbol)){
    IMAGEHLP_MODULEW64 info{};info.SizeOfStruct=sizeof(info);
    const bool pdb=SymGetModuleInfoW64(GetCurrentProcess(),reinterpret_cast<DWORD64>(address),&info)&&info.SymType==SymPdb&&!info.PdbUnmatched&&!info.DbgUnmatched;
    text+=(pdb?" [pdb!":" [unverified-symbol!")+QString::fromWCharArray(symbol->Name)+"+0x"+QString::number(displacement,16)+"]";
  }
  return text;
}
// Watchdog validation only (FORMAVIEW_LIFECYCLE_INJECT_STALL=1): a known, named 300 ms GUI-thread block.
__declspec(noinline) void injectedStallForWatchdogValidation() {
  QThread::msleep(300);
  // Retain this named frame in optimized builds; noinline alone permits a tail call.
  static std::atomic<unsigned> completions{0};completions.fetch_add(1);
}
bool systemModule(const QString &frame) {
  const auto name=frame.section('!',0,0).section('+',0,0).toLower();
  for(const char *skip:{"ntdll.dll","kernelbase.dll","kernel32.dll","win32u.dll","user32.dll","ucrtbase.dll","msvcp140.dll","vcruntime140.dll","gdi32.dll","gdi32full.dll","combase.dll","rpcrt4.dll","sechost.dll"})if(name==skip)return true;
  return false;
}
QJsonObject summarizeStalls(const std::vector<StallSample> &samples) {
  SymSetOptions(SYMOPT_UNDNAME|SYMOPT_DEFERRED_LOADS|SYMOPT_LOAD_LINES);
  QString symbolPath=QCoreApplication::applicationDirPath()+";E:/Qt/6.12.0/msvc2022_64/bin";
  const auto extraSymbols=qEnvironmentVariable("FORMAVIEW_FORENSICS_SYMBOLS");if(!extraSymbols.isEmpty())symbolPath+=";"+extraSymbols;
  const auto wideSymbolPath=symbolPath.toStdWString();SymInitializeW(GetCurrentProcess(),wideSymbolPath.c_str(),TRUE);
  std::map<void*,QString> cache;
  const auto name=[&](void *ip){auto it=cache.find(ip);if(it==cache.end())it=cache.emplace(ip,describeAddress(ip)).first;return it->second;};
  QJsonArray stalls,diagnosticSamples,captureTiming,modules;std::map<QString,int> owners,leaves;
  std::map<DWORD64,bool> recordedModules;
  const auto module=[&](void *ip){
    IMAGEHLP_MODULEW64 info{};info.SizeOfStruct=sizeof(info);
    if(!SymGetModuleInfoW64(GetCurrentProcess(),reinterpret_cast<DWORD64>(ip),&info) || recordedModules.contains(info.BaseOfImage))return;
    recordedModules[info.BaseOfImage]=true;const auto &g=info.PdbSig70;
    modules.append(QJsonObject{{"image",QString::fromWCharArray(info.ImageName)},
      {"loadedPdb",QString::fromWCharArray(info.LoadedPdbName)},
      {"base",QString::number(info.BaseOfImage,16)},{"imageBytes",static_cast<qint64>(info.ImageSize)},
      {"timestamp",static_cast<qint64>(info.TimeDateStamp)},{"symType",static_cast<int>(info.SymType)},
      {"pdbMatched",info.SymType==SymPdb&&!info.PdbUnmatched&&!info.DbgUnmatched},
      {"pdbGuid",QUuid(g.Data1,g.Data2,g.Data3,g.Data4[0],g.Data4[1],g.Data4[2],g.Data4[3],g.Data4[4],g.Data4[5],g.Data4[6],g.Data4[7]).toString(QUuid::WithoutBraces)},
      {"pdbAge",static_cast<qint64>(info.PdbAge)}});
  };
  std::size_t i=0;
  while(i<samples.size()) {
    std::size_t j=i;while(j+1<samples.size() && samples[j+1].heartbeat==samples[i].heartbeat)++j;
    QJsonObject stall{{"startMs",samples[i].ms-samples[i].stalledMs},{"observedSamples",static_cast<int>(j-i+1)},{"maxStalledMs",samples[j].stalledMs}};
    std::map<QString,int> groupOwners;QJsonArray stacks;
    for(std::size_t k=i;k<=j;++k) {
      const auto &v=samples[k];
      captureTiming.append(QJsonObject{{"ms",v.ms},{"guiAgeMs",v.guiAge},{"renderAgeMs",v.renderAge},
        {"trigger",v.renderTriggered?"render":"gui"},{"guiSuspended",v.guiSuspended},{"renderSuspended",v.renderSuspended},
        {"captureMs",v.captureMs},{"resumedMs",v.resumedMs},{"renderCaptureMs",v.renderCaptureMs},{"renderResumedMs",v.renderResumedMs},
        {"waitStartMs",v.waitStartMs},{"waitEndMs",v.waitEndMs},
        {"guiPauseUpperBoundMs",v.guiSuspended?v.resumedMs-v.captureMs:0},
        {"renderPauseUpperBoundMs",v.renderSuspended?v.renderResumedMs-v.renderCaptureMs:0},
        {"queryWallMs",v.waitEndMs-v.waitStartMs},{"iterationWallMs",v.waitEndMs-v.captureMs},
        {"suspendError",static_cast<qint64>(v.suspendError)},{"contextError",static_cast<qint64>(v.contextError)},
        {"resumeError",static_cast<qint64>(v.resumeError)},{"renderError",static_cast<qint64>(v.renderError)},
        {"renderResumeError",static_cast<qint64>(v.renderResumeError)},{"waitError",static_cast<qint64>(v.waitError)}});
      for(int f=0;f<v.count;++f){name(v.ip[f]);module(v.ip[f]);}
      for(int f=0;f<v.renderCount;++f){name(v.renderIp[f]);module(v.renderIp[f]);}
      QString owner=QStringLiteral("(none)");
      for(int f=0;f<samples[k].count;++f){const auto frame=name(samples[k].ip[f]);if(!systemModule(frame)){owner=frame;break;}}
      if(samples[k].count)leaves[name(samples[k].ip[0])]++;
      owners[owner]++;groupOwners[owner]++;
      if(samples[k].dispatch){QJsonArray allFrames,allWait,renderFrames;
        for(int f=0;f<v.renderCount;++f)renderFrames.append(name(v.renderIp[f]));
        for(int f=0;f<v.count;++f)allFrames.append(name(v.ip[f]));
        for(DWORD f=0;f<v.waitCount;++f){const auto &node=v.waitNodes[f];allWait.append(QJsonObject{
          {"type",static_cast<int>(node.type)},{"status",static_cast<int>(node.status)},
          {"pid",static_cast<qint64>(node.pid)},{"tid",static_cast<qint64>(node.tid)},
          {"objectName",node.objectName},{"threadWaitTime",static_cast<qint64>(node.waitTime)},
          {"contextSwitches",static_cast<qint64>(node.contextSwitches)}});}
        diagnosticSamples.append(QJsonObject{{"ms",v.ms},{"stalledMs",v.stalledMs},{"guiAgeMs",v.guiAge},{"renderAgeMs",v.renderAge},{"trigger",v.renderTriggered?"render":"gui"},
          {"heartbeatNs",QString::number(v.heartbeat)},{"renderWaitObject",v.renderWaitObject},{"swapchainWaitHandle",QString::number(v.swapchainHandle,16)},{"waitObject",v.waitObject},{"captureMs",v.captureMs},{"resumedMs",v.resumedMs},{"waitStartMs",v.waitStartMs},{"waitEndMs",v.waitEndMs},
          {"suspendError",static_cast<qint64>(v.suspendError)},{"waitError",static_cast<qint64>(v.waitError)},
          {"waitCycle",v.waitCycle!=FALSE},{"dispatch",model_forensics::snapshotJson(*v.dispatch)},
          {"waitChain",allWait},{"frames",allFrames},{"renderTid",static_cast<qint64>(v.renderId)},{"renderError",static_cast<qint64>(v.renderError)},
          {"renderCaptureMs",v.renderCaptureMs},{"renderResumedMs",v.renderResumedMs},{"renderFrames",renderFrames}});
      }
      if(k==i || k==(i+j)/2 || k==j){QJsonArray frames,chain;
        for(int f=0;f<std::min(samples[k].count,16);++f)frames.append(name(samples[k].ip[f]));
        for(DWORD f=0;f<samples[k].waitCount;++f){const auto &node=samples[k].waitNodes[f];chain.append(QJsonObject{
          {"type",static_cast<int>(node.type)},{"status",static_cast<int>(node.status)},{"pid",static_cast<qint64>(node.pid)},{"tid",static_cast<qint64>(node.tid)}});}
        stacks.append(QJsonObject{{"stalledMs",samples[k].stalledMs},{"frames",frames},{"waitChain",chain},
          {"waitError",static_cast<qint64>(samples[k].waitError)},{"waitCycle",samples[k].waitCycle!=FALSE}});
      }
    }
    QJsonObject groupOwnerJson;for(const auto &[key,count]:groupOwners)groupOwnerJson[key]=count;
    stall["firstNonSystemFrame"]=groupOwnerJson;stall["stacks"]=stacks;stalls.append(stall);i=j+1;
  }
  QJsonObject ownerJson,leafJson;for(const auto &[key,count]:owners)ownerJson[key]=count;for(const auto &[key,count]:leaves)leafJson[key]=count;
  SymCleanup(GetCurrentProcess());
  return {{"stalls",stalls},{"samples",static_cast<int>(samples.size())},{"firstNonSystemFrameHistogram",ownerJson},{"leafHistogram",leafJson},{"diagnosticSamples",diagnosticSamples},
    {"captureTiming",captureTiming},{"symbolModules",modules},
    {"captureBoundary","Pause intervals bracket SuspendThread/ResumeThread, including API and scheduling time; upper bounds, not exact induced frame cost. GUI and render are never suspended together. WCT runs after resume. Do not subtract these spans from frame times as a causal correction."}};
}
// Process-level resource row for lifecycle trends (private bytes, handles, GPU bytes).
QJsonObject usageRow(const QString &label,qint64 ms,std::uint64_t owned,std::uint64_t processGpu,std::uint64_t releases) {
  PROCESS_MEMORY_COUNTERS_EX counters{};counters.cb=sizeof(counters);
  GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),sizeof(counters));
  DWORD handles=0;GetProcessHandleCount(GetCurrentProcess(),&handles);
  return {{"label",label},{"ms",ms},{"privateBytes",static_cast<qint64>(counters.PrivateUsage)},{"workingSetBytes",static_cast<qint64>(counters.WorkingSetSize)},{"handles",static_cast<int>(handles)},{"ownedGpuBytes",static_cast<qint64>(owned)},{"processGpuBytes",static_cast<qint64>(processGpu)},{"releases",static_cast<qint64>(releases)}};
}
double percentile(std::vector<double> v,double p) {if(v.empty())return -1;std::sort(v.begin(),v.end());return v[static_cast<std::size_t>(std::ceil(p*static_cast<double>(v.size())))-1];}
}
int main(int argc,char **argv) {
  // Opt-in diagnostic isolation only: no keyboard input is tested in numerical
  // readback. This affects this GUI thread, never the user's system IME settings.
  const bool isolateIme=qEnvironmentVariable("FORMAVIEW_TEST_NO_IME")=="1";
  const bool imeDisabled=isolateIme && ImmDisableIME(GetCurrentThreadId());
  QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
  model_forensics::Application app(argc,argv);const auto args=app.arguments();
  if(args.size()<3){std::cerr<<"model_gpu_tests SAMPLE OUTPUT.json [seconds] [cpu|numeric]";return 2;}
  observeImeModules();
  QJsonObject report{{"percentileConvention","nearest-rank ceil(p*n)-1; no interpolation"},
    {"imeIsolationRequested",isolateIme},{"imeDisabledForGuiThread",imeDisabled}};try {
    require(!isolateIme || imeDisabled,"IME diagnostic isolation failed");
    const auto path=args[1];const double duration=args.size()>3?args[3].toDouble():10;
    const auto mode=args.size()>4?args[4]:QString("gpu");report["mode"]=mode;report["sample"]=path;
    const auto externals=qEnvironmentVariable("FORMAVIEW_GPU_EXTERNALS").split(';',Qt::SkipEmptyParts);report["externals"]=QJsonArray::fromStringList(externals);
    if(mode=="cpu")qputenv("FORMAVIEW_SKINNING","cpu");else qunsetenv("FORMAVIEW_SKINNING");
    if(mode=="fallback") {
      ModelSession session;QQuickWindow window;window.resize(800,600);
      auto item=std::make_unique<ModelView>(window.contentItem());item->setSize({800,600});item->setSession(&session);
      auto failures=std::make_shared<std::atomic<int>>(0);const auto original=item->skinFailure;
      item->skinFailure=[failures,original](QString reason,std::shared_ptr<const Frame> frame){++*failures;original(reason,std::move(frame));};
      window.show();openSample(session,path,externals);settle([&]{return session.inspectable() && session.frame() && session.frame()->geometry;},90000);
      session.togglePlay();QElapsedTimer warm;warm.start();settle([&]{return warm.elapsed()>200;},2000);
      qputenv("FORMAVIEW_GPU_FORCE_FALLBACK","1");item->update();settle([&]{return failures->load()>0 && session.inspectable() && session.frame() && !session.frame()->geometry;},90000);
      const auto revision=session.frame()->revision;settle([&]{return session.frame()->revision>revision;},30000);
      require(session.playing(),"GPU capability fallback stopped playback");report["fallbackCallbacks"]=failures->load();report["cpuPlaybackAdvanced"]=true;
      session.togglePlay();item.reset();session.clear();window.hide();window.releaseResources();QCoreApplication::processEvents();qunsetenv("FORMAVIEW_GPU_FORCE_FALLBACK");
    }
    else if(mode=="synthetic") {report["synthetic"]=synthetic();}
    else if(mode=="staff") {
      QFile oracleFile(QFileInfo(args[2]).absolutePath()+"/staff-source-oracle.json");
      require(oracleFile.open(QIODevice::ReadOnly),"Independent source oracle missing");
      const auto oracle=QJsonDocument::fromJson(oracleFile.readAll()).array();require(oracle.size()==16,"Independent oracle coverage");
      auto d=readModelDocument(path,{});auto g=buildModelGpuGeometry(*d,stateFor(*d),100,{});QJsonArray results;
      for(const auto &entry:oracle) {
        const auto row=entry.toObject();const int clip=row["clip"].toInt();const double seconds=row["seconds"].toDouble();const auto worlds=d->worldAt(clip,seconds);
        const auto expectedWorlds=row["worlds"].toArray();double worldError=0;
        for(std::size_t i=0;i<worlds.size();++i)for(int k=0;k<16;++k)worldError=std::max(worldError,std::abs(worlds[i][k]-expectedWorlds[int(i)].toArray()[k].toDouble()));
        require(worldError<1e-4,"glTF node evaluation differs from independent source oracle");
        auto pose=buildModelGpuPose(*d,*g,{},worlds,200+results.size(),clip,seconds);
        const auto cpu=buildAnimationFrame(*d,clip,seconds,300,false,&worlds);
        auto base=std::make_shared<Frame>();auto skin=std::make_shared<SkinData>(*g->frames.model->skin);skin->ranges.clear();
        const auto samples=row["samples"].toArray();double cpuError=0;
        skin->morphRanges.clear();
        for(const auto &value:samples){const auto s=value.toObject();const auto at=s["corner"].toInt();base->vertices.push_back(g->frames.model->vertices.at(at));skin->ranges.push_back(g->frames.model->skin->ranges.at(at));
          skin->morphRanges.push_back(g->frames.model->skin->morphRanges.empty()?std::array<std::uint32_t,2>{0,0}:g->frames.model->skin->morphRanges.at(at));}
        base->rgba={1,1,1,1};base->skin=skin;base->revision=pose.model->revision;auto packet=std::make_shared<Frame>(*pose.model);packet->geometry=base;packet->draws.clear();packet->explicitDraws=false;
        const auto pixels=read(packet,1);double gpuError=0;
        for(int i=0;i<samples.size();++i){const auto s=samples[i].toObject();const auto p=s["position"].toArray();const auto expected=skin->display.map(QVector3D(float(p[0].toDouble()),float(p[1].toDouble()),float(p[2].toDouble())));const auto &v=cpu->vertices.at(s["corner"].toInt());
          cpuError=std::max(cpuError,double((QVector3D(v.x,v.y,v.z)-expected).length()));gpuError=std::max(gpuError,double((QVector3D(pixels[i*4],pixels[i*4+1],pixels[i*4+2])-expected).length()));require(std::abs(pixels[i*4+3]-1)<1e-6,"Staff GPU sample not rendered");}
        require(cpuError<1e-4 && gpuError<1e-4,"Hand/staff deformation differs from independent glTF oracle");
        results.append(QJsonObject{{"clip",clip},{"seconds",seconds},{"samples",samples.size()},{"worldError",worldError},{"cpuPositionError",cpuError},{"gpuPositionError",gpuError}});
      }
      report["independentSourceOracle"]=results;
      ModelSession session;session.enableGpuSkinning(true);session.open(path);
      settle([&]{return session.inspectable() && session.frame() && session.frame()->geometry;},90000);
      session.setClipIndex(26);const auto info=session.clipInfo();session.seek(info.value("start").toDouble()+(info.value("end").toDouble()-info.value("start").toDouble())*.371);
      const auto unlocked=session.frame()->worlds;session.setRootLock(true);const auto locked=session.frame()->worlds;
      const auto matrix=[](const formaview::Matrix4 &v){QMatrix4x4 m;for(int r=0;r<4;++r)for(int c=0;c<4;++c)m(r,c)=float(v[r*4+c]);return m;};
      const auto correction=matrix(locked.at(1))*matrix(unlocked.at(1)).inverted();double commonError=0;
      for(std::size_t i=1;i<locked.size();++i){const auto expected=correction*matrix(unlocked[i]);const auto actual=matrix(locked[i]);for(int r=0;r<4;++r)for(int c=0;c<4;++c)commonError=std::max(commonError,double(std::abs(expected(r,c)-actual(r,c))));}
      require(commonError<1e-4,"Root lock changed weapon relative to hand");report["rootLockCommonTransformError"]=commonError;session.clear();
    }
    else if(mode=="lifecycle") {
      // G5: FORMAVIEW_LIFECYCLE = open | cancel | hide | soak; REPEATS applies to the first three, args[3] seconds to soak.
      const auto phase=qEnvironmentVariable("FORMAVIEW_LIFECYCLE","open");
      if(phase=="soak") require(std::isfinite(duration) && duration>0 &&
          duration*1000<=std::numeric_limits<int>::max(),
          "Soak duration must be positive and fit the Qt timer interval");
      const int repeats=std::max(1,qEnvironmentVariable("FORMAVIEW_LIFECYCLE_REPEATS","10").toInt());
      report["phase"]=phase;report["repeats"]=repeats;
      struct Live {std::atomic<long long> renderBeat{0};std::atomic<std::uint64_t> swapchainHandle{0};QJsonArray swapchainWaitables;std::atomic<DWORD> renderThread{0};std::mutex mutex;std::uint64_t owned{},process{},releases{},revision{},swaps{},uploadedMax{};bool gpu{},collecting{true};std::vector<double> intervals,swapTimes;struct Presented{double at{},seconds{};std::uint64_t revision{};int clip{-1};};std::vector<Presented> presents;bool forensic{};std::uint64_t presentOverflow{};int latestClip{-1};double latestSeconds{};std::vector<std::array<double,2>> slow;std::vector<std::array<double,5>> heavy;std::chrono::steady_clock::time_point last{},origin{};};
      auto live=std::make_shared<Live>();
      ModelSession session;QQuickWindow window;window.setTitle("FormaView GPU lifecycle");window.resize(1280,720);
      const bool foreground=qEnvironmentVariable("FORMAVIEW_PERF_FOREGROUND")=="1";report["foregroundCondition"]=foreground;
      const bool background=qEnvironmentVariable("FORMAVIEW_PERF_BACKGROUND")=="1";
      require(!(foreground && background),"Conflicting foreground/background conditions");
      report["backgroundCondition"]=background;
      if(background)report["backgroundKind"]="Visible focus-disabled window; occlusion/minimization not controlled";
      if(background)window.setFlag(Qt::WindowDoesNotAcceptFocus);
      if(foreground)window.setFlag(Qt::WindowStaysOnTopHint);
      auto item=std::make_unique<ModelView>(window.contentItem());item->setSize({1280,720});item->setSession(&session);
      item->setFixedColorBufferWidth(1920);item->setFixedColorBufferHeight(1080);
      report["fixedColorBufferPixels"]=QJsonArray{1920,1080};
      item->observe=[live](Observation o){live->renderBeat=beatNow();live->renderThread=GetCurrentThreadId();std::lock_guard lock(live->mutex);if(o.released){++live->releases;live->owned=0;return;}live->owned=o.owned_gpu_bytes;live->process=o.process_gpu_bytes;live->revision=o.frame_revision;live->latestClip=o.clip;live->latestSeconds=o.seconds;live->gpu=o.gpu_skinning;live->uploadedMax=std::max(live->uploadedMax,o.uploaded_bytes);if(live->origin.time_since_epoch().count() && (o.prepare_ms>3 || o.record_ms>3 || o.uploaded_bytes>100000))live->heavy.push_back({std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-live->origin).count(),o.prepare_ms,o.record_ms,static_cast<double>(o.uploaded_bytes),o.gpu_skinning?1.:0.});};
      QObject::connect(&window,&QQuickWindow::frameSwapped,&window,[live]{std::lock_guard lock(live->mutex);if(!live->collecting)return;const auto now=std::chrono::steady_clock::now();++live->swaps;const bool timed=live->origin.time_since_epoch().count()>0;const double at=timed?std::chrono::duration<double,std::milli>(now-live->origin).count():0;if(live->last.time_since_epoch().count()){const double gap=std::chrono::duration<double,std::milli>(now-live->last).count();live->intervals.push_back(gap);if(timed && gap>50)live->slow.push_back({at,gap});}if(timed){live->swapTimes.push_back(at);if(live->forensic){if(live->presents.size()<live->presents.capacity())live->presents.push_back({at,live->latestSeconds,live->revision,live->latestClip});else ++live->presentOverflow;}}live->last=now;},Qt::DirectConnection);
      QJsonArray rows;QElapsedTimer total;total.start();
      const auto saveRows=qScopeGuard([&]{report["rows"]=rows;});
      const auto sample=[&](const QString &label){std::lock_guard lock(live->mutex);auto row=usageRow(label,total.elapsed(),live->owned,live->process,live->releases);row["active"]=window.isActive();row["exposed"]=window.isExposed();row["nativeForeground"]=GetForegroundWindow()==reinterpret_cast<HWND>(window.winId());row["dpr"]=window.devicePixelRatio();row["refreshHz"]=window.screen()?window.screen()->refreshRate():0.;row["effectiveWidth"]=item?item->effectiveColorBufferSize().width():0;row["effectiveHeight"]=item?item->effectiveColorBufferSize().height():0;rows.append(row);};
      const auto pause=[&](int ms){QElapsedTimer t;t.start();while(t.elapsed()<ms){QCoreApplication::processEvents();QThread::msleep(1);}};
      const auto load=[&]{openSample(session,path,externals);settle([&]{return !session.busy() && session.inspectable() && session.frame() && bool(session.frame()->geometry);},90000);};
      const auto liveGpu=[&]{std::lock_guard lock(live->mutex);return live->gpu;};
      window.show();if(foreground){window.raise();window.requestActivate();}
      sample("start");
      if(phase=="open") {
        for(int i=0;i<repeats;++i) {
          load();session.setLoop(true);session.togglePlay();pause(1000);
          require(session.playing(),"Lifecycle playback stopped");require(liveGpu(),"Lifecycle frame not on GPU");
          session.togglePlay();session.clear();pause(400);
          require(!session.available(),"Cleared session still holds a document");
          sample("open "+QString::number(i+1));
        }
        // Diagnostic only: if committed memory falls after compaction the retained bytes were allocator cache, not live data.
        _heapmin();HeapCompact(GetProcessHeap(),0);pause(300);sample("compacted");
      } else if(phase=="cancel") {
        const int delays[]={0,1,3,8,20,60,150,400,1200};int real=0,natural=0;QJsonArray cancels;
        for(int i=0;i<repeats;++i) {
          const int delay=delays[i%9];
          session.open(path);pause(delay);
          const bool inFlight=session.busy();
          QElapsedTimer cancelTimer;cancelTimer.start();
          session.cancel();
          const double cancelCallMs=cancelTimer.nsecsElapsed()/1e6;
          while(session.busy()&&cancelTimer.elapsed()<60000){QCoreApplication::processEvents();QThread::msleep(1);}
          const double cancelMs=cancelTimer.nsecsElapsed()/1e6;
          require(!session.busy(),"Cancel did not clear busy within 60 s");
          pause(1500); // A stale worker result must not publish after cancel.
          require(!session.available() && session.error().isEmpty(),"Cancelled load published a document or error");
          if(inFlight)++real;else ++natural;
          cancels.append(QJsonObject{{"delayMs",delay},{"inFlightAtCancel",inFlight},{"cancelCallMs",cancelCallMs},{"cancelToIdleMs",cancelMs}});
          sample("cancel "+QString::number(i+1));
        }
        report["cancels"]=cancels;report["realCancels"]=real;report["completedBeforeCancel"]=natural;
        {std::vector<double> inflightMs;for(const auto &c:cancels){const auto o=c.toObject();if(o["inFlightAtCancel"].toBool())inflightMs.push_back(o["cancelToIdleMs"].toDouble());}
         std::sort(inflightMs.begin(),inflightMs.end());if(!inflightMs.empty()){report["cancelToIdleP95Ms"]=inflightMs[std::min(inflightMs.size()-1,static_cast<std::size_t>(std::ceil(inflightMs.size()*0.95))-1)];report["cancelToIdleMaxMs"]=inflightMs.back();}}
        require(real>=std::max(1,repeats/3),"Too few loads were actually in flight when cancelled");
        load();require(session.path()==path,"Session unusable after cancelled loads");sample("reopen");
      } else if(phase=="close") {
        // Closing the window while a load runs: each repeat destroys a fresh
        // view mid-load and then the session itself, without cancelling - the
        // teardown order a window close followed by app quit produces. The
        // session destructor blocks on the in-flight worker, which is the
        // exit latency a user would see; the final reopen proves the process
        // stays usable afterwards.
        const int delays[]={0,1,3,8,20,60,150,400,1200};int real=0,natural=0;QJsonArray closes;
        item.reset();window.hide();window.releaseResources();QCoreApplication::processEvents(); // this phase owns its windows
        const auto build=[&](ModelSession *target){
          auto w=std::make_unique<QQuickWindow>();w->setTitle("FormaView GPU lifecycle close");w->resize(1280,720);
          if(background)w->setFlag(Qt::WindowDoesNotAcceptFocus);
          if(foreground)w->setFlag(Qt::WindowStaysOnTopHint);
          auto it=std::make_unique<ModelView>(w->contentItem());it->setSize({1280,720});it->setSession(target);
          it->observe=[live](Observation o){std::lock_guard lock(live->mutex);if(o.released){++live->releases;live->owned=0;return;}live->owned=o.owned_gpu_bytes;live->process=o.process_gpu_bytes;live->revision=o.frame_revision;live->gpu=o.gpu_skinning;};
          QObject::connect(w.get(),&QQuickWindow::frameSwapped,w.get(),[live]{std::lock_guard lock(live->mutex);++live->swaps;},Qt::DirectConnection);
          w->show();if(foreground){w->raise();w->requestActivate();}
          return std::pair{std::move(w),std::move(it)};
        };
        for(int i=0;i<repeats;++i) {
          const int delay=delays[i%9];
          auto owned=std::make_unique<ModelSession>();
          auto view=build(owned.get());
          pause(300); // let the renderer initialize before the load starts
          const auto reference=qEnvironmentVariable("FORMAVIEW_GPU_REFERENCE");if(!reference.isEmpty())owned->setAnimationReference(reference);
          owned->open(path);pause(delay);
          const bool inFlight=owned->busy();
          view.second.reset();view.first->hide();view.first->releaseResources();pause(50);view.first.reset();QCoreApplication::processEvents();
          QElapsedTimer teardown;teardown.start();owned.reset();const qint64 teardownMs=teardown.elapsed();
          if(inFlight)++real;else ++natural;
          closes.append(QJsonObject{{"delayMs",delay},{"inFlightAtClose",inFlight},{"sessionTeardownMs",teardownMs}});
          sample("close "+QString::number(i+1));
        }
        report["closes"]=closes;report["realCloses"]=real;report["completedBeforeClose"]=natural;
        require(real>=std::max(1,repeats/3),"Too few loads were actually in flight when the window closed");
        for(const auto &entry:closes)require(entry.toObject().value("sessionTeardownMs").toInt()<120000,"Session teardown hung after close during load");
        auto reopen=build(&session);
        load();session.setLoop(true);session.togglePlay();pause(1000);
        require(session.playing() && liveGpu(),"Process unusable after close-during-load");
        session.togglePlay();session.clear();reopen.second.reset();reopen.first.reset();QCoreApplication::processEvents();
        sample("reopen");
      } else if(phase=="close-external") {
        // Closing the window while external animation attaches are in flight:
        // the document is open and inspectable, every FORMAVIEW_GPU_EXTERNALS
        // file has been submitted (<=8 readers run, the rest queue), and the
        // view plus session are destroyed without cancelling - the window
        // close during a multi-file attach. Same teardown-order and reopen
        // verification as "close"; the in-flight source is externalBusy().
        require(!externals.isEmpty(),"close-external needs FORMAVIEW_GPU_EXTERNALS");
        const int delays[]={0,1,3,8,20,60,150,400};int real=0,natural=0;QJsonArray closes;
        item.reset();window.hide();window.releaseResources();QCoreApplication::processEvents(); // this phase owns its windows
        const auto build=[&](ModelSession *target){
          auto w=std::make_unique<QQuickWindow>();w->setTitle("FormaView GPU lifecycle close-external");w->resize(1280,720);
          if(background)w->setFlag(Qt::WindowDoesNotAcceptFocus);
          if(foreground)w->setFlag(Qt::WindowStaysOnTopHint);
          auto it=std::make_unique<ModelView>(w->contentItem());it->setSize({1280,720});it->setSession(target);
          it->observe=[live](Observation o){std::lock_guard lock(live->mutex);if(o.released){++live->releases;live->owned=0;return;}live->owned=o.owned_gpu_bytes;live->process=o.process_gpu_bytes;live->revision=o.frame_revision;live->gpu=o.gpu_skinning;};
          QObject::connect(w.get(),&QQuickWindow::frameSwapped,w.get(),[live]{std::lock_guard lock(live->mutex);++live->swaps;},Qt::DirectConnection);
          w->show();if(foreground){w->raise();w->requestActivate();}
          return std::pair{std::move(w),std::move(it)};
        };
        for(int i=0;i<repeats;++i) {
          const int delay=delays[i%8];
          auto owned=std::make_unique<ModelSession>();
          auto view=build(owned.get());
          pause(300); // let the renderer initialize before the load starts
          const auto reference=qEnvironmentVariable("FORMAVIEW_GPU_REFERENCE");if(!reference.isEmpty())owned->setAnimationReference(reference);
          owned->open(path);settle([&]{return !owned->busy() && owned->inspectable();},90000);
          for(const auto &external:externals)owned->attachExternalAnimation(external);
          pause(delay);
          const bool inFlight=owned->externalBusy();
          // Product window-close order: hide first so the render loop stops
          // producing frames, then release GPU resources, then destroy the
          // view and window - not item-first while the window is exposed.
          view.first->hide();pause(50);view.first->releaseResources();QCoreApplication::processEvents();
          view.second.reset();view.first.reset();QCoreApplication::processEvents();
          QElapsedTimer teardown;teardown.start();owned.reset();const qint64 teardownMs=teardown.elapsed();
          if(inFlight)++real;else ++natural;
          closes.append(QJsonObject{{"delayMs",delay},{"inFlightAtClose",inFlight},{"sessionTeardownMs",teardownMs}});
          sample("close-external "+QString::number(i+1));
        }
        report["closes"]=closes;report["realCloses"]=real;report["completedBeforeClose"]=natural;report["externals"]=externals.size();
        require(real>=std::max(1,repeats/3),"Too few attaches were actually in flight when the window closed");
        for(const auto &entry:closes)require(entry.toObject().value("sessionTeardownMs").toInt()<120000,"Session teardown hung after close during attach");
        auto reopen=build(&session);
        load();session.setLoop(true);session.togglePlay();pause(1000);
        require(session.playing() && liveGpu(),"Process unusable after close-during-attach");
        session.togglePlay();session.clear();reopen.second.reset();reopen.first.reset();QCoreApplication::processEvents();
        sample("reopen");
      } else if(phase=="hide") {
        // Qt keeps the scene graph across hide by default; opt out so every hide destroys the renderer.
        window.setPersistentGraphics(false);window.setPersistentSceneGraph(false);
        load();session.setLoop(true);session.togglePlay();pause(800);sample("playing");
        QJsonArray cycles;
        for(int i=0;i<repeats;++i) {
          std::uint64_t releasesBefore,revisionBefore;{std::lock_guard lock(live->mutex);releasesBefore=live->releases;revisionBefore=live->revision;}
          const double timeBefore=session.time();
          window.hide();pause(300);window.releaseResources();pause(100);window.show();
          if(foreground){window.raise();window.requestActivate();}
          settle([&]{std::lock_guard lock(live->mutex);return live->revision>revisionBefore && live->gpu && live->owned>0;},15000);
          pause(300);
          std::uint64_t releasesAfter;{std::lock_guard lock(live->mutex);releasesAfter=live->releases;}
          require(session.playing(),"Playback stopped across hide/show");require(liveGpu(),"Not on GPU after show");
          cycles.append(QJsonObject{{"cycle",i+1},{"rendererReleases",static_cast<qint64>(releasesAfter-releasesBefore)},{"timeAdvance",session.time()-timeBefore}});
          sample("hide "+QString::number(i+1));
        }
        report["cycles"]=cycles;
        {std::lock_guard lock(live->mutex);require(live->releases>=static_cast<std::uint64_t>(repeats),"Hide did not release the renderer each cycle");}
        session.togglePlay();session.clear();pause(400);sample("cleared");
      } else if(phase=="state") {
        // Residency baseline: view-state switches in mode 0 must reuse the resident GPU geometry.
        const bool lodSample=qEnvironmentVariable("FORMAVIEW_LIFECYCLE_LOD")=="1";
        const bool playingDuring=qEnvironmentVariable("FORMAVIEW_LIFECYCLE_PLAY")=="1";
        load();
        if(lodSample) {
          session.selectMesh(0);session.classifyMesh("review",0,false);session.selectMesh(1);session.classifyMesh("review",1,false);session.setMode(0);
          settle([&]{return session.renderedStateCurrent() && session.inspectable();},180000);
          session.selectLodGroup(0);settle([&]{return session.renderedStateCurrent() && session.inspectable();},180000);
        }
        if(playingDuring){session.setLoop(true);session.togglePlay();pause(500);}
        std::map<QString,std::vector<double>> latency;std::map<QString,std::uint64_t> builds;QJsonArray ops;QStringList correctness;
        // The presented subset must equal what the original (per-state) CPU build shows for the same state.
        const auto verifyPresented=[&](const QString &name) {
          const auto doc=session.documentSnapshot();const auto state=session.renderStateForTest();const auto gpu=session.frame();
          if(!gpu || !gpu->geometry){correctness.append(name+": no GPU frame");return;}
          const auto cpu=buildModelFrames(*doc,state,5000,{},nullptr,false).model;
          std::uint64_t total=0,blend=0,cpuBlend=0;for(const auto &draw:gpu->draws){total+=draw.count;if(draw.blend)blend+=draw.count;}
          for(const auto &draw:cpu->draws)if(draw.blend)cpuBlend+=draw.count;
          if(total!=cpu->vertices.size())correctness.append(name+": shown vertices "+QString::number(total)+" != "+QString::number(cpu->vertices.size()));
          if(blend!=cpuBlend)correctness.append(name+": transparent vertices differ");
          if(gpu->sourceTriangles!=cpu->sourceTriangles || gpu->sourcePoints!=cpu->sourcePoints)correctness.append(name+": source statistics differ");
          const auto pose=buildAnimationFrame(*doc,static_cast<std::size_t>(gpu->clip),gpu->seconds,5001,doc->actorSkinDomain(),&gpu->worlds);
          const auto posed=buildModelFrames(*doc,state,5002,{},pose.get(),false).model;
          if(posed->hasDisplayBounds && total>0) {
            if(!gpu->hasDisplayBounds)correctness.append(name+": no display bounds");
            else for(int axis=0;axis<3;++axis)if(gpu->displayLow[axis]>posed->displayLow[axis]+1e-4F || gpu->displayHigh[axis]<posed->displayHigh[axis]-1e-4F)correctness.append(name+": display bounds do not contain shown geometry");
          }
          if(cpu->selectionVertices>0 && gpu->selectionVertices==0)correctness.append(name+": highlight missing");
        };
        const auto measure=[&](const QString &name,const std::function<void()> &run) {
          const auto buildsBefore=modelGpuGeometryBuildCount();const auto before=session.frame()?session.frame()->geometry:nullptr;
          std::cerr<<"state op: "<<name.toStdString()<<std::endl;
          QElapsedTimer clock;clock.start();run();
          settle([&]{return session.renderedStateCurrent() && session.inspectable();},180000);
          const double ms=static_cast<double>(clock.nsecsElapsed())/1e6;const auto after=session.frame()?session.frame()->geometry:nullptr;
          const auto delta=modelGpuGeometryBuildCount()-buildsBefore;
          latency[name].push_back(ms);builds[name]+=delta;verifyPresented(name);
          ops.append(QJsonObject{{"op",name},{"ms",ms},{"geometryBuilds",static_cast<qint64>(delta)},{"geometryReplaced",before!=after},{"vertices",static_cast<qint64>(session.frame()?session.frame()->vertices.size():0)},{"drawCount",static_cast<qint64>(session.frame()?session.frame()->draws.size():0)}});
          pause(60);
        };
        const int cycles=std::max(1,qEnvironmentVariable("FORMAVIEW_LIFECYCLE_CYCLES","4").toInt());
        const int meshCount=static_cast<int>(session.documentSnapshot()->meshes.size());
        for(int c=0;c<cycles;++c) {
          for(int bone:{0,3,7}) if(bone<session.bones().size())measure("selectBone",[&]{session.selectBone(bone);});
          measure("isolate on",[&]{session.setIsolated(true);});
          measure("selectMesh isolated",[&]{session.selectMesh(meshCount>1?1:0);});
          measure("selectMesh isolated",[&]{session.selectMesh(0);});
          measure("isolate off",[&]{session.setIsolated(false);});
          measure("selectMesh",[&]{session.selectMesh(meshCount>1?1:0);});
          measure("selectMesh",[&]{session.selectMesh(0);});
          if(lodSample){measure("requestLod",[&]{session.requestLod(1);});measure("requestLod",[&]{session.requestLod(0);});}
        }
        QJsonObject summary;QStringList failures;
        for(const auto &[name,values]:latency) {
          const double p95=percentile(values,.95);
          summary[name]=QJsonObject{{"count",static_cast<int>(values.size())},{"p95Ms",p95},{"maxMs",*std::max_element(values.begin(),values.end())},{"geometryBuilds",static_cast<qint64>(builds[name])}};
          if(builds[name]!=0)failures.append(name+": "+QString::number(builds[name])+" geometry rebuild(s)");
          if(p95>100)failures.append(name+": p95 "+QString::number(p95,'f',1)+" ms > 100 ms");
        }
        for(const auto &problem:correctness)failures.append("correctness: "+problem);
        report["correctness"]=QJsonArray::fromStringList(correctness);
        report["stateOps"]=ops;report["stateSummary"]=summary;report["residencyFailures"]=QJsonArray::fromStringList(failures);
        report["residencyPassed"]=failures.isEmpty();
        if(playingDuring)session.togglePlay();
        require(failures.isEmpty(),qPrintable("Residency: "+failures.join("; ")));
      } else if(phase=="lodfeedback") {
        // Input feedback of an LOD switch: how long the GUI thread is held by
        // requestLod() and how soon the request is visible (requested level,
        // preparing). Completion (rendered state current) is reported apart.
        // Run with FORMAVIEW_GPU_FORCE_FALLBACK=1 for the non-resident path.
        load();
        const bool synthetic=qEnvironmentVariable("FORMAVIEW_LIFECYCLE_LOD")=="1";
        if(synthetic) {
          session.selectMesh(0);session.classifyMesh("review",0,false);session.selectMesh(1);session.classifyMesh("review",1,false);session.setMode(0);
          settle([&]{return session.renderedStateCurrent() && session.inspectable();},180000);
        }
        require(!session.lodGroups().isEmpty(),"No LOD group in sample");
        session.selectLodGroup(0);settle([&]{return session.renderedStateCurrent() && session.inspectable();},180000);
        const auto levels=session.lodLevels();require(levels.size()>=2,"Need at least two LOD levels");
        std::vector<int> ids;for(const auto &v:levels)ids.push_back(v.toMap().value("level").toInt());
        std::vector<double> inputMs,visibleMs,doneMs;QJsonArray rowsOut;
        const int rounds=std::max(1,qEnvironmentVariable("FORMAVIEW_LIFECYCLE_CYCLES","10").toInt());
        for(int r=0;r<rounds;++r) for(const int id:ids) {
          QElapsedTimer clock;clock.start();
          session.requestLod(id);
          const double input=static_cast<double>(clock.nsecsElapsed())/1e6;
          const auto level=session.lodLevels();bool requested=false;
          for(const auto &v:level)if(v.toMap().value("level").toInt()==id)requested=v.toMap().value("requested").toBool();
          const double visible=static_cast<double>(clock.nsecsElapsed())/1e6;
          require(requested,"Requested level not visible right after the call");
          settle([&]{return session.renderedStateCurrent() && session.inspectable();},180000);
          const double done=static_cast<double>(clock.nsecsElapsed())/1e6;
          inputMs.push_back(input);visibleMs.push_back(visible);doneMs.push_back(done);
          rowsOut.append(QJsonObject{{"level",id},{"inputMs",input},{"visibleMs",visible},{"completeMs",done}});
          pause(60);
        }
        const double inputP95=percentile(inputMs,.95),visibleP95=percentile(visibleMs,.95),doneP95=percentile(doneMs,.95);
        report["lodFeedback"]=QJsonObject{{"rounds",rounds},{"levels",static_cast<int>(ids.size())},{"switches",static_cast<int>(inputMs.size())},{"gpuResident",liveGpu()},{"inputP95Ms",inputP95},{"visibleP95Ms",visibleP95},{"completeP95Ms",doneP95},{"completeMaxMs",*std::max_element(doneMs.begin(),doneMs.end())},{"rows",rowsOut}};
        require(visibleP95<=50.0,qPrintable("LOD input feedback P95 "+QString::number(visibleP95,'f',1)+" ms > 50 ms"));
      } else if(phase=="soak") {
        load();session.setLoop(true);session.togglePlay();
        // A foreground soak must stay foreground; a completed process alone
        // does not validate that condition. Activate once, never steal it back.
        const auto hwnd=reinterpret_cast<HWND>(window.winId());
        std::unique_ptr<ForegroundTrace> foregroundTrace;
        if(foreground) foregroundTrace=std::make_unique<ForegroundTrace>(hwnd);
        const auto saveForegroundTrace=qScopeGuard([&]{
          if(foregroundTrace){foregroundTrace->stop();report["foregroundTrace"]=foregroundTrace->report();}
        });
        if(foreground) require(foregroundTrace->ready(),"Cannot install foreground activation journal");
        if(foreground) {
          const DWORD foregroundThread=GetWindowThreadProcessId(GetForegroundWindow(),nullptr);
          const bool attached=foregroundThread && foregroundThread!=GetCurrentThreadId()
            && AttachThreadInput(GetCurrentThreadId(),foregroundThread,TRUE);
          window.raise();window.requestActivate();BringWindowToTop(hwnd);
          const bool activated=SetForegroundWindow(hwnd);
          if(attached)AttachThreadInput(GetCurrentThreadId(),foregroundThread,FALSE);
          pause(300);
          report["foregroundActivation"]=QJsonObject{{"attachSucceeded",attached},{"setForegroundSucceeded",activated},
            {"active",window.isActive()},{"exposed",window.isExposed()},{"nativeForeground",GetForegroundWindow()==hwnd}};
          require(window.isActive() && window.isExposed() && GetForegroundWindow()==hwnd,"Foreground soak could not acquire initial focus");
        }
        bool foregroundHeld=true;
        const auto focusControl=qEnvironmentVariable("FORMAVIEW_SOAK_FOCUS_LOSS_CONTROL");
        const bool focusLossControl=!focusControl.isEmpty();
        report["focusLossControl"]=focusLossControl;
        report["focusLossControlKind"]=focusControl;
        std::unique_ptr<QWindow> activationControl;
        if(focusLossControl) {
          require(foreground && duration>=10,"Focus-loss control requires a foreground soak of at least 10 seconds");
          require(focusControl=="1" || focusControl=="activate","Unknown focus-loss control");
          if(focusControl=="1") QTimer::singleShot(5000,&window,&QWindow::hide);
          else {
            activationControl=std::make_unique<QWindow>();
            activationControl->setTitle("FormaView controlled activation target");
            activationControl->setFlags(Qt::Tool | Qt::WindowStaysOnTopHint);
            activationControl->resize(240,120);
            QTimer::singleShot(5000,&window,[&]{
              activationControl->show();activationControl->requestActivate();
              SetForegroundWindow(reinterpret_cast<HWND>(activationControl->winId()));
            });
          }
        }
        const auto clips=static_cast<int>(session.clips().size());const int rotate=std::max(1,std::min(clips,8));
        QElapsedTimer run;run.start();qint64 nextSample=0,nextClip=10000,nextSpeed=20000,nextLock=30000;int tick=0;bool rootLocked=false;
        const double speeds[]={1.0,2.0,.5,1.0};
        const auto origin=std::chrono::steady_clock::now();{std::lock_guard lock(live->mutex);/* Intervals recorded since the window first showed, i.e. while the model loaded, before the soak clock starts. */QJsonArray loadGaps;int loadOver100=0;for(double v:live->intervals){if(v>100)++loadOver100;if(v>50)loadGaps.append(v);}report["loadPhaseIntervals"]=static_cast<int>(live->intervals.size());report["loadPhaseOver100Ms"]=loadOver100;report["loadPhaseOver50Ms"]=loadGaps;live->origin=origin;live->swapTimes.clear();live->intervals.clear();live->last={};}
        if(foregroundTrace) report["foregroundJournalSoakStartMs"]=foregroundTrace->elapsedMs();
        const auto since=[&]{return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-origin).count();};
        std::unique_ptr<model_forensics::Probe> diagnostics;QMetaObject::Connection poseConnection,renderDiagnosticConnection;
        std::atomic<bool> renderCalibrated{false};
        if(qEnvironmentVariable("FORMAVIEW_LIFECYCLE_FORENSICS")=="1"){
          const auto capacity=static_cast<std::size_t>(std::clamp(duration*200.0+10000.0,10000.0,500000.0));
          diagnostics=std::make_unique<model_forensics::Probe>(origin,capacity);
          poseConnection=QObject::connect(&session,&ModelSession::frameChanged,&session,[&]{diagnostics->publish(session.frame());});
          diagnostics->publish(session.frame());
          const bool renderControl=qEnvironmentVariable("FORMAVIEW_FORENSICS_RENDER_CALIBRATE")=="1";
          renderDiagnosticConnection=QObject::connect(&window,&QQuickWindow::beforeRendering,&window,[&,renderControl]{
            // Qt 6.12.0 is pinned by CMake. Read-only, test-only native access on render thread.
            if(window.rhi()&&window.rhi()->backend()==QRhi::D3D11&&window.swapChain()) {
              auto *sc=static_cast<QD3D11SwapChain*>(window.swapChain());
              const auto raw=reinterpret_cast<std::uintptr_t>(sc->frameLatencyWaitableObject);
              if(raw!=live->swapchainHandle.load()) {
                IDXGISwapChain2 *dxgi=nullptr;const auto hr=sc->swapChain?sc->swapChain->QueryInterface(__uuidof(IDXGISwapChain2),reinterpret_cast<void**>(&dxgi)):E_POINTER;
                HANDLE fromDxgi=dxgi?dxgi->GetFrameLatencyWaitableObject():nullptr;
                using Compare=BOOL(WINAPI*)(HANDLE,HANDLE);
                const auto compare=reinterpret_cast<Compare>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"),"CompareObjectHandles"));
                const bool same=compare&&fromDxgi&&compare(sc->frameLatencyWaitableObject,fromDxgi);
                const auto qtObject=model_forensics::waitObject(sc->frameLatencyWaitableObject);
                // Getter returns a fresh owned handle on this implementation. Keep Qt's handle untouched.
                if(fromDxgi&&fromDxgi!=sc->frameLatencyWaitableObject)CloseHandle(fromDxgi);
                if(dxgi)dxgi->Release();
                std::lock_guard lock(live->mutex);live->swapchainWaitables.append(QJsonObject{
                  {"atMs",since()},{"qtHandle",QString::number(raw,16)},
                  {"dxgiHandle",QString::number(reinterpret_cast<std::uintptr_t>(fromDxgi),16)},
                  {"sameKernelObject",same},{"compareApiAvailable",compare!=nullptr},{"qtObject",qtObject},{"queryHresult",static_cast<qint64>(hr)},{"renderTid",static_cast<qint64>(GetCurrentThreadId())},
                  {"qtVersion",qVersion()}});live->swapchainHandle=raw;
              }
            }
            if(renderControl&&since()>20000&&!renderCalibrated.exchange(true))diagnostics->calibrateRenderEvent();
          },Qt::DirectConnection);
          std::lock_guard lock(live->mutex);live->forensic=true;live->presents.reserve(capacity);
        }
        const auto disconnectPose=qScopeGuard([&]{if(diagnostics){QObject::disconnect(poseConnection);QObject::disconnect(renderDiagnosticConnection);
          window.hide();window.releaseResources();QCoreApplication::processEvents();}});
        QTimer calibration;calibration.setSingleShot(true);
        if(qEnvironmentVariable("FORMAVIEW_FORENSICS_CALIBRATE")=="1"){
          require(diagnostics&&duration>=30,"Forensic calibration requires enabled diagnostics and >=30 seconds");
          QObject::connect(&calibration,&QTimer::timeout,&window,[&]{diagnostics->calibrateMutex();});calibration.start(15000);
        }
        QTimer resizeControl;resizeControl.setSingleShot(true);
        if(qEnvironmentVariable("FORMAVIEW_FORENSICS_RESIZE")=="1") {
          require(diagnostics&&duration>=60,"Resize control requires diagnostics and >=60 seconds");
          QObject::connect(&resizeControl,&QTimer::timeout,&window,[&]{diagnostics->resizeControl(reinterpret_cast<HWND>(window.winId()));});resizeControl.start(30000);
        }
        const bool stimulus=qEnvironmentVariable("FORMAVIEW_FORENSICS_STIMULUS")=="1";
        report["forensicStimulus"]=stimulus;qint64 nextStimulus=5000;int stimulusIndex=0;
        const bool injectStall=qEnvironmentVariable("FORMAVIEW_LIFECYCLE_INJECT_STALL")=="1";bool injected=false;report["injectedStall"]=injectStall;
        const auto watchSetting=qEnvironmentVariable("FORMAVIEW_LIFECYCLE_WATCHDOG","1");
        require(watchSetting=="0" || watchSetting=="1","Watchdog switch must be 0 or 1");
        const bool watchEnabled=watchSetting=="1";report["watchdogEnabled"]=watchEnabled;
        Watch watch;watch.samples.reserve(8000);watch.probe=diagnostics.get();watch.renderThread=&live->renderThread;watch.swapchainHandle=&live->swapchainHandle;watch.renderBeat=diagnostics?&live->renderBeat:nullptr;
        HANDLE guiThread=nullptr;
        const auto closeGui=qScopeGuard([&]{if(guiThread)CloseHandle(guiThread);});
        std::thread watchdog;bool watchStopped=false;
        const auto stopWatch=qScopeGuard([&]{if(!watchStopped){watch.run=false;if(watchdog.joinable())watchdog.join();}});
        if(watchEnabled) {
          require(DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&guiThread,THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,0),"Cannot open own GUI thread for watchdog");
          watchdog=std::thread(watchdogLoop,guiThread,&watch,origin);
        }
        QJsonArray events;
        const auto timedAction=[&](const char *type,const std::function<void()> &action){const double start=since();action();events.append(QJsonObject{{"ms",start},{"type",type},{"callMs",since()-start}});};
        // Run the same event dispatcher as the application. Sleeping after a
        // manual processEvents() delays queued frame/vsync notifications and
        // adds test-generated jitter to the unchanged frameSwapped metric.
        QEventLoop soakLoop;
        QTimer actions,deadline;
        actions.setTimerType(Qt::PreciseTimer);actions.setSingleShot(true);
        deadline.setTimerType(Qt::PreciseTimer);deadline.setSingleShot(true);
        const auto durationMs=static_cast<qint64>(duration*1000);
        unsigned deadlineRearms=0;
        std::exception_ptr controlError;
        bool finished=false;
        const auto checkState=[&] {
          watch.beat=beatNow();
          const auto now=run.elapsed();
          const auto observedForeground=foreground ? GetForegroundWindow() : nullptr;
          if(foreground && (!window.isActive() || !window.isExposed() || observedForeground!=hwnd)) {
            foregroundTrace->observedLoss(observedForeground,window.isActive(),window.isExposed());
            foregroundHeld=false;report["foregroundLostAtMs"]=now;sample("soak focus lost");finished=true;return;
          }
          require(session.playing() && session.frameError().isEmpty(),"Soak playback stopped");
        };
        const auto controlStep=[&] {
          checkState();if(finished)return;
          if(injectStall && !injected && run.elapsed()>=15000){injected=true;injectedStallForWatchdogValidation();}
          const auto now=run.elapsed();
          if(stimulus&&now>=nextStimulus){
            const UINT messages[]={WM_GETTEXTLENGTH,WM_NCHITTEST,WM_THEMECHANGED};
            const auto message=messages[stimulusIndex++%3];
            POINT point{10,10};ClientToScreen(hwnd,&point);
            timedAction("controlledNativeMessage",[&]{SendMessageW(hwnd,message,0,message==WM_NCHITTEST?MAKELPARAM(point.x,point.y):0);});
            nextStimulus+=5000;
          }
          if(now>=nextSample){timedAction("sample",[&]{sample("soak "+QString::number(now/1000));});nextSample+=5000;}
          if(now>=nextClip){++tick;timedAction("setClipIndex",[&]{session.setClipIndex(tick%rotate);});if(tick%3==0)timedAction("seek",[&]{const auto info=session.clipInfo();session.seek(info.value("start").toDouble()+(info.value("end").toDouble()-info.value("start").toDouble())*((tick%7)/7.0));});nextClip+=10000;}
          if(now>=nextSpeed){timedAction("setSpeed",[&]{session.setSpeed(speeds[(now/20000)%4]);});nextSpeed+=20000;}
          if(now>=nextLock){rootLocked=!rootLocked;timedAction("setRootLock",[&]{session.setRootLock(rootLocked);});nextLock+=30000;}
          require(session.playing() && session.frameError().isEmpty(),"Soak playback stopped");
        };
        const auto checked=[&](const auto &action) {
          if(finished)return;
          try {action();} catch(...) {controlError=std::current_exception();finished=true;}
          if(finished)soakLoop.quit();
        };
        auto *dispatcher=QAbstractEventDispatcher::instance();
        require(dispatcher,"Missing GUI event dispatcher");
        const auto awake=QObject::connect(dispatcher,&QAbstractEventDispatcher::awake,&soakLoop,[&]{checked(checkState);});
        const auto idle=QObject::connect(dispatcher,&QAbstractEventDispatcher::aboutToBlock,&soakLoop,[&]{checked(checkState);});
        const auto active=QObject::connect(&window,&QWindow::activeChanged,&soakLoop,[&]{checked(checkState);});
        const auto visible=QObject::connect(&window,&QWindow::visibleChanged,&soakLoop,[&]{checked(checkState);});
        const auto disconnectChecks=qScopeGuard([&]{QObject::disconnect(awake);QObject::disconnect(idle);QObject::disconnect(active);QObject::disconnect(visible);});
        const auto armActions=[&] {
          auto next=std::min({nextSample,nextClip,nextSpeed,nextLock});
          if(stimulus)next=std::min(next,nextStimulus);
          if(injectStall&&!injected)next=std::min<qint64>(next,15000);
          if(next<durationMs)actions.start(int(std::max<qint64>(1,next-run.elapsed())));
        };
        QObject::connect(&actions,&QTimer::timeout,&soakLoop,[&]{checked(controlStep);if(!finished)armActions();});
        QObject::connect(&deadline,&QTimer::timeout,&soakLoop,[&]{
          // Timer delivery and QElapsedTimer millisecond rounding can differ
          // by a fraction of a millisecond. Never shorten the measured window.
          const auto remaining=durationMs-run.elapsed();
          if(remaining>0){++deadlineRearms;deadline.start(int(remaining));return;}
          checked(controlStep);finished=true;soakLoop.quit();
        });
        // Schedule the original actions against their absolute deadlines.
        // Dispatcher activity supplies the watchdog heartbeat and checks
        // native focus without introducing a high-frequency polling timer.
        checked(controlStep);
        if(!finished) {
          armActions();
          deadline.start(int(std::max<qint64>(0,durationMs-run.elapsed())));
          soakLoop.exec();
        }
        finished=true;actions.stop();deadline.stop();
        if(controlError)std::rethrow_exception(controlError);
        report["measuredDurationMs"]=run.elapsed();
        report["soakDeadlineRearms"]=int(deadlineRearms);
        {std::lock_guard lock(live->mutex);live->collecting=false;}
        watch.run=false;if(watchdog.joinable())watchdog.join();watchStopped=true;
        if(foregroundTrace) {
          // Foreground events are asynchronous. Preserve the failed boundary,
          // then drain activation notifications outside the measured interval.
          if(!foregroundHeld) {
            QElapsedTimer drain;drain.start();
            while(drain.elapsed()<250){QCoreApplication::processEvents();QThread::msleep(2);}
            foregroundTrace->afterLoss();
          }
          foregroundTrace->stop(); // Exclude our own cleanup/hide.
        }
        report["stallWatchdog"]=summarizeStalls(watch.samples);
        report["watchdogThread"]=QJsonObject{{"enabled",watchEnabled},{"checks",static_cast<qint64>(watch.checks)},
          {"suppressedEligibleChecks",static_cast<qint64>(watch.suppressed)},{"cpuMs",watch.cpuMs},
          {"samplingIntervalMs",10},{"triggerAgeMs",60},{"captureLimitPerStall",80},{"captureCapacity",8000}};
        if(diagnostics){
          report["forensics"]=diagnostics->report();auto f=report["forensics"].toObject();
          auto origins=diagnostics->originReport();const auto symbols=(QCoreApplication::applicationDirPath()+";"+qEnvironmentVariable("FORMAVIEW_FORENSICS_SYMBOLS")).toStdWString();
          SymInitializeW(GetCurrentProcess(),symbols.c_str(),TRUE);
          for(qsizetype i=0;i<origins.size();++i){auto o=origins[i].toObject();QJsonArray frames;
            for(const auto &ip:o["ips"].toArray())frames.append(describeAddress(reinterpret_cast<void*>(ip.toString().toULongLong(nullptr,16))));
            o["frames"]=frames;origins[i]=o;}
          SymCleanup(GetCurrentProcess());f["sizeOrigins"]=origins;report["forensics"]=f;
        }
        session.setSpeed(1.0);session.togglePlay();
        {std::lock_guard lock(live->mutex);const auto sorted=live->intervals;report["swapIntervals"]=static_cast<int>(sorted.size());report["swapP50Ms"]=percentile(sorted,.5);report["swapP95Ms"]=percentile(sorted,.95);report["swapP99Ms"]=percentile(sorted,.99);int over=0;for(double v:sorted)if(v>100)++over;report["swapOver100Ms"]=over;report["onGpu"]=live->gpu;
          QJsonArray swaps,slowRows,heavyRows;for(double v:live->swapTimes)swaps.append(std::round(v*1000)/1000);
          for(const auto &row:live->slow)slowRows.append(QJsonObject{{"endMs",row[0]},{"intervalMs",row[1]}});
          for(const auto &row:live->heavy)heavyRows.append(QJsonObject{{"ms",row[0]},{"prepareMs",row[1]},{"recordMs",row[2]},{"uploadedBytes",row[3]},{"gpu",row[4]>0}});
          report["swapTimesMs"]=swaps;report["slowSwaps"]=slowRows;report["heavyRenders"]=heavyRows;report["events"]=events;
          if(diagnostics){auto forensic=report["forensics"].toObject();QJsonArray presents;
            for(const auto &p:live->presents)presents.append(QJsonArray{p.at,static_cast<qint64>(p.revision),p.clip,p.seconds});
            forensic["swapchainWaitables"]=live->swapchainWaitables;forensic["presentedPoses"]=presents;forensic["presentColumns"]=QJsonArray{"atMs","revision","clip","seconds"};
            forensic["presentOverflow"]=static_cast<qint64>(live->presentOverflow);report["forensics"]=forensic;}
        }
        require(liveGpu(),"Soak ended off the GPU path");
        report["postMeasurementDiagnosticsMs"]=run.elapsed()-report["measuredDurationMs"].toInteger();
        report["foregroundConditionHeld"]=foreground ? QJsonValue(foregroundHeld) : QJsonValue(QJsonValue::Null);
        report["foregroundObservationBoundary"]="Checked at GUI dispatcher wake/block boundaries, active/visible changes, and action/deadline callbacks; one initial activation only; abort on observed focus/exposure loss, without stitching intervals";
        report["soakEventLoop"]="Native QEventLoop with absolute original action deadlines and monotonic-clock duration check; no manual processEvents/msleep polling";
        recordFrameTimeGate(report,report["swapP95Ms"].toDouble(),report["swapP99Ms"].toDouble(),report["swapIntervals"].toInt()>0);
        session.clear();pause(500);sample("cleared");
        require(foregroundHeld,"Foreground soak lost focus or exposure; partial run is not a completed foreground soak");
        if(foregroundTrace) require(foregroundTrace->complete(),"Foreground activation journal incomplete");
      } else if(phase=="exit") {
        // True process exit with a load in flight: the report is written
        // first, then the branch-scope objects (view, window, session) are
        // destroyed at the closing brace with the worker still running - the
        // same order as app quit - and main returns immediately after.
        const int delay=qEnvironmentVariable("FORMAVIEW_LIFECYCLE_DELAY","0").toInt();
        report["delayMs"]=delay;
        const auto exitReference=qEnvironmentVariable("FORMAVIEW_GPU_REFERENCE");if(!exitReference.isEmpty())session.setAnimationReference(exitReference);
        session.open(path);pause(delay);
        report["inFlightAtExit"]=session.busy();
        sample("opening");
        report["passed"]=true;report["exitTeardownPending"]=true;
        {QFile file(args[2]);if(file.open(QIODevice::WriteOnly))file.write(QJsonDocument(report).toJson());}
      } else throw std::runtime_error("Unknown lifecycle phase");
      report["rows"]=rows;
      // Proposed bounds fixed before measurement: handles +32, private +25% (+64 MiB) over the warm baseline.
      QJsonArray phaseRows;for(const auto &value:rows)if(value.toObject()["label"].toString().startsWith(phase))phaseRows.append(value);
      if(phaseRows.size()>=4) {
        const auto baseline=phaseRows.at(std::min<int>(3,static_cast<int>(phaseRows.size())-1)).toObject(),last=phaseRows.at(static_cast<int>(phaseRows.size())-1).toObject();
        report["handleGrowth"]=last["handles"].toInt()-baseline["handles"].toInt();
        report["privateGrowthBytes"]=last["privateBytes"].toDouble()-baseline["privateBytes"].toDouble();
        report["resourcePlateauPassed"]=report["handleGrowth"].toInt()<=32 && report["privateGrowthBytes"].toDouble()<=baseline["privateBytes"].toDouble()*.25+64.0*1024*1024;
      }
      if(phase!="exit"){item.reset();session.clear();window.hide();window.releaseResources();QCoreApplication::processEvents();}
    }
    else if(mode=="highlight") {
      ModelSession session;openSample(session,path,externals);const auto d=session.documentSnapshot();
      require(d&&d->animated()&&!d->meshes.empty(),"Animated mesh required for fallback-highlight control");
      auto state=stateFor(*d);int checked=0;double worst=0;
      const auto &clip=d->animationClips.at(d->animationClips.size()>1?1:0);
      const int ci=d->animationClips.size()>1?1:0;
      const auto geometry=buildModelGpuGeometry(*d,state,500,{});
      for(double factor:{.0,.371,.9}) {
        const double time=clip.start+(clip.end-clip.start)*factor;
        const auto worlds=d->worldAt(ci,time);
        const auto pose=buildAnimationFrame(*d,ci,time,501,d->actorSkinDomain(),&worlds);
        require(pose->worlds==worlds,"CPU pose discards the animated worlds required by skeleton highlighting");
        int perTime=0;
        for(qsizetype ni=0;ni<d->nodes.size() && perTime<8;++ni) {
          const auto record=d->nodes[ni].toMap();if(!record.value("bone").toBool())continue;
          state.highlightedNode=record.value("id").toString();const auto highlight=resolveGpuHighlight(*d,state.highlightedNode);
          if(highlight.node<0||highlight.edges.empty())continue;
          const auto cpu=buildModelFrames(*d,state,502,{},pose.get()).model;
          const auto gpu=buildModelGpuPose(*d,*geometry,{},worlds,503,ci,time,nullptr,&highlight).model;
          require(cpu->selectionVertices==gpu->selectionVertices&&cpu->selectionVertices>0,"CPU/GPU highlight edge counts differ");
          for(std::size_t i=0;i<cpu->selectionVertices;++i){
            const auto &a=cpu->guides[cpu->guides.size()-cpu->selectionVertices+i];
            const auto &b=gpu->guides[gpu->guides.size()-gpu->selectionVertices+i];
            worst=std::max(worst,double((QVector3D(a.x,a.y,a.z)-QVector3D(b.x,b.y,b.z)).length()));
          }
          ++checked;++perTime;
        }
      }
      require(checked>=3,"No meaningful animated highlight coverage");require(worst<=1e-4,"CPU fallback highlights rest skeleton instead of the displayed animated skeleton");
      report["highlightCases"]=checked;report["maxPositionError"]=worst;session.clear();
    }
    else if(mode=="morph") {
      // glTF morph-weight animation: the weights channel must actually drive
      // vertices (no "unsupported" warning), on the CPU pose and the GPU shader alike.
      auto d=readModelDocument(path,{});
      require(!d->warnings.join(';').contains("Unsupported animation path"),qPrintable("morph weights still flagged unsupported: "+d->warnings.join(';')));
      require(d->animationClips.size()>=1,"No clips");
      const auto &clip=d->animationClips.front();
      const auto worldsA=d->worldAt(0,clip.start);
      const auto a=buildAnimationFrame(*d,0,clip.start,400,false,&worldsA);
      const double mid=clip.start+(clip.end-clip.start)*.5;
      const auto worldsB=d->worldAt(0,mid);
      const auto b=buildAnimationFrame(*d,0,mid,401,false,&worldsB);
      double delta=0;for(std::size_t i=0;i<a->vertices.size()&&i<b->vertices.size();++i)delta=std::max(delta,double((QVector3D(a->vertices[i].x,a->vertices[i].y,a->vertices[i].z)-QVector3D(b->vertices[i].x,b->vertices[i].y,b->vertices[i].z)).length()));
      require(delta>0.1,"Morph animation shows no motion between keys");
      auto g=buildModelGpuGeometry(*d,stateFor(*d),100,{});
      QJsonArray samples;
      for(double factor:{0.0,.5,1.0}) {
        const double seconds=clip.start+(clip.end-clip.start)*factor;
        const auto worlds=d->worldAt(0,seconds);
        auto pose=buildModelGpuPose(*d,*g,{},worlds,200+samples.size(),0,seconds);
        const auto cpu=buildAnimationFrame(*d,0,seconds,300,false,&worlds);
        auto base=std::make_shared<Frame>();auto skin=std::make_shared<SkinData>(*g->frames.model->skin);skin->ranges.clear();
        const auto count=g->frames.model->vertices.size();
        skin->morphRanges.clear();
        for(std::size_t i=0;i<count;++i){base->vertices.push_back(g->frames.model->vertices[i]);skin->ranges.push_back(g->frames.model->skin->ranges[i]);
          skin->morphRanges.push_back(g->frames.model->skin->morphRanges.empty()?std::array<std::uint32_t,2>{0,0}:g->frames.model->skin->morphRanges[i]);}
        base->rgba={1,1,1,1};base->skin=skin;base->revision=pose.model->revision;
        auto packet=std::make_shared<Frame>(*pose.model);packet->geometry=base;packet->draws.clear();packet->explicitDraws=false;
        const auto pixels=read(packet,1);double gpuError=0;bool moved=false;
        for(std::size_t i=0;i<count;++i){
          const auto &v=cpu->vertices.at(i);moved=moved||std::abs(v.x)>1e-3||std::abs(v.z)>1e-3;
          gpuError=std::max(gpuError,double((QVector3D(pixels[i*4],pixels[i*4+1],pixels[i*4+2])-QVector3D(v.x,v.y,v.z)).length()));
          require(std::abs(pixels[i*4+3]-1)<1e-6,"Morph GPU sample not rendered");
        }
        require(moved||factor==0.0,"Morph weights did not animate the CPU pose");
        require(gpuError<=1e-4,"Morph GPU deformation differs from CPU oracle");
        samples.append(QJsonObject{{"seconds",seconds},{"cpuMoved",moved},{"gpuError",gpuError}});
      }
      report["samples"]=samples;report["cpuMotion"]=delta;
    }
    else if(mode=="dump") {
      // Independent-oracle export: CPU-posed position per source point at
      // clip-relative seconds from FORMAVIEW_DUMP_TIMES (comma list). A
      // third-party implementation (Blender) is compared offline.
      std::shared_ptr<const ModelDocument> d;ModelSession holder;
      if(externals.isEmpty())d=readModelDocument(path,{});else{openSample(holder,path,externals);d=holder.documentSnapshot();}
      const int ci=args.size()>5?args[5].toInt():0;
      require(ci>=0 && static_cast<std::size_t>(ci)<d->animationClips.size(),"Clip index out of range");
      const auto &clip=d->animationClips.at(static_cast<std::size_t>(ci));
      QStringList timeText=qEnvironmentVariable("FORMAVIEW_DUMP_TIMES").split(',',Qt::SkipEmptyParts);
      std::vector<double> times;for(const auto &t:timeText)times.push_back(t.toDouble());
      if(times.empty())for(double f:{0.0,.25,.5,.75,1.0})times.push_back((clip.end-clip.start)*f);
      report["clip"]=ci;report["clipName"]=clip.name;report["clipStart"]=clip.start;report["clipEnd"]=clip.end;report["clipFps"]=clip.fps;
      QJsonArray meshInfo;for(const auto &m:d->meshes)meshInfo.append(QJsonObject{{"name",m.name},{"points",static_cast<int>(m.points.size())},{"morphAnimated",m.morphAnimated}});
      report["meshes"]=meshInfo;
      {QJsonArray rest;for(const auto &m:d->meshes){QJsonArray xyz;const auto &src=m.animationPositions.empty()?m.raw:m.animationPositions;for(const auto &v:src){xyz.append(v.x);xyz.append(v.y);xyz.append(v.z);}rest.append(xyz);}report["restLocal"]=rest;}
      {QJsonArray names;for(const auto &n:d->nodes)names.append(n.toMap().value("name").toString());report["nodeNames"]=names;}
      {QJsonArray all;for(std::size_t mi=0;mi<d->meshes.size()&&mi<d->skinSlots.size();++mi){QJsonArray rows;const auto &pointSlots=d->skinSlots[mi];const auto &clusters=d->skinClusters[mi];const std::size_t step=std::max<std::size_t>(1,(qEnvironmentVariable("FORMAVIEW_DUMP_WEIGHTS_ALL")=="1"?1:pointSlots.size()/400));
        for(std::size_t pi=0;pi<pointSlots.size();pi+=step){QJsonArray entries;for(const auto &[clusterIndex,weightValue]:pointSlots[pi])entries.append(QJsonArray{d->nodes.at(static_cast<int>(clusters.at(clusterIndex).node)).toMap().value("name").toString(),weightValue});rows.append(QJsonArray{static_cast<int>(pi),entries});}
        all.append(QJsonObject{{"clusters",static_cast<int>(clusters.size())},{"rows",rows}});}
        report["skinWeights"]=all;}
      QJsonArray samples;std::uint64_t revision=700;
      for(const double t:times) {
        const auto worlds=d->worldAt(static_cast<std::size_t>(ci),clip.start+t);
        const auto cpu=buildAnimationFrame(*d,static_cast<std::size_t>(ci),clip.start+t,revision++,false,&worlds);
        QJsonArray perMesh;std::size_t offset=0;
        for(std::size_t mi=0;mi<d->meshes.size();++mi) {
          const auto &m=d->meshes[mi];std::vector<char> seen(m.points.size(),0);QJsonArray idx,xyz;
          for(std::size_t k=0;k<m.triangles.size();++k) {
            const auto point=m.corners.at(m.triangles[k]);if(seen.at(point))continue;seen[point]=1;
            const auto &v=cpu->vertices.at(offset+k);idx.append(static_cast<int>(point));xyz.append(v.x);xyz.append(v.y);xyz.append(v.z);
          }
          offset+=m.triangles.size();perMesh.append(QJsonObject{{"idx",idx},{"xyz",xyz}});
        }
        require(offset==cpu->vertices.size(),"Dump slot count differs from posed vertex count");
        QJsonArray worldFlat;for(const auto &w:worlds)for(const double v:w)worldFlat.append(v);
        samples.append(QJsonObject{{"t",t},{"meshes",perMesh},{"worlds",worldFlat},{"worldCount",static_cast<int>(worlds.size())}});
      }
      report["samples"]=samples;report["passed"]=true;
    }
    else if(mode=="numeric" || mode=="numeric-all") {
      std::shared_ptr<const ModelDocument> d;ModelSession holder;
      if(externals.isEmpty() && qEnvironmentVariable("FORMAVIEW_GPU_REFERENCE").isEmpty())d=readModelDocument(path,{});
      else{openSample(holder,path,externals);d=holder.documentSnapshot();report["externalStatus"]=d->externalStatus;report["externalNotice"]=d->externalNotice;}
      const auto digest=modelSourceDigest(*d);auto state=stateFor(*d);
      auto g=buildModelGpuGeometry(*d,state,100,{});QJsonArray samples;const auto cornerSlots=poseSlots(*d,state,g->frames.model->vertices.size());
      report["actorDomain"]=d->actorSkinDomain();report["pairPreviewDomain"]=d->pairPreviewDomain();
      const auto total=d->animationClips.size();
      std::vector<std::size_t> clips={0,total/2,total-1};
      if(mode=="numeric-all"){clips.clear();for(std::size_t i=0;i<total;++i)clips.push_back(i);}std::sort(clips.begin(),clips.end());clips.erase(std::unique(clips.begin(),clips.end()),clips.end());
      for(const auto ci:clips) {
        const auto &clip=d->animationClips.at(ci);
        for(double factor:{0.0,.371,1.0})samples.append(numerical(*d,*g,cornerSlots,static_cast<int>(ci),clip.start+(clip.end-clip.start)*factor,200+samples.size()));
      }
      report["samples"]=samples;report["vertices"]=static_cast<qint64>(g->frames.model->vertices.size());report["clusters"]=static_cast<int>(g->clusters.size());
      report["influenceBytes"]=static_cast<qint64>(g->frames.model->skin->influences.size()*4);
      require(modelSourceDigest(*d)==digest,"Source data mutated");report["sourceDigestStable"]=true;
      // P2 review: per-bone root-lock expectation (independent of the
      // evaluator): with a single root, locking the root local to the base
      // moves every bone world by exactly one common rigid correction, the
      // locked root translation equals the first-frame root translation, and
      // toggling the lock off restores the unlocked worlds at the same time.
      {
        ModelSession lockSession;lockSession.enableGpuSkinning(true);
        const auto reference=qEnvironmentVariable("FORMAVIEW_GPU_REFERENCE");if(!reference.isEmpty())lockSession.setAnimationReference(reference);
        lockSession.open(path);
        settle([&]{return !lockSession.busy() && lockSession.inspectable() && lockSession.frame();},90000);
        if(qEnvironmentVariable("FORMAVIEW_GPU_PAIR_MESH")=="1"){lockSession.setPairMeshVisible(true);require(lockSession.pairMeshVisible(),"Paired mesh unavailable for the lock check");}
        for(const auto &external:externals) {
          lockSession.attachExternalAnimation(external);settle([&]{return !lockSession.externalBusy();},120000);
          const auto info=lockSession.externalInfo();require(info.value("driven").toBool() && info.value("status").toString()!="refused","External drive refused for the lock check");
        }
        const auto totalClips=lockSession.clips().size();
        // The invariant is about skeleton chains: documents with weighted skin clusters (or a PSK actor).
        bool hasSkeleton=lockSession.documentSnapshot() && lockSession.documentSnapshot()->actorSkinDomain();
        if(const auto snapshot=lockSession.documentSnapshot())for(const auto &clusters:snapshot->skinClusters)if(!clusters.empty())hasSkeleton=true;
        if(!lockSession.animated() || totalClips==0) {
          // Root lock is an animation-time feature; static documents have
          // nothing to verify here.
          report["rootLockInvariant"]="static document skipped";
        } else if(!hasSkeleton) {
          // Rigid node-animated samples have no skeleton root to lock.
          report["rootLockInvariant"]="no skeleton: not applicable";
        } else {
        settle([&]{const auto f=lockSession.frame();return f && f->worlds.size()>1;},30000); // the animated frame lands after the first landing frame
        const auto roots=lockSession.rootCandidates();
        require(!roots.isEmpty(),"Root candidates missing for the lock check");
        const int root=roots.front().toMap().value("index").toInt();
        if(roots.size()>1)lockSession.setRootIndex(root);
        const auto lockDocument=lockSession.documentSnapshot();
        const auto &lockParents=lockDocument->nodeAnimDomain()?lockDocument->animNodeParents:lockDocument->boneParents;
        const auto inSubtree=[&](std::size_t bone) {
          auto at=bone;
          for(std::size_t guard=0;guard<=lockParents.size();++guard) {
            if(static_cast<std::int32_t>(at)==root)return true;
            const auto parent=lockParents.at(at);
            if(parent<0 || static_cast<std::size_t>(parent)>=lockParents.size())return false;
            at=static_cast<std::size_t>(parent);
          }
          return false;
        };
        lockSession.setClipIndex(static_cast<int>(std::min<std::size_t>(1,totalClips-1)));
        const auto toQMatrix=[](const formaview::Matrix4 &v){QMatrix4x4 m;for(int r=0;r<4;++r)for(int c=0;c<4;++c)m(r,c)=float(v[static_cast<std::size_t>(r*4+c)]);return m;};
        std::vector<double> pinRootTranslation;QJsonArray lockRows;
        for(const double factor:{0.0,.371,.9}) {
          const auto info=lockSession.clipInfo();
          lockSession.seek(info.value("start").toDouble()+(info.value("end").toDouble()-info.value("start").toDouble())*factor);
          const auto unlocked=lockSession.frame()->worlds;
          require(unlocked.size()>1 && static_cast<std::size_t>(root)<unlocked.size(),"Lock check world count");
          lockSession.setRootLock(true);
          require(lockSession.rootLockInfo().value("status").toString()=="locked","Root lock status during numeric check");
          const auto locked=lockSession.frame()->worlds;
          lockSession.setRootLock(false);
          const auto restored=lockSession.frame()->worlds;
          require(locked.size()==unlocked.size() && restored.size()==unlocked.size(),"Lock check world sizes");
          const auto correction=toQMatrix(locked.at(static_cast<std::size_t>(root)))*toQMatrix(unlocked.at(static_cast<std::size_t>(root))).inverted();
          double commonError=0,restoreError=0;std::size_t subtree=0;
          for(std::size_t i=0;i<locked.size();++i) {
            if(!inSubtree(i))continue; // Other roots' subtrees stay unlocked.
            ++subtree;
            const auto expected=correction*toQMatrix(unlocked[i]);
            const auto actual=toQMatrix(locked[i]);
            const auto back=toQMatrix(restored[i]);
            for(int r=0;r<4;++r)for(int c=0;c<4;++c) {
              commonError=std::max(commonError,double(std::abs(expected(r,c)-actual(r,c))));
              restoreError=std::max(restoreError,double(std::abs(back(r,c)-toQMatrix(unlocked[i])(r,c))));
            }
          }
          require(subtree>1,"Lock check subtree too small");
          // The locked root translation must sit at the first-frame root
          // translation (the base), sampled once at factor 0.
          const auto &rootRow=locked.at(static_cast<std::size_t>(root));
          pinRootTranslation.push_back(rootRow[3]);pinRootTranslation.push_back(rootRow[7]);pinRootTranslation.push_back(rootRow[11]);
          lockRows.append(QJsonObject{{"factor",factor},{"commonTransformError",commonError},{"restoreError",restoreError}});
          if(factor==0.0)report["lockCheckRoot"]=root;
          require(commonError<=1e-4,"Root lock is not one common rigid transform on this domain");
          require(restoreError<=1e-6,"Root lock off did not restore the unlocked worlds");
        }
        const double dx=pinRootTranslation[3]-pinRootTranslation[0],dy=pinRootTranslation[4]-pinRootTranslation[1],dz=pinRootTranslation[5]-pinRootTranslation[2];
        const double pinDrift=std::sqrt(dx*dx+dy*dy+dz*dz);
        const double ex=pinRootTranslation[6]-pinRootTranslation[0],ey=pinRootTranslation[7]-pinRootTranslation[1],ez=pinRootTranslation[8]-pinRootTranslation[2];
        const double pinDriftEnd=std::sqrt(ex*ex+ey*ey+ez*ez);
        report["rootLockInvariant"]=lockRows;report["rootPinDriftMid"]=pinDrift;report["rootPinDriftEnd"]=pinDriftEnd;
        require(pinDrift<=1e-4 && pinDriftEnd<=1e-4,"Locked root translation drifted from the first-frame base");
        }
        lockSession.clear();
      }
    } else if(mode=="interactions") {
      ModelSession session;QQuickWindow window;window.resize(1280,720);
      auto item=std::make_unique<ModelView>(window.contentItem());item->setSize({1280,720});item->setSession(&session);window.show();openSample(session,path,externals);
      settle([&]{return session.inspectable() && session.frame() && session.frame()->geometry;},90000);
      const auto geometry=session.frame()->geometry;const auto digest=session.sourceDataDigest();const auto info=session.clipInfo();
      session.seek(info.value("start").toDouble()+(info.value("end").toDouble()-info.value("start").toDouble())*.371);
      const auto sampled=session.frame();require(sampled->geometry==geometry,"Seek rebuilt static GPU geometry");
      const auto roots=session.rootCandidates();if(roots.size()>1)session.setRootIndex(roots.front().toMap().value("index").toInt());
      session.setRootLock(true);require(session.rootLockInfo().value("status")=="locked","GPU root lock status");
      session.setRootLock(false);require(session.frame()->palette==sampled->palette,"GPU root lock restore");
      session.setSpeed(1e100);require(session.speed()==1e100,"Playback speed capped");session.togglePlay();
      QElapsedTimer ticks;ticks.start();settle([&]{return ticks.elapsed()>100;},2000);session.setSpeed(.75);require(session.playing() && session.speed()==.75,"Speed edit stopped playback");session.togglePlay();
      const auto beforeCancel=session.frame()->geometry;
      session.setMode(1);session.cancel();
      QElapsedTimer cancelled;cancelled.start();settle([&]{return cancelled.elapsed()>200;},2000);
      require(session.mode()==0 && session.frame() && session.frame()->geometry==beforeCancel && session.frameError().isEmpty(),"Cancelled GPU view preparation published stale geometry or disabled GPU");
      report["gpuViewPreparationCancel"]=true;
      session.setMode(1);settle([&]{return session.inspectable() && session.frame() && session.frame()->uv;});
      const bool hasUv=!session.uvSets().isEmpty();
      require(!hasUv || !session.frame()->vertices.empty(),"GPU UV source view missing");
      session.setMode(2);settle([&]{return session.inspectable() && session.frame() && session.frame()->geometry && session.frame()->geometry->heat;});
      item->setWireframe(true);item->setGridVisible(true);item->setAxesVisible(true);
      session.setMode(0);settle([&]{return session.inspectable() && session.frame() && session.frame()->geometry && !session.frame()->geometry->heat;});
      // Independent exact triangle ray oracle, then the session's asynchronous
      // on-demand CPU snapshot picker; both use the same frozen rendered pose.
      const auto allBounds=session.focusBounds(false), selectedBounds=session.focusBounds(true);
      require(selectedBounds.second<=allBounds.second+1e-4F,"GPU selected focus includes unrelated mesh bounds");
      report["selectedFocus"]=true;
      const auto snapshot=session.frame();std::shared_ptr<const ModelDocument> source=externals.isEmpty()?readModelDocument(path,{}):session.documentSnapshot();auto state=stateFor(*source);
      const auto cpu=buildAnimationFrame(*source,static_cast<std::size_t>(snapshot->clip),snapshot->seconds,900,source->actorSkinDomain(),&snapshot->worlds);
      if(QFileInfo(path).fileName()=="multi-skin.gltf") {
        QVector3D low(1e30F,1e30F,1e30F),high(-1e30F,-1e30F,-1e30F);
        for(std::size_t i=0;i<source->meshes.at(0).triangles.size();++i){const auto &v=cpu->vertices.at(i);const QVector3D p(v.x,v.y,v.z);for(int axis=0;axis<3;++axis){low[axis]=std::min(low[axis],p[axis]);high[axis]=std::max(high[axis],p[axis]);}}
        require(((low+high)*.5F-selectedBounds.first).length()<1e-4F && std::abs((high-low).length()*.5F-selectedBounds.second)<1e-4F,"Selected focus disagrees with independent one-joint mesh oracle");
      }
      const auto &a=cpu->vertices.at(0),&b=cpu->vertices.at(1),&c=cpu->vertices.at(2);
      const QVector3D center((a.x+b.x+c.x)/3,(a.y+b.y+c.y)/3,(a.z+b.z+c.z)/3);
      QMatrix4x4 view;view.translate(-center.x(),-center.y(),0);view.scale(1,1,.001F);
      const auto expected=pickModelDocument(*source,state,cpu.get(),view,{0,0},false);
      require(expected.hitMesh>=0,"GPU picker independent ray has no hit");
      session.pick(view,{0,0},false,snapshot);settle([&]{return !session.picked().isEmpty();},30000);
      require(session.picked().value("point").toUInt()==source->meshes.at(static_cast<std::size_t>(expected.hitMesh)).corners.at(expected.hitCorner),"GPU snapshot pick mismatch");
      require(session.sourceDataDigest()==digest,"GPU interaction changed source");
      report["uv"]=hasUv; if(!hasUv)report["uvExcludedReason"]="Source has no UV set";report["weights"]=true;report["rootRestore"]=true;report["unlimitedSpeedEdit"]=true;report["snapshotPick"]=true;report["sourceDigestStable"]=true;
      item.reset();session.clear();window.hide();window.releaseResources();QCoreApplication::processEvents();
    } else {
      ModelSession session;QQuickWindow window;window.setTitle("FormaView Dasheng GPU performance");window.resize(1920,1080);
      // Diagnostic condition only: foreground viewing, to separate OS background throttling from app cost.
      const bool foreground=qEnvironmentVariable("FORMAVIEW_PERF_FOREGROUND")=="1";report["foregroundCondition"]=foreground;
      const bool background=qEnvironmentVariable("FORMAVIEW_PERF_BACKGROUND")=="1";
      require(!(foreground && background),"Conflicting foreground/background conditions");
      report["backgroundCondition"]=background;
      if(background)report["backgroundKind"]="Visible focus-disabled window; occlusion/minimization not controlled";
      if(background)window.setFlag(Qt::WindowDoesNotAcceptFocus);
      if(foreground)window.setFlag(Qt::WindowStaysOnTopHint);
      auto item=std::make_unique<ModelView>(window.contentItem());item->setSize({1920,1080});item->setSession(&session);
      struct Metrics {std::mutex mutex;std::vector<double> intervals,gpu,prepare,poseTimes,swapTimes;std::vector<std::uint64_t> uploads;std::uint64_t latest{},presented{},swapped{},ownedMax{},processMax{};std::chrono::steady_clock::time_point previous{},swapTime{},measureStart{};bool measuring{};};auto m=std::make_shared<Metrics>();
      item->observe=[m](Observation o){if(o.released)return;std::lock_guard lock(m->mutex);m->latest=o.frame_revision;m->ownedMax=std::max(m->ownedMax,o.owned_gpu_bytes);m->processMax=std::max(m->processMax,o.process_gpu_bytes);if(m->measuring){if(o.gpu_seconds>0)m->gpu.push_back(o.gpu_seconds*1000);m->prepare.push_back(o.prepare_ms);m->uploads.push_back(o.uploaded_bytes);}};
      QObject::connect(&window,&QQuickWindow::frameSwapped,&window,[m]{std::lock_guard lock(m->mutex);const auto now=std::chrono::steady_clock::now();m->swapped=m->latest;m->swapTime=now;if(m->measuring)m->swapTimes.push_back(std::chrono::duration<double,std::milli>(now-m->measureStart).count());if(!m->measuring||m->latest==m->presented)return;if(m->presented){m->intervals.push_back(std::chrono::duration<double,std::milli>(now-m->previous).count());m->poseTimes.push_back(std::chrono::duration<double,std::milli>(now-m->measureStart).count());}m->previous=now;m->presented=m->latest;},Qt::DirectConnection);
      window.show();openSample(session,path,externals);settle([&]{return !session.busy() && session.inspectable() && session.frame() && (mode=="cpu"?!session.frame()->vertices.empty():bool(session.frame()->geometry));},90000);
      const int clip=args.size()>5?args[5].toInt():0;session.setClipIndex(clip);
      const auto roots=session.rootCandidates();if(roots.size()>1)session.setRootIndex(roots.front().toMap().value("index").toInt());
      const auto previous=session.frame()->revision;session.setRootLock(true);
      settle([&]{return session.frame() && session.frame()->revision>previous && session.frame()->clip==clip;},30000);
      require(session.rootLockInfo().value("status")=="locked","Performance root lock did not apply");
      item->setFixedColorBufferWidth(1920);item->setFixedColorBufferHeight(1080);
      report["clip"]=clip;report["clipName"]=session.clipInfo().value("name").toString();report["rootLock"]=true;report["viewportPixels"]="1920x1080";
      item->fit();auto camera=item->cameraState();camera["automatic"]=false;item->restoreCamera(camera);
      report["gpuGeometry"]=bool(session.frame()->geometry);
      if(mode=="gpu") {
        if(foreground){window.raise();window.requestActivate();}
        const auto windowInfo=[&]{return QJsonObject{
          {"active",window.isActive()},{"exposed",window.isExposed()},
          {"dpr",window.devicePixelRatio()},
          {"bufferWidth",item->effectiveColorBufferSize().width()},
          {"bufferHeight",item->effectiveColorBufferSize().height()}};};
        const auto condition=[&](const QJsonObject &row){return row["exposed"].toBool() &&
          (!foreground || row["active"].toBool()) && (!background || !row["active"].toBool());};
        settle([&]{const auto row=windowInfo();std::lock_guard lock(m->mutex);
          return condition(row) && row["bufferWidth"].toInt()==1920 && row["bufferHeight"].toInt()==1080 &&
                 m->swapped>=session.frame()->revision;},10000);
        QJsonArray commandRows;
        const auto command=[&](const char *label,const std::function<void()> &action) {
          const auto beginWindow=windowInfo();
          const auto before=session.frame()->revision;
          const auto start=std::chrono::steady_clock::now();action();
          try {settle([&]{return session.frame() && session.frame()->revision>before;},10000);}
          catch(const std::exception &e){throw std::runtime_error(std::string(label)+" revision: "+e.what());}
          const auto target=session.frame()->revision;
          try {settle([&]{std::lock_guard lock(m->mutex);return m->swapped>=target && m->swapTime>=start;},10000);}
          catch(const std::exception &e){throw std::runtime_error(std::string(label)+" swap: "+e.what());}
          double latency;{std::lock_guard lock(m->mutex);latency=std::chrono::duration<double,std::milli>(m->swapTime-start).count();}
          const auto endWindow=windowInfo();
          commandRows.append(QJsonObject{{"action",label},{"ms",latency},
            {"beforeRevision",static_cast<qint64>(before)},{"targetRevision",static_cast<qint64>(target)},
            {"beginWindow",beginWindow},{"endWindow",endWindow}});
          report["commandRows"]=commandRows;
          require(condition(beginWindow) && condition(endWindow),"Command window condition not met");
          return latency;
        };
        std::vector<double> seeks,rootLatencies,clipLatencies;
        for(int i=0;i<100;++i) {
          seeks.push_back(command("seek",[&]{const auto info=session.clipInfo();session.seek(info.value("start").toDouble()+(info.value("end").toDouble()-info.value("start").toDouble())*((i%7)/7.0));}));
          rootLatencies.push_back(command("rootLock",[&]{session.setRootLock(i%2==0?false:true);}));
          if(session.clips().size()>1)
            clipLatencies.push_back(command("clip",[&]{session.setClipIndex(i%2==0?(clip==0?1:0):clip);}));
        }
        report["seekP95Ms"]=percentile(seeks,.95);report["rootLockP95Ms"]=percentile(rootLatencies,.95);report["residentClipP95Ms"]=percentile(clipLatencies,.95);
        const auto raw=[](const std::vector<double> &values){QJsonArray rows;for(double value:values)rows.append(value);return rows;};
        report["seekSamplesMs"]=raw(seeks);report["rootLockSamplesMs"]=raw(rootLatencies);report["residentClipSamplesMs"]=raw(clipLatencies);
        report["clipCount"]=session.clips().size();
        if(clipLatencies.empty())report["residentClipExcludedReason"]="Source has only one clip; no action-switch latency measured";
        if(clipLatencies.empty())report["residentClipP95Ms"]=QJsonValue(QJsonValue::Null);
        session.setClipIndex(clip);session.setRootLock(true);
      }
      session.setLoop(true);session.togglePlay();QElapsedTimer warm;warm.start();settle([&]{return warm.elapsed()>=5000;},10000);
      {std::lock_guard lock(m->mutex);m->measuring=true;m->presented=0;m->measureStart=std::chrono::steady_clock::now();}
      // Diagnostics only: presentation environment and GUI-thread lateness; no effect on the frame-time gate.
      QJsonArray environment;QElapsedTimer measure;measure.start();QTimer sampler;qint64 expected=0;
      QObject::connect(&sampler,&QTimer::timeout,&window,[&]{const auto now=measure.elapsed();auto *screen=window.screen();environment.append(QJsonObject{{"ms",now},{"lateMs",now-expected},{"screen",screen?screen->name():QString()},{"refreshHz",screen?screen->refreshRate():0.0},{"exposed",window.isExposed()},{"active",window.isActive()},{"visibility",static_cast<int>(window.visibility())}});expected=now+250;});
      sampler.start(250);if(foreground){window.raise();window.requestActivate();}
      settle([&]{return measure.elapsed()>=static_cast<qint64>(duration*1000);},static_cast<int>(duration*1000+10000));sampler.stop();report["environmentSamples"]=environment;
      session.togglePlay();{std::lock_guard lock(m->mutex);m->measuring=false;report["ownedGpuPeakBytes"]=static_cast<qint64>(m->ownedMax);report["processGpuPeakBytes"]=static_cast<qint64>(m->processMax);report["uniquePoseIntervals"]=static_cast<int>(m->intervals.size());report["p50Ms"]=percentile(m->intervals,.5);report["p95Ms"]=percentile(m->intervals,.95);report["p99Ms"]=percentile(m->intervals,.99);report["prepareP95Ms"]=percentile(m->prepare,.95);report["gpuP95Ms"]=percentile(m->gpu,.95);report["gpuTimerSamples"]=static_cast<int>(m->gpu.size());std::uint64_t maximum=0;for(auto value:m->uploads)maximum=std::max(maximum,value);report["maxUploadedBytesPerRender"]=static_cast<qint64>(maximum);QJsonArray intervals;for(double v:m->intervals)intervals.append(v);report["intervalsMs"]=intervals;QJsonArray poseTimes,swapTimes;for(double v:m->poseTimes)poseTimes.append(v);for(double v:m->swapTimes)swapTimes.append(v);report["poseTimesMs"]=poseTimes;report["swapTimesMs"]=swapTimes;}
      report["seconds"]=duration;
      const auto intervals=report.value("intervalsMs").toArray();double totalMs=0;for(const auto &value:intervals)totalMs+=value.toDouble();
      report["meanPoseFps"]=totalMs>0?intervals.size()*1000.0/totalMs:0;
      recordFrameTimeGate(report,report.value("p95Ms").toDouble(),report.value("p99Ms").toDouble(),report.value("meanPoseFps").toDouble()>=60);
      item.reset();session.clear();window.hide();window.releaseResources();QCoreApplication::processEvents();
    }
    report["passed"]=true;report["passedMeaning"]="Execution and applicable correctness assertions; performance gate is frameTimeGatePassed";
  }catch(const std::exception &e){report["passed"]=false;report["error"]=QString::fromUtf8(e.what());std::cerr<<e.what()<<std::endl;}
  observeImeModules();
  report["sogouPyObserved"]=sogouPyObserved;report["sogouTsfObserved"]=sogouTsfObserved;
  report["imeModuleObservationBoundary"]="Module presence sampled after app construction, each numerical readback, and before report output";
  QFile file(args[2]);if(!file.open(QIODevice::WriteOnly))return 3;file.write(QJsonDocument(report).toJson());return report["passed"].toBool()?0:1;
}
