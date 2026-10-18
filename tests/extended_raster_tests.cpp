#include "formaview/extended_raster.hpp"
#include <fstream>
#include <iostream>

using namespace formaview;
using namespace formaview::imaging;
namespace {
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
template <class F> void rejects(F f, ErrorCode expected) {
  try {
    f();
  } catch (const skeletal::Failure &e) {
    require(e.error.code == expected, "Wrong structured error code");
    return;
  }
  throw std::runtime_error("Expected rejection");
}
void write(const std::filesystem::path &path,
           const std::vector<std::byte> &bytes) {
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char *>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
  require(static_cast<bool>(f), "Fixture write failed");
}
ImageSubresource source() {
  ImageSubresource s;
  s.data_window = s.display_window = {0, 0, 32, 32};
  for (std::size_t c = 0; c < 4; ++c) {
    Buffer b;
    b.scalar = ScalarType::UInt8;
    b.element_count = 1024;
    b.bytes.resize(1024);
    for (std::size_t i = 0; i < 1024; ++i)
      b.bytes[i] = static_cast<std::byte>(
          c == 3 ? (i == 1023 ? 128 : 0) : (i * (c + 1) + c * 43) % 256);
    s.channels.push_back({std::array{"R", "G", "B", "A"}[c], std::move(b),
                          SampleEncoding::UNorm});
  }
  return s;
}
void run(const std::filesystem::path &root) {
  auto s = source();
  const auto original = s.channels[0].samples.bytes;
  ExtendedEncodeOptions o;
  o.alpha = AlphaMode::Straight;
  o.icc_profile = {
      std::byte{0x12},
      std::byte{
          0x34}}; // Opaque metadata bytes, not a claimed valid colour profile.
  for (const auto target :
       {ExtendedRasterTarget::Tiff, ExtendedRasterTarget::Webp}) {
    o.target = target;
    auto bytes = encode_extended(s, o);
    const auto path =
        root /
        (target == ExtendedRasterTarget::Tiff ? "rgba.tiff" : "rgba.webp");
    write(path, bytes);
    auto f = ExtendedRasterFile::open(path);
    auto r = f.decode(0);
    require(f.description().alpha == AlphaMode::Straight &&
                r.channels.size() == 4,
            "Alpha metadata lost");
    require(f.description().source.properties.at(
                target == ExtendedRasterTarget::Tiff ? "page.0.icc_hex"
                                                     : "icc_hex") == "1234",
            "ICC bytes changed");
    for (std::size_t c = 0; c < 4; ++c)
      require(r.channels[c].samples.bytes == s.channels[c].samples.bytes,
              "Lossless/transparent RGB changed");
    Limits tiny;
    tiny.input_bytes = 16;
    rejects([&] { (void)encode_extended(s, o, tiny); },
            ErrorCode::ResourceLimit);
    tiny = {};
    tiny.decoded_bytes = 32;
    rejects([&] { (void)encode_extended(s, o, tiny); },
            ErrorCode::ResourceLimit);
    std::stop_source stop;
    stop.request_stop();
    rejects([&] { (void)encode_extended(s, o, {}, stop.get_token()); },
            ErrorCode::Cancelled);
    rejects([&] { (void)ExtendedRasterFile::open(path, {}, stop.get_token()); },
            ErrorCode::Cancelled);
    rejects([&] { (void)f.decode(0, stop.get_token()); }, ErrorCode::Cancelled);
    rejects([&] { (void)f.decode(1); }, ErrorCode::InvalidArgument);
    o.alpha = AlphaMode::Unknown;
    rejects([&] { (void)encode_extended(s, o); }, ErrorCode::InvalidArgument);
    o.alpha = AlphaMode::Straight;
  }
  o.target = ExtendedRasterTarget::Webp;
  o.webp_lossless = false;
  rejects([&] { (void)encode_extended(s, o); }, ErrorCode::InvalidArgument);
  o.allow_lossy = true;
  write(root / "lossy.webp", encode_extended(s, o));
  auto lossy = ExtendedRasterFile::open(root / "lossy.webp").decode(0);
  require(lossy.channels[3].samples.bytes == s.channels[3].samples.bytes,
          "Lossy WebP alpha changed");
  o.alpha = AlphaMode::Premultiplied;
  rejects([&] { (void)encode_extended(s, o); }, ErrorCode::InvalidArgument);
  o.target = ExtendedRasterTarget::Tiff;
  for (auto compression :
       {TiffCompression::None, TiffCompression::Lzw, TiffCompression::Deflate,
        TiffCompression::PackBits}) {
    o.compression = compression;
    const auto path =
        root / ("compression-" + std::to_string(static_cast<int>(compression)) +
                ".tiff");
    write(path, encode_extended(s, o));
    auto f = ExtendedRasterFile::open(path);
    require(f.description().alpha == AlphaMode::Premultiplied,
            "Premultiplied TIFF tag changed");
    auto r = f.decode(0);
    for (std::size_t c = 0; c < 4; ++c)
      require(r.channels[c].samples.bytes == s.channels[c].samples.bytes,
              "TIFF compression changed values");
  }
  require(s.channels[0].samples.bytes == original, "Encoding modified source");
}
} // namespace
int wmain(int argc, wchar_t **argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("EXTERNAL_DIRECTORY required");
    std::filesystem::path root(argv[1]);
    std::filesystem::create_directories(root);
    run(root);
    std::cout << "PASS TIFF/WebP exactness, alpha, ICC, budgets, cancellation "
                 "and rejection\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
