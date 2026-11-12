// Opt-in CPU/session soak. QCoreApplication only: no window, scene graph or GPU.
// Usage: formaview_gif_stability_tests OUTPUT_DIRECTORY SECONDS (60..7200).
#include "image_session.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimer>
#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <stdexcept>
#include <vector>
#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
constexpr int width = 512, height = 384, exportCount = 512;
constexpr double mib = 1024. * 1024.;
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
void json(const QString &path, const QJsonObject &object) {
  QSaveFile file(path);
  const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
  require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(),
          "Cannot publish soak evidence");
}
QByteArray sha(const QString &path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "Cannot hash soak input");
  QCryptographicHash hash(QCryptographicHash::Sha256);
  require(hash.addData(&file), "Cannot stream soak input hash");
  return hash.result().toHex();
}
void word(QByteArray &out, int value) { out += char(value & 255); out += char(value >> 8); }
QByteArray rectangle(int x, int y, int w, int h, int color, int disposal) {
  auto out = QByteArray::fromHex("21f904");
  out += char((disposal << 2) | 1); word(out, 2); out += char(0); out += char(0);
  out += ','; word(out, x); word(out, y); word(out, w); word(out, h); out += char(0);
  // Explicit clear,index pairs keep LZW codes at 3 bits and make fixtures
  // independent of a GIF encoder or another compositor's disposal policy.
  QByteArray packed;
  unsigned bits = 0, bitCount = 0;
  const auto code = [&](unsigned value) {
    bits |= value << bitCount; bitCount += 3;
    while (bitCount >= 8) { packed += char(bits & 255); bits >>= 8; bitCount -= 8; }
  };
  for (int i = 0; i < w * h; ++i) { code(4); code(static_cast<unsigned>(color)); }
  code(5); if (bitCount) packed += char(bits & 255);
  out += char(2);
  for (qsizetype i = 0; i < packed.size(); i += 255) {
    const auto count = std::min<qsizetype>(255, packed.size() - i);
    out += char(count); out += packed.mid(i, count);
  }
  out += char(0);
  return out;
}
void fixture(const QString &path, int frames, int loops) {
  auto header = QByteArrayLiteral("GIF89a"); word(header, width); word(header, height);
  header += QByteArray::fromHex("810300000000ff000000ff000000ff21ff0b");
  header += "NETSCAPE2.0"; header += QByteArray::fromHex("0301"); word(header, loops); header += char(0);
  const std::array patches{rectangle(0, 0, width, height, 1, 1),
                           rectangle(32, 32, 32, 32, 2, 2),
                           rectangle(64, 64, 32, 32, 2, 3),
                           rectangle(96, 96, 32, 32, 3, 1)};
  QFile file(path);
  require(file.open(QIODevice::WriteOnly) && file.write(header) == header.size(), "Cannot write GIF header");
  for (int i = 0; i < frames; ++i)
    require(file.write(patches[i % 4]) == patches[i % 4].size(), "Cannot write GIF frames");
  require(file.write(";", 1) == 1, "Cannot write GIF trailer");
}
QColor expected(int frame, int x, int y) {
  const int phase = frame % 4;
  if (x >= 32 && x < 64 && y >= 32 && y < 64)
    return phase == 1 ? QColor(Qt::green) : phase >= 2 ? QColor(Qt::blue) : QColor(Qt::red);
  if (phase == 2 && x >= 64 && x < 96 && y >= 64 && y < 96) return QColor(Qt::green);
  if (phase == 3 && x >= 96 && x < 128 && y >= 96 && y < 128) return QColor(Qt::blue);
  return QColor(Qt::red);
}
struct Sample { double seconds{}, privateBytes{}, workingSet{}; int handles{}; };
Sample memory(double seconds) {
#ifdef Q_OS_WIN
  PROCESS_MEMORY_COUNTERS_EX counters{}; counters.cb = sizeof(counters);
  DWORD handles{};
  require(K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                                 sizeof(counters)) && GetProcessHandleCount(GetCurrentProcess(), &handles),
          "Cannot sample process memory/handles");
  return {seconds, double(counters.PrivateUsage), double(counters.WorkingSetSize), int(handles)};
