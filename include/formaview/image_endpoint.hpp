#pragma once
#include "formaview/image_batch.hpp"
#include <QObject>
#include <functional>

namespace formaview::imaging {
class ImageTaskGroups;
// Explicitly enabled local transport over an existing shared job service.
// Stopping this endpoint disconnects clients; it does not own job cancellation.
class ImageEndpoint final : public QObject {
public:
  ImageEndpoint(std::shared_ptr<ImageBatchService>, const QString &name,
                QObject *parent = nullptr, std::shared_ptr<ImageTaskGroups> groups = {});
  ~ImageEndpoint() override;
  void stop();
  [[nodiscard]] bool idle() const;
  std::function<void(const QJsonObject &)> observedClient;
  // Delivered on this object's thread after a state-changing request. Hosts
  // can resume observation after becoming idle, including metadata-free clients.
  std::function<void()> workChanged;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace formaview::imaging
