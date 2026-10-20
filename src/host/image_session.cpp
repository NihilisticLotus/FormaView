#include "image_session.hpp"
#include "formaview/image_batch.hpp"
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <deque>
#include <set>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
using namespace formaview;
using namespace formaview::imaging;
QString text(const std::string &s) { return QString::fromStdString(s); }
#ifdef Q_OS_WIN
// Hold a read-only lease through cache validation and decoding. File timestamps
// may remain unchanged until an existing writer closes its handle.
struct ImageReadLease {
  HANDLE handle{INVALID_HANDLE_VALUE};
  explicit ImageReadLease(const QString &path) {
    handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
                         FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
      throw skeletal::Failure(
          ErrorCode::Io,
          "Cannot open stable image input; it may be in use by a writer");
  }
  ~ImageReadLease() { CloseHandle(handle); }
  ImageReadLease(const ImageReadLease &) = delete;
  ImageReadLease &operator=(const ImageReadLease &) = delete;
};
#endif
// Last-write time alone can be restored by an editor or asset pipeline. Include
// file identity and NTFS change time, queried off the GUI thread, in cache
// keys.
QString fileStamp(const QString &path) {
#ifdef Q_OS_WIN
  const auto handle =
      CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, 0, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    throw skeletal::Failure(ErrorCode::Io, "Cannot identify image file");
  FILE_ID_INFO identity{};
  FILE_BASIC_INFO basic{};
  FILE_STANDARD_INFO standard{};
  const bool ok = GetFileInformationByHandleEx(handle, FileIdInfo, &identity,
                                               sizeof(identity)) &&
                  GetFileInformationByHandleEx(handle, FileBasicInfo, &basic,
                                               sizeof(basic)) &&
                  GetFileInformationByHandleEx(handle, FileStandardInfo,
                                               &standard, sizeof(standard));
  CloseHandle(handle);
  if (!ok)
    throw skeletal::Failure(ErrorCode::Io, "Cannot read image file identity");
  return QString::number(identity.VolumeSerialNumber) + ':' +
         QByteArray(reinterpret_cast<const char *>(identity.FileId.Identifier),
                    16)
             .toHex() +
         ':' + QString::number(basic.ChangeTime.QuadPart) + ':' +
         QString::number(basic.LastWriteTime.QuadPart) + ':' +
         QString::number(standard.EndOfFile.QuadPart);
#else
  const QFileInfo file(path);
  return QString::number(file.size()) + ':' +
         QString::number(file.lastModified().toMSecsSinceEpoch());
#endif
}
QString scalarName(ScalarType s) {
  switch (s) {
  case ScalarType::UInt8:
    return "uint8";
  case ScalarType::UInt16:
    return "uint16";
  case ScalarType::UInt32:
    return "uint32";
  case ScalarType::Int16:
    return "int16";
  case ScalarType::Int32:
    return "int32";
  case ScalarType::Half:
    return "half";
  case ScalarType::Float32:
    return "float";
  case ScalarType::Float64:
    return "double";
  }
  return {};
}
QString number(double v) {
  if (std::isnan(v))
    return "NaN";
  if (std::isinf(v))
    return v < 0 ? "-Inf" : "+Inf";
  if (v == 0 && std::signbit(v))
    return "-0";
  return QString::number(v, 'g', 17);
}
QVariantMap window(const ImageWindow &w) {
  return {{"x", static_cast<qlonglong>(w.x)},
          {"y", static_cast<qlonglong>(w.y)},
          {"width", static_cast<qulonglong>(w.width)},
          {"height", static_cast<qulonglong>(w.height)}};
}
SchedulerLimits limits() {
  SchedulerLimits l;
  l.workers = 1;
  l.queue_capacity = 3;
  l.foreground_queue_reserve = 0;
  l.foreground_worker_reserve = 0;
  l.capacity = {1, 1, 1536ULL * 1024 * 1024, 0};
  l.foreground_reserve = {0, 0, 0, 0};
  return l;
}
} // namespace
struct ImageSession::Pending {
  formaview::TaskHandle task;
  std::shared_ptr<std::shared_ptr<ImagePixels>> result;
  quint64 generation{};
  int index{};
  bool preserveDisplay{};
};
struct ImageSession::Cache {
  struct Entry {
    QString key;
    std::shared_ptr<ImagePixels> pixels;
  };
  std::deque<Entry> entries;
  // Only the image worker accesses this cache.
  void insert(QString key, std::shared_ptr<ImagePixels> p) {
    entries.push_front({std::move(key), std::move(p)});
    std::size_t bytes{};
    for (const auto &e : entries)
      bytes += e.pixels->bytes;
    while (entries.size() > 1 &&
           (entries.size() > 8 || bytes > 256ULL * 1024 * 1024)) {
      bytes -= entries.back().pixels->bytes;
      entries.pop_back();
    }
  }
};
ImageSession::ImageSession(QObject *parent)
    : QObject(parent), scheduler_(limits()), cache_(std::make_unique<Cache>()) {
  display_ = {{"channel", -1},   {"exposure", 0.},
              {"colour", false}, {"srgb", false},
              {"alpha", 0},      {"composite", false},
              {"tiled", false},  {"displayWindow", false},
              {"background", 0}};
  connect(&timer_, &QTimer::timeout, this, &ImageSession::poll);
  timer_.setInterval(8);
}
ImageSession::~ImageSession() { scheduler_.shutdown(); }
void ImageSession::open(const QString &path, int index) {
  load(path, index, {});
}
void ImageSession::selectSubresource(int index) {
  if (pixels_ && index >= 0 && index < pixels_->subresources.size())
    load(pixels_->path, index, {});
}
void ImageSession::resourcePage(int offset) {
  if (!pixels_ || offset < 0 || offset >= subresourceCount())
    return;
  resourceOffset_ = offset;
  emit changed();
}
void ImageSession::mapChannels(const QStringList &names) {
  if (pixels_ && names.size() == 4)
    load(pixels_->path, selected_, names);
}
void ImageSession::selectLayer(const QString &name) {
  if (!pixels_ || !pixels_->info.value("layers").toStringList().contains(name))
    return;
  QStringList names;
  for (const auto *component : {"R", "G", "B", "A"}) {
    const auto candidate =
        name.isEmpty() ? QString(component) : name + '.' + component;
    const auto found = std::find_if(
        pixels_->source.channels.begin(), pixels_->source.channels.end(),
        [&](const auto &c) { return text(c.name) == candidate; });
    names.append(found == pixels_->source.channels.end() ? QString{}
                                                         : candidate);
  }
  const auto luminance = name.isEmpty() ? QString("Y") : name + ".Y";
  if (std::all_of(names.begin(), names.begin() + 3,
                  [](const auto &n) { return n.isEmpty(); })) {
    const bool hasY = std::any_of(
        pixels_->source.channels.begin(), pixels_->source.channels.end(),
        [&](const auto &c) { return text(c.name) == luminance; });
    if (hasY) {
      names[0] = names[1] = names[2] = luminance;
      mapChannels(names);
      return;
    }
    for (const auto &c : pixels_->source.channels) {
      const auto channel = text(c.name);
      const auto layer = channel.contains('.')
                             ? channel.left(channel.lastIndexOf('.'))
                             : QString{};
      if (layer == name && channel != names[3]) {
        names[0] = channel;
        break;
      }
    }
  }
  mapChannels(names);
}
namespace {
QVariantMap resourceDimensions(const QVariantList &resources, int selected) {
  QVariantMap result;
  const auto current = resources[selected].toMap();
  QStringList parents;
  for (const auto *dimension : {"part", "mip", "array", "face", "slice"}) {
    QVariantList values;
    std::map<int, std::pair<int, QVariantMap>> choices;
    for (const auto &entry : resources) {
      const auto subresource = entry.toMap();
      bool match = true;
      for (const auto &parent : parents)
        match &= subresource.value(parent) == current.value(parent);
      if (!match)
        continue;
      const auto value = subresource.value(dimension).toInt();
      int score = 0, weight = 16;
      for (const auto *child : {"part", "mip", "array", "face", "slice"}) {
        if (subresource.value(child) == current.value(child))
          score += weight;
        weight /= 2;
      }
      const auto found = choices.find(value);
      if (found == choices.end() || score > found->second.first)
        choices[value] = {score, subresource};
    }
    QVariantMap indices;
    for (const auto &[value, choice] : choices) {
      values.append(
          QVariantMap{{"value", value}, {"name", choice.second.value("name")}});
      indices[QString::number(value)] = choice.second.value("index");
    }
    result[dimension] = values;
    result[QString(dimension) + "Indices"] = indices;
    parents.append(dimension);
  }
  result["current"] = current;
  return result;
}
} // namespace
QVariantMap ImageSession::dimensions() const {
  return pixels_ ? pixels_->info.value("dimensions").toMap() : QVariantMap{};
}
void ImageSession::selectDimension(const QString &name, int value) {
  if (!pixels_ ||
      !QStringList{"part", "mip", "array", "face", "slice"}.contains(name))
    return;
  const auto indices = dimensions().value(name + "Indices").toMap();
  if (indices.contains(QString::number(value)))
    selectSubresource(indices.value(QString::number(value)).toInt());
}
void ImageSession::load(const QString &path, int index, QStringList mapping) {
  for (const auto &job : pending_)
    job->task.cancel();
  const auto generation = ++generation_;
  requested_ = path;
  busy_ = true;
  error_.clear();
  pixel_.clear();
  samplePinned_ = false;
  emit changed();
  emit pixelChanged();
  auto result = std::make_shared<std::shared_ptr<ImagePixels>>();
  auto request = TaskRequest{};
  request.priority = Priority::Foreground;
  request.resources = {1, 1, 1024ULL * 1024 * 1024, 0};
  request.work = [this, path, index, mapping, result,
                  generation](std::stop_token stop) {
    try {
      const auto file = QFileInfo(path);
      if (!file.exists() || !file.isFile())
        throw skeletal::Failure(ErrorCode::Io, "Image file is unavailable");
#ifdef Q_OS_WIN
      const ImageReadLease lease(path);
#endif
      const auto stamp = fileStamp(path);
      const auto key =
          path + '\n' + stamp + '\n' + QString::number(index) + '\n' +
          QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(mapping))
                                .toJson(QJsonDocument::Compact));
      for (auto it = cache_->entries.begin(); it != cache_->entries.end(); ++it)
        if (it->key == key) {
          *result = it->pixels;
          std::rotate(cache_->entries.begin(), it, std::next(it));
          return TaskResult{};
        }
      const auto admission = ImageAdmission(true, stop);
      auto document = ImageDocument::open(
          std::filesystem::path(path.toStdWString()),
          {512ULL * 1024 * 1024, 512ULL * 1024 * 1024, 16384, 65536, 256},
          stop);
      if (index < 0 || static_cast<std::size_t>(index) >=
                           document.description().subresources.size())
        throw skeletal::Failure(ErrorCode::InvalidArgument,
                                "Subresource index out of range");
      auto p = std::make_shared<ImagePixels>();
      p->source = document.decode(static_cast<std::size_t>(index), stop);
      p->path = path;
      p->revision = generation;
      const auto &d = document.description();
      QVariantMap metadata;
      for (const auto &[k, v] : d.source.properties)
        if (!k.ends_with("icc_hex") && !k.ends_with("icc_base64"))
          metadata.insert(text(k), text(v));
      const auto semantics = document.semantics(index);
      p->info = {
          {"sha256", text(d.source.identity.sha256)},
          {"dataWindow", window(p->source.data_window)},
          {"displayWindow", window(p->source.display_window)},
          {"alpha", static_cast<int>(semantics.alpha)},
          {"containerAlpha", static_cast<int>(d.alpha)},
          {"meaning", static_cast<int>(semantics.meaning)},
          {"colourSpace",
           semantics.colour_space ? text(*semantics.colour_space) : QString{}},
          {"iccProfileSha256", text(semantics.icc_profile_sha256)},
          {"metadata", metadata},
          {"block", p->source.block_encoding ? text(*p->source.block_encoding)
                                             : QString{}},
          {"format", file.suffix().toUpper()}};
      bool mips = false, arrays = false, faces = false, slices = false;
      for (std::size_t i = 0; i < d.subresources.size(); ++i) {
        const auto &s = d.subresources[i];
        mips |= s.mip != 0;
        arrays |= s.array_layer != 0;
        faces |= s.cube_face != 0;
        slices |= s.depth_slice != 0;
        p->subresources.push_back(QVariantMap{
            {"index", static_cast<int>(i)},
            {"part", s.part},
            {"name", text(s.part_name)},
            {"mip", s.mip},
            {"array", s.array_layer},
            {"face", s.cube_face},
            {"slice", s.depth_slice},
            {"width", static_cast<qulonglong>(s.data_window.width)},
            {"height", static_cast<qulonglong>(s.data_window.height)}});
      }
      p->info["hasMips"] = mips;
      p->info["hasArrays"] = arrays;
      p->info["hasFaces"] = faces;
      p->info["hasSlices"] = slices;
      p->info["dimensions"] = resourceDimensions(p->subresources, index);
      QStringList defaults{"R", "G", "B", "A"};
      QStringList layers;
      QStringList channelNames;
      for (const auto &c : p->source.channels) {
        const auto name = text(c.name);
        channelNames.append(name);
        const auto layer =
            name.contains('.') ? name.left(name.lastIndexOf('.')) : QString{};
        if (!layers.contains(layer))
          layers.append(layer);
      }
      p->info["layers"] = layers;
      bool luminance = false;
      for (const auto &c : p->source.channels)
        if (c.name == "Y")
          luminance = true;
      if (luminance)
        defaults = {"Y", "Y", "Y", "A"};
      else if (!(channelNames.contains("R") && channelNames.contains("G") &&
                 channelNames.contains("B")))
        for (const auto &layer : layers) {
          const auto prefix = layer.isEmpty() ? QString{} : layer + '.';
          if (channelNames.contains(prefix + "R") &&
              channelNames.contains(prefix + "G") &&
              channelNames.contains(prefix + "B")) {
            defaults = {prefix + "R", prefix + "G", prefix + "B", prefix + "A"};
            break;
          }
        }
      const auto names = mapping.isEmpty() ? defaults : mapping;
      for (std::size_t c = 0; c < p->source.channels.size(); ++c) {
        const auto &channel = p->source.channels[c];
        p->bytes += channel.samples.bytes.size();
        p->channels.push_back(QVariantMap{
            {"name", text(channel.name)},
            {"type", scalarName(channel.samples.scalar)},
            {"encoding", channel.encoding == SampleEncoding::UNorm ? "UNorm"
                         : channel.encoding == SampleEncoding::SNorm
                             ? "SNorm"
                             : "Numeric"}});
        for (int n = 0; n < 4; ++n)
          if (text(channel.name) == names[n])
            p->mapping[static_cast<std::size_t>(n)] = static_cast<int>(c);
      }
      // A non-RGB image starts with its first real channel, never a fabricated
      // RGB view.
      if (std::all_of(p->mapping.begin(), p->mapping.end(),
                      [](int v) { return v < 0; }) &&
          !p->source.channels.empty()) {
        if (!mapping.isEmpty())
          throw skeletal::Failure(ErrorCode::InvalidArgument,
                                  "Map at least one existing source channel");
        p->mapping[0] = 0;
      }
      QVariantList mapped;
      for (int c : p->mapping)
        mapped.push_back(
            c >= 0 ? text(p->source.channels[static_cast<std::size_t>(c)].name)
                   : QString{});
      p->info.insert("mapping", mapped);
      p->bytes += p->source.encoded_blocks.size();
      p->bytes += static_cast<std::size_t>(
          QJsonDocument(QJsonObject::fromVariantMap(p->info))
              .toJson(QJsonDocument::Compact)
              .size());
      // QVariant/string containers also retain decoded metadata and indices.
      p->bytes += static_cast<std::size_t>(p->subresources.size()) * 512 +
                  static_cast<std::size_t>(p->channels.size()) * 256;
      const auto pixels =
          p->source.data_window.width * p->source.data_window.height;
      if (pixels > (512ULL * 1024 * 1024 -
                    std::min<std::size_t>(p->bytes, 512ULL * 1024 * 1024)) /
                       16)
        throw skeletal::Failure(
            ErrorCode::ResourceLimit,
            "Source plus display upload exceeds 512 MiB preview budget");
      p->rgba.resize(static_cast<std::size_t>(pixels) * 4);
      for (std::size_t i = 0; i < pixels; ++i) {
        if ((i & 4095) == 0 && stop.stop_requested())
          throw skeletal::Failure(ErrorCode::Cancelled,
                                  "Image preparation cancelled");
        for (std::size_t c = 0; c < 4; ++c)
          p->rgba[i * 4 + c] =
              p->mapping[c] >= 0
                  ? static_cast<float>(interpreted_value(
                        p->source
                            .channels[static_cast<std::size_t>(p->mapping[c])],
                        i))
              : c == 3 ? 1.F
                       : 0.F;
      }
      p->bytes += p->rgba.size() * sizeof(float);
      if (stop.stop_requested())
        throw skeletal::Failure(ErrorCode::Cancelled,
                                "Image preparation cancelled");
      if (fileStamp(path) != stamp)
        throw skeletal::Failure(ErrorCode::Io,
                                "Image changed while being decoded; reopen it");
      cache_->insert(key, p);
      *result = std::move(p);
      return TaskResult{};
    } catch (const skeletal::Failure &e) {
      TaskResult r;
      r.error = e.error;
      return r;
    } catch (const std::exception &e) {
      TaskResult r;
      r.error = Error{ErrorCode::Io, e.what(), {}, true};
      return r;
    }
  };
  auto submitted = scheduler_.submit(std::move(request));
  if (!submitted.task) {
    busy_ = false;
    error_ = text(submitted.error->message);
    emit changed();
    return;
  }
  auto job = std::make_unique<Pending>();
  job->task = *submitted.task;
  job->result = result;
  job->generation = generation;
  job->index = index;
  job->preserveDisplay = pixels_ && pixels_->path == path && selected_ == index;
  pending_.push_back(std::move(job));
  timer_.start();
}
void ImageSession::poll() {
  for (auto it = pending_.begin(); it != pending_.end();) {
    const auto state = (*it)->task.snapshot();
    if (!state.result) {
      ++it;
      continue;
    }
    if ((*it)->generation == generation_) {
      busy_ = false;
      if (state.result->error)
        error_ = text(state.result->error->message);
      else if (*(*it)->result) {
        pixels_ = *(*it)->result;
        selected_ = (*it)->index;
        resourceOffset_ = (selected_ / 100) * 100;
        const auto &m = pixels_->mapping;
        const auto oldChannel = display_["channel"].toInt();
        const bool validChannel =
            oldChannel < 0 ? m[0] >= 0 && m[1] >= 0 && m[2] >= 0
                           : oldChannel < 4 &&
                                 m[static_cast<std::size_t>(oldChannel)] >= 0;
        if (!(*it)->preserveDisplay || !validChannel)
          display_["channel"] =
              m[0] >= 0 && m[1] >= 0 && m[2] >= 0
                  ? -1
                  : static_cast<int>(
                        std::find_if(m.begin(), m.end(),
                                     [](int v) { return v >= 0; }) -
                        m.begin());
        if (!(*it)->preserveDisplay) {
          display_["alpha"] = pixels_->info["alpha"];
          display_["composite"] =
              m[3] >= 0 && pixels_->info["alpha"].toInt() >= 2;
          display_["colour"] = pixels_->info["meaning"].toInt() ==
                               static_cast<int>(ImageMeaning::Colour);
          display_["srgb"] = pixels_->info["colourSpace"].toString().contains(
              "linear", Qt::CaseInsensitive);
          display_["exposure"] = 0.;
        }
        emit displayChanged();
      }
      emit changed();
    }
    it = pending_.erase(it);
  }
  if (pending_.empty())
    timer_.stop();
}
void ImageSession::clear() {
  ++generation_;
  for (const auto &job : pending_)
    job->task.cancel();
  pixels_.reset();
  busy_ = false;
  error_.clear();
  requested_.clear();
  pixel_.clear();
  samplePinned_ = false;
  // FIFO worker cleanup releases cached CPU storage without blocking the UI.
  TaskRequest r;
  r.priority = Priority::Foreground;
  r.work = [this](std::stop_token) {
    cache_->entries.clear();
    return TaskResult{};
  };
  if (cleanup_)
    cleanup_->cancel();
  cleanup_ = scheduler_.submit(std::move(r)).task;
  emit changed();
  emit pixelChanged();
}
void ImageSession::setting(const QString &name, const QVariant &v) {
  if (!display_.contains(name))
    return;
  if (name == "channel") {
    const int c = v.toInt();
    if (!pixels_ || c < -1 || c > 3)
      return;
    if (c == -1 && (pixels_->mapping[0] < 0 || pixels_->mapping[1] < 0 ||
                    pixels_->mapping[2] < 0))
      return;
    if (c >= 0 && pixels_->mapping[static_cast<std::size_t>(c)] < 0)
      return;
  } else if (name == "exposure") {
    if (!std::isfinite(v.toDouble()) || std::abs(v.toDouble()) > 32)
      return;
  } else if ((name == "alpha" || name == "background") &&
             (v.toInt() < 0 || v.toInt() > 3))
    return;
  if (display_[name] == v)
    return;
  display_[name] = v;
  emit displayChanged();
  if (pixel_.value("valid").toBool())
    sample(pixel_["x"].toDouble(), pixel_["y"].toDouble(),
           pixel_["tileX"].toInt(), pixel_["tileY"].toInt());
}
void ImageSession::setSamplePinned(bool pinned) {
  pinned = pinned && pixel_.value("valid").toBool() && !busy_;
  if (samplePinned_ == pinned)
    return;
  samplePinned_ = pinned;
  emit pixelChanged();
}
void ImageSession::sample(double x, double y, int tileX, int tileY) {
  pixel_ = {
      {"valid", false}, {"x", x}, {"y", y}, {"tileX", tileX}, {"tileY", tileY}};
  if (!pixels_ || busy_ || !error_.isEmpty() || !std::isfinite(x) ||
      !std::isfinite(y)) {
    samplePinned_ = false;
    emit pixelChanged();
    return;
  }
  const auto &s = pixels_->source;
  const auto px = std::floor(x) - static_cast<double>(s.data_window.x),
             py = std::floor(y) - static_cast<double>(s.data_window.y);
  if (px < 0 || py < 0 || px >= s.data_window.width ||
      py >= s.data_window.height) {
    samplePinned_ = false;
    emit pixelChanged();
    return;
  }
  const auto i = static_cast<std::size_t>(py) * s.data_window.width +
                 static_cast<std::size_t>(px);
  QVariantList rows;
  for (const auto &c : s.channels)
    rows.push_back(
        QVariantMap{{"name", text(c.name)},
                    {"type", scalarName(c.samples.scalar)},
                    {"source", number(value(c, i))},
                    {"interpreted", number(interpreted_value(c, i))}});
  std::array<double, 4> v{0, 0, 0, 1};
  for (std::size_t c = 0; c < 4; ++c)
    if (pixels_->mapping[c] >= 0)
      v[c] = interpreted_value(
          s.channels[static_cast<std::size_t>(pixels_->mapping[c])], i);
  const auto channel = display_["channel"].toInt();
  if (channel >= 0)
    v = {v[static_cast<std::size_t>(channel)],
         v[static_cast<std::size_t>(channel)],
         v[static_cast<std::size_t>(channel)], 1};
  else
    for (std::size_t c = 0; c < 3; ++c) {
      if (display_["alpha"].toInt() == 3 && v[3] != 0)
        v[c] /= v[3];
      if (display_["colour"].toBool()) {
        v[c] *= std::exp2(display_["exposure"].toDouble());
        if (display_["srgb"].toBool())
          v[c] = v[c] <= .0031308 ? 12.92 * v[c]
                                  : 1.055 * std::pow(v[c], 1. / 2.4) - .055;
      }
    }
  QVariantList derived;
  for (double n : v)
    derived.push_back(number(n));
  pixel_["valid"] = true;
  pixel_["source"] = rows;
  pixel_["display"] = derived;
  emit pixelChanged();
}
QString ImageSession::pixelReport() const {
  auto report = pixel_;
  report["path"] = path();
  report["sha256"] = info().value("sha256");
  report["subresource"] = selected_;
  report["mapping"] = info().value("mapping");
  report["displayOptions"] = display_;
  return QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(report))
                               .toJson(QJsonDocument::Indented));
}
