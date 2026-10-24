// M07 glTF animation channel tests. Builds a self-contained animated .gltf
// (embedded base64 buffer) at runtime and verifies reading and evaluation
// against hand-computed expectations for linear, step and cubic-spline
// samplers. Optional real sample: gltf_anim_tests <glb-or-gltf> <out.json>
#include "formaview/gltf_anim.hpp"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
using formaview::gltf::AnimationClip;
using formaview::gltf::GltfAnimationData;
using formaview::gltf::Interpolation;
using formaview::gltf::TrackPath;

int failures = 0;
void check(const std::string &name, bool condition,
           const std::string &detail = {}) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL " << name << (detail.empty() ? "" : ": " + detail)
              << "\n";
  } else {
    std::cout << "pass " << name << "\n";
  }
}
bool near(double a, double b, double epsilon = 1e-9) {
  return std::abs(a - b) <= epsilon;
}
constexpr double kPi = 3.14159265358979323846;

std::string base64(const std::vector<std::uint8_t> &bytes) {
  static const char *table =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t i = 0; i < bytes.size(); i += 3) {
    const std::uint32_t triple = (bytes[i] << 16) |
                                 (i + 1 < bytes.size() ? bytes[i + 1] << 8 : 0) |
                                 (i + 2 < bytes.size() ? bytes[i + 2] : 0);
    out += table[(triple >> 18) & 63];
    out += table[(triple >> 12) & 63];
    out += i + 1 < bytes.size() ? table[(triple >> 6) & 63] : '=';
    out += i + 2 < bytes.size() ? table[triple & 63] : '=';
  }
  return out;
}

std::vector<std::uint8_t> floats(const std::vector<float> &values) {
  std::vector<std::uint8_t> bytes(values.size() * 4);
  std::memcpy(bytes.data(), values.data(), bytes.size());
  return bytes;
}

struct AccessorSpec {
  std::vector<float> values;
  const char *type;
  std::size_t components;
};

std::string buildAnimatedGltf(const std::filesystem::path &path,
                            const wchar_t *externalBufferName = nullptr) {
  // Three samplers with hand-computable expectations:
  //  node0.translation LINEAR  t=[0,2]   (0,0,0) -> (4,0,0)
  //  node0.rotation    LINEAR  t=[0,2]   identity -> 90 degrees about Z
  //  node1.scale       STEP    t=[0,1,2] 1 -> 2 -> 3
  //  node2.translation CUBICSPLINE t=[0,2] flat tangents, 0 -> 4
  //    (Hermite with zero tangents: smoothstep weights, 0.25 -> 0.625)
  const std::vector<AccessorSpec> accessors{
      {{0.f, 2.f}, "SCALAR", 1},                                            // 0 times TR
      {{0.f, 0.f, 0.f, 4.f, 0.f, 0.f}, "VEC3", 3},                         // 1 values TR
      {{0.f, 2.f}, "SCALAR", 1},                                            // 2 times rot
      {{0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.7071067811865476f,
        0.7071067811865476f}, "VEC4", 4},                                   // 3 values rot
      {{0.f, 1.f, 2.f}, "SCALAR", 1},                                       // 4 times scale
      {{1.f, 1.f, 1.f, 2.f, 2.f, 2.f, 3.f, 3.f, 3.f}, "VEC3", 3},          // 5 values scale
      {{0.f, 2.f}, "SCALAR", 1},                                            // 6 times cubic
      {{0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f,
        4.f, 0.f, 0.f, 0.f, 0.f, 0.f}, "VEC3", 3},                          // 7 cubic in/value/out
  };
  std::vector<std::uint8_t> buffer;
  std::string json = R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0,1,2]}],"scene":0,)";
  json += R"("nodes":[{"name":"spin"},{"name":"grow"},{"name":"curve"}],)";
  std::string views;
  for (std::size_t i = 0; i < accessors.size(); ++i) {
    const auto bytes = floats(accessors[i].values);
    const std::size_t offset = buffer.size();
    buffer.insert(buffer.end(), bytes.begin(), bytes.end());
    views += "{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) +
             ",\"byteLength\":" + std::to_string(bytes.size()) + "}";
    if (i + 1 < accessors.size())
      views += ',';
  }
  std::string accessorJson;
  for (std::size_t i = 0; i < accessors.size(); ++i) {
    const auto &spec = accessors[i];
    std::string minimum, maximum;
    for (std::size_t c = 0; c < spec.components; ++c) {
      double low = spec.values[c], high = spec.values[c];
      for (std::size_t v = c; v < spec.values.size(); v += spec.components) {
        low = std::min(low, static_cast<double>(spec.values[v]));
        high = std::max(high, static_cast<double>(spec.values[v]));
      }
      minimum += std::to_string(low);
      maximum += std::to_string(high);
      if (c + 1 < spec.components) {
        minimum += ',';
        maximum += ',';
      }
    }
    accessorJson += "{\"bufferView\":" + std::to_string(i) +
                    ",\"componentType\":5126,\"count\":" +
                    std::to_string(spec.values.size() / spec.components) +
                    ",\"type\":\"" + spec.type + "\",\"min\":[" + minimum +
                    "],\"max\":[" + maximum + "]}";
    if (i + 1 < accessors.size())
      accessorJson += ',';
  }
  json += "\"bufferViews\":[" + views + "],\"accessors\":[" + accessorJson +
          "],";
  json += R"("animations":[{"name":"Spin","channels":[)"
          R"({"sampler":0,"target":{"node":0,"path":"translation"}},)"
          R"({"sampler":1,"target":{"node":0,"path":"rotation"}},)"
          R"({"sampler":2,"target":{"node":1,"path":"scale"}},)"
          R"({"sampler":3,"target":{"node":2,"path":"translation"}}],)"
          R"("samplers":[{"input":0,"output":1,"interpolation":"LINEAR"},)"
          R"({"input":2,"output":3,"interpolation":"LINEAR"},)"
          R"({"input":4,"output":5,"interpolation":"STEP"},)"
          R"({"input":6,"output":7,"interpolation":"CUBICSPLINE"}]}],)";
  if (externalBufferName) {
    std::ofstream bin(path.parent_path() / externalBufferName,
                      std::ios::binary);
    bin.write(reinterpret_cast<const char *>(buffer.data()),
              static_cast<std::streamsize>(buffer.size()));
    json += "\"buffers\":[{\"byteLength\":" + std::to_string(buffer.size()) +
            ",\"uri\":\"" +
            std::filesystem::path(externalBufferName).string() + "\"}]}";
  } else {
    json += "\"buffers\":[{\"byteLength\":" + std::to_string(buffer.size()) +
            ",\"uri\":\"data:application/octet-stream;base64," +
            base64(buffer) + "\"}]}";
  }
  std::ofstream file(path, std::ios::binary);
  file << json;
  return json;
}

