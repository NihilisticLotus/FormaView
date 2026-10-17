#include "formaview/skeletal.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>

namespace formaview::skeletal {
namespace {
constexpr Matrix4 identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
void check_stop(std::stop_token stop) {
    if (stop.stop_requested())
        throw Failure(ErrorCode::Cancelled, "Actor read/evaluation cancelled");
}
void require(bool condition, std::string message) {
    if (!condition)
        throw Failure(ErrorCode::InvalidArgument, std::move(message));
}
Quat normalized(Quat q) {
    const auto n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    require(std::isfinite(n) && n > 1e-12, "Invalid zero/nonfinite quaternion");
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}
Quat slerp(Quat a, Quat b, double t) {
    a = normalized(a);
    b = normalized(b);
    auto d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) {
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    auto u = 1 - t, v = t;
    if (d < 0.9995) {
        const auto angle = std::acos(std::clamp(d, -1.0, 1.0));
        u = std::sin((1 - t) * angle) / std::sin(angle);
        v = std::sin(t * angle) / std::sin(angle);
    }
    return normalized({u * a.x + v * b.x, u * a.y + v * b.y, u * a.z + v * b.z, u * a.w + v * b.w});
}
Vec3 lerp(Vec3 a, Vec3 b, double t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}
Transform decoded(Vec3 t, Quat q, bool root, Vec3 scale = {1, 1, 1}) {
    // ActorX stores child orientation conjugated; root orientation is direct.
    if (!root)
        q = {-q.x, -q.y, -q.z, q.w};
    return {t, normalized(q), scale};
}
void validate_hierarchy(const ActorAsset &a, std::stop_token stop) {
    std::vector<std::uint8_t> state(a.bones.size());
    std::vector<std::size_t> chain;
    for (std::size_t i = 0; i < a.bones.size(); ++i) {
        check_stop(stop);
        chain.clear();
        auto at = i;
        while (state[at] == 0) {
            state[at] = 1;
            chain.push_back(at);
            if (!a.bones[at].parent)
                break;
            at = *a.bones[at].parent;
            require(at < a.bones.size(), "Bone parent out of bounds");
        }
        if (!chain.empty() && a.bones[chain.back()].parent)
            require(state[at] == 2, "Bone hierarchy cycle");
        for (auto j : chain)
            state[j] = 2;
    }
}
void worlds(const ActorAsset &a, Pose &p, std::stop_token stop,
            std::span<const std::optional<std::uint32_t>> parents = {}) {
    const auto parent = [&](std::size_t j) { return parents.empty() ? a.bones[j].parent : parents[j]; };
    p.world.resize(a.bones.size());
    std::vector<std::uint8_t> state(a.bones.size());
    std::vector<std::size_t> chain;
    chain.reserve(a.bones.size());
    for (std::size_t i = 0; i < a.bones.size(); ++i) {
        check_stop(stop);
        chain.clear();
        auto at = i;
        while (state[at] == 0) {
            state[at] = 1;
            chain.push_back(at);
            if (!parent(at))
                break;
            at = *parent(at);
            require(at < a.bones.size(), "Bone parent out of bounds");
        }
        if (!chain.empty() && parent(chain.back()))
            require(state[at] == 2, "Bone hierarchy cycle");
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const auto j = *it;
            const auto local = matrix(p.local[j]);
            p.world[j] = parent(j) ? multiply(p.world[*parent(j)], local) : local;
            require(std::all_of(p.world[j].begin(), p.world[j].end(),
                                [](double value) { return std::isfinite(value); }),
                    "Bone world transform overflow or nonfinite value");
            state[j] = 2;
        }
    }
}
class Reader {
  public:
    std::span<const std::byte> bytes;
    const ReadOptions &options;
    std::uint64_t reserved{};
    void budget(std::uint64_t count, std::uint64_t size, bool elements = true) {
        if ((elements && count > options.limits.elements) ||
            (size && count > (options.limits.decoded_bytes - reserved) / size))
            throw Failure(ErrorCode::ResourceLimit, "Actor decoded resource budget exceeded");
        reserved += count * size;
    }
    std::uint32_t u32(std::size_t off) const {
        require(off <= bytes.size() && bytes.size() - off >= 4, "Truncated scalar");
        std::uint32_t v{};
        for (unsigned i = 0; i < 4; ++i)
            v |= std::to_integer<std::uint32_t>(bytes[off + i]) << (i * 8);
        return v;
    }
    std::uint16_t u16(std::size_t off) const {
        return static_cast<std::uint16_t>(u8(off) | (static_cast<unsigned>(u8(off + 1)) << 8));
    }
    std::uint8_t u8(std::size_t off) const {
        require(off < bytes.size(), "Truncated byte");
        return std::to_integer<std::uint8_t>(bytes[off]);
    }
    std::int32_t i32(std::size_t off) const { return std::bit_cast<std::int32_t>(u32(off)); }
    double f32(std::size_t off) const {
        const auto v = std::bit_cast<float>(u32(off));
        require(std::isfinite(v), "Nonfinite actor scalar");
        return v;
    }
    Vec3 vec(std::size_t off) const { return {f32(off), f32(off + 4), f32(off + 8)}; }
    Quat quat(std::size_t off, bool allow_reference_placeholder = false) const {
        Quat q{f32(off), f32(off + 4), f32(off + 8), f32(off + 12)};
        if (allow_reference_placeholder && q.x == 0 && q.y == 0 && q.z == 0 && q.w == 0)
            return q;
        (void)normalized(q);
        return q;
    }
    std::string name(std::size_t off, std::size_t n) const {
        require(off <= bytes.size() && n <= bytes.size() - off, "Truncated name");
        std::string result;
        for (std::size_t i = 0; i < n && u8(off + i); ++i)
            result.push_back(static_cast<char>(u8(off + i)));
        return result;
    }
};
SourceMetadata metadata(const SourceMetadata &root, std::string key, std::string name) {
    auto result = root;
    result.identity.object_key = std::move(key);
    result.original_name = std::move(name);
    return result;
}
} // namespace
Failure::Failure(ErrorCode code, std::string message)
    : std::runtime_error(message), error{code, std::move(message), {}, false} {}
