#pragma once
#include <QCache>
#include <QImageReader>
#include <QMutex>
#include <QMutexLocker>
#include <QQuickImageProvider>
#include <QUrl>

// Qt Quick owns the provider and runs requests on its image-loading worker.
// List/GridView virtualization bounds live images; caching is explicitly
// capped.
class ThumbnailProvider final : public QQuickImageProvider {
public:
  ThumbnailProvider()
      : QQuickImageProvider(Image, ForceAsynchronousImageLoading),
        cache_(32 * 1024) {}
  int cacheKiB() {
    auto guard = QMutexLocker(&mutex_);
    return cache_.totalCost();
  }
  void clear() {
    auto guard = QMutexLocker(&mutex_);
    cache_.clear();
  }
  QImage requestImage(const QString &id, QSize *size, const QSize &) override {
    auto guard = QMutexLocker(&mutex_);
    if (const auto *cached = cache_.object(id)) {
      *size = cached->size();
      return *cached;
    }
    const auto split = id.indexOf('?');
    const auto path =
        QUrl::fromPercentEncoding((split < 0 ? id : id.left(split)).toUtf8());
    auto reader = QImageReader(path);
    reader.setAutoTransform(true);
    const auto sourceSize = reader.size();
    if (!sourceSize.isValid() || sourceSize.width() > 32768 ||
        sourceSize.height() > 32768)
      return {};
    reader.setScaledSize(sourceSize.scaled(160, 120, Qt::KeepAspectRatio));
    auto result = reader.read();
    if (result.isNull())
      return {};
    result =
        result.scaled(160, 120, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    *size = result.size();
    const auto cost = static_cast<int>((result.sizeInBytes() + 1023) / 1024);
    cache_.insert(id, std::make_unique<QImage>(result).release(), cost);
    return result;
  }

private:
  QMutex mutex_;
  QCache<QString, QImage> cache_;
};
