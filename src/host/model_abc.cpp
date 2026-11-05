#include "model_abc.hpp"
#include "formaview/file_access.hpp"
#include "model_metadata.hpp"
#include <QFileInfo>
#include <QMatrix4x4>
#include <algorithm>
#include <cmath>
#include <iterator>

using namespace formaview;
namespace {
void check(std::stop_token stop) {
  if (stop.stop_requested())
    throw skeletal::Failure(ErrorCode::Cancelled, "ABC cancelled");
}
#ifdef FORMAVIEW_MODEL_ABC
QMatrix4x4 qmatrix(const Matrix4 &m) {
  QMatrix4x4 result;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      result(r, c) = static_cast<float>(m[static_cast<std::size_t>(r * 4 + c)]);
  return result;
}
QString leaf(const std::string &path) {
  const auto full = QString::fromStdString(path);
  const auto slash = full.lastIndexOf('/');
  const auto name = slash >= 0 ? full.mid(slash + 1) : full;
  return name.isEmpty() ? full : name;
}
// Value for one corner of `face` referencing `point`, by GeomParam scope.
template <class T>
const T *scoped(const std::vector<T> &values, int scope, std::size_t corner,
                std::size_t face, std::size_t point) {
  std::size_t at = 0;
  switch (static_cast<cache::Scope>(scope)) {
  case cache::Scope::Constant:
    at = 0;
    break;
  case cache::Scope::Uniform:
    at = face;
    break;
  case cache::Scope::Varying:
  case cache::Scope::Vertex:
    at = point;
    break;
  case cache::Scope::FaceVarying:
    at = corner;
    break;
  default:
    return nullptr;
  }
  return at < values.size() ? &values[at] : nullptr;
}
#endif
} // namespace

