#include "formaview/skeletal.hpp"
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <set>
#include <thread>
#ifdef FORMAVIEW_TEST_FBX
#include "formaview/fbx.hpp"
#endif
namespace sk = formaview::skeletal;
namespace {
using Bytes = std::vector<std::byte>;
void check(bool ok, const char *msg) {
    if (!ok)
        throw std::runtime_error(msg);
}
void near(double a, double b, double tolerance = 1e-7) {
    check(std::abs(a - b) <= tolerance, "Numeric mismatch");
}
void u32(Bytes &b, std::uint32_t x) {
    for (unsigned i = 0; i < 4; ++i)
        b.push_back(static_cast<std::byte>((x >> (8 * i)) & 255));
}
void f32(Bytes &b, float x) { u32(b, std::bit_cast<std::uint32_t>(x)); }
void text(Bytes &b, std::string s, std::size_t n) {
    s.resize(n);
    for (char c : s)
        b.push_back(static_cast<std::byte>(c));
}
void vec(Bytes &b, float x, float y, float z) {
    f32(b, x);
    f32(b, y);
    f32(b, z);
}
void quat(Bytes &b) {
    vec(b, 0, 0, 0);
    f32(b, 1);
}
void block(Bytes &b, std::string name, std::uint32_t stride, const Bytes &data) {
    text(b, std::move(name), 20);
    u32(b, 1999801);
    u32(b, stride);
    u32(b, stride ? static_cast<std::uint32_t>(data.size() / stride) : 0);
    b.insert(b.end(), data.begin(), data.end());
}
Bytes bones(bool reordered = false) {
    Bytes b;
    for (unsigned i = 0; i < 7; ++i) {
        const auto j = reordered && i > 0 ? 7 - i : i;
        text(b, "bone" + std::to_string(j), 64);
        u32(b, 0);
        u32(b, i == 0 ? 6 : 0);
        u32(b, 0);
        quat(b);
        vec(b, i == 0 ? 10.0F : static_cast<float>(j), 0, 0);
        f32(b, 1);
        vec(b, 1, 1, 1);
    }
    return b;
}
Bytes mesh(bool wide = false, bool reordered = false) {
    Bytes b;
    block(b, "ACTRHEAD", 0, {});
    block(b, "REFSKELT", 120, bones(reordered));
    Bytes p;
    vec(p, 0, 0, 0);
    vec(p, 1, 0, 0);
    vec(p, 0, 1, 0);
    block(b, "PNTS0000", 12, p);
    Bytes w;
    for (unsigned i = 0; i < 4; ++i) {
        u32(w, i % 3);
        f32(w, static_cast<float>(i));
        f32(w, 0.5F);
        u32(w, 0);
    }
    block(b, "VTXW0000", 16, w);
    Bytes f;
    for (unsigned i = 0; i < 2; ++i) {
        for (unsigned j = 0; j < 3; ++j) {
            const auto k = i && j == 0 ? 3 : j;
            if (wide)
                u32(f, k);
            else {
                f.push_back(static_cast<std::byte>(k));
                f.push_back(std::byte{});
            }
        }
        f.push_back(std::byte{});
        f.push_back(std::byte{});
        u32(f, 1);
    }
    block(b, wide ? "FACE3200" : "FACE0000", wide ? 18 : 12, f);
    Bytes mat;
    text(mat, "material", 64);
    for (unsigned i = 0; i < 6; ++i)
        u32(mat, 0);
    block(b, "MATT0000", 88, mat);
    Bytes weights;
    for (unsigned i = 1; i < 7; ++i) {
        f32(weights, static_cast<float>(i) / 10);
        u32(weights, 0);
        u32(weights, i);
    }
    block(b, "RAWWEIGHTS", 12, weights);
    Bytes uv;
    for (unsigned i = 0; i < 4; ++i) {
        f32(uv, static_cast<float>(i) / 2);
        f32(uv, -1);
    }
    block(b, "EXTRAUVS0", 8, uv);
    Bytes ns;
    for (unsigned i = 0; i < 3; ++i)
        vec(ns, 0, 0, 1);
    block(b, "VTXNORMS", 12, ns);
    block(b, "VERTEXCOLOR", 4, Bytes(16, std::byte{128}));
    block(b, "FUTURE", 1, Bytes{std::byte{7}});
    return b;
}
Bytes animation() {
    Bytes b;
    block(b, "ANIMHEAD", 0, {});
    block(b, "BONENAMES", 120, bones());
    Bytes info;
    for (unsigned i = 0; i < 2; ++i) {
        text(info, "clip" + std::to_string(i), 64);
        text(info, "group", 64);
        u32(info, 7);
        u32(info, 0);
        u32(info, 0);
        u32(info, 0);
        f32(info, 0);
        f32(info, i ? 2.0F : 3.0F);
        f32(info, 23.976F);
        u32(info, 0);
        u32(info, i ? 3 : 0);
        u32(info, i ? 2 : 3);
    }
    block(b, "ANIMINFO", 168, info);
    Bytes keys, scales;
    for (unsigned i = 0; i < 5; ++i)
        for (unsigned j = 0; j < 7; ++j) {
            vec(keys, j ? static_cast<float>(j) : 11.0F + static_cast<float>(i), 0, 0);
            quat(keys);
            f32(keys, 1 / 23.976F);
            vec(scales, j ? 1.0F : 1.0F + static_cast<float>(i), 1, 1);
            f32(scales, 1 / 23.976F);
        }
    block(b, "ANIMKEYS", 32, keys);
    block(b, "SCALEKEYS", 16, scales);
    return b;
}
std::size_t offset(const Bytes &b, std::string_view name) {
    const auto a = sk::read_actor(b);
    for (const auto &c : a.chunks)
        if (c.name == name)
            return static_cast<std::size_t>(c.offset);
    throw std::runtime_error("Test chunk missing");
}
void set32(Bytes &b, std::size_t o, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        b[o + i] = static_cast<std::byte>((v >> (i * 8)) & 255);
}
void reject(const std::function<void()> &f,
            formaview::ErrorCode code = formaview::ErrorCode::InvalidArgument) {
    bool rejected{};
    try {
        f();
    } catch (const sk::Failure &e) {
        check(e.error.code == code, "Wrong error code");
        rejected = true;
    }
    check(rejected, "Invalid input was accepted");
}
void reading() {
    const auto m = sk::read_actor(mesh(true));
    const auto &g = *m.mesh;
    check(m.bones.size() == 7 && g.faces.size() == 2 && g.wedges.size() == 4, "Mesh counts");
    check(g.weights.size() == 6 && g.point_weights[0].size() == 6, "Full influences lost");
    near(g.weights[5].value, 0.6, 1e-6);
    check(g.render_mapping[3].source_point == 0 && g.render_mapping[3].render_vertex == 3,
          "UV seam mapping lost");
    check(g.extra_uvs.size() == 1 && g.normals.size() == 3 && g.colours.size() == 4, "PSKX attributes lost");
    near(g.extra_uvs.begin()->second[0].y, -1);
    check(m.source_bytes == mesh(true), "Raw unsupported payload changed");
    check(m.bones[0].bind_world.has_value(), "PSK bind missing");
    const auto a = sk::read_actor(animation());
    check(!a.mesh && a.sequences.size() == 2, "PSA independent contract");
    check(!a.bones[0].bind_world, "PSA falsely reported verified bind");
    // Some real PSA files carry zero placeholders in BONENAMES rather than a
    // reference pose; their actual animation keys are valid and must be retained.
    auto placeholder_bytes = animation();
    const auto placeholders = offset(placeholder_bytes, "BONENAMES");
    for (std::size_t j = 0; j < 7; ++j)
        for (std::size_t c = 0; c < 4; ++c)
            set32(placeholder_bytes, placeholders + j * 120 + 76 + c * 4, 0);
    const auto placeholder = sk::read_actor(placeholder_bytes);
    check(placeholder.source_bytes == placeholder_bytes, "PSA placeholder source bytes changed");
    reject([&] { (void)sk::default_pose(placeholder); }, formaview::ErrorCode::Unsupported);
    const auto reference = sk::read_actor(mesh());
    check(!sk::pair(placeholder, reference).compatible, "Missing PSA reference was certified");
    const auto reference_pair = sk::pair(placeholder, reference, 1e-4, sk::PairingPolicy::ReferenceSkeleton);
    check(reference_pair.compatible && !reference_pair.reference_verified,
          "Valid PSA keys with placeholder records must accept an explicit PSK reference");
    near(sk::evaluate_paired(placeholder, reference_pair, 0, 0).world[1][3], 12);
    auto invalid_key = placeholder_bytes;
    const auto key = offset(invalid_key, "ANIMKEYS");
    for (std::size_t c = 0; c < 4; ++c)
        set32(invalid_key, key + 12 + c * 4, 0);
    reject([&] { (void)sk::read_actor(invalid_key); });
    auto invalid_bind = mesh();
    const auto bind = offset(invalid_bind, "REFSKELT");
    for (std::size_t c = 0; c < 4; ++c)
        set32(invalid_bind, bind + 76 + c * 4, 0);
    reject([&] { (void)sk::read_actor(invalid_bind); });
    auto cyclic_placeholder = placeholder_bytes;
    set32(cyclic_placeholder, placeholders + 120 + 72, 2);
    set32(cyclic_placeholder, placeholders + 240 + 72, 1);
    reject([&] { (void)sk::read_actor(cyclic_placeholder); });
    const auto fps = *a.sequences[0].time.frame_rate;
    near(static_cast<double>(fps.numerator) / static_cast<double>(fps.denominator), a.sequences[0].fps, 0);
    near(a.sequences[1].time.source_start_seconds, 0);
    near(a.sequences[1].time.source_end_seconds, 2 / a.sequences[1].fps);
    check(sk::sha256({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "SHA256 empty vector");
    const auto abc = std::string{"abc"};
    check(sk::sha256(std::as_bytes(std::span{abc})) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA256 known vector");
    auto unusual = mesh();
    const auto weights = offset(unusual, "RAWWEIGHTS");
    set32(unusual, weights, std::bit_cast<std::uint32_t>(-0.25F));
    set32(unusual, weights + 12, std::bit_cast<std::uint32_t>(0.0F));
    const auto raw = sk::read_actor(unusual);
    near(raw.mesh->weights[0].value, -0.25);
    near(raw.mesh->weights[1].value, 0);
    check(raw.mesh->point_weights[0].size() == 6, "Anomalous raw influences were cleaned");
}
void pose() {
    const auto a = sk::read_actor(animation());
    const auto &s = a.sequences[0];
    const auto p = sk::evaluate(a, 0, 0.5 / s.fps);
    near(p.local[0].translation.x, 11.5);
    near(p.local[0].scale.x, 1.5);
    near(p.world[1][3], 13);
    near(sk::default_pose(a).world[0][3], 10);
    near(sk::evaluate(a, 1, 0).world[0][3], 14);
    near(sk::evaluate(a, 0, s.time.source_end_seconds).world[0][3], 13);
    const sk::Transform t{{3, 4, 5}, {0, 0, std::sqrt(0.5), std::sqrt(0.5)}, {2, 3, 4}};
    const auto value = sk::multiply(sk::matrix(t), sk::inverse(sk::matrix(t)));
    for (std::size_t i = 0; i < 16; ++i)
        near(value[i], i % 5 == 0 ? 1 : 0);
    reject([&] { (void)sk::evaluate(a, 0, -1); });
    reject([&] { (void)sk::evaluate(a, 0, s.time.source_end_seconds + 1); });
    reject([] { (void)sk::inverse(sk::matrix({{}, {}, {0, 1, 1}})); });
    auto raw = animation();
    const auto key = offset(raw, "ANIMKEYS");
    set32(raw, key + 28, std::bit_cast<std::uint32_t>(0.5F));
    const auto nonuniform = sk::read_actor(raw);
    check(!nonuniform.sequences[0].uniform_key_deltas, "Nonuniform raw key timing was hidden");
    reject([&] { (void)sk::evaluate(nonuniform, 0, 0); }, formaview::ErrorCode::Unsupported);
    auto overflow = a;
    for (std::size_t j = 1; j < overflow.bones.size(); ++j)
        overflow.bones[j].parent = static_cast<std::uint32_t>(j - 1);
    for (auto &scale_key : overflow.scale_keys)
        scale_key.scale = {1e100, 1e100, 1e100};
    reject([&] { (void)sk::evaluate(overflow, 0, 0); });
}
void errors() {
    const auto original = mesh();
    const auto parsed = sk::read_actor(original);
    std::set<std::size_t> boundaries;
    for (const auto &c : parsed.chunks)
        boundaries.insert(static_cast<std::size_t>(c.offset) +
                          static_cast<std::size_t>(c.stride) * static_cast<std::size_t>(c.count));
    for (std::size_t n = 0; n < original.size(); ++n)
        if (!boundaries.contains(n))
            reject([&] { (void)sk::read_actor(std::span{original}.first(n)); });
    auto b = original;
    set32(b, offset(b, "RAWWEIGHTS") + 8, 999);
    reject([&] { (void)sk::read_actor(b); });
    b = original;
    set32(b, offset(b, "VTXW0000"), 999);
    reject([&] { (void)sk::read_actor(b); });
    b = original;
    const auto wo = offset(b, "VTXW0000");
    set32(b, wo, 0xABCD0000);
    reject([&] { (void)sk::read_actor(b); });
    sk::ReadOptions old;
    old.wedge_index = sk::WedgeIndex::Legacy16;
    check(sk::read_actor(b, old).mesh->wedges[0].point == 0, "Explicit legacy wedge policy");
    const auto bo = offset(original, "REFSKELT");
    b = original;
    set32(b, bo + 120 + 72, 2);
    set32(b, bo + 240 + 72, 1);
    reject([&] { (void)sk::read_actor(b); });
    b = original;
    set32(b, offset(b, "PNTS0000") - 8, 0xFFFFFFFF);
    reject([&] { (void)sk::read_actor(b); });
    b = original;
    set32(b, offset(b, "PNTS0000"), 0x7FC00000);
    reject([&] { (void)sk::read_actor(b); });
    b = original;
    block(b, "PNTS0000", 12, Bytes(12));
    reject([&] { (void)sk::read_actor(b); });
    b = animation();
    set32(b, offset(b, "ANIMINFO") + 160, 3);
    reject([&] { (void)sk::read_actor(b); });
    b = animation();
    const auto so = offset(b, "SCALEKEYS");
    b.resize(so);
    set32(b, so - 4, 0);
    check(sk::read_actor(b).scale_keys.empty(), "Empty SCALEKEYS should be absent");
}
void pairing() {
    const auto a = sk::read_actor(animation()), m = sk::read_actor(mesh(false, true));
    const auto match = sk::pair(a, m);
    check(match.compatible && match.reference_verified && match.animation_to_mesh[1] == 6,
          "Reordered skeleton pairing");
    auto changed = m;
    changed.bones[1].source.original_name = "bone0";
    check(!sk::pair(a, changed).compatible, "Duplicate names accepted");
    changed = m;
    changed.bones[1].parent = 2;
    check(!sk::pair(a, changed).compatible, "Different hierarchy accepted");
    changed = m;
    changed.bones[1].default_local.translation.x += 1;
    check(!sk::pair(a, changed).reference_verified && !sk::pair(a, changed).compatible,
          "Different reference accepted");
    auto target = sk::read_actor(mesh());
    target.bones[2].parent = 1;
    target.bones[2].default_local.rotation = {0, 0, std::sqrt(0.5), std::sqrt(0.5)};
    auto flat = a;
    flat.bones[2].default_local.translation.x = 999;
    check(!sk::pair(flat, target).compatible, "Strict record checks lost");
    const auto bound = sk::pair(flat, target, 1e-4, sk::PairingPolicy::ReferenceSkeleton);
    check(bound.compatible && !bound.reference_verified && !bound.hierarchy_verified &&
              bound.uses_reference_skeleton && bound.parents[2] == 1U,
          "Reference skeleton should supply hierarchy despite abbreviated PSA records");
    const auto raw = sk::evaluate(flat, 0, 0);
    const auto pose = sk::evaluate_paired(flat, bound, 0, 0);
    near(pose.world[2][3], 14); // Animated root 11 + child 1 + grandchild 2.
    near(raw.world[2][3], 13);
    const std::string ini = "[RemoveTracks]\nCLIP0.0=trans\nclip0.2=all\nclip1.1=rot\n";
    const auto config = sk::read_psa_config(std::as_bytes(std::span{ini}), flat);
    const auto masked = sk::evaluate_paired(flat, bound, 0, 0, &config);
    near(masked.local[0].translation.x, 10); // PSK reference, not key 11.
    near(masked.local[2].translation.x, 2);  // PSK reference, not PSA record 999.
    near(masked.world[2][3], 13);
    near(sk::matrix(masked.local[2])[0], 0);
    near(sk::matrix(masked.local[2])[4], 1);
    near(sk::evaluate_paired(flat, bound, 0, 1 / flat.sequences[0].fps, &config).local[0].scale.x, 2);
    check(config.removed_tracks[1][1] == 2 && config.removed_tracks[1][2] == 0, "Companion clip mask leaked");
    check(flat.bones[2].default_local.translation.x == 999 && flat.bones[2].parent == 0U,
          "Pairing overwrote raw PSA records");
    const std::string invalid = "[RemoveTracks]\nclip0.7=all\n";
    reject([&] { (void)sk::read_psa_config(std::as_bytes(std::span{invalid}), flat); });
    for (const std::string bad :
         {"[RemoveTracks]\nclip0.0=unknown", "[RemoveTracks]\nclip0.0=rot\nCLIP0.0=all"})
        reject([&] { (void)sk::read_psa_config(std::as_bytes(std::span{bad}), flat); });
    auto other_config = config;
    other_config.animation_sha256 = "different-content";
    reject([&] { (void)sk::evaluate_paired(flat, bound, 0, 0, &other_config); });
    sk::ReadOptions constrained;
    constrained.limits.decoded_bytes = 1024;
    reject([&] { (void)sk::read_psa_config(std::as_bytes(std::span{ini}), flat, constrained); },
           formaview::ErrorCode::ResourceLimit);
    std::stop_source stop;
    stop.request_stop();
    constrained = {};
    constrained.stop = stop.get_token();
    reject([&] { (void)sk::read_psa_config(std::as_bytes(std::span{ini}), flat, constrained); },
           formaview::ErrorCode::Cancelled);
    auto duplicate = target;
    duplicate.bones[1].source.original_name = "bone0";
    check(!sk::pair(flat, duplicate, 1e-4, sk::PairingPolicy::ReferenceSkeleton).compatible,
          "Reference policy accepted ambiguous names");
}
void limits() {
    const auto b = animation();
    sk::ReadOptions o;
    o.limits.input_bytes = b.size() - 1;
    reject([&] { (void)sk::read_actor(b, o); }, formaview::ErrorCode::ResourceLimit);
    o = {};
    o.limits.decoded_bytes = 1;
    reject([&] { (void)sk::read_actor(b, o); }, formaview::ErrorCode::ResourceLimit);
    o = {};
    o.limits.elements = 3;
    reject([&] { (void)sk::read_actor(b, o); }, formaview::ErrorCode::ResourceLimit);
    o = {};
    o.limits.chunks = 2;
    reject([&] { (void)sk::read_actor(b, o); }, formaview::ErrorCode::ResourceLimit);
    std::stop_source stop;
    stop.request_stop();
    o = {};
    o.stop = stop.get_token();
    reject([&] { (void)sk::read_actor(b, o); }, formaview::ErrorCode::Cancelled);
    const auto a = sk::read_actor(b);
    reject([&] { (void)sk::evaluate(a, 0, 0, stop.get_token()); }, formaview::ErrorCode::Cancelled);
}
#ifdef FORMAVIEW_TEST_FBX
// Keep per-operation timings in CTest output so an I/O wait cannot look like a
// numerical or cancellation hang. Expected rejection tests also record exceptions.
template <class Fn> auto fbx_step(const std::filesystem::path &path, const char *operation, Fn fn) {
    const auto name = path.filename().u8string();
    const std::string label(reinterpret_cast<const char *>(name.data()), name.size());
    const auto begin = std::chrono::steady_clock::now();
    std::cerr << "FBX_BEGIN " << operation << " " << label << std::endl;
    try {
        auto result = fn();
        std::cerr << "FBX_END " << operation << " " << label << " seconds="
                  << std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count()
                  << std::endl;
        return result;
    } catch (...) {
        std::cerr << "FBX_EXCEPTION " << operation << " " << label << " seconds="
                  << std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count()
                  << std::endl;
        throw;
    }
}
sk::FbxWriteResult write_fbx_traced(const sk::ActorAsset &asset, std::size_t clip,
                                    const std::filesystem::path &path, const sk::ActorAsset *mesh = nullptr,
                                    const sk::FbxWriteOptions &options = {}) {
    return fbx_step(path, "write", [&] { return sk::write_fbx_clip(asset, clip, path, mesh, options); });
}
sk::FbxDocument read_fbx_traced(const std::filesystem::path &path, const sk::ReadOptions &options = {}) {
    return fbx_step(path, "read", [&] { return sk::FbxDocument::read(path, options); });
}
void fbx(const std::filesystem::path &directory) {
    std::filesystem::create_directories(directory);
    sk::ReadOptions o;
    o.coordinates = {formaview::Axis::PositiveZ, formaview::Axis::PositiveX, formaview::Handedness::Right,
                     0.01, "Synthetic numeric fixture"};
    const auto a = sk::read_actor(animation(), o), m = sk::read_actor(mesh(), o);
    for (const auto &[name, data] : std::vector<std::pair<std::string, Bytes>>{
             {"source.psk", mesh()}, {"source.pskx", mesh(true)}, {"source.psa", animation()}}) {
        std::ofstream file(directory / name, std::ios::binary);
        file.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
        check(static_cast<bool>(file), "Cannot write service test source fixture");
    }
    // Pure PSK and wide-index PSKX exports must preserve source data without fake animation.
    for (const bool wide : {false, true}) {
        const auto source = sk::read_actor(mesh(wide), o);
        const auto path = directory / (wide ? "model-pskx.fbx" : "model-psk.fbx");
        std::filesystem::remove(path);
        sk::write_fbx_model(source, path);
        const auto encoded = sk::encode_fbx_model(source);
        const auto memory_path = directory / (wide ? "memory-pskx.fbx" : "memory-psk.fbx");
        {
            std::ofstream stream_file(memory_path, std::ios::binary);
            stream_file.write(reinterpret_cast<const char *>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        }
        const auto memory_back = read_fbx_traced(memory_path);
        check(memory_back.data().clips.empty() && memory_back.data().meshes.size() == 1, "Memory FBX encoding failed");
        sk::FbxWriteOptions small_encoded;
        small_encoded.limits.decoded_bytes = 10000;
        reject([&] { (void)sk::encode_fbx_model(source, small_encoded); }, formaview::ErrorCode::ResourceLimit);
        const auto back = read_fbx_traced(path);
        const auto &d = back.data();
        check(d.clips.empty() && d.meshes.size() == 1, "Pure model has animation or missing mesh");
        const auto &geometry = d.meshes[0];
        check(geometry.skins.size() == 1, "Pure model missing skin");
        const auto scalar = [](const formaview::Buffer &b, std::size_t index) {
            check(b.scalar == formaview::ScalarType::Float64 && (index + 1) * sizeof(double) <= b.bytes.size(), "Invalid readback attribute");
            double value{};
            std::memcpy(&value, b.bytes.data() + index * sizeof(double), sizeof(double));
            return value;
        };
        const auto &g = geometry.geometry;
        check(g.positions.element_count == source.mesh->points.size(), "Pure model point count");
        for (std::size_t j = 0; j < source.mesh->points.size(); ++j) {
            near(scalar(g.positions, j * 3), source.mesh->points[j].x, 0);
            near(scalar(g.positions, j * 3 + 1), source.mesh->points[j].y, 0);
            near(scalar(g.positions, j * 3 + 2), source.mesh->points[j].z, 0);
        }
        check(g.face_sizes.size() == source.mesh->faces.size(), "Pure model face count");
        for (std::size_t f = 0; f < source.mesh->faces.size(); ++f) {
            check(g.face_sizes[f] == 3 && geometry.face_materials[f] == source.mesh->faces[f].material, "Pure model topology/material");
            for (std::size_t c = 0; c < 3; ++c) {
                const auto wedge = source.mesh->faces[f].wedges[2 - c];
                check(g.corner_points[f * 3 + c] == source.mesh->wedges[wedge].point, "Pure model corner mapping");
                near(scalar(g.corner_attributes.at("uv/0/UV0"), (f * 3 + c) * 2), source.mesh->wedges[wedge].uv.x, 0);
                near(scalar(g.corner_attributes.at("uv/0/UV0"), (f * 3 + c) * 2 + 1), 1 - source.mesh->wedges[wedge].uv.y, 0);
                near(scalar(g.corner_attributes.at("normal"), (f * 3 + c) * 3 + 2), 1, 0);
                near(scalar(g.corner_attributes.at("colour/0"), (f * 3 + c) * 4), 128.0 / 255, 1e-7);
                for (const auto &[name, uv] : source.mesh->extra_uvs) {
                    const auto &back_uv = g.corner_attributes.at("uv/1/" + name);
                    near(scalar(back_uv, (f * 3 + c) * 2), uv[wedge].x, 0);
                    near(scalar(back_uv, (f * 3 + c) * 2 + 1), 1 - uv[wedge].y, 0);
                }
            }
        }

        std::size_t count{};
        for (const auto &cluster : geometry.skins[0].clusters) {
            const auto name = d.nodes[cluster.bone_node].source.original_name;
            const auto bone = std::find_if(source.bones.begin(), source.bones.end(),
                [&](const auto &b) { return b.source.original_name == name; });
            check(bone != source.bones.end(), "Pure model lost bone name");
            for (std::size_t k = 0; k < 16; ++k) {
                near(cluster.bind_to_world[k], (*bone->bind_world)[k], 1e-6);
                near(d.nodes[cluster.bone_node].default_world[k], (*bone->bind_world)[k], 1e-6);
            }
            for (std::size_t j = 0; j < cluster.points.size(); ++j) {
                const auto weight = std::find_if(source.mesh->weights.begin(), source.mesh->weights.end(),
                    [&](const auto &w) { return w.point == cluster.points[j] && source.bones[w.bone].source.original_name == name; });
                check(weight != source.mesh->weights.end(), "Pure model added influence");
                near(cluster.weights[j], weight->value, 0);
                ++count;
            }
        }
        check(count == source.mesh->weights.size() && count == 6, "Pure model truncated influences");
        reject([&] { sk::write_fbx_model(source, path); }, formaview::ErrorCode::Conflict);
        std::stop_source stop;
        stop.request_stop();
        sk::FbxWriteOptions stopped;
        stopped.stop = stop.get_token();
        reject([&] { sk::write_fbx_model(source, directory / "cancel-model.fbx", stopped); }, formaview::ErrorCode::Cancelled);
        auto static_source = source;
        static_source.bones.clear();
        static_source.mesh->weights.clear();
        static_source.mesh->point_weights.clear();
        const auto static_path = directory / (wide ? "static-pskx.fbx" : "static-psk.fbx");
        std::filesystem::remove(static_path);
        sk::write_fbx_model(static_source, static_path);
        const auto static_back = read_fbx_traced(static_path);
        check(static_back.data().clips.empty() && static_back.data().meshes.size() == 1 &&
              static_back.data().meshes[0].skins.empty(), "Static model acquired skin or animation");
    }
    // Near a global XYZ singularity, cluster matrix roundtrips can differ from
    // the raw pose matrix. A distant bind origin amplifies that difference in
    // the relative inverse and used to fail IsValidBindPose despite valid input.
    auto near_singular = m;
    for (std::size_t j = 0; j < near_singular.bones.size(); ++j) {
        auto &bone = near_singular.bones[j];
        bone.default_local.rotation = j == 0 ? sk::Quat{std::sqrt(0.5), 5e-7, std::sqrt(0.5), -1.5e-6} : sk::Quat{0,0,0,1};
        bone.default_local.translation = j == 0 ? sk::Vec3{0,1,90} : sk::Vec3{0,10,0};
    }
    const auto singular_pose = sk::default_pose(near_singular);
    for (std::size_t j = 0; j < near_singular.bones.size(); ++j) near_singular.bones[j].bind_world = singular_pose.world[j];
    const auto singular_path = directory / "near-singular-bind.fbx";
    std::filesystem::remove(singular_path);
    sk::write_fbx_model(near_singular, singular_path);
    const auto singular_back = read_fbx_traced(singular_path);
    check(singular_back.data().clips.empty(), "Model gained an animation");
    for (const auto &cluster : singular_back.data().meshes.at(0).skins.at(0).clusters) {
        const auto &node = singular_back.data().nodes[cluster.bone_node];
        const auto bone = std::find_if(near_singular.bones.begin(), near_singular.bones.end(), [&](const auto &b) { return b.source.original_name == node.source.original_name; });
        check(bone != near_singular.bones.end(), "Bind regression lost a bone");
        for (std::size_t k = 0; k < 16; ++k) {
            near(cluster.bind_to_world[k], (*bone->bind_world)[k], 1e-4);
            near(node.default_world[k], (*bone->bind_world)[k], 1e-4);
        }
    }
    auto placeholder_bytes = animation();
    const auto records = offset(placeholder_bytes, "BONENAMES");
    for (std::size_t j = 0; j < 7; ++j)
        for (std::size_t c = 0; c < 4; ++c)
            set32(placeholder_bytes, records + j * 120 + 76 + c * 4, 0);
    const auto placeholder = sk::read_actor(placeholder_bytes, o);
    const auto placeholder_path = directory / "placeholder.fbx";
    std::filesystem::remove(placeholder_path);
    reject([&] { write_fbx_traced(placeholder, 0, placeholder_path); }, formaview::ErrorCode::Unsupported);
    check(!std::filesystem::exists(placeholder_path), "Rejected placeholder export created a file");
    sk::FbxWriteOptions reference_options;
    reference_options.pairing_policy = sk::PairingPolicy::ReferenceSkeleton;
    write_fbx_traced(placeholder, 0, placeholder_path, &m, reference_options);
    const auto placeholder_back = read_fbx_traced(placeholder_path);
    const auto placeholder_pair = sk::pair(placeholder, m, 1e-4, sk::PairingPolicy::ReferenceSkeleton);
    for (const double seconds :
         {0.0, 0.5 / placeholder.sequences[0].fps, placeholder.sequences[0].time.source_end_seconds}) {
        const auto expected = sk::evaluate_paired(placeholder, placeholder_pair, 0, seconds);
        const auto actual =
            placeholder_back.evaluate(0, std::min(seconds, placeholder_back.data().clips[0].end));
        for (std::size_t j = 0; j < placeholder.bones.size(); ++j) {
            const auto &nodes = placeholder_back.data().nodes;
            const auto it = std::find_if(nodes.begin(), nodes.end(), [&](const auto &node) {
                return node.bone && node.source.original_name == placeholder.bones[j].source.original_name;
            });
            check(it != nodes.end(), "Placeholder export lost a bone");
            for (std::size_t k = 0; k < 16; ++k)
                near(actual.world[static_cast<std::size_t>(it - nodes.begin())][k], expected.world[j][k],
                     1e-3);
        }
    }
    {
        auto multi = a;
        multi.sequences[1].fps = 30;
        multi.sequences[1].time.source_end_seconds = multi.sequences[1].frame_count / 30.0;
        const std::array<std::size_t, 2> selection{1, 0};
        std::vector<sk::FbxWriteResult> audits;
        sk::FbxWriteOptions multi_options;
        multi_options.include_mesh = false;
        const auto encoded = sk::encode_fbx_clips(multi, selection, &m, multi_options, &audits);
        const auto file = directory / "independent-stacks.fbx";
        std::ofstream stream(file, std::ios::binary);
        stream.write(reinterpret_cast<const char *>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        stream.close();
        const auto back = read_fbx_traced(file);
        const auto &data = back.data();
        check(data.clips.size() == 2 && data.meshes.empty() && audits.size() == 2,
              "Multiple independent stacks or skeleton-only output lost");
        for (std::size_t i = 0; i < selection.size(); ++i) {
            const auto selected = selection[i];
            const auto &seq = multi.sequences[selected];
            check(data.clips[i].source.original_name == seq.source.original_name, "Stack order/name changed");
            near(data.clips[i].start, 0);
            near(data.clips[i].end, seq.time.source_end_seconds, 1e-8);
            for (int half = 0; half <= seq.frame_count * 2; ++half) {
                const auto time = std::min(half * .5 / seq.fps, seq.time.source_end_seconds);
                const auto expected = sk::evaluate(multi, selected, time);
                const auto actual = back.evaluate(i, std::min(time, data.clips[i].end));
                for (std::size_t j = 0; j < multi.bones.size(); ++j) {
                    const auto it = std::find_if(data.nodes.begin(), data.nodes.end(), [&](const auto &n) {
                        return n.bone && n.source.original_name == multi.bones[j].source.original_name;
                    });
                    check(it != data.nodes.end(), "Stack export lost a bone");
                    for (std::size_t k = 0; k < 16; ++k)
                        near(actual.world[static_cast<std::size_t>(it - data.nodes.begin())][k], expected.world[j][k], 1e-3);
                }
            }
        }
        reject([&] { (void)sk::encode_fbx_clips(a, {}); }, formaview::ErrorCode::InvalidArgument);
        const std::array<std::size_t, 2> duplicate{0, 0};
        reject([&] { (void)sk::encode_fbx_clips(a, duplicate); }, formaview::ErrorCode::InvalidArgument);
        auto bounded = multi_options;
        bounded.limits.elements = 6;
        reject([&] { (void)sk::encode_fbx_clips(a, selection, &m, bounded); }, formaview::ErrorCode::ResourceLimit);
    }
    for (std::size_t clip = 0; clip < a.sequences.size(); ++clip) {
        const auto path = clip == 0 ? directory / std::filesystem::path(u8"骨骼动画-0.fbx")
                                    : directory / ("synthetic-" + std::to_string(clip) + ".fbx");
        std::filesystem::remove(path);
        const auto written = write_fbx_traced(a, clip, path, &m);
        const auto f = read_fbx_traced(path);
        const auto &d = f.data();
        check(d.clips.size() == 1 && d.meshes.size() == 1, "FBX scene counts");
        check(d.source.coordinates.up == formaview::Axis::PositiveZ &&
                  d.source.coordinates.forward == formaview::Axis::PositiveX &&
                  d.source.coordinates.handedness == formaview::Handedness::Right,
              "FBX source axes lost");
        auto editing_rate = std::max(1.0, std::round(a.sequences[clip].fps));
        while (editing_rate < written.bake_fps * 0.02) editing_rate *= 2;
        near(d.fps, editing_rate);
        near(d.clips[0].end, a.sequences[clip].time.source_end_seconds);
        for (int frame = 0; frame < a.sequences[clip].frame_count; ++frame) {
            const auto seconds = static_cast<double>(frame) / a.sequences[clip].fps;
            const auto src = sk::evaluate(a, clip, seconds);
            const auto dst = f.evaluate(0, seconds);
            for (std::size_t j = 0; j < a.bones.size(); ++j) {
                const auto it = std::find_if(d.nodes.begin(), d.nodes.end(), [&](const auto &n) {
                    return n.source.original_name == a.bones[j].source.original_name;
                });
                check(it != d.nodes.end(), "FBX bone name lost");
                const auto index = static_cast<std::size_t>(it - d.nodes.begin());
                for (std::size_t k = 0; k < 16; ++k)
                    near(src.world[j][k], dst.world[index][k], 1e-5);
            }
        }
        std::size_t weights{};
        for (const auto &c : d.meshes[0].skins[0].clusters) {
            weights += c.weights.size();
            for (std::size_t i = 0; i < c.weights.size(); ++i) {
                const auto it = std::find_if(m.mesh->weights.begin(), m.mesh->weights.end(), [&](auto w) {
                    return w.point == c.points[i] &&
                           m.bones[w.bone].source.original_name == d.nodes[c.bone_node].source.original_name;
                });
                check(it != m.mesh->weights.end(), "Exported influence has no source weight");
                near(c.weights[i], it->value, 0);
            }
        }
        check(weights == 6, "Six influences truncated on export");
        reject([&] { write_fbx_traced(a, clip, path, &m); }, formaview::ErrorCode::Conflict);
        sk::ReadOptions tiny;
        tiny.limits.decoded_bytes = 1;
        reject([&] { (void)read_fbx_traced(path, tiny); }, formaview::ErrorCode::ResourceLimit);
        std::stop_source stop;
        stop.request_stop();
        sk::ReadOptions cancelled;
        cancelled.stop = stop.get_token();
        reject([&] { (void)read_fbx_traced(path, cancelled); }, formaview::ErrorCode::Cancelled);
        sk::FbxWriteOptions small;
        small.limits.decoded_bytes = 1;
        reject([&] { write_fbx_traced(a, clip, directory / "must-not-exist.fbx", nullptr, small); },
               formaview::ErrorCode::ResourceLimit);
        sk::FbxWriteOptions stopped;
        stopped.stop = stop.get_token();
        reject([&] { write_fbx_traced(a, clip, directory / "must-not-exist.fbx", nullptr, stopped); },
               formaview::ErrorCode::Cancelled);
        check(!std::filesystem::exists(directory / "must-not-exist.fbx"), "Rejected export left output");
    }
    // Cross the Euler Y singularity and the +/-180 boundary. These audit times
    // are not export keys; a parent chain exposes orientation errors as motion.
    for (unsigned axis = 1; axis <= 2; ++axis) {
        auto crossing = a;
        auto reference = m;
        crossing.sequences.resize(1);
        crossing.sequences[0].frame_count = 2;
        crossing.sequences[0].time.source_end_seconds = 2 / crossing.sequences[0].fps;
        crossing.bones[2].parent = 1;
        reference.bones[2].parent = 1;
        const auto bind = sk::default_pose(reference);
        for (std::size_t j = 0; j < reference.bones.size(); ++j)
            reference.bones[j].bind_world = bind.world[j];
        for (std::size_t frame = 0; frame < 2; ++frame) {
            const auto degrees = axis == 1 ? (frame ? 91.0 : 89.0) : (frame ? -170.0 : 170.0);
            const auto angle = degrees * std::acos(-1.0) / 360;
            sk::Quat q{0, 0, 0, std::cos(angle)};
            if (axis == 1)
                q.y = -std::sin(angle);
            else
                q.z = -std::sin(angle);
            crossing.keys[frame * crossing.bones.size() + 1].rotation = q;
        }
        for (bool mesh_mode : {false, true}) {
            const auto path =
                directory / ("crossing-" + std::to_string(axis) + (mesh_mode ? "-mesh.fbx" : "-pure.fbx"));
            std::filesystem::remove(path);
            sk::FbxWriteOptions write;
            write.include_mesh = mesh_mode;
            write_fbx_traced(crossing, 0, path, &reference, write);
            const auto f = read_fbx_traced(path);
            const auto &nodes = f.data().nodes;
            for (double fraction : {0.137, 0.3819660112501051, 0.731}) {
                const auto seconds = fraction / crossing.sequences[0].fps;
                const auto expected = sk::evaluate(crossing, 0, seconds);
                const auto actual = f.evaluate(0, seconds);
                for (std::size_t j = 0; j < crossing.bones.size(); ++j) {
                    const auto it = std::find_if(nodes.begin(), nodes.end(), [&](const auto &node) {
                        return node.source.original_name == crossing.bones[j].source.original_name;
                    });
                    check(it != nodes.end(), "Crossing bone missing");
                    const auto index = static_cast<std::size_t>(it - nodes.begin());
                    for (std::size_t k = 0; k < 16; ++k)
                        near(expected.world[j][k], actual.world[index][k], 1e-4);
                }
            }
        }
    }
    // A single fixed-axis SLERP arc should choose its motion-aligned chart,
    // rather than needlessly subdividing a poorly conditioned Euler basis.
    auto coupled = a;
    coupled.sequences.resize(1);
    coupled.sequences[0].frame_count = 2;
    coupled.sequences[0].time.source_end_seconds = 2 / coupled.sequences[0].fps;
    coupled.keys[1].rotation = {0, 0, 0, 1};
    coupled.keys[coupled.bones.size() + 1].rotation = {-0.3, -0.4, -0.2, 0.8};
    const auto aligned_path = directory / "motion-aligned.fbx";
    std::filesystem::remove(aligned_path);
    const auto aligned = write_fbx_traced(coupled, 0, aligned_path, &m);
    check(aligned.sample_subdivisions == 1 && aligned.audited_world_matrix_error <= 0.0005,
          "Fixed-axis motion was not represented by a linear Euler channel");
    // Changing rotation axes still needs refinement. Keep a third source frame
    // so this fixture cannot be made exact by aligning a single fixed axis.
    coupled.sequences[0].frame_count = 3;
    coupled.sequences[0].time.source_end_seconds = 3 / coupled.sequences[0].fps;
    coupled.keys[2 * coupled.bones.size() + 1].rotation = {0.5, -0.3, 0.2, 0.6};
    sk::FbxWriteOptions unrefined;
    unrefined.max_sample_subdivisions = 1;
    const auto too_coarse = directory / "coupled-coarse.fbx";
    std::filesystem::remove(too_coarse);
    reject([&] { write_fbx_traced(coupled, 0, too_coarse, &m, unrefined); },
           formaview::ErrorCode::Unsupported);
    check(!std::filesystem::exists(too_coarse), "Rejected coarse bake left output");
    const auto refined = directory / "coupled-refined.fbx";
    std::filesystem::remove(refined);
    const auto bake = write_fbx_traced(coupled, 0, refined, &m);
    check(bake.sample_subdivisions > 1 && bake.audited_world_matrix_error <= 0.0005,
          "Coupled rotation was not refined");
    const auto coupled_back = read_fbx_traced(refined);
    auto lever_model = m;
    for (auto &point : lever_model.mesh->points) point.x += 200;
    const auto lever_path = directory / "skin-lever-arm.fbx";
    std::filesystem::remove(lever_path);
    const auto lever_bake = write_fbx_traced(coupled, 0, lever_path, &lever_model);
    check(lever_bake.samples > bake.samples, "Distant skin points did not refine the bake");
    const auto lever_back = read_fbx_traced(lever_path);
    for (double fraction : {0.137, 0.381966, 0.731}) {
        const auto seconds = fraction / coupled.sequences[0].fps;
        const auto expected = sk::evaluate(coupled, 0, seconds);
        const auto actual = lever_back.evaluate(0, seconds);
        for (const auto &cluster : lever_back.data().meshes.at(0).skins.at(0).clusters) {
            const auto &name = lever_back.data().nodes[cluster.bone_node].source.original_name;
            const auto bone = std::find_if(coupled.bones.begin(), coupled.bones.end(), [&](const auto &b) { return b.source.original_name == name; });
            check(bone != coupled.bones.end(), "Skin regression lost a bone");
            const auto index = static_cast<std::size_t>(bone - coupled.bones.begin());
            const auto e = sk::multiply(expected.world[index], sk::inverse(*lever_model.bones[index].bind_world));
            const auto v = sk::multiply(actual.world[cluster.bone_node], cluster.geometry_to_bone);
            for (auto point : cluster.points) {
                const auto &p = lever_model.mesh->points[point];
                for (std::size_t row = 0; row < 3; ++row)
                    near(e[row*4]*p.x + e[row*4+1]*p.y + e[row*4+2]*p.z + e[row*4+3],
                         v[row*4]*p.x + v[row*4+1]*p.y + v[row*4+2]*p.z + v[row*4+3], 0.0005);
            }
        }
    }
    // A long clip with one changing rotation must not charge every static T/R/S
    // channel for every source frame and every adaptive rotation subdivision.
    auto sparse = coupled;
    sparse.sequences[0].frame_count = 100;
    sparse.sequences[0].time.source_end_seconds = 100 / sparse.sequences[0].fps;
    sparse.keys.resize(100 * sparse.bones.size());
    sparse.scale_keys.resize(sparse.keys.size());
    for (std::size_t frame = 0; frame < 100; ++frame)
        for (std::size_t bone = 0; bone < sparse.bones.size(); ++bone) {
            sparse.keys[frame * sparse.bones.size() + bone] = coupled.keys[bone];
            sparse.scale_keys[frame * sparse.bones.size() + bone] = coupled.scale_keys[bone];
            if (frame && bone == 1)
                sparse.keys[frame * sparse.bones.size() + bone].rotation =
                    coupled.keys[coupled.bones.size() + bone].rotation;
        }
    sk::FbxWriteOptions bounded;
    bounded.include_mesh = false;
    bounded.limits.decoded_bytes = 512 * 1024;
    const auto sparse_path = directory / "sparse-bounded.fbx";
    std::filesystem::remove(sparse_path);
    const auto sparse_bake = write_fbx_traced(sparse, 0, sparse_path, &m, bounded);
    check(sparse_bake.audited_world_matrix_error <= 0.0005, "Sparse bake exceeded accuracy contract");
    const auto sparse_back = read_fbx_traced(sparse_path);
    for (double frame : {0.137, 0.381966, 0.731, 50.5, 100.0}) {
        const auto seconds = frame / sparse.sequences[0].fps;
        const auto expected = sk::evaluate(sparse, 0, seconds);
        const auto actual = sparse_back.evaluate(0, std::min(seconds, sparse_back.data().clips[0].end));
        for (std::size_t bone = 0; bone < sparse.bones.size(); ++bone) {
            const auto &nodes = sparse_back.data().nodes;
            const auto node = std::find_if(nodes.begin(), nodes.end(), [&](const auto &n) {
                return n.source.original_name == sparse.bones[bone].source.original_name;
            });
            check(node != nodes.end(), "Sparse bone missing");
            for (std::size_t k = 0; k < 16; ++k)
                near(expected.world[bone][k], actual.world[static_cast<std::size_t>(node - nodes.begin())][k], 0.0005);
        }
    }
    near(bake.source_fps, coupled.sequences[0].fps);
    auto coupled_editing_rate = std::max(1.0, std::round(coupled.sequences[0].fps));
    while (coupled_editing_rate < bake.bake_fps * 0.02) coupled_editing_rate *= 2;
    near(coupled_back.data().fps, coupled_editing_rate);
    check(coupled_back.data().fps < bake.bake_fps, "Bake frequency leaked into timeline");
    near(coupled_back.data().clips[0].end, coupled.sequences[0].time.source_end_seconds);
    auto fractional = coupled;
    fractional.sequences[0].fps = 30.428571701049805;
    fractional.sequences[0].time.source_end_seconds = fractional.sequences[0].frame_count / fractional.sequences[0].fps;
    const auto fractional_path = directory / "fractional-source-rate.fbx";
    std::filesystem::remove(fractional_path);
    const auto fractional_bake = write_fbx_traced(fractional, 0, fractional_path, &m);
    const auto fractional_back = read_fbx_traced(fractional_path);
    auto fractional_editing_rate = 30.0;
    while (fractional_editing_rate < fractional_bake.bake_fps * 0.02) fractional_editing_rate *= 2;
    near(fractional_back.data().fps, fractional_editing_rate);
    near(fractional_bake.source_fps, fractional.sequences[0].fps);
    near(fractional_back.data().clips[0].end, fractional.sequences[0].time.source_end_seconds);
    auto collapsed = coupled;
    collapsed.sequences[0].fps = 1e30;
    collapsed.sequences[0].time.source_end_seconds = 2e-30;
    reject([&] { write_fbx_traced(collapsed, 0, directory / "time-collision.fbx", &m); },
           formaview::ErrorCode::Unsupported);
    auto excessive_duration = coupled;
    excessive_duration.sequences[0].fps = 1e-30;
    excessive_duration.sequences[0].time.source_end_seconds = 2e30;
    reject([&] { write_fbx_traced(excessive_duration, 0, directory / "time-overflow.fbx", &m); },
           formaview::ErrorCode::Unsupported);
    auto excessive_value = coupled;
    for (auto &animation_key : excessive_value.keys)
        animation_key.translation.x = 1e100;
    reject([&] { write_fbx_traced(excessive_value, 0, directory / "curve-overflow.fbx"); },
           formaview::ErrorCode::Unsupported);
    auto non_ascii = animation();
    const auto sequence_name = offset(non_ascii, "ANIMINFO");
    non_ascii[sequence_name] = std::byte{0xE8};
    non_ascii[sequence_name + 1] = std::byte{0xB5};
    non_ascii[sequence_name + 2] = std::byte{0xB0};
    non_ascii[sequence_name + 3] = std::byte{};
    const auto preserved_name = sk::read_actor(non_ascii, o);
    check(preserved_name.sequences[0].source.original_name == "\xE8\xB5\xB0",
          "Non-ASCII source name bytes changed");
    reject([&] { write_fbx_traced(preserved_name, 0, directory / "unsupported-name.fbx", &m); },
           formaview::ErrorCode::Unsupported);
    sk::FbxWriteOptions bad_subdivisions;
    bad_subdivisions.max_sample_subdivisions = 3;
    reject([&] { write_fbx_traced(coupled, 0, directory / "bad-bake-options.fbx", &m, bad_subdivisions); },
           formaview::ErrorCode::InvalidArgument);
    for (double fraction : {0.137, 0.3819660112501051, 0.731}) {
        const auto seconds = fraction / coupled.sequences[0].fps;
        const auto expected = sk::evaluate(coupled, 0, seconds);
        const auto actual = coupled_back.evaluate(0, seconds);
        for (std::size_t j = 0; j < coupled.bones.size(); ++j) {
            const auto &nodes = coupled_back.data().nodes;
            const auto it = std::find_if(nodes.begin(), nodes.end(), [&](const auto &node) {
                return node.source.original_name == coupled.bones[j].source.original_name;
            });
            check(it != nodes.end(), "Refined bone missing");
            for (std::size_t k = 0; k < 16; ++k)
                near(expected.world[j][k], actual.world[static_cast<std::size_t>(it - nodes.begin())][k],
                     0.0005);
        }
    }
    const auto invalid = directory / "malformed.fbx";
    {
        std::ofstream file(invalid, std::ios::binary);
        file << "Kaydara FBX Binary invalid";
    }
    reject([&] { (void)read_fbx_traced(invalid); });
    auto negative = m;
    negative.mesh->weights[0].value = -0.25;
    reject([&] { write_fbx_traced(a, 0, directory / "negative.fbx", &negative); },
           formaview::ErrorCode::Unsupported);
    const auto unknown = sk::read_actor(animation());
    reject([&] { write_fbx_traced(unknown, 0, directory / "unknown-axes.fbx"); },
           formaview::ErrorCode::Unsupported);
    for (const auto preset : {sk::FbxAxisPreset::Maya, sk::FbxAxisPreset::Houdini,
                             sk::FbxAxisPreset::Unreal, sk::FbxAxisPreset::Max,
                             sk::FbxAxisPreset::Cinema4D, sk::FbxAxisPreset::Blender,
                             sk::FbxAxisPreset::Unity}) {
        sk::FbxWriteOptions converted;
        converted.output_axes = preset;
        converted.output_metres_per_unit = 1;
        const std::array<std::size_t, 2> clips{0, 1};
        const auto encoded = sk::encode_fbx_clips(a, clips, &m, converted);
        const auto file = directory / ("axes-" + std::to_string(static_cast<int>(preset)) + ".fbx");
        std::ofstream stream(file, std::ios::binary);
        stream.write(reinterpret_cast<const char *>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        stream.close();
        const auto back = read_fbx_traced(file);
        const auto &d = back.data();
        check(d.meshes.size() == 1 && d.clips.size() == 2, "Coordinate conversion changed scene counts");
        near(d.metres_per_unit, 1);
        const bool z_up = preset == sk::FbxAxisPreset::Unreal || preset == sk::FbxAxisPreset::Max || preset == sk::FbxAxisPreset::Blender;
        const bool left = preset == sk::FbxAxisPreset::Unreal || preset == sk::FbxAxisPreset::Cinema4D || preset == sk::FbxAxisPreset::Unity;
        check(d.source.coordinates.up == (z_up ? formaview::Axis::PositiveZ : formaview::Axis::PositiveY) &&
              d.source.coordinates.handedness == (left ? formaview::Handedness::Left : formaview::Handedness::Right), "Target coordinate metadata differs");
        const auto transformed = [&](const std::array<double, 3> &p) {
            if (preset == sk::FbxAxisPreset::Unreal) return std::array<double, 3>{p[0] * .01, -p[1] * .01, p[2] * .01};
            if (z_up) return std::array<double, 3>{-p[1] * .01, p[0] * .01, p[2] * .01};
            return std::array<double, 3>{-p[1] * .01, p[2] * .01, (left ? p[0] : -p[0]) * .01};
        };
        const auto point = [](const formaview::Matrix4 &matrix, const std::array<double, 3> &p) {
            std::array<double, 3> result{};
            for (std::size_t row = 0; row < 3; ++row)
                result[row] = matrix[row * 4] * p[0] + matrix[row * 4 + 1] * p[1] + matrix[row * 4 + 2] * p[2] + matrix[row * 4 + 3];
            return result;
        };
        const auto &mesh_back = d.meshes[0];
        std::size_t weight_count{};
        for (const auto &c : mesh_back.skins[0].clusters) weight_count += c.weights.size();
        check(weight_count == m.mesh->weights.size(), "Coordinate conversion lost influences");
        for (std::size_t clip = 0; clip < a.sequences.size(); ++clip) {
            for (const auto fraction : {0.0, .25, .5, .75, 1.0}) {
                const auto seconds = a.sequences[clip].time.source_end_seconds * fraction;
                const auto source_pose = sk::evaluate(a, clip, seconds);
                const auto output_pose = back.evaluate(clip, std::min(seconds, d.clips[clip].end));
                for (std::size_t j = 0; j < a.bones.size(); ++j) {
                    const auto it = std::find_if(d.nodes.begin(), d.nodes.end(), [&](const auto &n) {
                        return n.bone && n.source.original_name == a.bones[j].source.original_name;
                    });
                    check(it != d.nodes.end(), "Coordinate conversion lost bone");
                    const auto expected = transformed(point(source_pose.world[j], {0, 0, 0}));
                    const auto actual = point(output_pose.world[static_cast<std::size_t>(it - d.nodes.begin())], {0, 0, 0});
                    for (std::size_t c = 0; c < 3; ++c) near(actual[c], expected[c], 1e-5);
                }
                // Compare normalized skinned source positions using every raw influence.
                for (std::size_t p = 0; p < m.mesh->points.size(); ++p) {
                    std::array<double, 3> actual{}, expected{}, vertex{};
                    std::memcpy(vertex.data(), mesh_back.geometry.positions.bytes.data() + p * 3 * sizeof(double), 3 * sizeof(double));
                    double total{};
                    for (const auto &w : m.mesh->weights) if (w.point == p) {
                        const auto v = m.mesh->points[p];
                        const auto skinned = point(sk::multiply(source_pose.world[w.bone], sk::inverse(*m.bones[w.bone].bind_world)), {v.x, v.y, v.z});
                        for (std::size_t c = 0; c < 3; ++c) expected[c] += w.value * skinned[c];
                        total += w.value;
                    }
                    if (total == 0) continue;
                    for (auto &v : expected) v /= total;
                    expected = transformed(expected);
                    for (const auto &cluster : mesh_back.skins[0].clusters)
                        for (std::size_t w = 0; w < cluster.points.size(); ++w) if (cluster.points[w] == p) {
                            const auto skinned = point(sk::multiply(output_pose.world[cluster.bone_node], cluster.geometry_to_bone), vertex);
                            for (std::size_t c = 0; c < 3; ++c) actual[c] += cluster.weights[w] * skinned[c] / total;
                        }
                    for (std::size_t c = 0; c < 3; ++c) near(actual[c], expected[c], 1e-5);
                }
            }
        }
    }
    auto long_clip = a;
    long_clip.sequences.resize(1);
    long_clip.sequences[0].frame_count = 8000;
    long_clip.sequences[0].time.source_end_seconds = 8000 / long_clip.sequences[0].fps;
    long_clip.keys.resize(8000 * 7, a.keys[0]);
    long_clip.scale_keys.resize(8000 * 7, a.scale_keys[0]);
    const auto cancelled_path = directory / "mid-cancel.fbx";
    std::stop_source source;
    sk::FbxWriteOptions cancel_options;
    cancel_options.stop = source.get_token();
    auto result = std::async(std::launch::async, [&] {
        try {
            write_fbx_traced(long_clip, 0, cancelled_path, nullptr, cancel_options);
            return false;
        } catch (const sk::Failure &e) {
            return e.error.code == formaview::ErrorCode::Cancelled;
        }
    });
    bool started = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline &&
           result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        for (const auto &entry : std::filesystem::directory_iterator(directory))
            if (entry.path().filename().string().starts_with("mid-cancel.fbx.formaview-"))
                started = true;
        if (started)
            break;
        std::this_thread::yield();
    }
    source.request_stop();
    check(started, "Did not observe an active SDK writer temporary output");
    check(result.wait_for(std::chrono::seconds(5)) == std::future_status::ready && result.get(),
          "Active SDK export did not cancel");
    check(!std::filesystem::exists(cancelled_path), "Cancelled SDK export published an output");
    for (const auto &entry : std::filesystem::directory_iterator(directory))
        check(!entry.path().filename().string().starts_with("mid-cancel.fbx.formaview-"),
              "Cancelled SDK export leaked its temporary file");
}
#endif
} // namespace
int main(int argc, char **argv) {
    try {
        check(argc >= 2, "Expected test case");
        const std::string arg = argv[1];
#ifdef FORMAVIEW_TEST_FBX
        if (arg == "fbx") {
            check(argc == 3, "FBX test requires external artifact directory");
            fbx(argv[2]);
            return 0;
        }
#endif
        if (arg == "reading")
            reading();
        else if (arg == "pose")
            pose();
        else if (arg == "errors")
            errors();
        else if (arg == "pairing")
            pairing();
        else if (arg == "limits")
            limits();
        else
            throw std::runtime_error("Unknown test case");
        std::cout << arg << " passed (synthetic boundary/numeric fixtures)\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
