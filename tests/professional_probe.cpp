#include "formaview/geometry_cache.hpp"
#include "formaview/preview.hpp"
#include "formaview/pro_image.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>

namespace {
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}
std::string quote(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\')
      out += '\\';
    if (static_cast<unsigned char>(c) < 32)
      out += ' ';
    else
      out += c;
  }
  return out + '"';
}
void stats(std::ostream &out, std::vector<double> times) {
  if (times.empty()) {
    out << "{\"n\":0}";
    return;
  }
  std::sort(times.begin(), times.end());
  const auto at = [&](double p) {
    return times[static_cast<std::size_t>(
                     std::ceil(p * static_cast<double>(times.size()))) -
                 1];
  };
  out << "{\"n\":" << times.size() << ",\"p95_ms\":" << at(.95)
      << ",\"p99_ms\":" << at(.99) << ",\"max_ms\":" << times.back() << "}";
}
} // namespace
int wmain(int argc, wchar_t **argv) {
  try {
    if (argc != 4)
      throw std::runtime_error(
          "Usage: professional_probe image|abc|mesh INPUT EXTERNAL_OUTPUT_DIR");
    const std::filesystem::path input(argv[2]), output(argv[3]);
    std::filesystem::create_directories(output);
    std::ofstream json(output / "result.json");
    json << std::setprecision(17);
    const auto start = Clock::now();
    if (std::wstring_view(argv[1]) == L"image" ||
        std::wstring_view(argv[1]) == L"image-summary") {
      auto file = formaview::imaging::ImageFile::open(input);
      const auto open_ms = ms(start);
      const auto &d = file.description();
      json << "{\"sha256\":" << quote(d.source.identity.sha256)
           << ",\"open_ms\":" << open_ms
           << ",\"resident_bytes\":" << file.resident_bytes()
           << ",\"alpha\":" << static_cast<int>(d.alpha)
           << ",\"subresources\":[";
      for (std::size_t i = 0; i < d.subresources.size(); ++i) {
        const auto read_start = Clock::now();
        auto s = file.decode(i);
        const auto decode_ms = ms(read_start);
        if (i)
          json << ',';
        json << "{\"index\":" << i << ",\"part\":" << s.part
             << ",\"part_name\":" << quote(s.part_name) << ",\"mip\":" << s.mip
             << ",\"array\":" << s.array_layer << ",\"face\":" << s.cube_face
             << ",\"slice\":" << s.depth_slice
             << ",\"width\":" << s.data_window.width
             << ",\"height\":" << s.data_window.height
             << ",\"x\":" << s.data_window.x << ",\"y\":" << s.data_window.y
             << ",\"decode_ms\":" << decode_ms << ",\"channels\":[";
        for (std::size_t c = 0; c < s.channels.size(); ++c) {
          const auto &channel = s.channels[c];
          const auto name =
              std::to_string(i) + "-" + std::to_string(c) + ".bin";
          if (std::wstring_view(argv[1]) == L"image") {
            std::ofstream raw(output / name, std::ios::binary);
            raw.write(
                reinterpret_cast<const char *>(channel.samples.bytes.data()),
                static_cast<std::streamsize>(channel.samples.bytes.size()));
          }
          if (c)
            json << ',';
          json << "{\"name\":" << quote(channel.name)
               << ",\"scalar\":" << static_cast<int>(channel.samples.scalar)
               << ",\"file\":" << quote(name) << ",\"sha256\":"
               << quote(formaview::skeletal::sha256(channel.samples.bytes))
               << '}';
        }
        json << "]}";
      }
      json << "]}";
    } else if (std::wstring_view(argv[1]) == L"abc") {
      auto archive = formaview::cache::Archive::open(input);
      json << "{\"open_ms\":" << ms(start) << ",\"meshes\":[";
      for (std::size_t i = 0; i < archive.meshes().size(); ++i) {
        const auto &info = archive.meshes()[i];
        if (i)
          json << ',';
        json << "{\"path\":" << quote(info.path)
             << ",\"topology\":" << static_cast<int>(info.topology)
             << ",\"times\":[";
        for (std::size_t t = 0; t < info.times.size(); ++t) {
          if (t)
            json << ',';
          json << info.times[t];
        }
        json << "],\"samples\":[";
        std::vector<double> timings;
        std::vector<std::size_t> order(info.times.size());
        std::iota(order.begin(), order.end(), 0);
        std::mt19937 random(303);
        std::shuffle(order.begin(), order.end(), random);
        // Seeded permutation; each sample releases before the next.
        for (std::size_t k = 0; k < info.times.size(); ++k) {
          const auto t = order[k];
          const auto before = Clock::now();
          auto sample = archive.read(i, info.times[t]);
          timings.push_back(ms(before));
          if (k)
            json << ',';
          json << "{\"index\":" << sample.index
               << ",\"seconds\":" << sample.seconds
               << ",\"points\":" << sample.positions.size()
               << ",\"corners\":" << sample.corner_points.size()
               << ",\"uv\":" << sample.uv.size()
               << ",\"normals\":" << sample.normals.size() << "}";
          const auto write = [&](const auto &values, const char *type) {
            std::ofstream bin(output / (std::to_string(i) + "-" +
                                        std::to_string(sample.index) + "-" +
                                        type + ".bin"),
                              std::ios::binary);
            bin.write(reinterpret_cast<const char *>(values.data()),
                      static_cast<std::streamsize>(values.size() *
                                                   sizeof(values[0])));
          };
          write(sample.positions, "positions");
          write(sample.uv, "uv");
          write(sample.normals, "normals");
          write(sample.corner_points, "indices");
          write(sample.face_sizes, "counts");
          write(sample.world, "world");
        }
        json << "],\"random_read\":";
        stats(json, timings);
        json << '}';
      }
      json << "]}";
    } else if (std::wstring_view(argv[1]) == L"mesh") {
      auto source = formaview::skeletal::read_actor(input);
      auto pose = formaview::skeletal::default_pose(source);
      if (!source.mesh)
        throw std::runtime_error("No mesh");
      const auto &mesh = *source.mesh;
      formaview::preview::WeightIndex weights(mesh, source.bones.size());
      std::size_t max_influences = 0;
      for (const auto &w : mesh.point_weights)
        max_influences = std::max(max_influences, w.size());
      json << "{\"open_ms\":" << ms(start)
           << ",\"points\":" << mesh.points.size()
           << ",\"faces\":" << mesh.faces.size()
           << ",\"bones\":" << source.bones.size()
           << ",\"max_influences\":" << max_influences
           << ",\"uv_sets\":" << mesh.extra_uvs.size() + 1;
      std::vector<double> evaluation, selection;
      formaview::preview::SkinEvaluator evaluator(source);
      for (int iteration = 0; iteration < 200; ++iteration) {
        const auto before = Clock::now();
        const auto &frame = evaluator.evaluate(pose.world);
        evaluation.push_back(ms(before));
        for (const auto &v : frame.corners) {
          const auto &p = mesh.points[v.point];
          if (std::abs(v.position.x - p.x) > 1e-7 ||
              std::abs(v.position.y - p.y) > 1e-7 ||
              std::abs(v.position.z - p.z) > 1e-7)
            throw std::runtime_error("Bind deformation mismatch");
        }
        const auto select_start = Clock::now();
        const auto selected = weights.select(
            static_cast<std::size_t>(iteration) % source.bones.size());
        selection.push_back(ms(select_start));
        if (selected.size() != mesh.points.size())
          throw std::runtime_error("Selection mismatch");
      }
      for (const auto &[name, uv] : mesh.extra_uvs) {
        const auto frame =
            formaview::preview::evaluate_mesh(source, pose.world, name);
        for (const auto &v : frame.corners)
          if (v.uv.x != uv[v.wedge].x || v.uv.y != uv[v.wedge].y)
            throw std::runtime_error("UV mismatch");
      }
      json << ",\"full_skin\":";
      stats(json, evaluation);
      json << ",\"heat_selection\":";
      stats(json, selection);
      json << '}';
    } else
      throw std::runtime_error("Unknown mode");
    json.flush();
    if (!json)
      throw std::runtime_error("Cannot write report");
    std::cout << "PASS\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
