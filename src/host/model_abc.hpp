#pragma once
// Alembic (.abc) geometry-cache preview: one immutable ModelDocument per
// merged-timeline sample, read on demand by workers and kept in a bounded
// GUI-thread LRU cache. Only Ogawa polygon meshes are previewed.
#include "model_document.hpp"
#include "formaview/geometry_cache.hpp"
#include <QString>
#include <QVector3D>
#include <atomic>
#include <list>
#include <memory>
#include <optional>
#include <stop_token>

class ModelAbcSource : public std::enable_shared_from_this<ModelAbcSource> {
public:
  // Worker-only: leases and hashes the file, then indexes the archive.
  static std::shared_ptr<const ModelAbcSource> open(const QString &path,
                                                    std::stop_token);
  ~ModelAbcSource();
  [[nodiscard]] const QString &path() const { return path_; }
  [[nodiscard]] const QString &hash() const { return hash_; }
  [[nodiscard]] const std::vector<double> &times() const;
  [[nodiscard]] const std::vector<formaview::cache::MeshInfo> &meshes() const;
  [[nodiscard]] const std::vector<formaview::cache::SkippedObject> &skipped() const;
  // Timeline sample nearest to `seconds` (ties resolve to the earlier one).
  [[nodiscard]] std::size_t nearest(double seconds) const;
  // Worker-safe (reads are serialised by the archive). Builds the document
  // for timeline sample `index`; `display` fixes centre/radius so every
  // sample shares one display space (the first sample's when absent).
  [[nodiscard]] std::shared_ptr<ModelDocument>
  read(std::size_t index, std::stop_token,
       std::optional<std::pair<QVector3D, float>> display = {}) const;
  // Completed sample reads (verification instrument).
  [[nodiscard]] std::uint64_t reads() const { return reads_.load(); }

private:
  ModelAbcSource();
  struct Impl;
  std::unique_ptr<Impl> impl_;
  QString path_, hash_;
  mutable std::atomic<std::uint64_t> reads_{};
};

// Approximate resident bytes of one sample document (cache accounting).
std::uint64_t modelAbcDocumentBytes(const ModelDocument &);

// GUI-thread LRU of sample documents of one archive. Bounded by bytes and
// count; the most recently inserted document is always retained even when it
// alone exceeds the byte budget (it is the one being displayed).
class ModelAbcCache {
public:
  explicit ModelAbcCache(std::uint64_t maxBytes = 256ULL * 1024 * 1024,
                         std::size_t maxSamples = 64)
      : maxBytes_(maxBytes), maxSamples_(maxSamples) {}
  std::shared_ptr<ModelDocument> find(std::size_t index);
  [[nodiscard]] bool contains(std::size_t index) const;
  void insert(std::shared_ptr<ModelDocument>);
  void clear();
  void setLimits(std::uint64_t maxBytes, std::size_t maxSamples);
  [[nodiscard]] std::size_t size() const { return entries_.size(); }
  [[nodiscard]] std::uint64_t bytes() const { return bytes_; }
  [[nodiscard]] std::uint64_t maxBytes() const { return maxBytes_; }
  [[nodiscard]] std::size_t maxSamples() const { return maxSamples_; }
  [[nodiscard]] std::uint64_t hits() const { return hits_; }
  [[nodiscard]] std::uint64_t evictions() const { return evictions_; }

private:
  struct Entry {
    std::size_t index;
    std::shared_ptr<ModelDocument> document;
    std::uint64_t bytes;
  };
  void trim();
  std::list<Entry> entries_;
  std::uint64_t bytes_{}, maxBytes_, hits_{}, evictions_{};
  std::size_t maxSamples_;
};
