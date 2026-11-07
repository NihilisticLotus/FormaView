#pragma once
#include "formaview/image_batch.hpp"
#include <functional>

namespace formaview::imaging {
// Desktop-only orchestration. Child plans keep the public 64-item contract.
// Creation/submission/recovery perform IO on a control worker; queries/cancel
// read cached snapshots and are safe on the GUI thread.
class ImageTaskGroups final {
public:
  ImageTaskGroups(AccessScope, std::filesystem::path journal, ImageBatchLimits = {});
  ~ImageTaskGroups();
  ImageTaskGroups(const ImageTaskGroups &) = delete;
  ImageTaskGroups &operator=(const ImageTaskGroups &) = delete;
  // Synchronous preparation notifications on the calling thread. Counts are
  // local to each stage; an absent total means work cannot be quantified yet.
  using PreparationProgress = std::function<void(const QJsonObject &)>;
  QJsonObject request(const QJsonObject &, std::stop_token = {}, PreparationProgress = {});
  bool contains(const QString &) const;
  bool has_active_work() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