#else
  (void)seconds;
  throw std::runtime_error("Process memory acceptance requires the Windows probe");
#endif
}
double median(std::vector<double> values) {
  require(!values.empty(), "Insufficient memory samples");
  std::sort(values.begin(), values.end()); return values[values.size() / 2];
}

class Soak {
public:
  Soak(QString output, int seconds) : root_(std::move(output)), duration_(seconds),
      warmup_(std::min(60., seconds / 6.)), window_(std::min(120., seconds / 4.)),
      play_(std::make_unique<ImageSession>()), writer_(std::make_unique<ImageSession>()) {
    require(QDir().mkpath(root_), "Cannot create soak evidence directory");
    require(!QFileInfo::exists(root_ + "/summary.json"), "Use a fresh evidence directory");
    infinite_ = root_ + "/infinite.gif"; finite_ = root_ + "/finite.gif";
    long_ = root_ + "/export.gif"; still_ = root_ + "/still.png";
    fixture(infinite_, 4, 0); fixture(finite_, 4, 2); fixture(long_, exportCount, 0);
    QImage still(16, 16, QImage::Format_ARGB32); still.fill(Qt::red);
    require(still.save(still_), "Cannot create still-image fixture");
    for (const auto &path : {infinite_, finite_, long_, still_}) hashes_[path] = QString::fromLatin1(sha(path));
    for (int f = 0; f < 4; ++f) {
      expected_[f] = QImage(width, height, QImage::Format_ARGB32);
      for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) expected_[f].setPixelColor(x, y, expected(f, x, y));
    }
    json(root_ + "/protocol.json", {{"seconds", seconds}, {"warmup_seconds", warmup_},
      {"comparison_window_seconds", window_}, {"canvas", "512x384"}, {"play_frames", 4},
      {"export_frames", exportCount}, {"frame_delay_ms", 20}, {"private_tail_growth_limit_mib", 16},
      {"private_peak_growth_limit_mib", 64}, {"private_slope_limit_mib_per_minute", .5},
      {"handle_growth_limit", 16}, {"cancel_limit_ms", 2000}, {"seek_limit_ms", 5000},
      {"input_sha256", hashes_}, {"scope", "QCoreApplication; production ImageSession; CPU memory only"}});
    samplesFile_.setFileName(root_ + "/samples.csv");
    events_.setFileName(root_ + "/events.jsonl");
    require(samplesFile_.open(QIODevice::WriteOnly) && events_.open(QIODevice::WriteOnly), "Cannot open evidence streams");
    samplesFile_.write("seconds,phase,private_bytes,working_set_bytes,handles,presentations,natural_wraps,exports,cancels,frame,playing,busy,export_progress\n");
    QObject::connect(play_.get(), &ImageSession::changed, &tick_, [this] { guard([&] { presented(); }); });
    QObject::connect(&tick_, &QTimer::timeout, &tick_, [this] { guard([&] { tick(); }); });
    origin_ = Clock::now(); lastPresentation_ = 0;
    play_->open(infinite_); writer_->open(long_);
    mode_ = "initial"; modeStart_ = 0;
    nextAction_ = warmup_ * 1000.;
    tick_.start(10);
  }
  int result() const { return result_; }
