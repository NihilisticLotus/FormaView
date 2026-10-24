#pragma once
// Test-only, explicitly enabled GUI diagnostics. Never linked into the product.
#include <QEvent>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QScopeGuard>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <cstdint>

namespace model_forensics {
using Clock=std::chrono::steady_clock;
struct Dispatch {
  std::uint64_t sequence{},window{},wparam{},lparam{};
  double start{},elapsed{};
  unsigned message{},sendFlags{};
  char receiver[64]{};
};
struct Snapshot {
  bool available{};
  unsigned nativeDepth{},qtDepth{};
  std::array<Dispatch,32> native{},qt{};
  std::uint64_t publishedRevision{},publishedHash{},poseContentVersion{},serial{},nativeCount{},qtCount{};
  double publishedAt{-1},publishedSeconds{};
  int publishedClip{-1};
};
struct Publication {
  double at{},seconds{};
  std::uint64_t revision{},hash{},contentVersion{};
  int clip{-1};
};
// Query only a duplicated candidate handle AFTER the GUI resumes. No name
// queries for pipes/files: only synchronization object names are requested.
inline QJsonObject waitObject(HANDLE candidate) {
  QJsonObject out{{"candidateHandle",QString::number(reinterpret_cast<std::uintptr_t>(candidate),16)}};
  HANDLE duplicate=nullptr;
  if(!DuplicateHandle(GetCurrentProcess(),candidate,GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS)) {
    out["duplicateError"]=static_cast<qint64>(GetLastError());return out;
  }
  const auto close=qScopeGuard([&]{CloseHandle(duplicate);});
  using Query=LONG(NTAPI*)(HANDLE,ULONG,PVOID,ULONG,PULONG);
  const auto query=reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryObject"));
  if(!query){out["queryUnavailable"]=true;return out;}
  struct Text {USHORT length,maximum;PWSTR buffer;};
  const auto info=[&](ULONG kind,const char *key){
    alignas(void*) std::array<unsigned char,8192> buffer{};ULONG needed{};
    const LONG status=query(duplicate,kind,buffer.data(),static_cast<ULONG>(buffer.size()),&needed);
    out[QString::fromLatin1(key)+"Status"]=static_cast<qint64>(status);
    if(status<0)return QString();
    const auto &str=*reinterpret_cast<const Text*>(buffer.data());
    const auto lo=reinterpret_cast<std::uintptr_t>(buffer.data()),at=reinterpret_cast<std::uintptr_t>(str.buffer);
    if(!str.length){out[key]=QString();return QString();}
    if(str.length%sizeof(wchar_t)||at<lo||at>lo+buffer.size()||str.length>lo+buffer.size()-at){out["invalidQueryBuffer"]=true;return QString();}
    const auto value=QString::fromWCharArray(str.buffer,str.length/sizeof(wchar_t));out[key]=value;return value;
  };
  const auto type=info(2,"type");
  if(type=="Event"||type=="Mutant"||type=="Semaphore")info(1,"name");
  out["boundary"]="Register-derived candidate; duplicate/query occurs after resume and handle reuse is possible. Events have no mutex-style owning thread.";
  return out;
}
class Probe;
inline std::atomic<Probe*> active{nullptr};
inline QJsonObject dispatchJson(const Dispatch &v) {
  return {{"sequence",static_cast<qint64>(v.sequence)},{"startMs",v.start},{"durationMs",v.elapsed},
    {"message",static_cast<int>(v.message)},{"hwnd",QString::number(v.window,16)},
    {"wParam",QString::number(v.wparam,16)},{"lParam",QString::number(v.lparam,16)},
    {"inSendMessageFlags",static_cast<int>(v.sendFlags)},{"receiverClass",QString::fromLatin1(v.receiver)}};
}
inline QJsonObject snapshotJson(const Snapshot &v) {
  QJsonArray native,qt;
  for(unsigned i=0;i<std::min<unsigned>(v.nativeDepth,32);++i)native.append(dispatchJson(v.native[i]));
  for(unsigned i=0;i<std::min<unsigned>(v.qtDepth,32);++i)qt.append(dispatchJson(v.qt[i]));
  return {{"available",v.available},{"nativeDepth",static_cast<int>(v.nativeDepth)},
    {"qtDepth",static_cast<int>(v.qtDepth)},{"native",native},{"qt",qt},
    {"publishedRevision",static_cast<qint64>(v.publishedRevision)},
    {"publishedHash",QString::number(v.publishedHash,16)},{"publishedAtMs",v.publishedAt},
    {"publishedClip",v.publishedClip},{"publishedSeconds",v.publishedSeconds},
    {"poseContentVersion",static_cast<qint64>(v.poseContentVersion)},{"serial",static_cast<qint64>(v.serial)},
    {"nativeCount",static_cast<qint64>(v.nativeCount)},{"qtCount",static_cast<qint64>(v.qtCount)}};
}
class Probe {
public:
  static constexpr UINT calibrationMessage=WM_APP+0x241;
  explicit Probe(Clock::time_point origin,std::size_t capacity):origin_(origin),gui_(GetCurrentThreadId()) {
    publications_.reserve(std::min<std::size_t>(capacity,500000));
    nativeSlow_.reserve(4096);qtSlow_.reserve(4096);
    Probe *expected=nullptr;
    if(!active.compare_exchange_strong(expected,this))throw std::runtime_error("Duplicate forensic probe");
    before_=SetWindowsHookExW(WH_CALLWNDPROC,beforeHook,nullptr,gui_);
    after_=SetWindowsHookExW(WH_CALLWNDPROCRET,afterHook,nullptr,gui_);
    if(!before_||!after_) {
      const auto error=GetLastError();close();throw std::runtime_error("GUI hook unavailable: "+std::to_string(error));
    }
    WNDCLASSW wc{};wc.lpfnWndProc=calibrationProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"FormaView.ForensicCalibration";
    atom_=RegisterClassW(&wc);
    if(atom_)control_=CreateWindowExW(0,wc.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,wc.hInstance,nullptr);
    if(!control_){close();throw std::runtime_error("Forensic control window unavailable");}
  }
  ~Probe(){close();}
  Probe(const Probe&)=delete;
  DWORD guiId() const {return gui_;}
  double now() const {return std::chrono::duration<double,std::milli>(Clock::now()-origin_).count();}
  // Called with GUI suspended: no allocation and never wait for its mutex.
  Snapshot snapshot() {
    Snapshot result;
    if(!mutex_.try_lock())return result;
    result=state_;result.available=true;result.serial=serial_;result.nativeCount=nativeCount_;result.qtCount=qtCount_;
    mutex_.unlock();return result;
  }
  std::uint64_t enterQt(QObject *receiver,QEvent *event) {
    Dispatch d;d.start=now();d.message=static_cast<unsigned>(event->type());
    d.window=reinterpret_cast<std::uintptr_t>(receiver);
    if(receiver)strncpy_s(d.receiver,receiver->metaObject()->className(),_TRUNCATE);
    std::lock_guard lock(mutex_);d.sequence=++serial_;++qtCount_;
    if(state_.qtDepth<32)state_.qt[state_.qtDepth]=d;else ++depthOverflow_;
    ++state_.qtDepth;return d.sequence;
  }
  void leaveQt(std::uint64_t sequence) {
    const double end=now();std::lock_guard lock(mutex_);
    if(!state_.qtDepth){++unmatched_;return;}
    --state_.qtDepth;if(state_.qtDepth>=32)return;
    auto d=state_.qt[state_.qtDepth];if(d.sequence!=sequence)++unmatched_;
    d.elapsed=end-d.start;recordSlow(qtSlow_,d);
  }
  template<class Frame> void publish(const Frame &frame) {
    if(!frame)return;
    // Hash actual pose payload, not just its revision. Full palette when present;
    // CPU worlds otherwise. A changed revision alone is not a changed pose.
    std::uint64_t hash=14695981039346656037ULL;
    const auto add=[&](const void *data,std::size_t size){
      const auto *bytes=static_cast<const unsigned char*>(data);
      for(std::size_t i=0;i<size;++i){hash^=bytes[i];hash*=1099511628211ULL;}
    };
    if(!frame->palette.empty())add(frame->palette.data(),frame->palette.size()*sizeof(float));
    else if(!frame->worlds.empty())add(frame->worlds.data(),frame->worlds.size()*sizeof(frame->worlds[0]));
    const auto sameBytes=[](const auto &a,const auto &b){return a.size()==b.size() && (a.empty() || std::memcmp(a.data(),b.data(),a.size()*sizeof(a[0]))==0);};
    if(!contentVersion_ || !sameBytes(frame->palette,previousPalette_) || !sameBytes(frame->worlds,previousWorlds_))++contentVersion_;
    previousPalette_=frame->palette;previousWorlds_=frame->worlds;
    const Publication p{now(),frame->seconds,frame->revision,hash,contentVersion_,frame->clip};
    std::lock_guard lock(mutex_);
    state_.publishedRevision=p.revision;state_.publishedHash=p.hash;state_.publishedAt=p.at;state_.poseContentVersion=p.contentVersion;
    state_.publishedClip=p.clip;state_.publishedSeconds=p.seconds;
    if(publications_.size()<publications_.capacity())publications_.push_back(p);else ++publicationOverflow_;
  }
  void calibrateMutex() {
    const auto name=L"Local\\FormaView.Forensics."+std::to_wstring(GetCurrentProcessId());
    HANDLE mutex=CreateMutexW(nullptr,FALSE,name.c_str());
    HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,nullptr),go=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    const auto handles=qScopeGuard([&]{if(mutex)CloseHandle(mutex);if(ready)CloseHandle(ready);if(go)CloseHandle(go);});
    if(!mutex||!ready||!go)throw std::runtime_error("Forensic calibration handle failure");
    std::atomic<DWORD> owner{0};
    std::thread worker([&]{owner=GetCurrentThreadId();const auto acquired=WaitForSingleObject(mutex,5000);
      SetEvent(ready);if(acquired==WAIT_OBJECT_0){WaitForSingleObject(go,5000);Sleep(300);ReleaseMutex(mutex);}});
    const auto join=qScopeGuard([&]{SetEvent(go);if(worker.joinable())worker.join();});
    if(WaitForSingleObject(ready,5000)!=WAIT_OBJECT_0)throw std::runtime_error("Forensic worker failed to acquire mutex");
    const double start=now();SetEvent(go);
    SendMessageW(control_,calibrationMessage,0,reinterpret_cast<LPARAM>(mutex));
    calibration_=QJsonObject{{"kind","knownNativeMutexWait"},{"startMs",start},{"endMs",now()},
      {"message",static_cast<int>(calibrationMessage)},{"hwnd",QString::number(reinterpret_cast<std::uintptr_t>(control_),16)},
      {"mutexName",QString::fromStdWString(name)},{"mutexHandle",QString::number(reinterpret_cast<std::uintptr_t>(mutex),16)},{"ownerTid",static_cast<qint64>(owner.load())},{"guiTid",static_cast<qint64>(gui_)}};
  }
  void resizeControl(HWND window) {
    RECT before{};if(!GetWindowRect(window,&before))throw std::runtime_error("Resize control window unavailable");
    std::atomic<unsigned> posted{0};const double start=now();
    std::thread keys([&]{
      Sleep(50);
      for(int i=0;i<50;++i){const WPARAM key=(i<25?VK_RIGHT:VK_LEFT);
        if(PostMessageW(window,WM_KEYDOWN,key,1))++posted;
        if(PostMessageW(window,WM_KEYUP,key,static_cast<LPARAM>(0xc0000001)))++posted;
        Sleep(10);
      }
      PostMessageW(window,WM_KEYDOWN,VK_ESCAPE,1);
      PostMessageW(window,WM_KEYUP,VK_ESCAPE,static_cast<LPARAM>(0xc0000001));
      PostMessageW(window,WM_CANCELMODE,0,0);
    });
    const auto join=qScopeGuard([&]{if(keys.joinable())keys.join();});
    // SC_SIZE uses the native modal loop, on this test-owned window only.
    SendMessageW(window,WM_SYSCOMMAND,SC_SIZE,0);const double end=now();
    if(keys.joinable())keys.join();
    RECT after{};GetWindowRect(window,&after);
    SetWindowPos(window,nullptr,before.left,before.top,before.right-before.left,before.bottom-before.top,SWP_NOACTIVATE|SWP_NOZORDER);
    resize_=QJsonObject{{"kind","controlledNativeSizeLoop"},{"startMs",start},{"endMs",end},
      {"message",WM_SYSCOMMAND},{"wParam",SC_SIZE},{"hwnd",QString::number(reinterpret_cast<std::uintptr_t>(window),16)},
      {"postedKeys",static_cast<int>(posted.load())},{"beforeWidth",static_cast<int>(before.right-before.left)},{"afterWidth",static_cast<int>(after.right-after.left)},
      {"boundary","Posted keys target only the owned test window; no SendInput or system setting changes. Not a natural/historical reproduction."}};
  }
  QJsonObject report() {
    std::lock_guard lock(mutex_);QJsonArray native,qt,publications;
    for(const auto &d:nativeSlow_)native.append(dispatchJson(d));
    for(const auto &d:qtSlow_)qt.append(dispatchJson(d));
    for(const auto &p:publications_)publications.append(QJsonArray{p.at,static_cast<qint64>(p.revision),p.clip,p.seconds,QString::number(p.hash,16),static_cast<qint64>(p.contentVersion)});
    return {{"enabled",true},{"guiTid",static_cast<qint64>(gui_)},{"nativeDispatch",native},{"qtDispatch",qt},
      {"nativeCount",static_cast<qint64>(nativeCount_)},{"qtCount",static_cast<qint64>(qtCount_)},
      {"depthOverflow",static_cast<qint64>(depthOverflow_)},{"unmatchedReturns",static_cast<qint64>(unmatched_)},
      {"slowRecordOverflow",static_cast<qint64>(slowOverflow_)},{"publicationOverflow",static_cast<qint64>(publicationOverflow_)},
      {"posePublications",publications},{"publicationColumns",QJsonArray{"atMs","revision","clip","seconds","posePayloadHash","exactPayloadVersion"}},
      {"calibration",calibration_},{"resizeControl",resize_},{"pendingNativeDepth",static_cast<int>(state_.nativeDepth)},{"pendingQtDepth",static_cast<int>(state_.qtDepth)},{"boundary","Hooks observe this GUI thread only; WCT may lack names for unsupported objects. Pose hash is diagnostic correlation, not an equality proof."}};
  }
private:
  static LRESULT CALLBACK calibrationProc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
    if(message==calibrationMessage){const HANDLE mutex=reinterpret_cast<HANDLE>(lp);
      const auto wait=WaitForSingleObject(mutex,5000);if(wait==WAIT_OBJECT_0)ReleaseMutex(mutex);return wait;}
    return DefWindowProcW(hwnd,message,wp,lp);
  }
  static LRESULT CALLBACK beforeHook(int code,WPARAM wp,LPARAM lp) {
    const DWORD priorError=GetLastError();auto *probe=active.load();
    if(code==HC_ACTION&&probe){const auto &m=*reinterpret_cast<const CWPSTRUCT*>(lp);
      Dispatch d;d.start=probe->now();d.message=m.message;d.window=reinterpret_cast<std::uintptr_t>(m.hwnd);
      d.wparam=static_cast<std::uint64_t>(m.wParam);d.lparam=static_cast<std::uint64_t>(m.lParam);d.sendFlags=InSendMessageEx(nullptr);
      std::lock_guard lock(probe->mutex_);d.sequence=++probe->serial_;++probe->nativeCount_;
      if(probe->state_.nativeDepth<32)probe->state_.native[probe->state_.nativeDepth]=d;else ++probe->depthOverflow_;
      ++probe->state_.nativeDepth;}
    SetLastError(priorError);return CallNextHookEx(nullptr,code,wp,lp);
  }
  static LRESULT CALLBACK afterHook(int code,WPARAM wp,LPARAM lp) {
    const DWORD priorError=GetLastError();auto *probe=active.load();
    if(code==HC_ACTION&&probe){const auto &m=*reinterpret_cast<const CWPRETSTRUCT*>(lp);const auto end=probe->now();
      std::lock_guard lock(probe->mutex_);
      if(!probe->state_.nativeDepth)++probe->unmatched_;
      else {--probe->state_.nativeDepth;if(probe->state_.nativeDepth<32){auto d=probe->state_.native[probe->state_.nativeDepth];
        if(d.message!=m.message||d.window!=reinterpret_cast<std::uintptr_t>(m.hwnd))++probe->unmatched_;
        d.elapsed=end-d.start;probe->recordSlow(probe->nativeSlow_,d);}}}
    SetLastError(priorError);return CallNextHookEx(nullptr,code,wp,lp);
  }
  void recordSlow(std::vector<Dispatch> &records,const Dispatch &d) {
    if(d.elapsed<20)return;
    if(records.size()<records.capacity())records.push_back(d);else ++slowOverflow_;
  }
  void close() {
    if(control_){DestroyWindow(control_);control_=nullptr;}
    if(atom_){UnregisterClassW(L"FormaView.ForensicCalibration",GetModuleHandleW(nullptr));atom_=0;}
    if(before_){UnhookWindowsHookEx(before_);before_=nullptr;}
    if(after_){UnhookWindowsHookEx(after_);after_=nullptr;}
    if(active.load()==this)active=nullptr;
  }
  Clock::time_point origin_;DWORD gui_{};HHOOK before_{},after_{};HWND control_{};ATOM atom_{};
  std::mutex mutex_;Snapshot state_;
  std::uint64_t serial_{},nativeCount_{},qtCount_{},depthOverflow_{},unmatched_{},slowOverflow_{},publicationOverflow_{};
  std::uint64_t contentVersion_{};std::vector<float> previousPalette_;std::vector<std::array<double,16>> previousWorlds_;
  std::vector<Dispatch> nativeSlow_,qtSlow_;std::vector<Publication> publications_;QJsonObject calibration_,resize_;
};
class Application:public QGuiApplication {
public:using QGuiApplication::QGuiApplication;
  bool notify(QObject *receiver,QEvent *event) override {
    if(GetCurrentThreadId()!=guiThread_)return QGuiApplication::notify(receiver,event);
    auto *probe=active.load();
    if(!probe)return QGuiApplication::notify(receiver,event);
    const auto sequence=probe->enterQt(receiver,event);const auto finish=qScopeGuard([&]{probe->leaveQt(sequence);});
    return QGuiApplication::notify(receiver,event);
  }
private:DWORD guiThread_{GetCurrentThreadId()};
};
} // namespace model_forensics
