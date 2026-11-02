#include "formaview/fbx.hpp"
#include "formaview/file_access.hpp"
#include <cmath>
#include <limits>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <ufbx.h>

namespace formaview::skeletal {
namespace {
void cancelled(std::stop_token stop) {
  if (stop.stop_requested())
    throw Failure(ErrorCode::Cancelled, "FBX operation cancelled");
}
Matrix4 convert(const ufbx_matrix &m) {
  return {m.m00, m.m01, m.m02, m.m03, m.m10, m.m11, m.m12, m.m13,
          m.m20, m.m21, m.m22, m.m23, 0,     0,     0,     1};
}
// SDK 2020.2 inheritance boundary, independently checked against SDK-authored
// negative-scale, pivot, Euler-order and mixed-inheritance chains. Source
// local channels remain ufbx-decoded; only their hierarchical composition is
// adapted. A singular parent has no unique rotation/scale decomposition and
// retains the reader result instead of manufacturing an inverse.
struct EvaluatedWorlds {
  std::vector<ufbx_matrix> values;
  bool complete{true};
};
EvaluatedWorlds sdk_worlds(const ufbx_scene &scene, std::stop_token stop) {
  EvaluatedWorlds result;
  result.values.resize(scene.nodes.count);
  std::vector<const ufbx_node *> order(scene.nodes.begin(), scene.nodes.end());
  std::stable_sort(order.begin(), order.end(), [](auto *a, auto *b) {
    return a->node_depth < b->node_depth;
  });
  const auto product = [](const ufbx_matrix &a, const ufbx_matrix &b) {
    return ufbx_matrix_mul(&a, &b);
  };
  for (const auto *node : order) {
    cancelled(stop);
    auto &world = result.values[node->typed_id];
    world = node->node_to_world;
    if (!node->parent) continue;
    const auto &parent = result.values[node->parent->typed_id];
    auto inherited = parent;
    inherited.cols[3] = {0, 0, 0};
    auto rotation = node->local_transform;
    rotation.translation = {0, 0, 0};
    rotation.scale = {1, 1, 1};
    const auto localRotation = ufbx_transform_to_matrix(&rotation);
    auto scaling = ufbx_identity_transform;
    scaling.scale = node->local_transform.scale;
    const auto localScale = ufbx_transform_to_matrix(&scaling);
    if (node->inherit_mode == UFBX_INHERIT_MODE_IGNORE_PARENT_SCALE) {
      const auto scale = node->parent->local_transform.scale;
      if (scale.x == 0 || scale.y == 0 || scale.z == 0) {
        result.complete = false;
        continue;
      }
      auto inverse = ufbx_identity_transform;
      inverse.scale = {1 / scale.x, 1 / scale.y, 1 / scale.z};
      inherited = product(inherited, ufbx_transform_to_matrix(&inverse));
    }
    if (node->inherit_mode == UFBX_INHERIT_MODE_COMPONENTWISE_SCALE) {
      auto normalized = inherited;
      const double sign = ufbx_matrix_determinant(&inherited) < 0 ? -1 : 1;
      bool valid = ufbx_matrix_determinant(&inherited) != 0;
      for (auto column = 0; column < 3; ++column) {
        auto &v = normalized.cols[column];
        const double length = std::hypot(v.x, v.y, v.z);
        if (!(length > 0) || !std::isfinite(length)) { valid = false; break; }
        v.x *= sign / length; v.y *= sign / length; v.z *= sign / length;
      }
      if (!valid) { result.complete = false; continue; }
      // FBX AMatrix extracts XYZ Euler angles from signed normalized columns.
      // Gram-Schmidt and polar/quaternion decomposition differ under shear.
      constexpr double radiansToDegrees = 57.2957795130823208768;
      const double cy=std::hypot(normalized.m00,normalized.m10);
      // FbxEuler::DegenerateThreshold defaults to 16*FLT_EPSILON in SDK 2020.2.
      // Near gimbal lock the SDK sets Z=0 and retains the coupled angle in X.
      const bool singular=cy<=16.0*std::numeric_limits<float>::epsilon();
      const ufbx_vec3 angles{
          (singular?std::atan2(-normalized.m12,normalized.m11):std::atan2(normalized.m21,normalized.m22))*radiansToDegrees,
          std::atan2(-normalized.m20,cy)*radiansToDegrees,
          (singular?0.0:std::atan2(normalized.m10,normalized.m00))*radiansToDegrees};
      auto transform = ufbx_identity_transform;
      transform.rotation = ufbx_euler_to_quat(angles, UFBX_ROTATION_ORDER_XYZ);
      const auto parentRotation = ufbx_transform_to_matrix(&transform);
      const auto inverseRotation = ufbx_matrix_invert(&parentRotation);
      const auto shearScale = product(inverseRotation, inherited);
      world = product(product(product(parentRotation, localRotation), shearScale), localScale);
    } else {
      // Rrs cancels the parent's LOCAL scale before ordinary multiplication;
      // its remaining inherited scale/shear stays before the local rotation.
      world = product(product(inherited, localRotation), localScale);
    }
    world.cols[3] = ufbx_transform_position(&parent, node->local_transform.translation);
  }
  return result;
}
ufbx_matrix effective_local(const ufbx_node &node,
                            const std::vector<ufbx_matrix> &worlds) {
  if (!node.parent) return worlds[node.typed_id];
  const auto &parent = worlds[node.parent->typed_id];
  const auto determinant = ufbx_matrix_determinant(&parent);
  if (determinant == 0 || !std::isfinite(determinant)) return node.node_to_parent;
  const auto inverse = ufbx_matrix_invert(&parent);
  return ufbx_matrix_mul(&inverse, &worlds[node.typed_id]);
}

std::string str(ufbx_string s) { return {s.data, s.length}; }
FbxMaterial material_value(const ufbx_material &material) {
  FbxMaterial value;
  value.name = str(material.name);
  const auto &map = material.pbr.base_color;
  if (map.has_value)
    value.colour = {map.value_vec3.x, map.value_vec3.y, map.value_vec3.z};
  if (material.pbr.opacity.has_value)
    value.opacity = material.pbr.opacity.value_real;
  if (map.texture && map.texture_enabled) {
    value.colour_texture = str(map.texture->relative_filename.length
                                   ? map.texture->relative_filename
                                   : map.texture->filename);
    value.colour_uv = str(map.texture->uv_set);
    value.colour_resolved = str(map.texture->filename);
    value.wrap_u = map.texture->wrap_u == UFBX_WRAP_CLAMP ? 1 : 0;
    value.wrap_v = map.texture->wrap_v == UFBX_WRAP_CLAMP ? 1 : 0;
    const auto &uv = map.texture->uv_to_texture;
    value.colour_uv_transform = {uv.m00, uv.m01, uv.m03,
                                 uv.m10, uv.m11, uv.m13};
    if (map.texture->content.size) {
      if (map.texture->content.size > 64ULL * 1024 * 1024)
        throw Failure(ErrorCode::ResourceLimit,
                      "Embedded FBX texture exceeds encoded budget");
      const auto *begin =
          static_cast<const std::byte *>(map.texture->content.data);
      value.colour_content = std::make_shared<const std::vector<std::byte>>(
          begin, begin + map.texture->content.size);
    }
  }
  return value;
}
SourceMetadata source(const SourceMetadata &root, const ufbx_element &e) {
  auto s = root;
  s.identity.object_key = "element/" + std::to_string(e.element_id);
  s.original_name = str(e.name);
  return s;
}
ufbx_progress_result progress(void *user, const ufbx_progress *) {
  return static_cast<std::stop_token *>(user)->stop_requested()
             ? UFBX_PROGRESS_CANCEL
             : UFBX_PROGRESS_CONTINUE;
}
[[noreturn]] void failed(const ufbx_error &e) {
  auto code = ErrorCode::InvalidArgument;
  if (e.type == UFBX_ERROR_CANCELLED)
    code = ErrorCode::Cancelled;
  if (e.type == UFBX_ERROR_MEMORY_LIMIT ||
      e.type == UFBX_ERROR_ALLOCATION_LIMIT)
    code = ErrorCode::ResourceLimit;
  throw Failure(code, "ufbx: " + str(e.description));
}
template <class T> void append(Buffer &b, const T &v) {
  const auto old = b.bytes.size();
  b.bytes.resize(old + sizeof(T));
  std::memcpy(b.bytes.data() + old, &v, sizeof(T));
}
struct MaterialFiles {
  AccessScope scope;
  std::stop_token stop;
  std::vector<InputLease> leases;
  std::vector<FbxData::Resource> resources;
};
bool open_material(void *user, ufbx_stream *stream, const char *name,
                   std::size_t length, const ufbx_open_file_info *info) {
  if (info->type != UFBX_OPEN_FILE_OBJ_MTL)
    return false;
  auto &context = *static_cast<MaterialFiles *>(user);
  FbxData::Resource resource;
  if (info->original_filename.size)
    resource.reference.assign(
        static_cast<const char *>(info->original_filename.data),
        info->original_filename.size);
  resource.resolved.assign(name, length);
  try {
    cancelled(context.stop);
    const auto path = std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t *>(name), length));
    const auto original = std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t *>(resource.reference.data()), resource.reference.size()));
    if (original.has_root_path())
      throw Failure(ErrorCode::OutOfScope, "Absolute material library references are not permitted");
    auto lease =
        InputLease::open(context.scope, path, 64ULL * 1024 * 1024, context.stop);
    std::ifstream input(path, std::ios::binary);
    std::vector<std::byte> bytes(
        static_cast<std::size_t>(lease.snapshot().size_bytes));
    if (!input.read(reinterpret_cast<char *>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size())))
      throw Failure(ErrorCode::Io, "Cannot read material library");
    ufbx_error error{};
    if (!ufbx_open_memory_ctx(stream, info->context, bytes.data(), bytes.size(),
                              nullptr, &error))
      throw Failure(ErrorCode::ResourceLimit,
                    "Material stream allocation failed");
    resource.loaded = true;
    resource.detail = lease.snapshot().content_sha256;
    context.leases.push_back(std::move(lease));
    context.resources.push_back(std::move(resource));
    return true;
  } catch (const std::exception &error) {
    resource.detail = error.what();
    context.resources.push_back(std::move(resource));
    return false;
  }
}
} // namespace
struct FbxDocument::Impl {
  std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> scene{
      nullptr, ufbx_free_scene};
  FbxData data;
  Limits limits;
};
FbxDocument::FbxDocument(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
const FbxData &FbxDocument::data() const noexcept { return impl_->data; }
transform::Evidence FbxDocument::transform_evidence(std::stop_token stop) const {
  cancelled(stop);
  transform::Evidence out;
  out.sha256 = impl_->data.source.identity.sha256;
  out.adapterVersion = "ufbx-0.23.1/sdk-inheritance-1/r13-evidence-4";
  const auto &scene = *impl_->scene;
  if (scene.nodes.count > transform::Policy{}.maxNodes)
    return out; // Incomplete, never silently truncate the full node domain.
  const auto &settings = scene.settings;
  const char *coordinateProps[] = {"UnitScaleFactor", "CoordAxis", "CoordAxisSign",
      "UpAxis", "UpAxisSign", "FrontAxis", "FrontAxisSign"};
  if (std::all_of(std::begin(coordinateProps), std::end(coordinateProps),
                  [&](const char *key) { return ufbx_find_prop(&settings.props, key); })) {
    transform::Coordinates c;
    c.metresPerUnit = settings.unit_meters;
    const ufbx_coordinate_axis axes[] = {settings.axes.right, settings.axes.up, settings.axes.front};
    bool known = true;
    for (std::size_t i = 0; i < 3; ++i) {
      const auto axis = static_cast<int>(axes[i]);
      if (axis < 0 || axis >= 6) { known = false; break; }
      c.axes[i] = (axis / 2 + 1) * (axis % 2 ? -1 : 1);
    }
    if (known) out.coordinates = c;
  }
  const auto vector = [](ufbx_vec3 v) -> transform::Vec3 { return {v.x,v.y,v.z}; };
  const auto quaternion = [](ufbx_quat q) -> transform::Quat { return {q.x,q.y,q.z,q.w}; };
  const auto evaluated = sdk_worlds(scene, stop);
  out.nodes.reserve(scene.nodes.count);
  for (const auto *node : scene.nodes) {
    cancelled(stop);
    transform::Node n;
    n.fileIndex = node->typed_id;
    if (node->parent) n.parent = node->parent->typed_id;
    const auto &t = node->local_transform;
    n.restLocal = transform::Trs{vector(t.translation), quaternion(t.rotation), vector(t.scale)};
    n.syntheticRoot = node->is_root;
    if (node->inherit_scale_node) n.inheritScaleParent = node->inherit_scale_node->typed_id;
    // The pinned reader's documented FBX defaults are part of its semantics:
    // missing pivot/offset/pre/post = zero, order = XYZ. Do not infer a
    // canonical meaning for coordinate conversion or synthetic scale helpers.
    if (evaluated.complete && !node->has_adjust_transform && !node->has_root_adjust_transform &&
        !node->is_scale_helper && !node->is_geometry_transform_helper &&
        node->rotation_order < UFBX_ROTATION_ORDER_SPHERIC) {
      transform::Evaluation e;
      e.rotationSpace = node->use_rotation_space;
      e.rotationOrder = static_cast<std::uint8_t>(node->use_rotation_space ? node->rotation_order : UFBX_ROTATION_ORDER_XYZ);
      e.inheritance = static_cast<std::uint8_t>(node->inherit_mode);
      const auto v = [&](const char *key) {
        const auto *p = ufbx_find_prop(&node->props,key);
        return p ? p->value_vec3 : ufbx_zero_vec3;
      };
      if (node->use_rotation_space) {
        e.preRotation = quaternion(ufbx_euler_to_quat(v("PreRotation"), UFBX_ROTATION_ORDER_XYZ));
        e.postRotation = quaternion(ufbx_euler_to_quat(v("PostRotation"), UFBX_ROTATION_ORDER_XYZ));
      }
      e.rotationPivot = vector(v("RotationPivot")); e.scalingPivot = vector(v("ScalingPivot"));
      e.rotationOffset = vector(v("RotationOffset")); e.scalingOffset = vector(v("ScalingOffset"));
      n.evaluation = e;
    }
    out.nodes.push_back(n);
  }
  out.fullNodeDomain = true;
  out.bindings.emplace();
  const auto matrix = [](const ufbx_matrix &m) {
    transform::Matrix a{};
    for (std::size_t c=0;c<4;++c) for (std::size_t r=0;r<3;++r) a[c*4+r]=m.cols[c].v[r];
    a[15]=1; return a;
  };
  std::size_t seenSkins=0;
  for (const auto *mesh : scene.meshes) {
    for (const auto *skin : mesh->skin_deformers) {
      ++seenSkins;
      if (skin->skinning_method != UFBX_SKINNING_METHOD_LINEAR || mesh->instances.count == 0) {
        out.bindings.reset(); return out;
      }
      for (const auto *instance : mesh->instances) {
        cancelled(stop);
        // A declared BindPose must independently contain the mesh instance
        // and every joint. Never substitute node_to_world/current pose.
        const ufbx_pose *witness=nullptr;
        for (const auto *pose : scene.poses) {
          cancelled(stop);
          if (!pose->is_bind_pose || !ufbx_get_bone_pose(pose,instance)) continue;
          bool complete=true;
          for (const auto *cluster : skin->clusters) {
            cancelled(stop);
            if (!cluster->bone_node || !ufbx_get_bone_pose(pose,cluster->bone_node)) {complete=false;break;}
          }
          if (!complete) continue;
          if (witness) {out.bindings.reset(); return out;} // Ambiguous bind epochs.
          witness=pose;
        }
        if (!witness || instance->geometry_transform_helper || instance->has_adjust_transform ||
            instance->has_root_adjust_transform) {out.bindings.reset();return out;}
        const auto *meshPose=ufbx_get_bone_pose(witness,instance);
        const auto geometryWorld=ufbx_matrix_mul(&meshPose->bone_to_world,&instance->geometry_to_node);
        for (const auto *cluster : skin->clusters) {
          cancelled(stop);
          if (out.bindings->size()+2 > transform::Policy{}.maxBindings) {out.bindings.reset();return out;}
          const auto *jointPose=ufbx_get_bone_pose(witness,cluster->bone_node);
          const auto geometryToBone=ufbx_matrix_mul(&cluster->mesh_node_to_bone,&instance->geometry_to_node);
          // Check BOTH independently stored joint witnesses against the mesh
          // BindPose. This detects conflicting cluster links and fallback data.
          out.bindings->push_back({cluster->bone_node->typed_id,matrix(jointPose->bone_to_world),matrix(geometryToBone),matrix(geometryWorld),instance->typed_id,0});
          out.bindings->push_back({cluster->bone_node->typed_id,matrix(cluster->bind_to_world),matrix(geometryToBone),matrix(geometryWorld),instance->typed_id,1});
        }
      }
    }
  }
  // Unattached deformers are not silently omitted from the complete domain.
  if (seenSkins != scene.skin_deformers.count) out.bindings.reset();
  return out;
}
FbxDocument FbxDocument::read(const std::filesystem::path &path,
                              const ReadOptions &options) {
  return read_impl(path, options, false);
}
FbxDocument FbxDocument::read_obj(const std::filesystem::path &path,
                                  const ReadOptions &options) {
  return read_impl(path, options, true);
}
FbxDocument FbxDocument::read_impl(const std::filesystem::path &path,
                                   const ReadOptions &options, bool obj) {
  cancelled(options.stop);
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f)
    throw Failure(ErrorCode::Io, "Cannot open FBX");
  const auto end = f.tellg();
  if (end < 0)
    throw Failure(ErrorCode::Io, "Cannot size FBX");
  const auto size = static_cast<std::uint64_t>(end);
  if (size > options.limits.input_bytes ||
      size > options.limits.decoded_bytes / 4)
    throw Failure(ErrorCode::ResourceLimit, "FBX input budget exceeded");
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  f.seekg(0);
  for (std::size_t off = 0; off < bytes.size();) {
    cancelled(options.stop);
    const auto n = std::min<std::size_t>(1024 * 1024, bytes.size() - off);
    if (!f.read(reinterpret_cast<char *>(bytes.data() + off),
                static_cast<std::streamsize>(n)))
      throw Failure(ErrorCode::InputChanged,
                    "FBX changed/truncated during read");
    off += n;
  }
  if (f.peek() != std::ifstream::traits_type::eof())
    throw Failure(ErrorCode::InputChanged, "FBX grew during read");
  auto stop = options.stop;
  ufbx_load_opts opts{};
  opts.file_format = obj ? UFBX_FILE_FORMAT_OBJ : UFBX_FILE_FORMAT_FBX;
  MaterialFiles materials;
  // The caller's OBJ path establishes authority, never the requested MTL path.
  materials.scope.input_roots = {path.parent_path()};
  materials.stop = stop;
  const auto filename = path.u8string();
  opts.filename = {reinterpret_cast<const char *>(filename.data()),
                   filename.size()};
  opts.load_external_files = obj;
  opts.ignore_missing_external_files = obj;
  if (obj) {
    opts.open_file_cb.fn = open_material;
    opts.open_file_cb.user = &materials;
  }
  opts.clean_skin_weights = false;
  opts.index_error_handling = UFBX_INDEX_ERROR_HANDLING_ABORT_LOADING;
  opts.node_depth_limit = options.limits.bones;
  const auto share =
      static_cast<std::size_t>((options.limits.decoded_bytes - size) / 3);
  if (!share)
    throw Failure(ErrorCode::ResourceLimit, "FBX memory budget is empty");
  opts.temp_allocator.memory_limit = share;
  opts.result_allocator.memory_limit = share;
  opts.progress_cb.fn = progress;
  opts.progress_cb.user = &stop;
  opts.progress_interval_hint = 65536;
  ufbx_error error{};
  auto impl = std::make_shared<Impl>();
  impl->limits = options.limits;
  impl->scene.reset(
      ufbx_load_memory(bytes.data(), bytes.size(), &opts, &error));
  if (!impl->scene)
    failed(error);
  const auto &s = *impl->scene;
  auto &d = impl->data;
  for (const auto &lease : materials.leases)
    lease.require_unchanged(lease.snapshot());
  d.resources = std::move(materials.resources);
  d.source.identity = {sha256(bytes, stop), "ufbx", "0.23.1", "scene"};
  d.source.coordinates.metres_per_unit = s.settings.unit_meters;
  d.source.coordinates.source_convention =
      obj ? "OBJ axes and units unspecified"
          : "FBX original axes; no target_axes or target_unit conversion";
  d.version = s.metadata.version;
  d.ascii = s.metadata.ascii;
  d.metres_per_unit = s.settings.unit_meters;
  d.fps = s.settings.frames_per_second;
  d.raw_axes = {s.settings.axes.right, s.settings.axes.up,
                s.settings.axes.front};
  if (ufbx_coordinate_axes_valid(s.settings.axes)) {
    constexpr std::array<Axis, 6> axes{Axis::PositiveX, Axis::NegativeX,
                                       Axis::PositiveY, Axis::NegativeY,
                                       Axis::PositiveZ, Axis::NegativeZ};
    const auto right = static_cast<unsigned>(s.settings.axes.right);
    const auto up = static_cast<unsigned>(s.settings.axes.up);
    const auto front = static_cast<unsigned>(s.settings.axes.front);
    d.source.coordinates.up = axes[up];
    d.source.coordinates.forward = axes[front ^ 1];
    std::array<double, 3> x{}, y{}, z{};
    x[right / 2] = (right & 1) ? -1 : 1;
    y[up / 2] = (up & 1) ? -1 : 1;
    z[front / 2] = (front & 1) ? -1 : 1;
    const auto determinant = (x[1] * y[2] - x[2] * y[1]) * z[0] +
                             (x[2] * y[0] - x[0] * y[2]) * z[1] +
                             (x[0] * y[1] - x[1] * y[0]) * z[2];
    d.source.coordinates.handedness =
        determinant > 0 ? Handedness::Right : Handedness::Left;
  }
  if (s.bones.count > options.limits.bones)
    throw Failure(ErrorCode::ResourceLimit, "FBX bone limit exceeded");
  std::uint64_t remaining = share;
  auto budget = [&](std::uint64_t count, std::uint64_t stride) {
    if (count > options.limits.elements ||
        (stride && count > remaining / stride))
      throw Failure(ErrorCode::ResourceLimit, "FBX snapshot budget exceeded");
    remaining -= count * stride;
  };
  std::map<const ufbx_material *, FbxMaterial> materialCache;
  const auto materialSnapshot =
      [&](const ufbx_material *material) -> const FbxMaterial & {
    if (const auto cached = materialCache.find(material);
        cached != materialCache.end())
      return cached->second;
    if (material->pbr.base_color.texture)
      budget(material->pbr.base_color.texture->content.size, 1);
    return materialCache.emplace(material, material_value(*material))
        .first->second;
  };
  budget(s.nodes.count, 2 * sizeof(FbxNode) + 512);
  const auto evaluated = sdk_worlds(s, stop);
  d.nodes.reserve(s.nodes.count);
  for (const auto *n : s.nodes) {
    cancelled(stop);
    FbxNode node;
    node.source = source(d.source, n->element);
    node.bone = n->bone != nullptr;
    node.implicit_scene_root = n->is_root;
    if (n->parent)
      node.parent = n->parent->typed_id;
    node.default_local = convert(effective_local(*n, evaluated.values));
    node.default_world = convert(evaluated.values[n->typed_id]);
    node.geometry_world = convert(ufbx_matrix_mul(&evaluated.values[n->typed_id], &n->geometry_to_node));
    budget(n->materials.count, sizeof(FbxMaterial) + 256);
    for (const auto *material : n->materials)
      node.materials.push_back(materialSnapshot(material));
    if (n->attrib) {
      if (const auto *lod = ufbx_as_lod_group(n->attrib)) {
        node.relative_lod_distances = lod->relative_distances;
        for (std::size_t i = 0;
             i < lod->lod_levels.count && i < n->children.count; ++i)
          node.lod_levels.push_back(
              {n->children[i]->typed_id, lod->lod_levels[i].distance,
               static_cast<std::uint32_t>(lod->lod_levels[i].display)});
      }
    }
    d.nodes.push_back(std::move(node));
  }
  budget(s.meshes.count, 2 * sizeof(FbxMesh) + 512);
  for (const auto *m : s.meshes) {
    cancelled(stop);
    FbxMesh out;
    auto &g = out.geometry;
    g.source = source(d.source, m->element);
    budget(m->num_vertices, 3 * sizeof(double));
    g.positions = {ScalarType::Float64, 3, m->num_vertices, {}};
    g.positions.bytes.reserve(m->num_vertices * 3 * sizeof(double));
    for (const auto v : m->vertices) {
      append(g.positions, v.x);
      append(g.positions, v.y);
      append(g.positions, v.z);
    }
    budget(m->num_faces, 2 * sizeof(std::uint64_t));
    budget(m->num_indices,
           2 * (sizeof(std::uint64_t) + sizeof(SourceVertexMapping)));
    for (std::size_t i = 0; i < m->faces.count; ++i) {
      const auto face = m->faces[i];
      g.face_sizes.push_back(face.num_indices);
      out.face_materials.push_back(m->face_material.count ? m->face_material[i]
                                                          : 0);
      for (std::uint32_t k = 0; k < face.num_indices; ++k) {
        const auto corner = face.index_begin + k;
        const auto point = m->vertex_indices[corner];
        g.corner_points.push_back(point);
        g.render_mapping.push_back({point, i, k, corner});
      }
      if (face.num_indices >= 3) {
        const auto capacity =
            static_cast<std::size_t>(face.num_indices - 2) * 3;
        budget(capacity, sizeof(std::uint32_t));
        std::vector<std::uint32_t> triangles(capacity);
        const auto count =
            ufbx_triangulate_face(triangles.data(), triangles.size(), m, face);
        out.triangle_corners.insert(
            out.triangle_corners.end(), triangles.begin(),
            triangles.begin() + static_cast<std::ptrdiff_t>(count) * 3);
      }
    }
    for (const auto *material : m->materials) {
      out.materials.push_back(materialSnapshot(material));
    }
    budget(m->uv_sets.count, 128);
    for (const auto &set : m->uv_sets) {
      budget(m->num_indices, 2 * sizeof(double));
      Buffer b{ScalarType::Float64, 2, m->num_indices, {}};
      b.bytes.reserve(m->num_indices * 2 * sizeof(double));
      for (std::size_t k = 0; k < m->num_indices; ++k) {
        const auto uv = ufbx_get_vertex_vec2(&set.vertex_uv, k);
        append(b, uv.x);
        append(b, uv.y);
      }
      auto name = "uv/" + std::to_string(g.corner_attributes.size()) + "/" +
                  str(set.name);
      g.corner_attributes.emplace(std::move(name), std::move(b));
    }
    if (m->vertex_normal.exists) {
      budget(m->num_indices, 3 * sizeof(double));
      Buffer b{ScalarType::Float64, 3, m->num_indices, {}};
      for (std::size_t k = 0; k < m->num_indices; ++k) {
        const auto v = ufbx_get_vertex_vec3(&m->vertex_normal, k);
        append(b, v.x);
        append(b, v.y);
        append(b, v.z);
      }
      g.corner_attributes.emplace("normal", std::move(b));
    }
    for (std::size_t i = 0; i < m->color_sets.count; ++i) {
      budget(m->num_indices, 4 * sizeof(double));
      Buffer b{ScalarType::Float64, 4, m->num_indices, {}};
      for (std::size_t k = 0; k < m->num_indices; ++k) {
        const auto v = ufbx_get_vertex_vec4(&m->color_sets[i].vertex_color, k);
        append(b, v.x);
        append(b, v.y);
        append(b, v.z);
        append(b, v.w);
      }
      g.corner_attributes.emplace("colour/" + std::to_string(i), std::move(b));
    }
    for (const auto *n : m->instances)
      out.instance_nodes.push_back(n->typed_id);
    for (const auto *skin : m->skin_deformers) {
      budget(skin->clusters.count, 2 * sizeof(FbxCluster) + 512);
      FbxSkin fs;
      fs.source = source(d.source, skin->element);
      fs.method = skin->skinning_method;
      for (const auto *cluster : skin->clusters) {
        cancelled(stop);
        if (!cluster->bone_node)
          throw Failure(ErrorCode::Unsupported, "FBX unbound cluster");
        FbxCluster c;
        c.source = source(d.source, cluster->element);
        c.bone_node = cluster->bone_node->typed_id;
        c.geometry_to_bone = convert(cluster->geometry_to_bone);
        c.bind_to_world = convert(cluster->bind_to_world);
        budget(cluster->vertices.count,
               2 * (sizeof(std::uint32_t) + sizeof(double)));
        c.points.assign(cluster->vertices.begin(), cluster->vertices.end());
        c.weights.assign(cluster->weights.begin(), cluster->weights.end());
        if (c.points.size() != c.weights.size())
          throw Failure(ErrorCode::InvalidArgument,
                        "FBX cluster weight length mismatch");
        for (auto p : c.points)
          if (p >= m->num_vertices)
            throw Failure(ErrorCode::InvalidArgument,
                          "FBX raw cluster point out of bounds");
        fs.clusters.push_back(std::move(c));
      }
      out.skins.push_back(std::move(fs));
    }
    d.meshes.push_back(std::move(out));
  }
  budget(s.anim_stacks.count, 2 * sizeof(FbxClip) + 512);
  for (const auto *clip : s.anim_stacks)
    d.clips.push_back({source(d.source, clip->element), clip->time_begin,
                       clip->time_end, d.fps});
  cancelled(stop);
  return FbxDocument{std::move(impl)};
}
MatrixPose FbxDocument::evaluate(std::size_t clip, double seconds,
                                 std::stop_token stop) const {
  cancelled(stop);
  const auto &s = *impl_->scene;
  if (clip >= s.anim_stacks.count || !std::isfinite(seconds) ||
      seconds < s.anim_stacks[clip]->time_begin ||
      seconds > s.anim_stacks[clip]->time_end)
    throw Failure(ErrorCode::InvalidArgument,
                  "FBX clip/time outside source range");
  ufbx_evaluate_opts opts{};
  opts.temp_allocator.memory_limit =
      static_cast<std::size_t>(impl_->limits.decoded_bytes / 4);
  opts.result_allocator.memory_limit = opts.temp_allocator.memory_limit;
  ufbx_error error{};
  std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> pose{
      ufbx_evaluate_scene(&s, s.anim_stacks[clip]->anim, seconds, &opts,
                          &error),
      ufbx_free_scene};
  if (!pose)
    failed(error);
  cancelled(stop);
  const auto evaluated = sdk_worlds(*pose, stop);
  MatrixPose result;
  result.local.reserve(s.nodes.count);
  result.world.reserve(s.nodes.count);
  for (const auto *n : pose->nodes) {
    cancelled(stop);
    result.local.push_back(convert(effective_local(*n, evaluated.values)));
    result.world.push_back(convert(evaluated.values[n->typed_id]));
  }
  return result;
}
} // namespace formaview::skeletal
