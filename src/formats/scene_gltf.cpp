#include "scene_gltf.hpp"
#include "gltf_storage.hpp"
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>

namespace formaview::scene_export {
namespace {
[[noreturn]] void unsupported(const char *message) {
  throw skeletal::Failure(ErrorCode::Unsupported, message);
}
void *allocate(void *user, cgltf_size size) {
  try {
    static_cast<Scene *>(user)->reserve(size);
    return std::malloc(size);
  } catch (...) {
    return nullptr;
  }
}
void release(void *, void *pointer) { std::free(pointer); }
const cgltf_accessor *attribute(const cgltf_attribute *a, size_t count,
                                cgltf_attribute_type type, int index = 0) {
  for (size_t i = 0; i < count; ++i)
    if (a[i].type == type && a[i].index == index)
      return a[i].data;
  return nullptr;
}
QByteArray data_uri(const char *uri) {
  const QByteArray value(uri);
  const auto comma = value.indexOf(',');
  if (comma < 0 || !value.first(comma).endsWith(";base64"))
    invalid("Only base64 glTF data URIs are supported");
  const auto decoded = QByteArray::fromBase64Encoding(
      value.mid(comma + 1), QByteArray::AbortOnBase64DecodingErrors);
  if (!decoded)
    invalid("Malformed base64 glTF data URI");
  return decoded.decoded;
}
FbxAMatrix from_columns(const float *v) {
  FbxAMatrix result;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      if (!std::isfinite(v[r * 4 + c]))
        invalid("Nonfinite glTF matrix");
      result[r][c] = v[r * 4 + c];
    }
  return result;
}
void property(FbxObject *object, const char *name, double value) {
  auto p = FbxProperty::Create(object, FbxDoubleDT, name);
  p.ModifyFlag(FbxPropertyFlags::eUserDefined, true);
  p.Set(value);
}
FbxFileTexture *texture(Scene &s, const cgltf_data &data,
                        const cgltf_texture_view &view) {
  if (!view.texture)
    return nullptr;
  const auto *image = view.texture->image;
  if (!image)
    unsupported("glTF texture requires an ordinary image fallback");
  QString name;
  try {
    if (image->uri && !QByteArray(image->uri).startsWith("data:"))
      name = s.image(QString::fromUtf8(image->uri));
    else {
      QByteArray bytes;
      if (image->uri)
        bytes = data_uri(image->uri);
      else if (image->buffer_view) {
        const auto *raw = cgltf_buffer_view_data(image->buffer_view);
        if (!raw || image->buffer_view->size > INT_MAX)
          invalid("Invalid embedded glTF image");
        bytes = QByteArray(reinterpret_cast<const char *>(raw),
                           static_cast<qsizetype>(image->buffer_view->size));
      } else
        invalid("Missing glTF image payload");
      // Payload signatures win: mimeType is optional and may be absent or wrong.
      const auto mime = QString::fromUtf8(image->mime_type);
      const auto suffix =
          bytes.startsWith(QByteArray::fromHex("89504e47"))        ? "png"
          : bytes.startsWith(QByteArray::fromHex("ffd8ff"))        ? "jpg"
          : bytes.startsWith("RIFF") && bytes.mid(8, 4) == "WEBP" ? "webp"
          : mime.contains("jpeg")                                  ? "jpg"
          : mime.contains("webp")                                  ? "webp"
                                                                   : "png";
      name = s.image(
          QString("image-%1.%2").arg(image - data.images).arg(suffix), bytes);
    }
  } catch (const skeletal::Failure &e) {
    if (e.error.code == ErrorCode::Cancelled ||
        e.error.code == ErrorCode::ResourceLimit)
      throw;
    s.loss("external_texture_unavailable", QString::fromUtf8(e.what()));
    return nullptr;
  }
  const auto uv = view.has_transform && view.transform.has_texcoord
                      ? view.transform.texcoord
                      : view.texcoord;
  auto *t = s.texture(name, QString("UV%1").arg(uv));
  // glTF uses top-left UV coordinates. Vertices are flipped below; the
  // transform is conjugated by that flip so transformed textures retain their
  // orientation.
  if (view.has_transform) {
    const auto &v = view.transform;
    if (std::abs(v.rotation) > 1e-8)
      s.loss("gltf_texture_rotation",
             "Rotated KHR_texture_transform is retained as metadata; FBX "
             "texture rotation semantics vary by importer.");
    t->SetScale(v.scale[0], v.scale[1]);
    t->SetTranslation(v.offset[0], 1 - v.offset[1] - v.scale[1]);
    t->SetRotation(0, 0, -v.rotation * 180 / 3.14159265358979323846);
  }
  if (view.texture->sampler) {
    const auto &sampler = *view.texture->sampler;
    t->WrapModeU.Set(sampler.wrap_s == cgltf_wrap_mode_clamp_to_edge
                         ? FbxTexture::eClamp
                         : FbxTexture::eRepeat);
    t->WrapModeV.Set(sampler.wrap_t == cgltf_wrap_mode_clamp_to_edge
                         ? FbxTexture::eClamp
                         : FbxTexture::eRepeat);
    if (sampler.wrap_s == cgltf_wrap_mode_mirrored_repeat ||
        sampler.wrap_t == cgltf_wrap_mode_mirrored_repeat)
      s.loss("gltf_mirrored_repeat",
             "FBX texture wrapping uses repeat in place of mirrored repeat.");
    s.loss("gltf_sampler_filter", "glTF sampler minification/magnification "
                                  "filters have no portable FBX equivalent.");
  }
  return t;
}
std::vector<FbxSurfacePhong *> materials(Scene &s, const cgltf_data &data) {
  std::vector<FbxSurfacePhong *> out;
  for (size_t i = 0; i < data.materials_count; ++i) {
    s.check();
    const auto &m = data.materials[i];
    auto *dst = FbxSurfacePhong::Create(s.scene, m.name ? m.name : "Material");
    const auto &pbr = m.pbr_metallic_roughness;
    dst->Diffuse.Set(FbxDouble3(pbr.base_color_factor[0],
                                pbr.base_color_factor[1],
                                pbr.base_color_factor[2]));
    dst->TransparencyFactor.Set(m.alpha_mode == cgltf_alpha_mode_opaque
                                    ? 0
                                    : 1 - pbr.base_color_factor[3]);
    dst->Emissive.Set(FbxDouble3(m.emissive_factor[0], m.emissive_factor[1],
                                 m.emissive_factor[2]));
    if (m.has_emissive_strength)
      dst->EmissiveFactor.Set(m.emissive_strength.emissive_strength);
    property(dst, "glTF.metallic", pbr.metallic_factor);
    property(dst, "glTF.roughness", pbr.roughness_factor);
    property(dst, "glTF.alphaMode", m.alpha_mode);
    property(dst, "glTF.alphaCutoff", m.alpha_cutoff);
    property(dst, "glTF.doubleSided", m.double_sided);
    property(dst, "glTF.unlit", m.unlit);
    if (auto *t = texture(s, data, pbr.base_color_texture))
      dst->Diffuse.ConnectSrcObject(t);
    if (auto *t = texture(s, data, m.normal_texture))
      dst->NormalMap.ConnectSrcObject(t);
    if (auto *t = texture(s, data, m.emissive_texture))
      dst->Emissive.ConnectSrcObject(t);
    for (const auto &[view, name] :
         std::vector<std::pair<const cgltf_texture_view *, const char *>>{
             {&pbr.metallic_roughness_texture, "glTF.metallicRoughnessTexture"},
             {&m.occlusion_texture, "glTF.occlusionTexture"}})
      if (auto *t = texture(s, data, *view)) {
        auto p = FbxProperty::Create(dst, FbxDouble3DT, name);
        p.ModifyFlag(FbxPropertyFlags::eUserDefined, true);
        p.ConnectSrcObject(t);
      }
    s.loss("gltf_pbr_shading",
           "Base colour, normal/emission textures and material slots are "
           "retained. Metallic/roughness/alpha/culling semantics are metadata; "
           "FBX Phong does not reproduce glTF PBR shading.");
    if (m.has_pbr_specular_glossiness || m.has_clearcoat ||
        m.has_transmission || m.has_volume || m.has_ior || m.has_specular ||
        m.has_sheen || m.has_iridescence || m.has_anisotropy ||
        m.has_dispersion)
      s.loss("gltf_material_extensions",
             "Advanced glTF material extension shading is not reproduced in "
             "FBX Phong.");
    out.push_back(dst);
  }
  return out;
}
} // namespace
const std::vector<float> &GltfAccessors::read(const cgltf_accessor *a) {
  if (!a || a->count > scene.limits.elements)
    invalid("Invalid glTF accessor count");
  if (const auto it = values.find(a); it != values.end())
    return it->second;
  const auto components = cgltf_num_components(a->type);
  if (!components ||
      a->count > scene.limits.decoded_bytes / (components * sizeof(float)))
    invalid("glTF accessor exceeds budget");
  const auto count = a->count * components;
  scene.reserve(count * sizeof(float));
  auto &out = values[a];
  out.resize(count);
  auto base = *a;
  base.is_sparse = false;
  if (cgltf_accessor_unpack_floats(&base, out.data(), count) != count)
    invalid("Cannot read glTF accessor");
  if (a->is_sparse) {
    const auto &s = a->sparse;
    cgltf_accessor indices{};
    indices.type = cgltf_type_scalar;
    indices.component_type = s.indices_component_type;
    indices.count = s.count;
    indices.stride = cgltf_component_size(indices.component_type);
    indices.buffer_view = s.indices_buffer_view;
    indices.offset = s.indices_byte_offset;
    auto sparse = base;
    sparse.buffer_view = s.values_buffer_view;
    sparse.offset = s.values_byte_offset;
    sparse.count = s.count;
    sparse.stride = cgltf_calc_size(sparse.type, sparse.component_type);
    size_t previous{};
    for (size_t i = 0; i < s.count; ++i) {
      scene.check();
      const auto at = cgltf_accessor_read_index(&indices, i);
      if (at >= a->count || (i && at <= previous))
        invalid("Invalid glTF sparse indices");
      previous = at;
      if (!cgltf_accessor_read_float(&sparse, i, out.data() + at * components,
                                     components))
        invalid("Cannot read sparse glTF values");
    }
  }
  for (const auto v : out)
    if (!std::isfinite(v))
      invalid("Nonfinite glTF accessor value");
  return out;
}
void read_gltf(Scene &s, const QByteArray &bytes) {
  cgltf_options options{};
  options.memory = {allocate, release, &s};
  cgltf_data *raw{};
  if (cgltf_parse(&options, bytes.constData(),
                  static_cast<size_t>(bytes.size()),
                  &raw) != cgltf_result_success) {
    s.check();
    invalid("Invalid or over-budget glTF document");
  }
  std::vector<QByteArray> buffers;
  const std::unique_ptr<cgltf_data, decltype(&cgltf_free)> owner(raw,
                                                                 cgltf_free);
  auto &data = *raw;
  if (QString::fromUtf8(data.asset.version) != "2.0")
    unsupported("Only glTF 2.0 is supported");
  if (data.nodes_count > s.limits.bones ||
      data.animations_count > s.limits.clips)
    invalid("glTF scene exceeds node/clip limits");
  for (size_t i = 0; i < data.extensions_required_count; ++i) {
    const QString ext = QString::fromUtf8(data.extensions_required[i]);
    if (ext != "KHR_texture_transform" && ext != "KHR_materials_unlit" &&
        ext != "KHR_mesh_quantization" && ext != "VRMC_vrm" &&
        ext != "VRMC_materials_mtoon")
      unsupported(
          ("Unsupported required glTF extension: " + ext).toUtf8().constData());
  }
  buffers.resize(data.buffers_count);
  for (size_t i = 0; i < data.buffers_count; ++i) {
    s.check();
    auto &b = data.buffers[i];
    if (!b.uri) {
      if (i || !data.bin || b.size > data.bin_size)
        invalid("Missing GLB buffer");
      b.data = const_cast<void *>(data.bin);
    } else {
      buffers[i] = QByteArray(b.uri).startsWith("data:")
                       ? data_uri(b.uri)
                       : s.load(s.relative(QString::fromUtf8(b.uri)));
      s.reserve(static_cast<std::uint64_t>(buffers[i].size()));
      if (b.size > static_cast<size_t>(buffers[i].size()))
        invalid("Truncated glTF buffer");
      b.data = buffers[i].data();
    }
    b.data_free_method = cgltf_data_free_method_none;
  }
  gltf::validate_loaded_storage(data, s.limits, s.stop);
  if (cgltf_validate(raw) != cgltf_result_success)
    invalid("glTF structure validation failed");
  GltfAccessors access{s, {}};
  s.scene->GetGlobalSettings().SetAxisSystem(FbxAxisSystem::MayaYUp);
  s.scene->GetGlobalSettings().SetSystemUnit(FbxSystemUnit::m);
  std::set<const cgltf_node *> joints;
  for (size_t i = 0; i < data.skins_count; ++i)
    for (size_t j = 0; j < data.skins[i].joints_count; ++j)
      joints.insert(data.skins[i].joints[j]);
  std::vector<FbxNode *> nodes(data.nodes_count);
  for (size_t i = 0; i < data.nodes_count; ++i) {
    s.check();
    const auto &src = data.nodes[i];
    s.reserve(2048);
    auto *node = FbxNode::Create(
        s.scene, src.name ? src.name : ("Node_" + std::to_string(i)).c_str());
    nodes[i] = node;
    float local[16];
    cgltf_node_transform_local(&src, local);
    const auto m = from_columns(local);
    if (src.has_matrix) {
      FbxAMatrix reconstructed;
      reconstructed.SetTRS(m.GetT(), m.GetR(), m.GetS());
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
          if (std::abs(m[r][c] - reconstructed[r][c]) >
              1e-5 * std::max(1.0, std::abs(m[r][c])))
            unsupported("glTF node matrix cannot be represented as FBX TRS");
      node->LclTranslation.Set(m.GetT());
      node->LclRotation.Set(m.GetR());
      node->LclScaling.Set(m.GetS());
    } else {
      // Decomposing a signed-scale matrix can redistribute signs into rotation.
      // Animation replaces individual TRS channels, so retain source channels.
      FbxAMatrix rotation;
      rotation.SetQ(FbxQuaternion(src.rotation[0], src.rotation[1],
                                  src.rotation[2], src.rotation[3]));
      node->LclTranslation.Set(FbxDouble3(src.translation[0], src.translation[1], src.translation[2]));
      node->LclRotation.Set(rotation.GetR());
      node->LclScaling.Set(FbxDouble3(src.scale[0], src.scale[1], src.scale[2]));
    }
    node->SetRotationOrder(FbxNode::eSourcePivot, eEulerXYZ);
    node->SetTransformationInheritType(FbxTransform::eInheritRSrs);
    if (joints.contains(&src)) {
      auto *bone = FbxSkeleton::Create(s.scene, node->GetName());
      bone->SetSkeletonType(FbxSkeleton::eLimbNode);
      node->SetNodeAttribute(bone);
    }
    if (src.camera || src.light)
      s.loss("gltf_camera_light", "Camera/light objects are omitted; their "
                                  "transform nodes are retained.");
    if (src.has_mesh_gpu_instancing)
      unsupported(
          "EXT_mesh_gpu_instancing requires explicit instance expansion");
  }
  for (size_t i = 0; i < data.nodes_count; ++i)
    (data.nodes[i].parent
         ? nodes[static_cast<size_t>(data.nodes[i].parent - data.nodes)]
         : s.scene->GetRootNode())
        ->AddChild(nodes[i]);
  if (data.scenes_count > 1)
    s.loss("gltf_multiple_scenes",
           "All source nodes are exported into one FBX scene; glTF scene "
           "selection is not retained.");
  auto mats = materials(s, data);
  GltfMorphs morphs;
  for (size_t ni = 0; ni < data.nodes_count; ++ni) {
    const auto &src = data.nodes[ni];
    if (!src.mesh)
      continue;
    for (size_t pi = 0; pi < src.mesh->primitives_count; ++pi) {
      s.check();
      const auto &p = src.mesh->primitives[pi];
      if (p.has_draco_mesh_compression)
        unsupported("Draco compressed glTF needs decoding before export");
      if (p.type != cgltf_primitive_type_triangles &&
          p.type != cgltf_primitive_type_triangle_strip &&
          p.type != cgltf_primitive_type_triangle_fan)
        unsupported(
            "glTF point/line primitives cannot be exported as polygon meshes");
      const auto *positions = attribute(p.attributes, p.attributes_count,
                                        cgltf_attribute_type_position);
      if (!positions || positions->type != cgltf_type_vec3 ||
          positions->count > INT_MAX)
        invalid("Invalid glTF positions");
      const auto count = positions->count;
      const auto &v = access.read(positions);
      s.reserve(count * 256);
      auto *mesh = FbxMesh::Create(s.scene, (std::string(nodes[ni]->GetName()) +
                                             "_primitive_" + std::to_string(pi))
                                                .c_str());
      auto *node = FbxNode::Create(s.scene, mesh->GetName());
      node->SetTransformationInheritType(FbxTransform::eInheritRSrs);
      nodes[ni]->AddChild(node);
      node->SetNodeAttribute(mesh);
      mesh->InitControlPoints(static_cast<int>(count));
      for (size_t i = 0; i < count; ++i)
        mesh->SetControlPointAt(
            FbxVector4(v[i * 3], v[i * 3 + 1], v[i * 3 + 2]),
            static_cast<int>(i));
      const auto *normals = attribute(p.attributes, p.attributes_count,
                                      cgltf_attribute_type_normal);
      const std::vector<float> *nv{};
      if (normals) {
        if (normals->type != cgltf_type_vec3 || normals->count != count)
          invalid("Invalid glTF normals");
        nv = &access.read(normals);
        auto *layer = mesh->CreateElementNormal();
        layer->SetMappingMode(FbxLayerElement::eByControlPoint);
        layer->SetReferenceMode(FbxLayerElement::eDirect);
        for (size_t i = 0; i < count; ++i)
          layer->GetDirectArray().Add(
              FbxVector4((*nv)[i * 3], (*nv)[i * 3 + 1], (*nv)[i * 3 + 2]));
      }
      for (size_t ai = 0; ai < p.attributes_count; ++ai) {
        const auto &a = p.attributes[ai];
        if (a.data->count != count)
          invalid("glTF attribute count mismatch");
        if (a.type == cgltf_attribute_type_texcoord) {
          if (a.data->type != cgltf_type_vec2)
            invalid("Invalid glTF UV");
          const auto &uv = access.read(a.data);
          auto *layer =
              mesh->CreateElementUV(("UV" + std::to_string(a.index)).c_str());
          layer->SetMappingMode(FbxLayerElement::eByControlPoint);
          layer->SetReferenceMode(FbxLayerElement::eDirect);
          for (size_t i = 0; i < count; ++i)
            layer->GetDirectArray().Add(
                FbxVector2(uv[i * 2], 1 - uv[i * 2 + 1]));
        } else if (a.type == cgltf_attribute_type_color) {
          if (a.data->type != cgltf_type_vec3 &&
              a.data->type != cgltf_type_vec4)
            invalid("Invalid glTF colours");
          const auto &col = access.read(a.data);
          const auto n = cgltf_num_components(a.data->type);
          auto *layer = mesh->CreateElementVertexColor();
          layer->SetName(("COLOR_" + std::to_string(a.index)).c_str());
          layer->SetMappingMode(FbxLayerElement::eByControlPoint);
          layer->SetReferenceMode(FbxLayerElement::eDirect);
          for (size_t i = 0; i < count; ++i)
            layer->GetDirectArray().Add(FbxColor(col[i * n], col[i * n + 1],
                                                 col[i * n + 2],
                                                 n == 4 ? col[i * n + 3] : 1));
        } else if (a.type == cgltf_attribute_type_tangent) {
          if (a.data->type != cgltf_type_vec4)
            invalid("Invalid glTF tangents");
          const auto &t = access.read(a.data);
          auto *layer = mesh->CreateElementTangent();
          layer->SetMappingMode(FbxLayerElement::eByControlPoint);
          layer->SetReferenceMode(FbxLayerElement::eDirect);
          for (size_t i = 0; i < count; ++i)
            layer->GetDirectArray().Add(FbxVector4(
                t[i * 4], t[i * 4 + 1], t[i * 4 + 2], -t[i * 4 + 3]));
        } else if (a.type == cgltf_attribute_type_custom)
          s.loss("gltf_custom_attribute", QString::fromUtf8(a.name));
      }
      std::vector<size_t> indices;
      const auto corners = p.indices ? p.indices->count : count;
      if (corners > s.limits.elements)
        invalid("glTF index budget exceeded");
      s.reserve(corners * sizeof(size_t));
      indices.resize(corners);
      if (p.indices &&
          (p.indices->type != cgltf_type_scalar || p.indices->is_sparse))
        unsupported("Sparse or non-scalar glTF index accessor is unsupported");
      for (size_t i = 0; i < corners; ++i) {
        indices[i] = p.indices ? cgltf_accessor_read_index(p.indices, i) : i;
        if (indices[i] >= count)
          invalid("glTF index outside vertex array");
      }
      if (p.material)
        node->AddMaterial(
            mats[static_cast<size_t>(p.material - data.materials)]);
      auto *layer = mesh->CreateElementMaterial();
      layer->SetMappingMode(FbxLayerElement::eByPolygon);
      layer->SetReferenceMode(FbxLayerElement::eIndexToDirect);
      const auto triangle = [&](size_t a, size_t b, size_t c) {
        mesh->BeginPolygon(p.material ? 0 : -1);
        mesh->AddPolygon(static_cast<int>(indices[a]));
        mesh->AddPolygon(static_cast<int>(indices[b]));
        mesh->AddPolygon(static_cast<int>(indices[c]));
        mesh->EndPolygon();
      };
      if (p.type == cgltf_primitive_type_triangles) {
        if (corners % 3)
          invalid("Incomplete glTF triangle");
        for (size_t i = 0; i < corners; i += 3)
          triangle(i, i + 1, i + 2);
      } else
        for (size_t i = 2; i < corners; ++i)
          if (p.type == cgltf_primitive_type_triangle_fan)
            triangle(0, i - 1, i);
          else if (i % 2)
            triangle(i - 1, i - 2, i);
          else
            triangle(i - 2, i - 1, i);
      if (!normals)
        mesh->GenerateNormals();
      if (src.skin) {
        const auto &skin = *src.skin;
        if (skin.joints_count > s.limits.bones)
          invalid("glTF joint budget exceeded");
        const std::vector<float> *inverse{};
        if (skin.inverse_bind_matrices) {
          if (skin.inverse_bind_matrices->type != cgltf_type_mat4 ||
              skin.inverse_bind_matrices->count != skin.joints_count)
            invalid("Invalid glTF inverse bind matrices");
          inverse = &access.read(skin.inverse_bind_matrices);
        }
        auto *deformer = FbxSkin::Create(s.scene, "glTF skin");
        mesh->AddDeformer(deformer);
        std::vector<FbxCluster *> clusters(skin.joints_count);
        for (size_t j = 0; j < skin.joints_count; ++j) {
          auto *cluster = FbxCluster::Create(s.scene, "glTF joint");
          clusters[j] = cluster;
          deformer->AddCluster(cluster);
          cluster->SetLink(
              nodes[static_cast<size_t>(skin.joints[j] - data.nodes)]);
          cluster->SetLinkMode(FbxCluster::eNormalize);
          FbxAMatrix identity;
          cluster->SetTransformMatrix(identity);
          if (inverse) {
            const FbxMatrix ibm(from_columns(inverse->data() + j * 16));
            if (std::abs(ibm.Determinant()) < 1e-15)
              invalid("Singular glTF inverse bind matrix");
            // The cluster API can decompose its stored matrix on readback.
            // Reject bindings it cannot retain instead of exporting a changed
            // skin palette (for example, shear from scaled joint hierarchies).
            const auto inverseBind = ibm.Inverse();
            FbxAMatrix bind;
            for (int r = 0; r < 4; ++r)
              for (int c = 0; c < 4; ++c)
                bind[r][c] = inverseBind.Get(r, c);
            cluster->SetTransformLinkMatrix(bind);
            FbxAMatrix stored;
            cluster->GetTransformLinkMatrix(stored);
            const auto retained = FbxMatrix(stored).Inverse();
            // Match the node TRS and animation component precision policy.
            for (int r = 0; r < 4; ++r)
              for (int c = 0; c < 4; ++c)
                if (!std::isfinite(retained.Get(r, c)) ||
                    std::abs(retained.Get(r, c) - ibm.Get(r, c)) >
                        1e-5 * std::max(1.0, std::abs(ibm.Get(r, c))))
                  unsupported("glTF inverse bind matrix cannot be retained by FBX cluster API");
          } else
            cluster->SetTransformLinkMatrix(identity);
        }
        std::vector<double> sums(count);
        bool found{};
        for (size_t ai = 0; ai < p.attributes_count; ++ai)
          if (p.attributes[ai].type == cgltf_attribute_type_weights) {
            found = true;
            const auto &w = p.attributes[ai];
            const auto *j = attribute(p.attributes, p.attributes_count,
                                      cgltf_attribute_type_joints, w.index);
            if (!j || w.data->type != cgltf_type_vec4 ||
                j->type != cgltf_type_vec4 || j->count != count ||
                j->normalized)
              invalid("Invalid glTF joint/weight sets");
            const auto &wv = access.read(w.data), &jv = access.read(j);
            for (size_t i = 0; i < count; ++i)
              for (size_t k = 0; k < 4; ++k) {
                const auto weight = wv[i * 4 + k], joint = jv[i * 4 + k];
                if (weight < 0 || joint < 0 || joint >= skin.joints_count ||
                    std::floor(joint) != joint)
                  invalid("Invalid glTF skin influence");
                if (weight > 0) {
                  clusters[static_cast<size_t>(joint)]->AddControlPointIndex(
                      static_cast<int>(i), weight);
                  sums[i] += weight;
                  s.reserve(24);
                }
              }
          }
        if (!found)
          invalid("Skinned glTF primitive has no weights");
        for (const auto sum : sums) {
          if (sum <= 0)
            invalid("Skinned glTF vertex has no valid influence");
          if (std::abs(sum - 1) > 1e-4)
            s.loss("gltf_weight_normalization",
                   "Source influence values are retained; the FBX skin "
                   "normalizes non-unit sums during deformation.");
        }
      }
      if (p.targets_count) {
        auto *blend = FbxBlendShape::Create(s.scene, "glTF morphs");
        mesh->AddDeformer(blend);
        auto &channels = morphs[&src];
        if (channels.size() < p.targets_count)
          channels.resize(p.targets_count);
        for (size_t ti = 0; ti < p.targets_count; ++ti) {
          s.reserve(count * 64);
          const auto &target = p.targets[ti];
          const auto *delta =
                         attribute(target.attributes, target.attributes_count,
                                   cgltf_attribute_type_position),
                     *dn = attribute(target.attributes, target.attributes_count,
                                     cgltf_attribute_type_normal);
          if (delta &&
              (delta->type != cgltf_type_vec3 || delta->count != count))
            invalid("Invalid glTF position morph");
          if (dn && (dn->type != cgltf_type_vec3 || dn->count != count || !nv))
            invalid("Invalid glTF normal morph");
          const auto *dv = delta ? &access.read(delta) : nullptr,
                     *nd = dn ? &access.read(dn) : nullptr;
          const auto name = ti < src.mesh->target_names_count
                                ? std::string(src.mesh->target_names[ti])
                                : "Morph_" + std::to_string(ti);
          auto *shape = FbxShape::Create(s.scene, name.c_str());
          shape->InitControlPoints(static_cast<int>(count));
          for (size_t i = 0; i < count; ++i)
            shape->SetControlPointAt(
                FbxVector4(v[i * 3] + (dv ? (*dv)[i * 3] : 0),
                           v[i * 3 + 1] + (dv ? (*dv)[i * 3 + 1] : 0),
                           v[i * 3 + 2] + (dv ? (*dv)[i * 3 + 2] : 0)),
                static_cast<int>(i));
          if (nd) {
            auto *nl = shape->CreateElementNormal();
            nl->SetMappingMode(FbxLayerElement::eByControlPoint);
            nl->SetReferenceMode(FbxLayerElement::eDirect);
            for (size_t i = 0; i < count; ++i)
              nl->GetDirectArray().Add(
                  FbxVector4((*nv)[i * 3] + (*nd)[i * 3],
                             (*nv)[i * 3 + 1] + (*nd)[i * 3 + 1],
                             (*nv)[i * 3 + 2] + (*nd)[i * 3 + 2]));
          }
          if (attribute(target.attributes, target.attributes_count,
                        cgltf_attribute_type_tangent))
            s.loss("gltf_morph_tangents",
                   "Morph position and normal deltas are retained; tangent "
                   "deltas are omitted.");
          auto *channel = FbxBlendShapeChannel::Create(s.scene, name.c_str());
          channel->AddTargetShape(shape, 100);
          blend->AddBlendShapeChannel(channel);
          channels[ti].push_back(channel);
          const auto weight = src.weights_count > ti ? src.weights[ti]
                              : src.mesh->weights_count > ti
                                  ? src.mesh->weights[ti]
                                  : 0;
          channel->DeformPercent.Set(weight * 100.0);
        }
      }
    }
  }
  gltf_animations(s, data, access, nodes, morphs);
  for (size_t i = 0; i < data.data_extensions_count; ++i)
    string_property(
        s.scene,
        ("glTF.Extension." + std::string(data.data_extensions[i].name)).c_str(),
        QString::fromUtf8(data.data_extensions[i].data));
  for (size_t i = 0; i < data.extensions_used_count; ++i) {
    const QString name = QString::fromUtf8(data.extensions_used[i]);
    if (name == "VRM" || name.startsWith("VRMC_"))
      s.loss("vrm_semantics",
             name + ": geometry, skin and morphs are exported; humanoid "
                    "mapping, expressions, spring physics, look-at constraints "
                    "and MToon runtime behavior are not FBX equivalents.");
    else if (name != "KHR_texture_transform" &&
             name != "KHR_mesh_quantization" && name != "KHR_materials_unlit")
      s.loss("gltf_extension", name);
  }
}
} // namespace formaview::scene_export
