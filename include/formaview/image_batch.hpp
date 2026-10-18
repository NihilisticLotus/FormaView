#pragma once
#include "formaview/image_requests.hpp"
#include <memory>

namespace formaview::imaging {
// Process-wide admission for allocating image work. Cached GUI publication and
// lightweight job queries do not acquire it. Never acquire it on the GUI thread
// or recursively around ImageBatchService::request().
class ImageAdmission final {
public:
  explicit ImageAdmission(bool foreground, std::stop_token = {});
  ~ImageAdmission();
  ImageAdmission(const ImageAdmission &) = delete;
  ImageAdmission &operator=(const ImageAdmission &) = delete;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
struct ImageBatchLimits {
  std::size_t records{128}, items{64}, queue{16};
  Limits image{512ULL * 1024 * 1024, 512ULL * 1024 * 1024, 16384, 65536, 256};
};
// One owner per journal directory. Mutations may flush journal data and belong
// on a control worker; get_plan/get_job/list_jobs/cancel only inspect memory.
// Disconnecting a client does not own or cancel a batch.
class ImageBatchService final {
public:
  ImageBatchService(AccessScope, std::filesystem::path journal,
                    ImageBatchLimits = {});
  ~ImageBatchService();
  ImageBatchService(const ImageBatchService &) = delete;
  ImageBatchService &operator=(const ImageBatchService &) = delete;
  [[nodiscard]] QJsonObject request(const QJsonObject &, std::stop_token = {});
  [[nodiscard]] bool has_active_work() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace formaview::imaging