private:
  double ms() const { return std::chrono::duration<double, std::milli>(Clock::now() - origin_).count(); }
  template<class F> void guard(F function) {
    if (finished_) return;
    try { function(); } catch (const std::exception &error) { finish(error.what()); }
  }
  void event(const QString &name, QJsonObject data = {}) {
    data["event"] = name; data["ms"] = ms();
    events_.write(QJsonDocument(data).toJson(QJsonDocument::Compact) + '\n'); events_.flush();
  }
  void presented() {
    if (play_->busy() || !play_->available() || !play_->error().isEmpty()) return;
    const auto pixels = play_->pixels();
    if (pixels->revision == revision_) return;
    revision_ = pixels->revision;
    if (play_->path() == still_) return;
    require(pixels->source.data_window.width == width && pixels->source.data_window.height == height,
            "Unexpected playback canvas");
    const auto check = [&](int x, int y) {
      const auto i = std::size_t(y * width + x) * 4;
      const auto actual = QColor::fromRgbF(pixels->rgba[i], pixels->rgba[i+1], pixels->rgba[i+2], pixels->rgba[i+3]);
      require(actual == expected(play_->frame(), x, y), "Published frame pixel mismatch");
    };
    check(0, 0); check(40, 40); check(72, 72); check(104, 104); check(width-1, height-1);
    if (previousPath_ == play_->path() && previousFrame_ == 3 && play_->frame() == 0 &&
        mode_.isEmpty() && play_->playing()) ++wraps_;
    previousPath_ = play_->path(); previousFrame_ = play_->frame();
    if (play_->path() == finite_ && play_->frame() == 3) ++finiteTails_;
    if (mode_ == "seek" && play_->frame() == seekTarget_) seekSeen_ = true;
    weak_.push_back(pixels); if (weak_.size() > 128) weak_.pop_front();
    ++presentations_; lastPresentation_ = ms();
  }
  void resumeInfinite() {
    play_->open(infinite_); mode_ = "resume"; modeStart_ = ms();
  }
  void playActions(double now) {
    require(play_->error().isEmpty(), "Playback/decode error");
    if (!mode_.isEmpty()) require(now - modeStart_ < 5000, "Playback action stalled");
    if ((mode_ == "initial" || mode_ == "resume") && !play_->busy()) {
      require(play_->path() == infinite_ && play_->frameCount() == 4, "Reopen failed");
      play_->togglePlay(); mode_.clear(); event("play");
    } else if (mode_ == "pause-drain" && !play_->busy()) {
      pauseRevision_ = play_->pixels()->revision; mode_ = "paused"; pauseAt_ = now;
    } else if (mode_ == "paused" && now - pauseAt_ >= 250) {
      require(!play_->playing() && play_->pixels()->revision == pauseRevision_, "Paused frame advanced");
      ++pauses_; play_->togglePlay(); mode_.clear(); event("pause-resume");
    } else if (mode_ == "seek" && seekSeen_) {
      maxSeekMs_ = std::max(maxSeekMs_, now - modeStart_); ++seeks_; mode_.clear(); event("seek-burst-complete");
    } else if (mode_ == "tail" && !play_->playing() && !play_->busy()) {
      require(play_->frame() == 3, "Loop-off stopped before the tail");
      ++loopStops_; event("loop-off-tail"); resumeInfinite();
    } else if (mode_ == "clear" && now - modeStart_ >= 100) {
      require(!play_->available() && !play_->playing(), "Clear retained presentation/playback");
      ++clears_; resumeInfinite();
    } else if (mode_ == "finite-open" && !play_->busy()) {
      require(play_->info().value("loopCount").toInt() == 2, "Finite source loop metadata lost");
      mode_ = "finite-play"; play_->togglePlay();
    } else if (mode_ == "finite-play" && !play_->playing() && !play_->busy()) {
      require(play_->frame() == 3 && finiteTails_ == 3, "Finite source did not run exactly three passes");
      ++finiteRuns_; event("finite-loop-complete"); resumeInfinite();
    } else if (mode_ == "still" && !play_->busy()) {
      require(play_->path() == still_ && !play_->animated() && !play_->playing(), "Still switch failed");
      ++switches_; resumeInfinite();
    } else if (mode_ == "shared-export" && !play_->exporting()) {
      require(play_->exportError().isEmpty() && play_->exportProgress() == 4, "Export on playing session failed");
      ++sharedExports_; mode_.clear(); event("same-session-export-complete");
    }
    if (!mode_.isEmpty() || now < nextAction_) return;
    nextAction_ += 10000; modeStart_ = now;
    switch (action_++ % 7) {
    case 0: play_->togglePlay(); mode_ = "pause-drain"; break;
    case 1:
      mode_ = "seek"; seekSeen_ = false; seekTarget_ = 2;
      for (int i = 0; i < 128; ++i) play_->seekFrame((i * 7) % 4);
      play_->seekFrame(seekTarget_); seekRequests_ += 129; break;
    case 2: play_->setLoop(false); mode_ = "tail"; break;
    case 3: play_->clear(); mode_ = "clear"; break;
    case 4: finiteTails_ = 0; play_->open(finite_); mode_ = "finite-open"; break;
    case 5: play_->open(still_); mode_ = "still"; break;
    case 6: play_->exportFrames(root_ + "/shared", "frame-", "overwrite"); mode_ = "shared-export"; break;
    }
    event("play-action", {{"mode", mode_}});
  }
  void exportActions(double now, bool stopping) {
    require(writer_->error().isEmpty(), "Export session decode/reopen error");
    if (exportState_ == "opening" && !writer_->busy()) {
      require(writer_->frameCount() == exportCount, "Export source reopen failed"); exportState_ = "idle";
    }
    if (exportState_ == "running") {
      require(now - exportStarted_ < 60000, "Streaming export stalled");
      if (writer_->exporting() && !cancelRequested_ &&
          ((exportKind_ != 0 && writer_->exportProgress() >= 8) || stopping)) {
        cancelRequested_ = true; cancelAt_ = now;
        if (exportKind_ == 2) writer_->clear();
        else if (exportKind_ == 3) writer_->open(infinite_);
        else writer_->cancelExport();
        event("export-cancel-request", {{"kind", exportKind_}, {"progress", writer_->exportProgress()}});
      }
      if (writer_->exporting()) {
        if (cancelRequested_) require(now - cancelAt_ <= 2000, "Export cancellation stalled");
        return;
      }
      files_ = QDir(batch_).entryList({"*.png"}, QDir::Files, QDir::Name);
      require(!files_.isEmpty() && QDir(batch_).entryList({".formaview-*"}, QDir::Files | QDir::Hidden).isEmpty(),
              "Export left no output or leaked transaction files");
      if (cancelRequested_) {
        require(writer_->exportError().contains("cancel", Qt::CaseInsensitive) &&
                files_.size() < exportCount && writer_->exportProgress() < exportCount,
                "Cancelled export reported completion or lost its error");
        maxCancelMs_ = std::max(maxCancelMs_, now - cancelAt_); ++cancels_;
        if (exportKind_ == 2) ++clearCancels_; else if (exportKind_ == 3) ++openCancels_; else ++explicitCancels_;
      } else {
        require(writer_->exportError().isEmpty() && writer_->exportProgress() == exportCount && files_.size() == exportCount,
                "Completed export has wrong progress/count/error");
        ++exports_;
      }
      verified_ = 0; exportState_ = "verify";
      event("export-terminal", {{"cancelled", cancelRequested_}, {"files", int(files_.size())},
                                 {"progress", writer_->exportProgress()}});
    }
    if (exportState_ == "verify") {
      // Bound verification work per event-loop turn so playback remains live.
      for (int end = std::min(verified_ + 4, int(files_.size())); verified_ < end; ++verified_) {
        const auto wanted = QString("frame-%1.png").arg(verified_, 4, 10, QLatin1Char('0'));
        require(files_[verified_] == wanted, "Export frame numbering has a gap");
        const QImage image(batch_ + '/' + wanted);
        require(!image.isNull() && image.convertToFormat(QImage::Format_ARGB32) == expected_[verified_ % 4],
                "Exported PNG is incomplete or has incorrect composed pixels");
        require(QFile::remove(batch_ + '/' + wanted), "Cannot release verified PNG"); ++pngs_;
      }
      if (verified_ != files_.size()) return;
      require(QDir().rmdir(batch_), "Export folder contains unaccounted files");
      files_.clear(); ++batchCount_; writer_->open(long_); exportState_ = "opening"; nextExport_ = now + 1000;
      event("export-verified");
    }
    if (exportState_ == "idle" && !stopping && now >= nextExport_ && !writer_->busy()) {
      batch_ = root_ + QString("/batch-%1").arg(batchCount_, 5, 10, QLatin1Char('0'));
      exportKind_ = batchCount_ % 4; cancelRequested_ = false; exportStarted_ = now;
      writer_->exportFrames(batch_, "frame-", "skip");
      require(writer_->exporting(), "Export did not start"); exportState_ = "running";
      event("export-start", {{"kind", exportKind_}});
    }
  }
  QJsonObject statistics() const {
    std::vector<double> early, late, earlyHandles;
    double maxPrivate = 0, maxHandles = 0, sx = 0, sy = 0, sxx = 0, sxy = 0, n = 0;
    for (const auto &s : samples_) {
      if (s.seconds < warmup_ || s.seconds > duration_) continue;
      if (s.seconds <= warmup_ + window_) { early.push_back(s.privateBytes); earlyHandles.push_back(s.handles); }
      if (s.seconds >= duration_ - window_) late.push_back(s.privateBytes);
      maxPrivate = std::max(maxPrivate, s.privateBytes); maxHandles = std::max(maxHandles, double(s.handles));
      const double x = s.seconds / 60., y = s.privateBytes / mib;
      sx += x; sy += y; sxx += x*x; sxy += x*y; ++n;
    }
    if (early.empty() || late.empty()) return {};
    const double base = median(early), tail = median(late);
    return {{"baseline_private_mib", base / mib}, {"tail_private_mib", tail / mib},
      {"tail_growth_mib", (tail-base) / mib}, {"peak_growth_mib", (maxPrivate-base) / mib},
      {"private_slope_mib_per_minute", n*sxx > sx*sx ? (n*sxy-sx*sy)/(n*sxx-sx*sx) : 0},
      {"handle_growth", maxHandles - median(earlyHandles)}};
  }
  QJsonObject summary(const QString &status, const QString &error = {}) const {
    return {{"status", status}, {"error", error}, {"elapsed_seconds", ms()/1000.}, {"requested_seconds", duration_},
      {"presentations", presentations_}, {"natural_wraps", wraps_}, {"seek_bursts", seeks_},
      {"seek_requests", seekRequests_}, {"max_seek_ms", maxSeekMs_}, {"pause_resumes", pauses_},
      {"loop_off_tails", loopStops_}, {"finite_three_pass_runs", finiteRuns_}, {"clears", clears_}, {"still_switches", switches_},
      {"same_session_exports", sharedExports_}, {"complete_exports", exports_}, {"cancelled_exports", cancels_},
      {"explicit_cancels", explicitCancels_}, {"clear_cancels", clearCancels_}, {"open_cancels", openCancels_},
      {"max_cancel_ms", maxCancelMs_}, {"verified_pngs", pngs_}, {"max_live_presentations", maxLive_},
      {"memory", statistics()}, {"sample_count", int(samples_.size())}, {"input_sha256", hashes_}};
  }
  void finish(const QString &error) {
    if (finished_) return;
    finished_ = true; tick_.stop(); result_ = error.isEmpty() ? 0 : 1;
    try { event(error.isEmpty() ? "passed" : "failed", {{"error", error}});
          json(root_ + "/summary.json", summary(error.isEmpty() ? "passed" : "failed", error)); }
    catch (...) { result_ = 1; }
    QCoreApplication::exit(result_);
  }
  void tick() {
    const double now = ms();
    if (now >= nextSample_) {
      const auto s = memory(now/1000.); samples_.push_back(s); nextSample_ = now + 1000;
      int alive = 0; for (const auto &frame : weak_) alive += !frame.expired();
      maxLive_ = std::max(maxLive_, alive); require(alive <= 3, "Old presentation objects accumulate");
      const auto line = QString("%1,%2,%3,%4,%5,%6,%7,%8,%9,%10,%11,%12,%13\n")
        .arg(s.seconds, 0, 'f', 3).arg(stopping_ ? "drain" : "run").arg(s.privateBytes, 0, 'f', 0)
        .arg(s.workingSet, 0, 'f', 0).arg(s.handles).arg(presentations_).arg(wraps_).arg(exports_).arg(cancels_)
        .arg(play_->frame()).arg(play_->playing()).arg(play_->busy()).arg(writer_->exportProgress()).toUtf8();
      require(samplesFile_.write(line) == line.size() && samplesFile_.flush(), "Cannot append memory sample");
      if (int(now/1000.) % 5 == 0) json(root_ + "/summary.json", summary("running"));
    }
    if (!stopping_ && now >= duration_ * 1000.) {
      stopping_ = true; if (play_->playing()) play_->togglePlay(); event("measurement-end");
    }
    if (!stopping_) {
      playActions(now);
      if (mode_.isEmpty() && play_->playing()) require(now - lastPresentation_ < 5000, "Natural playback stalled");
    }
    exportActions(now, stopping_);
    if (!stopping_ || (exportState_ != "idle" && exportState_ != "opening") || writer_->busy()) return;
    if (!drained_) {
      play_->clear(); writer_->clear(); drained_ = true; drainedAt_ = now; event("clear-both"); return;
    }
    if (now - drainedAt_ < 1000) return;
    for (const auto &frame : weak_) require(frame.expired(), "Presentation survives clear");
    for (auto it = hashes_.begin(); it != hashes_.end(); ++it)
      require(QString::fromLatin1(sha(it.key())) == it.value().toString(), "Soak modified an input");
    const auto stats = statistics();
    require(!stats.isEmpty(), "Memory measurement incomplete");
    require(stats["tail_growth_mib"].toDouble() <= 16 && stats["peak_growth_mib"].toDouble() <= 64,
            "Private memory growth exceeds fixed gates");
    if (duration_ >= 600) require(stats["private_slope_mib_per_minute"].toDouble() <= .5, "Private memory rises over time");
    require(stats["handle_growth"].toDouble() <= 16, "Process handles accumulate");
    require(presentations_ >= duration_ * 4 && wraps_ >= duration_ / 2, "Too little real playback for acceptance");
    require(exports_ >= std::max(1, duration_ / 90) && explicitCancels_ >= std::max(1, duration_ / 180) &&
            clearCancels_ >= std::max(1, duration_ / 180) && openCancels_ >= std::max(1, duration_ / 180),
            "Insufficient completed/cancelled streaming exports");
    require(pauses_ && seeks_ && loopStops_ && (duration_ < 100 || (finiteRuns_ && switches_ && sharedExports_)),
            "Control exercise coverage incomplete");
    event("inputs-unchanged-and-gates-passed"); finish({});
  }
  QString root_, infinite_, finite_, long_, still_, previousPath_, mode_, exportState_{"opening"}, batch_;
  int duration_{}, result_{1}, previousFrame_{-1}, action_{}, seekTarget_{}, finiteTails_{}, exportKind_{}, batchCount_{}, verified_{};
  int presentations_{}, wraps_{}, seeks_{}, seekRequests_{}, pauses_{}, loopStops_{}, finiteRuns_{}, clears_{}, switches_{};
  int sharedExports_{}, exports_{}, cancels_{}, explicitCancels_{}, clearCancels_{}, openCancels_{}, pngs_{}, maxLive_{};
  double warmup_{}, window_{}, nextSample_{}, nextAction_{}, nextExport_{}, modeStart_{}, pauseAt_{}, lastPresentation_{};
  double maxSeekMs_{}, maxCancelMs_{}, exportStarted_{}, cancelAt_{}, drainedAt_{};
  quint64 revision_{}, pauseRevision_{};
  bool finished_{}, stopping_{}, drained_{}, seekSeen_{}, cancelRequested_{};
  Clock::time_point origin_;
  std::unique_ptr<ImageSession> play_, writer_;
  QTimer tick_;
  QFile samplesFile_, events_;
  QJsonObject hashes_;
  std::array<QImage, 4> expected_;
  QStringList files_;
  std::deque<std::weak_ptr<const ImagePixels>> weak_;
  std::vector<Sample> samples_;
};
} // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QImageReader::setAllocationLimit(32); // Match the production host.
  try {
    require(argc == 3, "Usage: formaview_gif_stability_tests OUTPUT_DIRECTORY SECONDS");
    bool valid = false; const int seconds = QString::fromLocal8Bit(argv[2]).toInt(&valid);
    require(valid && seconds >= 60 && seconds <= 7200, "Soak duration must be 60..7200 seconds");
    Soak soak(QDir(QString::fromLocal8Bit(argv[1])).absolutePath(), seconds);
    app.exec(); return soak.result();
  } catch (const std::exception &error) { qCritical("%s", error.what()); return 1; }
}