#ifdef FORMAVIEW_MODEL_ABC
struct ModelAbcSource::Impl {
  InputLease lease;
  cache::Archive archive;
};
ModelAbcSource::ModelAbcSource() = default;
ModelAbcSource::~ModelAbcSource() = default;
const std::vector<double> &ModelAbcSource::times() const {
  return impl_->archive.times();
}
const std::string &ModelAbcSource::application() const {
  return impl_->archive.application();
}
const std::vector<cache::MeshInfo> &ModelAbcSource::meshes() const {
  return impl_->archive.meshes();
}
const std::vector<cache::SkippedObject> &ModelAbcSource::skipped() const {
  return impl_->archive.skipped();
}
std::size_t ModelAbcSource::nearest(double seconds) const {
  const auto &t = times();
  if (t.empty() || !std::isfinite(seconds))
    return 0;
  const auto it = std::lower_bound(t.begin(), t.end(), seconds);
  if (it == t.begin())
    return 0;
  if (it == t.end())
    return t.size() - 1;
  const auto after = static_cast<std::size_t>(it - t.begin());
  return seconds - t[after - 1] <= *it - seconds ? after - 1 : after;
}
std::shared_ptr<const ModelAbcSource> ModelAbcSource::open(const QString &path,
                                                           std::stop_token stop) {
  const std::filesystem::path source(path.toStdWString());
  AccessScope scope;
  scope.input_roots = {source.parent_path()};
  const cache::Limits limits;
  // The lease is held for the archive's lifetime: every sample read comes
  // from the content identified by this hash.
  auto lease = InputLease::open(scope, source, limits.input_bytes, stop);
  check(stop);
  auto archive = cache::Archive::open(source, limits, stop);
  check(stop);
  std::shared_ptr<ModelAbcSource> result(new ModelAbcSource);
  result->impl_.reset(new Impl{std::move(lease), std::move(archive)});
  result->path_ = path;
  result->hash_ =
      QString::fromStdString(result->impl_->lease.snapshot().content_sha256);
  return result;
}
std::shared_ptr<ModelDocument>
ModelAbcSource::read(std::size_t index, std::stop_token stop,
                     std::optional<std::pair<QVector3D, float>> display,
                     std::shared_ptr<const ModelDocument> reference) const {
  check(stop);
  const auto &timeline = times();
  if (index >= timeline.size())
    throw skeletal::Failure(ErrorCode::InvalidArgument,
                            "ABC sample index outside the timeline");
  const double seconds = timeline[index];
  auto d = std::make_shared<ModelDocument>();
  d->geometryCache = shared_from_this();
  d->cacheSample = index;
  d->hash = hash_;
  d->coordinates = QStringLiteral(
      "Alembic Ogawa 路 Y up convention 路 units unspecified 路 clockwise "
      "polygon winding");
  const auto &infos = meshes();
  // A neighbouring document of the same archive lets unchanged data be
  // reused: identical stored arrays (Alembic content digests) and the same
  // world matrix mean an identical mesh; identical topology digests mean the
  // triangulation and every corner-domain array except UVs/normals carry over.
  const bool related = reference && reference->geometryCache.get() == this &&
                       reference->meshes.size() == infos.size() &&
                       reference->cacheMeshKeys.size() == infos.size() &&
                       reference->cacheMeshWorlds.size() == infos.size() &&
                       reference->cacheMeshFaces.size() == infos.size();
  for (std::size_t mi = 0; mi < infos.size(); ++mi) {
    check(stop);
    const auto &info = infos[mi];
    const auto nodeId = QString("node/%1").arg(mi);
    const auto appendNode = [&](const Matrix4 &matrix) {
      const auto world = modelMatrixValues(matrix);
      d->nodes.append(QVariantMap{{"id", nodeId},
                                  {"stableId", QString::fromStdString(info.path)},
                                  {"name", QString::fromStdString(info.path)},
                                  {"parent", QString{}},
                                  {"bone", false},
                                  {"local", world},
                                  {"world", world},
                                  {"geometryWorld", world},
                                  {"active", true}});
    };
    const ModelMesh *previous = related ? &reference->meshes[mi] : nullptr;
    if (previous) {
      const auto keys = impl_->archive.keys(mi, seconds);
      const auto matrix = impl_->archive.world(mi, seconds);
      if (keys.same_content(reference->cacheMeshKeys[mi]) &&
          matrix == reference->cacheMeshWorlds[mi]) {
        auto m = *previous;
        // Derived per-document annotations are recomputed by finalize.
        m.uvBounds.clear();
        m.invalidUVs.clear();
        m.classificationCandidate.clear();
        d->cacheMeshSamples.push_back(impl_->archive.sample_index(mi, seconds));
        d->cacheMeshKeys.push_back(keys);
        d->cacheMeshWorlds.push_back(matrix);
        d->cacheMeshFaces.push_back(reference->cacheMeshFaces[mi]);
        d->cacheFaces += reference->cacheMeshFaces[mi];
        d->cacheCorners += m.corners.size();
        appendNode(matrix);
        d->meshes.push_back(std::move(m));
        ++meshReuses_;
        continue;
      }
    }
    const auto sample = impl_->archive.read(mi, seconds, stop);
    ++meshDecodes_;
    d->cacheMeshSamples.push_back(sample.index);
    d->cacheMeshKeys.push_back(sample.keys);
    d->cacheMeshWorlds.push_back(sample.world);
    d->cacheMeshFaces.push_back(sample.face_sizes.size());
    appendNode(sample.world);
    ModelMesh m;
    m.name = leaf(info.path);
    m.id = QString::fromStdString(info.path);
    m.nodeId = nodeId;
    const auto transform = qmatrix(sample.world);
    const auto normalMatrix = transform.normalMatrix();
    m.raw = sample.positions;
    m.points.reserve(sample.positions.size());
    for (const auto &v : sample.positions)
      m.points.push_back(transform.map(QVector3D(static_cast<float>(v.x),
                                                 static_cast<float>(v.y),
                                                 static_cast<float>(v.z))));
    m.materials = {QStringLiteral("Default")};
    m.references = {QString{}};
    m.textures.resize(1);
    m.texturePaths = QStringList{QString{}};
    m.materialUv = {0};
    m.alphaModes = {0};
    m.alphaCutoffs = {.5F};
    m.uvAffine.push_back({1, 0, 0, 0, 1, 0});
    const bool hasUv = !sample.uv.empty();
    const bool hasNormals = !sample.normals.empty();
    if (hasUv) {
      m.uvNames.append(QStringLiteral("uv/0/st"));
      m.uvs.emplace_back().reserve(sample.corner_points.size());
    }
    const auto corners = sample.corner_points.size();
    const bool sameTopology =
        previous && sample.keys.same_topology(reference->cacheMeshKeys[mi]) &&
        previous->corners.size() == corners &&
        reference->cacheMeshFaces[mi] == sample.face_sizes.size();
    if (sameTopology) {
      m.corners = previous->corners;
      m.faces = previous->faces;
      m.triangles = previous->triangles;
      m.triangleFaces = previous->triangleFaces;
      m.triangleCorners = previous->triangleCorners;
      m.colours = previous->colours;
      m.alphas = previous->alphas;
      m.cornerMaterial = previous->cornerMaterial;
      ++topologyReuses_;
    } else {
      m.corners.reserve(corners);
      m.faces.reserve(corners);
      m.colours.assign(corners, QVector3D(.7F, .7F, .7F));
      m.alphas.assign(corners, 1.F);
      m.cornerMaterial.assign(corners, 0);
    }
    m.normals.reserve(corners);
    std::size_t corner = 0;
    for (std::size_t face = 0; face < sample.face_sizes.size(); ++face) {
      if ((face & 4095) == 0)
        check(stop);
      const auto count = static_cast<std::size_t>(sample.face_sizes[face]);
      const auto first = corner;
      for (std::size_t k = 0; k < count; ++k, ++corner) {
        const auto point = static_cast<std::size_t>(sample.corner_points[corner]);
        if (!sameTopology) {
          m.corners.push_back(static_cast<std::uint32_t>(point));
          m.faces.push_back(face);
        }
        if (hasUv) {
          const auto *uv = scoped(sample.uv, sample.uv_scope, corner, face, point);
          m.uvs.front().emplace_back(uv ? uv->x : 0., uv ? uv->y : 0.);
        }
        QVector3D n;
        if (hasNormals)
          if (const auto *v = scoped(sample.normals, sample.normal_scope,
                                     corner, face, point))
            for (int r = 0; r < 3; ++r)
              n[r] = normalMatrix(r, 0) * static_cast<float>(v->x) +
                     normalMatrix(r, 1) * static_cast<float>(v->y) +
                     normalMatrix(r, 2) * static_cast<float>(v->z);
        m.normals.push_back(n.normalized());
      }
      if (sameTopology)
        continue;
      // Alembic polygons wind clockwise; the fan reverses the display order
      // while corners, UVs and normals keep the source order.
      for (std::size_t k = 1; k + 1 < count; ++k) {
        m.triangles.push_back(static_cast<std::uint32_t>(first));
        m.triangles.push_back(static_cast<std::uint32_t>(first + k + 1));
        m.triangles.push_back(static_cast<std::uint32_t>(first + k));
        m.triangleFaces.push_back(face);
        m.triangleCorners.push_back({0, k + 1, k});
      }
    }
    d->cacheFaces += sample.face_sizes.size();
    d->cacheCorners += corners;
    d->meshes.push_back(std::move(m));
  }
  constexpr std::size_t listed = 20;
  const auto &skipped = this->skipped();
  for (std::size_t i = 0; i < skipped.size() && i < listed; ++i)
    d->warnings.append(QStringLiteral("ABC object not previewed: %1 (%2)")
                           .arg(QString::fromStdString(skipped[i].path),
                                QString::fromStdString(skipped[i].schema)));
  if (skipped.size() > listed)
    d->warnings.append(QStringLiteral("ABC objects not previewed: %1 more")
                           .arg(skipped.size() - listed));
  d->name = QFileInfo(path_).fileName();
  finalizeModelGeometry(*d, stop, true);
  if (display) {
    d->center = display->first;
    d->radius = display->second;
  }
  check(stop);
  ++reads_;
  return d;
}

