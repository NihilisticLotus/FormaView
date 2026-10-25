#pragma once
#include "formaview/image_batch.hpp"

namespace formaview::imaging {
// Desktop-only orchestration. Child plans keep the public 64-item contract.
// Creation/submission/recovery perform IO on a control worker; queries/cancel
// read cached snapshots and are safe on the GUI thread.
class ImageTaskGroups final {
public:
  ImageTaskGroups(AccessScope, std::filesystem::path journal);
  ~ImageTaskGroups();
  ImageTaskGroups(const ImageTaskGroups &) = delete;
  ImageTaskGroups &operator=(const ImageTaskGroups &) = delete;
  QJsonObject request(const QJsonObject &, std::stop_token = {});
  bool contains(const QString &) const;
  bool has_active_work() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
