#pragma once
#include "formaview/image_batch.hpp"
#include <QObject>
#include <functional>

namespace formaview::imaging {
// Explicitly enabled local transport over an existing shared job service.
// Stopping this endpoint disconnects clients; it does not own job cancellation.
class ImageEndpoint final : public QObject {
public:
  ImageEndpoint(std::shared_ptr<ImageBatchService>, const QString &name,
                QObject *parent = nullptr);
  ~ImageEndpoint() override;
  void stop();
  [[nodiscard]] bool idle() const;
  std::function<void(const QJsonObject &)> observedClient;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace formaview::imaging