#else
struct ModelAbcSource::Impl {};
ModelAbcSource::ModelAbcSource() = default;
ModelAbcSource::~ModelAbcSource() = default;
const std::vector<double> &ModelAbcSource::times() const {
  static const std::vector<double> none;
  return none;
}
const std::string &ModelAbcSource::application() const {
  static const std::string none;
  return none;
}
const std::vector<cache::MeshInfo> &ModelAbcSource::meshes() const {
  static const std::vector<cache::MeshInfo> none;
  return none;
}
const std::vector<cache::SkippedObject> &ModelAbcSource::skipped() const {
  static const std::vector<cache::SkippedObject> none;
  return none;
}
std::size_t ModelAbcSource::nearest(double) const { return 0; }
std::shared_ptr<const ModelAbcSource> ModelAbcSource::open(const QString &,
                                                           std::stop_token) {
  throw skeletal::Failure(ErrorCode::Unsupported,
                          "ABC preview is not available in this build");
}
std::shared_ptr<ModelDocument>
ModelAbcSource::read(std::size_t, std::stop_token,
                     std::optional<std::pair<QVector3D, float>>,
                     std::shared_ptr<const ModelDocument>) const {
  throw skeletal::Failure(ErrorCode::Unsupported,
                          "ABC preview is not available in this build");
}
#endif

