#include "formaview/image_requests.hpp"
#ifdef FORMAVIEW_MODEL_EXPORT
#include "formaview/model_export.hpp"
#endif
#include <QCryptographicHash>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <set>

namespace formaview::imaging {
namespace {
[[noreturn]] void invalid(const char *message) {
  throw skeletal::Failure(ErrorCode::InvalidArgument, message);
}
void keys(const QJsonObject &object,
          std::initializer_list<const char *> allowed) {
  for (auto it = object.begin(); it != object.end(); ++it) {
    bool found = false;
    for (auto key : allowed)
      if (it.key() == key) {
        found = true;
        break;
      }
    if (!found)
      invalid("Unknown request field");
  }
}
QString string(const QJsonObject &o, const char *key, QString fallback = {}) {
  if (!o.contains(key))
    return fallback;
  if (!o[key].isString())
    invalid("Expected string field");
  return o[key].toString();
}
bool boolean(const QJsonObject &o, const char *key, bool fallback = false) {
  if (!o.contains(key))
    return fallback;
  if (!o[key].isBool())
    invalid("Expected boolean field");
  return o[key].toBool();
}
double number(const QJsonObject &o, const char *key, double fallback) {
  if (!o.contains(key))
    return fallback;
  if (!o[key].isDouble() || !std::isfinite(o[key].toDouble()))
    invalid("Expected finite numeric field");
  return o[key].toDouble();
}
std::uint32_t integer(const QJsonObject &o, const char *key,
                      std::uint32_t fallback,
                      std::uint32_t maximum = UINT32_MAX) {
  const auto v = number(o, key, fallback);
  if (v < 0 || v > maximum || std::floor(v) != v)
    invalid("Expected bounded nonnegative integer");
  return static_cast<std::uint32_t>(v);
}
QJsonArray array(const QJsonObject &o, const char *key) {
  if (!o[key].isArray())
    invalid("Expected array field");
  return o[key].toArray();
}
template <class T>
T choice(const QJsonObject &o, const char *key, const char *fallback,
         std::initializer_list<std::pair<const char *, T>> choices) {
  const auto text = string(o, key, fallback);
  for (const auto &[name, value] : choices)
    if (text == name)
      return value;
  invalid("Unknown enum value");
}
std::filesystem::path path(const QString &value) {
  if (value.isEmpty())
    invalid("Empty path");
  return std::filesystem::path(value.toStdWString());
}
QString qpath(const std::filesystem::path &value) {
  return QString::fromStdWString(value.wstring());
}
QString scalar(ScalarType type) {
  switch (type) {
  case ScalarType::UInt8:
    return "uint8";
  case ScalarType::UInt16:
    return "uint16";
  case ScalarType::UInt32:
    return "uint32";
  case ScalarType::Int16:
    return "int16";
  case ScalarType::Int32:
    return "int32";
  case ScalarType::Half:
    return "half";
  case ScalarType::Float32:
    return "float32";
  case ScalarType::Float64:
    return "float64";
  }
  invalid("Unknown scalar");
}
QString alpha(AlphaMode value) {
  switch (value) {
  case AlphaMode::Unknown:
    return "unknown";
  case AlphaMode::Absent:
    return "absent";
  case AlphaMode::Straight:
    return "straight";
  case AlphaMode::Premultiplied:
    return "premultiplied";
  }
  invalid("Unknown alpha mode");
}
QJsonObject window(const ImageWindow &w) {
  return {{"x", static_cast<qint64>(w.x)},
          {"y", static_cast<qint64>(w.y)},
          {"width", static_cast<qint64>(w.width)},
          {"height", static_cast<qint64>(w.height)}};
}
QJsonObject snapshot(const InputSnapshot &s) {
  return {{"path", qpath(s.resolved_path)},
          {"size", static_cast<qint64>(s.size_bytes)},
          {"modified_ticks", QString::number(s.modified_ticks)},
          {"file_identity", QString::fromStdString(s.file_identity)},
          {"sha256", QString::fromStdString(s.content_sha256)}};
}
QString error_name(ErrorCode code) {
  switch (code) {
  case ErrorCode::Unsupported:
    return "unsupported";
  case ErrorCode::InvalidArgument:
    return "invalid_argument";
  case ErrorCode::QueueFull:
    return "queue_full";
  case ErrorCode::ResourceLimit:
    return "resource_limit";
  case ErrorCode::ShuttingDown:
    return "shutting_down";
  case ErrorCode::Cancelled:
    return "cancelled";
  case ErrorCode::StaleResult:
    return "stale_result";
  case ErrorCode::InputChanged:
    return "input_changed";
  case ErrorCode::OutOfScope:
    return "out_of_scope";
  case ErrorCode::Conflict:
    return "conflict";
  case ErrorCode::Io:
    return "io";
  case ErrorCode::Internal:
    return "internal";
  }
  return "internal";
}
QString digest(const QJsonObject &object) {
  return QCryptographicHash::hash(
             QJsonDocument(object).toJson(QJsonDocument::Compact),
             QCryptographicHash::Sha256)
      .toHex();
}
} // namespace
AccessScope parse_image_scope(const QJsonObject &o) {
  keys(o,
       {"input_roots", "output_roots", "allow_overwrite", "policy_revision"});
  AccessScope scope;
  for (const auto &[key, roots] :
       {std::pair{"input_roots", &scope.input_roots},
        std::pair{"output_roots", &scope.output_roots}}) {
    const auto list = array(o, key);
    if (list.isEmpty() || list.size() > 64)
      invalid("Scope requires one to 64 explicit roots");
    for (const auto &value : list) {
      if (!value.isString())
        invalid("Scope root must be a string");
      if (value.toString().trimmed().isEmpty())
        invalid(roots == &scope.input_roots ? "Input scope root must not be empty"
                                           : "Output scope root must not be empty");
      roots->push_back(path(value.toString()));
    }
  }
  scope.allow_overwrite = boolean(o, "allow_overwrite");
  scope.policy_revision = string(o, "policy_revision").toStdString();
  if (scope.policy_revision.empty())
    invalid("Scope policy revision required");
  return scope;
}
ImageConversionOptions parse_image_options(const QJsonObject &o) {
  keys(o, {"target",
           "subresource",
           "channels",
           "output_scalar",
           "alpha_action",
           "alpha_channel",
           "source_alpha",
           "meaning",
           "transfer",
           "exposure",
           "background_linear",
           "quantization",
           "non_finite",
           "window",
           "metadata",
           "allow_lossy",
           "quality",
           "png_compression",
           "webp_lossless",
           "exr_tiled",
           "tiff_compression",
           "dds_format"});
  ImageConversionOptions result;
  result.target = choice<ImageTarget>(o, "target", "png",
                                      {{"png", ImageTarget::Png},
                                       {"jpeg", ImageTarget::Jpeg},
                                       {"bmp", ImageTarget::Bmp},
                                       {"tga", ImageTarget::Tga},
                                       {"tiff", ImageTarget::Tiff},
                                       {"webp", ImageTarget::Webp},
                                       {"dds", ImageTarget::Dds},
                                       {"hdr", ImageTarget::Hdr},
                                       {"exr", ImageTarget::Exr}});
  result.subresource = integer(o, "subresource", 0);
  auto &t = result.transform;
  const auto channels = array(o, "channels");
  if (channels.isEmpty() || channels.size() > 256)
    invalid("Explicit channel mapping required");
  for (const auto &item : channels) {
    if (!item.isObject())
      invalid("Channel mapping must be an object");
    const auto m = item.toObject();
    keys(m, {"source", "destination"});
    const auto a = string(m, "source"), b = string(m, "destination");
    if (a.isEmpty() || b.isEmpty() || a.size() > 255 || b.size() > 255)
      invalid("Invalid channel mapping name");
    t.channels.push_back({a.toStdString(), b.toStdString()});
  }
  if (o.contains("output_scalar"))
    t.output_scalar = choice<ScalarType>(o, "output_scalar", "float32",
                                         {{"uint8", ScalarType::UInt8},
                                          {"uint16", ScalarType::UInt16},
                                          {"half", ScalarType::Half},
                                          {"float32", ScalarType::Float32}});
  t.alpha_action = choice<AlphaAction>(o, "alpha_action", "preserve",
                                       {{"preserve", AlphaAction::Preserve},
                                        {"drop", AlphaAction::Drop},
                                        {"composite", AlphaAction::Composite}});
  t.source_alpha =
      choice<AlphaMode>(o, "source_alpha", "unknown",
                        {{"unknown", AlphaMode::Unknown},
                         {"absent", AlphaMode::Absent},
                         {"straight", AlphaMode::Straight},
                         {"premultiplied", AlphaMode::Premultiplied}});
  t.alpha_channel = string(o, "alpha_channel").toStdString();
  t.meaning = choice<ImageMeaning>(
      o, "meaning", "data",
      {{"data", ImageMeaning::Data}, {"colour", ImageMeaning::Colour}});
  t.transfer = choice<Transfer>(o, "transfer", "preserve",
                                {{"preserve", Transfer::Preserve},
                                 {"linear_to_srgb", Transfer::LinearToSrgb},
                                 {"srgb_to_linear", Transfer::SrgbToLinear}});
  t.exposure = number(o, "exposure", 0);
  if (o.contains("background_linear")) {
    const auto b = array(o, "background_linear");
    if (b.size() != 3)
      invalid("Linear background needs three values");
    for (int i = 0; i < 3; ++i) {
      if (!b[i].isDouble() || !std::isfinite(b[i].toDouble()))
        invalid("Invalid linear background");
      t.background_linear[static_cast<std::size_t>(i)] = b[i].toDouble();
    }
  }
  t.quantization =
      choice<Quantization>(o, "quantization", "reject_loss",
                           {{"reject_loss", Quantization::RejectLoss},
                            {"clamp_round", Quantization::ClampRound}});
  t.non_finite = choice<NonFinite>(o, "non_finite", "preserve",
                                   {{"preserve", NonFinite::Preserve},
                                    {"reject", NonFinite::Reject},
                                    {"replace_zero", NonFinite::ReplaceZero}});
  result.window = choice<WindowPolicy>(
      o, "window", "preserve",
      {{"preserve", WindowPolicy::Preserve},
       {"selected_data", WindowPolicy::SelectedDataWindow}});
  result.metadata = choice<MetadataPolicy>(
      o, "metadata", "preserve_supported",
      {{"preserve_supported", MetadataPolicy::PreserveSupported},
       {"drop", MetadataPolicy::Drop}});
  result.allow_lossy = boolean(o, "allow_lossy");
  result.quality = static_cast<int>(integer(o, "quality", 90, 100));
  result.png_compression =
      static_cast<int>(integer(o, "png_compression", 0, 9));
  result.webp_lossless = boolean(o, "webp_lossless", true);
  result.exr_tiled = boolean(o, "exr_tiled");
  result.dds_format = integer(o, "dds_format", 0);
  result.tiff_compression =
      choice<TiffCompression>(o, "tiff_compression", "deflate",
                              {{"none", TiffCompression::None},
                               {"lzw", TiffCompression::Lzw},
                               {"deflate", TiffCompression::Deflate},
                               {"packbits", TiffCompression::PackBits}});
  return result;
}
QJsonObject image_error_json(const skeletal::Failure &f) {
  QJsonObject details;
  for (const auto &[k, v] : f.error.details)
    details[QString::fromStdString(k)] = QString::fromStdString(v);
  return {{"version", 1},
          {"error", QJsonObject{{"code", error_name(f.error.code)},
                                {"message", QString::fromUtf8(f.what())},
                                {"details", details},
                                {"retryable", f.error.retryable}}}};
}
std::vector<std::wstring> image_input_extensions() {
  const auto readers = QImageReader::supportedImageFormats();
  std::vector<std::wstring> result{L".tga", L".tif", L".tiff", L".webp", L".dds", L".hdr", L".rgbe", L".exr"};
  for (const auto &[codec, extensions] : {
      std::pair{"png", std::vector<std::wstring>{L".png"}},
      std::pair{"jpeg", std::vector<std::wstring>{L".jpg", L".jpeg", L".jpe", L".jfif"}},
      std::pair{"bmp", std::vector<std::wstring>{L".bmp", L".dib"}},
      std::pair{"gif", std::vector<std::wstring>{L".gif"}},
      std::pair{"ico", std::vector<std::wstring>{L".ico"}}}) {
    if (readers.contains(codec) && (QByteArray(codec) != "ico" ||
        (readers.contains("png") && readers.contains("bmp"))))
      result.insert(result.end(), extensions.begin(), extensions.end());
  }
  std::sort(result.begin(), result.end());
  return result;
}
QJsonObject image_capabilities() {
  const auto readers = QImageReader::supportedImageFormats();
  const auto writers = QImageWriter::supportedImageFormats();
  QJsonArray reads, writes;
  for (const auto *format : {"png", "jpeg", "bmp", "gif", "ico", "tga", "tiff",
                             "webp", "dds", "hdr", "exr"}) {
    const QByteArray name(format);
    const bool qt = name == "png" || name == "jpeg" || name == "bmp" ||
                    name == "gif" || name == "ico";
    if ((qt && !readers.contains(name)) ||
        (name == "ico" &&
         (!readers.contains("png") || !readers.contains("bmp"))))
      continue;
    reads.append(QString::fromLatin1(format));
  }
  for (const auto target :
       {ImageTarget::Png, ImageTarget::Jpeg, ImageTarget::Bmp, ImageTarget::Tga,
        ImageTarget::Tiff, ImageTarget::Webp, ImageTarget::Dds,
        ImageTarget::Hdr, ImageTarget::Exr}) {
    const auto format = QString::fromLatin1(image_target_name(target).data());
    if ((target == ImageTarget::Png || target == ImageTarget::Jpeg ||
         target == ImageTarget::Bmp) &&
        !writers.contains(format.toLatin1()))
      continue;
    writes.append(format);
  }
  QJsonArray extensions;
  for (const auto &name : image_input_extensions()) extensions.append(QString::fromStdWString(name));
  return {
      {"api_version", 1},
      {"read_formats", reads},
      {"read_extensions", extensions},
      {"write_targets", writes},
#ifdef FORMAVIEW_MODEL_EXPORT
      {"model_export", model_export::capabilities()},
#endif
      {"capabilities", QJsonArray{QJsonObject{{"id", "image.inspect"},
                                              {"input_formats", reads},
                                              {"operation", "inspect"}},
                                  QJsonObject{{"id", "image.convert"},
                                              {"input_formats", reads},
                                              {"output_formats", writes},
                                              {"approval_required", true}}}},
      {"limitations",
       QJsonArray{
           "Per-file decoding and conversion compatibility require preflight",
           "GIF: first static frame only; ICO: explicitly selected entry",
           "BMP includes validated bare DIB variants",
           "EXR: no deep or subsampled channels; tiled level zero only",
           "One selected subresource per output; no generated mip chains, "
           "arrays or cubes",
           "Viewport state is excluded; losses require explicit "
           "acknowledgement",
           "Input and output access require an explicit validated scope"}}};
}
QJsonObject execute_image_request(
    const QJsonObject &request, const AccessScope &scope, Limits limits,
    std::stop_token stop, std::span<const InputSnapshot> protected_inputs,
    const std::function<void(const QJsonObject &)> &commit_observer,
    std::span<const std::filesystem::path> reserved_outputs,
    const std::filesystem::path &approved_output) {
  keys(request, {"version", "operation", "input", "output", "options",
                 "conflict", "approval_digest", "acknowledge_losses",
                 "subresource", "offset", "limit"});
  if (integer(request, "version", 0) != 1)
    invalid("Request version must be 1");
  const auto operation = string(request, "operation");
  if (operation == "capabilities") {
    keys(request, {"version", "operation"});
    return image_capabilities();
  }
  if (operation != "inspect" && operation != "preflight" &&
      operation != "convert")
    invalid("Unsupported image operation");
#ifdef FORMAVIEW_MODEL_EXPORT
  if (model_export::handles(request)) {
    skeletal::Limits model_limits;
    model_limits.input_bytes = limits.input_bytes;
    model_limits.decoded_bytes = limits.decoded_bytes;
    return model_export::execute(request, scope, model_limits, stop, protected_inputs,
                                 commit_observer, reserved_outputs, approved_output);
  }
#endif
  auto lease = InputLease::open(scope, path(string(request, "input")),
                                limits.input_bytes, stop);
  const auto &input = lease.snapshot();
  auto document = ImageDocument::open(input.resolved_path, limits, stop);
  const auto &description = document.description();
  if (description.source.identity.sha256 != input.content_sha256)
    throw skeletal::Failure(ErrorCode::InputChanged,
                            "Decoded source differs from held snapshot");
  if (operation == "inspect") {
    const auto selected = integer(request, "subresource", 0);
    // Validate the requested page before decoding pixels for it.
    const auto offset = integer(request, "offset", 0),
               limit = integer(request, "limit", 100, 100);
    if (!limit || offset > description.subresources.size())
      invalid("Invalid subresource page");
    auto image = document.decode(selected, stop);
    const auto semantics = document.semantics(selected);
    QJsonArray channels;
    for (const auto &c : image.channels)
      channels.append(QJsonObject{
          {"name", QString::fromStdString(c.name)},
          {"scalar", scalar(c.samples.scalar)},
          {"encoding", c.encoding == SampleEncoding::UNorm   ? "unorm"
                       : c.encoding == SampleEncoding::SNorm ? "snorm"
                                                             : "numeric"}});
    QJsonArray resources;
    const auto end =
        std::min<std::size_t>(description.subresources.size(),
                              static_cast<std::size_t>(offset) + limit);
    for (std::size_t i = offset; i < end; ++i) {
      const auto &s = description.subresources[i];
      resources.append(
          QJsonObject{{"index", static_cast<qint64>(i)},
                      {"part", static_cast<qint64>(s.part)},
                      {"part_name", QString::fromStdString(s.part_name)},
                      {"mip", static_cast<qint64>(s.mip)},
                      {"array_layer", static_cast<qint64>(s.array_layer)},
                      {"cube_face", static_cast<qint64>(s.cube_face)},
                      {"depth_slice", static_cast<qint64>(s.depth_slice)},
                      {"data_window", window(s.data_window)},
                      {"display_window", window(s.display_window)}});
    }
    QJsonObject props;
    for (const auto &[key, value] : description.source.properties)
      if (!key.ends_with("icc_base64") && !key.ends_with("icc_hex"))
        props[QString::fromStdString(key)] = QString::fromStdString(value);
    return {
        {"version", 1},
        {"input", snapshot(input)},
        {"adapter",
         QString::fromStdString(description.source.identity.adapter)},
        {"adapter_version",
         QString::fromStdString(description.source.identity.adapter_version)},
        {"alpha", alpha(semantics.alpha)},
        {"container_alpha", alpha(description.alpha)},
        {"colour_space",
         semantics.colour_space
             ? QJsonValue(QString::fromStdString(*semantics.colour_space))
             : QJsonValue(QJsonValue::Null)},
        {"icc_profile_sha256",
         QString::fromStdString(semantics.icc_profile_sha256)},
        {"selected_channels", channels},
        {"metadata", props},
        {"subresources", resources},
        {"total", static_cast<qint64>(description.subresources.size())},
        {"next_offset", end < description.subresources.size()
                            ? QJsonValue(static_cast<qint64>(end))
                            : QJsonValue(QJsonValue::Null)}};
  }
  if (!request["options"].isObject())
    invalid("Explicit conversion options required");
  const auto options = parse_image_options(request["options"].toObject());
  const auto conflict =
      choice<ConflictPolicy>(request, "conflict", "skip",
                             {{"skip", ConflictPolicy::Skip},
                              {"rename", ConflictPolicy::Rename},
                              {"overwrite", ConflictPolicy::Overwrite}});
  const auto output =
      approved_output.empty()
          ? allocate_output(scope, path(string(request, "output")), input,
                            conflict, reserved_outputs, stop)
          : inspect_output(scope, approved_output, input, conflict);
  if (conflict == ConflictPolicy::Rename && output.exists)
    throw skeletal::Failure(
        ErrorCode::Conflict,
        "Approved output name is occupied; run a new preflight");
  require_output_not_input(output, protected_inputs);
  auto product = prepare_image_conversion(document, options, limits, stop);
  QJsonArray losses;
  for (const auto &l : product.losses)
    losses.append(QJsonObject{{"code", QString::fromStdString(l.code)},
                              {"detail", QString::fromStdString(l.detail)}});
  auto canonical = request;
  canonical.remove("operation");
  canonical.remove("approval_digest");
  canonical.remove("acknowledge_losses");
  QJsonArray in_roots, out_roots;
  for (const auto &p : scope.input_roots)
    in_roots.append(qpath(p));
  for (const auto &p : scope.output_roots)
    out_roots.append(qpath(p));
  const QJsonObject policy{
      {"revision", QString::fromStdString(scope.policy_revision)},
      {"inputs", in_roots},
      {"outputs", out_roots},
      {"overwrite", scope.allow_overwrite}};
  const QJsonObject approval{
      {"request", canonical},     {"resolved_output", qpath(output.path)},
      {"input", snapshot(input)}, {"losses", losses},
      {"scope", policy},          {"implementation", "M05-image-request-v2"}};
  const auto approval_digest = digest(approval);
  QJsonObject result{
      {"version", 1},
      {"input", snapshot(input)},
      {"output", qpath(output.path)},
      {"output_exists", output.exists},
      {"target",
       QString::fromLatin1(
           image_target_name(options.target).data(),
           static_cast<qsizetype>(image_target_name(options.target).size()))},
      {"encoded_bytes", static_cast<qint64>(product.encoded.size())},
      {"losses", losses},
      {"approval_digest", approval_digest}};
  if (operation == "preflight")
    return result;
  if (string(request, "approval_digest") != approval_digest)
    throw skeletal::Failure(
        ErrorCode::InputChanged,
        "Preflight approval does not match current input, parameters or scope");
  if (!losses.isEmpty() && !boolean(request, "acknowledge_losses"))
    invalid("Preflight losses require explicit acknowledgement");
  const auto output_sha256 =
      QString::fromStdString(skeletal::sha256(product.encoded, stop));
  OutputTransaction transaction(scope, output.path, input, conflict,
                                protected_inputs);
  const auto observe = [&](const OutputIntent &intent) {
    if (!commit_observer)
      return;
    commit_observer(QJsonObject{
        {"temporary", qpath(intent.temporary)},
        {"destination", qpath(intent.destination)},
        {"file_identity", QString::fromStdString(intent.file_identity)},
        {"output_sha256", output_sha256},
        {"result", result}});
  };
  observe(transaction.temporary());
  transaction.write(product.encoded, stop);
  const auto committed = transaction.commit(stop, observe);
  result["status"] = committed.skipped ? "skipped" : "succeeded";
  result["committed_output"] = qpath(committed.path);
  if (!committed.skipped)
    result["output_sha256"] = output_sha256;
  return result;
}
} // namespace formaview::imaging
