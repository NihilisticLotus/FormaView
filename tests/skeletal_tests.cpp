#include "formaview/skeletal.hpp"
#include <bit>
#include <cmath>
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
void fbx(const std::filesystem::path &directory) {
    std::filesystem::create_directories(directory);
    sk::ReadOptions o;
    o.coordinates = {formaview::Axis::PositiveZ, formaview::Axis::PositiveX, formaview::Handedness::Right,
                     0.01, "Synthetic numeric fixture"};
    const auto a = sk::read_actor(animation(), o), m = sk::read_actor(mesh(), o);
    for (std::size_t clip = 0; clip < a.sequences.size(); ++clip) {
        const auto path = clip == 0 ? directory / std::filesystem::path(u8"骨骼动画-0.fbx")
                                    : directory / ("synthetic-" + std::to_string(clip) + ".fbx");
        std::filesystem::remove(path);
        sk::write_fbx_clip(a, clip, path, &m);
        const auto f = sk::FbxDocument::read(path);
        const auto &d = f.data();
        check(d.clips.size() == 1 && d.meshes.size() == 1, "FBX scene counts");
        check(d.source.coordinates.up == formaview::Axis::PositiveZ &&
                  d.source.coordinates.forward == formaview::Axis::PositiveX &&
                  d.source.coordinates.handedness == formaview::Handedness::Right,
              "FBX source axes lost");
        near(d.fps, a.sequences[clip].fps);
        near(d.clips[0].end, a.sequences[clip].time.source_end_seconds);
        for (int frame = 0; frame < a.sequences[clip].frame_count; ++frame) {
            const auto seconds = static_cast<double>(frame) / d.fps;
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
        reject([&] { sk::write_fbx_clip(a, clip, path, &m); }, formaview::ErrorCode::Conflict);
        sk::ReadOptions tiny;
        tiny.limits.decoded_bytes = 1;
        reject([&] { (void)sk::FbxDocument::read(path, tiny); }, formaview::ErrorCode::ResourceLimit);
        std::stop_source stop;
        stop.request_stop();
        sk::ReadOptions cancelled;
        cancelled.stop = stop.get_token();
        reject([&] { (void)sk::FbxDocument::read(path, cancelled); }, formaview::ErrorCode::Cancelled);
        sk::FbxWriteOptions small;
        small.limits.decoded_bytes = 1;
        reject([&] { sk::write_fbx_clip(a, clip, directory / "must-not-exist.fbx", nullptr, small); },
               formaview::ErrorCode::ResourceLimit);
        sk::FbxWriteOptions stopped;
        stopped.stop = stop.get_token();
        reject([&] { sk::write_fbx_clip(a, clip, directory / "must-not-exist.fbx", nullptr, stopped); },
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
            sk::write_fbx_clip(crossing, 0, path, &reference, write);
            const auto f = sk::FbxDocument::read(path);
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
    // Coupled-axis rotation cannot be represented by linear Euler curves at the
    // source rate. Refinement must meet the world-space tolerance off the bake grid.
    auto coupled = a;
    coupled.sequences.resize(1);
    coupled.sequences[0].frame_count = 2;
    coupled.sequences[0].time.source_end_seconds = 2 / coupled.sequences[0].fps;
    coupled.keys[1].rotation = {0, 0, 0, 1};
    coupled.keys[coupled.bones.size() + 1].rotation = {-0.3, -0.4, -0.2, 0.8};
    sk::FbxWriteOptions unrefined;
    unrefined.max_sample_subdivisions = 1;
    const auto too_coarse = directory / "coupled-coarse.fbx";
    std::filesystem::remove(too_coarse);
    reject([&] { sk::write_fbx_clip(coupled, 0, too_coarse, &m, unrefined); },
           formaview::ErrorCode::Unsupported);
    check(!std::filesystem::exists(too_coarse), "Rejected coarse bake left output");
    const auto refined = directory / "coupled-refined.fbx";
    std::filesystem::remove(refined);
    const auto bake = sk::write_fbx_clip(coupled, 0, refined, &m);
    check(bake.sample_subdivisions > 1 && bake.audited_world_matrix_error <= 0.0005,
          "Coupled rotation was not refined");
    const auto coupled_back = sk::FbxDocument::read(refined);
    near(bake.source_fps, coupled.sequences[0].fps);
    near(coupled_back.data().fps, bake.bake_fps);
    near(coupled_back.data().clips[0].end, coupled.sequences[0].time.source_end_seconds);
    auto collapsed = coupled;
    collapsed.sequences[0].fps = 1e30;
    collapsed.sequences[0].time.source_end_seconds = 2e-30;
    reject([&] { sk::write_fbx_clip(collapsed, 0, directory / "time-collision.fbx", &m); },
           formaview::ErrorCode::Unsupported);
    auto excessive_duration = coupled;
    excessive_duration.sequences[0].fps = 1e-30;
    excessive_duration.sequences[0].time.source_end_seconds = 2e30;
    reject([&] { sk::write_fbx_clip(excessive_duration, 0, directory / "time-overflow.fbx", &m); },
           formaview::ErrorCode::Unsupported);
    auto excessive_value = coupled;
    for (auto &animation_key : excessive_value.keys)
        animation_key.translation.x = 1e100;
    reject([&] { sk::write_fbx_clip(excessive_value, 0, directory / "curve-overflow.fbx"); },
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
    reject([&] { sk::write_fbx_clip(preserved_name, 0, directory / "unsupported-name.fbx", &m); },
           formaview::ErrorCode::Unsupported);
    sk::FbxWriteOptions bad_subdivisions;
    bad_subdivisions.max_sample_subdivisions = 3;
    reject([&] { sk::write_fbx_clip(coupled, 0, directory / "bad-bake-options.fbx", &m, bad_subdivisions); },
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
    reject([&] { (void)sk::FbxDocument::read(invalid); });
    auto negative = m;
    negative.mesh->weights[0].value = -0.25;
    reject([&] { sk::write_fbx_clip(a, 0, directory / "negative.fbx", &negative); },
           formaview::ErrorCode::Unsupported);
    const auto unknown = sk::read_actor(animation());
    reject([&] { sk::write_fbx_clip(unknown, 0, directory / "unknown-axes.fbx"); },
           formaview::ErrorCode::Unsupported);
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
            sk::write_fbx_clip(long_clip, 0, cancelled_path, nullptr, cancel_options);
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
