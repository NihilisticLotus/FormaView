#include "formaview/file_access.hpp"
#include "formaview/image_conversion.hpp"
#include "formaview/image_requests.hpp"
#include <QCoreApplication>
#include <QColorSpace>
#include <QtEndian>
#include <QJsonArray>
#include <QFile>
#include <fstream>
#include <iostream>

using namespace formaview;
using namespace formaview::imaging;
namespace {
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
void write(const std::filesystem::path &path,
           const std::vector<std::byte> &bytes) {
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  require(static_cast<bool>(file), "Output failed");
}
template <class F> void rejects(F f) {
  try {
    f();
  } catch (const skeletal::Failure &) {
    return;
  }
  throw std::runtime_error("Expected structured rejection");
}
bool loss(const ImageConversionProduct &p, const std::string &code) {
  return std::any_of(p.losses.begin(), p.losses.end(),
                     [&](const auto &l) { return l.code == code; });
}
void profile_layouts(const ImageSubresource &rgba, const std::filesystem::path &root) {
  const auto raw = [](const QColorSpace &space) {
    require(space.isValid(), "Invalid generated colour profile");
    const auto bytes = space.iccProfile();
    require(bytes.size() >= 128, "Missing generated ICC header");
    return std::vector<std::byte>(reinterpret_cast<const std::byte *>(bytes.constData()),
                                 reinterpret_cast<const std::byte *>(bytes.constData()) + bytes.size());
  };
  const auto rgbProfile = raw(QColorSpace(QColorSpace::SRgb));
  const auto grayProfile = raw(QColorSpace(QPointF(0.3127, 0.3290), QColorSpace::TransferFunction::Gamma, 2.2f));
  write(root / "rgb.icc", rgbProfile); write(root / "gray.icc", grayProfile);
  auto rgb = rgba; rgb.channels.pop_back();
  auto gray = rgb; gray.channels.resize(1); gray.channels[0].name = "Y";
  auto grayAlpha = gray; grayAlpha.channels.push_back(rgba.channels.back());
  const auto mismatch = [](const auto &encode) {
    try { encode(); }
    catch (const skeletal::Failure &e) {
      require(e.error.code == ErrorCode::Unsupported &&
                  std::string(e.what()).find("ICC colour model") != std::string::npos,
              "Wrong ICC model mismatch diagnostic");
      return;
    }
    throw std::runtime_error("Encoder accepted an ICC profile for the wrong output colour model");
  };
  const auto check = [&](const ImageSubresource &image, bool isGray, bool hasAlpha) {
    for (const auto target : {ImageTarget::Png, ImageTarget::Jpeg, ImageTarget::Tiff, ImageTarget::Webp}) {
      if ((hasAlpha && target == ImageTarget::Jpeg) || (isGray && target == ImageTarget::Webp)) continue;
      const auto encode = [&](const std::vector<std::byte> &profile) {
        if (target == ImageTarget::Tiff || target == ImageTarget::Webp) {
          ExtendedEncodeOptions o;
          o.target = target == ImageTarget::Tiff ? ExtendedRasterTarget::Tiff : ExtendedRasterTarget::Webp;
          o.alpha = hasAlpha ? AlphaMode::Straight : AlphaMode::Absent;
          o.icc_profile = profile;
          return encode_extended(image, o);
        }
        RasterEncodeOptions o;
        o.target = target == ImageTarget::Png ? RasterTarget::Png : RasterTarget::Jpeg;
        o.alpha = hasAlpha ? AlphaMode::Straight : AlphaMode::Absent;
        o.icc_profile = profile;
        return encode_raster(image, o);
      };
      mismatch([&] { write(root / "mismatched-profile.bin", encode(isGray ? rgbProfile : grayProfile)); });
      const auto &matching = isGray ? grayProfile : rgbProfile;
      const auto path = root / ("matching-" + std::to_string(static_cast<int>(target)) +
                                (isGray ? "-gray" : "-rgb") + (hasAlpha ? "-alpha.bin" : ".bin"));
      write(path, encode(matching));
      const auto doc = ImageDocument::open(path);
      require(doc.semantics(0).icc_profile_sha256 == skeletal::sha256(matching),
              "Matching ICC profile bytes changed");
    }
  };
  check(rgb, false, false); check(rgba, false, true);
  check(gray, true, false); check(grayAlpha, true, true);
  auto largeProfile = rgbProfile;
  largeProfile.resize(150000, std::byte{0}); // Unused ICC padding preserves the transfer tags.
  qToBigEndian<std::uint32_t>(static_cast<std::uint32_t>(largeProfile.size()), largeProfile.data());
  RasterEncodeOptions segmented; segmented.target = RasterTarget::Jpeg; segmented.icc_profile = largeProfile;
  write(root / "large-profile.jpg", encode_raster(rgb, segmented));
  require(ImageDocument::open(root / "large-profile.jpg").semantics(0).icc_profile_sha256 == skeletal::sha256(largeProfile),
          "Multi-segment JPEG ICC bytes changed");
  Limits smallBudget; smallBudget.input_bytes = 10000;
  bool budgetRejected = false;
  try { (void)encode_raster(rgb, segmented, smallBudget); }
  catch (const skeletal::Failure &e) { budgetRejected = e.error.code == ErrorCode::ResourceLimit; }
  require(budgetRejected, "JPEG ICC segments exceeded the output budget");
  const auto path = root / "rgb-profile.png";
  RasterEncodeOptions tagged; tagged.icc_profile = rgbProfile;
  write(path, encode_raster(rgb, tagged));
  const auto doc = ImageDocument::open(path);
  for (const auto target : {ImageTarget::Png, ImageTarget::Jpeg, ImageTarget::Tiff}) {
    ImageConversionOptions options; options.target = target; options.allow_lossy = true;
    options.transform.channels = {{"R", "Y"}};
    mismatch([&] { (void)prepare_image_conversion(doc, options); });
    options.metadata = MetadataPolicy::Drop;
    const auto dropped = prepare_image_conversion(doc, options);
    require(loss(dropped, "icc_profile"), "Explicit ICC drop must report loss");
    write(root / "dropped-profile.bin", dropped.encoded);
    const auto decoded = ImageDocument::open(root / "dropped-profile.bin");
    require(decoded.semantics(0).icc_profile_sha256.empty(), "Dropped ICC was retained");
    require(decoded.decode(0).channels.front().samples.bytes == rgb.channels.front().samples.bytes,
            "Metadata drop changed gray channel samples");
  }
  const auto cmykPath = qEnvironmentVariable("FORMAVIEW_TEST_CMYK_PROFILE");
  if (!cmykPath.isEmpty()) {
    QFile file(cmykPath); require(file.open(QIODevice::ReadOnly), "Cannot read CMYK ICC fixture");
    const auto bytes = file.readAll();
    require(bytes.size() >= 128 && bytes.mid(16, 4) == "CMYK", "Fixture is not a CMYK profile");
    const std::vector<std::byte> profile(reinterpret_cast<const std::byte *>(bytes.constData()),
                                      reinterpret_cast<const std::byte *>(bytes.constData()) + bytes.size());
    for (const auto target : {ExtendedRasterTarget::Tiff, ExtendedRasterTarget::Webp}) {
      ExtendedEncodeOptions o; o.target = target; o.icc_profile = profile;
      mismatch([&] { (void)encode_extended(rgb, o); });
    }
    std::cout << "PASS external CMYK ICC rejected for RGB TIFF/WebP\n";
  } else std::cout << "SKIP external CMYK ICC: FORMAVIEW_TEST_CMYK_PROFILE is unset\n";
  std::cout << "PASS RGB/gray ICC output models, exact profile bytes and explicit metadata drop\n";
}
void dds_alpha_contract(const ImageSubresource &rgba, const std::filesystem::path &root) {
  // Alpha presence follows the format and declaration, never its pixel values.
  for (const auto mode : {AlphaMode::Absent, AlphaMode::Straight,
                         AlphaMode::Premultiplied, AlphaMode::Unknown}) {
    for (const auto sample : {0U, 127U, 255U}) {
      auto input = rgba;
      std::fill(input.channels[3].samples.bytes.begin(), input.channels[3].samples.bytes.end(), std::byte(sample));
      ProfessionalEncodeOptions encoded;
      encoded.target = ProfessionalTarget::Dds; encoded.alpha = mode;
      if (mode == AlphaMode::Absent) {
        rejects([&] { (void)encode_professional(input, encoded); });
        input.channels.pop_back();
      }
      const auto name = "dds-alpha-" + std::to_string(static_cast<int>(mode)) + '-' + std::to_string(sample);
      write(root / (name + ".dds"), encode_professional(input, encoded));
      const auto doc = ImageDocument::open(root / (name + ".dds"));
      require(doc.description().alpha == mode, "DDS alpha declaration changed");
      const auto decoded = doc.decode(0);
      require(decoded.channels.size() == (mode == AlphaMode::Absent ? 3 : 4),
              "DDS opaque padding was exposed as a real alpha channel");
      ImageConversionOptions options;
      options.target = ImageTarget::Tga;
      options.transform.source_alpha = mode;
      for (const auto &channel : decoded.channels) options.transform.channels.push_back({channel.name, channel.name});
      if (mode != AlphaMode::Absent) options.transform.alpha_channel = "A";
      auto result = prepare_image_conversion(doc, options);
      write(root / (name + ".tga"), result.encoded);
      const auto roundtrip = ImageDocument::open(root / (name + ".tga")).decode(0);
      require(roundtrip.channels.size() == decoded.channels.size(), "DDS/TGA alpha presence changed");
      for (std::size_t c = 0; c < roundtrip.channels.size(); ++c)
        require(roundtrip.channels[c].samples.bytes == input.channels[c].samples.bytes,
                "DDS/TGA hidden RGB or alpha values changed");
    }
  }
  // Native RGB uses RGBA storage in UInt8 DDS, but the fourth component is padding.
  auto rgb = rgba; rgb.channels.pop_back();
  ProfessionalEncodeOptions encoded; encoded.target = ProfessionalTarget::Dds;
  auto bytes = encode_professional(rgb, encoded);
  write(root / "dds-rgb-padding.dds", bytes);
  require(ImageDocument::open(root / "dds-rgb-padding.dds").decode(0).channels.size() == 3,
          "RGB DDS round trip fabricated alpha");
  auto broken = bytes;
  broken[151] = std::byte{};
  write(root / "dds-opaque-contradiction.dds", broken);
  rejects([&] { (void)ImageDocument::open(root / "dds-opaque-contradiction.dds").decode(0); });
  // An X8 format has no alpha even when its metadata says unknown (legacy DDS).
  qToLittleEndian<quint32>(88, reinterpret_cast<uchar *>(bytes.data()) + 128); // B8G8R8X8_UNORM
  qToLittleEndian<quint32>(0, reinterpret_cast<uchar *>(bytes.data()) + 144);
  write(root / "dds-bgrx-unknown.dds", bytes);
  const auto bgrx = ImageDocument::open(root / "dds-bgrx-unknown.dds");
  require(bgrx.description().alpha == AlphaMode::Absent, "X8 DDS must not claim alpha");
  const auto decoded = bgrx.decode(0);
  require(decoded.channels.size() == 3 && decoded.channels[0].name == "B" &&
          decoded.channels[0].samples.bytes == rgb.channels[0].samples.bytes,
          "BGRX source channel order/stride changed");
  std::cout << "PASS DDS opaque/straight/premultiplied/unknown alpha, zero/partial/opaque values, RGB padding and BGRX\n";
}
void run(const std::filesystem::path &root) {
  const auto capabilities = execute_image_request(
      {{"version", 1}, {"operation", "capabilities"}}, {});
  require(capabilities["read_formats"].toArray().size() == 11,
          "Required image handlers are unavailable");
  require(capabilities["write_targets"].toArray().size() == 9,
          "Required image encoders are unavailable");
  ImageSubresource source;
  source.data_window = source.display_window = {0, 0, 8, 8};
  for (std::size_t c = 0; c < 4; ++c) {
    Buffer b;
    b.scalar = ScalarType::UInt8;
    b.element_count = 64;
    b.bytes.resize(64, static_cast<std::byte>(32 + c * 64));
    source.channels.push_back({std::array{"R", "G", "B", "A"}[c], std::move(b),
                               SampleEncoding::UNorm});
  }
  RasterEncodeOptions png;
  dds_alpha_contract(source, root);
  png.alpha = AlphaMode::Straight;
  write(root / "source.png", encode_raster(source, png));
  profile_layouts(source, root);
  const auto iccPath = qEnvironmentVariable("FORMAVIEW_TEST_ICC_PROFILE");
  if (!iccPath.isEmpty()) {
    QFile profile(iccPath);
    require(profile.open(QIODevice::ReadOnly), "Cannot read external ICC profile");
    const auto bytes = profile.readAll();
    require(!bytes.isEmpty() && bytes.size() <= 1024 * 1024, "Invalid ICC fixture size");
    auto rgb = source;
    rgb.channels.pop_back();
    for (const auto target : {RasterTarget::Png, RasterTarget::Jpeg}) {
      RasterEncodeOptions encoded;
      encoded.target = target;
      encoded.icc_profile.assign(reinterpret_cast<const std::byte *>(bytes.constData()),
          reinterpret_cast<const std::byte *>(bytes.constData()) + bytes.size());
      const auto path = root / (target == RasterTarget::Png ? "icc.png" : "icc.jpg");
      write(path, encode_raster(rgb, encoded));
      const auto loaded = RasterFile::open(path);
      const auto &properties = loaded.description().source.properties;
      const auto found = properties.find("entry.0.icc_base64");
      require(found != properties.end() &&
          QByteArray::fromBase64(QByteArray::fromStdString(found->second)) == bytes,
          "ICC bytes changed across raster encoding and metadata extraction");
    }
    std::cout << "PASS external ICC exact bytes across PNG and JPEG\n";
  } else {
    std::cout << "SKIP external ICC: FORMAVIEW_TEST_ICC_PROFILE is unset\n";
  }
#ifdef _WIN32
  AccessScope scope;
  scope.input_roots = {root};
  scope.output_roots = {root};
  auto lease = InputLease::open(scope, root / "source.png", 1024 * 1024);
#endif
  auto doc = ImageDocument::open(root / "source.png");
  require(doc.resident_bytes() > 0, "Missing source storage");
  ImageConversionOptions options;
  options.transform.channels = {{"R", "R"}, {"G", "G"}, {"B", "B"}, {"A", "A"}};
  rejects([&] { (void)prepare_image_conversion(doc, options); });
  options.transform.alpha_channel = "A";
  options.transform.source_alpha = AlphaMode::Straight;
  {
    auto unknown = options;
    unknown.target = ImageTarget::Tga;
    unknown.transform.source_alpha = AlphaMode::Unknown;
    const auto encoded = prepare_image_conversion(doc, unknown);
    write(root / "unknown-alpha.tga", encoded.encoded);
    const auto tga = ImageDocument::open(root / "unknown-alpha.tga");
    require(tga.description().alpha == AlphaMode::Unknown, "Unknown alpha must not become straight");
    auto zero = encoded.encoded;
    for (std::size_t p = 18 + 3; p < 18 + 8 * 8 * 4; p += 4) zero[p] = std::byte{0};
    write(root / "zero-alpha.tga", zero);
    const auto zeroDoc = ImageDocument::open(root / "zero-alpha.tga");
    const auto zeroImage = zeroDoc.decode(0);
    require(zeroImage.channels.size() == 4 &&
                std::all_of(zeroImage.channels.back().samples.bytes.begin(), zeroImage.channels.back().samples.bytes.end(),
                            [](auto v) { return v == std::byte{0}; }),
            "All-zero source alpha must remain a real zero channel");
    (void)prepare_image_conversion(zeroDoc, unknown);
    const auto original = doc.decode(0), roundtrip = tga.decode(0);
    require(roundtrip.channels.size() == original.channels.size(), "Unknown alpha channel retained");
    for (std::size_t c = 0; c < original.channels.size(); ++c)
      require(roundtrip.channels[c].name == original.channels[c].name &&
                  roundtrip.channels[c].samples.bytes == original.channels[c].samples.bytes,
              "Unknown alpha raw conversion changes channel samples");
    auto drop = unknown;
    drop.transform.alpha_action = AlphaAction::Drop;
    drop.transform.channels.pop_back();
    require(loss(prepare_image_conversion(tga, drop), "alpha_removed"), "Unknown alpha drop must report loss");
    auto composite = drop; composite.transform.alpha_action = AlphaAction::Composite;
    rejects([&] { (void)prepare_image_conversion(tga, composite); });
    auto colour = unknown; colour.transform.meaning = ImageMeaning::Colour;
    colour.transform.transfer = Transfer::LinearToSrgb;
    rejects([&] { (void)prepare_image_conversion(tga, colour); });
    auto pngUnknown = unknown; pngUnknown.target = ImageTarget::Png;
    rejects([&] { (void)prepare_image_conversion(tga, pngUnknown); });
    for (const auto mode : {AlphaMode::Straight, AlphaMode::Premultiplied}) {
      auto tagged = unknown; tagged.transform.source_alpha = mode;
      const auto bytes = prepare_image_conversion(tga, tagged).encoded;
      write(root / "tagged-alpha.tga", bytes);
      require(ImageDocument::open(root / "tagged-alpha.tga").description().alpha == mode, "Explicit alpha type retained");
    }
    std::cout << "PASS unknown alpha raw TGA, drop, typed tags and operation/PNG restrictions\n";
  }
  auto reordered = options;
  std::reverse(reordered.transform.channels.begin(),
               reordered.transform.channels.end());
  require(prepare_image_conversion(doc, reordered).encoded ==
              prepare_image_conversion(doc, options).encoded,
          "Named channel enumeration order changed raster output");
  for (const auto target :
       {ImageTarget::Png, ImageTarget::Jpeg, ImageTarget::Bmp, ImageTarget::Tga,
        ImageTarget::Tiff, ImageTarget::Webp, ImageTarget::Dds,
        ImageTarget::Hdr, ImageTarget::Exr}) {
    auto o = options;
    o.target = target;
    if (target == ImageTarget::Jpeg || target == ImageTarget::Bmp ||
        target == ImageTarget::Hdr) {
      o.transform.alpha_action = AlphaAction::Drop;
      o.transform.channels.pop_back();
    }
    if (target == ImageTarget::Jpeg || target == ImageTarget::Hdr) {
      rejects([&] { (void)prepare_image_conversion(doc, o); });
      o.allow_lossy = true;
    }
    if (target == ImageTarget::Exr || target == ImageTarget::Hdr) {
      o.transform.output_scalar = ScalarType::Float32;
      o.transform.quantization = Quantization::ClampRound;
    }
    auto result = prepare_image_conversion(doc, o);
    require(result.input_sha256 == doc.description().source.identity.sha256,
            "Content identity changed");
    require(!result.encoded.empty(), "Empty output");
    if (target == ImageTarget::Jpeg || target == ImageTarget::Hdr)
      require(loss(result, "lossy_codec"), "Unreported codec loss");
    if (o.transform.alpha_action == AlphaAction::Drop)
      require(loss(result, "alpha_removed"), "Unreported alpha loss");
    const auto path =
        root / ("converted." + std::string(image_target_name(target)));
#ifdef _WIN32
    require(result.input_sha256 == lease.snapshot().content_sha256,
            "Held snapshot differs from decoded bytes");
    OutputTransaction output(scope, path, lease.snapshot(),
                             ConflictPolicy::Skip);
    output.write(result.encoded);
    require(!output.commit().skipped, "Unexpected output conflict");
#else
    write(path, result.encoded);
#endif
    auto reopened = ImageDocument::open(path);
    auto decoded = reopened.decode(0);
    require(decoded.data_window.width == 8 && decoded.data_window.height == 8,
            "Output dimensions changed");
    for (const auto &c : decoded.channels) {
      const auto it =
          std::find_if(source.channels.begin(), source.channels.end(),
                       [&](const auto &s) { return c.name == s.name; });
      require(it != source.channels.end(), "Unexpected output channel");
      const auto error =
          std::abs(interpreted_value(c, 0) - interpreted_value(*it, 0));
      require(error < (target == ImageTarget::Jpeg  ? .02
                       : target == ImageTarget::Hdr ? .008
                                                    : 1e-6),
              "Output numeric mismatch");
    }
  }
  auto wrong = options;
  wrong.target = static_cast<ImageTarget>(999);
  rejects([&] { (void)prepare_image_conversion(doc, wrong); });
  wrong = options;
  wrong.subresource = 1;
  rejects([&] { (void)prepare_image_conversion(doc, wrong); });
  std::stop_source stop;
  stop.request_stop();
  rejects([&] {
    (void)prepare_image_conversion(doc, options, {}, stop.get_token());
  });
  const auto source_hash = doc.description().source.identity.sha256;
  require(ImageDocument::open(root / "source.png")
                  .description()
                  .source.identity.sha256 == source_hash,
          "Conversion changed source");
}
} // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    const auto args = app.arguments();
    if (args.size() != 2)
      throw std::runtime_error("EXTERNAL_DIRECTORY required");
    const std::filesystem::path root(args[1].toStdWString());
    std::filesystem::create_directories(root);
    run(root);
    std::cout << "PASS shared image conversion across nine targets, identity, "
                 "losses and cancellation\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
