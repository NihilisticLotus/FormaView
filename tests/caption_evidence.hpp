#pragma once
#include <QtGlobal>
#ifdef Q_OS_WIN
#include <QAbstractNativeEventFilter>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPoint>
#include <windows.h>

// Test-only, bounded native-event evidence. No disk I/O or logging in the
// gesture path; a failing test publishes the snapshot before HWND retirement.
class CaptionEvidence final : public QAbstractNativeEventFilter {
public:
  explicit CaptionEvidence(HWND hwnd):hwnd_(hwnd){qGuiApp->installNativeEventFilter(this);}
  ~CaptionEvidence() override {qGuiApp->removeNativeEventFilter(this);}
  void checkpoint(const QString &phase, QPoint point={}, int hit=0){
    phase_=phase;record(0,WPARAM(hit),0,point);
  }
  QByteArray json() const{return QJsonDocument(QJsonObject{{"events",events_},{"dropped",dropped_}}).toJson(QJsonDocument::Compact);}
  bool nativeEventFilter(const QByteArray &,void *raw,qintptr *) override {
    const auto *m=static_cast<MSG *>(raw);
    if(m->hwnd==hwnd_ && (m->message==WM_NCLBUTTONDOWN||m->message==WM_LBUTTONUP||
       m->message==WM_NCLBUTTONUP||m->message==WM_CANCELMODE||m->message==WM_CAPTURECHANGED||
       m->message==WM_ACTIVATE||m->message==WM_SYSCOMMAND||m->message==WM_WINDOWPOSCHANGED||
       m->message==WM_KEYDOWN))record(m->message,m->wParam,m->lParam);
    return false;
  }
private:
  void record(UINT message,WPARAM wp,LPARAM lp,QPoint point={}){
    RECT r{};GetWindowRect(hwnd_,&r);POINT origin{0,0};ClientToScreen(hwnd_,&origin);
    if(events_.size()==128){events_.removeAt(0);++dropped_;}
    events_.append(QJsonObject{{"phase",phase_},{"tick",double(GetTickCount64())},
      {"message",int(message)},{"wp",QString::number(wp,16)},{"lp",QString::number(quintptr(lp),16)},
      {"hwnd",QString::number(quintptr(hwnd_),16)},
      {"capture",QString::number(quintptr(GetCapture()),16)},
      {"foreground",QString::number(quintptr(GetForegroundWindow()),16)},
      {"rect",QJsonArray{int(r.left),int(r.top),int(r.right),int(r.bottom)}},
      {"clientOrigin",QJsonArray{int(origin.x),int(origin.y)}},
      {"screenPoint",QJsonArray{point.x(),point.y()}},{"dpi",int(GetDpiForWindow(hwnd_))},
      {"enabled",bool(IsWindowEnabled(hwnd_))},{"iconic",bool(IsIconic(hwnd_))},{"zoom",bool(IsZoomed(hwnd_))}});
  }
  HWND hwnd_{};QString phase_;QJsonArray events_;int dropped_{};
};
#endif
