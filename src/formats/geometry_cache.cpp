#include "formaview/geometry_cache.hpp"
#include <Alembic/AbcCoreFactory/All.h>
#include <Alembic/AbcGeom/All.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cmath>
#include <fstream>
#include <functional>
#include <mutex>
#include <istream>
#include <set>
#include <span>
#include <streambuf>
#include <type_traits>

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
// Alembic reports unreadable archives with its own exceptions. Keep the
// structured contract: malformed input is invalid_argument and a failed
// library allocation is a resource limit, never an internal fault.
template <class F> decltype(auto) library(F &&body) {
  try {
    return body();
  } catch (const skeletal::Failure &) {
    throw;
  } catch (const std::bad_alloc &) {
    throw skeletal::Failure(ErrorCode::ResourceLimit, "ABC reader allocation failed");
  } catch (const std::filesystem::filesystem_error &error) {
    throw skeletal::Failure(ErrorCode::Io, std::string("ABC input unavailable: ") + error.what());
  } catch (const std::exception &error) {
    throw skeletal::Failure(ErrorCode::InvalidArgument,
                            std::string("ABC reader rejected the archive: ") + error.what());
  }
}
std::uint64_t count(const A::IArrayProperty &p,
                    const A::ISampleSelector &selector) {
  Alembic::Util::Dimensions dimensions;
  p.getDimensions(dimensions, selector);
  return dimensions.numPoints();
}
std::string key_of(A::IArrayProperty p, const A::ISampleSelector &s) {
  if (!p.valid())
    return {};
  Alembic::AbcCoreAbstract::ArraySampleKey key;
  if (!p.getKey(key, s))
    return {};
  return key.digest.str() + ":" + std::to_string(key.numBytes) + ":" +
         std::to_string(static_cast<int>(key.origPOD));
}
template <class Param>
void param_keys(Param param, const A::ISampleSelector &s,
                std::string &values, std::string &indices) {
  if (!param.valid())
    return;
  values = key_of(param.getValueProperty(), s);
  if (param.isIndexed())
    indices = key_of(param.getIndexProperty(), s);
}
SampleKeys keys_for(G::IPolyMeshSchema &schema,
                    const A::ISampleSelector &s) {
  SampleKeys keys;
  keys.positions = key_of(schema.getPositionsProperty(), s);
  keys.face_indices = key_of(schema.getFaceIndicesProperty(), s);
  keys.face_counts = key_of(schema.getFaceCountsProperty(), s);
  param_keys(schema.getUVsParam(), s, keys.uv, keys.uv_indices);
  param_keys(schema.getNormalsParam(), s, keys.normals, keys.normal_indices);
  return keys;
}
// World matrix of a mesh object from its transform ancestors.
Matrix4 world_of(G::IPolyMesh mesh, const A::ISampleSelector &selector) {
  auto world = skeletal::matrix({});
  // Alembic matrices act on row vectors; transpose at the contract boundary.
  for (auto parent = mesh.getParent(); parent.valid();
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
      world = skeletal::multiply(converted, world);
      if (!sample.getInheritsXforms())
        break;
    }
  }
  return world;
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
bool hdf5_signature(std::span<const char> bytes) {
  return bytes.size() >= 8 &&
         std::equal(bytes.begin(), bytes.begin() + 8, "\x89HDF\r\n\x1a\n");
}
// Ogawa bounds every group child count and data size by the stream size, but
// its std::istream reader reports an unlimited size, so forged counts in
// in-memory bytes reach vector allocations unchecked. Walk the reachable
// group graph once and require every count, size and position to fit the
// buffer; reject cycles, which no writer produces and readers may recurse on.
void validate_ogawa(std::span<const char> bytes, std::stop_token stop) {
  const auto malformed = [] {
    throw skeletal::Failure(ErrorCode::InvalidArgument,
                            "Malformed Ogawa structure: size or position "
                            "outside the archive");
  };
  const std::uint64_t size = bytes.size();
  const auto u64 = [&](std::uint64_t at) {
    if (at > size || size - at < 8)
      malformed();
    std::uint64_t value{};
    std::memcpy(&value, bytes.data() + at, sizeof value);
    return value;
  };
  if (size < 16 || std::string_view(bytes.data(), 5) != "Ogawa")
    return; // Not Ogawa: Alembic rejects it without trusting any count.
  constexpr std::uint64_t data_bit = 1ULL << 63;
  struct Frame {
    std::uint64_t position, children, next;
  };
  std::vector<Frame> path;
  std::set<std::uint64_t> finished, open;
  std::uint64_t entries = 0;
  const auto enter = [&](std::uint64_t position) {
    if (position == 0 || finished.contains(position))
      return; // 0 is the empty group.
    if (open.contains(position))
      throw skeletal::Failure(ErrorCode::InvalidArgument,
                              "Malformed Ogawa structure: group cycle");
    const auto children = u64(position);
    // Written groups never overlap, so all child entries together fit the
    // buffer; overlapping forged groups cannot multiply the walk.
    if (children > (size - position - 8) / 8 || children > size / 8 - entries)
      malformed();
    entries += children;
    open.insert(position);
    path.push_back({position, children, 0});
  };
  enter(u64(8));
  std::uint64_t steps = 0;
  while (!path.empty()) {
    if ((++steps & 4095) == 0)
      cancel(stop);
    auto &frame = path.back();
    if (frame.next == frame.children) {
      open.erase(frame.position);
      finished.insert(frame.position);
      path.pop_back();
      continue;
    }
    const auto child = u64(frame.position + 8 + 8 * frame.next++);
    if (child & data_bit) {
      const auto position = child & ~data_bit;
      if (position != 0 && u64(position) > size - position - 8)
        malformed();
    } else {
      enter(child);
    }
  }
}
// Read-only, seekable view of caller-owned bytes for Ogawa's stream reader.
class MemoryBuffer final : public std::streambuf {
public:
  explicit MemoryBuffer(std::span<const char> bytes) {
    auto *begin = const_cast<char *>(bytes.data());
    setg(begin, begin, begin + bytes.size());
  }

protected:
  pos_type seekoff(off_type offset, std::ios_base::seekdir dir,
                   std::ios_base::openmode which) override {
    if (!(which & std::ios_base::in))
      return pos_type(off_type(-1));
    // Integer positions: no pointer arithmetic outside the buffer.
    const off_type size = egptr() - eback();
    const off_type base = dir == std::ios_base::beg   ? 0
                          : dir == std::ios_base::cur ? gptr() - eback()
                                                      : size;
    if (offset < -base || offset > size - base)
      return pos_type(off_type(-1));
    setg(eback(), eback() + (base + offset), egptr());
    return pos_type(base + offset);
  }
  pos_type seekpos(pos_type position, std::ios_base::openmode which) override {
    return seekoff(off_type(position), std::ios_base::beg, which);
  }
};
} // namespace
struct Archive::Impl {
  Limits limits;
  // Memory sources: declared before the archive so they outlive it.
  std::shared_ptr<const void> owner;
  std::unique_ptr<MemoryBuffer> buffer;
  std::unique_ptr<std::istream> stream;
  A::IArchive archive;
  std::vector<G::IPolyMesh> objects;
  std::vector<MeshInfo> info;
  std::vector<SkippedObject> skipped;
  std::vector<double> times;
  std::string application;
  // Last decoded topology per mesh, identified by Alembic content digests.
  struct Memo {
    std::string indices, counts;
    std::shared_ptr<const std::vector<std::int32_t>> faces, corners;
    std::size_t max_point{};
  };
  std::vector<Memo> memo;
  std::uint64_t topology_reuses{};
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
const std::string &Archive::application() const { return impl_->application; }
UnitConvention unit_convention(std::string_view application) {
  std::string lower(application);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const auto has = [&](const char *name) {
    return lower.find(name) != std::string::npos;
  };
  if (has("maya") || has("cinema 4d") || has("cinema4d"))
    return {.01, true};
  if (has("houdini") || has("blender"))
    return {1, true};
  return {1, false};
}
Archive Archive::open(const std::filesystem::path &path, Limits limits,
                      std::stop_token stop) {
  return library([&]() -> Archive {
  cancel(stop);
  check(std::filesystem::file_size(path) <= limits.input_bytes,
        "ABC input budget exceeded");
  check(!hdf5_signature(path),
        "HDF5 Alembic archives are not supported; re-export as Ogawa",
        ErrorCode::Unsupported);
  auto p = std::make_unique<Impl>();
  p->limits = limits;
  F::IFactory factory;
  factory.setOgawaNumStreams(1);
  factory.setOgawaReadStrategy(F::IFactory::kFileStreams);
  F::IFactory::CoreType type;
  const auto utf8 = path.u8string();
  p->archive = factory.getArchive(std::string(utf8.begin(), utf8.end()), type);
  check(p->archive.valid() && type == F::IFactory::kOgawa,
        "Only Ogawa archives are supported", ErrorCode::Unsupported);
  return index(std::move(p), limits, stop);
  });
}
Archive Archive::open(std::span<const char> bytes,
                      std::shared_ptr<const void> owner, Limits limits,
                      std::stop_token stop) {
  return library([&]() -> Archive {
  cancel(stop);
  check(bytes.size() <= limits.input_bytes, "ABC input budget exceeded");
  check(!hdf5_signature(bytes),
        "HDF5 Alembic archives are not supported; re-export as Ogawa",
        ErrorCode::Unsupported);
  auto p = std::make_unique<Impl>();
  p->limits = limits;
  p->owner = std::move(owner);
  p->buffer = std::make_unique<MemoryBuffer>(bytes);
  validate_ogawa(bytes, stop);
  p->stream = std::make_unique<std::istream>(p->buffer.get());
  F::IFactory factory;
  F::IFactory::CoreType type;
  std::vector<std::istream *> streams{p->stream.get()};
  p->archive = factory.getArchive(streams, type);
  check(p->archive.valid() && type == F::IFactory::kOgawa,
        "Only Ogawa archives are supported", ErrorCode::Unsupported);
  return index(std::move(p), limits, stop);
  });
}
Archive Archive::index(std::unique_ptr<Impl> p, Limits limits,
                       std::stop_token stop) {
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
  p->memo.resize(p->objects.size());
  {
    std::string version, written, description;
    Alembic::Util::uint32_t number{};
    A::GetArchiveInfo(p->archive, p->application, version, number, written,
                      description);
  }
  return Archive(std::move(p));
}
Sample Archive::read(std::size_t mesh, double seconds,
                     std::stop_token stop) const {
  return library([&]() -> Sample {
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
  // Each parameter is decoded once: indices are validated against the
  // decoded values and the domain, then expanded here (no second Alembic
  // read and no intermediate expanded copy).
  const auto load_param = [&](auto param, auto &out, int &scope_out) {
    if (!param.valid())
      return;
    auto indexed = param.getIndexedValue(selector);
    const auto vals = indexed.getVals();
    check(static_cast<bool>(vals), "Missing ABC attribute values",
          ErrorCode::InvalidArgument);
    const auto indices = indexed.getIndices();
    if (param.isIndexed()) {
      check(static_cast<bool>(indices), "Missing ABC attribute indices",
            ErrorCode::InvalidArgument);
      for (std::size_t i = 0; i < indices->size(); ++i)
        check((*indices)[i] < vals->size(), "ABC attribute index invalid",
              ErrorCode::InvalidArgument);
    }
    const auto size = param.isIndexed() ? indices->size() : vals->size();
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
    scope_out = static_cast<int>(indexed.getScope());
    out.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
      const auto &v = (*vals)[param.isIndexed() ? (*indices)[i] : i];
      if constexpr (std::is_same_v<std::decay_t<decltype(out)>,
                                   std::vector<skeletal::Vec2>>)
        out.push_back({v.x, v.y});
      else
        out.push_back({v.x, v.y, v.z});
    }
    cancel(stop);
  };
  Sample result;
  result.index = static_cast<std::size_t>(selected.first);
  result.seconds = selected.second;
  result.keys = keys_for(schema, selector);
  {
    A::P3fArraySamplePtr positions;
    schema.getPositionsProperty().get(positions, selector);
    if (positions)
      for (const auto &v : std::span(positions->get(), positions->size()))
        result.positions.push_back({v.x, v.y, v.z});
  }
  cancel(stop);
  // Topology whose stored content digests equal the previous read is the
  // same data: reuse the validated arrays instead of decoding them again.
  auto &memo = impl_->memo[mesh];
  if (!result.keys.face_indices.empty() &&
      memo.indices == result.keys.face_indices &&
      memo.counts == result.keys.face_counts && memo.faces && memo.corners &&
      memo.faces->size() == faces && memo.corners->size() == corners) {
    result.face_sizes = *memo.faces;
    result.corner_points = *memo.corners;
    check(memo.corners->empty() || memo.max_point < result.positions.size(),
          "ABC point index invalid", ErrorCode::InvalidArgument);
    result.topology_reused = true;
    ++impl_->topology_reuses;
  } else {
    A::Int32ArraySamplePtr counts, indices;
    schema.getFaceCountsProperty().get(counts, selector);
    schema.getFaceIndicesProperty().get(indices, selector);
    if (counts)
      result.face_sizes.assign(counts->get(), counts->get() + counts->size());
    if (indices)
      result.corner_points.assign(indices->get(),
                                  indices->get() + indices->size());
    std::size_t sum = 0;
    for (auto n : result.face_sizes) {
      check(n >= 0, "Negative polygon size", ErrorCode::InvalidArgument);
      sum += static_cast<std::size_t>(n);
    }
    check(sum == result.corner_points.size(), "ABC corner domain mismatch",
          ErrorCode::InvalidArgument);
    std::size_t max_point = 0;
    for (auto i : result.corner_points) {
      check(i >= 0 && static_cast<std::size_t>(i) < result.positions.size(),
            "ABC point index invalid", ErrorCode::InvalidArgument);
      max_point = std::max(max_point, static_cast<std::size_t>(i));
    }
    memo = {result.keys.face_indices, result.keys.face_counts,
            std::make_shared<const std::vector<std::int32_t>>(result.face_sizes),
            std::make_shared<const std::vector<std::int32_t>>(result.corner_points),
            max_point};
  }
  check(result.face_sizes.size() == faces &&
            result.corner_points.size() == corners,
        "ABC corner domain mismatch", ErrorCode::InvalidArgument);
  cancel(stop);
  load_param(uv, result.uv, result.uv_scope);
  load_param(normals, result.normals, result.normal_scope);
  result.world = world_of(impl_->objects[mesh], selector);
  cancel(stop);
  return result;
  });
}
SampleKeys Archive::keys(std::size_t mesh, double seconds) const {
  return library([&]() -> SampleKeys {
  std::scoped_lock lock(impl_->reading);
  check(mesh < impl_->objects.size() && std::isfinite(seconds),
        "Invalid ABC sample request", ErrorCode::InvalidArgument);
  return keys_for(impl_->objects[mesh].getSchema(),
                  A::ISampleSelector(seconds, A::ISampleSelector::kNearIndex));
  });
}
Matrix4 Archive::world(std::size_t mesh, double seconds) const {
  return library([&]() -> Matrix4 {
  std::scoped_lock lock(impl_->reading);
  check(mesh < impl_->objects.size() && std::isfinite(seconds),
        "Invalid ABC sample request", ErrorCode::InvalidArgument);
  return world_of(impl_->objects[mesh],
                  A::ISampleSelector(seconds, A::ISampleSelector::kNearIndex));
  });
}
std::size_t Archive::sample_index(std::size_t mesh, double seconds) const {
  return library([&]() -> std::size_t {
  std::scoped_lock lock(impl_->reading);
  check(mesh < impl_->objects.size() && std::isfinite(seconds),
        "Invalid ABC sample request", ErrorCode::InvalidArgument);
  auto &schema = impl_->objects[mesh].getSchema();
  if (!schema.getNumSamples())
    return 0;
  return static_cast<std::size_t>(
      schema.getTimeSampling()->getNearIndex(seconds, schema.getNumSamples()).first);
  });
}
std::uint64_t Archive::topology_reuses() const {
  std::scoped_lock lock(impl_->reading);
  return impl_->topology_reuses;
}
} // namespace formaview::cache
