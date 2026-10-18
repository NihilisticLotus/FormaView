#include "formaview/pro_image_write.hpp"
#include <DirectXTex.h>
#include <Imath/half.h>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>

namespace {
using namespace formaview;
using namespace formaview::imaging;
void require(bool ok, const char *s) {
  if (!ok)
    throw std::runtime_error(s);
}
void write(const std::filesystem::path &p,
           const std::vector<std::byte> &bytes) {
  std::ofstream f(p, std::ios::binary);
  f.write(reinterpret_cast<const char *>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
  require(static_cast<bool>(f), "Fixture output failed");
}
template <class F> void rejects(F f) {
  try {
    f();
  } catch (const skeletal::Failure &) {
    return;
  }
  throw std::runtime_error("Expected structured rejection");
}
ImageSubresource source(std::size_t count = 4) {
  ImageSubresource s;
  s.data_window = s.display_window = {0, 0, 8, 8};
  const std::array<std::string, 4> names{"R", "G", "B", "A"};
  const std::array<float, 4> values{.25F, .5F, .75F, .5F};
  for (std::size_t c = 0; c < count; ++c) {
    ImageChannel ch;
    ch.name = names[c];
    ch.samples.scalar = ScalarType::Float32;
    ch.samples.element_count = 64;
    ch.samples.bytes.resize(64 * 4);
    for (std::size_t i = 0; i < 64; ++i)
      std::memcpy(ch.samples.bytes.data() + i * 4, &values[c], 4);
    s.channels.push_back(std::move(ch));
  }
  return s;
}
void run(const std::filesystem::path &dir) {
  auto s = source();
  s.data_window.x = -3;
  s.data_window.y = 7;
  s.part_name = "actual.beauty";
  s.channels[0].name = "beauty.R";
  ProfessionalEncodeOptions o;
  for (bool tiled : {false, true}) {
    o.exr_tiled = tiled;
    const auto path = dir / (tiled ? "tiled.exr" : "scanline.exr");
    write(path, encode_professional(s, o));
    auto file = ImageFile::open(path);
    auto r = file.decode(0);
    require(r.data_window.x == -3 && r.data_window.y == 7 &&
                r.part_name == s.part_name,
            "EXR window/name changed");
    for (const auto &ch : s.channels) {
      bool found = false;
      for (const auto &out : r.channels)
        if (out.name == ch.name) {
          found = true;
          require(out.samples.bytes == ch.samples.bytes,
                  "EXR float bits changed");
        }
      require(found, "EXR channel name changed");
    }
  }
  s = source(3);
  o = {};
  o.target = ProfessionalTarget::Hdr;
  rejects([&] { (void)encode_professional(s, o); });
  o.allow_lossy = true;
  write(dir / "output.hdr", encode_professional(s, o));
  auto hdr = ImageFile::open(dir / "output.hdr").decode(0);
  for (std::size_t c = 0; c < 3; ++c)
    require(std::abs(value(hdr.channels[c], 0) - (.25 * (c + 1))) < .005,
            "RGBE round trip out of tolerance");
  s = source();
  TransformOptions tr;
  tr.channels = {{"R", "R"}, {"G", "G"}, {"B", "B"}, {"A", "A"}};
  tr.alpha_channel = "A";
  tr.source_alpha = AlphaMode::Straight;
  tr.output_scalar = ScalarType::UInt8;
  tr.quantization = Quantization::ClampRound;
  auto integer = transform(s, tr).image;
  o = {};
  o.target = ProfessionalTarget::Tga;
  o.alpha = AlphaMode::Straight;
  write(dir / "output.tga", encode_professional(integer, o));
  auto tga = ImageFile::open(dir / "output.tga").decode(0);
  for (std::size_t c = 0; c < 4; ++c)
    require(tga.channels[c].samples.bytes == integer.channels[c].samples.bytes,
            "TGA pixel mismatch");
  for (const auto count : {1U, 3U, 4U}) {
    auto input = integer;
    input.channels.resize(count);
    if (count == 1)
      input.channels[0].name = "Y";
    if (count == 4)
      std::fill(input.channels[3].samples.bytes.begin(),
                input.channels[3].samples.bytes.end(), std::byte{});
    const auto path = dir / ("tga-" + std::to_string(count) + ".tga");
    write(path, encode_professional(input, o));
    auto file = ImageFile::open(path);
    auto result = file.decode(0);
    require(result.channels.size() == count,
            "TGA fabricated/missing source channel");
    for (std::size_t c = 0; c < count; ++c) {
      require(result.channels[c].name == input.channels[c].name,
              "TGA channel meaning changed");
      require(result.channels[c].samples.bytes ==
                  input.channels[c].samples.bytes,
              "TGA actual channel bytes changed");
    }
    require(count == 4 || file.description().alpha == AlphaMode::Absent,
            "TGA absent alpha misclassified");
  }
  o.target = ProfessionalTarget::Dds;
  write(dir / "native.dds", encode_professional(integer, o));
  auto native = ImageFile::open(dir / "native.dds").decode(0);
  for (std::size_t c = 0; c < 4; ++c)
    require(native.channels[c].samples.bytes ==
                integer.channels[c].samples.bytes,
            "DDS native bytes changed");
  for (const auto format :
       {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC3_UNORM,
        DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC4_SNORM, DXGI_FORMAT_BC5_UNORM,
        DXGI_FORMAT_BC5_SNORM, DXGI_FORMAT_BC6H_UF16, DXGI_FORMAT_BC6H_SF16,
        DXGI_FORMAT_BC7_UNORM, DXGI_FORMAT_BC7_UNORM_SRGB}) {
    const auto count =
        format == DXGI_FORMAT_BC4_UNORM || format == DXGI_FORMAT_BC4_SNORM ? 1U
        : format == DXGI_FORMAT_BC5_UNORM || format == DXGI_FORMAT_BC5_SNORM
            ? 2U
        : format == DXGI_FORMAT_BC6H_UF16 || format == DXGI_FORMAT_BC6H_SF16
            ? 3U
            : 4U;
    auto input = source(count);
    o.dds_format = static_cast<std::uint32_t>(format);
    o.allow_lossy = true;
    const auto path = dir / ("bc-" + std::to_string(format) + ".dds");
    write(path, encode_professional(input, o));
    auto result = ImageFile::open(path).decode(0);
    require(result.channels.size() == count,
            "BC actual channel count mismatch");
    for (std::size_t c = 0; c < std::min<std::size_t>(count, 3); ++c)
      require(std::abs(value(result.channels[c], 0) - (.25 * (c + 1))) < .04,
              "BC colour numeric error");
  }
  o.dds_format = DXGI_FORMAT_BC4_UNORM;
  rejects([&] { (void)encode_professional(source(4), o); });
  for (const auto format :
       {DXGI_FORMAT_BC4_SNORM, DXGI_FORMAT_BC5_SNORM, DXGI_FORMAT_BC6H_SF16}) {
    const auto count = format == DXGI_FORMAT_BC4_SNORM   ? 1U
                       : format == DXGI_FORMAT_BC5_SNORM ? 2U
                                                         : 3U;
    auto input = source(count);
    for (std::size_t c = 0; c < count; ++c)
      for (std::size_t i = 0; i < 64; ++i) {
        const auto magnitude = format == DXGI_FORMAT_BC6H_SF16 ? 4.F : 1.F;
        const float v = -.25F * static_cast<float>(c + 1) * magnitude;
        std::memcpy(input.channels[c].samples.bytes.data() + 4 * i, &v, 4);
      }
    o.dds_format = static_cast<std::uint32_t>(format);
    const auto path = dir / ("negative-" + std::to_string(format) + ".dds");
    write(path, encode_professional(input, o));
    auto result = ImageFile::open(path).decode(0);
    for (std::size_t c = 0; c < count; ++c)
      require(std::abs(value(result.channels[c], 0) -
                       value(input.channels[c], 0)) < .04,
              "Signed BC lost negative values");
  }
  o = {};
  std::stop_source stop;
  stop.request_stop();
  rejects(
      [&] { (void)encode_professional(source(), o, {}, stop.get_token()); });
  Limits limits;
  limits.decoded_bytes = 4;
  rejects([&] { (void)encode_professional(source(), o, limits); });
}
} // namespace
int wmain(int argc, wchar_t **argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("pro_image_write_tests EXTERNAL_DIRECTORY");
    std::filesystem::path root(argv[1]);
    std::filesystem::create_directories(root);
    run(root);
    std::cout << "PASS EXR/HDR/TGA/DDS native + BC1-7 and explicit rejection\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
