#include "formaview/file_access.hpp"
#include "model_document.hpp"
#include "model_metadata.hpp"
#include "model_texture.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <algorithm>
#include <cgltf.h>
#include <cmath>
#include <cstdlib>
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
  if (cgltf_validate(raw) != cgltf_result_success)
    fail("glTF validation failed");
  Accessors access{budget, {}};
  std::vector<QMatrix4x4> worlds(raw->nodes_count);
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
          const auto weights = node.weights ? node.weights : node.mesh->weights;
          const auto weightsCount =
              node.weights ? node.weights_count : node.mesh->weights_count;
          if (t >= weightsCount || weights[t] == 0)
            continue;
          const auto *delta = attribute(primitive.targets[t].attributes,
                                        primitive.targets[t].attributes_count,
                                        cgltf_attribute_type_position);
          if (delta) {
            const auto &values = access.read(delta);
            if (delta->count != count || delta->type != cgltf_type_vec3)
              fail("glTF morph target shape mismatch");
            v +=
                QVector3D(values[i * 3], values[i * 3 + 1], values[i * 3 + 2]) *
                weights[t];
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
            n +=
                QVector3D(values[i * 3], values[i * 3 + 1], values[i * 3 + 2]) *
                weights[t];
          }
        }
        localPositions.push_back(v);
        localNormals.push_back(n);
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
      result->meshes.push_back(std::move(m));
    }
  }
  for (const auto &lease : inputs.leases)
    lease.require_unchanged(lease.snapshot());
  check(stop);
  return result;
}
