#pragma once
#include "formaview/image_batch.hpp"
#include <functional>

namespace formaview::imaging {
// Shared orchestration. Child plans retain at most 64 items per journal record.
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
  bool has_submission_key(const QString &key, const QString &exceptPlan = {}) const;
  bool contains(const QString &) const;
  bool has_active_work() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
