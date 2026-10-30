#pragma once
#include <QQuickAsyncImageProvider>
#include <memory>

// One bounded worker; cancellation never publishes stale pixels.
// Only derived thumbnails are cached (32 MiB), never full source images.
class ThumbnailProvider final : public QQuickAsyncImageProvider {
public:
  ThumbnailProvider();
  ~ThumbnailProvider() override;
  QQuickImageResponse *requestImageResponse(const QString &,
                                            const QSize &) override;
  int cacheKiB() const;
  void clear();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
