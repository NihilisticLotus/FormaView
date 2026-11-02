#include "scene_sdk.hpp"
#include <QStringDecoder>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

namespace formaview::scene_export {
namespace {
struct Reader {
  Scene &scene;
  const QByteArray &bytes;
  qsizetype at{};
  bool utf8{};
  int vertex_size{}, texture_size{}, material_size{}, bone_size{}, morph_size{},
      rigid_size{};
  template <class T> T read() {
    scene.check();
    if (static_cast<qsizetype>(sizeof(T)) > bytes.size() - at)
      invalid("Truncated PMX record");
    T value;
    std::memcpy(&value, bytes.constData() + at, sizeof(T));
    at += sizeof(T);
    return value;
  }
  void skip(qsizetype size) {
    if (size < 0 || size > bytes.size() - at)
      invalid("Truncated PMX block");
    at += size;
    scene.check();
  }
  int count() {
    const auto n = read<int32_t>();
    if (n < 0 || static_cast<std::uint64_t>(n) > scene.limits.elements)
      invalid("PMX count outside resource limits");
    return n;
  }
  double real() {
    const auto v = read<float>();
    if (!std::isfinite(v))
      invalid("Nonfinite PMX value");
    return v;
  }
  FbxVector4 vec(int n = 3) {
    FbxVector4 v;
    for (int i = 0; i < n; ++i)
      v[i] = real();
    return v;
  }
  int index(int width) {
    if (width == 1)
      return read<int8_t>();
    if (width == 2)
      return read<int16_t>();
    return read<int32_t>();
  }
  int vertex(int count) {
    const auto i = vertex_size == 1   ? static_cast<uint32_t>(read<uint8_t>())
                   : vertex_size == 2 ? static_cast<uint32_t>(read<uint16_t>())
                                      : read<uint32_t>();
    if (i >= static_cast<uint32_t>(count))
      invalid("PMX vertex index out of bounds");
    return static_cast<int>(i);
  }
  QString text() {
    const auto size = count();
    if (size > bytes.size() - at)
      invalid("Truncated PMX string");
    QStringDecoder decoder(utf8 ? QStringDecoder::Utf8
                                : QStringDecoder::Utf16LE);
    QString value =
        decoder.decode(QByteArrayView(bytes.constData() + at, size));
    at += size;
    if (decoder.hasError() || value.contains(QChar(0)))
      invalid("Invalid PMX string encoding");
    scene.reserve(static_cast<std::uint64_t>(size) * 2);
    return value;
  }
};
struct Vertex {
  FbxVector4 position, normal;
  std::vector<FbxVector4> uvs;
  std::vector<std::pair<int, double>> weights;
  bool qdef{};
};
struct Bone {
  QString name;
  FbxVector4 position;
  int parent;
};
struct Morph {
  QString name;
  int type{};
  std::map<int, FbxVector4> offsets;
  std::vector<std::pair<int, double>> groups;
};
void valid(int i, size_t size, bool optional = false) {
  if ((optional && i == -1))
    return;
  if (i < 0 || static_cast<size_t>(i) >= size)
    invalid("PMX record index out of bounds");
}
void finite_morph(const FbxVector4 &value) {
  if (!std::isfinite(value[0]) || !std::isfinite(value[1]) ||
      !std::isfinite(value[2]))
    invalid("PMX morph displacement overflow");
}
} // namespace
void read_pmx(Scene &s, const QByteArray &bytes) {
  Reader r{s, bytes};
  if (bytes.size() < 17 || bytes.first(4) != "PMX ")
    invalid("Invalid PMX signature");
  r.skip(4);
  const auto version = r.real();
  if (std::abs(version - 2.0) > 1e-5 && std::abs(version - 2.1) > 1e-5)
    invalid("Only PMX 2.0/2.1 is supported");
  const auto header = r.read<uint8_t>();
  if (header < 8)
    invalid("Incomplete PMX globals");
  const auto encoding = r.read<uint8_t>(), extra_uv = r.read<uint8_t>();
  if (encoding > 1 || extra_uv > 4)
    invalid("Invalid PMX globals");
  r.utf8 = encoding == 1;
  r.vertex_size = r.read<uint8_t>();
  r.texture_size = r.read<uint8_t>();
  r.material_size = r.read<uint8_t>();
  r.bone_size = r.read<uint8_t>();
  r.morph_size = r.read<uint8_t>();
  r.rigid_size = r.read<uint8_t>();
  for (const auto width : {r.vertex_size, r.texture_size, r.material_size,
                           r.bone_size, r.morph_size, r.rigid_size})
    if (width != 1 && width != 2 && width != 4)
      invalid("Invalid PMX index width");
  r.skip(header - 8);
  const auto model_name = r.text(), english_name = r.text(), comment = r.text(),
             english_comment = r.text();
  string_property(s.scene, "PMX.Name", model_name);
  string_property(s.scene, "PMX.EnglishName", english_name);
  string_property(s.scene, "PMX.Comment", comment);
  string_property(s.scene, "PMX.EnglishComment", english_comment);
  const auto vertex_count = r.count();
  s.reserve(static_cast<std::uint64_t>(vertex_count) * (512 + extra_uv * 64));
  std::vector<Vertex> vertices(static_cast<size_t>(vertex_count));
  bool has_qdef{};
  for (auto &v : vertices) {
    v.position = r.vec();
    v.normal = r.vec();
    v.uvs.push_back(r.vec(2));
    for (int u = 0; u < extra_uv; ++u)
      v.uvs.push_back(r.vec(4));
    const auto type = r.read<uint8_t>();
    const int count = type == 0                  ? 1
                      : (type == 1 || type == 3) ? 2
                      : (type == 2 || type == 4) ? 4
                                                 : 0;
    if (!count || (type == 4 && version < 2.05))
      invalid("Invalid PMX skin type");
    for (int i = 0; i < count; ++i)
      v.weights.emplace_back(r.index(r.bone_size), 0);
    if (count == 1)
      v.weights[0].second = 1;
    else if (count == 2) {
      v.weights[0].second = r.real();
      v.weights[1].second = 1 - v.weights[0].second;
    } else
      for (auto &w : v.weights)
        w.second = r.real();
    if (type == 3) {
      r.vec();
      r.vec();
      r.vec();
      s.loss("pmx_sdef",
             "SDEF control vectors have no FBX equivalent; the original two "
             "bone influences are exported as linear skinning.");
    }
    v.qdef = type == 4;
    has_qdef |= v.qdef;
    const auto edge = r.real();
    if (edge != 1)
      s.loss("pmx_edge_scale",
             "Per-vertex toon edge scale is omitted from FBX shading.");
  }
  const auto corner_count = r.count();
  if (corner_count % 3)
    invalid("Incomplete PMX triangle");
  s.reserve(static_cast<std::uint64_t>(corner_count) * 16);
  std::vector<int> indices(static_cast<size_t>(corner_count));
  for (auto &i : indices)
    i = r.vertex(vertex_count);
  const auto texture_count = r.count();
  s.reserve(static_cast<std::uint64_t>(texture_count) * 64);
  std::vector<QString> textures;
  for (int i = 0; i < texture_count; ++i)
    textures.push_back(r.text());
  auto *mesh = FbxMesh::Create(s.scene, model_name.toUtf8().constData());
  auto *mesh_node = FbxNode::Create(s.scene, model_name.toUtf8().constData());
  mesh_node->SetNodeAttribute(mesh);
  s.scene->GetRootNode()->AddChild(mesh_node);
  mesh->InitControlPoints(vertex_count);
  auto *normal = mesh->CreateElementNormal();
  normal->SetMappingMode(FbxLayerElement::eByControlPoint);
  normal->SetReferenceMode(FbxLayerElement::eDirect);
  for (int i = 0; i < vertex_count; ++i) {
    mesh->SetControlPointAt(vertices[static_cast<size_t>(i)].position, i);
    normal->GetDirectArray().Add(vertices[static_cast<size_t>(i)].normal);
  }
  for (int u = 0; u <= extra_uv; ++u) {
    auto *uv = mesh->CreateElementUV(("UV" + std::to_string(u)).c_str());
    uv->SetMappingMode(FbxLayerElement::eByControlPoint);
    uv->SetReferenceMode(FbxLayerElement::eDirect);
    for (const auto &v : vertices)
      uv->GetDirectArray().Add(
          FbxVector2(v.uvs[static_cast<size_t>(u)][0],
                     u ? v.uvs[static_cast<size_t>(u)][1] : 1 - v.uvs[0][1]));
    if (u) {
      auto *zw = mesh->CreateElementUV(
          ("PMX_AdditionalUV" + std::to_string(u) + "_ZW").c_str());
      zw->SetMappingMode(FbxLayerElement::eByControlPoint);
      zw->SetReferenceMode(FbxLayerElement::eDirect);
      for (const auto &v : vertices)
        zw->GetDirectArray().Add(FbxVector2(v.uvs[static_cast<size_t>(u)][2],
                                            v.uvs[static_cast<size_t>(u)][3]));
    }
  }
  const auto material_count = r.count();
  s.reserve(static_cast<std::uint64_t>(material_count) * 2048);
  int face_begin{};
  auto *mat_layer = mesh->CreateElementMaterial();
  mat_layer->SetMappingMode(FbxLayerElement::eByPolygon);
  mat_layer->SetReferenceMode(FbxLayerElement::eIndexToDirect);
  for (int i = 0; i < material_count; ++i) {
    const auto name = r.text(), en = r.text();
    auto *m = FbxSurfacePhong::Create(s.scene, name.toUtf8().constData());
    string_property(m, "PMX.EnglishName", en);
    const auto diffuse = r.vec(4), specular = r.vec();
    const auto shininess = r.real();
    const auto ambient = r.vec();
    m->Diffuse.Set(FbxDouble3(diffuse[0], diffuse[1], diffuse[2]));
    m->TransparencyFactor.Set(1 - diffuse[3]);
    m->Specular.Set(FbxDouble3(specular[0], specular[1], specular[2]));
    m->Shininess.Set(shininess);
    m->Ambient.Set(FbxDouble3(ambient[0], ambient[1], ambient[2]));
    const auto flags = r.read<uint8_t>();
    r.vec(4);
    r.real();
    const int tex = r.index(r.texture_size), sphere = r.index(r.texture_size);
    const auto sphere_mode = r.read<uint8_t>(), shared = r.read<uint8_t>();
    if (shared > 1)
      invalid("Invalid PMX toon mode");
    const auto toon =
        shared ? static_cast<int>(r.read<uint8_t>()) : r.index(r.texture_size);
    string_property(m, "PMX.Comment", r.text());
    const auto count = r.count();
    if (count % 3 || count > corner_count - face_begin)
      invalid("PMX material face range is invalid");
    mesh_node->AddMaterial(m);
    valid(tex, textures.size(), true);
    valid(sphere, textures.size(), true);
    if (!shared)
      valid(toon, textures.size(), true);
    if (tex >= 0)
      try {
        m->Diffuse.ConnectSrcObject(
            s.texture(s.image(textures[static_cast<size_t>(tex)])));
      } catch (const skeletal::Failure &e) {
        if (e.error.code == ErrorCode::Cancelled ||
            e.error.code == ErrorCode::ResourceLimit)
          throw;
        s.loss("external_texture_unavailable",
               textures[static_cast<size_t>(tex)] + ": " +
                   QString::fromUtf8(e.what()));
      }
    if (flags || sphere_mode || shared || toon >= 0)
      s.loss("pmx_material_semantics",
             "Toon/sphere shading, edge rendering, shadow/culling flags are "
             "not reproduced by FBX Phong. Base colour, alpha, specular, "
             "diffuse textures and material assignments are retained.");
    for (int face = face_begin; face < face_begin + count; face += 3) {
      mesh->BeginPolygon(i);
      for (int c = 0; c < 3; ++c)
        mesh->AddPolygon(indices[static_cast<size_t>(face + c)]);
      mesh->EndPolygon();
    }
    face_begin += count;
  }
  if (face_begin != corner_count)
    invalid("PMX material ranges do not cover all faces");
  const auto bone_count = r.count();
  if (static_cast<std::uint64_t>(bone_count) > s.limits.bones)
    invalid("PMX bone budget exceeded");
  s.reserve(static_cast<std::uint64_t>(bone_count) * 2048);
  std::vector<Bone> bones;
  std::vector<FbxNode *> nodes;
  for (int i = 0; i < bone_count; ++i) {
    const auto name = r.text(), en = r.text();
    const auto position = r.vec();
    const auto parent = r.index(r.bone_size);
    r.read<int32_t>();
    const auto flags = r.read<uint16_t>();
    if (flags & 1)
      valid(r.index(r.bone_size), static_cast<size_t>(bone_count), true);
    else
      r.vec();
    if (flags & 0x300) {
      valid(r.index(r.bone_size), static_cast<size_t>(bone_count));
      r.real();
    }
    if (flags & 0x400)
      r.vec();
    if (flags & 0x800) {
      r.vec();
      r.vec();
    }
    if (flags & 0x2000)
      r.read<int32_t>();
    if (flags & 0x20) {
      valid(r.index(r.bone_size), static_cast<size_t>(bone_count));
      r.count();
      r.real();
      const auto n = r.count();
      for (int j = 0; j < n; ++j) {
        valid(r.index(r.bone_size), static_cast<size_t>(bone_count));
        const auto limited = r.read<uint8_t>();
        if (limited > 1)
          invalid("Invalid PMX IK limit flag");
        if (limited) {
          r.vec();
          r.vec();
        }
      }
    }
    if (flags & (0x20 | 0x300 | 0x400 | 0x800 | 0x1000 | 0x2000))
      s.loss("pmx_bone_controls",
             "PMX IK, append transforms, local/fixed-axis controls, external "
             "parents and physics evaluation order are omitted; source "
             "hierarchy and rest/bind positions are retained.");
    valid(parent, static_cast<size_t>(bone_count), true);
    if (parent == i)
      invalid("Self-parent PMX bone");
    bones.push_back({name, position, parent});
    auto *node = FbxNode::Create(s.scene, name.toUtf8().constData());
    auto *skeleton = FbxSkeleton::Create(s.scene, name.toUtf8().constData());
    skeleton->SetSkeletonType(parent < 0 ? FbxSkeleton::eRoot
                                         : FbxSkeleton::eLimbNode);
    node->SetNodeAttribute(skeleton);
    string_property(node, "PMX.EnglishName", en);
    nodes.push_back(node);
  }
  for (int i = 0; i < bone_count; ++i) {
    const auto &bone = bones[static_cast<size_t>(i)];
    int depth{};
    for (auto p = bone.parent; p >= 0; p = bones[static_cast<size_t>(p)].parent)
      if (++depth > 256 || depth > bone_count)
        invalid("Cyclic or excessively deep PMX bone hierarchy");
    auto *parent = bone.parent < 0 ? s.scene->GetRootNode()
                                   : nodes[static_cast<size_t>(bone.parent)];
    parent->AddChild(nodes[static_cast<size_t>(i)]);
    nodes[static_cast<size_t>(i)]->LclTranslation.Set(
        bone.parent < 0
            ? bone.position
            : bone.position - bones[static_cast<size_t>(bone.parent)].position);
  }
  auto *skin = FbxSkin::Create(s.scene, "PMX skin");
  skin->SetSkinningType(has_qdef ? FbxSkin::eBlend : FbxSkin::eLinear);
  mesh->AddDeformer(skin);
  std::vector<FbxCluster *> clusters;
  for (int i = 0; i < bone_count; ++i) {
    auto *c = FbxCluster::Create(s.scene, "PMX joint");
    c->SetLink(nodes[static_cast<size_t>(i)]);
    c->SetLinkMode(FbxCluster::eNormalize);
    FbxAMatrix bind, identity;
    bind.SetT(bones[static_cast<size_t>(i)].position);
    c->SetTransformMatrix(identity);
    c->SetTransformLinkMatrix(bind);
    skin->AddCluster(c);
    clusters.push_back(c);
  }
  for (int i = 0; i < vertex_count; ++i) {
    const auto &v = vertices[static_cast<size_t>(i)];
    double sum{};
    std::map<int, double> merged;
    for (const auto &[bone, weight] : v.weights) {
      if (weight < 0)
        invalid("Negative PMX influence");
      if (weight == 0)
        continue;
      valid(bone, clusters.size());
      merged[bone] += weight;
      sum += weight;
    }
    if (sum <= 0)
      invalid("PMX vertex has no valid influence");
    for (const auto &[bone, weight] : merged)
      clusters[static_cast<size_t>(bone)]->AddControlPointIndex(i, weight);
    if (std::abs(sum - 1) > 1e-5)
      s.loss("pmx_weight_normalization",
             "Raw influence values are preserved; FBX skin evaluation "
             "normalizes non-unit sums.");
    if (has_qdef)
      skin->AddControlPointIndex(i, v.qdef ? 1 : 0);
  }
  const auto morph_count = r.count();
  s.reserve(static_cast<std::uint64_t>(morph_count) * 192);
  std::vector<Morph> morphs(static_cast<size_t>(morph_count));
  for (auto &m : morphs) {
    m.name = r.text();
    r.text();
    r.read<uint8_t>();
    m.type = r.read<uint8_t>();
    const auto count = r.count();
    for (int j = 0; j < count; ++j) {
      if (m.type == 0 || m.type == 9) {
        const auto id = r.index(r.morph_size);
        valid(id, morphs.size());
        m.groups.emplace_back(id, r.real());
      } else if (m.type == 1) {
        const auto id = r.vertex(vertex_count);
        const auto delta = r.vec();
        m.offsets[id] += delta;
        finite_morph(m.offsets[id]);
        s.reserve(80);
      } else if (m.type == 2) {
        valid(r.index(r.bone_size), bones.size());
        r.vec();
        r.vec(4);
      } else if (m.type >= 3 && m.type <= 7) {
        r.vertex(vertex_count);
        r.vec(4);
      } else if (m.type == 8) {
        valid(r.index(r.material_size), static_cast<size_t>(material_count),
              true);
        if (r.read<uint8_t>() > 1)
          invalid("Invalid PMX material morph operation");
        for (int k = 0; k < 28; ++k)
          r.real();
      } else if (m.type == 10) {
        r.index(r.rigid_size);
        r.read<uint8_t>();
        r.vec();
        r.vec();
      } else
        invalid("Unknown PMX morph type");
    }
    if (m.type != 0 && m.type != 1)
      s.loss("pmx_nonvertex_morph",
             m.name + ": bone, UV, material, flip or impulse morph behavior "
                      "has no equivalent FBX vertex blend shape.");
  }
  std::vector<int> state(morphs.size());
  const std::function<void(size_t, unsigned)> resolve = [&](size_t i, unsigned depth) {
    if (depth > 256)
      throw skeletal::Failure(ErrorCode::ResourceLimit, "PMX group morph nesting exceeds limit");
    if (state[i] == 2)
      return;
    if (state[i] == 1)
      invalid("Cyclic PMX group morph");
    state[i] = 1;
    auto &m = morphs[i];
    if (m.type == 0)
      for (const auto &[id, weight] : m.groups) {
        resolve(static_cast<size_t>(id), depth + 1);
        for (const auto &[vertex, delta] :
             morphs[static_cast<size_t>(id)].offsets) {
          m.offsets[vertex] += delta * weight;
          finite_morph(m.offsets[vertex]);
          s.reserve(80);
        }
        if (morphs[static_cast<size_t>(id)].type != 0 &&
            morphs[static_cast<size_t>(id)].type != 1)
          s.loss("pmx_partial_group_morph",
                 m.name +
                     ": only vertex displacement members are represented.");
      }
    state[i] = 2;
  };
  auto *blend = FbxBlendShape::Create(s.scene, "PMX morphs");
  mesh->AddDeformer(blend);
  for (size_t i = 0; i < morphs.size(); ++i) {
    resolve(i, 0);
    const auto &m = morphs[i];
    if (m.type != 0 && m.type != 1)
      continue;
    s.reserve(static_cast<std::uint64_t>(vertex_count) * 32);
    auto *shape = FbxShape::Create(s.scene, m.name.toUtf8().constData());
    shape->InitControlPoints(vertex_count);
    for (int v = 0; v < vertex_count; ++v) {
      const auto it = m.offsets.find(v);
      const auto point = vertices[static_cast<size_t>(v)].position +
          (it == m.offsets.end() ? FbxVector4(0, 0, 0) : it->second);
      finite_morph(point);
      shape->SetControlPointAt(point, v);
    }
    auto *channel =
        FbxBlendShapeChannel::Create(s.scene, m.name.toUtf8().constData());
    channel->AddTargetShape(shape, 100);
    blend->AddBlendShapeChannel(channel);
  }
  const auto displays = r.count();
  for (int i = 0; i < displays; ++i) {
    r.text();
    r.text();
    r.read<uint8_t>();
    const auto count = r.count();
    for (int j = 0; j < count; ++j) {
      const auto type = r.read<uint8_t>();
      if (type > 1)
        invalid("Invalid PMX display element");
      valid(r.index(type ? r.morph_size : r.bone_size),
            type ? morphs.size() : bones.size());
    }
  }
  if (displays)
    s.loss("pmx_display_frames", "MMD display-frame organization is omitted.");
  const auto rigid_count = r.count();
  for (int i = 0; i < rigid_count; ++i) {
    r.text();
    r.text();
    valid(r.index(r.bone_size), bones.size(), true);
    r.skip(4);
    for (int k = 0; k < 14; ++k)
      r.real();
    r.skip(1);
  }
  const auto joint_count = r.count();
  for (int i = 0; i < joint_count; ++i) {
    r.text();
    r.text();
    const auto type = r.read<uint8_t>();
    if (type > 5)
      invalid("Unknown PMX joint type");
    valid(r.index(r.rigid_size), static_cast<size_t>(rigid_count), true);
    valid(r.index(r.rigid_size), static_cast<size_t>(rigid_count), true);
    for (int k = 0; k < 24; ++k)
      r.real();
  }
  if (rigid_count || joint_count)
    s.loss("pmx_physics",
           "Rigid bodies and physics joints are omitted; FBX contains the rest "
           "model and skin, without MMD simulation.");
  if (version > 2.05 && r.at < bytes.size()) {
    const auto soft = r.count();
    for (int i = 0; i < soft; ++i) {
      r.text();
      r.text();
      r.skip(1);
      valid(r.index(r.material_size), static_cast<size_t>(material_count));
      r.skip(4);
      r.count();
      r.count();
      r.real();
      r.real();
      r.read<int32_t>();
      for (int k = 0; k < 18; ++k)
        r.real();
      for (int k = 0; k < 4; ++k)
        r.count();
      for (int k = 0; k < 3; ++k)
        r.real();
      const auto anchors = r.count();
      for (int j = 0; j < anchors; ++j) {
        valid(r.index(r.rigid_size), static_cast<size_t>(rigid_count));
        r.vertex(vertex_count);
        r.skip(1);
      }
      const auto pins = r.count();
      for (int j = 0; j < pins; ++j)
        r.vertex(vertex_count);
    }
    if (soft)
      s.loss("pmx_soft_bodies", "PMX soft-body simulation is omitted.");
  }
  if (r.at != bytes.size())
    invalid("Unrecognized trailing PMX data");
  s.scene->GetGlobalSettings().SetAxisSystem(FbxAxisSystem(
      FbxAxisSystem::eYAxis,
      static_cast<FbxAxisSystem::EFrontVector>(-FbxAxisSystem::eParityOdd),
      FbxAxisSystem::eLeftHanded));
  s.scene->GetGlobalSettings().SetSystemUnit(FbxSystemUnit::m);
  s.loss("pmx_unit_assumption",
         "PMX declares no physical scale. Numeric coordinates are preserved in "
         "left-handed Y-up space and tagged as metres before optional output "
         "conversion.");
  s.metadata["pmx_version"] = version;
}
} // namespace formaview::scene_export
