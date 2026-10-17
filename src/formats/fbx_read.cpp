#include "formaview/fbx.hpp"
#include <cmath>
#include <cstring>
#include <fstream>
#include <ufbx.h>

namespace formaview::skeletal {
namespace {
void cancelled(std::stop_token stop) {
    if (stop.stop_requested())
        throw Failure(ErrorCode::Cancelled, "FBX operation cancelled");
}
Matrix4 convert(const ufbx_matrix &m) {
    return {m.m00, m.m01, m.m02, m.m03, m.m10, m.m11, m.m12, m.m13, m.m20, m.m21, m.m22, m.m23, 0, 0, 0, 1};
}
std::string str(ufbx_string s) { return {s.data, s.length}; }
SourceMetadata source(const SourceMetadata &root, const ufbx_element &e) {
    auto s = root;
    s.identity.object_key = "element/" + std::to_string(e.element_id);
    s.original_name = str(e.name);
    return s;
}
ufbx_progress_result progress(void *user, const ufbx_progress *) {
    return static_cast<std::stop_token *>(user)->stop_requested() ? UFBX_PROGRESS_CANCEL
                                                                  : UFBX_PROGRESS_CONTINUE;
}
[[noreturn]] void failed(const ufbx_error &e) {
    auto code = ErrorCode::InvalidArgument;
    if (e.type == UFBX_ERROR_CANCELLED)
        code = ErrorCode::Cancelled;
    if (e.type == UFBX_ERROR_MEMORY_LIMIT || e.type == UFBX_ERROR_ALLOCATION_LIMIT)
        code = ErrorCode::ResourceLimit;
    throw Failure(code, "ufbx: " + str(e.description));
}
template <class T> void append(Buffer &b, const T &v) {
    const auto old = b.bytes.size();
    b.bytes.resize(old + sizeof(T));
    std::memcpy(b.bytes.data() + old, &v, sizeof(T));
}
} // namespace
struct FbxDocument::Impl {
    std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> scene{nullptr, ufbx_free_scene};
    FbxData data;
    Limits limits;
};
FbxDocument::FbxDocument(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
const FbxData &FbxDocument::data() const noexcept { return impl_->data; }
FbxDocument FbxDocument::read(const std::filesystem::path &path, const ReadOptions &options) {
    cancelled(options.stop);
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        throw Failure(ErrorCode::Io, "Cannot open FBX");
    const auto end = f.tellg();
    if (end < 0)
        throw Failure(ErrorCode::Io, "Cannot size FBX");
    const auto size = static_cast<std::uint64_t>(end);
    if (size > options.limits.input_bytes || size > options.limits.decoded_bytes / 4)
        throw Failure(ErrorCode::ResourceLimit, "FBX input budget exceeded");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    f.seekg(0);
    for (std::size_t off = 0; off < bytes.size();) {
        cancelled(options.stop);
        const auto n = std::min<std::size_t>(1024 * 1024, bytes.size() - off);
        if (!f.read(reinterpret_cast<char *>(bytes.data() + off), static_cast<std::streamsize>(n)))
            throw Failure(ErrorCode::InputChanged, "FBX changed/truncated during read");
        off += n;
    }
    if (f.peek() != std::ifstream::traits_type::eof())
        throw Failure(ErrorCode::InputChanged, "FBX grew during read");
    auto stop = options.stop;
    ufbx_load_opts opts{};
    opts.file_format = UFBX_FILE_FORMAT_FBX;
    opts.load_external_files = false;
    opts.clean_skin_weights = false;
    opts.index_error_handling = UFBX_INDEX_ERROR_HANDLING_ABORT_LOADING;
    opts.node_depth_limit = options.limits.bones;
    const auto share = static_cast<std::size_t>((options.limits.decoded_bytes - size) / 3);
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
    impl->scene.reset(ufbx_load_memory(bytes.data(), bytes.size(), &opts, &error));
    if (!impl->scene)
        failed(error);
    const auto &s = *impl->scene;
    auto &d = impl->data;
    d.source.identity = {sha256(bytes, stop), "ufbx", "0.23.1", "scene"};
    d.source.coordinates.metres_per_unit = s.settings.unit_meters;
    d.source.coordinates.source_convention = "FBX original axes; no target_axes or target_unit conversion";
    d.version = s.metadata.version;
    d.ascii = s.metadata.ascii;
    d.metres_per_unit = s.settings.unit_meters;
    d.fps = s.settings.frames_per_second;
    d.raw_axes = {s.settings.axes.right, s.settings.axes.up, s.settings.axes.front};
    if (ufbx_coordinate_axes_valid(s.settings.axes)) {
        constexpr std::array<Axis, 6> axes{Axis::PositiveX, Axis::NegativeX, Axis::PositiveY,
                                           Axis::NegativeY, Axis::PositiveZ, Axis::NegativeZ};
        const auto right = static_cast<unsigned>(s.settings.axes.right);
        const auto up = static_cast<unsigned>(s.settings.axes.up);
        const auto front = static_cast<unsigned>(s.settings.axes.front);
        d.source.coordinates.up = axes[up];
        d.source.coordinates.forward = axes[front ^ 1];
        std::array<double, 3> x{}, y{}, z{};
        x[right / 2] = (right & 1) ? -1 : 1;
        y[up / 2] = (up & 1) ? -1 : 1;
        z[front / 2] = (front & 1) ? -1 : 1;
        const auto determinant = (x[1] * y[2] - x[2] * y[1]) * z[0] + (x[2] * y[0] - x[0] * y[2]) * z[1] +
                                 (x[0] * y[1] - x[1] * y[0]) * z[2];
        d.source.coordinates.handedness = determinant > 0 ? Handedness::Right : Handedness::Left;
    }
    if (s.bones.count > options.limits.bones)
        throw Failure(ErrorCode::ResourceLimit, "FBX bone limit exceeded");
    std::uint64_t remaining = share;
    auto budget = [&](std::uint64_t count, std::uint64_t stride) {
        if (count > options.limits.elements || (stride && count > remaining / stride))
            throw Failure(ErrorCode::ResourceLimit, "FBX snapshot budget exceeded");
        remaining -= count * stride;
    };
    budget(s.nodes.count, 2 * sizeof(FbxNode) + 512);
    d.nodes.reserve(s.nodes.count);
    for (const auto *n : s.nodes) {
        cancelled(stop);
        FbxNode node;
        node.source = source(d.source, n->element);
        node.bone = n->bone != nullptr;
        if (n->parent)
            node.parent = n->parent->typed_id;
        node.default_local = convert(n->node_to_parent);
        node.default_world = convert(n->node_to_world);
        node.geometry_world = convert(n->geometry_to_world);
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
        budget(m->num_indices, 2 * (sizeof(std::uint64_t) + sizeof(SourceVertexMapping)));
        for (std::size_t i = 0; i < m->faces.count; ++i) {
            const auto face = m->faces[i];
            g.face_sizes.push_back(face.num_indices);
            out.face_materials.push_back(m->face_material.count ? m->face_material[i] : 0);
            for (std::uint32_t k = 0; k < face.num_indices; ++k) {
                const auto corner = face.index_begin + k;
                const auto point = m->vertex_indices[corner];
                g.corner_points.push_back(point);
                g.render_mapping.push_back({point, i, k, corner});
            }
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
            auto name = "uv/" + std::to_string(g.corner_attributes.size()) + "/" + str(set.name);
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
                budget(cluster->vertices.count, 2 * (sizeof(std::uint32_t) + sizeof(double)));
                c.points.assign(cluster->vertices.begin(), cluster->vertices.end());
                c.weights.assign(cluster->weights.begin(), cluster->weights.end());
                if (c.points.size() != c.weights.size())
                    throw Failure(ErrorCode::InvalidArgument, "FBX cluster weight length mismatch");
                for (auto p : c.points)
                    if (p >= m->num_vertices)
                        throw Failure(ErrorCode::InvalidArgument, "FBX raw cluster point out of bounds");
                fs.clusters.push_back(std::move(c));
            }
            out.skins.push_back(std::move(fs));
        }
        d.meshes.push_back(std::move(out));
    }
    budget(s.anim_stacks.count, 2 * sizeof(FbxClip) + 512);
    for (const auto *clip : s.anim_stacks)
        d.clips.push_back({source(d.source, clip->element), clip->time_begin, clip->time_end, d.fps});
    cancelled(stop);
    return FbxDocument{std::move(impl)};
}
MatrixPose FbxDocument::evaluate(std::size_t clip, double seconds, std::stop_token stop) const {
    cancelled(stop);
    const auto &s = *impl_->scene;
    if (clip >= s.anim_stacks.count || !std::isfinite(seconds) || seconds < s.anim_stacks[clip]->time_begin ||
        seconds > s.anim_stacks[clip]->time_end)
        throw Failure(ErrorCode::InvalidArgument, "FBX clip/time outside source range");
    ufbx_evaluate_opts opts{};
    opts.temp_allocator.memory_limit = static_cast<std::size_t>(impl_->limits.decoded_bytes / 4);
    opts.result_allocator.memory_limit = opts.temp_allocator.memory_limit;
    ufbx_error error{};
    std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> pose{
        ufbx_evaluate_scene(&s, s.anim_stacks[clip]->anim, seconds, &opts, &error), ufbx_free_scene};
    if (!pose)
        failed(error);
    cancelled(stop);
    MatrixPose result;
    result.local.reserve(s.nodes.count);
    result.world.reserve(s.nodes.count);
    for (const auto *n : pose->nodes) {
        cancelled(stop);
        result.local.push_back(convert(n->node_to_parent));
        result.world.push_back(convert(n->node_to_world));
    }
    return result;
}
} // namespace formaview::skeletal
