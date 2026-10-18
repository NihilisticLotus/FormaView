#include "formaview/geometry_cache.hpp"
#include "formaview/preview.hpp"
#include "formaview/pro_image.hpp"
#include "formaview/scheduler.hpp"
#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>
#include <DirectXTex.h>
#include <Imath/half.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfMultiPartOutputFile.h>
#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfOutputPart.h>
#include <OpenEXR/ImfPartType.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>

namespace {
using namespace formaview;
void require(bool ok, const char *s) {
  if (!ok)
    throw std::runtime_error(s);
}
template <class Fn> void rejects(Fn fn) {
  bool rejected = false;
  try {
    fn();
  } catch (const std::exception &) {
    rejected = true;
  }
  require(rejected, "Expected rejection");
}
void images(const std::filesystem::path &dir) {
  namespace E = OPENEXR_IMF_NAMESPACE;
  const auto exr = dir / "precision.exr";
  std::array<float, 6> f{-3.25F,
                         1.000000119F,
                         65536.F,
                         std::numeric_limits<float>::infinity(),
                         std::numeric_limits<float>::quiet_NaN(),
                         -0.F};
  std::array<IMATH_NAMESPACE::half, 6> h{-2.F, .5F, 60000.F, 0.F, 1.F, 2.F};
  std::array<std::uint32_t, 6> u{0, 1, 16777217, 4294967295U, 3, 4};
  {
    E::Header header(3, 2);
    header.dataWindow() = IMATH_NAMESPACE::Box2i({-3, 7}, {-1, 8});
    header.channels().insert("layer.Z", E::Channel(E::FLOAT));
    header.channels().insert("R", E::Channel(E::HALF));
    header.channels().insert("id", E::Channel(E::UINT));
    E::FrameBuffer fb;
    fb.insert("layer.Z",
              E::Slice::Make(E::FLOAT, f.data(), header.dataWindow()));
    fb.insert("R", E::Slice::Make(E::HALF, h.data(), header.dataWindow()));
    fb.insert("id", E::Slice::Make(E::UINT, u.data(), header.dataWindow()));
    E::OutputFile out(exr.string().c_str(), header);
    out.setFrameBuffer(fb);
    out.writePixels(2);
  }
  auto input = imaging::ImageFile::open(exr);
  auto s = input.decode(0);
  require(s.data_window.x == -3 && s.data_window.y == 7 &&
              s.display_window.width == 3,
          "EXR window loss");
  for (const auto &c : s.channels) {
    if (c.name == "layer.Z") {
      require(c.samples.scalar == ScalarType::Float32, "float narrowed");
      require(std::memcmp(c.samples.bytes.data(), f.data(), sizeof(f)) == 0,
              "Float source bits changed");
    }
    if (c.name == "R")
      require(std::memcmp(c.samples.bytes.data(), h.data(), sizeof(h)) == 0,
              "Half source bits changed");
    if (c.name == "id")
      require(imaging::value(c, 2) == 16777217 &&
                  imaging::value(c, 3) == 4294967295.,
              "UINT precision lost");
  }
  const auto multipartPath = dir / "multipart.exr";
  {
    std::array<E::Header, 2> headers{E::Header(2, 1), E::Header(2, 1)};
    for (int part = 0; part < 2; ++part) {
      headers[part].setName(part == 0 ? "luminance" : "depth");
      headers[part].setType(E::SCANLINEIMAGE);
      headers[part].dataWindow() = IMATH_NAMESPACE::Box2i({part, 0}, {part, 0});
      headers[part].channels().insert(part == 0 ? "Y" : "Z",
                                      E::Channel(E::FLOAT));
    }
    E::MultiPartOutputFile output(multipartPath.string().c_str(),
                                  headers.data(), 2);
    for (int part = 0; part < 2; ++part) {
      float sample = part == 0 ? .25F : 7.5F;
      E::FrameBuffer frame;
      frame.insert(
          part == 0 ? "Y" : "Z",
          E::Slice::Make(E::FLOAT, &sample, headers[part].dataWindow()));
      E::OutputPart entry(output, part);
      entry.setFrameBuffer(frame);
      entry.writePixels(1);
    }
  }
  const auto multipart = imaging::ImageFile::open(multipartPath);
  require(multipart.description().subresources.size() == 2,
          "EXR part count lost");
  const auto depthPart = multipart.decode(1);
  require(depthPart.part_name == "depth" && depthPart.data_window.x == 1 &&
              depthPart.channels[0].name == "Z" &&
              imaging::value(depthPart.channels[0], 0) == 7.5,
          "EXR selected part content or window changed");
  imaging::DisplayOptions options;
  options.channels = {"layer.Z", "layer.Z", "layer.Z", "missing"};
  options.meaning = ImageMeaning::Data;
  options.exposure = 4;
  options.apply_srgb = true;
  auto display = imaging::display_rgba(s, options);
  require(display[0] == f[0] && display[12] == 1 && display[13] == 0 &&
              display[14] == 1,
          "Data/invalid display policy");
  const auto source_hash = skeletal::sha256(s.channels[0].samples.bytes);
  options.meaning = ImageMeaning::Colour;
  options.apply_srgb = false;
  display = imaging::display_rgba(s, options);
  require(display[0] == f[0] * 16, "Exposure not applied");
  require(source_hash == skeletal::sha256(s.channels[0].samples.bytes),
          "Display modified source");
  ImageSubresource alpha;
  alpha.data_window = {0, 0, 1, 1};
  for (const auto &[name, sample] :
       std::array<std::pair<const char *, float>, 4>{
           {{"R", .2F}, {"G", .1F}, {"B", .05F}, {"A", .5F}}}) {
    Buffer b;
    b.scalar = ScalarType::Float32;
    b.element_count = 1;
    b.bytes.resize(4);
    std::memcpy(b.bytes.data(), &sample, 4);
    alpha.channels.push_back({name, std::move(b)});
  }
  imaging::DisplayOptions compositing;
  compositing.composite = true;
  rejects([&] { (void)imaging::display_rgba(alpha, compositing); });
  compositing.alpha = AlphaMode::Premultiplied;
  auto premult = imaging::display_rgba(alpha, compositing);
  compositing.alpha = AlphaMode::Straight;
  auto straight = imaging::display_rgba(alpha, compositing);
  require(std::abs(premult[0] - .29F) < 1e-6F &&
              std::abs(straight[0] - .19F) < 1e-6F && premult[3] == 1,
          "Explicit alpha interpretation mismatch");
  compositing.alpha = AlphaMode::Premultiplied;
  compositing.meaning = ImageMeaning::Colour;
  compositing.exposure = 1;
  compositing.apply_srgb = true;
  const auto linearComposite = imaging::display_rgba(alpha, compositing);
  const auto expectedComposite = 1.055 * std::pow(.49, 1. / 2.4) - .055;
  require(std::abs(linearComposite[0] - expectedComposite) < 1e-6,
          "Premultiplied alpha must be composited before OETF");
  ImageSubresource signedImage;
  signedImage.data_window = {0, 0, 1, 1};
  ImageChannel signedChannel;
  signedChannel.name = "R";
  signedChannel.encoding = SampleEncoding::SNorm;
  signedChannel.samples.scalar = ScalarType::Int16;
  signedChannel.samples.element_count = 1;
  const std::int16_t negative = -16384;
  signedChannel.samples.bytes.resize(sizeof(negative));
  std::memcpy(signedChannel.samples.bytes.data(), &negative, sizeof(negative));
  signedImage.channels.push_back(signedChannel);
  imaging::DisplayOptions signedOptions;
  signedOptions.channels = {"R", "R", "R", "missing"};
  const auto signedDisplay = imaging::display_rgba(signedImage, signedOptions);
  require(std::abs(signedDisplay[0] - (-16384. / 32767.)) < 1e-6,
          "SNorm display must preserve signed normalized values");
  rejects([&] {
    imaging::Limits limit;
    limit.decoded_bytes = 4;
    auto file = imaging::ImageFile::open(exr, limit);
    (void)file.decode(0);
  });
  std::stop_source stop;
  stop.request_stop();
  rejects([&] { (void)input.decode(0, stop.get_token()); });
  const std::array<DXGI_FORMAT, 12> formats{
      DXGI_FORMAT_BC1_UNORM,      DXGI_FORMAT_BC2_UNORM,
      DXGI_FORMAT_BC3_UNORM,      DXGI_FORMAT_BC4_UNORM,
      DXGI_FORMAT_BC4_SNORM,      DXGI_FORMAT_BC5_UNORM,
      DXGI_FORMAT_BC5_SNORM,      DXGI_FORMAT_BC6H_UF16,
      DXGI_FORMAT_BC6H_SF16,      DXGI_FORMAT_BC7_UNORM,
      DXGI_FORMAT_BC7_UNORM_SRGB, DXGI_FORMAT_R32G32B32A32_FLOAT};
  for (auto format : formats) {
    const float red = format == DXGI_FORMAT_BC4_SNORM ||
                              format == DXGI_FORMAT_BC5_SNORM ||
                              format == DXGI_FORMAT_BC6H_SF16
                          ? -.25F
                          : .25F;
    DirectX::ScratchImage rgba, encoded;
    require(SUCCEEDED(
                rgba.Initialize2D(DXGI_FORMAT_R32G32B32A32_FLOAT, 8, 8, 1, 1)),
            "Init");
    auto *p = reinterpret_cast<float *>(rgba.GetPixels());
    for (int i = 0; i < 64; ++i) {
      p[i * 4] = red;
      p[i * 4 + 1] = .5F;
      p[i * 4 + 2] = .75F;
      p[i * 4 + 3] = 1;
    }
    const DirectX::ScratchImage *save = &rgba;
    if (DirectX::IsCompressed(format)) {
      require(SUCCEEDED(DirectX::Compress(*rgba.GetImage(0, 0, 0), format,
                                          DirectX::TEX_COMPRESS_DEFAULT, .5F,
                                          encoded)),
              "BC encode");
      save = &encoded;
    }
    const auto path = dir / ("format-" + std::to_string(format) + ".dds");
    require(SUCCEEDED(DirectX::SaveToDDSFile(
                save->GetImages(), save->GetImageCount(), save->GetMetadata(),
                DirectX::DDS_FLAGS_FORCE_DX10_EXT, path.c_str())),
            "DDS write");
    auto file = imaging::ImageFile::open(path);
    auto decoded = file.decode(0);
    require(!decoded.encoded_blocks.empty(), "Encoded blocks lost");
    for (const auto &c : decoded.channels)
      if (c.name == "R")
        require(std::abs(imaging::value(c, 0) -
                         (DirectX::IsSRGB(format)
                              ? 1.055 * std::pow(red, 1.0 / 2.4) - .055
                              : red)) < .035,
                "BC source decode mismatch");
  }
  for (int kind = 0; kind < 3; ++kind) {
    DirectX::ScratchImage image;
    const auto init =
        kind == 0   ? image.InitializeCube(DXGI_FORMAT_R32_FLOAT, 8, 8, 2, 4)
        : kind == 1 ? image.Initialize3D(DXGI_FORMAT_R32_FLOAT, 8, 4, 4, 3)
                    : image.Initialize2D(DXGI_FORMAT_R32_FLOAT, 8, 4, 3, 4);
    require(SUCCEEDED(init), "Subresource init");
    for (std::size_t i = 0; i < image.GetImageCount(); ++i) {
      const auto &plane = image.GetImages()[i];
      for (std::size_t y = 0; y < plane.height; ++y)
        for (std::size_t x = 0; x < plane.width; ++x) {
          const float v = static_cast<float>(i) + .25F;
          std::memcpy(plane.pixels + y * plane.rowPitch + x * 4, &v, 4);
        }
    }
    const auto path = dir / ("subresources-" + std::to_string(kind) + ".dds");
    require(SUCCEEDED(DirectX::SaveToDDSFile(
                image.GetImages(), image.GetImageCount(), image.GetMetadata(),
                DirectX::DDS_FLAGS_FORCE_DX10_EXT, path.c_str())),
            "Subresource save");
    auto file = imaging::ImageFile::open(path);
    require(file.description().subresources.size() == image.GetImageCount(),
            "Subresource count");
    for (std::size_t i = 0; i < image.GetImageCount(); ++i) {
      auto plane = file.decode(i);
      require(imaging::value(plane.channels[0], 0) ==
                  static_cast<double>(i) + .25,
              "Subresource address");
    }
  }
}
void caches(const std::filesystem::path &dir) {
  namespace A = Alembic::Abc;
  namespace G = Alembic::AbcGeom;
  namespace O = Alembic::AbcCoreOgawa;
  const auto path = dir / "varying.abc";
  {
    A::OArchive archive(O::WriteArchive(), path.string());
    const std::vector<double> times{.25, .75, 2};
    auto sampling = std::make_shared<A::TimeSampling>(
        A::TimeSamplingType(A::TimeSamplingType::kAcyclic), times);
    const auto ts = archive.addTimeSampling(*sampling);
    G::OPolyMesh mesh(archive.getTop(), "mesh", ts);
    for (int i = 0; i < 3; ++i) {
      std::vector<A::V3f> p{
          {0, 0, static_cast<float>(i)}, {1, 0, 0}, {0, 1, 0}};
      std::vector<std::int32_t> idx{0, 1, 2}, counts{3};
      if (i == 1) {
        p.push_back({1, 1, 0});
        idx = {0, 1, 3, 0, 3, 2};
        counts = {3, 3};
      }
      std::vector<A::V2f> uv(idx.size(), {static_cast<float>(i), .5F});
      std::vector<A::N3f> n(idx.size(), {0, static_cast<float>(i), 1});
      G::OPolyMeshSchema::Sample sample(
          A::P3fArraySample(p), A::Int32ArraySample(idx),
          A::Int32ArraySample(counts),
          G::OV2fGeomParam::Sample(A::V2fArraySample(uv), G::kFacevaryingScope),
          G::ON3fGeomParam::Sample(A::N3fArraySample(n), G::kFacevaryingScope));
      mesh.getSchema().set(sample);
    }
  }
  auto archive = cache::Archive::open(path);
  require(archive.meshes().size() == 1 &&
              archive.meshes()[0].topology == Topology::Varying,
          "Topology classification");
  for (const auto [time, index] : std::array<std::pair<double, std::size_t>, 5>{
           {{2, 2}, {.25, 0}, {.8, 1}, {.26, 0}, {1.9, 2}}}) {
    const auto sample = archive.read(0, time);
    require(sample.index == index, "Nonuniform time selection");
    require(sample.positions.size() == (index == 1 ? 4 : 3), "Topology stale");
    require(sample.uv[0].x == static_cast<double>(index) &&
                sample.normals[0].y == static_cast<double>(index),
            "Animated UV/normal stale");
  }
  rejects([&] {
    cache::Limits limits;
    limits.sample_bytes = 8;
    auto limited = cache::Archive::open(path, limits);
    (void)limited.read(0, .75);
  });
  const auto malformed = dir / "invalid-uv-index.abc";
  {
    A::OArchive out(O::WriteArchive(), malformed.string());
    G::OPolyMesh mesh(out.getTop(), "mesh");
    std::vector<A::V3f> p{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    std::vector<std::int32_t> indices{0, 1, 2}, counts{3};
    std::vector<A::V2f> uv{{0, 0}};
    std::vector<std::uint32_t> uv_indices{0, 0, 99};
    mesh.getSchema().set(G::OPolyMeshSchema::Sample(
        A::P3fArraySample(p), A::Int32ArraySample(indices),
        A::Int32ArraySample(counts),
        G::OV2fGeomParam::Sample(A::V2fArraySample(uv),
                                 A::UInt32ArraySample(uv_indices),
                                 G::kFacevaryingScope)));
  }
  rejects([&] { (void)cache::Archive::open(malformed).read(0, 0); });
}
void preview_test() {
  skeletal::ActorAsset source;
  source.mesh.emplace();
  auto &mesh = *source.mesh;
  mesh.points = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  mesh.wedges = {{0, 0, {}}, {1, 0, {}}, {2, 0, {}}};
  mesh.faces.push_back({{0, 1, 2}, 0, 0, 0});
  mesh.point_weights.resize(3);
  const auto identity = skeletal::matrix({});
  std::vector<Matrix4> world(8, identity);
  for (std::uint32_t b = 0; b < 8; ++b) {
    skeletal::Bone bone;
    bone.bind_world = identity;
    source.bones.push_back(bone);
    world[b][3] = b;
    for (std::uint32_t p = 0; p < 3; ++p) {
      mesh.point_weights[p].push_back(
          static_cast<std::uint32_t>(mesh.weights.size()));
      mesh.weights.push_back({p, b, .125});
    }
  }
  mesh.extra_uvs["UV3"] = {{-2, 3}, {4, 5}, {6, 7}};
  const auto frame = preview::evaluate_mesh(source, world, "UV3");
  require(frame.corners[0].position.x == 3.5 && frame.corners[0].uv.x == -2,
          "Full eight influences/UV");
  require(mesh.weights.size() == 24, "Source weights changed");
  const auto hit = preview::pick(frame, {3.6, .1, 2}, {0, 0, -1});
  require(hit && hit->source_point == 0 && hit->source_wedge == 0,
          "Pick source mapping");
  preview::WeightIndex index(mesh, 8);
  require(index.select(7)[0].sum == .125, "Weight beyond fourth lost");
  preview::BoundedCache<int, std::vector<int>> cache(16);
  require(cache.put(0, std::make_unique<std::vector<int>>(4), 16), "Cache put");
  require(cache.put(1, std::make_unique<std::vector<int>>(2), 8),
          "Cache evict");
  require(!cache.get(0) && cache.bytes() == 8, "Cache budget");
  cache.clear();
  require(cache.bytes() == 0, "Cache release");
}
void background(const std::filesystem::path &dir) {
  images(dir);
  Scheduler scheduler;
  LatestRequest latest;
  std::promise<void> entered, unblock;
  auto gate = unblock.get_future().share();
  std::atomic<bool> published = false;
  auto ticket = latest.issue();
  TaskRequest request;
  request.resources = {1, 1, 64ULL * 1024 * 1024, 0};
  request.generation = ticket;
  request.work = [&, gate, ticket](std::stop_token stop) {
    entered.set_value();
    gate.wait();
    auto file = imaging::ImageFile::open(dir / "precision.exr", {}, stop);
    auto sample = file.decode(0, stop);
    (void)sample;
    const auto accepted = ticket.publish([&] { published = true; });
    (void)accepted;
    return TaskResult{};
  };
  auto first = scheduler.submit(std::move(request));
  require(first.task.has_value(), "Worker submission failed");
  entered.get_future().wait();
  auto replacement = latest.issue();
  const auto before = std::chrono::steady_clock::now();
  require(first.task->cancel(), "Cancellation not accepted");
  require(std::chrono::steady_clock::now() - before <
              std::chrono::milliseconds(100),
          "Cancellation blocked by I/O");
  require(!ticket.current() && replacement.current(), "Stale result isolation");
  unblock.set_value();
  require(first.task->wait_for(std::chrono::seconds(5)),
          "Worker did not finish");
  require(!published, "Cancelled source published");
  scheduler.shutdown();
  require(scheduler.stats().reserved.memory_bytes == 0,
          "Worker budget not released");
}
} // namespace
int wmain(int argc, wchar_t **argv) {
  try {
    require(
        argc == 3,
        "Usage: professional_tests image|abc|preview|background EXTERNAL_DIR");
    const std::filesystem::path dir(argv[2]);
    std::filesystem::create_directories(dir);
    const std::wstring_view test(argv[1]);
    if (test == L"image")
      images(dir);
    else if (test == L"abc")
      caches(dir);
    else if (test == L"preview")
      preview_test();
    else if (test == L"background")
      background(dir);
    else
      throw std::runtime_error("Unknown test");
    std::cout << "PASS\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
