// QCoreApplication only: CPU/session measurements are not GPU or UI paint
// timings.
#include "formaview/image_preview.hpp"
#include "image_session.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QImageReader>
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
int previewMatrix(const QString &root, const QString &output) {
  QJsonArray cases;
  QDirIterator files(root, QDir::Files, QDirIterator::Subdirectories);
  while (files.hasNext()) {
    const auto path = files.next();
    const auto suffix = QFileInfo(path).suffix().toLower();
    if (!QStringList{"png", "jpg", "jpeg", "jpe", "jfif", "bmp", "dib", "ico",
                     "tga", "hdr", "rgbe", "tif", "tiff", "webp", "dds", "exr"}
             .contains(suffix))
      continue;
    QJsonObject result{{"file", path}};
    try {
      auto reference =
          ImageDocument::open(std::filesystem::path(path.toStdWString()));
      auto preview =
          ImageDocument::open(std::filesystem::path(path.toStdWString()),
                              {512ULL * 1024 * 1024, 512ULL * 1024 * 1024,
                               INT_MAX, 65536, 256, true});
      require(reference.description().subresources.size() ==
                  preview.description().subresources.size(),
              "Preview catalog size");
      int regions = 0;
      for (std::size_t entry = 0;
           entry < reference.description().subresources.size(); ++entry) {
        const auto aSemantics = reference.semantics(entry),
                   bSemantics = preview.semantics(entry);
        require(aSemantics.alpha == bSemantics.alpha &&
                    aSemantics.meaning == bSemantics.meaning &&
                    aSemantics.colour_space == bSemantics.colour_space &&
                    aSemantics.icc_profile_sha256 ==
                        bSemantics.icc_profile_sha256,
                "Preview source semantics");
        const auto full = reference.decode(entry);
        const auto w = full.data_window.width, h = full.data_window.height;
        const std::vector<PreviewRequest> requests{
            {0, 0, 1, 1, 1, 1},
            {w - 1, h - 1, 1, 1, 1, 1},
            {w / 3, h / 3, std::min<std::uint64_t>(w - w / 3, 17),
             std::min<std::uint64_t>(h - h / 3, 11),
             std::min<std::uint64_t>(w - w / 3, 17),
             std::min<std::uint64_t>(h - h / 3, 11)},
            {0, 0, w, h, std::min<std::uint64_t>(w, 9),
             std::min<std::uint64_t>(h, 7)}};
        for (const auto &request : requests) {
          const auto patch = preview.decode_preview(entry, request);
          require(patch.channels.size() == full.channels.size(),
                  "Preview channel count");
          require(patch.mip == full.mip && patch.part == full.part &&
                      patch.array_layer == full.array_layer &&
                      patch.cube_face == full.cube_face &&
                      patch.depth_slice == full.depth_slice,
                  "Preview resource identity");
          for (std::size_t c = 0; c < full.channels.size(); ++c) {
            const auto &a = full.channels[c], &b = patch.channels[c];
            require(a.name == b.name && a.encoding == b.encoding &&
                        a.samples.scalar == b.samples.scalar,
                    "Preview native channel schema");
            // JPEG/WebP use codec reconstruction filters for reduced
            // previews; exact comparisons apply to native-sized regions.
            if ((QStringList{"jpg", "jpeg", "jpe", "jfif", "webp"}.contains(
                    suffix)) &&
                (request.width != request.output_width ||
                 request.height != request.output_height))
              continue;
            const auto component = preview_component(a.samples.scalar);
            for (std::uint64_t y = 0; y < request.output_height; ++y)
              for (std::uint64_t x = 0; x < request.output_width; ++x)
                if (std::memcmp(a.samples.bytes.data() +
                                    (request.sy(y) * w + request.sx(x)) *
                                        component,
                                b.samples.bytes.data() +
                                    (y * request.output_width + x) * component,
                                component))
                  throw std::runtime_error(
                      "Sample mismatch entry=" + std::to_string(entry) +
                      " channel=" + a.name +
                      " x=" + std::to_string(request.sx(x)) +
                      " y=" + std::to_string(request.sy(y)));
          }
          ++regions;
        }
      }
      std::stop_source cancel;
      cancel.request_stop();
      bool cancelled = false;
      try {
        static_cast<void>(
            preview.decode_preview(0, {0, 0, 1, 1, 1, 1}, cancel.get_token()));
      } catch (const skeletal::Failure &e) {
        cancelled = e.error.code == ErrorCode::Cancelled;
      }
      require(cancelled, "Preview cancellation");
      result["regions"] = regions;
      result["passed"] = true;
    } catch (const std::exception &e) {
      // A malformed/unsupported full decode must also be rejected by
      // preview open/decode; never count it as a successful image case.
      bool referenceRejected = false, previewRejected = false;
      try {
        auto d =
            ImageDocument::open(std::filesystem::path(path.toStdWString()));
        for (std::size_t n = 0; n < d.description().subresources.size(); ++n)
          static_cast<void>(d.decode(n));
      } catch (...) {
        referenceRejected = true;
      }
      if (referenceRejected) {
        try {
          auto d =
              ImageDocument::open(std::filesystem::path(path.toStdWString()),
                                  {512ULL * 1024 * 1024, 512ULL * 1024 * 1024,
                                   INT_MAX, 65536, 256, true});
          static_cast<void>(d.decode_preview(0, {0, 0, 1, 1, 1, 1}));
        } catch (...) {
          previewRejected = true;
        }
      }
      result["passed"] = referenceRejected && previewRejected;
      result["expectedRejection"] = referenceRejected;
      result["error"] = e.what();
    }
    cases.append(result);
  }
  save(output, QJsonObject{{"cases", cases}});
  int failures = 0;
  for (const auto &c : cases)
    if (!c.toObject()["passed"].toBool()) {
      ++failures;
      std::cerr << c.toObject()["file"].toString().toStdString() << ": "
                << c.toObject()["error"].toString().toStdString() << '\n';
    }
  std::cout << cases.size() << " files, " << failures << " failures\n";
  return failures ? 1 : 0;
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
  while ((session.busy() || session.refining()) && timer.elapsed() < 60000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
  }
  require(!session.busy() && !session.refining(), "Session timed out");
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
  // Smaller previews correctly retain more entries within the same byte
  // budget. Exercise the entry bound as well instead of assuming 4K buffers.
  for (const auto &r : QStringList{"R","G","B"})
    for (const auto &g : QStringList{"R","G","B"}) {
      session.mapChannels({r,g,"B","A"});
      waitFor(session);
      require(session.error().isEmpty(), "Bounded mapped preview completes");
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
    if (argc == 4 && app.arguments()[1] == "--gif-preview-samples") {
      QJsonArray frames;
      for (const auto &file :
           QDir(app.arguments()[2]).entryInfoList({"*.gif"}, QDir::Files)) {
        ImageSession session;
        session.open(file.absoluteFilePath());
        waitFor(session);
        require(session.error().isEmpty(), qPrintable(session.error()));
        const int count = session.frameCount();
        for (int frame = 0; frame < count; ++frame) {
          session.seekFrame(frame);
          waitFor(session);
          for (int view = 0; view < 2; ++view) {
            const auto before = session.pixels();
            const auto full = before->previewFull;
            if (view) {
              session.viewport(
                  QRect(full.width() / 3, full.height() / 3,
                        std::min(47, full.width() - full.width() / 3),
                        std::min(31, full.height() - full.height() / 3)),
                  QSize(47, 31));
              waitFor(session);
            }
            require(session.error().isEmpty(), qPrintable(session.error()));
            const auto p = session.pixels();
            require(session.overview() && session.overview()->info["frame"] == p->info["frame"],
                    "GIF overview must belong to the current frame");
            require(session.overview()->previewRegion == QRect(QPoint(),full), "GIF full overview");
            const auto w = p->source.data_window.width,
                       h = p->source.data_window.height;
            QByteArray rgba(qsizetype(w * h * 4), Qt::Uninitialized);
            for (std::uint64_t n = 0; n < w * h; ++n)
              for (std::size_t c = 0; c < 4; ++c)
                rgba[qsizetype(n * 4 + c)] =
                    char(std::to_integer<unsigned char>(
                        p->source.channels[c].samples.bytes[n]));
            const auto r = p->previewRegion;
            frames.append(QJsonObject{
                {"file", file.absoluteFilePath()},
                {"frame", frame},
                {"x", r.x()},
                {"y", r.y()},
                {"width", r.width()},
                {"height", r.height()},
                {"outputWidth", int(w)},
                {"outputHeight", int(h)},
                {"sha256",
                 QString::fromLatin1(digest(rgba.constData(), rgba.size()))},
                {"retainedBytes", qint64(p->bytes)},
                {"memory", memory()}});
          }
          // Reset the whole-canvas request before the next frame.
          session.viewport(QRect(QPoint(), session.pixels()->previewFull),
                           QSize(2048, 2048));
          waitFor(session);
        }
      }
      require(!frames.isEmpty(), "No GIF fixtures");
      save(app.arguments()[3], QJsonObject{{"frames", frames}});
      return 0;
    }
    if (argc == 3 && app.arguments()[1] == "--make-preview-fixtures") {
      const QDir root(app.arguments()[2]);
      require(!root.exists(), "Use a fresh preview fixture directory");
      require(QDir().mkpath(root.absolutePath()), "Cannot create fixtures");
      ImageSubresource source;
      source.data_window = source.display_window = {0, 0, 4096, 2304};
      for (int c = 0; c < 4; ++c) {
        Buffer b;
        b.scalar = ScalarType::UInt8;
        b.element_count = 4096ULL * 2304;
        b.bytes.resize(b.element_count);
        for (std::size_t y = 0; y < 2304; ++y)
          for (std::size_t x = 0; x < 4096; ++x)
            b.bytes[y * 4096 + x] =
                std::byte((x * (c + 1) + y * (c + 3) + c * 41) % 256);
        source.channels.push_back({std::array{"R", "G", "B", "A"}[c],
                                   std::move(b), SampleEncoding::UNorm});
      }
      const auto write = [&](const QString &name,
                             const std::vector<std::byte> &bytes) {
        QFile file(root.filePath(name));
        require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly),
                "Cannot write fixture");
        require(file.write(reinterpret_cast<const char *>(bytes.data()),
                           qsizetype(bytes.size())) == qsizetype(bytes.size()),
                "Short fixture write");
      };
      RasterEncodeOptions raster;
      raster.target = RasterTarget::Png;
      raster.alpha = AlphaMode::Straight;
      write("large-rgba.png", encode_raster(source, raster));
      auto rgb8 = source;
      rgb8.channels.pop_back();
      raster.target = RasterTarget::Bmp;
      raster.alpha = AlphaMode::Absent;
      write("large-rgb.bmp", encode_raster(rgb8, raster));
      ExtendedEncodeOptions extended;
      extended.target = ExtendedRasterTarget::Tiff;
      extended.alpha = AlphaMode::Straight;
      write("large-rgba.tiff", encode_extended(source, extended));
      extended.target = ExtendedRasterTarget::Webp;
      write("large-rgba.webp", encode_extended(source, extended));
      ProfessionalEncodeOptions pro;
      pro.target = ProfessionalTarget::Dds;
      pro.alpha = AlphaMode::Straight;
      write("large-rgba.dds", encode_professional(source, pro));
      for (auto &c : source.channels) {
        std::vector<std::byte> wide(c.samples.bytes.size() * 2);
        for (std::size_t n = 0; n < c.samples.bytes.size(); ++n) {
          const std::uint16_t v = std::uint16_t(
              std::to_integer<unsigned char>(c.samples.bytes[n]) * 257U +
              (n % 3));
          std::memcpy(wide.data() + n * 2, &v, 2);
        }
        c.samples.scalar = ScalarType::UInt16;
        c.samples.bytes = std::move(wide);
      }
      raster.target = RasterTarget::Png;
      raster.alpha = AlphaMode::Straight;
      write("large-rgba16.png", encode_raster(source, raster));
      for (auto &c : source.channels) {
        std::vector<std::byte> numeric(c.samples.element_count * 4);
        for (std::size_t n = 0; n < c.samples.element_count; ++n) {
          std::uint16_t v;
          std::memcpy(&v, c.samples.bytes.data() + n * 2, 2);
          const float f = float(v) / 16384.F - 1.F;
          std::memcpy(numeric.data() + n * 4, &f, 4);
        }
        c.samples.scalar = ScalarType::Float32;
        c.samples.bytes = std::move(numeric);
        c.encoding = SampleEncoding::Numeric;
      }
      extended.target = ExtendedRasterTarget::Tiff;
      write("large-float.tiff", encode_extended(source, extended));
      source.data_window.x = -17;
      source.data_window.y = 23;
      source.display_window = {-31, -7, 4200, 2400};
      pro.target = ProfessionalTarget::Exr;
      write("large-scanline.exr", encode_professional(source, pro));
      pro.exr_tiled = true;
      write("large-tiled.exr", encode_professional(source, pro));
      source.data_window = source.display_window = {0, 0, 4096, 2304};
      source.channels.pop_back();
      // RGBE does not encode negative values.
      for (auto &c : source.channels)
        for (std::size_t n = 0; n < c.samples.element_count; ++n) {
          float f;
          std::memcpy(&f, c.samples.bytes.data() + n * 4, 4);
          f = std::max(f, 0.F);
          std::memcpy(c.samples.bytes.data() + n * 4, &f, 4);
        }
      pro.target = ProfessionalTarget::Hdr;
      pro.allow_lossy = true;
      write("large.hdr", encode_professional(source, pro));
      std::cout << "Created large preview fixtures\n";
      return 0;
    }
    if (argc == 4 && app.arguments()[1] == "--preview-navigation") {
      QImageReader::setAllocationLimit(32);
      auto document = ImageDocument::open(std::filesystem::path(app.arguments()[2].toStdWString()),
                                        {512ULL*1024*1024, 512ULL*1024*1024, INT_MAX, 65536, 256, true});
      const auto patch = document.decode_preview(0, {113, 227, 5001, 5001, 1536, 1536});
      require(patch.data_window.width == 1536, "Unaligned reduced JPEG region");
      ImageSession session;
      session.open(app.arguments()[2]);
      waitFor(session);
      require(session.error().isEmpty() && session.overview(), "Full overview available");
      QJsonArray steps;
      const auto full = session.pixels()->previewFull;
      for (int repeat = 0; repeat < 3; ++repeat)
        for (const auto region : {QRect(113,227,5001,5001), QRect(2331,1317,3301,2903),
                                  QRect(6001,6003,1001,1003), QRect(0,0,2001,1003),
                                  QRect(QPoint(),full)}) {
          const auto base = session.overview();
          session.viewport(region,QSize(1536,1536));
          require(!session.busy(), "Refinement must not block the viewer");
          require(session.overview() == base, "Keep overview while refining");
          if (repeat == 0 && region.x() == 113) {
            session.sample(full.width()/2,full.height()/2);
            session.setSamplePinned(true);
          }
          waitFor(session);
          QElapsedTimer sampleWait;
          sampleWait.start();
          while (!session.samplePinned() && sampleWait.elapsed()<10000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents,5);
            QThread::msleep(1);
          }
          require(session.samplePinned(), "Exact pixel picking remains usable while refining");
          require(session.error().isEmpty(), "Navigation decode failed");
          require(session.pixels()->previewRegion == region, "Publish requested region");
          require(session.overview()->previewRegion == QRect(QPoint(),full), "Full overview coverage");
          require(session.overview()->info["sha256"] == session.pixels()->info["sha256"], "Overview identity");
          require(session.overview()->uploadBytes() <= 2048ULL*2048*4, "Bounded overview");
          require(session.pixels()->uploadBytes() <= 2048ULL*2048*4, "Bounded detail");
          steps.append(QJsonObject{{"x",region.x()},{"y",region.y()}, {"width",region.width()},
                                   {"height",region.height()},{"memory",memory()}});
        }
      save(app.arguments()[3], QJsonObject{{"unaligned_region", true}, {"navigation",steps}});
      return 0;
    }
    if (argc == 4 && app.arguments()[1] == "--preview-matrix")
      return previewMatrix(app.arguments()[2], app.arguments()[3]);
    if (argc == 4 && app.arguments()[1] == "--preview-samples") {
      QImageReader::setAllocationLimit(32);
      QJsonArray results;
      const QDir directory(app.arguments()[2]);
      for (const auto &file : directory.entryInfoList(QDir::Files, QDir::Name)) {
        if (!QStringList{"jpg","jpeg","tga","png","bmp","tif","tiff","webp","dds","exr","hdr","ico","gif"}.contains(file.suffix().toLower()))
          continue;
        ImageSession session;
        session.open(file.absoluteFilePath());
        waitFor(session);
        if (!session.error().isEmpty())
          throw std::runtime_error(session.error().toStdString());
        const auto pixels = session.pixels();
        require(bool(pixels), "Preview must produce pixels");
        require(pixels->uploadBytes() <= 2048ULL*2048*4, "Bounded preview texture");
        QJsonArray probes;
        QString regionHash;
        if (!pixels->previewFull.isEmpty()) {
          const auto full=pixels->previewFull;
          for (const auto point : {QPoint(0,0),QPoint(full.width()/2,full.height()/2),QPoint(full.width()-1,full.height()-1),QPoint(full.width()/3,full.height()/3),QPoint(full.width()*2/3,full.height()*2/3)}) {
            session.sample(point.x(),point.y());
            QElapsedTimer elapsed; elapsed.start();
            while(!session.pixel().value("valid").toBool() && elapsed.elapsed()<10000) {
              QCoreApplication::processEvents(QEventLoop::AllEvents,5); QThread::msleep(1);
            }
            require(session.pixel().value("valid").toBool(), "Exact asynchronous pixel sample");
            probes.append(QJsonObject::fromVariantMap(session.pixel()));
          }
          session.viewport(QRect(full.width()/2,full.height()/2,256,192),QSize(256,192));
          waitFor(session);
          require(session.error().isEmpty(), "Native-resolution visible region");
          require(session.pixels()->source.data_window.width<=256, "Region allocation is bounded");
          regionHash=QString::fromLatin1(digest(session.pixels()->uploadData(),qsizetype(session.pixels()->uploadBytes())));
        }
        results.append(QJsonObject{{"file", file.fileName()},
            {"sha256", pixels->info.value("sha256").toString()},
            {"upload_sha256", QString::fromLatin1(normalizedHash(*pixels))},
            {"probes",probes},
            {"region_rgba8_sha256",regionHash},
            {"memory",memory()},
            {"retained_bytes", static_cast<qint64>(pixels->bytes)}});
        session.clear();
      }
      require(results.size() > 0, "No preview samples");
      save(app.arguments()[3], QJsonObject{{"samples", results}});
      std::cout << "Passed preview samples" << std::endl;
      return 0;
    }
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
