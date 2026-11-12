// QCoreApplication only: CPU/session measurements are not GPU or UI paint
// timings.
#include "image_session.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <windows.h>

#include <psapi.h>

using namespace formaview;
using namespace formaview::imaging;
namespace {
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
double ms(const QElapsedTimer &timer) { return timer.nsecsElapsed() / 1e6; }
QByteArray digest(const void *data, qsizetype bytes) {
  return QCryptographicHash::hash(
             QByteArrayView(static_cast<const char *>(data), bytes),
             QCryptographicHash::Sha256)
      .toHex();
}
QJsonObject memory() {
  PROCESS_MEMORY_COUNTERS_EX m{};
  m.cb = sizeof(m);
  require(GetProcessMemoryInfo(GetCurrentProcess(),
                               reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&m),
                               sizeof(m)),
          "Read process memory");
  return {{"rss", static_cast<qint64>(m.WorkingSetSize)},
          {"private", static_cast<qint64>(m.PrivateUsage)}};
}
QByteArray normalizedHash(const ImagePixels &pixels) {
  if (!pixels.byteUpload())
    return digest(pixels.rgba.data(),
                  static_cast<qsizetype>(pixels.uploadBytes()));
  // Evidence only, outside measured preparation/readiness; same canonical
  // float representation as the baseline. Never retained by the session cache.
  std::vector<float> values(pixels.rgba8.size());
  for (std::size_t i = 0; i < values.size(); ++i)
    values[i] = static_cast<float>(
        std::to_integer<unsigned char>(pixels.rgba8[i]) / 255.);
  return digest(values.data(),
                static_cast<qsizetype>(values.size() * sizeof(float)));
}
void save(const QString &path, const QJsonObject &object) {
  QFile out(path);
  require(out.open(QIODevice::WriteOnly), "Write evidence");
  out.write(QJsonDocument(object).toJson());
}
template <class T>
ImageChannel channel(std::string name, ScalarType scalar,
                     SampleEncoding encoding, const std::vector<T> &values) {
  Buffer buffer;
  buffer.scalar = scalar;
  buffer.element_count = values.size();
  buffer.bytes.resize(values.size() * sizeof(T));
  std::memcpy(buffer.bytes.data(), values.data(), buffer.bytes.size());
  return {std::move(name), std::move(buffer), encoding};
}
ImagePixels fixture() {
  ImagePixels p;
  p.source.data_window = {0, 0, 256, 1};
  std::vector<unsigned char> values(256);
  for (int i = 0; i < 256; ++i)
    values[static_cast<std::size_t>(i)] = static_cast<unsigned char>(i);
  for (const auto *name : {"R", "G", "B", "A"}) {
    p.source.channels.push_back(
        channel(name, ScalarType::UInt8, SampleEncoding::UNorm, values));
    std::rotate(values.begin(), values.begin() + 43, values.end());
  }
  p.mapping = {0, 1, 2, 3};
  p.bytes = 1024;
  return p;
}
void checkValues(ImagePixels &p, bool byteUpload) {
  const auto original = p.source.channels;
  const auto sourceBytes = p.bytes;
  p.prepareUpload({});
  require(p.byteUpload() == byteUpload, "Unexpected upload representation");
  require(p.bytes == sourceBytes + p.uploadBytes(),
          "Preview memory accounting");
  require((p.byteUpload() ? p.rgba.empty() : p.rgba8.empty()),
          "One upload representation only");
  for (std::size_t c = 0; c < original.size(); ++c)
    require(p.source.channels[c].samples.bytes == original[c].samples.bytes,
            "Source must remain unchanged");
  for (std::size_t i = 0;
       i < p.source.data_window.width * p.source.data_window.height; ++i)
    for (std::size_t c = 0; c < 4; ++c) {
      const auto expected =
          p.mapping[c] >= 0
              ? static_cast<float>(interpreted_value(
                    original[static_cast<std::size_t>(p.mapping[c])], i))
          : c == 3 ? 1.F
                   : 0.F;
      const auto actual =
          p.byteUpload()
              ? static_cast<float>(
                    std::to_integer<unsigned char>(p.rgba8[i * 4 + c]) / 255.)
              : p.rgba[i * 4 + c];
      require((std::isnan(expected) && std::isnan(actual)) ||
                  (actual == expected &&
                   std::signbit(actual) == std::signbit(expected)),
              "Upload values differ from source");
    }
}
void expectFailure(ImagePixels &p, ErrorCode code, std::stop_token stop = {}) {
  try {
    p.prepareUpload(stop);
  } catch (const skeletal::Failure &e) {
    require(e.error.code == code, "Unexpected preparation error");
    return;
  }
  throw std::runtime_error("Expected preparation failure");
}
void writeTga(const QString &path, unsigned char red) {
  QByteArray header(18, '\0');
  header[2] = 2;
  header[12] = 0;
  header[13] = 15; // 3840
  header[14] = 112;
  header[15] = 8; // 2160
  header[16] = 32;
  header[17] = 40; // top origin, 8-bit alpha
  QByteArray pixels(3840 * 2160 * 4, '\0');
  for (qsizetype i = 0; i < pixels.size(); i += 4) {
    pixels[i] = 17;
    pixels[i + 1] = 33;
    pixels[i + 2] = static_cast<char>(red);
    pixels[i + 3] = 127;
  }
  QFile out(path);
  require(out.open(QIODevice::WriteOnly), "Write test TGA");
  require(out.write(header) == header.size() &&
              out.write(pixels) == pixels.size(),
          "Complete test TGA");
}
void waitFor(ImageSession &session) {
  QElapsedTimer timer;
  timer.start();
  while (session.busy() && timer.elapsed() < 60000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
  }
  require(!session.busy(), "Session timed out");
}
QJsonObject regression(const QString &directory) {
  int cases = 0;
  for (const auto mapping : {std::array<int, 4>{0, 1, 2, 3},
                             {2, 0, 1, -1},
                             {0, 0, 0, 3},
                             {-1, 1, -1, 3}}) {
    auto p = fixture();
    p.mapping = mapping;
    checkValues(p, true);
    ++cases;
  }
  // Alpha association is metadata: do not unpremultiply or discard RGB where
  // A=0.
  for (auto alpha :
       {AlphaMode::Absent, AlphaMode::Straight, AlphaMode::Premultiplied}) {
    auto p = fixture();
    p.info["alpha"] = static_cast<int>(alpha);
    if (alpha == AlphaMode::Absent)
      p.mapping[3] = -1;
    checkValues(p, true);
    require(p.info["alpha"].toInt() == static_cast<int>(alpha),
            "Alpha metadata retained");
    ++cases;
  }
  for (auto scalar : {ScalarType::UInt16, ScalarType::UInt32, ScalarType::Int16,
                      ScalarType::Int32, ScalarType::Half, ScalarType::Float32,
                      ScalarType::Float64}) {
    auto p = fixture();
    p.mapping = {0, -1, -1, -1};
    p.source.channels.clear();
    if (scalar == ScalarType::UInt16)
      p.source.channels.push_back(
          channel("Y", scalar, SampleEncoding::UNorm,
                  std::vector<std::uint16_t>(256, 32769)));
    if (scalar == ScalarType::UInt32)
      p.source.channels.push_back(
          channel("Y", scalar, SampleEncoding::UNorm,
                  std::vector<std::uint32_t>(256, 2147483649U)));
    if (scalar == ScalarType::Int16)
      p.source.channels.push_back(
          channel("Y", scalar, SampleEncoding::SNorm,
                  std::vector<std::int16_t>(256, -32768)));
    if (scalar == ScalarType::Int32)
      p.source.channels.push_back(
          channel("Y", scalar, SampleEncoding::SNorm,
                  std::vector<std::int32_t>(
                      256, std::numeric_limits<std::int32_t>::min())));
    if (scalar == ScalarType::Half)
      p.source.channels.push_back(
          channel("Y", scalar, SampleEncoding::Numeric,
                  std::vector<std::uint16_t>(
                      256, 0x4200))); // binary16 3.0, outside UNorm
    if (scalar == ScalarType::Float32) {
      std::vector<float> v(256, 4.25F);
      v[0] = -0.F;
      v[1] = -3.F;
      v[2] = std::numeric_limits<float>::infinity();
      v[3] = std::numeric_limits<float>::quiet_NaN();
      p.source.channels.push_back(
          channel("Y", scalar, SampleEncoding::Numeric, v));
    }
    if (scalar == ScalarType::Float64)
      p.source.channels.push_back(channel("Y", scalar, SampleEncoding::Numeric,
                                          std::vector<double>(256, 1.125)));
    checkValues(p, false);
    ++cases;
  }
  {
    auto p = fixture();
    p.source.channels[0].encoding = SampleEncoding::Numeric;
    checkValues(p, false);
    ++cases;
  }
  {
    auto p = fixture();
    p.source.channels[0].encoding = SampleEncoding::SNorm;
    expectFailure(p, ErrorCode::InvalidArgument);
    ++cases;
  }
  for (int index : {-2, 4}) {
    auto p = fixture();
    p.mapping[0] = index;
    expectFailure(p, ErrorCode::InvalidArgument);
    ++cases;
  }
  {
    auto p = fixture();
    p.source.channels[0].samples.bytes.pop_back();
    expectFailure(p, ErrorCode::InvalidArgument);
    ++cases;
  }
  {
    auto p = fixture();
    p.source.channels[0].samples.element_count = 255;
    expectFailure(p, ErrorCode::InvalidArgument);
    ++cases;
  }
  {
    auto p = fixture();
    p.source.data_window.width = std::numeric_limits<std::uint64_t>::max();
    p.source.data_window.height = 2;
    expectFailure(p, ErrorCode::InvalidArgument);
    ++cases;
  }
  {
    auto p = fixture();
    p.source.data_window.width = 0;
    expectFailure(p, ErrorCode::InvalidArgument);
    ++cases;
  }
  {
    auto p = fixture();
    p.bytes = 512ULL * 1024 * 1024 - 1024;
    p.prepareUpload({});
    require(p.bytes == 512ULL * 1024 * 1024, "Exact byte budget boundary");
    ++cases;
  }
  {
    auto p = fixture();
    p.bytes = 512ULL * 1024 * 1024 - 1023;
    expectFailure(p, ErrorCode::ResourceLimit);
    ++cases;
  }
  {
    auto p = fixture();
    p.bytes = 512ULL * 1024 * 1024 + 1;
    expectFailure(p, ErrorCode::ResourceLimit);
    ++cases;
  }
  {
    auto p = fixture();
    std::stop_source stop;
    stop.request_stop();
    expectFailure(p, ErrorCode::Cancelled, stop.get_token());
    require(p.uploadBytes() == 0, "Cancel before allocation");
    ++cases;
  }
  {
    auto p = fixture();
    p.prepareUpload({});
    expectFailure(p, ErrorCode::InvalidArgument);
    ++cases;
  }
  require(QDir().mkpath(directory), "Fixture directory");
  QTemporaryDir temp(QDir(directory).filePath("session-XXXXXX"));
  require(temp.isValid(), "Isolated test fixture");
  const auto a = temp.filePath("A.tga"), b = temp.filePath("B.tga");
  writeTga(a, 99);
  writeTga(b, 201);
  ImageSession session;
  const auto open = [&](const QString &path) {
    session.open(path);
    waitFor(session);
    require(session.error().isEmpty() && session.path() == path,
            "Successful session open");
  };
  open(a);
  std::weak_ptr<const ImagePixels> first = session.pixels();
  const auto oldHash = session.info()["sha256"].toString();
  open(b);
  open(a);
  require(first.lock() == session.pixels(),
          "4K A/B/A must reuse cached source");
  ++cases;
  session.mapChannels({"B", "R", "G", ""});
  waitFor(session);
  require(session.error().isEmpty() &&
              session.pixels()->mapping == std::array<int, 4>{2, 0, 1, -1},
          "Remapping is part of cache key");
  ++cases;
  const auto oldTime = QFileInfo(a).lastModified();
  writeTga(a, 73);
  {
    QFile file(a);
    require(file.open(QIODevice::ReadWrite), "Open modified fixture");
    require(file.setFileTime(oldTime, QFileDevice::FileModificationTime),
            "Restore mtime for identity test");
  }
  open(a);
  require(session.info()["sha256"].toString() != oldHash,
          "Same-size same-mtime changes invalidate cache");
  require(interpreted_value(session.pixels()->source.channels[0], 0) ==
              73 / 255.,
          "New source contents visible");
  ++cases;
  session.open(a);
  session.open(b);
  waitFor(session);
  require(session.error().isEmpty() && session.path() == b,
          "Latest generation wins");
  ++cases;
  session.open(temp.filePath("missing.tga"));
  waitFor(session);
  require(!session.error().isEmpty(), "Unavailable input error");
  ++cases;
  open(a);
  first = session.pixels();
  for (const auto &mapping :
       {QStringList{"R", "B", "G", "A"}, QStringList{"G", "B", "R", "A"},
        QStringList{"B", "G", "R", "A"}, QStringList{"R", "R", "R", "A"}}) {
    session.mapChannels(mapping);
    waitFor(session);
    require(session.error().isEmpty(), "Mapped preview completes");
  }
  require(first.expired(),
          "LRU evicts old 4K buffers at the unchanged 256 MiB limit");
  ++cases;
  std::weak_ptr<const ImagePixels> last = session.pixels();
  session.clear();
  require(!session.available() && !session.busy(), "Clear session");
  QElapsedTimer cleanup;
  cleanup.start();
  while (!last.expired() && cleanup.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
  }
  require(last.expired(), "Clear releases retained upload/source storage");
  ++cases;
  return {{"passed", true},
          {"cases", cases},
          {"gui_paint_verified", false},
          {"gpu_upload_verified", false}};
}
QJsonObject benchmark(const QString &directory) {
  const auto files =
      QDir(directory).entryInfoList({"*.tga"}, QDir::Files, QDir::Name);
  require(files.size() >= 2, "At least two TGA samples required");
  QJsonArray stages, requests;
  // An isolated call chain measures decoding/preparation without inventing a
  // GPU timestamp. Session end-to-end measurements below use the actual host.
  for (const auto &file : files) {
    QElapsedTimer clock;
    clock.start();
    auto document = ImageDocument::open(
        std::filesystem::path(file.absoluteFilePath().toStdWString()),
        {512ULL * 1024 * 1024, 512ULL * 1024 * 1024, 16384, 65536, 256}, {});
    const double opened = ms(clock);
    clock.restart();
    ImagePixels pixels;
    pixels.source = document.decode(0, {});
    const double decoded = ms(clock);
    for (std::size_t c = 0; c < pixels.source.channels.size(); ++c) {
      pixels.bytes += pixels.source.channels[c].samples.bytes.size();
      for (int n = 0; n != 4; ++n)
        if (pixels.source.channels[c].name ==
            std::array<std::string, 4>{"R", "G", "B", "A"}[n])
          pixels.mapping[n] = static_cast<int>(c);
    }
    clock.restart();
    pixels.prepareUpload({});
    const double prepared = ms(clock);
    stages.append(QJsonObject{
        {"file", file.fileName()},
        {"document_open_ms", opened},
        {"decode_ms", decoded},
        {"prepare_upload_ms", prepared},
        {"upload_bytes", static_cast<qint64>(pixels.uploadBytes())},
        {"retained_bytes", static_cast<qint64>(pixels.bytes)},
        {"width", static_cast<qint64>(pixels.source.data_window.width)},
        {"height", static_cast<qint64>(pixels.source.data_window.height)}});
  }
  ImageSession session;
  QElapsedTimer clock, heartbeatClock;
  double busySignal = -1, maxGap = 0;
  QObject::connect(&session, &ImageSession::changed, &session, [&] {
    if (session.busy() && busySignal < 0)
      busySignal = ms(clock);
  });
  QTimer heartbeat;
  heartbeat.setInterval(1);
  QObject::connect(&heartbeat, &QTimer::timeout, &session, [&] {
    maxGap = std::max(maxGap, ms(heartbeatClock));
    heartbeatClock.restart();
  });
  std::map<QString, std::weak_ptr<const ImagePixels>> previous;
  const auto load = [&](int index, const QString &label) {
    const auto path = files[index].absoluteFilePath();
    busySignal = -1;
    maxGap = 0;
    clock.start();
    heartbeatClock.start();
    heartbeat.start();
    session.open(path);
    const auto callMs = ms(clock);
    while (session.busy() && clock.elapsed() < 60000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
      QThread::msleep(1);
    }
    const auto ready = ms(clock);
    heartbeat.stop();
    require(!session.busy() && session.error().isEmpty() &&
                session.path() == path,
            "ImageSession open must complete successfully");
    const auto pixels = session.pixels();
    const bool hit = previous[path].lock() == pixels;
    previous[path] = pixels;
    const auto hash = normalizedHash(*pixels);
    requests.append(
        QJsonObject{{"label", label},
                    {"file", files[index].fileName()},
                    {"open_call_ms", callMs},
                    {"busy_signal_ms", busySignal},
                    {"ready_signal_ms", ready},
                    {"event_loop_max_gap_ms", maxGap},
                    {"cache_hit", hit},
                    {"source_sha256", pixels->info.value("sha256").toString()},
                    {"normalized_upload_sha256", QString::fromLatin1(hash)},
                    {"retained_bytes", static_cast<qint64>(pixels->bytes)},
                    {"memory", memory()}});
    std::cout << label.toStdString() << " ready_ms=" << ready << " hit=" << hit
              << std::endl;
  };
  load(0, "session-cold-A");
  load(0, "hot-A");
  load(1, "switch-B");
  load(0, "return-A");
  for (int n = 0; n != 3; ++n) {
    load(1, "repeat-B");
    load(0, "repeat-A");
  }
  for (int n = 0; n < files.size(); ++n)
    load(n, "directory-forward");
  for (int n = static_cast<int>(files.size()) - 1; n >= 0; --n)
    load(n, "directory-backward");
  session.clear();
  return {{"stages", stages},
          {"requests", requests},
          {"gui_paint_verified", false},
          {"gpu_upload_verified", false},
          {"cache_conditions",
           "CPU/session cold means a new session; OS cache is not cleared. "
           "Stage measurements precede session measurements."}};
}
} // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    require(argc == 4 && (app.arguments()[1] == "--benchmark" ||
                          app.arguments()[1] == "--regression"),
            "Usage: --benchmark INPUT_DIRECTORY OUTPUT_JSON or --regression "
            "FIXTURE_DIRECTORY OUTPUT_JSON");
    const auto result = app.arguments()[1] == "--benchmark"
                            ? benchmark(app.arguments()[2])
                            : regression(app.arguments()[2]);
    save(app.arguments()[3], result);
    std::cout << "Passed" << std::endl;
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
