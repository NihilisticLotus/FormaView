#include <span>
#include "formaview/file_access.hpp"
#include "formaview/gltf_anim.hpp"
#include "../formats/gltf_storage.hpp"
#include "model_document.hpp"
#include "model_metadata.hpp"
#include "model_texture.hpp"
#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <algorithm>
#include <cgltf.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <numeric>
#include <set>

namespace {
using namespace formaview;
[[noreturn]] void fail(const char *message) {
  throw skeletal::Failure(ErrorCode::InvalidArgument, message);
}
void check(std::stop_token stop) {
  if (stop.stop_requested())
    throw skeletal::Failure(ErrorCode::Cancelled, "glTF loading cancelled");
}
QString name(const char *value) {
  return value ? QString::fromUtf8(value) : QString{};
}
struct Budget {
  std::size_t live{}, decoded{};
  std::stop_token stop;
  void reserve(std::size_t n) {
    check(stop);
    if (n > 1024ULL * 1024 * 1024 - decoded)
      throw skeletal::Failure(ErrorCode::ResourceLimit,
                              "glTF decoded data budget exceeded");
    decoded += n;
  }
  static void *allocate(void *user, std::size_t size) {
    auto &b = *static_cast<Budget *>(user);
    if (b.stop.stop_requested() || size > 512ULL * 1024 * 1024 - b.live)
      return nullptr;
    auto *p =
        static_cast<std::size_t *>(std::malloc(size + sizeof(std::size_t)));
    if (!p)
      return nullptr;
    *p = size;
    b.live += size;
    return p + 1;
  }
  static void release(void *user, void *data) {
    if (!data)
      return;
    auto *p = static_cast<std::size_t *>(data) - 1;
    static_cast<Budget *>(user)->live -= *p;
    std::free(p);
  }
};
struct Inputs {
  std::stop_token stop;
  std::vector<InputLease> leases;
  std::uint64_t bytes{};
  QByteArray read(const QString &path) {
    check(stop);
    const std::filesystem::path p(path.toStdWString());
    AccessScope scope;
    scope.input_roots = {p.parent_path()};
    auto lease = InputLease::open(scope, p, 512ULL * 1024 * 1024, stop);
    if (lease.snapshot().size_bytes > 512ULL * 1024 * 1024 - bytes)
      throw skeletal::Failure(ErrorCode::ResourceLimit,
                              "glTF input budget exceeded");
    bytes += lease.snapshot().size_bytes;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
      fail("Cannot read glTF resource");
    QByteArray data;
    while (!f.atEnd()) {
      check(stop);
      data += f.read(1024 * 1024);
      if (f.error() != QFileDevice::NoError)
        fail("glTF resource read failed");
    }
    lease.require_unchanged(lease.snapshot());
    leases.push_back(std::move(lease));
    return data;
  }
};
QString localUri(const QString &base, const char *uri) {
  const QUrl value = QUrl::fromEncoded(QByteArray(uri), QUrl::StrictMode);
  if (!value.isValid() || !value.isRelative() || !value.authority().isEmpty() ||
      value.hasQuery() || value.hasFragment())
    fail("glTF external resource must be a relative local URI");
  const auto decoded = value.path(QUrl::FullyDecoded);
  if (decoded.contains(QChar(0)) || QDir::isAbsolutePath(decoded) ||
      decoded.contains(':'))
    fail("Invalid glTF resource path");
  return QDir::cleanPath(QDir(base).absoluteFilePath(decoded));
}
QByteArray dataUri(const char *value) {
  const QByteArray uri(value);
  const auto comma = uri.indexOf(',');
  if (comma < 0 || !uri.left(comma).endsWith(";base64"))
    fail("Unsupported glTF data URI encoding");
  const auto result = QByteArray::fromBase64Encoding(
      uri.mid(comma + 1), QByteArray::AbortOnBase64DecodingErrors);
  if (!result)
    fail("Malformed glTF base64 data URI");
  return result.decoded;
}
QMatrix4x4 matrix(const float *v) {
  QMatrix4x4 m;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      m(r, c) = v[c * 4 + r];
  return m;
}
struct Accessors {
  Budget &budget;
  std::map<std::pair<const cgltf_accessor *, bool>, std::vector<float>> cache;
  const std::vector<float> &read(const cgltf_accessor *a,
                                 bool encoded = false) {
    const auto key = std::make_pair(a, encoded);
    if (const auto it = cache.find(key); it != cache.end())
      return it->second;
    if (!a || a->count > 2500000)
      fail("glTF accessor count exceeds preview budget");
    const auto components = cgltf_num_components(a->type);
    if (!components ||
        a->count > std::numeric_limits<std::size_t>::max() / components)
      fail("Invalid glTF accessor shape");
    const auto count = a->count * components;
    budget.reserve(count * sizeof(float));
    auto &out = cache[key];
    out.resize(count);
    auto base = *a;
    base.is_sparse = false;
    if (encoded)
      base.normalized = false;
    if (cgltf_accessor_unpack_floats(&base, out.data(), count) != count)
      fail("Cannot read glTF accessor");
    if (a->is_sparse) {
      const auto &s = a->sparse;
      cgltf_accessor indices{};
      indices.type = cgltf_type_scalar;
      indices.component_type = s.indices_component_type;
      indices.count = s.count;
      indices.stride = cgltf_component_size(indices.component_type);
      indices.buffer_view = s.indices_buffer_view;
      indices.offset = s.indices_byte_offset;
      auto values = base;
      values.buffer_view = s.values_buffer_view;
      values.offset = s.values_byte_offset;
      values.count = s.count;
      values.stride = cgltf_calc_size(values.type, values.component_type);
      std::vector<float> element(components);
      std::size_t previous = 0;
      for (std::size_t i = 0; i < s.count; ++i) {
        check(budget.stop);
        const auto at = cgltf_accessor_read_index(&indices, i);
        if (at >= a->count || (i && at <= previous))
          fail("Invalid glTF sparse indices");
        previous = at;
        if (!cgltf_accessor_read_float(&values, i, element.data(), components))
          fail("Cannot read glTF sparse values");
        std::copy(element.begin(), element.end(),
                  out.begin() + static_cast<std::ptrdiff_t>(at * components));
      }
    }
    return out;
  }
};
const cgltf_accessor *attribute(const cgltf_attribute *attributes,
                                std::size_t count, cgltf_attribute_type type,
                                int index = 0) {
  for (std::size_t i = 0; i < count; ++i)
    if (attributes[i].type == type && attributes[i].index == index)
      return attributes[i].data;
  return nullptr;
}
// Row-major Matrix4 -> translation/rotation/scale for partially animated
// nodes: an animation channel overriding only some components keeps the
// file's static value for the rest. Lossy only for sheared matrix nodes,
// which glTF animation targets do not carry in practice.
[[nodiscard]] skeletal::Transform decomposeTrs(const formaview::Matrix4 &m) {
  skeletal::Transform t;
  t.translation = {m[3], m[7], m[11]};
  const auto sx = std::hypot(m[0], m[4], m[8]), sy = std::hypot(m[1], m[5], m[9]),
             sz = std::hypot(m[2], m[6], m[10]);
  t.scale = {sx > 1e-12 ? sx : 1.0, sy > 1e-12 ? sy : 1.0,
             sz > 1e-12 ? sz : 1.0};
  const double r[9] = {m[0] / t.scale.x, m[4] / t.scale.x, m[8] / t.scale.x,
                       m[1] / t.scale.y, m[5] / t.scale.y, m[9] / t.scale.y,
                       m[2] / t.scale.z, m[6] / t.scale.z, m[10] / t.scale.z};
  const double trace = r[0] + r[4] + r[8];
  skeletal::Quat q;
  if (trace > 0) {
    const auto s = std::sqrt(trace + 1.0) * 2;
    q = {(r[5] - r[7]) / s, (r[6] - r[2]) / s, (r[1] - r[3]) / s, 0.25 * s};
  } else if (r[0] > r[4] && r[0] > r[8]) {
    const auto s = std::sqrt(1.0 + r[0] - r[4] - r[8]) * 2;
    q = {0.25 * s, (r[3] + r[1]) / s, (r[6] + r[2]) / s, (r[5] - r[7]) / s};
  } else if (r[4] > r[8]) {
    const auto s = std::sqrt(1.0 + r[4] - r[0] - r[8]) * 2;
    q = {(r[3] + r[1]) / s, 0.25 * s, (r[7] + r[5]) / s, (r[6] - r[2]) / s};
  } else {
    const auto s = std::sqrt(1.0 + r[8] - r[0] - r[4]) * 2;
    q = {(r[6] + r[2]) / s, (r[7] + r[5]) / s, 0.25 * s, (r[1] - r[3]) / s};
  }
  const auto norm = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (norm > 1e-12) {
    q.x /= norm;
    q.y /= norm;
    q.z /= norm;
    q.w /= norm;
  }
  t.rotation = q;
  return t;
}
} // namespace

