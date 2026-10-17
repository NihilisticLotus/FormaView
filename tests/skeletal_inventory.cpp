#include "formaview/skeletal.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

namespace sk = formaview::skeletal;
namespace {
std::string quote(std::string_view value) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 32 || c >= 127) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else
            out += static_cast<char>(c);
    }
    return out + '"';
}
std::filesystem::path path(std::string_view text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(text.data()), text.size()));
}
void strings(std::ostream &out, const std::vector<std::string> &values) {
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i)
            out << ',';
        out << quote(values[i]);
    }
    out << ']';
}
void pose(std::ostream &out, const sk::Pose &p) {
    double maximum{};
    for (const auto &m : p.world)
        for (auto value : m) {
            if (!std::isfinite(value))
                throw std::runtime_error("Nonfinite evaluated matrix");
            maximum = std::max(maximum, std::abs(value));
        }
    out << "{\"bones\":" << p.world.size() << ",\"maximum_component\":" << maximum << '}';
}
sk::ReadOptions options(std::stop_token stop) {
    sk::ReadOptions result;
    result.stop = stop;
    result.coordinates = {formaview::Axis::PositiveZ, formaview::Axis::PositiveX,
                          formaview::Handedness::Right, 0.01,
                          "Caller-supplied ActorX convention; no embedded declaration"};
    return result;
}
std::vector<std::byte> bytes(const std::filesystem::path &p, std::uint64_t limit) {
    const auto n = std::filesystem::file_size(p);
    if (n > limit)
        throw sk::Failure(formaview::ErrorCode::ResourceLimit, "Companion input budget exceeded");
    std::vector<std::byte> result(static_cast<std::size_t>(n));
    std::ifstream in(p, std::ios::binary);
    if (!in.read(reinterpret_cast<char *>(result.data()), static_cast<std::streamsize>(n)))
        throw std::runtime_error("Companion read failed");
    return result;
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
#else
int main(int argc, char **argv) {
#endif
    try {
        if (argc < 3 || argc > 4)
            throw std::runtime_error("Usage: inventory manifest.tsv new-report.jsonl [stop-file]");
        const std::filesystem::path report = argv[2];
        if (std::filesystem::exists(report))
            throw std::runtime_error("Inventory report already exists");
        std::ifstream manifest{std::filesystem::path(argv[1]), std::ios::binary};
        std::ofstream out(report, std::ios::binary);
        if (!manifest || !out)
            throw std::runtime_error("Cannot open manifest/report");
        out.precision(17);
        std::stop_source stop;
        std::jthread monitor;
        if (argc == 4) {
            const std::filesystem::path stop_file = argv[3];
            monitor = std::jthread([&, stop_file](std::stop_token done) {
                while (!done.stop_requested()) {
                    std::error_code ec;
                    if (std::filesystem::exists(stop_file, ec)) {
                        stop.request_stop();
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            });
        }
        const auto read = options(stop.get_token());
        std::string cached_reference_name;
        std::optional<sk::ActorAsset> cached_reference; // At most one asset plus one reference resident.
        std::string line;
        std::size_t index{}, accepted{}, failures{};
        while (std::getline(manifest, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (stop.stop_requested()) {
                out << "{\"cancelled\":true,\"next_index\":" << index << "}\n" << std::flush;
                return 3;
            }
            const auto first = line.find('\t'), second = line.find('\t', first + 1);
            if (first == std::string::npos || second == std::string::npos)
                throw std::runtime_error("Manifest requires UTF-8 input<TAB>PSK-or--<TAB>config-or--");
            const auto source = path(std::string_view(line).substr(0, first));
            const auto reference = line.substr(first + 1, second - first - 1);
            const auto companion = line.substr(second + 1);
            const auto begin = std::chrono::steady_clock::now();
            // Buffer one record so a late pose error cannot leave invalid JSON.
            std::ostringstream record;
            record.precision(17);
            record << "{\"index\":" << index;
            try {
                const auto a = sk::read_actor(source, read);
                record << ",\"sha256\":" << quote(a.source.identity.sha256)
                       << ",\"source_bytes\":" << a.source_bytes.size() << ",\"bones\":" << a.bones.size()
                       << ",\"animation\":" << (a.animation ? "true" : "false")
                       << ",\"missing_default_records\":"
                       << std::count_if(a.bones.begin(), a.bones.end(),
                                        [](auto &b) { return !b.default_local_valid; })
                       << ",\"keys\":" << a.keys.size() << ",\"scale_keys\":" << a.scale_keys.size()
                       << ",\"warnings\":";
                strings(record, a.warnings);
                record << ",\"default_pose\":";
                try {
                    std::ostringstream result;
                    result.precision(17);
                    pose(result, sk::default_pose(a, stop.get_token()));
                    record << "{\"available\":true,\"summary\":" << result.str() << '}';
                } catch (const sk::Failure &e) {
                    if (e.error.code != formaview::ErrorCode::Unsupported)
                        throw;
                    record << "{\"available\":false,\"reason\":" << quote(e.what()) << '}';
                }
                std::optional<sk::Pairing> pairing;
                std::optional<sk::PsaConfig> config;
                if (a.animation && reference != "-") {
                    if (!cached_reference || cached_reference_name != reference) {
                        cached_reference.reset();
                        cached_reference.emplace(sk::read_actor(path(reference), read));
                        cached_reference_name = reference;
                    }
                    const auto &mesh = *cached_reference;
                    pairing = sk::pair(a, mesh, 1e-4, sk::PairingPolicy::ReferenceSkeleton);
                    record << ",\"pairing\":{\"compatible\":" << (pairing->compatible ? "true" : "false")
                           << ",\"reference_sha256\":" << quote(mesh.source.identity.sha256)
                           << ",\"reference_records_verified\":"
                           << (pairing->reference_verified ? "true" : "false") << ",\"reasons\":";
                    strings(record, pairing->reasons);
                    record << '}';
                    if (pairing->compatible && companion != "-") {
                        const auto raw = bytes(path(companion), read.limits.input_bytes);
                        config = sk::read_psa_config(raw, a, read);
                        record << ",\"config_sha256\":" << quote(config->source.identity.sha256);
                    }
                }
                if (a.mesh) {
                    const auto &m = *a.mesh;
                    std::size_t maximum{};
                    for (const auto &weights : m.point_weights)
                        maximum = std::max(maximum, weights.size());
                    record << ",\"mesh\":{\"points\":" << m.points.size() << ",\"faces\":" << m.faces.size()
                           << ",\"weights\":" << m.weights.size() << ",\"uv_sets\":" << m.extra_uvs.size() + 1
                           << ",\"max_raw_influences\":" << maximum << '}';
                }
                record << ",\"clips\":[";
                for (std::size_t clip = 0; clip < a.sequences.size(); ++clip) {
                    if (clip)
                        record << ',';
                    const auto &s = a.sequences[clip];
                    record << "{\"name_bytes\":" << quote(s.source.original_name)
                           << ",\"frames\":" << s.frame_count << ",\"first_raw_frame\":" << s.first_raw_frame
                           << ",\"fps\":" << s.fps << ",\"end\":" << s.time.source_end_seconds
                           << ",\"uniform_deltas\":" << (s.uniform_key_deltas ? "true" : "false")
                           << ",\"pose_checks\":[";
                    const auto last = static_cast<double>(s.frame_count - 1) / s.fps;
                    bool first_pose = true;
                    for (const double t : {0.0, last / 2, last, s.time.source_end_seconds}) {
                        if (!first_pose)
                            record << ',';
                        first_pose = false;
                        record << "{\"seconds\":" << t << ",\"raw\":";
                        pose(record, sk::evaluate(a, clip, t, stop.get_token()));
                        if (pairing && pairing->compatible) {
                            record << ",\"paired\":";
                            pose(record, sk::evaluate_paired(a, *pairing, clip, t,
                                                             config ? &*config : nullptr, stop.get_token()));
                        }
                        record << '}';
                    }
                    record << "]}";
                }
                record << ']';
                auto tiny = read;
                tiny.limits.decoded_bytes = 1;
                bool rejected{};
                try {
                    (void)sk::read_actor(source, tiny);
                } catch (const sk::Failure &e) {
                    rejected = e.error.code == formaview::ErrorCode::ResourceLimit;
                }
                if (!rejected)
                    throw std::runtime_error("Tiny read budget was not rejected");
                record << ",\"tiny_budget_rejected\":true,\"accepted\":true";
                ++accepted;
            } catch (const std::exception &e) {
                record.str(std::string{});
                record.clear();
                record << "{\"index\":" << index << ",\"accepted\":false,\"error\":" << quote(e.what());
                if (const auto *failure = dynamic_cast<const sk::Failure *>(&e))
                    record << ",\"error_code\":" << static_cast<int>(failure->error.code);
                ++failures;
            }
            record << ",\"elapsed_seconds\":"
                   << std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count() << '}';
            out << record.str() << '\n' << std::flush;
            if (!out)
                throw std::runtime_error("Inventory report write failed");
            ++index;
        }
        if (stop.stop_requested()) {
            out << "{\"cancelled\":true,\"next_index\":" << index << "}\n" << std::flush;
            return 3;
        }
        std::cout << "inspected=" << index << " accepted=" << accepted << " rejected=" << failures << '\n';
        return 0; // Per-file failures are evidence, not omitted from the denominator.
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