std::uint64_t modelAbcDocumentBytes(const ModelDocument &d) {
  std::uint64_t bytes = sizeof(ModelDocument);
  for (const auto &m : d.meshes) {
    bytes += m.raw.size() * sizeof(skeletal::Vec3) +
             m.points.size() * sizeof(QVector3D);
    bytes += m.corners.size() *
             (sizeof(std::uint32_t) + sizeof(std::uint64_t) +
              2 * sizeof(QVector3D) + sizeof(float) + sizeof(std::uint32_t));
    for (const auto &uv : m.uvs)
      bytes += uv.size() * sizeof(QPointF);
    bytes += m.triangles.size() * sizeof(std::uint32_t) +
             m.triangleFaces.size() * sizeof(std::uint64_t) +
             m.triangleCorners.size() * sizeof(std::array<std::uint64_t, 3>);
  }
  return bytes;
}

std::shared_ptr<ModelDocument> ModelAbcCache::find(std::size_t index) {
  const auto it = std::find_if(entries_.begin(), entries_.end(),
                               [&](const Entry &e) { return e.index == index; });
  if (it == entries_.end())
    return nullptr;
  entries_.splice(entries_.begin(), entries_, it);
  ++hits_;
  return entries_.front().document;
}
std::shared_ptr<const ModelDocument>
ModelAbcCache::nearest(std::size_t index) const {
  const Entry *best = nullptr;
  std::size_t distance = 0;
  for (const auto &e : entries_) {
    const auto d = e.index > index ? e.index - index : index - e.index;
    if (!best || d < distance) {
      best = &e;
      distance = d;
    }
  }
  return best ? best->document : nullptr;
}
bool ModelAbcCache::contains(std::size_t index) const {
  return std::any_of(entries_.begin(), entries_.end(),
                     [&](const Entry &e) { return e.index == index; });
}
void ModelAbcCache::insert(std::shared_ptr<ModelDocument> document) {
  if (!document)
    return;
  const auto index = document->cacheSample;
  const auto it = std::find_if(entries_.begin(), entries_.end(),
                               [&](const Entry &e) { return e.index == index; });
  if (it != entries_.end()) {
    entries_.splice(entries_.begin(), entries_, it);
    return;
  }
  const auto bytes = modelAbcDocumentBytes(*document);
  entries_.push_front({index, std::move(document), bytes});
  bytes_ += bytes;
  trim();
}
void ModelAbcCache::trim() {
  while (entries_.size() > 1 &&
         (entries_.size() > maxSamples_ || bytes_ > maxBytes_)) {
    bytes_ -= entries_.back().bytes;
    entries_.pop_back();
    ++evictions_;
  }
}
void ModelAbcCache::clear() {
  entries_.clear();
  bytes_ = 0;
}
void ModelAbcCache::setLimits(std::uint64_t maxBytes, std::size_t maxSamples) {
  maxBytes_ = maxBytes;
  maxSamples_ = std::max<std::size_t>(maxSamples, 1);
  trim();
}
