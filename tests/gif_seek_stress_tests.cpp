// Opt-in deep seek/cancel probe against the soak's synthetic 512-frame GIF.
#include "image_session.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <vector>
#include <windows.h>
#include <psapi.h>

namespace {
using Clock = std::chrono::steady_clock;
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
double ms(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
void waitFor(const std::function<bool()> &predicate, int timeout = 5000) {
  if (predicate()) return;
  QEventLoop loop; QTimer poll, deadline;
  poll.setInterval(1); deadline.setSingleShot(true);
  QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (predicate()) loop.quit(); });
  QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
  poll.start(); deadline.start(timeout); loop.exec();
  require(predicate(), "Deep seek/cancel timed out");
}
void delay(int milliseconds) {
  QEventLoop loop; QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit); loop.exec();
}
QByteArray sha(const QString &path) {
  QFile file(path); require(file.open(QIODevice::ReadOnly), "Cannot hash deep-seek fixture");
  QCryptographicHash hash(QCryptographicHash::Sha256); require(hash.addData(&file), "Cannot stream hash");
  return hash.result().toHex();
}
struct Memory { double bytes{}; int handles{}; };
Memory memory() {
  PROCESS_MEMORY_COUNTERS_EX value{}; value.cb = sizeof(value); DWORD handles{};
  require(K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&value),
          sizeof(value)) && GetProcessHandleCount(GetCurrentProcess(), &handles), "Cannot sample process");
  return {double(value.PrivateUsage), int(handles)};
}
QColor expected(int frame, int x, int y) {
  const int phase = frame % 4;
  if (x >= 32 && x < 64 && y >= 32 && y < 64)
    return phase == 1 ? QColor(Qt::green) : phase >= 2 ? QColor(Qt::blue) : QColor(Qt::red);
  if (phase == 2 && x >= 64 && x < 96 && y >= 64 && y < 96) return QColor(Qt::green);
  if (phase == 3 && x >= 96 && x < 128 && y >= 96 && y < 128) return QColor(Qt::blue);
  return QColor(Qt::red);
}
void check(ImageSession &session, int frame) {
  require(session.error().isEmpty() && session.available() && session.frame() == frame, "Wrong deep-seek publication");
  const auto p = session.pixels();
  require(p->source.data_window.width == 512 && p->source.data_window.height == 384, "Wrong deep-seek dimensions");
  for (int coordinate : {0, 40, 72, 104, 383}) {
    const auto i = std::size_t(coordinate * 512 + coordinate) * 4;
    const auto actual = QColor::fromRgbF(p->rgba[i], p->rgba[i+1], p->rgba[i+2], p->rgba[i+3]);
    require(actual == expected(frame, coordinate, coordinate), "Deep-seek composed pixel mismatch");
  }
}
QJsonObject run(const QString &source, const QString &output) {
  QTemporaryDir temporary(QFileInfo(output).absolutePath() + "/gif-seek-XXXXXX");
  require(temporary.isValid(), "Cannot allocate isolated seek outputs");
  const auto hash = sha(source); const auto origin = Clock::now();
  double maxSeek = 0, maxCancel = 0, maxDestructor = 0, peak = 0;
  int seeks = 0, burstRequests = 0, activeClears = 0, retained = 0, pngs = 0;
  std::vector<std::weak_ptr<const ImagePixels>> frames;
  ImageSession session; session.open(source); waitFor([&] { return !session.busy(); });
  require(session.frameCount() == 512, "Deep-seek probe needs the soak's 512-frame fixture");
  const auto seek = [&](int target) {
    frames.push_back(session.pixels()); const auto start = Clock::now(); session.seekFrame(target);
    waitFor([&] { return !session.busy(); }); check(session, target);
    maxSeek = std::max(maxSeek, ms(start)); peak = std::max(peak, memory().bytes); ++seeks;
  };
  seek(511); seek(0); seek(255); seek(3); // Warm both decoder restart and forward paths.
  const auto base = memory(); peak = base.bytes;
  for (int round = 0; round < 16; ++round) {
    for (int target : {511, 0, 255, 3}) seek(target);
    peak = std::max(peak, memory().bytes);
  }
  for (int round = 0; round < 8; ++round) {
    for (int i = 0; i < 64; ++i) session.seekFrame((i * 97) % 512);
    session.seekFrame(511); burstRequests += 65;
    waitFor([&] { return !session.busy(); }); check(session, 511);
    seek(0);
  }
  // Queue deep seeks behind an active streaming export, then cancel export.
  // The latest seek must still publish, with the partial PNGs remaining valid.
  const auto directory = temporary.filePath("partial");
  session.exportFrames(directory, "frame-", "skip");
  waitFor([&] { return session.exportProgress() >= 8 || !session.exporting(); });
  require(session.exporting(), "Export finished before contention/cancel was exercised");
  session.seekFrame(511); session.seekFrame(0); session.seekFrame(511);
  const auto cancelled = Clock::now(); session.cancelExport();
  waitFor([&] { return !session.exporting(); }, 2000); maxCancel = ms(cancelled);
  waitFor([&] { return !session.busy(); }); check(session, 511);
  require(session.exportError().contains("cancel", Qt::CaseInsensitive) && session.exportProgress() < 512,
          "Contended export cancellation was not reported");
  const auto outputs = QDir(directory).entryList({"*.png"}, QDir::Files, QDir::Name);
  require(!outputs.empty() && outputs.size() < 512, "No partial export output");
  for (const auto &name : outputs) {
    const QImage image(directory + '/' + name);
    require(!image.isNull() && image.size() == QSize(512, 384), "Cancelled export left unreadable PNG"); ++pngs;
  }
  require(QDir(directory).entryList({".formaview-*"}, QDir::Files | QDir::Hidden).isEmpty(), "Transaction file leaked");
  // Cancel a decode after the event loop has yielded to the worker, then reopen.
  for (int round = 0; round < 8; ++round) {
    seek(0); session.seekFrame(511); delay(1);
    require(session.busy(), "Deep decode completed before active clear was exercised");
    session.clear(); delay(30);
    require(!session.available() && !session.busy() && !session.playing(), "Cancelled frame appeared after clear");
    session.open(source); waitFor([&] { return !session.busy(); }); check(session, 0); ++activeClears;
    peak = std::max(peak, memory().bytes);
  }
  // Destruction must join a cancelled decoder and release its captures/lease.
  for (int round = 0; round < 8; ++round) {
    auto child = std::make_unique<ImageSession>(); child->open(source);
    waitFor([&] { return !child->busy(); }); child->seekFrame(511); delay(1);
    peak = std::max(peak, memory().bytes);
    const auto start = Clock::now(); child.reset(); maxDestructor = std::max(maxDestructor, ms(start));
  }
  const auto tail = memory();
  peak = std::max(peak, tail.bytes);
  for (const auto &frame : frames) retained += !frame.expired();
  require(retained <= 1, "Deep seeks retain old presentation frames");
  require(tail.bytes - base.bytes <= 32 * 1024 * 1024 && peak - base.bytes <= 64 * 1024 * 1024,
          "Deep seek memory grows beyond fixed bounds");
  require(tail.handles <= base.handles + 4 && maxDestructor <= 2000, "Deep seek teardown leaks/stalls");
  session.clear(); delay(100);
  for (const auto &frame : frames) require(frame.expired(), "Deep-seek presentation survived clear");
  require(sha(source) == hash, "Deep seek modified its source");
  return {{"status", "passed"}, {"elapsed_ms", ms(origin)}, {"seeks", seeks}, {"burst_requests", burstRequests},
    {"max_seek_ms", maxSeek}, {"max_cancel_ms", maxCancel}, {"active_clear_reopens", activeClears},
    {"active_destructors", 8}, {"max_destructor_ms", maxDestructor}, {"partial_pngs", pngs},
    {"private_growth_mib", (tail.bytes-base.bytes)/(1024.*1024.)}, {"handle_growth", tail.handles-base.handles},
    {"max_measured_private_growth_mib", (peak-base.bytes)/(1024.*1024.)}, {"source_sha256", QString::fromLatin1(hash)}};
}
} // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv); QImageReader::setAllocationLimit(32);
  if (argc != 3) return 2;
  const auto source = QString::fromLocal8Bit(argv[1]), output = QString::fromLocal8Bit(argv[2]);
  QJsonObject result;
  try { result = run(source, output); }
  catch (const std::exception &error) { result = {{"status", "failed"}, {"error", error.what()}}; }
  QSaveFile file(output); const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Indented);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) return 2;
  return result["status"] == "passed" ? 0 : 1;
}