Matrix4 multiply(const Matrix4 &a, const Matrix4 &b) {
    Matrix4 c{};
    for (std::size_t i = 0; i < 4; ++i)
        for (std::size_t j = 0; j < 4; ++j)
            for (std::size_t k = 0; k < 4; ++k)
                c[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
    return c;
}
Matrix4 matrix(const Transform &t) {
    const auto q = normalized(t.rotation);
    const auto x = q.x, y = q.y, z = q.z, w = q.w;
    return {(1 - 2 * y * y - 2 * z * z) * t.scale.x,
            (2 * x * y - 2 * z * w) * t.scale.y,
            (2 * x * z + 2 * y * w) * t.scale.z,
            t.translation.x,
            (2 * x * y + 2 * z * w) * t.scale.x,
            (1 - 2 * x * x - 2 * z * z) * t.scale.y,
            (2 * y * z - 2 * x * w) * t.scale.z,
            t.translation.y,
            (2 * x * z - 2 * y * w) * t.scale.x,
            (2 * y * z + 2 * x * w) * t.scale.y,
            (1 - 2 * x * x - 2 * y * y) * t.scale.z,
            t.translation.z,
            0,
            0,
            0,
            1};
}
Matrix4 inverse(const Matrix4 &value) {
    auto a = value;
    auto b = identity;
    for (std::size_t col = 0; col < 4; ++col) {
        auto pivot = col;
        for (auto row = col + 1; row < 4; ++row)
            if (std::abs(a[row * 4 + col]) > std::abs(a[pivot * 4 + col]))
                pivot = row;
        require(std::isfinite(a[pivot * 4 + col]) && std::abs(a[pivot * 4 + col]) > 1e-14, "Singular matrix");
        for (std::size_t j = 0; j < 4; ++j) {
            std::swap(a[col * 4 + j], a[pivot * 4 + j]);
            std::swap(b[col * 4 + j], b[pivot * 4 + j]);
        }
        const auto d = a[col * 4 + col];
        for (std::size_t j = 0; j < 4; ++j) {
            a[col * 4 + j] /= d;
            b[col * 4 + j] /= d;
        }
        for (std::size_t row = 0; row < 4; ++row)
            if (row != col) {
                const auto f = a[row * 4 + col];
                for (std::size_t j = 0; j < 4; ++j) {
                    a[row * 4 + j] -= f * a[col * 4 + j];
                    b[row * 4 + j] -= f * b[col * 4 + j];
                }
            }
    }
    return b;
}
ActorAsset read_actor(std::span<const std::byte> bytes, const ReadOptions &options) {
    check_stop(options.stop);
    if (bytes.size() > options.limits.input_bytes)
        throw Failure(ErrorCode::ResourceLimit, "Actor input budget exceeded");
    Reader r{bytes, options};
    r.budget(bytes.size(), 1, false);
    ActorAsset a;
    a.source_bytes.assign(bytes.begin(), bytes.end());
    a.source.identity = {sha256(bytes, options.stop), "actorx", "1", "asset"};
    a.source.coordinates = options.coordinates;
    a.warnings.push_back("ActorX does not declare axes/units; convention is supplied by caller and must be "
                         "verified against provenance");
    std::set<std::string> seen;
    for (std::size_t off = 0; off < bytes.size();) {
        check_stop(options.stop);
        require(bytes.size() - off >= 32, "Truncated ActorX chunk header");
        Chunk c{r.name(off, 20), r.i32(off + 20), r.i32(off + 24), r.i32(off + 28), off + 32};
        off += 32;
        require(c.stride >= 0 && c.count >= 0, "Negative chunk stride/count");
        const auto count = static_cast<std::uint64_t>(c.count), size = static_cast<std::uint64_t>(c.stride);
        require(size == 0 ? count == 0 : count <= (bytes.size() - off) / size,
                "Truncated ActorX chunk payload");
        if (a.chunks.size() >= options.limits.chunks)
            throw Failure(ErrorCode::ResourceLimit, "Actor chunk limit exceeded");
        r.budget(1, sizeof(Chunk) + 64);
        a.chunks.push_back(c);
        if (a.chunks.size() == 1) {
            require(c.name == "ACTRHEAD" || c.name == "ANIMHEAD", "Missing ActorX signature");
            a.animation = c.name == "ANIMHEAD";
        }
        if (c.name == "ACTRHEAD" || c.name == "ANIMHEAD") {
            require(size == 0 && count == 0, "Invalid ActorX signature payload");
            require((c.name == "ANIMHEAD") == a.animation, "Mixed ActorX signatures");
            off += count * size;
            continue;
        }
        require(seen.insert(c.name).second, "Duplicate ActorX data chunk: " + c.name);
        off += count * size;
    }
    require(!a.chunks.empty(), "Empty ActorX file");
    auto chunk = [&](std::string_view name, std::uint64_t stride, bool mandatory = true) -> const Chunk * {
        const auto it =
            std::find_if(a.chunks.begin(), a.chunks.end(), [&](const auto &c) { return c.name == name; });
        if (it == a.chunks.end()) {
            require(!mandatory, "Missing chunk: " + std::string(name));
            return nullptr;
        }
        require(static_cast<std::uint64_t>(it->stride) == stride, "Unsupported chunk stride: " + it->name);
        return &*it;
    };
    auto each = [&](const Chunk *c, std::uint64_t storage, auto fn) {
        // Account for vector capacity growth before any allocation, including malformed
        // counts. Do not reserve from untrusted counts ahead of this admission check.
        if (!c)
            return;
        r.budget(static_cast<std::uint64_t>(c->count), storage * 2);
        for (std::int32_t i = 0; i < c->count; ++i) {
            if ((i & 1023) == 0)
                check_stop(options.stop);
            fn(static_cast<std::size_t>(c->offset) +
               static_cast<std::size_t>(i) * static_cast<std::size_t>(c->stride));
        }
    };
    const auto bones = chunk(a.animation ? "BONENAMES" : "REFSKELT", 120);
    if (static_cast<std::uint32_t>(bones->count) > options.limits.bones)
        throw Failure(ErrorCode::ResourceLimit, "Bone limit exceeded");
    each(bones, sizeof(Bone) + 512, [&](auto o) {
        Bone b;
        b.source = metadata(a.source, "bone/" + std::to_string(a.bones.size()), r.name(o, 64));
        b.flags = r.i32(o + 64);
        b.child_count = r.i32(o + 68);
        b.source_parent = r.i32(o + 72);
        const auto index = a.bones.size();
        if (!(b.source_parent == -1 || (index == 0 && b.source_parent == 0))) {
            require(b.source_parent >= 0 && b.source_parent < bones->count &&
                        static_cast<std::size_t>(b.source_parent) != index,
                    "Invalid bone parent");
            b.parent = static_cast<std::uint32_t>(b.source_parent);
        }
        b.raw_rotation = r.quat(o + 76, a.animation);
        b.raw_translation = r.vec(o + 92);
        b.raw_length = r.f32(o + 104);
        b.raw_size = r.vec(o + 108);
        const auto &q = b.raw_rotation;
        b.default_local_valid = q.x != 0 || q.y != 0 || q.z != 0 || q.w != 0;
        b.default_local = b.default_local_valid ? decoded(b.raw_translation, q, !b.parent)
                                                : Transform{b.raw_translation, q};
        a.bones.push_back(std::move(b));
    });
    validate_hierarchy(a, options.stop); // PSA placeholders cannot be used to validate default matrices.
    const bool missing_defaults =
        std::any_of(a.bones.begin(), a.bones.end(), [](const auto &b) { return !b.default_local_valid; });
    if (missing_defaults)
        a.warnings.push_back("PSA BONENAMES has zero reference placeholders; keys retained, no default pose. "
                             "Supply an explicit reference PSK for reference-dependent operations");
    else if (a.animation)
        (void)default_pose(a, options.stop);
    std::vector<std::uint32_t> child_counts(a.bones.size());
    for (const auto &b : a.bones)
        if (b.parent)
            ++child_counts[*b.parent];
    for (std::size_t j = 0; j < a.bones.size(); ++j)
        if (a.bones[j].child_count != static_cast<std::int32_t>(child_counts[j])) {
            a.warnings.push_back("Source child counts disagree with parent indices; hierarchy provenance "
                                 "requires verification");
            break;
        }
    if (!a.animation) {
        const auto defaults = default_pose(a, options.stop);
        for (std::size_t i = 0; i < a.bones.size(); ++i)
            a.bones[i].bind_world = defaults.world[i];
        a.mesh.emplace();
        auto &m = *a.mesh;
        const auto points = chunk("PNTS0000", 12);
        each(points, sizeof(Vec3) + sizeof(std::vector<std::uint32_t>),
             [&](auto o) { m.points.push_back(r.vec(o)); });
        m.point_weights.resize(m.points.size());
        const auto wedges = chunk("VTXW0000", 16);
        each(wedges, sizeof(Wedge), [&](auto o) {
            const auto p = options.wedge_index == WedgeIndex::Legacy16 ? r.u16(o) : r.u32(o);
            require(p < m.points.size(), "Wedge source point out of bounds");
            m.wedges.push_back({p, r.u8(o + 12), {r.f32(o + 4), r.f32(o + 8)}});
        });
        require(!(seen.contains("FACE0000") && seen.contains("FACE3200")),
                "Ambiguous simultaneous face blocks");
        const auto wide = seen.contains("FACE3200");
        const auto faces = chunk(wide ? "FACE3200" : "FACE0000", wide ? 18 : 12);
        each(faces, sizeof(Face) + 3 * sizeof(SourceVertexMapping), [&](auto o) {
            Face f;
            for (std::size_t k = 0; k < 3; ++k) {
                f.wedges[k] = wide ? r.u32(o + 4 * k) : r.u16(o + 2 * k);
                require(f.wedges[k] < m.wedges.size(), "Face wedge out of bounds");
            }
            const auto d = wide ? 12U : 6U;
            f.material = r.u8(o + d);
            f.auxiliary_material = r.u8(o + d + 1);
            f.smoothing = r.u32(o + d + 2);
            for (std::size_t k = 0; k < 3; ++k)
                m.render_mapping.push_back({m.wedges[f.wedges[k]].point, m.faces.size(), k, f.wedges[k]});
            m.faces.push_back(f);
        });
        each(chunk("MATT0000", 88), sizeof(Material) + 64, [&](auto o) {
            Material mat;
            mat.name = r.name(o, 64);
            for (std::size_t k = 0; k < 6; ++k)
                mat.properties[k] = r.i32(o + 64 + 4 * k);
            m.materials.push_back(std::move(mat));
        });
        for (const auto &f : m.faces)
            require(f.material < m.materials.size(), "Face material out of bounds");
        for (const auto &w : m.wedges)
            require(w.material < m.materials.size(), "Wedge material out of bounds");
        each(chunk("RAWWEIGHTS", 12), sizeof(Weight) + 2 * sizeof(std::uint32_t), [&](auto o) {
            const auto p = r.i32(o + 4), b = r.i32(o + 8);
            require(p >= 0 && static_cast<std::size_t>(p) < m.points.size() && b >= 0 &&
                        static_cast<std::size_t>(b) < a.bones.size(),
                    "Skin weight index out of bounds");
            m.point_weights[static_cast<std::size_t>(p)].push_back(
                static_cast<std::uint32_t>(m.weights.size()));
            m.weights.push_back({static_cast<std::uint32_t>(p), static_cast<std::uint32_t>(b), r.f32(o)});
        });
        each(chunk("VTXNORMS", 12, false), sizeof(Vec3), [&](auto o) { m.normals.push_back(r.vec(o)); });
        require(m.normals.empty() || m.normals.size() == m.points.size(),
                "Normal source point count mismatch");
        each(chunk("VERTEXCOLOR", 4, false), 4,
             [&](auto o) { m.colours.push_back({r.u8(o), r.u8(o + 1), r.u8(o + 2), r.u8(o + 3)}); });
        require(m.colours.empty() || m.colours.size() == m.wedges.size(), "Colour wedge count mismatch");
        for (const auto &c : a.chunks)
            if (c.name.starts_with("EXTRAUV")) {
                require(c.stride == 8 && static_cast<std::size_t>(c.count) == m.wedges.size(),
                        "Extra UV wedge count/stride mismatch");
                auto &uv = m.extra_uvs[c.name];
                each(&c, sizeof(Vec2), [&](auto o) { uv.push_back({r.f32(o), r.f32(o + 4)}); });
            }
    } else {
        require(!a.bones.empty(), "Animation without bones");
        each(chunk("ANIMKEYS", 32), sizeof(Key),
             [&](auto o) { a.keys.push_back({r.vec(o), r.quat(o + 12), r.f32(o + 28)}); });
        each(chunk("SCALEKEYS", 16, false), sizeof(ScaleKey),
             [&](auto o) { a.scale_keys.push_back({r.vec(o), r.f32(o + 12)}); });
        require(a.scale_keys.empty() || a.scale_keys.size() == a.keys.size(), "Scale key count mismatch");
        const auto info = chunk("ANIMINFO", 168);
        std::uint64_t timing_checks{};
        if (static_cast<std::uint32_t>(info->count) > options.limits.clips)
            throw Failure(ErrorCode::ResourceLimit, "Clip limit exceeded");
        each(info, sizeof(Sequence) + 512, [&](auto o) {
            Sequence s;
            s.source = metadata(a.source, "clip/" + std::to_string(a.sequences.size()), r.name(o, 64));
            s.group = r.name(o + 64, 64);
            s.bone_count = r.i32(o + 128);
            s.root_include = r.i32(o + 132);
            s.compression = r.i32(o + 136);
            s.key_quota = r.i32(o + 140);
            s.key_reduction = r.f32(o + 144);
            s.track_time = r.f32(o + 148);
            s.fps = r.f32(o + 152);
            s.start_bone = r.i32(o + 156);
            s.first_raw_frame = r.i32(o + 160);
            s.frame_count = r.i32(o + 164);
            require(s.bone_count == static_cast<std::int32_t>(a.bones.size()) && s.start_bone == 0,
                    "Partial bone tracks are not supported");
            require(s.frame_count > 0 && s.first_raw_frame >= 0 && s.fps > 0,
                    "Invalid PSA frame range/rate (no automatic legacy repair)");
            const auto end =
                (static_cast<std::uint64_t>(s.first_raw_frame) + static_cast<std::uint64_t>(s.frame_count)) *
                a.bones.size();
            require(end <= a.keys.size(), "PSA key range outside ANIMKEYS (no automatic CUE4Parse repair)");
            s.time.source_end_seconds = static_cast<double>(s.frame_count) / s.fps;
            s.time.end_boundary = EndBoundary::Exclusive;
            // Float32 rates are represented exactly as a rational with a power-of-two denominator.
            int exponent{};
            const auto mantissa = std::frexp(s.fps, &exponent);
            require(exponent >= -30 && exponent <= 30, "PSA frame rate outside supported range");
            auto numerator = static_cast<std::int64_t>(std::ldexp(mantissa, 24));
            auto denominator = std::int64_t{1};
            if (exponent <= 24)
                denominator <<= (24 - exponent);
            else
                numerator <<= (exponent - 24);
            s.time.frame_rate = Rational{numerator, denominator};
            const auto key_begin = static_cast<std::uint64_t>(s.first_raw_frame) * a.bones.size();
            if (end - key_begin > options.limits.elements - timing_checks)
                throw Failure(ErrorCode::ResourceLimit, "PSA clip timing inspection work budget exceeded");
            timing_checks += end - key_begin;
            const auto delta = a.keys[static_cast<std::size_t>(key_begin)].delta_time;
            for (auto k = key_begin; k < end; ++k) {
                if ((k & 1023) == 0)
                    check_stop(options.stop);
                if (std::abs(a.keys[static_cast<std::size_t>(k)].delta_time - delta) >
                    1e-6 * std::max(1.0, std::abs(delta)))
                    s.uniform_key_deltas = false;
                if (!a.scale_keys.empty() && std::abs(a.scale_keys[static_cast<std::size_t>(k)].delta_time -
                                                      delta) > 1e-6 * std::max(1.0, std::abs(delta)))
                    s.uniform_key_deltas = false;
            }
            if (!s.uniform_key_deltas)
                a.warnings.push_back("Nonuniform PSA raw key timing retained; uniform-fps evaluator is "
                                     "unavailable for this clip");
            a.sequences.push_back(std::move(s));
        });
    }
    const std::set<std::string> known{"ACTRHEAD", "ANIMHEAD",    "BONENAMES", "REFSKELT", "PNTS0000",
                                      "VTXW0000", "FACE0000",    "FACE3200",  "MATT0000", "RAWWEIGHTS",
                                      "VTXNORMS", "VERTEXCOLOR", "ANIMINFO",  "ANIMKEYS", "SCALEKEYS"};
    for (const auto &c : a.chunks)
        if (!known.contains(c.name) && !c.name.starts_with("EXTRAUV"))
            a.warnings.push_back("Uninterpreted chunk retained verbatim: " + c.name);
    check_stop(options.stop);
    return a;
}
ActorAsset read_actor(const std::filesystem::path &path, const ReadOptions &options) {
    check_stop(options.stop);
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw Failure(ErrorCode::Io, "Cannot open ActorX input");
    const auto end = file.tellg();
    if (end < 0)
        throw Failure(ErrorCode::Io, "Cannot size ActorX input");
    const auto size = static_cast<std::uint64_t>(end);
    if (size > options.limits.input_bytes || size > options.limits.decoded_bytes / 2)
        throw Failure(ErrorCode::ResourceLimit, "Actor input staging budget exceeded");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    for (std::size_t off = 0; off < bytes.size();) {
        check_stop(options.stop);
        const auto n = std::min<std::size_t>(1024 * 1024, bytes.size() - off);
        if (!file.read(reinterpret_cast<char *>(bytes.data() + off), static_cast<std::streamsize>(n)))
            throw Failure(ErrorCode::Io, "Actor input changed/truncated during read");
        off += n;
    }
    if (file.peek() != std::ifstream::traits_type::eof())
        throw Failure(ErrorCode::InputChanged, "Actor input grew during read");
    auto adjusted = options;
    adjusted.limits.decoded_bytes -= size;
    return read_actor(bytes, adjusted);
}
Pose default_pose(const ActorAsset &a, std::stop_token stop) {
    Pose p;
    p.local.reserve(a.bones.size());
    for (const auto &b : a.bones) {
        if (!b.default_local_valid)
            throw Failure(ErrorCode::Unsupported,
                          "PSA reference records are placeholders; an explicit PSK reference is required");
        p.local.push_back(b.default_local);
    }
    worlds(a, p, stop);
    return p;
}
namespace {
Pose evaluate_impl(const ActorAsset &a, std::size_t clip, double seconds, std::stop_token stop,
                   const Pairing *binding = nullptr, const PsaConfig *config = nullptr) {
    check_stop(stop);
    require(clip < a.sequences.size() && std::isfinite(seconds), "Invalid clip/time");
    const auto &s = a.sequences[clip];
    if (!s.uniform_key_deltas)
        throw Failure(ErrorCode::Unsupported,
                      "Nonuniform PSA raw key timing requires a verified timing interpretation");
    require(seconds >= s.time.source_start_seconds && seconds <= s.time.source_end_seconds,
            "Time outside clip");
    const auto frame = std::min(seconds * s.fps, static_cast<double>(s.frame_count - 1));
    const auto lo = static_cast<std::size_t>(std::floor(frame)),
               hi = std::min(lo + 1, static_cast<std::size_t>(s.frame_count - 1));
    const auto t = frame - static_cast<double>(lo);
    Pose p;
    p.local.reserve(a.bones.size());
    for (std::size_t j = 0; j < a.bones.size(); ++j) {
        check_stop(stop);
        const auto begin = static_cast<std::size_t>(s.first_raw_frame) * a.bones.size();
        const auto l = begin + lo * a.bones.size() + j, h = begin + hi * a.bones.size() + j;
        const auto &kl = a.keys.at(l);
        const auto &kh = a.keys.at(h);
        const auto scale = a.scale_keys.empty() ? Vec3{1, 1, 1}
                                                : lerp(a.scale_keys.at(l).scale, a.scale_keys.at(h).scale, t);
        const auto parent = binding ? binding->parents[j] : a.bones[j].parent;
        auto local = decoded(lerp(kl.translation, kh.translation, t), slerp(kl.rotation, kh.rotation, t),
                             !parent, scale);
        if (config) {
            const auto flags = config->removed_tracks[clip][j];
            if (flags & 1)
                local.translation = binding->reference_local[j].translation;
            if (flags & 2)
                local.rotation = binding->reference_local[j].rotation;
        }
        p.local.push_back(local);
    }
    worlds(a, p, stop,
           binding ? std::span{binding->parents} : std::span<const std::optional<std::uint32_t>>{});
    return p;
}
} // namespace
Pose evaluate(const ActorAsset &a, std::size_t clip, double seconds, std::stop_token stop) {
    return evaluate_impl(a, clip, seconds, stop);
}
Pose evaluate_paired(const ActorAsset &a, const Pairing &binding, std::size_t clip, double seconds,
                     const PsaConfig *config, std::stop_token stop) {
    require(binding.compatible && binding.animation_sha256 == a.source.identity.sha256 &&
                binding.parents.size() == a.bones.size() && binding.reference_local.size() == a.bones.size(),
            "Pairing does not belong to this animation or is incomplete");
    if (config) {
        require(config->animation_sha256 == a.source.identity.sha256 &&
                    config->removed_tracks.size() == a.sequences.size() && clip < a.sequences.size() &&
                    config->removed_tracks[clip].size() == a.bones.size(),
                "PSA companion does not belong to this animation or is incomplete");
        for (const auto flags : config->removed_tracks[clip])
            require(flags <= 3, "Invalid PSA companion track flags");
    }
    return evaluate_impl(a, clip, seconds, stop, &binding, config);
}
Pairing pair(const ActorAsset &a, const ActorAsset &m, double tolerance, PairingPolicy policy) {
    Pairing result;
    if (!a.animation || !m.mesh || m.animation || !std::isfinite(tolerance) || tolerance < 0) {
        result.reasons.push_back(
            "Pair requires PSA and reference PSK mesh plus a finite nonnegative tolerance");
        return result;
    }
    std::map<std::string, std::uint32_t> names;
    for (std::size_t j = 0; j < m.bones.size(); ++j)
        if (!names.emplace(m.bones[j].source.original_name, static_cast<std::uint32_t>(j)).second)
            result.reasons.push_back("Duplicate mesh bone name");
    std::set<std::string> anames;
    for (const auto &b : a.bones) {
        const auto &name = b.source.original_name;
        const auto it = names.find(name);
        if (!anames.insert(name).second || it == names.end()) {
            result.reasons.push_back("Missing or ambiguous animation bone: " + name);
            continue;
        }
        result.animation_to_mesh.push_back(it->second);
    }
    if (a.bones.size() != m.bones.size())
        result.reasons.push_back("Bone counts differ");
    if (!result.reasons.empty())
        return result;
    result.animation_sha256 = a.source.identity.sha256;
    result.reference_sha256 = m.source.identity.sha256;
    result.uses_reference_skeleton = policy == PairingPolicy::ReferenceSkeleton;
    std::vector<std::uint32_t> mesh_to_animation(m.bones.size());
    for (std::size_t j = 0; j < a.bones.size(); ++j)
        mesh_to_animation[result.animation_to_mesh[j]] = static_cast<std::uint32_t>(j);
    bool reference = true, hierarchy = true;
    for (std::size_t j = 0; j < a.bones.size(); ++j) {
        const auto &x = a.bones[j];
        const auto &y = m.bones[result.animation_to_mesh[j]];
        if (x.parent.has_value() != y.parent.has_value() ||
            (x.parent && result.animation_to_mesh[*x.parent] != *y.parent))
            hierarchy = false;
        result.parents.push_back(y.parent ? std::optional{mesh_to_animation[*y.parent]} : std::nullopt);
        result.reference_local.push_back(y.default_local);
        if (!x.default_local_valid || !y.default_local_valid)
            reference = false;
        else {
            const auto u = matrix(x.default_local), v = matrix(y.default_local);
            for (std::size_t k = 0; k < 16; ++k)
                if (std::abs(u[k] - v[k]) > tolerance)
                    reference = false;
        }
    }
    result.reference_verified = reference;
    result.hierarchy_verified = hierarchy;
    auto &diagnostics = result.uses_reference_skeleton ? result.warnings : result.reasons;
    if (!hierarchy)
        diagnostics.push_back(
            "PSA parent records differ; paired evaluation uses the reference PSK hierarchy");
    if (!reference)
        diagnostics.push_back(
            "PSA bone records do not verify the PSK reference pose; PSK reference retained");
    result.compatible = result.reasons.empty();
    return result;
}
PsaConfig read_psa_config(std::span<const std::byte> bytes, const ActorAsset &a, const ReadOptions &options) {
    check_stop(options.stop);
    require(a.animation, "PSA companion requires an animation");
    if (bytes.size() > options.limits.input_bytes || bytes.size() > options.limits.decoded_bytes / 4 ||
        a.sequences.size() > options.limits.clips || a.bones.size() > options.limits.bones ||
        (!a.bones.empty() && a.sequences.size() > options.limits.elements / a.bones.size()))
        throw Failure(ErrorCode::ResourceLimit, "PSA companion input/work budget exceeded");
    const auto masks = a.sequences.size() * a.bones.size();
    if (masks > options.limits.decoded_bytes - bytes.size() * 4)
        throw Failure(ErrorCode::ResourceLimit, "PSA companion decoded budget exceeded");
    Reader budget{bytes, options};
    budget.budget(bytes.size(), 2, false); // Raw copy and text staging.
    budget.budget(a.sequences.size(), sizeof(std::vector<std::uint8_t>) + 256);
    budget.budget(masks, 1);
    for (const auto &clip : a.sequences)
        budget.budget(clip.source.original_name.size(), 4, false);
    PsaConfig result;
    result.source = a.source;
    result.source.identity.sha256 = sha256(bytes, options.stop);
    result.source.identity.object_key = "actorx-companion/RemoveTracks";
    result.animation_sha256 = a.source.identity.sha256;
    result.source_bytes.assign(bytes.begin(), bytes.end());
    result.removed_tracks.assign(a.sequences.size(), std::vector<std::uint8_t>(a.bones.size()));
    const auto trim = [](std::string_view v) {
        const auto start = v.find_first_not_of(" \t\r");
        if (start == std::string_view::npos)
            return std::string_view{};
        return v.substr(start, v.find_last_not_of(" \t\r") - start + 1);
    };
    const auto lower = [](std::string_view v) {
        std::string out(v);
        for (auto &c : out)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return out;
    };
    std::map<std::string, std::vector<std::size_t>> clips;
    for (std::size_t i = 0; i < a.sequences.size(); ++i)
        clips[lower(a.sequences[i].source.original_name)].push_back(i);
    std::string contents;
    if (!bytes.empty())
        contents.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    require(contents.find('\0') == std::string::npos, "NUL in PSA companion text");
    std::string_view remaining(contents);
    if (remaining.starts_with("\xef\xbb\xbf"))
        remaining.remove_prefix(3);
    bool remove_section{};
    std::uint64_t entries{};
    std::set<std::string> keys;
    while (!remaining.empty()) {
        check_stop(options.stop);
        const auto newline = remaining.find('\n');
        auto line = trim(remaining.substr(0, newline));
        remaining = newline == std::string_view::npos ? std::string_view{} : remaining.substr(newline + 1);
        if (line.empty() || line.starts_with(';') || line.starts_with('#'))
            continue;
        if (line.starts_with('[')) {
            require(line.ends_with(']'), "Malformed PSA companion section");
            remove_section = lower(trim(line.substr(1, line.size() - 2))) == "removetracks";
            continue;
        }
        if (!remove_section)
            continue; // Other sections remain verbatim in source_bytes.
        if (++entries > options.limits.elements)
            throw Failure(ErrorCode::ResourceLimit, "PSA companion entry work budget exceeded");
        budget.budget(line.size(), 4, false);
        budget.budget(1, 512); // Key set and possible warning/node overhead.
        const auto equals = line.find('=');
        require(equals != std::string_view::npos, "Missing PSA companion track value");
        const auto key = lower(trim(line.substr(0, equals)));
        require(keys.insert(key).second, "Duplicate PSA companion track entry");
        const auto dot = key.rfind('.');
        require(dot != std::string::npos, "Malformed PSA companion clip.bone key");
        std::uint32_t bone{};
        const auto digits = std::string_view(key).substr(dot + 1);
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), bone);
        require(parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size() &&
                    bone < a.bones.size(),
                "PSA companion bone index outside animation");
        const auto value = lower(trim(line.substr(equals + 1)));
        require(value == "all" || value == "trans" || value == "rot", "Unknown PSA companion track value");
        const auto clip = clips.find(key.substr(0, dot));
        if (clip == clips.end()) {
            result.warnings.push_back("Companion entry names a clip absent from this PSA: " + key);
            continue;
        }
        const std::uint8_t flags = value == "all" ? 3 : value == "trans" ? 1 : 2;
        for (const auto index : clip->second)
            result.removed_tracks[index][bone] = flags;
    }
    return result;
}
} // namespace formaview::skeletal
