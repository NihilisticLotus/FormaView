#include "formaview/file_access.hpp"
#include "formaview/image_conversion.hpp"
#include "formaview/image_requests.hpp"
#include <QCoreApplication>
#include <QJsonArray>
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
  png.alpha = AlphaMode::Straight;
  write(root / "source.png", encode_raster(source, png));
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
