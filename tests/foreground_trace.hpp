#pragma once

// Test-only activation journal. Hooks run on the installing GUI thread and
// record fixed-size metadata, never window titles, input text, or screenshots.
#include <windows.h>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <array>
#include <chrono>
#include <set>

class ForegroundTrace final {
  using Clock = std::chrono::steady_clock;
  struct Record {
    double ms{};
    DWORD event{}, eventTick{}, tid{}, pid{}, foregroundTid{}, foregroundPid{};
    HWND window{}, foreground{}, other{};
    ULONG_PTR detail{};
    bool visible{}, iconic{};
  };
  inline static thread_local ForegroundTrace *current_{};
  Clock::time_point origin_{Clock::now()};
  const ULONGLONG startTick_{GetTickCount64()};
  HWND target_{};
  HHOOK native_{};
  HWINEVENTHOOK foreground_{};
  std::array<Record, 1024> records_{};
  unsigned count_{}, lost_{};
  DWORD nativeError_{}, foregroundError_{};
  bool stopped_{};

  void append(DWORD event, HWND window, DWORD tick=0, HWND other=nullptr,
              ULONG_PTR detail=0) {
    if (count_ == records_.size()) { ++lost_; return; }
    auto &r=records_[count_++];
    r.ms=std::chrono::duration<double,std::milli>(Clock::now()-origin_).count();
    r.event=event; r.eventTick=tick; r.window=window; r.other=other; r.detail=detail;
    r.foreground=GetForegroundWindow();
    if(window) r.tid=GetWindowThreadProcessId(window,&r.pid);
    if(r.foreground) r.foregroundTid=GetWindowThreadProcessId(r.foreground,&r.foregroundPid);
    r.visible=IsWindowVisible(target_)!=FALSE; r.iconic=IsIconic(target_)!=FALSE;
  }
  static void CALLBACK foregroundEvent(HWINEVENTHOOK, DWORD event, HWND window,
      LONG, LONG, DWORD, DWORD tick) {
    if(current_) current_->append(0x10000+event,window,tick);
  }
  static LRESULT CALLBACK nativeEvent(int code, WPARAM wp, LPARAM lp) {
    if(code>=0 && current_) {
      const auto &m=*reinterpret_cast<const CWPSTRUCT *>(lp);
      if(m.hwnd==current_->target_ &&
         (m.message==WM_ACTIVATE || m.message==WM_ACTIVATEAPP ||
          m.message==WM_SETFOCUS || m.message==WM_KILLFOCUS ||
          m.message==WM_SHOWWINDOW || m.message==WM_DESTROY))
        current_->append(m.message,m.hwnd,GetTickCount(),
          m.message==WM_ACTIVATE ? reinterpret_cast<HWND>(m.lParam) :
          (m.message==WM_SETFOCUS || m.message==WM_KILLFOCUS) ? reinterpret_cast<HWND>(m.wParam) : nullptr,
          m.wParam);
    }
    return CallNextHookEx(nullptr,code,wp,lp);
  }
  static QString handle(HWND value) {
    return QString::number(reinterpret_cast<quintptr>(value),16);
  }
public:
  explicit ForegroundTrace(HWND target):target_(target) {
    Q_ASSERT(!current_); current_=this;
    native_=SetWindowsHookExW(WH_CALLWNDPROC,nativeEvent,nullptr,GetCurrentThreadId());
    if(!native_) nativeError_=GetLastError();
    foreground_=SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,
      nullptr,foregroundEvent,0,0,WINEVENT_OUTOFCONTEXT);
    if(!foreground_) foregroundError_=GetLastError();
    append(0x20000,target_);
  }
  ForegroundTrace(const ForegroundTrace &)=delete;
  ForegroundTrace &operator=(const ForegroundTrace &)=delete;
  ~ForegroundTrace() { stop(); }
  bool ready() const { return native_ && foreground_ && !stopped_; }
  bool complete() const { return !nativeError_ && !foregroundError_ && !lost_; }
  double elapsedMs() const {
    return std::chrono::duration<double,std::milli>(Clock::now()-origin_).count();
  }
  void observedLoss(HWND observed, bool active, bool exposed) {
    append(0x20001,observed,GetTickCount(),target_,(active?1:0)|(exposed?2:0));
  }
  void afterLoss() { append(0x20002,GetForegroundWindow(),GetTickCount()); }
  void stop() {
    if(stopped_)return;
    if(foreground_) UnhookWinEvent(foreground_);
    if(native_) UnhookWindowsHookEx(native_);
    current_=nullptr; stopped_=true;
  }
  QJsonObject report() const {
    QJsonArray rows,owners; std::set<DWORD> pids;
    for(unsigned i=0;i<count_;++i) {
      const auto &r=records_[i];
      rows.append(QJsonObject{{"ms",r.ms},{"event",qint64(r.event)},
        {"eventTick",qint64(r.eventTick)},{"hwnd",handle(r.window)},
        {"pid",qint64(r.pid)},{"tid",qint64(r.tid)},
        {"foregroundHwnd",handle(r.foreground)},{"foregroundPid",qint64(r.foregroundPid)},
        {"foregroundTid",qint64(r.foregroundTid)},{"otherHwnd",handle(r.other)},
        {"detail",qint64(r.detail)},{"targetVisible",r.visible},{"targetIconic",r.iconic}});
      if(r.pid)pids.insert(r.pid);
      if(r.foregroundPid)pids.insert(r.foregroundPid);
    }
    for(const auto pid:pids) {
      QJsonObject owner{{"pid",qint64(pid)}};
      HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
      if(process) {
        wchar_t path[32768]{};DWORD size=32768;
        if(QueryFullProcessImageNameW(process,0,path,&size))
          owner["imageName"]=QFileInfo(QString::fromWCharArray(path,size)).fileName();
        else owner["queryError"]=qint64(GetLastError());
        CloseHandle(process);
      } else owner["openError"]=qint64(GetLastError());
      owners.append(owner);
    }
    return {{"targetHwnd",handle(target_)},{"records",rows},{"owners",owners},
      {"complete",complete()},{"startTick64",qint64(startTick_)},
      {"eventNames",QJsonObject{{"6","WM_ACTIVATE"},{"7","WM_SETFOCUS"},
        {"8","WM_KILLFOCUS"},{"24","WM_SHOWWINDOW"},{"28","WM_ACTIVATEAPP"},
        {"2","WM_DESTROY"},{"65539","EVENT_SYSTEM_FOREGROUND"},
        {"131072","journalStart"},{"131073","observedLoss"},{"131074","afterLoss"}}},
      {"lostRecords",int(lost_)},{"nativeHookError",qint64(nativeError_)},
      {"foregroundHookError",qint64(foregroundError_)},
      {"boundary","GUI-thread native activation and asynchronous foreground events; ms is callback time from journal creation. Owner image names are queried after measurement and can be unavailable or PID-reused. A foreground event identifies the destination, not who requested activation. No focus recovery or grace period."}};
  }
};
