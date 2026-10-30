#include "thumbnail.hpp"
#ifdef FORMAVIEW_IMAGE_UI
#include "formaview/image_batch.hpp"
#include "formaview/image_conversion.hpp"
#endif
#include <QCache>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QImageIOHandler>
#include <QImageReader>
#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <png.h>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <vector>

namespace {
constexpr int width = 160, height = 120;
constexpr qint64 maxInput = 128LL * 1024 * 1024;
constexpr std::size_t pngMemory = 16ULL * 1024 * 1024;
struct Result {
  QImage image;
  QString error;
};
class Response final : public QQuickImageResponse {
public:
  std::stop_source stop;
  void cancel() override { stop.request_stop(); }
  QQuickTextureFactory *textureFactory() const override {
    return QQuickTextureFactory::textureFactoryForImage(result_.image);
  }
  QString errorString() const override { return result_.error; }
  void complete(Result result) {
    QMetaObject::invokeMethod(
        this,
        [this, result = std::move(result)]() mutable {
          result_ = stop.stop_requested() ? Result{{}, "Thumbnail cancelled"}
                                          : std::move(result);
          emit finished(); // Last access; Qt owns cleanup after this signal.
        },
        Qt::QueuedConnection);
  }

private:
  Result result_;
};

// libpng's longjmp never crosses C++ objects created after setjmp. Mutable
// buffers live in this heap-owned context and are released on every exit.
struct PngRead {
  QFile file;
  std::stop_token stop;
  std::size_t allocated{};
  char error[192]{};
  std::vector<unsigned char> rows;
  QImage image;
  png_structp png{};
  png_infop info{};
  explicit PngRead(const QString &path, std::stop_token token)
      : file(path), stop(token) {}
  ~PngRead() {
    if (png)
      png_destroy_read_struct(&png, &info, nullptr);
  }
};
// MSVC max_align_t is 8, while its jmp_buf uses 16-byte SIMD stores. Preserve
// malloc's 16-byte alignment after our accounting header as well.
struct alignas(16) Allocation {
  std::size_t size;
  std::byte padding[16 - sizeof(std::size_t)];
};
png_voidp pngAllocate(png_structp png, png_alloc_size_t size) {
  auto *state = static_cast<PngRead *>(png_get_mem_ptr(png));
  if (size > pngMemory - state->allocated)
    return nullptr;
  auto *block =
      static_cast<Allocation *>(std::malloc(sizeof(Allocation) + size));
  if (!block)
    return nullptr;
  block->size = size;
  state->allocated += size;
  return block + 1;
}
void pngFree(png_structp png, png_voidp memory) {
  if (!memory)
    return;
  auto *state = static_cast<PngRead *>(png_get_mem_ptr(png));
  auto *block = static_cast<Allocation *>(memory) - 1;
  state->allocated -= block->size;
  std::free(block);
}
void pngError(png_structp png, png_const_charp message) {
  auto *state = static_cast<PngRead *>(png_get_error_ptr(png));
  std::copy_n(message, std::min(std::strlen(message), sizeof(state->error) - 1),
              state->error);
  png_longjmp(png, 1);
}
void pngRead(png_structp png, png_bytep bytes, png_size_t count) {
  auto *state = static_cast<PngRead *>(png_get_io_ptr(png));
  if (state->stop.stop_requested())
    png_error(png, "Thumbnail cancelled");
  if (count > static_cast<png_size_t>(maxInput) ||
      state->file.read(reinterpret_cast<char *>(bytes),
                       static_cast<qint64>(count)) !=
          static_cast<qint64>(count))
    png_error(png, "Truncated PNG");
}
// This jump boundary has only trivially destructible locals. The owning C++
// context is one stack frame above it, including during cancellation/errors.
void allocatePngImage(PngRead *state, QSize target) {
  state->image = QImage(target, QImage::Format_RGBA8888);
}
static_assert(std::is_trivially_destructible_v<QSize>);
#ifdef _MSC_VER
#pragma warning(push)
// The isolated jump frame below contains only scalars, pointers and QSize.
// MSVC warns about setjmp in C++ even when no destructor is bypassed.
#pragma warning(disable : 4611)
#endif
bool readPng(PngRead *state) {
  auto *png = state->png;
  auto *info = state->info;
  auto *jump = png_set_longjmp_fn(png, longjmp, sizeof(jmp_buf));
  if (!jump) {
    std::memcpy(state->error, "PNG jump buffer unavailable", 28);
    return false;
  }
  if (setjmp(*jump))
    return false;
  png_set_read_fn(png, state, pngRead);
  png_set_user_limits(png, 32768, 32768);
  png_set_chunk_cache_max(png, 32);
  png_set_chunk_malloc_max(png, 1024 * 1024);
  png_read_info(png, info);
  const auto w = png_get_image_width(png, info),
             h = png_get_image_height(png, info);
  const auto type = png_get_color_type(png, info);
  if (png_get_bit_depth(png, info) == 16)
    png_set_strip_16(png);
  if (type == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(png);
  if (type == PNG_COLOR_TYPE_GRAY && png_get_bit_depth(png, info) < 8)
    png_set_expand_gray_1_2_4_to_8(png);
  const bool transparency = png_get_valid(png, info, PNG_INFO_tRNS) != 0;
  if (transparency)
    png_set_tRNS_to_alpha(png);
  if (type == PNG_COLOR_TYPE_GRAY || type == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png);
  if (!(type & PNG_COLOR_MASK_ALPHA) && !transparency)
    png_set_filler(png, 255, PNG_FILLER_AFTER);
  const auto passes = png_set_interlace_handling(png);
  if (static_cast<std::uint64_t>(w) * h * passes > 256ULL * 1024 * 1024)
    png_error(png, "PNG exceeds thumbnail scan budget");
  png_read_update_info(png, info);
  const auto stride = png_get_rowbytes(png, info);
  if (!w || !h || stride != static_cast<std::size_t>(w) * 4)
    png_error(png, "Invalid PNG dimensions");
  // Never enlarge: selected input scanlines must be distinct.
  const auto target =
      QSize(static_cast<int>(w), static_cast<int>(h))
          .scaled(std::min(width, static_cast<int>(w)),
                  std::min(height, static_cast<int>(h)), Qt::KeepAspectRatio);
  allocatePngImage(state, target);
  if (state->image.isNull())
    png_error(png, "Thumbnail allocation failed");
  // At most 120 selected scanlines plus scratch (15.2 MiB at 32K).
  // Selected rows persist for correct Adam7 combination across passes.
  state->rows.resize(stride * (static_cast<std::size_t>(target.height()) + 1));
  for (int pass = 0; pass < passes; ++pass) {
    int next = 0;
    for (png_uint_32 y = 0; y < h; ++y) {
      if (state->stop.stop_requested())
        png_error(png, "Thumbnail cancelled");
      const auto wanted = static_cast<png_uint_32>(
          (static_cast<std::uint64_t>(2 * next + 1) * h) /
          (2 * target.height()));
      auto *row =
          state->rows.data() + stride * (next < target.height() && y == wanted
                                             ? next++
                                             : target.height());
      png_read_row(png, row, nullptr);
    }
  }
  png_read_end(png, info);
  for (int y = 0; y < target.height(); ++y) {
    auto *out = state->image.scanLine(y);
    for (int x = 0; x < target.width(); ++x) {
      const auto sx =
          (static_cast<std::uint64_t>(2 * x + 1) * w) / (2 * target.width());
      std::memcpy(out + x * 4, state->rows.data() + y * stride + sx * 4, 4);
    }
  }
  return true;
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif
Result pngThumbnail(const QString &path, std::stop_token stop) {
  auto state = std::make_unique<PngRead>(path, stop);
  if (!state->file.open(QIODevice::ReadOnly))
    return {{}, "Image unavailable"};
  state->png = png_create_read_struct_2(
      PNG_LIBPNG_VER_STRING, state.get(), pngError,
      [](png_structp, png_const_charp) {}, state.get(), pngAllocate, pngFree);
  if (!state->png)
    return {{}, "PNG allocation failed"};
  state->info = png_create_info_struct(state->png);
  if (!state->info)
    return {{}, "PNG allocation failed"};
  if (!readPng(state.get()))
    return {{}, QString::fromUtf8(state->error)};
  return {std::move(state->image), {}};
}

#ifdef FORMAVIEW_IMAGE_UI
Result documentThumbnail(const QString &path, std::stop_token stop) {
  using namespace formaview;
  using namespace formaview::imaging;
  auto document =
      ImageDocument::open(std::filesystem::path(path.toStdWString()),
                          {static_cast<std::uint64_t>(maxInput),
                           512ULL * 1024 * 1024, 16384, 65536, 256},
                          stop);
  const auto &entries = document.description().subresources;
  if (entries.empty())
    return {{}, "Image contains no subresource"};
  std::size_t selected = 0;
  for (std::size_t i = 1; i < entries.size(); ++i) {
    const auto &s = entries[i], &first = entries[0], &best = entries[selected];
    if (s.part == first.part && s.array_layer == first.array_layer &&
        s.cube_face == first.cube_face && s.depth_slice == first.depth_slice &&
        s.data_window.width >= width && s.data_window.height >= height &&
        s.data_window.width < best.data_window.width)
      selected = i;
  }
  const auto source = document.decode(selected, stop);
  const auto semantics = document.semantics(selected);
  const auto w = source.data_window.width, h = source.data_window.height;
  if (!w || !h)
    return {{}, "Empty image"};
  const auto target = QSize(static_cast<int>(w), static_cast<int>(h))
                          .scaled(width, height, Qt::KeepAspectRatio);
  QImage image(target, QImage::Format_RGBA8888);
  if (image.isNull())
    return {{}, "Thumbnail allocation failed"};
  const auto channel = [&](const std::string &name) -> const ImageChannel * {
    for (const auto &c : source.channels)
      if (c.name == name)
        return &c;
    return nullptr;
  };
  std::array<const ImageChannel *, 4> channels{channel("R"), channel("G"),
                                               channel("B"), channel("A")};
  if (const auto *luma = channel("Y"))
    channels = {luma, luma, luma, channel("A")};
  else if (!channels[0] && !channels[1] && !channels[2]) {
    for (const auto &c : source.channels) {
      if (!c.name.ends_with(".R"))
        continue;
      const auto prefix = c.name.substr(0, c.name.size() - 1);
      if (channel(prefix + "G") && channel(prefix + "B")) {
        channels = {&c, channel(prefix + "G"), channel(prefix + "B"),
                    channel(prefix + "A")};
        break;
      }
    }
    if (!channels[0] && !source.channels.empty())
      channels[0] = &source.channels.front();
  }
  for (int y = 0; y < target.height(); ++y) {
    if (stop.stop_requested())
      return {{}, "Thumbnail cancelled"};
    auto *out = image.scanLine(y);
    const auto sy =
        static_cast<std::uint64_t>(2 * y + 1) * h / (2 * target.height());
    for (int x = 0; x < target.width(); ++x) {
      const auto sx =
          static_cast<std::uint64_t>(2 * x + 1) * w / (2 * target.width());
      std::array<double, 4> rgba{0, 0, 0, 1};
      for (std::size_t c = 0; c < 4; ++c)
        if (channels[c])
          rgba[c] = interpreted_value(*channels[c], sy * w + sx);
      if (!std::all_of(rgba.begin(), rgba.end(),
                       [](double v) { return std::isfinite(v); }))
        rgba = {1, 0, 1, 1};
      for (std::size_t c = 0; c < 3; ++c) {
        if (semantics.alpha == AlphaMode::Premultiplied && rgba[3] > 0)
          rgba[c] /= rgba[3];
        if (semantics.meaning == ImageMeaning::Colour &&
            semantics.colour_space == "linear")
          rgba[c] = rgba[c] <= 0.0031308
                        ? 12.92 * rgba[c]
                        : 1.055 * std::pow(rgba[c], 1 / 2.4) - 0.055;
      }
      for (std::size_t c = 0; c < 4; ++c)
        out[x * 4 + c] =
            static_cast<uchar>(std::lround(std::clamp(rgba[c], 0., 1.) * 255));
    }
  }
  return {std::move(image), {}};
}
#endif
Result decode(const QString &path, std::stop_token stop) {
#ifdef FORMAVIEW_IMAGE_UI
  const formaview::imaging::ImageAdmission admission(false, stop);
#endif
  if (stop.stop_requested())
    return {{}, "Thumbnail cancelled"};
  QFile signature(path);
  if (!signature.open(QIODevice::ReadOnly))
    return {{}, "Image unavailable"};
  const auto magic = signature.read(8);
  signature.close();
  if (magic == QByteArray("\x89PNG\r\n\x1a\n", 8))
    return pngThumbnail(path, stop);
  QImageReader reader(path);
  reader.setAutoTransform(false); // Same stored orientation as image preview.
  const auto size = reader.size();
  if (size.isValid() && size.width() <= 32768 && size.height() <= 32768 &&
      (reader.supportsOption(QImageIOHandler::ScaledSize) ||
       static_cast<qint64>(size.width()) * size.height() <=
           32LL * 1024 * 1024 / 8)) {
    reader.setScaledSize(size.scaled(width, height, Qt::KeepAspectRatio));
    auto image = reader.read();
    if (!image.isNull())
      return {image.scaled(width, height, Qt::KeepAspectRatio,
                           Qt::SmoothTransformation),
              {}};
  }
#ifdef FORMAVIEW_IMAGE_UI
  return documentThumbnail(path, stop);
#else
  return {{}, reader.errorString()};
#endif
}
QString stamp(const QFileInfo &file) {
  return QString::number(file.size()) + ':' +
         QString::number(file.lastModified().toMSecsSinceEpoch()) + ':' +
         QString::number(file.metadataChangeTime().toMSecsSinceEpoch());
}
} // namespace

struct ThumbnailProvider::Impl {
  struct Work {
    QString id;
    Response *response;
  };
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::deque<Work> queue;
  QCache<QString, QImage> cache{32 * 1024};
  std::uint64_t generation{};
  bool closing{};
  Response *active{};
  std::jthread worker;
  Impl() : worker([this] { run(); }) {}
  ~Impl() {
    {
      std::lock_guard lock(mutex);
      closing = true;
      if (active)
        active->cancel();
      for (auto &work : queue)
        work.response->cancel();
    }
    wake.notify_one();
    worker.join();
  }
  void run() {
    for (;;) {
      Work work;
      std::uint64_t revision;
      {
        std::unique_lock lock(mutex);
        wake.wait(lock, [&] { return closing || !queue.empty(); });
        if (queue.empty())
          return;
        work = std::move(queue.front());
        queue.pop_front();
        active = work.response;
        revision = generation;
      }
      Result result;
      const auto stop = work.response->stop.get_token();
      try {
        if (stop.stop_requested())
          result.error = "Thumbnail cancelled";
        else {
          const auto encoded = QByteArray::fromBase64(
              work.id.section('?', 0, 0).toLatin1(),
              QByteArray::Base64UrlEncoding |
                  QByteArray::AbortOnBase64DecodingErrors);
          const auto path =
              QString::fromUtf8(QByteArray::fromPercentEncoding(encoded));
          const QFileInfo file(path);
          if (path.isEmpty() || !file.isAbsolute() || !file.isFile() ||
              file.size() > maxInput)
            result.error =
                "Image unavailable or exceeds thumbnail input budget";
          else {
            const auto before = stamp(file);
            const auto key = path + '\n' + before;
            {
              std::lock_guard lock(mutex);
              if (const auto *image = cache.object(key))
                result.image = *image;
            }
            if (result.image.isNull()) {
              result = decode(path, stop);
              if (before != stamp(QFileInfo(path)))
                result = {{}, "Image changed while loading"};
              if (!result.image.isNull() && !stop.stop_requested()) {
                const auto cost = static_cast<int>(
                    (result.image.sizeInBytes() + key.size() * 2 + 256 + 1023) /
                    1024);
                std::lock_guard lock(mutex);
                if (revision == generation)
                  cache.insert(key, new QImage(result.image), cost);
              }
            }
          }
        }
      } catch (const std::exception &error) {
        result = {{}, QString::fromUtf8(error.what())};
      } catch (...) {
        result = {{}, "Thumbnail decode failed"};
      }
      if (stop.stop_requested())
        result = {{}, "Thumbnail cancelled"};
      {
        std::lock_guard lock(mutex);
        active = nullptr;
      }
      work.response->complete(std::move(result));
    }
  }
};
ThumbnailProvider::ThumbnailProvider() : impl_(std::make_unique<Impl>()) {}
ThumbnailProvider::~ThumbnailProvider() = default;
QQuickImageResponse *ThumbnailProvider::requestImageResponse(const QString &id,
                                                             const QSize &) {
  auto *response = new Response;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closing || impl_->queue.size() >= 128) {
      response->complete({{}, "Thumbnail queue is full"});
      return response;
    }
    impl_->queue.push_back({id, response});
  }
  impl_->wake.notify_one();
  return response;
}
int ThumbnailProvider::cacheKiB() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->cache.totalCost();
}
void ThumbnailProvider::clear() {
  std::lock_guard lock(impl_->mutex);
  ++impl_->generation;
  impl_->cache.clear();
}
