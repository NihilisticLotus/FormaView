#include "formaview/fbx.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
namespace sk = formaview::skeletal;
namespace {
std::string quoted(std::string_view s) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 32) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else
            out += static_cast<char>(c);
    }
    return out + '"';
}
void matrices(std::ostream &out, const std::vector<formaview::Matrix4> &values) {
    out << '[';
    bool first = true;
    for (const auto &m : values) {
        if (!first)
            out << ',';
        first = false;
        out << '[';
        for (std::size_t i = 0; i < 16; ++i) {
            if (i)
                out << ',';
            out << m[i];
        }
        out << ']';
    }
    out << ']';
}
double difference(const formaview::Matrix4 &a, const formaview::Matrix4 &b) {
    double error{};
    for (std::size_t k = 0; k < 16; ++k)
        error = std::max(error, std::abs(a[k] - b[k]));
    return error;
}
sk::ReadOptions options() {
    sk::ReadOptions o;
    o.coordinates = {formaview::Axis::PositiveZ, formaview::Axis::PositiveX, formaview::Handedness::Right,
                     0.01, "Explicit candidate ActorX numeric convention, awaiting DCC validation"};
    return o;
}
void snapshot(const sk::FbxDocument &f, std::ostream &out) {
    const auto &d = f.data();
    out << "{\"version\":" << d.version << ",\"ascii\":" << (d.ascii ? "true" : "false")
        << ",\"unit\":" << d.metres_per_unit << ",\"fps\":" << d.fps << ",\"nodes\":" << d.nodes.size()
        << ",\"meshes\":" << d.meshes.size() << ",\"clips\":[";
    for (std::size_t i = 0; i < d.clips.size(); ++i) {
        if (i)
            out << ',';
        const auto &c = d.clips[i];
        out << "{\"name\":" << quoted(c.source.original_name) << ",\"start\":" << c.start
            << ",\"end\":" << c.end << '}';
    }
    out << "]}";
}
bool verify_clip(const std::filesystem::path &source, const std::filesystem::path &reference,
                 const std::filesystem::path &companion, const std::filesystem::path &output,
                 std::size_t clip, std::ostream &out) {
    const auto a = sk::read_actor(source, options());
    const auto m = sk::read_actor(reference, options());
    const auto pairing = sk::pair(a, m, 1e-4, sk::PairingPolicy::ReferenceSkeleton);
    if (!pairing.compatible || clip >= a.sequences.size())
        throw std::runtime_error("Invalid animation reference or clip");
    const auto size = std::filesystem::file_size(companion);
    if (size > options().limits.input_bytes)
        throw std::runtime_error("Config budget exceeded");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::ifstream file(companion, std::ios::binary);
    if (!file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size)))
        throw std::runtime_error("Cannot read config");
    const auto config = sk::read_psa_config(bytes, a, options());
    const auto f = sk::FbxDocument::read(output);
    const auto &d = f.data();
    const auto &seq = a.sequences[clip];
    if (d.clips.size() != 1 || d.clips[0].source.original_name != seq.source.original_name ||
        std::abs(d.clips[0].start - seq.time.source_start_seconds) > 1e-8 ||
        std::abs(d.clips[0].end - seq.time.source_end_seconds) > 1e-8)
        throw std::runtime_error("Exported clip identity or timing differs");
    std::map<std::string, std::size_t> nodes;
    for (std::size_t i = 0; i < d.nodes.size(); ++i)
        if (d.nodes[i].bone) nodes.emplace(d.nodes[i].source.original_name, i);
    if (nodes.size() != a.bones.size()) throw std::runtime_error("Exported bone count differs");
    double error{}, config_effect{};
    for (std::int64_t frame = 0; frame <= static_cast<std::int64_t>(seq.frame_count) * 2; ++frame) {
        const auto seconds = std::min(frame * .5 / seq.fps, seq.time.source_end_seconds);
        const auto expected = sk::evaluate_paired(a, pairing, clip, seconds, &config);
        const auto unmasked = sk::evaluate_paired(a, pairing, clip, seconds);
        const auto actual = f.evaluate(0, std::clamp(seconds, d.clips[0].start, d.clips[0].end));
        for (std::size_t j = 0; j < a.bones.size(); ++j) {
            const auto &world = actual.world.at(nodes.at(a.bones[j].source.original_name));
            for (const auto v : world)
                if (!std::isfinite(v)) throw std::runtime_error("Nonfinite exported matrix");
            error = std::max(error, difference(world, expected.world[j]));
            config_effect = std::max(config_effect, difference(unmasked.world[j], expected.world[j]));
        }
    }
    out << "{\"passed\":" << (error < 1e-3 ? "true" : "false")
        << ",\"world_matrix_error\":" << error << ",\"config_effect\":" << config_effect
        << ",\"mesh_count\":" << d.meshes.size() << ",\"config_sha256\":"
        << quoted(config.source.identity.sha256) << '}';
    return error < 1e-3;
}
bool verify_model(const sk::ActorAsset &source, const std::filesystem::path &output, std::ostream &out) {
    const auto back = sk::FbxDocument::read(output);
    const auto &d = back.data();
    const auto &m = *source.mesh;
    bool ok = d.clips.empty() && d.meshes.size() == 1;
    double error{}, geometry_error{}, bind_error{}, default_error{};
    auto *category_error = &geometry_error;
    const auto compare = [&](double a, double b) {
        if (!std::isfinite(a) || !std::isfinite(b)) { ok = false; return; }
        error = std::max(error, std::abs(a - b));
        *category_error = std::max(*category_error, std::abs(a - b));
    };
    const auto scalar = [](const formaview::Buffer &b, std::size_t i) {
        if (b.scalar != formaview::ScalarType::Float64 || (i + 1) * sizeof(double) > b.bytes.size())
            throw std::runtime_error("Invalid independent readback buffer");
        double v{};
        std::memcpy(&v, b.bytes.data() + i * sizeof(double), sizeof(v));
        return v;
    };
    if (!ok) throw std::runtime_error("Pure model output has unexpected mesh/animation count");
    const auto &mesh = d.meshes[0];
    const auto &g = mesh.geometry;
    ok = g.positions.element_count == m.points.size() && g.face_sizes.size() == m.faces.size();
    if (!ok) throw std::runtime_error("Pure model geometry counts differ");
    for (std::size_t p = 0; p < m.points.size(); ++p) {
        compare(scalar(g.positions, p * 3), m.points[p].x);
        compare(scalar(g.positions, p * 3 + 1), m.points[p].y);
        compare(scalar(g.positions, p * 3 + 2), m.points[p].z);
    }
    for (std::size_t f = 0; f < m.faces.size(); ++f) {
        ok = ok && g.face_sizes[f] == 3 && mesh.face_materials[f] == m.faces[f].material;
        for (std::size_t c = 0; c < 3; ++c) {
            const auto wedge = m.faces[f].wedges[2 - c];
            const auto point = m.wedges[wedge].point;
            const auto corner = f * 3 + c;
            ok = ok && g.corner_points[corner] == point;
            const auto &uv0 = g.corner_attributes.at("uv/0/UV0");
            compare(scalar(uv0, corner * 2), m.wedges[wedge].uv.x);
            compare(scalar(uv0, corner * 2 + 1), 1 - m.wedges[wedge].uv.y);
            std::size_t set = 1;
            for (const auto &[name, values] : m.extra_uvs) {
                const auto &uv = g.corner_attributes.at("uv/" + std::to_string(set++) + "/" + name);
                compare(scalar(uv, corner * 2), values[wedge].x);
                compare(scalar(uv, corner * 2 + 1), 1 - values[wedge].y);
            }
            if (!m.normals.empty()) {
                const auto &ns = g.corner_attributes.at("normal");
                compare(scalar(ns, corner * 3), m.normals[point].x);
                compare(scalar(ns, corner * 3 + 1), m.normals[point].y);
                compare(scalar(ns, corner * 3 + 2), m.normals[point].z);
            }
        }
    }
    category_error = &default_error;
    std::map<std::string, std::size_t> nodes;
    for (std::size_t i = 0; i < d.nodes.size(); ++i)
        if (d.nodes[i].bone) nodes.emplace(d.nodes[i].source.original_name, i);
    ok = ok && nodes.size() == source.bones.size();
    for (const auto &bone : source.bones) {
        const auto &node = d.nodes.at(nodes.at(bone.source.original_name));
        for (std::size_t k = 0; k < 16; ++k) compare(node.default_world[k], (*bone.bind_world)[k]);
        if (bone.parent) ok = ok && node.parent == nodes.at(source.bones[*bone.parent].source.original_name);
    }
    using WeightKey = std::tuple<std::string, std::uint32_t, double>;
    std::vector<WeightKey> expected, actual;
    for (const auto &weight : m.weights)
        expected.emplace_back(source.bones[weight.bone].source.original_name, weight.point, weight.value);
    category_error = &bind_error;
    for (const auto &skin : mesh.skins)
        for (const auto &cluster : skin.clusters) {
            const auto &name = d.nodes[cluster.bone_node].source.original_name;
            const auto bone = std::find_if(source.bones.begin(), source.bones.end(), [&](const auto &b) { return b.source.original_name == name; });
            if (bone == source.bones.end()) throw std::runtime_error("Unexpected output cluster");
            for (std::size_t k = 0; k < 16; ++k) compare(cluster.bind_to_world[k], (*bone->bind_world)[k]);
            for (std::size_t j = 0; j < cluster.points.size(); ++j)
                actual.emplace_back(name, cluster.points[j], cluster.weights[j]);
        }
    std::sort(expected.begin(), expected.end()); std::sort(actual.begin(), actual.end());
    // Match the established ActorX roundtrip tolerances by semantic domain.
    ok = ok && expected == actual && geometry_error < 1e-8 && bind_error < 1e-4 && default_error < 1e-4;
    out << "{\"passed\":" << (ok ? "true" : "false") << ",\"max_error\":" << error
        << ",\"geometry_error\":" << geometry_error << ",\"bind_error\":" << bind_error << ",\"default_error\":" << default_error
        << ",\"points\":" << m.points.size() << ",\"faces\":" << m.faces.size()
        << ",\"weights\":" << actual.size() << ",\"bones\":" << nodes.size()
        << ",\"uv_sets\":" << m.extra_uvs.size() + 1 << ",\"source_sha256\":" << quoted(source.source.identity.sha256)
        << ",\"output_sha256\":" << quoted(d.source.identity.sha256) << '}';
    return ok;
}
bool roundtrip(const sk::ActorAsset &a, const sk::ActorAsset *m, std::size_t clip,
               const std::filesystem::path &output, std::ostream &out,
               const sk::FbxWriteOptions &options = {}) {
    const auto bake = sk::write_fbx_clip(a, clip, output, m, options);
    const auto binding = m ? sk::pair(a, *m, 1e-4, options.pairing_policy) : sk::Pairing{};
    const auto pose_at = [&](double seconds) {
        return m ? sk::evaluate_paired(a, binding, clip, seconds, options.psa_config)
                 : sk::evaluate(a, clip, seconds);
    };
    sk::ReadOptions read_options;
    read_options.limits = options.limits;
    read_options.stop = options.stop;
    const auto f = sk::FbxDocument::read(output, read_options);
    const auto &d = f.data();
    const auto &s = a.sequences[clip];
    std::map<std::string, std::uint32_t> names;
    for (std::size_t i = 0; i < d.nodes.size(); ++i)
        if (d.nodes[i].bone)
            names.emplace(d.nodes[i].source.original_name, static_cast<std::uint32_t>(i));
    bool ok =
        d.clips.size() == 1 && names.size() == a.bones.size() && std::abs(d.fps - bake.bake_fps) < 1e-8 &&
        d.clips[0].source.original_name == s.source.original_name && std::abs(d.clips[0].start) < 1e-9 &&
        std::abs(d.clips[0].end - s.time.source_end_seconds) < 1e-8;
    for (const auto &b : a.bones)
        if (!names.contains(b.source.original_name))
            ok = false;
    out << "{\"clip\":" << clip << ",\"mesh\":" << (m && options.include_mesh ? "true" : "false")
        << ",\"output_fbx_sha256\":" << quoted(d.source.identity.sha256)
        << ",\"bake\":{\"subdivisions\":" << bake.sample_subdivisions << ",\"source_fps\":" << bake.source_fps
        << ",\"bake_fps\":" << bake.bake_fps << ",\"samples\":" << bake.samples
        << ",\"audited_world_matrix_error\":" << bake.audited_world_matrix_error << '}' << ",\"fbx\":";
    snapshot(f, out);
    double local_error{}, world_error{}, root_error{}, subframe_error{};
    out << ",\"bone_names\":[";
    for (std::size_t j = 0; j < a.bones.size(); ++j) {
        if (j)
            out << ',';
        out << quoted(a.bones[j].source.original_name);
    }
    out << "],\"defaults_world\":";
    std::vector<formaview::Matrix4> defaults;
    for (const auto &b : a.bones)
        defaults.push_back(d.nodes[names.at(b.source.original_name)].default_world);
    matrices(out, defaults);
    out << ",\"samples\":[";
    std::set<std::int32_t> frames{0, s.frame_count / 4, s.frame_count / 2, s.frame_count - 1};
    if (s.frame_count <= 256)
        for (std::int32_t frame = 0; frame < s.frame_count; ++frame)
            frames.insert(frame);
    bool first = true;
    for (auto frame : frames) {
        const auto seconds = static_cast<double>(frame) / s.fps;
        const auto pose = pose_at(seconds);
        const auto back = f.evaluate(0, seconds);
        if (!first)
            out << ',';
        first = false;
        out << "{\"time\":" << seconds << ",\"world\":";
        matrices(out, pose.world);
        out << ",\"local\":[";
        for (std::size_t j = 0; j < a.bones.size(); ++j) {
            if (j)
                out << ',';
            const auto lm = sk::matrix(pose.local[j]);
            out << '[';
            for (std::size_t k = 0; k < 16; ++k) {
                if (k)
                    out << ',';
                out << lm[k];
            }
            out << ']';
            if (names.contains(a.bones[j].source.original_name)) {
                const auto index = names.at(a.bones[j].source.original_name);
                local_error = std::max(local_error, difference(lm, back.local[index]));
                world_error = std::max(world_error, difference(pose.world[j], back.world[index]));
                const auto parent = m ? binding.parents[j] : a.bones[j].parent;
                if (!parent)
                    root_error = std::max(root_error, difference(pose.world[j], back.world[index]));
                if (parent)
                    ok = ok && d.nodes[index].parent == names.at(a.bones[*parent].source.original_name);
            }
        }
        out << "]}";
    }
    std::set<std::int32_t> half_frames;
    if (s.frame_count > 1) {
        half_frames.insert(0);
        if (s.frame_count <= 256)
            for (std::int32_t frame = 0; frame < s.frame_count - 1; ++frame)
                half_frames.insert(frame);
    }
    for (const auto frame : half_frames) {
        for (const auto fraction : {0.137, 0.3819660112501051, 0.5, 0.731}) {
            const auto sec = (static_cast<double>(frame) + fraction) / s.fps;
            const auto p = pose_at(sec);
            const auto back = f.evaluate(0, sec);
            for (std::size_t j = 0; j < a.bones.size(); ++j)
                if (names.contains(a.bones[j].source.original_name))
                    subframe_error = std::max(
                        subframe_error,
                        difference(p.world[j], back.world[names.at(a.bones[j].source.original_name)]));
        }
    }
    out << "],\"max_local_error\":" << local_error << ",\"max_world_error\":" << world_error
        << ",\"max_root_error\":" << root_error << ",\"subframe_world_error\":" << subframe_error;
    ok = ok && local_error < 1e-3 && world_error < 1e-3 && root_error < 1e-3;
    if (m && options.include_mesh) {
        const auto &src = *m->mesh;
        bool mesh_ok = d.meshes.size() == 1;
        double bind_error{}, weight_error{}, position_error{}, uv_error{};
        std::uint64_t actual_weights{};
        std::size_t uv_sets{};
        if (mesh_ok) {
            const auto &bm = d.meshes[0];
            const auto &g = bm.geometry;
            mesh_ok = g.positions.element_count == src.points.size() &&
                      g.face_sizes.size() == src.faces.size() &&
                      g.corner_points.size() == src.faces.size() * 3 && bm.skins.size() == 1;
            for (const auto &[name, attr] : g.corner_attributes)
                if (name.starts_with("uv/")) {
                    (void)attr;
                    ++uv_sets;
                }
            mesh_ok = mesh_ok && uv_sets == src.extra_uvs.size() + 1;
            auto number = [](const formaview::Buffer &b, std::size_t index) {
                double x{};
                std::memcpy(&x, b.bytes.data() + index * sizeof(double), sizeof(double));
                return x;
            };
            if (g.positions.element_count == src.points.size())
                for (std::size_t p = 0; p < src.points.size(); ++p) {
                    const auto v = src.points[p];
                    position_error = std::max({position_error, std::abs(number(g.positions, p * 3) - v.x),
                                               std::abs(number(g.positions, p * 3 + 1) - v.y),
                                               std::abs(number(g.positions, p * 3 + 2) - v.z)});
                }
            for (const auto &[name, attr] : g.corner_attributes)
                if (name.starts_with("uv/")) {
                    const std::vector<sk::Vec2> *extra = nullptr;
                    const auto slash = name.find('/', 3);
                    const auto setname = name.substr(slash + 1);
                    if (setname != "UV0") {
                        const auto it = src.extra_uvs.find(setname);
                        if (it == src.extra_uvs.end()) {
                            mesh_ok = false;
                            continue;
                        }
                        extra = &it->second;
                    }
                    for (std::size_t face = 0; face < src.faces.size(); ++face)
                        for (std::size_t corner = 0; corner < 3; ++corner) {
                            const auto w = src.faces[face].wedges[2 - corner];
                            const auto uv = extra ? (*extra)[w] : src.wedges[w].uv;
                            const auto index = face * 3 + corner;
                            uv_error = std::max({uv_error, std::abs(number(attr, index * 2) - uv.x),
                                                 std::abs(number(attr, index * 2 + 1) - (1 - uv.y))});
                            mesh_ok = mesh_ok && g.corner_points[index] == src.wedges[w].point;
                        }
                }
            if (bm.skins.size() == 1) {
                std::map<std::pair<std::string, std::uint32_t>, std::vector<double>> expected, actual;
                for (const auto &w : src.weights)
                    expected[{m->bones[w.bone].source.original_name, w.point}].push_back(w.value);
                for (const auto &c : bm.skins[0].clusters) {
                    const auto &name = d.nodes[c.bone_node].source.original_name;
                    const auto it = std::find_if(m->bones.begin(), m->bones.end(), [&](const auto &b) {
                        return b.source.original_name == name;
                    });
                    if (it == m->bones.end()) {
                        mesh_ok = false;
                        continue;
                    }
                    bind_error = std::max(bind_error, difference(*it->bind_world, c.bind_to_world));
                    bind_error =
                        std::max(bind_error, difference(sk::inverse(*it->bind_world), c.geometry_to_bone));
                    for (std::size_t k = 0; k < c.points.size(); ++k) {
                        actual[{name, c.points[k]}].push_back(c.weights[k]);
                        ++actual_weights;
                    }
                }
                mesh_ok = mesh_ok && expected.size() == actual.size() && actual_weights == src.weights.size();
                for (auto &[key, values] : expected) {
                    auto &other = actual[key];
                    std::sort(values.begin(), values.end());
                    std::sort(other.begin(), other.end());
                    if (values.size() != other.size())
                        mesh_ok = false;
                    else
                        for (std::size_t k = 0; k < values.size(); ++k)
                            weight_error = std::max(weight_error, std::abs(values[k] - other[k]));
                }
            }
        }
        mesh_ok =
            mesh_ok && bind_error < 1e-4 && weight_error < 1e-8 && position_error < 1e-8 && uv_error < 1e-8;
        ok = ok && mesh_ok;
        out << ",\"mesh_check\":{\"passed\":" << (mesh_ok ? "true" : "false")
            << ",\"source_points\":" << src.points.size() << ",\"source_faces\":" << src.faces.size()
            << ",\"source_weights\":" << src.weights.size() << ",\"readback_weights\":" << actual_weights
            << ",\"uv_sets\":" << uv_sets << ",\"bind_error\":" << bind_error
            << ",\"weight_error\":" << weight_error << ",\"position_error\":" << position_error
            << ",\"uv_error\":" << uv_error << '}';
    } else
        ok = ok && d.meshes.empty();
    out << ",\"sample_points_passed\":" << (ok ? "true" : "false")
        << ",\"half_frame_sample_count\":" << half_frames.size();
    ok = ok && subframe_error < 1e-3;
    out << ",\"passed\":" << (ok ? "true" : "false") << '}';
    return ok;
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
#else
int main(int argc, char **argv) {
#endif
    try {
        if (argc == 8 && std::filesystem::path(argv[1]) == "--verify-actor-clip") {
            std::ofstream out{std::filesystem::path(argv[7])};
            if (!out) throw std::runtime_error("Cannot open report");
            out << std::setprecision(17);
            const auto index = std::filesystem::path(argv[6]).string();
            if (index.empty() || index.find_first_not_of("0123456789") != std::string::npos)
                throw std::runtime_error("Invalid clip index");
            return verify_clip(argv[2], argv[3], argv[4], argv[5], std::stoull(index), out) ? 0 : 2;
        }
        if (argc != 5 && argc != 7 && argc != 8)
            throw std::runtime_error("Usage: skeletal_probe input.psa|input.fbx pair.psk|- output-directory "
                                     "report.json [companion.config|- reference [fbx-budget-bytes]]");
        const std::filesystem::path input = argv[1], paired = argv[2], directory = argv[3], report = argv[4];
        std::ofstream out(report);
        if (!out)
            throw std::runtime_error("Cannot open report");
        out << std::setprecision(17);
        if (input.extension() == ".fbx") {
            const auto f = sk::FbxDocument::read(input);
            snapshot(f, out);
            if (!f.data().clips.empty())
                (void)f.evaluate(0, f.data().clips[0].start);
            return 0;
        }
        const auto a = sk::read_actor(input, options());
        if (a.animation && directory == "-") {
            out << "{\"bones\":" << a.bones.size() << ",\"keys\":" << a.keys.size()
                << ",\"scale_keys\":" << a.scale_keys.size() << ",\"clips\":[";
            for (std::size_t i = 0; i < a.sequences.size(); ++i) {
                if (i)
                    out << ',';
                const auto &s = a.sequences[i];
                out << "{\"name\":" << quoted(s.source.original_name)
                    << ",\"first_raw_frame\":" << s.first_raw_frame << ",\"frames\":" << s.frame_count
                    << ",\"fps\":" << s.fps << ",\"start\":" << s.time.source_start_seconds
                    << ",\"end_exclusive\":" << s.time.source_end_seconds << '}';
                (void)sk::evaluate(a, i, 0);
            }
            out << "],\"warnings\":[";
            for (std::size_t i = 0; i < a.warnings.size(); ++i) {
                if (i)
                    out << ',';
                out << quoted(a.warnings[i]);
            }
            out << "]}";
            return 0;
        }
        if (!a.animation) {
            if (paired != "-") return verify_model(a, paired, out) ? 0 : 2;
            const auto &m = *a.mesh;
            out << "{\"bones\":" << a.bones.size() << ",\"points\":" << m.points.size()
                << ",\"faces\":" << m.faces.size() << ",\"weights\":" << m.weights.size()
                << ",\"extra_uvs\":" << m.extra_uvs.size() << ",\"normals\":" << m.normals.size()
                << ",\"colours\":" << m.colours.size() << '}';
            return 0;
        }
        std::optional<sk::ActorAsset> m;
        if (paired != "-")
            m = sk::read_actor(paired, options());
        sk::FbxWriteOptions write_options;
        if (argc == 8) {
            const auto value = std::filesystem::path(argv[7]).string();
            if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                throw std::runtime_error("FBX budget must be a positive byte count");
            write_options.limits.decoded_bytes = std::stoull(value);
            if (!write_options.limits.decoded_bytes ||
                write_options.limits.decoded_bytes > 16ULL * 1024 * 1024 * 1024)
                throw std::runtime_error("Probe FBX budget must be at most 16 GiB");
        }
        std::optional<sk::PsaConfig> config;
        if (argc >= 7) {
            if (std::filesystem::path(argv[6]) != "reference" || !m)
                throw std::runtime_error("Explicit reference mode requires a PSK reference");
            write_options.pairing_policy = sk::PairingPolicy::ReferenceSkeleton;
            const std::filesystem::path companion = argv[5];
            if (companion != "-") {
                const auto size = std::filesystem::file_size(companion);
                if (size > options().limits.input_bytes)
                    throw std::runtime_error("Companion input budget exceeded");
                std::vector<std::byte> bytes(static_cast<std::size_t>(size));
                std::ifstream file(companion, std::ios::binary);
                if (!file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size)))
                    throw std::runtime_error("Cannot read companion input");
                config = sk::read_psa_config(bytes, a, options());
                write_options.psa_config = &*config;
            }
        }
        out << "{\"sha256\":" << quoted(a.source.identity.sha256)
            << ",\"fbx_budget_bytes\":" << write_options.limits.decoded_bytes
            << ",\"writer\":" << quoted(sk::fbx_writer_version()) << ",\"clip_count\":" << a.sequences.size();
        if (m) {
            const auto p = sk::pair(a, *m, 1e-4, write_options.pairing_policy);
            out << ",\"pairing\":{\"compatible\":" << (p.compatible ? "true" : "false")
                << ",\"reference_verified\":" << (p.reference_verified ? "true" : "false")
                << ",\"reasons\":[";
            for (std::size_t i = 0; i < p.reasons.size(); ++i) {
                if (i)
                    out << ',';
                out << quoted(p.reasons[i]);
            }
            out << "],\"uses_reference_skeleton\":" << (p.uses_reference_skeleton ? "true" : "false")
                << ",\"companion_sha256\":" << quoted(config ? config->source.identity.sha256 : "") << '}';
        }
        out << ",\"results\":[";
        bool ok = true, first = true;
        for (std::size_t clip = 0; clip < a.sequences.size(); ++clip)
            for (unsigned mode = 0; mode < (m ? 2U : 1U); ++mode) {
                if (!first)
                    out << ',';
                first = false;
                const auto file =
                    directory / ("clip-" + std::to_string(clip) + (mode ? "-mesh.fbx" : "-skeleton.fbx"));
                try {
                    std::ostringstream item;
                    item << std::setprecision(17);
                    write_options.include_mesh = mode != 0;
                    const auto reference =
                        m && (mode || write_options.pairing_policy == sk::PairingPolicy::ReferenceSkeleton)
                            ? &*m
                            : nullptr;
                    const auto passed = roundtrip(a, reference, clip, file, item, write_options);
                    out << item.str();
                    ok = passed && ok;
                } catch (const std::exception &e) {
                    out << "{\"error\":" << quoted(e.what()) << '}';
                    ok = false;
                }
            }
        out << "],\"passed\":" << (ok ? "true" : "false") << "}\n";
        std::cout << "report written; passed=" << ok << '\n';
        return ok ? 0 : 2;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
