#pragma once

// Test-only bounded capture. GUI journals have a single writer. Render callbacks
// never inspect a QObject; their journal and stop barrier share one small mutex.
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

namespace stress_trace {
using Clock = std::chrono::steady_clock;
using Id = std::uint64_t;
enum class Phase { Setup, Open, Cancel, Supersede, WaitInspect, WaitView,
                   Orbit, WaitRender, Clear, WaitDestroyed, WaitClear, Resources };
struct Limits {
  std::size_t calls{524288}, operations{2048}, publications{16384}, renders{131072};
};
class Trace {
public:
  struct Token { Id id{}, parent{}; std::size_t slot{}; unsigned depth{}; bool stored{}; };
  struct Call {
    Id id{}, parent{}, receiver{}, operationIn{}, operationOut{}, publicationIn{}, publicationOut{};
    qint64 enter{}, exit{};
    int type{};
    unsigned depth{};
    bool closed{}, unwound{};
  };
  struct Operation { Id id{}; qint64 ns{}; int cycle{}, phase{}; };
  struct Publication { Id id{}, revision{}, operation{}; qint64 ns{}; };
  struct Render { Id id{}, sync{}, publication{}, operation{}; qint64 ns{}; int kind{}; };

  explicit Trace(Limits limits = {}) : origin_(Clock::now()), calls_(limits.calls),
      operations_(limits.operations), publications_(limits.publications), renders_(limits.renders) {
    initializationMs_ = std::chrono::duration<double, std::milli>(Clock::now()-origin_).count();
    origin_ = Clock::now();
  }
  qint64 ns(Clock::time_point value) const {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value-origin_).count();
  }
  Token enter(const void *receiver, int type, Clock::time_point started) {
    Token token{++entered_, active_, callCount_, ++depth_, callCount_<calls_.size()};
    active_ = token.id;
    if (token.stored) {
      auto &r=calls_[callCount_++];
      r.id=token.id; r.parent=token.parent; r.receiver=reinterpret_cast<std::uintptr_t>(receiver);
      r.enter=ns(started); r.type=type; r.depth=token.depth;
      r.operationIn=currentOperation_.load(std::memory_order_relaxed);
      r.publicationIn=published_.load(std::memory_order_relaxed);
    } else ++callLoss_;
    return token;
  }
  void leave(Token token, Clock::time_point finished, bool unwound=false) {
    if (active_!=token.id || depth_!=token.depth) ++stackErrors_;
    if (token.stored) {
      auto &r=calls_[token.slot]; r.exit=ns(finished); r.closed=true; r.unwound=unwound;
      r.operationOut=currentOperation_.load(std::memory_order_relaxed);
      r.publicationOut=published_.load(std::memory_order_relaxed);
    }
    ++completed_; if(unwound) ++unwound_;
    active_=token.parent; if(depth_) --depth_; else ++stackErrors_;
  }
  void phase(int cycle, Phase phase) {
    const Id id=++operationTotal_;
    if(operationCount_<operations_.size())
      operations_[operationCount_++]={id,ns(Clock::now()),cycle,static_cast<int>(phase)};
    else ++operationLoss_;
    currentOperation_.store(id,std::memory_order_release);
  }
  void publish(Id revision) {
    const Id id=++publicationTotal_;
    if(publicationCount_<publications_.size()) {
      publications_[publicationCount_++]={id,revision,currentOperation_.load(),ns(Clock::now())};
      // Each publication slot is immutable once visible to a render callback.
      published_.store(id,std::memory_order_release);
    } else { ++publicationLoss_; published_.store(0,std::memory_order_release); }
  }
  void render(bool sync) {
    std::lock_guard lock(renderMutex_);
    if(stopped_) return;
    const auto operation=currentOperation_.load(std::memory_order_acquire);
    const auto publication=sync ? published_.load(std::memory_order_acquire) : syncedPublication_;
    const auto now=ns(Clock::now());
    const Id id=++renderTotal_;
    if(sync) { lastSync_=id; syncedPublication_=publication; }
    if(renderCount_<renders_.size())
      renders_[renderCount_++]={id,lastSync_,syncedPublication_,operation,now,sync?0:1};
    else ++renderLoss_;
    if(!sync) { ++swaps_; if(!lastSync_) ++swapsWithoutSync_; }
  }
  void stop() { std::lock_guard lock(renderMutex_); stopped_=true; }
  bool complete() const {
    return stopped_ && entered_==completed_ && depth_==0 && !active_ && !stackErrors_ &&
      !unwound_ && !callLoss_ && !operationLoss_ && !publicationLoss_ && !renderLoss_;
  }
  const Call &call(std::size_t i) const { return calls_.at(i); }
  const Render &renderRecord(std::size_t i) const { return renders_.at(i); }
  std::size_t callCount() const { return callCount_; }
  Id entered() const { return entered_; }
  Id callLoss() const { return callLoss_; }
  Id swaps() const { return swaps_; }
  QJsonObject summary() const {
    return {{"complete",complete()},{"callsEntered",qint64(entered_)},{"callsCompleted",qint64(completed_)},
      {"callsStored",qint64(callCount_)},{"callLoss",qint64(callLoss_)},{"stackErrors",qint64(stackErrors_)},
      {"unwoundCalls",qint64(unwound_)},{"openDepth",int(depth_)},
      {"operationCount",qint64(operationCount_)},{"operationLoss",qint64(operationLoss_)},
      {"publicationCount",qint64(publicationCount_)},{"publicationLoss",qint64(publicationLoss_)},
      {"renderCount",qint64(renderCount_)},{"renderLoss",qint64(renderLoss_)},
      {"swaps",qint64(swaps_)},{"swapsWithoutPriorCapturedSync",qint64(swapsWithoutSync_)},
      {"initializationMs",initializationMs_},
      {"capacity",QJsonArray{qint64(calls_.size()),qint64(operations_.size()),qint64(publications_.size()),qint64(renders_.size())}},
      {"bufferBytes",qint64(calls_.size()*sizeof(Call)+operations_.size()*sizeof(Operation)+publications_.size()*sizeof(Publication)+renders_.size()*sizeof(Render))}};
  }
  // Called after measurement and the render stop barrier. No growing JSON tree.
  bool write(const QString &path) const {
    if(!stopped_) return false;
    QSaveFile file(path); if(!file.open(QIODevice::WriteOnly)) return false;
    const auto line=[&](const QJsonObject &row) {
      const auto bytes=QJsonDocument(row).toJson(QJsonDocument::Compact)+'\n';
      return file.write(bytes)==bytes.size();
    };
    if(!line({{"schema",1},{"summary",summary()},
      {"clock","steady_clock nanoseconds relative to capture origin; journals grouped by kind, not global time order"},
      {"callColumns",QJsonArray{"id","parent","depth","enterNs","exitNs","receiverHex","eventType","operationIn","operationOut","publicationIn","publicationOut","closed","unwound"}},
      {"operationColumns",QJsonArray{"id","ns","cycle","phase"}},
      {"phaseNames",QJsonArray{"setup","open","cancel","supersede","waitInspect","waitView","orbit","waitRender500","clear","waitDestroyed","waitClear20","resourceSample"}},
      {"publicationColumns",QJsonArray{"id","ns","revision","operation"}},
      {"renderColumns",QJsonArray{"id","ns","kind0Sync1Swap","syncId","publicationAtSync","operationAtCallback"}},
      {"boundary","receiverHex is an opaque address that may be reused, never dereferenced later. publicationAtSync identifies GUI publication at afterSynchronizing, not proof of a particular rendered payload or physical presentation. Initial swaps may have no captured prior sync. Missing records invalidate completeness. Inclusive notify counts and the original zero-over100ms assertion are never deduplicated."}})) return false;
    for(std::size_t i=0;i<callCount_;++i) { const auto &r=calls_[i];
      if(!line({{"call",QJsonArray{qint64(r.id),qint64(r.parent),int(r.depth),r.enter,r.exit,QString::number(r.receiver,16),r.type,qint64(r.operationIn),qint64(r.operationOut),qint64(r.publicationIn),qint64(r.publicationOut),r.closed,r.unwound}}})) return false;
    }
    for(std::size_t i=0;i<operationCount_;++i) { const auto &r=operations_[i];
      if(!line({{"operation",QJsonArray{qint64(r.id),r.ns,r.cycle,r.phase}}})) return false;
    }
    for(std::size_t i=0;i<publicationCount_;++i) { const auto &r=publications_[i];
      if(!line({{"publication",QJsonArray{qint64(r.id),r.ns,qint64(r.revision),qint64(r.operation)}}})) return false;
    }
    for(std::size_t i=0;i<renderCount_;++i) { const auto &r=renders_[i];
      if(!line({{"render",QJsonArray{qint64(r.id),r.ns,r.kind,qint64(r.sync),qint64(r.publication),qint64(r.operation)}}})) return false;
    }
    return file.commit();
  }
private:
  Clock::time_point origin_;
  std::vector<Call> calls_;
  std::vector<Operation> operations_;
  std::vector<Publication> publications_;
  std::vector<Render> renders_;
  std::size_t callCount_{},operationCount_{},publicationCount_{},renderCount_{};
  Id entered_{},completed_{},active_{},callLoss_{},operationTotal_{},operationLoss_{},
    publicationTotal_{},publicationLoss_{},renderTotal_{},renderLoss_{},stackErrors_{},unwound_{},
    lastSync_{},syncedPublication_{},swaps_{},swapsWithoutSync_{};
  unsigned depth_{};
  std::atomic<Id> currentOperation_{0},published_{0};
  mutable std::mutex renderMutex_;
  bool stopped_{};
  double initializationMs_{};
};
} // namespace stress_trace