std::shared_ptr<ModelDocument> readGltfModel(const QString &path,
                                             std::stop_token stop) {
  Budget budget;
  budget.stop = stop;
  Inputs inputs;
  inputs.stop = stop;
  const auto main = inputs.read(path);
  auto result = std::make_shared<ModelDocument>();
  result->hash =
      QString::fromStdString(inputs.leases.front().snapshot().content_sha256);
  result->coordinates = "glTF 2.0 · right-handed · Y up · metres";
  result->linearColour = true;
  cgltf_options options{};
  options.memory = {Budget::allocate, Budget::release, &budget};
  cgltf_data *raw = nullptr;
  if (cgltf_parse(&options, main.constData(),
                  static_cast<std::size_t>(main.size()),
                  &raw) != cgltf_result_success) {
    check(stop);
    fail("Invalid glTF/GLB document");
  }
  const std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(raw,
                                                                cgltf_free);
  if (QString::fromUtf8(raw->asset.version) != "2.0")
    fail("Only glTF 2.0 is supported");
  if (raw->asset.min_version &&
      QString::fromUtf8(raw->asset.min_version) != "2.0")
    fail("glTF requires an unsupported minimum version");
  for (std::size_t i = 0; i < raw->extensions_required_count; ++i) {
    const std::string_view extension(raw->extensions_required[i]);
    if (extension != "KHR_materials_unlit" &&
        extension != "KHR_texture_transform")
      throw skeletal::Failure(ErrorCode::Unsupported,
                              "Unsupported required glTF extension: " +
                                  std::string(extension));
  }
  const QDir directory(QFileInfo(path).absolutePath());
  std::vector<QByteArray> buffers(raw->buffers_count);
  for (std::size_t i = 0; i < raw->buffers_count; ++i) {
    check(stop);
    auto &b = raw->buffers[i];
    if (!b.uri) {
      if (i != 0 || !raw->bin || b.size > raw->bin_size)
        fail("Missing GLB binary buffer");
      b.data = const_cast<void *>(raw->bin);
      b.data_free_method = cgltf_data_free_method_none;
      continue;
    }
    buffers[i] = QByteArray(b.uri).startsWith("data:")
                     ? dataUri(b.uri)
                     : inputs.read(localUri(directory.path(), b.uri));
    budget.reserve(static_cast<std::size_t>(buffers[i].size()));
    if (b.size > static_cast<std::size_t>(buffers[i].size()))
      fail("Truncated glTF external buffer");
    b.data = buffers[i].data();
    b.data_free_method = cgltf_data_free_method_none;
  }
  for (std::size_t i = 0; i < raw->buffer_views_count; ++i)
    if (raw->buffer_views[i].has_meshopt_compression)
      throw skeletal::Failure(
          ErrorCode::Unsupported,
          "glTF meshopt-compressed buffers require a separate decoder");
  // Preview never had node/clip/element caps; the decoded-bytes Budget governs.
  skeletal::Limits unbounded;
  unbounded.bones = unbounded.clips = std::numeric_limits<std::uint32_t>::max();
  unbounded.elements = std::numeric_limits<std::uint64_t>::max();
  formaview::gltf::validate_loaded_storage(*raw, unbounded, stop);
  if (cgltf_validate(raw) != cgltf_result_success)
    fail("glTF validation failed");
  Accessors access{budget, {}};
  std::vector<QMatrix4x4> worlds(raw->nodes_count);
  std::vector<std::uint32_t> meshNodes; // node index per mesh (morph drive)
  std::vector<std::vector<double>> meshDefaultWeights;
  std::vector<int> degrees(raw->nodes_count);
  std::vector<std::size_t> order;
  order.reserve(raw->nodes_count);
  for (std::size_t i = 0; i < raw->nodes_count; ++i) {
    if (raw->nodes[i].parent)
      degrees[i] = 1;
    else
      order.push_back(i);
  }
  for (std::size_t at = 0; at < order.size(); ++at) {
    check(stop);
    const auto i = order[at];
    const auto &node = raw->nodes[i];
    float transform[16];
    cgltf_node_transform_local(&node, transform);
    worlds[i] =
        node.parent
            ? worlds[static_cast<std::size_t>(node.parent - raw->nodes)] *
                  matrix(transform)
            : matrix(transform);
    for (std::size_t c = 0; c < node.children_count; ++c) {
      const auto child =
          static_cast<std::size_t>(node.children[c] - raw->nodes);
      if (child >= degrees.size() || --degrees[child] != 0)
        fail("Invalid glTF node hierarchy");
      order.push_back(child);
    }
  }
  if (order.size() != raw->nodes_count)
    fail("Cyclic glTF node hierarchy");
  std::set<const cgltf_node *> active;
  std::vector<const cgltf_node *> pending;
  const auto *scene =
      raw->scene ? raw->scene : (raw->scenes_count ? &raw->scenes[0] : nullptr);
  if (scene)
    pending.assign(scene->nodes, scene->nodes + scene->nodes_count);
  else
    for (std::size_t i = 0; i < raw->nodes_count; ++i)
      if (!raw->nodes[i].parent)
        pending.push_back(&raw->nodes[i]);
  while (!pending.empty()) {
    const auto *node = pending.back();
    pending.pop_back();
    if (!active.insert(node).second)
      fail("Repeated glTF scene node");
    for (std::size_t c = 0; c < node->children_count; ++c)
      pending.push_back(node->children[c]);
  }
  std::size_t vertexBudget = 0;
  std::set<const cgltf_node *> jointNodes;
  for (std::size_t i = 0; i < raw->skins_count; ++i)
    for (std::size_t j = 0; j < raw->skins[i].joints_count; ++j)
      jointNodes.insert(raw->skins[i].joints[j]);
  for (std::size_t ni = 0; ni < raw->nodes_count; ++ni) {
    check(stop);
    const auto &node = raw->nodes[ni];
    const auto id = QString("node/%1").arg(ni);
    float local[16];
    cgltf_node_transform_local(&node, local);
    result->nodes.append(QVariantMap{
        {"id", id},
        {"stableId", result->hash + ":gltf:" + id},
        {"name", name(node.name)},
        {"parent", node.parent
                       ? QString("node/%1").arg(node.parent - raw->nodes)
                       : QString{}},
        {"active", active.contains(&node)},
        {"bone", jointNodes.contains(&node)}});
    auto metadata = result->nodes.back().toMap();
    metadata["local"] = modelMatrixValues(matrix(local));
    metadata["world"] = modelMatrixValues(worlds[ni]);
    if (node.has_translation)
      metadata["rawTranslation"] = QVariantList{
          node.translation[0], node.translation[1], node.translation[2]};
    if (node.has_rotation)
      metadata["rawRotation"] =
          QVariantList{node.rotation[0], node.rotation[1], node.rotation[2],
                       node.rotation[3]};
    result->nodes.back() = std::move(metadata);
    if (!node.mesh || !active.contains(&node))
      continue;
    if (node.has_mesh_gpu_instancing)
      throw skeletal::Failure(ErrorCode::Unsupported,
                              "glTF GPU instancing extension is not decoded");
    for (std::size_t pi = 0; pi < node.mesh->primitives_count; ++pi) {
      check(stop);
      const auto &primitive = node.mesh->primitives[pi];
      if (primitive.has_draco_mesh_compression)
        throw skeletal::Failure(ErrorCode::Unsupported,
                                "glTF Draco compression is not decoded");
      if (primitive.type != cgltf_primitive_type_triangles &&
          primitive.type != cgltf_primitive_type_triangle_strip &&
          primitive.type != cgltf_primitive_type_triangle_fan)
        throw skeletal::Failure(
            ErrorCode::Unsupported,
            "This glTF primitive is not a triangle surface");
      const auto *positions =
          attribute(primitive.attributes, primitive.attributes_count,
                    cgltf_attribute_type_position);
      if (!positions || positions->type != cgltf_type_vec3)
        fail("glTF primitive has no VEC3 positions");
      const auto &p = access.read(positions);
      const auto count = positions->count;
      budget.reserve(count * 128);
      ModelMesh m;
      m.id = QString("mesh/%1/primitive/%2/instance/%3")
                 .arg(node.mesh - raw->meshes)
                 .arg(pi)
                 .arg(ni);
      m.nodeId = id;
      m.name =
          name(node.name) + QString(" [%1 · primitive/%2]").arg(id).arg(pi);
      m.uvTopOrigin = true;
      const auto *normal =
          attribute(primitive.attributes, primitive.attributes_count,
                    cgltf_attribute_type_normal);
      const auto *normalValues = normal ? &access.read(normal) : nullptr;
      m.raw.reserve(count);
      m.points.reserve(count);
      m.corners.resize(count);
      std::iota(m.corners.begin(), m.corners.end(), 0U);
      m.normals.resize(count);
      m.colours.resize(count, QVector3D(1, 1, 1));
      m.alphas.resize(count, 1.F);
      m.faces.resize(count);
      m.cornerMaterial.resize(count);
      std::vector<QVector3D> localPositions;
      std::vector<QVector3D> localNormals;
      const float *staticWeights = node.weights ? node.weights : node.mesh->weights;
      const cgltf_size staticWeightsCount =
          node.weights ? node.weights_count : node.mesh->weights_count;
      // Each target owns a full per-vertex position array, even when it
      // references the same accessor as another target. Account for those
      // allocations before constructing any of them.
      if (primitive.targets_count > std::numeric_limits<std::size_t>::max() /
                                        sizeof(ModelMorphTarget))
        throw skeletal::Failure(ErrorCode::ResourceLimit,
                                "glTF morph target budget exceeded");
      budget.reserve(primitive.targets_count * sizeof(ModelMorphTarget));
      for (std::size_t t = 0; t < primitive.targets_count; ++t) {
        check(stop);
        budget.reserve(count * sizeof(skeletal::Vec3));
        if (normalValues && attribute(primitive.targets[t].attributes,
                                      primitive.targets[t].attributes_count,
                                      cgltf_attribute_type_normal))
          budget.reserve(count * sizeof(skeletal::Vec3));
      }
      std::vector<ModelMorphTarget> morphTargets(primitive.targets_count);
      for (std::size_t t = 0; t < primitive.targets_count; ++t)
        morphTargets[t].positionDeltas.resize(count);
      localPositions.reserve(count);
      localNormals.reserve(count);
      for (std::size_t i = 0; i < count; ++i) {
        m.raw.push_back({p[i * 3], p[i * 3 + 1], p[i * 3 + 2]});
        QVector3D v(p[i * 3], p[i * 3 + 1], p[i * 3 + 2]);
        QVector3D n;
        if (normalValues) {
          if (normal->type != cgltf_type_vec3 || normal->count != count)
            fail("glTF normal shape mismatch");
          n = {(*normalValues)[i * 3], (*normalValues)[i * 3 + 1],
               (*normalValues)[i * 3 + 2]};
        }
        for (std::size_t t = 0; t < primitive.targets_count; ++t) {
          const float staticWeight =
              t < staticWeightsCount ? staticWeights[t] : 0.f;
          const auto *delta = attribute(primitive.targets[t].attributes,
                                        primitive.targets[t].attributes_count,
                                        cgltf_attribute_type_position);
          if (delta) {
            const auto &values = access.read(delta);
            if (delta->count != count || delta->type != cgltf_type_vec3)
              fail("glTF morph target shape mismatch");
            const QVector3D d(values[i * 3], values[i * 3 + 1],
                              values[i * 3 + 2]);
            // Deltas are always retained: animated weights may drive a target
            // whose static default is zero. The default only shapes the
            // loaded mesh when nonzero.
            morphTargets[t].positionDeltas[i] = {d.x(), d.y(), d.z()};
            if (staticWeight != 0) v += d * staticWeight;
          }
          const auto *normalDelta =
              attribute(primitive.targets[t].attributes,
                        primitive.targets[t].attributes_count,
                        cgltf_attribute_type_normal);
          if (normalDelta && normalValues) {
            if (normalDelta->count != count ||
                normalDelta->type != cgltf_type_vec3)
              fail("glTF morph normal shape mismatch");
            const auto &values = access.read(normalDelta);
            const QVector3D d(values[i * 3], values[i * 3 + 1],
                              values[i * 3 + 2]);
            morphTargets[t].normalDeltas.resize(count);
            morphTargets[t].normalDeltas[i] = {d.x(), d.y(), d.z()};
            if (staticWeight != 0) n += d * staticWeight;
          }
        }
        if (normalValues) {
          // Preserve the source directly: subtracting a large morph delta
          // from the rounded sum can erase the original normal entirely.
          m.morphBaseNormals.push_back({(*normalValues)[i * 3],
                                        (*normalValues)[i * 3 + 1],
                                        (*normalValues)[i * 3 + 2]});
        }
        localPositions.push_back(v);
        localNormals.push_back(n);
        m.animationPositions.push_back({v.x(), v.y(), v.z()});
        m.animationNormals.push_back(n);
        m.points.push_back(worlds[ni].map(v));
        if (normalValues) {
          if (normal->type != cgltf_type_vec3 || normal->count != count)
            fail("glTF normal shape mismatch");
          m.normals[i] =
              worlds[ni].inverted().transposed().mapVector(n).normalized();
        }
      }
      std::map<int, const cgltf_accessor *> uv, weights, joints;
      for (std::size_t a = 0; a < primitive.attributes_count; ++a) {
        const auto &v = primitive.attributes[a];
        if (v.type == cgltf_attribute_type_texcoord)
          uv[v.index] = v.data;
        else if (v.type == cgltf_attribute_type_weights)
          weights[v.index] = v.data;
        else if (v.type == cgltf_attribute_type_joints)
          joints[v.index] = v.data;
      }
      for (const auto &[set, a] : uv) {
        if (a->type != cgltf_type_vec2 || a->count != count)
          fail("glTF UV shape mismatch");
        m.uvNames.append(QString("TEXCOORD_%1").arg(set));
        auto &values = m.uvs.emplace_back();
        const auto &src = access.read(a);
        for (std::size_t i = 0; i < count; ++i)
          values.emplace_back(src[i * 2], src[i * 2 + 1]);
      }
      if (weights.size() != joints.size())
        fail("glTF JOINTS and WEIGHTS sets do not match");
      if (node.skin) {
        ModelSkin skin;
        skin.id =
            result->hash + QString(":gltf:skin/%1").arg(node.skin - raw->skins);
        skin.method = "glTF linear blend · default pose";
        skin.name = name(node.skin->name) +
                    QString(" [skin/%1]").arg(node.skin - raw->skins);
        skin.weights.resize(count);
        for (std::size_t j = 0; j < node.skin->joints_count; ++j)
          skin.bones.append(
              name(node.skin->joints[j]->name) +
              QString(" [node/%1]").arg(node.skin->joints[j] - raw->nodes));
        for (const auto &[set, a] : weights) {
          const auto it = joints.find(set);
          if (it == joints.end() || a->type != cgltf_type_vec4 ||
              a->count != count || it->second->type != cgltf_type_vec4 ||
              it->second->count != count)
            fail("glTF skin attribute shape mismatch");
          if ((a->component_type != cgltf_component_type_r_32f &&
               a->component_type != cgltf_component_type_r_8u &&
               a->component_type != cgltf_component_type_r_16u) ||
              (it->second->component_type != cgltf_component_type_r_8u &&
               it->second->component_type != cgltf_component_type_r_16u))
            fail("Invalid glTF weight/joint component types");
          const auto &w = access.read(a, true),
                     &j = access.read(it->second, true);
          const double divisor =
              a->normalized
                  ? (a->component_type == cgltf_component_type_r_8u ? 255.0
                                                                    : 65535.0)
                  : 1.0;
          budget.reserve(count * 4 * sizeof(ModelWeight));
          for (std::size_t v = 0; v < count; ++v)
            for (std::size_t k = 0; k < 4; ++k) {
              const auto bone = j[v * 4 + k];
              if (!std::isfinite(bone) || bone < 0 ||
                  bone >= node.skin->joints_count || std::floor(bone) != bone)
                fail("glTF joint index out of range");
              skin.weights[v].push_back(
                  {static_cast<int>(bone), w[v * 4 + k] / divisor});
            }
        }
        std::vector<QMatrix4x4> skinMatrices;
        skinMatrices.reserve(node.skin->joints_count);
        const auto *inverse = node.skin->inverse_bind_matrices;
        const auto *inverseValues = inverse ? &access.read(inverse) : nullptr;
        if (inverse && (inverse->type != cgltf_type_mat4 ||
                        inverse->count < node.skin->joints_count))
          fail("Invalid glTF inverse bind matrices");
        for (std::size_t j = 0; j < node.skin->joints_count; ++j) {
          QMatrix4x4 bind;
          if (inverseValues)
            bind = matrix(inverseValues->data() + j * 16);
          skin.palette.append(QVariantMap{
              {"nodeId",
               QString("node/%1").arg(node.skin->joints[j] - raw->nodes)},
              {"inverseBind", modelMatrixValues(bind)},
              {"inverseBindProvided", bool(inverseValues)},
              {"defaultJointWorld",
               modelMatrixValues(worlds[static_cast<std::size_t>(
                   node.skin->joints[j] - raw->nodes)])}});
          skinMatrices.push_back(worlds[static_cast<std::size_t>(
                                     node.skin->joints[j] - raw->nodes)] *
                                 bind);
        }
        bool invalid = false, normalized = false;
        for (const auto &records : skin.weights) {
          double sum = 0;
          for (const auto &w : records) {
            if (!std::isfinite(w.value) || w.value < 0)
              invalid = true;
            sum += w.value;
          }
          if (!records.empty() && sum <= 0)
            invalid = true;
          if (sum > 0 && std::abs(sum - 1) > 1e-6)
            normalized = true;
        }
        if (invalid)
          result->warnings.append(m.id +
                                  ": invalid source skin weights; deformation "
                                  "paused, source records preserved");
        else
          for (std::size_t v = 0; v < count; ++v) {
            const auto &records = skin.weights[v];
            if (records.empty())
              continue;
            double sum = 0;
            for (const auto &w : records)
              sum += w.value;
            QMatrix4x4 blended;
            blended.fill(0);
            for (const auto &w : records) {
              const auto &transform =
                  skinMatrices.at(static_cast<std::size_t>(w.bone));
              for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                  blended(r, c) +=
                      static_cast<float>(transform(r, c) * w.value / sum);
            }
            m.points[v] = blended.map(localPositions[v]);
            if (normalValues)
              m.normals[v] = blended.inverted()
                                 .transposed()
                                 .mapVector(localNormals[v])
                                 .normalized();
          }
        if (normalized)
          result->warnings.append(m.id +
                                  ": preview normalizes derived skin matrices "
                                  "only; raw source weights remain unchanged");
        m.skins.push_back(std::move(skin));
      } else if (!weights.empty())
        result->warnings.append(
            m.id + ": source weights without an active skin binding");
      std::vector<std::uint32_t> indices;
      if (primitive.indices) {
        if (primitive.indices->type != cgltf_type_scalar)
          fail("glTF indices must be scalar");
        const auto &values = access.read(primitive.indices, true);
        for (auto v : values) {
          if (!std::isfinite(v) || v < 0 || v >= count || std::floor(v) != v)
            fail("glTF triangle index out of range");
          indices.push_back(static_cast<std::uint32_t>(v));
        }
      } else
        indices = m.corners;
      const auto triangle = [&](std::uint32_t a, std::uint32_t b,
                                std::uint32_t c) {
        m.triangles.insert(m.triangles.end(), {a, b, c});
      };
      if (primitive.type == cgltf_primitive_type_triangles) {
        if (indices.size() % 3)
          fail("glTF triangle index count is not divisible by three");
        m.triangles = std::move(indices);
      } else
        for (std::size_t i = 2; i < indices.size(); ++i) {
          if (primitive.type == cgltf_primitive_type_triangle_fan)
            triangle(indices[0], indices[i - 1], indices[i]);
          else if (i % 2)
            triangle(indices[i - 1], indices[i - 2], indices[i]);
          else
            triangle(indices[i - 2], indices[i - 1], indices[i]);
        }
      vertexBudget += m.triangles.size();
      if (vertexBudget > 2500000)
        throw skeletal::Failure(ErrorCode::ResourceLimit,
                                "glTF triangle preview budget exceeded");
      m.materials.append(primitive.material ? name(primitive.material->name)
                                            : "Default");
      m.references.append("");
      m.texturePaths.append("");
      m.textures.resize(1);
      m.materialUv.push_back(0);
      m.uvTransforms.push_back({0, 0, 1, 1, 0});
      m.alphaModes.push_back(0);
      m.alphaCutoffs.push_back(.5F);
      if (primitive.material) {
        const auto &material = *primitive.material;
        const auto &base = material.pbr_metallic_roughness;
        for (auto &c : m.colours)
          c = {base.base_color_factor[0], base.base_color_factor[1],
               base.base_color_factor[2]};
        for (auto &alpha : m.alphas)
          alpha = base.base_color_factor[3];
        m.unlit = material.unlit;
        m.alphaModes[0] = material.alpha_mode == cgltf_alpha_mode_mask    ? 1
                          : material.alpha_mode == cgltf_alpha_mode_blend ? 2
                                                                          : 0;
        m.alphaCutoffs[0] = material.alpha_cutoff;
        if (m.alphaModes[0] == 2)
          result->warnings.append(
              m.id + ": transparent surfaces use draw-range depth sorting; "
                     "intersecting transparency may differ");
        const auto &texture = base.base_color_texture;
        if (texture.texture && texture.texture->image) {
          const auto &source = *texture.texture->image;
          QByteArray encoded;
          QString reference;
          try {
            if (source.uri) {
              reference = name(source.uri);
              if (QByteArray(source.uri).startsWith("data:"))
                reference = QString("image/%1 · embedded data URI")
                                .arg(&source - raw->images);
              encoded =
                  QByteArray(source.uri).startsWith("data:")
                      ? dataUri(source.uri)
                      : inputs.read(localUri(directory.path(), source.uri));
            } else if (source.buffer_view) {
              const auto *view = source.buffer_view;
              const auto *bytes = cgltf_buffer_view_data(view);
              if (!bytes || view->size > 64ULL * 1024 * 1024)
                fail("Invalid embedded glTF image");
              reference = QString("image/%1 · bufferView/%2")
                              .arg(&source - raw->images)
                              .arg(view - raw->buffer_views);
              encoded = QByteArray(reinterpret_cast<const char *>(bytes),
                                   static_cast<qsizetype>(view->size));
            }
            if (!encoded.isEmpty()) {
              auto decoded = decodeModelTexture(encoded, stop);
              budget.reserve(decoded->rgba.size() * sizeof(float));
              const auto *sampler = texture.texture->sampler;
              decoded->linearMag =
                  !sampler || sampler->mag_filter != cgltf_filter_type_nearest;
              decoded->linearMin =
                  !sampler ||
                  sampler->min_filter == cgltf_filter_type_undefined ||
                  sampler->min_filter == cgltf_filter_type_linear ||
                  sampler->min_filter ==
                      cgltf_filter_type_linear_mipmap_nearest ||
                  sampler->min_filter == cgltf_filter_type_linear_mipmap_linear;
              decoded->mipmaps =
                  !sampler ||
                  sampler->min_filter == cgltf_filter_type_undefined ||
                  sampler->min_filter >=
                      cgltf_filter_type_nearest_mipmap_nearest;
              decoded->linearMip =
                  !sampler ||
                  sampler->min_filter == cgltf_filter_type_undefined ||
                  sampler->min_filter ==
                      cgltf_filter_type_nearest_mipmap_linear ||
                  sampler->min_filter == cgltf_filter_type_linear_mipmap_linear;
              const auto wrap = [](cgltf_wrap_mode value) {
                return value == cgltf_wrap_mode_clamp_to_edge     ? 1
                       : value == cgltf_wrap_mode_mirrored_repeat ? 2
                                                                  : 0;
              };
              decoded->wrapU = sampler ? wrap(sampler->wrap_s) : 0;
              decoded->wrapV = sampler ? wrap(sampler->wrap_t) : 0;
              m.textures[0] = std::move(decoded);
              m.texturePaths[0] = reference;
            }
          } catch (const skeletal::Failure &e) {
            if (e.error.code == ErrorCode::Cancelled)
              throw;
            result->resources.append(
                QVariantMap{{"reference", reference},
                            {"loaded", false},
                            {"detail", QString::fromUtf8(e.what())}});
          }
          m.references[0] = reference;
          const int texcoord =
              texture.has_transform && texture.transform.has_texcoord
                  ? texture.transform.texcoord
                  : texture.texcoord;
          const auto target = uv.find(texcoord);
          if (target == uv.end()) {
            m.textures[0].reset();
            m.texturePaths[0].clear();
            result->warnings.append(
                m.id + ": base colour references an absent UV set");
          } else
            m.materialUv[0] =
                static_cast<int>(std::distance(uv.begin(), target));
          if (texture.has_transform)
            m.uvTransforms[0] = {
                texture.transform.offset[0], texture.transform.offset[1],
                texture.transform.scale[0], texture.transform.scale[1],
                texture.transform.rotation};
        }
      }
      if (const auto *colours =
              attribute(primitive.attributes, primitive.attributes_count,
                        cgltf_attribute_type_color)) {
        if (colours->count != count || (colours->type != cgltf_type_vec3 &&
                                        colours->type != cgltf_type_vec4))
          fail("glTF vertex colour shape mismatch");
        const auto &values = access.read(colours);
        const std::size_t stride = colours->type == cgltf_type_vec4 ? 4 : 3;
        for (std::size_t i = 0; i < count; ++i) {
          m.colours[i] *= QVector3D(values[i * stride], values[i * stride + 1],
                                    values[i * stride + 2]);
          if (stride == 4)
            m.alphas[i] *= values[i * stride + 3];
        }
      }
      m.morphTargets = std::move(morphTargets);
      budget.reserve(primitive.targets_count * sizeof(double));
      auto &defaults = meshDefaultWeights.emplace_back(primitive.targets_count, 0.0);
      for (std::size_t t = 0; t < primitive.targets_count && t < staticWeightsCount; ++t)
        defaults[t] = staticWeights[t];
      meshNodes.push_back(static_cast<std::uint32_t>(ni));
      result->meshes.push_back(std::move(m));
    }
  }
  // M07 native animation: the file's own clips drive its own nodes and
  // meshes. Channels address this file's node indices directly — no mapping,
  // no attached external state; unsupported paths stay flagged, never
  // reinterpreted. Extraction shape checks mirror the verified core reader
  // (src/formats/gltf_anim.cpp) while reusing this file's leased inputs.
  auto anims = std::make_shared<formaview::gltf::GltfAnimationData>();
  anims->node_count = static_cast<std::uint32_t>(raw->nodes_count);
  if (raw->animations_count > 0) {
    std::vector<std::size_t> clipKeys;
    for (std::size_t c = 0; c < raw->animations_count; ++c) {
      check(stop);
      const auto &animation = raw->animations[c];
      formaview::gltf::AnimationClip clip;
      clip.name = name(animation.name).toStdString();
      std::size_t keys = 0;
      for (std::size_t k = 0; k < animation.channels_count; ++k) {
        const auto &channel = animation.channels[k];
        if (!channel.target_node) {
          anims->warnings.push_back("Animation channel without target node");
          continue;
        }
        if (!channel.sampler || !channel.sampler->input ||
            !channel.sampler->output)
          fail("Animation sampler without accessors");
        formaview::gltf::AnimationChannel parsed;
        parsed.node_index =
            static_cast<std::uint32_t>(channel.target_node - raw->nodes);
        std::uint32_t expected = 0;
        switch (channel.target_path) {
        case cgltf_animation_path_type_translation:
          parsed.path = formaview::gltf::TrackPath::Translation;
          expected = 3;
          break;
        case cgltf_animation_path_type_rotation:
          parsed.path = formaview::gltf::TrackPath::Rotation;
          expected = 4;
          break;
        case cgltf_animation_path_type_scale:
          parsed.path = formaview::gltf::TrackPath::Scale;
          expected = 3;
          break;
        case cgltf_animation_path_type_weights:
          // Animated morph weights are supported for meshes (linear/step);
          // cubic-spline and weights on non-mesh nodes stay flagged.
          if (channel.target_node->mesh &&
              channel.target_node->mesh->primitives_count > 0 &&
              channel.sampler->interpolation !=
                  cgltf_interpolation_type_cubic_spline) {
            parsed.path = formaview::gltf::TrackPath::Weights;
          } else {
            parsed.path = formaview::gltf::TrackPath::Unsupported;
            anims->warnings.push_back(
                channel.sampler->interpolation ==
                        cgltf_interpolation_type_cubic_spline
                    ? "Cubic-spline morph weights not supported; channel "
                      "retained as flagged"
                    : "Unsupported animation path (weights) retained as "
                      "flagged");
          }
          break;
        default:
          parsed.path = formaview::gltf::TrackPath::Unsupported;
          anims->warnings.push_back(
              "Unsupported animation path retained as flagged");
          break;
        }
        switch (channel.sampler->interpolation) {
        case cgltf_interpolation_type_step:
          parsed.interpolation = formaview::gltf::Interpolation::Step;
          break;
        case cgltf_interpolation_type_cubic_spline:
          parsed.interpolation = formaview::gltf::Interpolation::CubicSpline;
          break;
        default:
          parsed.interpolation = formaview::gltf::Interpolation::Linear;
          break;
        }
        const auto &input = *channel.sampler->input;
        const auto &output = *channel.sampler->output;
        if (input.type != cgltf_type_scalar)
          fail("Animation times must be scalar");
        const auto components =
            static_cast<std::uint32_t>(cgltf_num_components(output.type));
        if (!components)
          fail("Unsupported glTF accessor type");
        if (parsed.path != formaview::gltf::TrackPath::Unsupported &&
            parsed.path != formaview::gltf::TrackPath::Weights &&
            components != expected)
          fail("Animation value type does not match target path");
        const auto multiplier = parsed.interpolation == formaview::gltf::Interpolation::CubicSpline ? 3u : 1u;
        const auto targets = (parsed.path == formaview::gltf::TrackPath::Unsupported ||
                               parsed.path == formaview::gltf::TrackPath::Weights) && channel.target_node->mesh &&
                                     channel.target_node->mesh->primitives_count > 0
                                 ? channel.target_node->mesh->primitives[0].targets_count : 1u;
        if (!input.count || !targets || input.count > 2500000 / multiplier / targets ||
            output.count > 2500000 || output.count != input.count * multiplier * targets)
          fail("Animation key count mismatch");
        if (channel.target_node->has_matrix && parsed.path != formaview::gltf::TrackPath::Unsupported)
          fail("Animated glTF nodes must use TRS, not a matrix");
        budget.reserve((input.count + output.count * components) *
                       sizeof(double));
        const auto &times = access.read(&input);
        const auto &values = access.read(&output);
        parsed.components = components;
        parsed.times.reserve(times.size());
        for (double t : times) {
          if (!std::isfinite(t) || t < 0 || (!parsed.times.empty() && t <= parsed.times.back()))
            fail("Animation times must be finite, nonnegative and strictly increasing");
          parsed.times.push_back(t);
        }
        parsed.values.reserve(values.size());
        for (double v : values) {
          if ((parsed.values.size() & 1023) == 0) check(stop);
          if (!std::isfinite(v)) fail("Non-finite animation value");
          parsed.values.push_back(v);
        }
        if (!parsed.times.empty()) {
          clip.start = clip.channels.empty()
                           ? parsed.times.front()
                           : std::min(clip.start, parsed.times.front());
          clip.end = clip.channels.empty()
                         ? parsed.times.back()
                         : std::max(clip.end, parsed.times.back());
        }
        keys = std::max(keys, input.count);
        clip.channels.push_back(std::move(parsed));
      }
      anims->clips.push_back(std::move(clip));
      clipKeys.push_back(keys);
      if (anims->clips.back().channels.empty())
        anims->warnings.push_back("Animation clip without usable channels");
    }
    if (!anims->clips.empty() && anims->node_count > 0) {
      result->animNodeParents.resize(raw->nodes_count);
      result->animNodeLocals.resize(raw->nodes_count);
      std::vector<skeletal::Transform> staticTrs(raw->nodes_count);
      for (std::size_t i = 0; i < raw->nodes_count; ++i) {
        const auto &node = raw->nodes[i];
        result->animNodeParents[i] =
            node.parent
                ? static_cast<std::int32_t>(node.parent - raw->nodes)
                : std::int32_t{-1};
        float local[16];
        cgltf_node_transform_local(&node, local);
        auto &m = result->animNodeLocals[i];
        for (int r = 0; r < 4; ++r)
          for (int col = 0; col < 4; ++col)
            m[static_cast<std::size_t>(r * 4 + col)] = local[col * 4 + r];
        // Partial channels must preserve authored signed/zero scale. Matrix
        // decomposition is neither unique nor lossless for those TRS values.
        if (node.has_matrix)
          staticTrs[i] = decomposeTrs(m);
        else {
          staticTrs[i].translation = {node.translation[0], node.translation[1], node.translation[2]};
          staticTrs[i].rotation = {node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3]};
          staticTrs[i].scale = {node.scale[0], node.scale[1], node.scale[2]};
        }
      }
      for (std::size_t c = 0; c < anims->clips.size(); ++c) {
        const auto &clip = anims->clips[c];
        const auto keys = clipKeys.at(c);
        ModelAnimationClip entry;
        entry.name = QString::fromStdString(clip.name);
        entry.start = clip.start;
        entry.end = clip.end;
        // glTF stores real seconds without a frame rate; the densest key
        // count gives the display/step mapping only (evaluation stays in
        // seconds).
        entry.fps = keys > 1 && clip.end > clip.start
                        ? (static_cast<double>(keys) - 1) / (clip.end - clip.start)
                        : 30.0;
        entry.lastFrame = keys > 0 ? static_cast<int>(keys) - 1 : 0;
        result->animationClips.push_back(entry);
        result->clips.push_back(QVariantMap{
            {"name", entry.name},
            {"start", entry.start},
            {"end", entry.end},
            {"fps", entry.fps},
            {"lastFrame", entry.lastFrame}});
      }
      result->localAt =
          [anims, staticTrs,
           staticLocals = result->animNodeLocals](std::size_t clip,
                                                  double seconds) {
            const auto pose = formaview::gltf::evaluate_clip(*anims, clip,
                                                             seconds);
            auto locals = staticLocals;
            for (std::size_t i = 0; i < pose.size() && i < locals.size(); ++i) {
              const auto &o = pose[i];
              if (!(o.translation || o.rotation || o.scale))
                continue;
              auto trs = staticTrs[i];
              if (o.translation)
                trs.translation = o.local.translation;
              if (o.rotation)
                trs.rotation = o.local.rotation;
              if (o.scale)
                trs.scale = o.local.scale;
              locals[i] = skeletal::matrix(trs);
            }
            return locals;
          };
      result->worldAt =
          [localAt = result->localAt,
           parents = result->animNodeParents](std::size_t clip, double seconds) {
            return chainWorlds(localAt(clip, seconds), parents);
          };
      bool anyWeights = false;
      for (const auto &clipData : anims->clips)
        for (const auto &ch : clipData.channels)
          if (ch.path == formaview::gltf::TrackPath::Weights) anyWeights = true;
      if (anyWeights && !result->meshes.empty()) {
        for (auto &mesh : result->meshes)
          if (!mesh.morphTargets.empty()) mesh.morphAnimated = true;
        auto targetCounts = std::make_shared<std::vector<std::size_t>>();
        for (const auto &mesh : result->meshes)
          targetCounts->push_back(mesh.morphTargets.size());
        result->morphWeightsAt =
            [anims, meshNodes, targetCounts, meshDefaultWeights](std::size_t clip, double seconds) {
              std::vector<std::vector<double>> weights(targetCounts->size());
              if (clip >= anims->clips.size() || !std::isfinite(seconds))
                return weights;
              // Unanimated meshes (or clips without a weight channel) retain
              // the node/mesh defaults rather than silently reverting to zero.
              weights = meshDefaultWeights;
              for (const auto &channel : anims->clips[clip].channels) {
                if (channel.path != formaview::gltf::TrackPath::Weights ||
                    channel.times.empty() || channel.values.empty() ||
                    channel.components == 0)
                  continue;
                for (std::size_t mi = 0; mi < targetCounts->size(); ++mi) {
                  if (mi >= meshNodes.size() || (*targetCounts)[mi] == 0 ||
                      channel.node_index != meshNodes[mi])
                    continue;
                  // Weights channels store one value per target per key, but
                  // the accessor type is SCALAR: derive the per-key stride.
                  const auto perKey = channel.values.size() / channel.times.size();
                  const auto count = std::min<std::size_t>((*targetCounts)[mi],
                                                           perKey);
                  auto &out = weights[mi];
                  out.assign(count, 0.0);
                  if (channel.times.size() == 1 || seconds >= channel.times.back()) {
                    const auto key = seconds >= channel.times.back() ? channel.times.size()-1 : 0;
                    for (std::size_t t = 0; t < count; ++t)
                      out[t] = channel.values[key*perKey+t];
                    continue;
                  }
                  // hi stays at size-2 so hi+1 is always a real key; the
                  // clamped u holds the last key at/past the clip end.
                  std::size_t hi = 0;
                  while (hi + 2 < channel.times.size() &&
                         channel.times[hi + 1] <= seconds)
                    ++hi;
                  const double span = channel.times[hi + 1] - channel.times[hi];
                  const double u =
                      channel.interpolation ==
                              formaview::gltf::Interpolation::Step
                          ? 0.0
                          : std::clamp((seconds - channel.times[hi]) / span,
                                       0.0, 1.0);
                  for (std::size_t t = 0; t < count; ++t) {
                    const double lo = channel.values[hi * perKey + t];
                    const double hiV = channel.values[(hi + 1) * perKey + t];
                    out[t] = lo + (hiV - lo) * u;
                  }
                }
              }
              return weights;
            };
      }
      result->nativeAnimated = true;
      if (result->meshes.empty()) {
        // Animation-only glTF previews as skeleton lines like standalone PSA.
        result->skeletonOnly = true;
        result->boneParents = result->animNodeParents;
        result->boneNames.reserve(raw->nodes_count);
        for (std::size_t i = 0; i < raw->nodes_count; ++i)
          result->boneNames.push_back(name(raw->nodes[i].name));
      }
      for (const auto &warning : anims->warnings)
        result->warnings.append(QString::fromStdString(warning));
    }
  }
  // Snapshot only bounded transform metadata. Do not retain cgltf pointers or
  // duplicate animation/geometry buffers, and account for the snapshot allocation.
  namespace tv = skeletal::transform;
  auto evidence = std::make_shared<tv::Evidence>();
  evidence->format = tv::Format::Gltf;
  evidence->adapterVersion = "cgltf/r13-evidence-2";
  QCryptographicHash identity(QCryptographicHash::Sha256);
  identity.addData(QByteArrayLiteral("FormaView/glTF/evidence/1:"));
  for (const auto &lease : inputs.leases) {
    lease.require_unchanged(lease.snapshot());
    // Fixed-width ordered content hashes include external buffers. Main JSON
    // SHA alone is not an identity proof when a .bin changes independently.
    identity.addData(QByteArray::fromStdString(lease.snapshot().content_sha256));
  }
  evidence->sha256 = identity.result().toHex().toStdString();
  if (raw->nodes_count <= tv::Policy{}.maxNodes) {
    budget.reserve(raw->nodes_count * sizeof(tv::Node));
    evidence->nodes.reserve(raw->nodes_count);
    evidence->coordinates = tv::Coordinates{1,{1,2,3}}; // glTF 2.0 normative convention.
    std::vector<bool> matrixAnimated(raw->nodes_count,false);
    for (const auto &animation : std::span(raw->animations,raw->animations_count))
      for (const auto &channel : std::span(animation.channels,animation.channels_count))
        if (channel.target_node) matrixAnimated[static_cast<std::size_t>(channel.target_node-raw->nodes)]=true;
    for (std::size_t i=0;i<raw->nodes_count;++i) {
      check(stop);
      const auto &node=raw->nodes[i];
      tv::Node n; n.fileIndex=static_cast<std::uint32_t>(i);
      if (node.parent) n.parent=static_cast<std::size_t>(node.parent-raw->nodes);
      // Keep a static matrix in its source representation. The format forbids
      // matrix+TRS and animation targeting a matrix node.
      if (node.has_matrix && !node.has_translation && !node.has_rotation && !node.has_scale && !matrixAnimated[i]) {
        tv::Matrix m{};for(std::size_t j=0;j<16;++j)m[j]=node.matrix[j];
        bool orthogonal=true;
        for(std::size_t c=0;c<3;++c)for(std::size_t d=c+1;d<3;++d) {
          double dot=0,cc=0,dd=0;for(std::size_t r=0;r<3;++r){dot+=m[c*4+r]*m[d*4+r];cc+=m[c*4+r]*m[c*4+r];dd+=m[d*4+r]*m[d*4+r];}
          if(cc==0||dd==0||!std::isfinite(dot)||std::abs(dot)>4*std::numeric_limits<float>::epsilon()*std::sqrt(cc*dd))orthogonal=false;
        }
        if(orthogonal)n.restMatrix=m;
      } else if (!node.has_matrix) {
        tv::Trs t;
        for (std::size_t j=0;j<3;++j) {
          t.translation[j]=node.has_translation?node.translation[j]:0;
          t.scale[j]=node.has_scale?node.scale[j]:1;
        }
        if (node.has_rotation) for (std::size_t j=0;j<4;++j) t.rotation[j]=node.rotation[j];
        // glTF declares unit quaternions. Account only for float decoding:
        // projection must round each component back to the ORIGINAL float.
        // Anything outside that exact rounding cell remains insufficient.
        double norm=0;for(double q:t.rotation)norm+=q*q;
        if(std::isfinite(norm)&&norm>0) {
          auto canonical=t.rotation;bool roundTrip=true;
          for(std::size_t j=0;j<4;++j){canonical[j]/=std::sqrt(norm);if(static_cast<float>(canonical[j])!=static_cast<float>(t.rotation[j]))roundTrip=false;}
          if(roundTrip)t.rotation=canonical;
        }
        n.restLocal=t;
      }
      n.evaluation=tv::Evaluation{}; // glTF TRS uses standard parent multiplication.
      evidence->nodes.push_back(n);
    }
    evidence->fullNodeDomain=true;
    if (raw->skins_count==0) evidence->bindings.emplace();
    // Native glTF proof: enumerate source IBM operators, without inventing a
    // bind-world pose. Cross-file core compares them under the node mapping.
    evidence->nativeSkinBindings.emplace();
    std::vector<bool> seen(raw->skins_count,false);
    bool complete=true;
    for(std::size_t i=0;i<raw->nodes_count && complete;++i) {
      check(stop);const auto &node=raw->nodes[i];if(!node.skin)continue;
      if(!node.mesh){complete=false;break;}
      seen[static_cast<std::size_t>(node.skin-raw->skins)]=true;
      const auto *inverse=node.skin->inverse_bind_matrices;
      if(inverse&&(inverse->type!=cgltf_type_mat4||inverse->component_type!=cgltf_component_type_r_32f||inverse->normalized||inverse->count<node.skin->joints_count)){complete=false;break;}
      const auto *values=inverse?&access.read(inverse):nullptr;
      for(std::size_t j=0;j<node.skin->joints_count;++j) {
        check(stop);
        if(evidence->nativeSkinBindings->size()>=tv::Policy{}.maxBindings){complete=false;break;}
        budget.reserve(sizeof(tv::NativeSkinBinding));
        tv::NativeSkinBinding binding;binding.instance=i;binding.joint=static_cast<std::size_t>(node.skin->joints[j]-raw->nodes);
        if(values)for(std::size_t k=0;k<16;++k)binding.inverseBind[k]=(*values)[j*16+k];
        else binding.inverseBind={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        evidence->nativeSkinBindings->push_back(binding);
      }
    }
    if(!complete||std::find(seen.begin(),seen.end(),false)!=seen.end())evidence->nativeSkinBindings.reset();
  }
  // Fixed-decoder identity mapping only. Preserve raw defaults (including
  // non-unit quaternions); this is separate from the normative format proof.
  bool decoderComplete=evidence->fullNodeDomain && evidence->nativeSkinBindings.has_value()
      && anims->warnings.empty()
      && anims->clips.size()==raw->animations_count;
  auto snapshot=std::make_shared<ModelDecoderSnapshot>();
  for(std::size_t i=0;i<raw->extensions_used_count;++i){
    const auto extension=name(raw->extensions_used[i]);
    if(extension=="KHR_materials_unlit"||extension=="KHR_texture_transform")
      snapshot->excludedShadingExtensions.append(extension);
    else decoderComplete=false;
  }
  auto append=[&](const auto &value) {
    budget.reserve(sizeof(value));
    snapshot->metadata.append(reinterpret_cast<const char*>(&value),sizeof(value));
  };
  append(raw->nodes_count);
  for(std::size_t i=0;i<raw->nodes_count && decoderComplete;++i) {
    check(stop);const auto &n=raw->nodes[i];
    auto nodeName=name(n.name);budget.reserve(sizeof(QString)+static_cast<std::size_t>(nodeName.size())*sizeof(QChar));
    snapshot->nodeNames.push_back(std::move(nodeName));
    const auto parent=n.parent?static_cast<std::int64_t>(n.parent-raw->nodes):std::int64_t{-1};append(parent);
    append(n.has_matrix);append(n.has_translation);append(n.has_rotation);append(n.has_scale);
    for(float v:n.matrix){decoderComplete &= std::isfinite(v);append(v);}
    for(float v:n.translation){decoderComplete &= std::isfinite(v);append(v);}
    for(float v:n.rotation){decoderComplete &= std::isfinite(v);append(v);}
    for(float v:n.scale){decoderComplete &= std::isfinite(v);append(v);}
    float local[16];cgltf_node_transform_local(&n,local);
    for(float v:local){decoderComplete &= std::isfinite(v);append(v);}
  }
  // Morph proof covers decoded local position/normal inputs, separately from
  // skinning weights, materials and rasterization. Keep complete byte evidence,
  // not a hash standing in for equality. Optional evidence must not make a
  // previously loadable model fail its resource budget.
  if (decoderComplete) {
    const auto overhead=raw->nodes_count*(sizeof(QByteArray)+sizeof(std::size_t));
    if(overhead>1024ULL*1024*1024-budget.decoded)decoderComplete=false;
    else {
      budget.reserve(overhead);
      snapshot->morphInputs.resize(raw->nodes_count);
      snapshot->morphTargetCounts.resize(raw->nodes_count);
    }
  }
  std::size_t morphBytes=0, morphItems=0;
  for(std::size_t ni=0;ni<raw->nodes_count && decoderComplete;++ni) {
    check(stop);const auto &node=raw->nodes[ni];
    if(!node.mesh)continue;
    std::size_t targetCount=0;
    for(std::size_t pi=0;pi<node.mesh->primitives_count;++pi)
      targetCount=std::max(targetCount,node.mesh->primitives[pi].targets_count);
    if(!targetCount)continue;
    snapshot->morphTargetCounts[ni]=targetCount;
    auto &bytes=snapshot->morphInputs[ni];
    const auto add=[&](const auto &value) {
      if((++morphItems%4096)==0)check(stop);
      if(!decoderComplete)return;
      if(sizeof(value)>64ULL*1024*1024-morphBytes || sizeof(value)>1024ULL*1024*1024-budget.decoded) {
        decoderComplete=false;return;
      }
      budget.reserve(sizeof(value));morphBytes+=sizeof(value);
      bytes.append(reinterpret_cast<const char*>(&value),sizeof(value));
    };
    add(node.mesh->primitives_count);add(targetCount);
    const auto *defaults=node.weights?node.weights:node.mesh->weights;
    const auto defaultCount=node.weights?node.weights_count:node.mesh->weights_count;
    for(std::size_t i=0;i<targetCount;++i){const float w=i<defaultCount?defaults[i]:0.f;decoderComplete &= std::isfinite(w);add(w);}
    std::size_t primitives=0;
    const auto triples=[&](const auto &values){add(values.size());for(const auto &v:values){add(v.x);add(v.y);add(v.z);}};
    for(std::size_t mi=0;mi<result->meshes.size() && decoderComplete;++mi) {
      if(meshNodes[mi]!=ni)continue;
      ++primitives;const auto &mesh=result->meshes[mi];
      if(mesh.morphTargets.size()!=targetCount){decoderComplete=false;break;}
      triples(mesh.raw);triples(mesh.animationPositions);triples(mesh.morphBaseNormals);
      add(mesh.animationNormals.size());for(const auto &normal:mesh.animationNormals){add(normal.x());add(normal.y());add(normal.z());}
      add(mesh.triangles.size());for(auto v:mesh.triangles)add(v);
      add(mesh.corners.size());for(auto v:mesh.corners)add(v);
      for(const auto &target:mesh.morphTargets){triples(target.positionDeltas);triples(target.normalDeltas);}
    }
    if(primitives!=node.mesh->primitives_count)decoderComplete=false;
  }

  for(std::size_t c=0;c<anims->clips.size() && decoderComplete;++c) {
    if(anims->clips[c].channels.size()!=raw->animations[c].channels_count)decoderComplete=false;
    std::set<std::pair<std::uint32_t,int>> targets;
    for(const auto &channel:anims->clips[c].channels) {
      check(stop);
      if(channel.path==gltf::TrackPath::Unsupported
          || channel.node_index>=raw->nodes_count ||
          (raw->nodes[channel.node_index].has_matrix && channel.path!=gltf::TrackPath::Weights)
          || !targets.emplace(channel.node_index,static_cast<int>(channel.path)).second)decoderComplete=false;
    }
  }
  if(decoderComplete) {
    append(evidence->nativeSkinBindings->size());
    for(const auto &binding:*evidence->nativeSkinBindings) {
      check(stop);append(binding.instance);append(binding.joint);
      for(double v:binding.inverseBind){decoderComplete &= std::isfinite(v);append(v);}
    }
    if(decoderComplete){snapshot->sourceSha=QString::fromStdString(evidence->sha256);
      snapshot->animations=anims;result->decoderSnapshot=std::move(snapshot);}
  }
  result->transformEvidence = [evidence=std::shared_ptr<const tv::Evidence>(evidence)](std::stop_token token) {
    check(token);
    return *evidence;
  };
  check(stop);
  return result;
}