void runSynthetic(const std::filesystem::path &path) {
  {
    GltfAnimationData cubic;
    cubic.node_count = 1;
    formaview::gltf::AnimationChannel channel;
    channel.node_index = 0;
    channel.path = TrackPath::Rotation;
    channel.components = 4;
    channel.interpolation = Interpolation::CubicSpline;
    channel.times = {0, 2};
    // in/value/out, four components each; nonzero tangents distinguish
    // proper Hermite interpolation from normalized linear interpolation.
    channel.values = {0,0,0,0, 0,0,0,1, 0,0,1,0,
                      0,0,0,1, 0,0,1,0, 0,0,0,0};
    AnimationClip clip;
    clip.channels.push_back(channel);
    cubic.clips.push_back(clip);
    const auto first = formaview::gltf::evaluate_clip(cubic, 0, 0)[0].local.rotation;
    const auto last = formaview::gltf::evaluate_clip(cubic, 0, 2)[0].local.rotation;
    const auto mid = formaview::gltf::evaluate_clip(cubic, 0, 1)[0].local.rotation;
    check("cubic.rotation.first", near(first.z, 0) && near(first.w, 1));
    check("cubic.rotation.last", near(last.z, 1) && near(last.w, 0));
    // Hermite midpoint is (0,0,.75,.25), normalized by sqrt(.625).
    check("cubic.rotation.tangents", near(mid.z, 3/std::sqrt(10.0)) &&
          near(mid.w, 1/std::sqrt(10.0)));
  }
  {
    GltfAnimationData bad; bad.node_count=1;
    AnimationClip clip;
    formaview::gltf::AnimationChannel channel;
    channel.node_index=0; channel.path=TrackPath::Translation; channel.components=3;
    channel.times={0,1}; channel.values={0}; clip.channels={channel}; bad.clips={clip};
    bool rejected=false;
    try { (void)formaview::gltf::evaluate_clip(bad,0,.5); }
    catch (const formaview::skeletal::Failure &) { rejected=true; }
    check("shape.malformed_rejected",rejected);
  }
  buildAnimatedGltf(path);
  const formaview::skeletal::Limits limits;
  const GltfAnimationData data =
      formaview::gltf::read_gltf_animations(path, limits);
  check("read.clips", data.clips.size() == 1);
  check("read.nodes", data.node_count == 3);
  const AnimationClip &clip = data.clips[0];
  check("read.name", clip.name == "Spin");
  {
    // P2 review: the reader must survive a directory outside the ANSI code
    // page (no path.string() -> fopen) like the product loader.
    const auto exotic =
        path.parent_path() /
        std::filesystem::path(std::wstring(L"\xD835\xDD4F\xD83C\xDFAC-dir"));
    std::filesystem::create_directories(exotic);
    buildAnimatedGltf(exotic / path.filename());
    bool unicodeOk = false;
    try {
      const auto unicode = formaview::gltf::read_gltf_animations(
          exotic / path.filename(), limits);
      unicodeOk = unicode.clips.size() == 1 && unicode.node_count == 3;
    } catch (const std::exception &) {
    }
    check("read.unicode_path", unicodeOk);
  }
  {
    // P2 review: an external .bin buffer with a relative URI must load
    // through the constrained reader instead of cgltf_load_buffers.
    const auto external = path.parent_path() / L"external-buffer-case";
    std::filesystem::create_directories(external);
    buildAnimatedGltf(external / path.filename(), L"anim.bin");
    bool externalOk = false;
    try {
      const auto externalData = formaview::gltf::read_gltf_animations(
          external / path.filename(), limits);
      externalOk = externalData.clips.size() == 1 &&
                   externalData.node_count == 3 &&
                   externalData.clips.front().channels.size() == 4;
    } catch (const std::exception &) {
    }
    check("read.external_buffer", externalOk);
  }
  check("read.range", clip.start == 0.0 && clip.end == 2.0);
  check("read.channels", clip.channels.size() == 4);

  const auto at = [&](double seconds) {
    return formaview::gltf::evaluate_clip(data, 0, seconds);
  };
  {
    const auto pose = at(1.0);
    check("linear.translation",
          pose[0].translation && near(pose[0].local.translation.x, 2.0));
    // slerp(identity, 90deg Z, 0.5) = 45 degrees about Z.
    check("linear.rotation",
          pose[0].rotation &&
              near(pose[0].local.rotation.z,
                   std::sin(kPi / 8), 1e-6) &&
              near(pose[0].local.rotation.w, std::cos(kPi / 8), 1e-6));
    check("flags.translation_only",
          !pose[0].scale && pose[0].local.scale.x == 1.0);
    check("step.scale",
          pose[1].scale && near(pose[1].local.scale.x, 2.0));
    check("cubic.midpoint",
          pose[2].translation && near(pose[2].local.translation.x, 2.0));
  }
  {
    const auto pose = at(0.25);
    // 0.25 s of a 2 s clip is one eighth of the ramp: 0 -> 4 gives 0.5.
    check("linear.quarter", near(pose[0].local.translation.x, 0.5));
    check("step.holds", near(pose[1].local.scale.x, 1.0));
    // Hermite with zero tangents: smoothstep(0.125)*4 = 0.171875.
    check("cubic.quarter",
          near(pose[2].local.translation.x, 0.171875, 1e-6));
  }
  {
    const auto before = at(-1.0), after = at(99.0);
    check("clamp.start", before[0].local.translation.x == 0.0 &&
                             before[1].local.scale.x == 1.0);
    check("clamp.end", after[0].local.translation.x == 4.0 &&
                           after[1].local.scale.x == 3.0);
  }
}
} // namespace

