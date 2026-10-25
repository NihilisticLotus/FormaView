#include "formaview/image_transform.hpp"
#include <Imath/half.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

namespace {
using namespace formaview;
using namespace formaview::imaging;
void require(bool v, const char *s) { if (!v) throw std::runtime_error(s); }
template<class F> void rejects(F f, ErrorCode code = ErrorCode::InvalidArgument) {
  try { f(); } catch (const skeletal::Failure &e) {
    require(e.error.code == code, "Unexpected structured error code"); return;
  }
  throw std::runtime_error("Expected rejection");
}
template<class T> ImageChannel channel(std::string name, ScalarType type,
                                    std::initializer_list<T> values,
                                    SampleEncoding encoding = SampleEncoding::Numeric) {
  ImageChannel c;
  c.name = std::move(name); c.samples.scalar = type; c.encoding = encoding;
  c.samples.element_count = values.size(); c.samples.bytes.resize(values.size() * sizeof(T));
  std::memcpy(c.samples.bytes.data(), values.begin(), c.samples.bytes.size()); return c;
}
ImageSubresource rgba() {
  ImageSubresource s;
  s.data_window = {-32, 16, 2, 1}; s.display_window = {0, 0, 100, 50};
  s.part = 3; s.part_name = "beauty";
  s.channels = {channel<float>("beauty.R", ScalarType::Float32, {.125F, -2.F}),
                channel<float>("beauty.G", ScalarType::Float32, {.25F, .5F}),
                channel<float>("beauty.B", ScalarType::Float32, {2.F, 1.F}),
                channel<float>("beauty.A", ScalarType::Float32, {.5F, 0.F})};
  return s;
}
TransformOptions options() {
  TransformOptions o;
  o.channels = {{"beauty.R", "R"}, {"beauty.G", "G"}, {"beauty.B", "B"}, {"beauty.A", "A"}};
  o.alpha_channel = "beauty.A"; o.source_alpha = AlphaMode::Straight; return o;
}
void preservation() {
  auto source = rgba(); auto o = options();
  source.channels[0] = channel<float>("beauty.R", ScalarType::Float32,
      {-0.F, std::numeric_limits<float>::quiet_NaN()});
  const auto original = source.channels[0].samples.bytes;
  auto out = transform(source, o);
  require(out.image.channels[0].samples.bytes == original && source.channels[0].samples.bytes == original,
          "Source bits or NaN/negative-zero changed");
  require(out.image.data_window.x == -32 && out.image.display_window.width == 100 &&
          out.image.part_name == "beauty" && out.image.part == 3, "Window or part identity lost");
  source.channels[0] = channel<std::uint32_t>("beauty.R", ScalarType::UInt32, {16777217U, 4294967295U});
  out = transform(source, o);
  require(value(out.image.channels[0], 0) == 16777217. && value(out.image.channels[0], 1) == 4294967295.,
          "UINT narrowed through float");
  o.channels.pop_back(); rejects([&]{ (void)transform(source, o); });
  o.alpha_action = AlphaAction::Drop;
  out = transform(source, o); require(out.report.alpha_removed && out.report.omitted_channels.size() == 1,
                                    "Alpha omission not reported");
}
void quantization() {
  auto source = rgba(); auto o = options(); o.output_scalar = ScalarType::UInt8;
  rejects([&]{ (void)transform(source, o); });
  o.quantization = Quantization::ClampRound;
  auto out = transform(source, o);
  require(value(out.image.channels[0], 0) == 32 && value(out.image.channels[0], 1) == 0 &&
          value(out.image.channels[2], 0) == 255 && value(out.image.channels[3], 0) == 128,
          "Clamp/round or alpha quantization differs from explicit rule");
  require(out.report.clipped == 2 && out.report.samples == 8 && out.report.precision_changed == 6,
          "Quantization denominators incorrect");
  source.channels[0] = channel<float>("beauty.R", ScalarType::Float32,
      {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()});
  rejects([&]{ (void)transform(source, o); });
  o.non_finite = NonFinite::ReplaceZero; out = transform(source, o);
  require(out.report.non_finite_replaced == 2 && value(out.image.channels[0], 0) == 0,
          "Non-finite replacement not explicit/accounted");
  ImageSubresource integers; integers.data_window = {0, 0, 3, 1};
  integers.channels = {channel<std::uint8_t>("R", ScalarType::UInt8, {0, 128, 255}, SampleEncoding::UNorm)};
  TransformOptions exact; exact.channels = {{"R", "R"}}; exact.output_scalar = ScalarType::UInt16;
  auto widened = transform(integers, exact);
  require(value(widened.image.channels[0], 1) == 32896 && widened.report.precision_changed == 0,
          "Lossless UNorm8 to UNorm16 widening failed");
  auto signed_c = channel<std::int16_t>("R", ScalarType::Int16, {-32768, -32767, 32767}, SampleEncoding::SNorm);
  require(interpreted_value(signed_c, 0) == -1 && interpreted_value(signed_c, 2) == 1, "SNorm limits wrong");
}
void colour_alpha() {
  auto source = rgba(); auto o = options(); o.meaning = ImageMeaning::Colour;
  o.output_scalar = ScalarType::Float32; o.quantization = Quantization::ClampRound;
  o.transfer = Transfer::LinearToSrgb; o.exposure = 1;
  auto out = transform(source, o);
  // Independent fixed reference values, not computed with production helpers.
  require(std::abs(value(out.image.channels[0], 0) - .5370987304831942) < 1e-6 &&
          std::abs(value(out.image.channels[1], 0) - .7353569830524495) < 1e-6 &&
          std::abs(value(out.image.channels[2], 0) - 1.824796295276116) < 1e-6 &&
          value(out.image.channels[3], 0) == .5, "HDR display transform or alpha changed");
  o.meaning = ImageMeaning::Data; rejects([&]{ (void)transform(source, o); });
  o.meaning = ImageMeaning::Colour; o.source_alpha = AlphaMode::Unknown;
  rejects([&]{ (void)transform(source, o); });
  o.source_alpha = AlphaMode::Straight; o.alpha_action = AlphaAction::Composite;
  o.channels.pop_back(); o.exposure = 0; o.background_linear = {.25, .25, .25};
  out = transform(source, o);
  require(std::abs(value(out.image.channels[0], 0) - .47021403501609) < 1e-6,
          "Composite must happen in linear space before OETF");
  require(std::abs(value(out.image.channels[0], 1) - .5370987304831942) < 1e-6,
          "Zero-alpha hidden RGB leaked into composite");
  o.transfer = Transfer::Preserve; o.source_alpha = AlphaMode::Premultiplied;
  out = transform(source, o);
  require(value(out.image.channels[0], 0) == .25, "Premultiplied input multiplied by alpha twice");
  o.output_scalar = ScalarType::Half; source.channels[2] = channel<float>("beauty.B", ScalarType::Float32, {1e30F, 1.F});
  rejects([&]{ (void)transform(source, o); });
  // A transfer on a non-RGB destination has no defined semantics: it must be
  // rejected, never silently skipped while the report claims it was applied.
  auto grey = rgba(); grey.channels = {channel<float>("beauty.Y", ScalarType::Float32, {.25F, .5F})};
  auto g = options(); g.meaning = ImageMeaning::Colour;
  g.channels = {{"beauty.Y", "Y"}};
  g.alpha_channel.clear(); g.source_alpha = AlphaMode::Absent;
  g.output_scalar = ScalarType::Float32; g.quantization = Quantization::ClampRound;
  g.transfer = Transfer::SrgbToLinear;
  rejects([&]{ (void)transform(grey, g); });
  g.transfer = Transfer::Preserve; g.exposure = 1;
  rejects([&]{ (void)transform(grey, g); });
  g.exposure = 0;
  const auto passthrough = transform(grey, g);
  require(value(passthrough.image.channels[0], 0) == .25 && !passthrough.report.colour_transformed,
          "No-transfer non-RGB passthrough must stay untouched and unreported");
}
void limits_errors() {
  auto source = rgba(); auto o = options(); Limits small; small.decoded_bytes = 8;
  rejects([&]{ (void)transform(source, o, small); }, ErrorCode::ResourceLimit);
  std::stop_source stop; stop.request_stop();
  rejects([&]{ (void)transform(source, o, {}, stop.get_token()); }, ErrorCode::Cancelled);
  o.channels[1].destination = "R"; rejects([&]{ (void)transform(source, o); });
  o = options(); source.channels[0].samples.bytes.pop_back(); rejects([&]{ (void)transform(source, o); });
  source = rgba(); source.channels[0].encoding = SampleEncoding::SNorm;
  rejects([&]{ (void)transform(source, o); });
}
}
int main() {
  try { preservation(); quantization(); colour_alpha(); limits_errors();
    std::cout << "PASS preservation, quantization, colour/alpha, limits/errors\n"; return 0;
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