namespace {
struct DecoderNodeView { std::int64_t parent{}; QByteArray local; };
struct DecoderView {
  std::vector<DecoderNodeView> nodes;
  std::map<std::pair<std::size_t,std::size_t>,QByteArray> bindings;
};
// Parse our own bounded, version-local metadata, never native pointers/padding.
// Reject trailing bytes and duplicate binding identities rather than hiding corruption.
bool decoderView(const ModelDecoderSnapshot &s, DecoderView &out, std::stop_token stop) {
  qsizetype offset=0;
  auto read=[&](auto &value){if(sizeof(value)>static_cast<std::size_t>(s.metadata.size()-offset))return false;
    std::memcpy(&value,s.metadata.constData()+offset,sizeof(value));offset+=sizeof(value);return true;};
  std::size_t n=0;if(!read(n)||n>65536||!s.animations||n!=s.animations->node_count)return false;
  constexpr qsizetype localBytes=4*sizeof(cgltf_bool)+42*sizeof(float);
  out.nodes.reserve(n);
  for(std::size_t i=0;i<n;++i){
    if(stop.stop_requested())return false;
    DecoderNodeView node;if(!read(node.parent)||node.parent < -1||node.parent>=static_cast<std::int64_t>(n)
      ||s.metadata.size()-offset<localBytes)return false;
    node.local=s.metadata.mid(offset,localBytes);offset+=localBytes;out.nodes.push_back(std::move(node));
  }
  std::vector<unsigned char> state(n);
  for(std::size_t i=0;i<n;++i){
    if(stop.stop_requested())return false;
    auto j=static_cast<std::int64_t>(i);
    while(j>=0&&state[j]==0){state[j]=1;j=out.nodes[j].parent;}
    if(j>=0&&state[j]==1)return false;
    j=static_cast<std::int64_t>(i);while(j>=0&&state[j]==1){state[j]=2;j=out.nodes[j].parent;}
  }
  std::size_t bindings=0;if(!read(bindings)||bindings>262144)return false;
  for(std::size_t i=0;i<bindings;++i){
    if(stop.stop_requested())return false;
    std::size_t instance=0,joint=0;if(!read(instance)||!read(joint)||instance>=n||joint>=n||s.metadata.size()-offset<128)return false;
    if(!out.bindings.emplace(std::make_pair(instance,joint),s.metadata.mid(offset,128)).second)return false;offset+=128;
  }
  return offset==s.metadata.size();
}
}

