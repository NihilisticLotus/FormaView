#include "formaview/geometry_cache.hpp"
#include <Alembic/AbcCoreFactory/All.h>
#include <Alembic/AbcGeom/All.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>

namespace formaview::cache {
namespace {
namespace A = Alembic::Abc;
namespace G = Alembic::AbcGeom;
namespace F = Alembic::AbcCoreFactory;
void check(bool ok, const std::string &text,
           ErrorCode code = ErrorCode::ResourceLimit) {
  if (!ok)
    throw skeletal::Failure(code, text);
}
void cancel(std::stop_token s) {
  check(!s.stop_requested(), "ABC cancelled", ErrorCode::Cancelled);
}
std::uint64_t count(const A::IArrayProperty &p,
                    const A::ISampleSelector &selector) {
  Alembic::Util::Dimensions dimensions;
  p.getDimensions(dimensions, selector);
  return dimensions.numPoints();
}
std::string schema_name(const A::IObject &object) {
  const auto &meta = object.getMetaData();
  auto schema = meta.get("schema");
  if (schema.empty())
    schema = meta.get("schemaObjTitle");
  return schema;
}
bool hdf5_signature(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  char head[8]{};
  file.read(head, sizeof head);
  return file.gcount() == 8 &&
         std::equal(head, head + 8, "\x89HDF\r\n\x1a\n");
}
} // namespace
struct Archive::Impl {
  Limits limits;
  A::IArchive archive;
  std::vector<G::IPolyMesh> objects;
  std::vector<MeshInfo> info;
  std::vector<SkippedObject> skipped;
  std::vector<double> times;
  mutable std::mutex reading;
};
Archive::Archive(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
Archive::Archive(Archive &&) noexcept = default;
Archive &Archive::operator=(Archive &&) noexcept = default;
Archive::~Archive() = default;
const std::vector<MeshInfo> &Archive::meshes() const { return impl_->info; }
const std::vector<SkippedObject> &Archive::skipped() const {
  return impl_->skipped;
}
const std::vector<double> &Archive::times() const { return impl_->times; }
Archive Archive::open(const std::filesystem::path &path, Limits limits,
                      std::stop_token stop) {
  cancel(stop);
  check(std::filesystem::file_size(path) <= limits.input_bytes,
        "ABC input budget exceeded");
  auto p = std::make_unique<Impl>();
  p->limits = limits;
  check(!hdf5_signature(path),
        "HDF5 Alembic archives are not supported; re-export as Ogawa",
        ErrorCode::Unsupported);
  F::IFactory factory;
  factory.setOgawaNumStreams(1);
  factory.setOgawaReadStrategy(F::IFactory::kFileStreams);
  F::IFactory::CoreType type;
  const auto utf8 = path.u8string();
  p->archive = factory.getArchive(std::string(utf8.begin(), utf8.end()), type);
  check(p->archive.valid() && type == F::IFactory::kOgawa,
        "Only Ogawa archives are supported", ErrorCode::Unsupported);
  std::uint32_t visited = 0;
  std::set<double> times;
  const auto add_times = [&](const A::TimeSamplingPtr &sampling,
                             std::size_t samples) {
    check(samples <= limits.samples, "ABC sample count budget exceeded");
    for (std::size_t i = 0; i < samples; ++i) {
      times.insert(sampling->getSampleTime(static_cast<A::index_t>(i)));
      check(times.size() <= limits.samples, "ABC timeline budget exceeded");
    }
  };
  std::function<void(A::IObject, unsigned)> visit = [&](A::IObject object,
                                                        unsigned depth) {
    cancel(stop);
    check(++visited <= limits.objects && depth <= 256,
          "ABC hierarchy budget exceeded");
    if (G::IPolyMesh::matches(object.getHeader())) {
      G::IPolyMesh mesh(object, A::kWrapExisting);
      auto &schema = mesh.getSchema();
      MeshInfo info;
      info.path = object.getFullName();
      const auto variance = schema.getTopologyVariance();
      info.topology = variance == G::kHeterogenousTopology ? Topology::Varying
                                                           : Topology::Fixed;
      info.constant = variance == G::kConstantTopology;
      check(schema.getNumSamples() <= limits.samples,
            "ABC sample count budget exceeded");
      for (std::size_t i = 0; i < schema.getNumSamples(); ++i)
        info.times.push_back(schema.getTimeSampling()->getSampleTime(
            static_cast<A::index_t>(i)));
      if (schema.getNumSamples() > 1)
        add_times(schema.getTimeSampling(), schema.getNumSamples());
      info.uv = schema.getUVsParam().valid();
      info.normals = schema.getNormalsParam().valid();
      p->objects.push_back(mesh);
      p->info.push_back(std::move(info));
      // Face sets and other mesh children carry no separate geometry here.
      return;
    }
    if (G::IXform::matches(object.getHeader())) {
      G::IXform xform(object, A::kWrapExisting);
      auto &schema = xform.getSchema();
      if (!schema.isConstant() && schema.getNumSamples() > 1)
        add_times(schema.getTimeSampling(), schema.getNumSamples());
    } else if (object.getParent().valid()) {
      const auto schema = schema_name(object);
      // Schema-less objects are plain groups; anything else is recognised
      // but not previewed, and reported instead of failing the archive.
      if (!schema.empty()) {
        check(p->skipped.size() < limits.objects,
              "ABC hierarchy budget exceeded");
        p->skipped.push_back({object.getFullName(), schema});
        return;
      }
    }
    for (std::size_t i = 0; i < object.getNumChildren(); ++i)
      visit(object.getChild(i), depth + 1);
  };
  visit(p->archive.getTop(), 0);
  cancel(stop);
  if (p->info.empty()) {
    std::string kinds;
    for (const auto &item : p->skipped)
      if (kinds.find(item.schema) == std::string::npos)
        kinds += (kinds.empty() ? "" : ", ") + item.schema;
    check(false,
          kinds.empty()
              ? "ABC archive contains no polygon meshes"
              : "ABC archive contains no polygon meshes; unsupported "
                "schemas: " + kinds,
          ErrorCode::Unsupported);
  }
  if (times.empty())
    for (const auto &info : p->info)
      if (!info.times.empty())
        times.insert(info.times.front());
  if (times.empty())
    times.insert(0);
  p->times.assign(times.begin(), times.end());
  return Archive(std::move(p));
}
Sample Archive::read(std::size_t mesh, double seconds,
                     std::stop_token stop) const {
  std::scoped_lock lock(impl_->reading);
  cancel(stop);
  check(mesh < impl_->objects.size() && std::isfinite(seconds),
        "Invalid ABC sample request", ErrorCode::InvalidArgument);
  auto &schema = impl_->objects[mesh].getSchema();
  check(schema.getNumSamples() > 0, "ABC mesh has no samples",
        ErrorCode::Unsupported);
  const auto selected =
      schema.getTimeSampling()->getNearIndex(seconds, schema.getNumSamples());
  const A::ISampleSelector selector(seconds, A::ISampleSelector::kNearIndex);
  const auto points = count(schema.getPositionsProperty(), selector);
  const auto corners = count(schema.getFaceIndicesProperty(), selector);
  const auto faces = count(schema.getFaceCountsProperty(), selector);
  const auto cap = impl_->limits.sample_bytes;
  check(points <= cap / 48 && corners <= cap / 48 && faces <= cap / 8,
        "ABC geometry budget exceeded");
  std::uint64_t estimated = points * 48 + corners * 8 + faces * 8;
  const auto uv = schema.getUVsParam();
  const auto normals = schema.getNormalsParam();
  const auto check_param = [&](auto param, std::uint64_t stride) {
    if (!param.valid())
      return;
    const auto values = count(param.getValueProperty(), selector);
    const auto indices =
        param.isIndexed() ? count(param.getIndexProperty(), selector) : values;
    check(values <= cap / stride && indices <= cap / stride,
          "ABC attribute budget exceeded");
    estimated += values * stride + indices * (stride + 4);
  };
  check_param(uv, 16);
  check_param(normals, 24);
  check(estimated <= cap, "ABC aggregate sample budget exceeded");
  // Validate indexed parameters before Alembic expands them by indexing values.
  const auto validate_param = [&](auto param) {
    if (!param.valid())
      return;
    auto indexed = param.getIndexedValue(selector);
    check(static_cast<bool>(indexed.getVals()), "Missing ABC attribute values",
          ErrorCode::InvalidArgument);
    const auto indices = indexed.getIndices();
    if (param.isIndexed()) {
      check(static_cast<bool>(indices), "Missing ABC attribute indices",
            ErrorCode::InvalidArgument);
      for (std::size_t i = 0; i < indices->size(); ++i)
        check((*indices)[i] < indexed.getVals()->size(),
              "ABC attribute index invalid", ErrorCode::InvalidArgument);
    }
    const auto size =
        param.isIndexed() ? indices->size() : indexed.getVals()->size();
    std::uint64_t expected = 0;
    switch (indexed.getScope()) {
    case G::kConstantScope:
      expected = 1;
      break;
    case G::kUniformScope:
      expected = faces;
      break;
    case G::kVertexScope:
    case G::kVaryingScope:
      expected = points;
      break;
    case G::kFacevaryingScope:
      expected = corners;
      break;
    default:
      check(false, "Unsupported ABC attribute scope", ErrorCode::Unsupported);
    }
    check(size == expected, "ABC attribute domain mismatch",
          ErrorCode::InvalidArgument);
    cancel(stop);
  };
  validate_param(uv);
  validate_param(normals);
  G::IPolyMeshSchema::Sample raw;
  schema.get(raw, selector);
  cancel(stop);
  Sample result;
  result.index = static_cast<std::size_t>(selected.first);
  result.seconds = selected.second;
  if (raw.getPositions())
    for (const auto &v :
         std::span(raw.getPositions()->get(), raw.getPositions()->size()))
      result.positions.push_back({v.x, v.y, v.z});
  if (raw.getFaceCounts())
    result.face_sizes.assign(raw.getFaceCounts()->get(),
                             raw.getFaceCounts()->get() +
                                 raw.getFaceCounts()->size());
  if (raw.getFaceIndices())
    result.corner_points.assign(raw.getFaceIndices()->get(),
                                raw.getFaceIndices()->get() +
                                    raw.getFaceIndices()->size());
  std::size_t sum = 0;
  for (auto n : result.face_sizes) {
    check(n >= 0, "Negative polygon size", ErrorCode::InvalidArgument);
    sum += static_cast<std::size_t>(n);
  }
  check(sum == result.corner_points.size(), "ABC corner domain mismatch",
        ErrorCode::InvalidArgument);
  for (auto i : result.corner_points)
    check(i >= 0 && static_cast<std::size_t>(i) < result.positions.size(),
          "ABC point index invalid", ErrorCode::InvalidArgument);
  if (uv.valid()) {
    auto expanded = uv.getExpandedValue(selector);
    result.uv_scope = static_cast<int>(expanded.getScope());
    for (const auto &v :
         std::span(expanded.getVals()->get(), expanded.getVals()->size()))
      result.uv.push_back({v.x, v.y});
  }
  if (normals.valid()) {
    auto expanded = normals.getExpandedValue(selector);
    result.normal_scope = static_cast<int>(expanded.getScope());
    for (const auto &v :
         std::span(expanded.getVals()->get(), expanded.getVals()->size()))
      result.normals.push_back({v.x, v.y, v.z});
  }
  result.world = skeletal::matrix({});
  // Alembic matrices act on row vectors; transpose at the contract boundary.
  for (auto parent = impl_->objects[mesh].getParent(); parent.valid();
       parent = parent.getParent()) {
    if (G::IXform::matches(parent.getHeader())) {
      G::IXform x(parent, A::kWrapExisting);
      auto sample = x.getSchema().getValue(selector);
      const auto m = sample.getMatrix();
      Matrix4 converted;
      for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t col = 0; col < 4; ++col)
          converted[row * 4 + col] =
              m[static_cast<int>(col)][static_cast<int>(row)];
      result.world = skeletal::multiply(converted, result.world);
      if (!sample.getInheritsXforms())
        break;
    }
  }
  cancel(stop);
  return result;
}
} // namespace formaview::cache