int wmain(int argc, wchar_t **argv) {
  try {
    const auto directory = std::filesystem::temp_directory_path();
    runSynthetic(directory / "formaview-m07-gltf-anim-test.gltf");
  } catch (const std::exception &failure) {
    std::cerr << "glTF animation synthetic stage failed: " << failure.what()
              << std::endl;
    return 2;
  }
  if (argc == 3) {
    QJsonObject evidence;
    try {
      const auto data = formaview::gltf::read_gltf_animations(
          std::filesystem::path(argv[1]), formaview::skeletal::Limits{});
      evidence["clips"] = static_cast<int>(data.clips.size());
      QJsonArray list;
      for (const auto &clip : data.clips) {
        QJsonObject entry{{"name", QString::fromStdString(clip.name)},
                          {"start", clip.start},
                          {"end", clip.end},
                          {"channels", static_cast<int>(clip.channels.size())}};
        if (!clip.channels.empty()) {
          const auto pose =
              formaview::gltf::evaluate_clip(data, 0, clip.start);
          entry["nodes"] = static_cast<int>(pose.size());
        }
        list.append(entry);
      }
      evidence["detail"] = list;
    } catch (const std::exception &failure) {
      evidence["error"] = failure.what();
    }
    QFile output(QString::fromWCharArray(argv[2]));
    if (output.open(QIODevice::WriteOnly))
      output.write(QJsonDocument(evidence).toJson(QJsonDocument::Indented));
  }
  if (failures) {
    std::cerr << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "glTF animation tests complete\n";
  return 0;
}