QVariantMap compareFixedDecoder(const std::shared_ptr<const ModelDecoderSnapshot> &a,
    const std::shared_ptr<const ModelDecoderSnapshot> &b, std::stop_token stop,
    const FixedDecoderOptions &options) {
  QVariantMap report{{"domain","fixedDecoderProgram"},{"version",4},{"promotionEnabled",false},
    {"scope","mapped decoded node transforms and local morph inputs; same process implementation; excludes skin weights, shading and rasterization"}};
  // Callers may only tighten the built-in limits.
  const auto channelBudget=std::min<std::size_t>(options.channelBudget,1048576);
  const auto scalarBudget=std::min<std::size_t>(options.scalarBudget,67108864);
  auto finish=[&](const char *v,const char *r){
    report.insert("verdict",v);report.insert("reason",r);
    if(!report.contains("curveCorrespondence"))report.insert("curveCorrespondence",QVariantMap{
      {"verdict","insufficient"},{"reason",r},{"promotionEnabled",false},{"floatingEvaluationEquivalent",false}});
    return report;
  };
  if(stop.stop_requested())return finish("cancelled","cancelled");
  if(!a||!b||!a->animations||!b->animations)return finish("insufficient","missingEvidence");
  report.insert("sourceSha",a->sourceSha);report.insert("targetSha",b->sourceSha);
  report.insert("snapshotVersion","cgltf-current-process-decoder-v2");
  DecoderView av,bv;
  if(!decoderView(*a,av,stop)||!decoderView(*b,bv,stop))
    return stop.stop_requested()?finish("cancelled","cancelled"):finish("insufficient","invalidSnapshot");
  const auto n=av.nodes.size();if(n!=bv.nodes.size())return finish("insufficient","mappingConflict");
  std::vector<std::size_t> map(n);std::iota(map.begin(),map.end(),0);
  const bool contract=!options.explicitMapping.empty();
  if(contract) {
    // A supplied contract is a statement by the caller, never repaired or completed here.
    if(options.explicitMapping.size()!=n)return finish("insufficient","invalidExplicitMapping");
    std::vector<unsigned char> used(n);
    for(std::size_t i=0;i<n;++i){const auto j=options.explicitMapping[i];
      if(j>=n||used[j])return finish("insufficient","invalidExplicitMapping");used[j]=1;map[i]=j;}
  } else {
    bool named=a->nodeNames.size()==n&&b->nodeNames.size()==n;
    std::map<QString,std::size_t> ids;std::set<QString> seen;
    if(named)for(std::size_t i=0;i<n;++i){
      if(stop.stop_requested())return finish("cancelled","cancelled");
      if(b->nodeNames[i].isEmpty()||!ids.emplace(b->nodeNames[i],i).second){named=false;break;}
    }
    if(named)for(std::size_t i=0;i<n;++i){const auto it=ids.find(a->nodeNames[i]);
      if(a->nodeNames[i].isEmpty()||it==ids.end()||!seen.insert(a->nodeNames[i]).second){named=false;break;}map[i]=it->second;}
    if(!named){if(a->metadata!=b->metadata)return finish("insufficient","mappingConflict");std::iota(map.begin(),map.end(),0);}
  }
  bool identity=true;QByteArray mappingBytes(contract?"explicit-bijection-v3:":"full-node-bijection-v2:");QVariantList pairs;
  // Parent topology is never relaxed. A differing default only records the node: the
  // exact-default conclusion stays insufficient, and may be explained by forced overrides below.
  std::vector<unsigned char> defaultDiffers(n);std::size_t differing=0;
  for(std::size_t i=0;i<n;++i){
    if(stop.stop_requested())return finish("cancelled","cancelled");
    const auto j=map[i];identity &= i==j;
    const auto &u=av.nodes[i],&v=bv.nodes[j];
    if((u.parent<0)!=(v.parent<0)||(u.parent>=0&&map[u.parent]!=static_cast<std::size_t>(v.parent)))
      return finish("insufficient","mappedDefaultOrParentConflict");
    if(u.local!=v.local){defaultDiffers[i]=1;++differing;}
    mappingBytes+=QByteArray::number(i)+":"+QByteArray::number(j)+";";
    pairs.append(QVariantMap{{"source",static_cast<uint>(i)},{"target",static_cast<uint>(j)}});
  }
  if(av.bindings.size()!=bv.bindings.size())return finish("insufficient","mappedBindingConflict");
  for(const auto &[key,value]:av.bindings){
    if(stop.stop_requested())return finish("cancelled","cancelled");
    const auto it=bv.bindings.find({map[key.first],map[key.second]});
    if(it==bv.bindings.end()||it->second!=value)return finish("insufficient","mappedBindingConflict");
  }
  report.insert("nodes",static_cast<uint>(n));
  report.insert("mapping",contract?"explicitBijection":identity?"fullIdentity":"fullNameBijection");
  report.insert("mappedPairs",pairs);
  report.insert("mappingSha",QString::fromLatin1(QCryptographicHash::hash(mappingBytes,QCryptographicHash::Sha256).toHex()));
  const auto morphShape=[&](const ModelDecoderSnapshot &v){return
    (v.morphInputs.empty()&&v.morphTargetCounts.empty()) ||
    (v.morphInputs.size()==n&&v.morphTargetCounts.size()==n);};
  if(!morphShape(*a)||!morphShape(*b))return finish("insufficient","invalidMorphSnapshot");
  std::size_t morphScalars=0,morphNodes=0;
  for(std::size_t i=0;i<n;++i){
    if(stop.stop_requested())return finish("cancelled","cancelled");
    const auto j=map[i];
    const auto ac=a->morphTargetCounts.empty()?0:a->morphTargetCounts[i];
    const auto bc=b->morphTargetCounts.empty()?0:b->morphTargetCounts[j];
    const QByteArray empty;
    const auto &u=a->morphInputs.empty()?empty:a->morphInputs[i];
    const auto &v=b->morphInputs.empty()?empty:b->morphInputs[j];
    if(ac!=bc||u.size()!=v.size()||((ac==0)!=u.isEmpty()))return finish("insufficient","mappedMorphConflict");
    if(ac)++morphNodes;
    for(qsizetype offset=0;offset<u.size();offset+=4096){
      if(stop.stop_requested())return finish("cancelled","cancelled");
      const auto bytes=std::min<qsizetype>(4096,u.size()-offset);
      morphScalars+=(static_cast<std::size_t>(bytes)+7)/8;
      if(morphScalars>scalarBudget)return finish("insufficient","budgetExceeded");
      if(std::memcmp(u.constData()+offset,v.constData()+offset,static_cast<std::size_t>(bytes)))return finish("insufficient","mappedMorphConflict");
    }
  }
  report.insert("morphNodes",static_cast<qulonglong>(morphNodes));
  report.insert("excludedShadingExtensions",QVariantMap{{"source",a->excludedShadingExtensions},{"target",b->excludedShadingExtensions}});
  const auto &x=*a->animations,&y=*b->animations;
  // Coverage means identical externally supplied local overrides, not equality
  // of the authored curves. Defaults/parent/IBM above must be exact, including
  // unoverridden components and the separate static-local branch.
  // unionMask: path groups present in any clip. forced: groups carrying a well-formed,
  // non-empty channel in EVERY clip. animatedAll: every clip overrides at least one group,
  // so the static local branch is never evaluated for that node.
  struct Coverage{std::vector<unsigned> unionMask,forced;std::vector<unsigned char> animatedAll;};
  std::size_t channels=0,validatedScalars=morphScalars;
  auto cover=[&](const ModelDecoderSnapshot &snapshot,const gltf::GltfAnimationData &data,Coverage &out){
    out.unionMask.assign(n,0);out.forced.assign(n,data.clips.empty()?0u:7u);out.animatedAll.assign(n,data.clips.empty()?0:1);
    std::vector<unsigned> valid(n);
    for(const auto &clip:data.clips){
      if(!std::isfinite(clip.start)||!std::isfinite(clip.end)||clip.start>clip.end)return false;
      std::set<std::pair<std::uint32_t,int>> targets;std::fill(valid.begin(),valid.end(),0u);
      for(const auto &c:clip.channels){
        if(stop.stop_requested()||++channels>channelBudget||c.node_index>=n)return false;
        unsigned bit=0;
        switch(c.path){case gltf::TrackPath::Translation:bit=1;break;case gltf::TrackPath::Rotation:bit=2;break;case gltf::TrackPath::Scale:bit=4;break;case gltf::TrackPath::Weights:bit=8;break;default:return false;}
        const auto expected=bit==8?(snapshot.morphTargetCounts.empty()?0:snapshot.morphTargetCounts[c.node_index]):(bit==2?4u:3u);
        if(!expected || (bit==8&&c.interpolation==gltf::Interpolation::CubicSpline) || c.components!=(bit==8?1u:expected)||!targets.emplace(c.node_index,static_cast<int>(c.path)).second)return false;
        out.unionMask[c.node_index]|=bit;
        const std::size_t per=static_cast<std::size_t>(bit==8?expected:c.components)*(c.interpolation==gltf::Interpolation::CubicSpline?3u:1u);
        if(c.interpolation!=gltf::Interpolation::Linear&&c.interpolation!=gltf::Interpolation::Step&&c.interpolation!=gltf::Interpolation::CubicSpline)return false;
        if(c.times.empty()||c.times.size()>std::numeric_limits<std::size_t>::max()/per||c.values.size()!=c.times.size()*per)return false;
        for(std::size_t k=0;k<c.times.size();++k){
          if((k%4096)==0&&stop.stop_requested())return false;
          if(++validatedScalars>scalarBudget||!std::isfinite(c.times[k])||(k&&c.times[k]<=c.times[k-1]))return false;
        }
        for(std::size_t k=0;k<c.values.size();++k){
          if((k%4096)==0&&stop.stop_requested())return false;
          if(++validatedScalars>scalarBudget||!std::isfinite(c.values[k]))return false;
        }
        if(c.times.front()<clip.start||c.times.back()>clip.end)return false;
        valid[c.node_index]|=bit;
      }
      for(std::size_t i=0;i<n;++i){out.forced[i]&=valid[i];if(!(valid[i]&7u))out.animatedAll[i]=0;}
    }
    return true;};
  Coverage ca,cb;
  // The established scalar cap counts corresponding scalar pairs, not both
  // sides added together. Validate each side under that same cap before the
  // pairwise comparison below; otherwise a valid 45M-scalar program is rejected
  // merely because validation reads both inputs. Neither cap is enlarged.
  const bool validSource=cover(*a,x,ca);
  if(!validSource)return stop.stop_requested()?finish("cancelled","cancelled"):
      finish("insufficient",(channels>channelBudget||validatedScalars>scalarBudget)?"budgetExceeded":"invalidChannels");
  validatedScalars=morphScalars;
  if(!cover(*b,y,cb))return stop.stop_requested()?finish("cancelled","cancelled"):
      finish("insufficient",(channels>channelBudget||validatedScalars>scalarBudget)?"budgetExceeded":"invalidChannels");
  QVariantList coverage;int components=0;
  for(std::size_t i=0;i<n;++i){const auto mask=ca.unionMask[i]&cb.unionMask[map[i]]&7u;if(!mask)continue;
    components+=static_cast<int>((mask&1)!=0)+static_cast<int>((mask&2)!=0)+static_cast<int>((mask&4)!=0);
    coverage.append(QVariantMap{{"source",static_cast<uint>(i)},{"target",static_cast<uint>(map[i])},{"mask",mask}});
  }
  if(!differing) {
    report.insert("commonCoverage",QVariantMap{{"verdict",components?"sameCommonOverrides":"insufficient"},
      {"reason",components?"exactMappedDefaultsParentsAndBindings":"noCommonChannels"},
      {"condition","identical finite local TRS overrides with identical presence flags on listed paths; all other components remain default"},
      {"authoredActionsEquivalent",false},{"clipPairAligned",false},{"aggregation","unionPerInputThenIntersection"},{"promotionEnabled",false},{"componentCount",components},{"paths",coverage}});
  } else {
    // Differing defaults can only be explained away where the evaluator provably never reads
    // them: the node is overridden in every clip of both inputs, and every differing T/R/S group
    // carries a well-formed channel in every clip of both inputs. Anything else stays observable.
    struct Group{qsizetype offset,length;unsigned bit;};
    constexpr Group groups[]{{80,12,1},{92,16,2},{108,12,4}};
    const QByteArray noMatrix(4,'\0');
    QVariantList dead;bool allDead=true;
    for(std::size_t i=0;i<n&&allDead;++i){
      if(!defaultDiffers[i])continue;
      if(stop.stop_requested())return finish("cancelled","cancelled");
      const auto j=map[i];const auto &u=av.nodes[i].local,&v=bv.nodes[j].local;
      bool ok=ca.animatedAll[i]&&cb.animatedAll[j]&&u.left(4)==noMatrix&&v.left(4)==noMatrix;
      unsigned deadGroups=0;
      if(ok)for(const auto &g:groups){
        if(u.mid(g.offset,g.length)==v.mid(g.offset,g.length))continue;
        if((ca.forced[i]&cb.forced[j]&g.bit)==g.bit)deadGroups|=g.bit;else{ok=false;break;}
      }
      if(!ok){allDead=false;break;}
      dead.append(QVariantMap{{"source",static_cast<uint>(i)},{"target",static_cast<uint>(j)},{"deadGroups",deadGroups}});
    }
    const bool proven=allDead&&components>0;
    report.insert("forcedOverrideCoverage",QVariantMap{{"verdict",proven?"sameCommonOverridesDeadDefaults":"insufficient"},
      {"reason",!allDead?"defaultObservable":components?"deadDefaultsUnobservable":"noCommonChannels"},
      {"condition","parents and bindings exact; every differing default group is overridden by a well-formed channel in every clip of both inputs and its node is overridden in every clip; identical supplied overrides on listed paths"},
      {"authoredActionsEquivalent",false},{"clipPairAligned",false},{"aggregation","unionPerInputThenIntersection"},{"promotionEnabled",false},
      {"componentCount",components},{"paths",coverage},{"deadDefaults",dead}});
    return finish("insufficient","mappedDefaultOrParentConflict");
  }
  // Charge the comparison by scalar pairs; validation has its own identical cap.
  std::size_t count=morphScalars;
  auto equal=[&](const std::vector<double> &u,const std::vector<double> &v){if(u.size()!=v.size())return false;
    for(std::size_t i=0;i<u.size();++i){if((i%4096)==0&&stop.stop_requested())return false;
      if(++count>scalarBudget||std::memcmp(&u[i],&v[i],sizeof(double))!=0)return false;}return true;};
  bool same=x.clips.size()==y.clips.size();
  for(std::size_t c=0;c<x.clips.size()&&same;++c){
    const auto &u=x.clips[c],&v=y.clips[c];
    if(u.start!=v.start||u.end!=v.end||u.channels.size()!=v.channels.size()){same=false;break;}
    std::map<std::pair<std::uint32_t,int>,const gltf::AnimationChannel*> target;
    for(const auto &q:v.channels)target.emplace(std::make_pair(q.node_index,static_cast<int>(q.path)),&q);
    for(const auto &p:u.channels){
      if(stop.stop_requested())return finish("cancelled","cancelled");
      const auto it=target.find({static_cast<std::uint32_t>(map[p.node_index]),static_cast<int>(p.path)});
      if(it==target.end()){same=false;break;}const auto &q=*it->second;
      if(p.interpolation!=q.interpolation||p.components!=q.components||!equal(p.times,q.times)||!equal(p.values,q.values)){same=false;break;}
    }
  }
  // Names select candidate clip pairs only; all decoded keys, interpolation,
  // components and values must independently prove the correspondence.
  // Timestamp translation is a curve-data statement, NOT a proof of identical
  // IEEE-754 sampling after t+offset (cancellation/rounding can differ).
  QVariantMap alignment{{"promotionEnabled",false},{"floatingEvaluationEquivalent",false},
    {"scope","exact decoded curve keys under a constant timestamp translation; no time scaling, retargeting or floating-evaluation equivalence"}};
  QVariantList alignedPairs;
  bool aligned=same;
  if(same) {
    for(std::size_t i=0;i<x.clips.size();++i)alignedPairs.append(QVariantMap{
      {"sourceClip",static_cast<qulonglong>(i)},{"targetClip",static_cast<qulonglong>(i)},{"targetTimeOffset",0.0}});
    alignment.insert("mapping","sameIndexExactProgram");
  } else {
    aligned=x.clips.size()==y.clips.size()&&!x.clips.empty();
    std::map<std::string,std::size_t> clipNames;std::set<std::string> sourceNames;
    for(std::size_t i=0;i<y.clips.size()&&aligned;++i)
      aligned=!y.clips[i].name.empty()&&clipNames.emplace(y.clips[i].name,i).second;
    // TwoSum's residual checks exact real addition of binary64 inputs. A
    // rounded offset or a rounded translated key is never accepted silently.
    const auto exactSum=[](double a,double b,double target){
      if(!std::isfinite(a)||!std::isfinite(b)||!std::isfinite(target))return false;
      const double sum=a+b,part=sum-a;
      const double residual=(a-(sum-part))+(b-part);
      return std::isfinite(sum)&&sum==target&&residual==0;
    };
    for(std::size_t i=0;i<x.clips.size()&&aligned;++i) {
      if(stop.stop_requested())return finish("cancelled","cancelled");
      const auto &u=x.clips[i];const auto found=clipNames.find(u.name);
      if(found==clipNames.end()||!sourceNames.insert(u.name).second){aligned=false;break;}
      const auto j=found->second;const auto &v=y.clips[j];const double offset=v.start-u.start;
      if(!exactSum(u.start,offset,v.start)||!exactSum(u.end,offset,v.end)||u.channels.size()!=v.channels.size()){aligned=false;break;}
      std::map<std::pair<std::uint32_t,int>,const gltf::AnimationChannel*> target;
      for(const auto &channel:v.channels)target.emplace(std::make_pair(channel.node_index,static_cast<int>(channel.path)),&channel);
      for(const auto &p:u.channels) {
        const auto it=target.find({static_cast<std::uint32_t>(map[p.node_index]),static_cast<int>(p.path)});
        if(it==target.end()){aligned=false;break;}const auto &q=*it->second;
        if(p.interpolation!=q.interpolation||p.components!=q.components||p.times.size()!=q.times.size()||!equal(p.values,q.values)){aligned=false;break;}
        for(std::size_t k=0;k<p.times.size();++k) {
          if((k%4096)==0&&stop.stop_requested())return finish("cancelled","cancelled");
          if(++count>scalarBudget||!exactSum(p.times[k],offset,q.times[k])){aligned=false;break;}
        }
        if(!aligned)break;
      }
      if(aligned)alignedPairs.append(QVariantMap{{"sourceClip",static_cast<qulonglong>(i)},
        {"targetClip",static_cast<qulonglong>(j)},{"targetTimeOffset",offset}});
    }
    alignment.insert("mapping","uniqueNamesThenExactKeyProof");
  }
  alignment.insert("verdict",aligned?"exactKeysUnderTimeTranslation":"insufficient");
  alignment.insert("pairs",aligned?alignedPairs:QVariantList{});
  report.insert("curveCorrespondence",alignment);

  report.insert("comparedScalars",static_cast<qulonglong>(count));
  if(stop.stop_requested())return finish("cancelled","cancelled");
  if(count>scalarBudget)return finish("insufficient","budgetExceeded");
  return same?finish("equivalent","exactMappedDecodedProgram"):finish("insufficient","differentProgram");
}
