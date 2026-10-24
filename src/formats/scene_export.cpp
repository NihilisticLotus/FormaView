#include "scene_sdk.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUrl>
#include <algorithm>
#include <climits>
#include <cstring>
#include <set>

namespace formaview::scene_export {
namespace sk = skeletal;
[[noreturn]] void invalid(const char *message) {
  throw sk::Failure(ErrorCode::InvalidArgument, message);
}
namespace {
struct Destroy {
  template <class T> void operator()(T *p) const {
    if (p)
      p->Destroy();
  }
};
QString key(const char *name) {
  return QString::fromUtf8(name).replace('\\', '/');
}
class Stream final : public FbxStream {
public:
  QByteArray data;
  Stream(QByteArray source, std::uint64_t limit, std::stop_token stop,
         bool writable)
      : data(std::move(source)),
        limit_(std::min<std::uint64_t>(limit, INT_MAX)), stop_(stop),
        writable_(writable) {}
  EState GetState() override { return open_ ? eOpen : eClosed; }
  bool Open(void *) override {
    position_ = 0;
    open_ = true;
    return true;
  }
  bool Close() override {
    open_ = false;
    return true;
  }
  bool Flush() override { return !error_; }
  int Write(const void *p, int size) override {
    if (!writable_ || size < 0 || stop_.stop_requested() || error_ ||
        static_cast<std::uint64_t>(size) > limit_ - position_) {
      error_ = 1;
      return 0;
    }
    try {
      const auto end = position_ + size;
      if (end > data.size())
        data.resize(end);
      if (size)
        std::memcpy(data.data() + position_, p, static_cast<std::size_t>(size));
      position_ = end;
      return size;
    } catch (...) {
      error_ = 1;
      return 0;
    }
  }
  int Read(void *p, int size) const override {
    if (size < 0 || stop_.stop_requested() || error_ ||
        position_ > data.size()) {
      error_ = 1;
      return 0;
    }
    const auto count = std::min<qint64>(size, data.size() - position_);
    if (count)
      std::memcpy(p, data.constData() + position_,
                  static_cast<std::size_t>(count));
    position_ += count;
    return static_cast<int>(count);
  }
  int GetReaderID() const override { return -1; }
  int GetWriterID() const override { return -1; }
  void Seek(const FbxInt64 &offset, const FbxFile::ESeekPos &origin) override {
    const qint64 base = origin == FbxFile::eBegin ? 0
                        : origin == FbxFile::eEnd ? data.size()
                                                  : position_;
    if (offset < -base || offset > static_cast<qint64>(limit_) - base) {
      error_ = 1;
      return;
    }
    position_ = base + offset;
  }
  long GetPosition() const override { return static_cast<long>(position_); }
  void SetPosition(long p) override {
    if (p < 0 || static_cast<std::uint64_t>(p) > limit_)
      error_ = 1;
    else
      position_ = p;
  }
  int GetError() const override { return error_; }
  void ClearError() override {}

private:
  std::uint64_t limit_;
  std::stop_token stop_;
  bool writable_, open_{};
  mutable qint64 position_{};
  mutable int error_{};
};
bool progress(void *data, float, const char *) {
  return !static_cast<Scene *>(data)->stop.stop_requested();
}
FbxCallback::State read_media(void *user, FbxClassId, const char *name,
                              const void *data, size_t size) {
  auto &s = *static_cast<Scene *>(user);
  try {
    s.reserve(size);
    if (size > INT_MAX)
      invalid("Embedded media exceeds byte limit");
    s.media.insert(key(name), QByteArray(static_cast<const char *>(data),
                                         static_cast<qsizetype>(size)));
    return FbxCallback::eHandled;
  } catch (const std::exception &e) {
    s.callback_error = QString::fromUtf8(e.what());
    return FbxCallback::eFailure;
  }
}
FbxCallback::State write_media(void *user, FbxClassId, const char *name,
                               const void **data, size_t *size) {
  auto &s = *static_cast<Scene *>(user);
  try {
    const auto it = s.media.constFind(key(name));
    // Always handle the callback; never let SDK open an unchecked path.
    *data = it == s.media.cend() ? nullptr : it->constData();
    *size = it == s.media.cend() ? 0 : static_cast<size_t>(it->size());
    return s.stop.stop_requested() ? FbxCallback::eFailure
                                   : FbxCallback::eHandled;
  } catch (...) {
    *data = nullptr;
    *size = 0;
    return FbxCallback::eFailure;
  }
}
QString id(const Scene &s, int i) {
  return s.metadata["sha256"].toString() + "/animation/" + QString::number(i);
}
QJsonObject snapshot(const InputSnapshot &s) {
  return {{"path", QString::fromStdWString(s.resolved_path.wstring())},
          {"sha256", QString::fromStdString(s.content_sha256)},
          {"file_identity", QString::fromStdString(s.file_identity)},
          {"size", static_cast<qint64>(s.size_bytes)},
          {"modified_ticks", QString::number(s.modified_ticks)}};
}
} // namespace
Scene::Scene(const std::filesystem::path &p, Loader reader, sk::Limits budget,
             std::stop_token token)
    : path(p), load(std::move(reader)), limits(budget), stop(token),
      manager(FbxManager::Create()) {
  if (!manager)
    throw sk::Failure(ErrorCode::ResourceLimit, "Cannot allocate FBX manager");
  auto *io = FbxIOSettings::Create(manager.get(), IOSROOT);
  manager->SetIOSettings(io);
  io->SetBoolProp(IMP_FBX_EXTRACT_EMBEDDED_DATA, true);
  io->SetBoolProp(EXP_FBX_EMBEDDED, true);
  scene = FbxScene::Create(manager.get(), "FormaView scene export");
}
void Scene::check() const {
  if (stop.stop_requested())
    throw sk::Failure(ErrorCode::Cancelled, "Scene export cancelled");
}
void Scene::reserve(std::uint64_t bytes) {
  check();
  if (bytes > limits.decoded_bytes - allocated)
    throw sk::Failure(ErrorCode::ResourceLimit,
                      "Scene decoded budget exceeded");
  allocated += bytes;
}
void Scene::loss(const QString &code, const QString &detail) {
  const QJsonObject value{{"code", code}, {"detail", detail}};
  if (!losses.contains(value))
    losses.append(value);
}
std::filesystem::path Scene::relative(const QString &uri) const {
  const auto decoded = QUrl::fromPercentEncoding(uri.toUtf8());
  if (decoded.isEmpty() || decoded.contains(QChar(0)) ||
      decoded.contains(':') || decoded.startsWith('/') ||
      decoded.startsWith('\\'))
    invalid("Resource URI must be a relative local path");
  return (path.parent_path() / std::filesystem::path(decoded.toStdWString()))
      .lexically_normal();
}
QString Scene::image(const QString &uri, const std::optional<QByteArray> &content) {
  const auto source = QString(uri).replace('\\', '/');
  if (!content && media_sources.contains(source))
    return media_sources[source];
  auto bytes = content ? *content : load(relative(source));
  if (bytes.isEmpty())
    invalid("Empty texture payload");
  // Resource names are not identities: an embedded image may have the same
  // name as a different external image. Give every payload its own FBX name.
  const auto digest = QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
  const auto suffix = QFileInfo(source).suffix();
  const auto name = "formaview-media-" + digest + (suffix.isEmpty() ? "" : "." + suffix);
  if (!media.contains(name)) {
    reserve(static_cast<std::uint64_t>(bytes.size()));
    media.insert(name, std::move(bytes));
  }
  if (!content)
    media_sources.insert(source, name);
  return name;
}
FbxFileTexture *Scene::texture(const QString &name, const QString &uv) {
  auto *t = FbxFileTexture::Create(scene, name.toUtf8().constData());
  t->SetFileName(name.toUtf8().constData());
  t->SetRelativeFileName(name.toUtf8().constData());
  t->SetTextureUse(FbxTexture::eStandard);
  t->SetMappingType(FbxTexture::eUV);
  t->UVSet.Set(FbxString(uv.toUtf8().constData()));
  return t;
}
void Scene::preload_media() {
  for (int i = 0; i < scene->GetSrcObjectCount<FbxFileTexture>(); ++i) {
    check();
    auto *t = scene->GetSrcObject<FbxFileTexture>(i);
    const auto name = key(t->GetFileName());
    if (media.contains(name))
      continue;
    const auto relative_name = key(t->GetRelativeFileName());
    if (media.contains(relative_name)) {
      media.insert(name, media[relative_name]);
      continue;
    }
    try {
      const auto p = std::filesystem::path(name.toStdWString());
      auto data = load(p.is_absolute() ? p : path.parent_path() / p);
      reserve(static_cast<std::uint64_t>(data.size()));
      media.insert(name, std::move(data));
    } catch (const sk::Failure &e) {
      if (e.error.code == ErrorCode::Cancelled ||
          e.error.code == ErrorCode::ResourceLimit)
        throw;
      loss("external_texture_unavailable",
           name + ": " + QString::fromUtf8(e.what()));
    }
  }
}
FbxAxisSystem axis_system(const QString &name) {
  using A = FbxAxisSystem;
  if (name == "maya" || name == "houdini")
    return A::MayaYUp;
  if (name == "3ds-max" || name == "blender")
    return A::Max;
  if (name == "unreal")
    return A(A::eZAxis, static_cast<A::EFrontVector>(-A::eParityEven),
             A::eLeftHanded);
  if (name == "cinema4d" || name == "unity")
    return A(A::eYAxis, static_cast<A::EFrontVector>(-A::eParityOdd),
             A::eLeftHanded);
  invalid("Unknown output axis preset");
}
FbxAMatrix matrix(const formaview::Matrix4 &value) {
  FbxAMatrix result;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      result[r][c] = value[static_cast<size_t>(c * 4 + r)];
  return result;
}
void string_property(FbxObject *object, const char *name, const QString &text) {
  auto property = FbxProperty::Create(object, FbxStringDT, name);
  property.ModifyFlag(FbxPropertyFlags::eUserDefined, true);
  property.Set(FbxString(text.toUtf8().constData()));
}
struct Document::Impl {
  Scene source;
  explicit Impl(const std::filesystem::path &p, Loader l, sk::Limits b,
                std::stop_token t)
      : source(p, std::move(l), b, t) {}
};
Document::Document(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
Document::Document(Document &&) noexcept = default;
Document::~Document() = default;
bool handles(const QString &s) {
#ifdef FORMAVIEW_GLTF_EXPORT
  if (s == "gltf" || s == "glb" || s == "vrm")
    return true;
#endif
  return s == "fbx" || s == "obj" || s == "pmx";
}
Document Document::read(const std::filesystem::path &path,
                        const QByteArray &bytes, Loader loader,
                        sk::Limits limits, std::stop_token stop) {
  auto impl = std::make_unique<Impl>(path, std::move(loader), limits, stop);
  auto &s = impl->source;
  const auto ext =
      QFileInfo(QString::fromStdWString(path.wstring())).suffix().toLower();
  s.metadata = {
      {"format", ext},
      {"sha256", QString::fromLatin1(
                     QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
                         .toHex())}};
  if (ext == "obj")
    read_obj(s, bytes);
  else if (ext == "pmx")
    read_pmx(s, bytes);
#ifdef FORMAVIEW_GLTF_EXPORT
  else if (ext == "gltf" || ext == "glb" || ext == "vrm")
    read_gltf(s, bytes);
#endif
  else if (ext == "fbx") {
    Stream stream(bytes, limits.input_bytes, stop, false);
    const std::unique_ptr<FbxImporter, Destroy> importer(
        FbxImporter::Create(s.manager.get(), "source"));
    const auto format =
        s.manager->GetIOPluginRegistry()->FindReaderIDByExtension("fbx");
    if (!importer->Initialize(&stream, nullptr, format,
                              s.manager->GetIOSettings()))
      invalid(importer->GetStatus().GetErrorString());
    auto *callback =
        FbxEmbeddedFileCallback::Create(s.manager.get(), "source media");
    callback->RegisterReadFunction(read_media, &s);
    importer->SetEmbeddedFileReadCallback(callback);
    importer->SetProgressCallback(progress, &s);
    if (!importer->Import(s.scene)) {
      s.check();
      invalid(importer->GetStatus().GetErrorString());
    }
    if (!s.callback_error.isEmpty())
      invalid(s.callback_error.toUtf8().constData());
    s.preload_media();
  } else
    invalid("Unsupported scene input");
  s.check();
  if (s.scene->GetNodeCount() > static_cast<int>(limits.bones) ||
      s.scene->GetSrcObjectCount<FbxAnimStack>() >
          static_cast<int>(limits.clips))
    throw sk::Failure(ErrorCode::ResourceLimit,
                      "Scene node/clip budget exceeded");
  std::uint64_t elements{};
  for (int i = 0; i < s.scene->GetSrcObjectCount<FbxMesh>(); ++i) {
    const auto *mesh = s.scene->GetSrcObject<FbxMesh>(i);
    elements += static_cast<std::uint64_t>(mesh->GetControlPointsCount()) +
                mesh->GetPolygonVertexCount();
  }
  for (int i = 0; i < s.scene->GetSrcObjectCount<FbxAnimCurve>(); ++i)
    elements += s.scene->GetSrcObject<FbxAnimCurve>(i)->KeyGetCount();
  if (elements > limits.elements)
    throw sk::Failure(ErrorCode::ResourceLimit,
                      "Scene geometry/animation element budget exceeded");
  return Document(std::move(impl));
}
QJsonObject Document::inspect() const {
  auto &s = impl_->source;
  QJsonArray clips;
  for (int i = 0; i < s.scene->GetSrcObjectCount<FbxAnimStack>(); ++i) {
    auto *stack = s.scene->GetSrcObject<FbxAnimStack>(i);
    const auto span = stack->GetLocalTimeSpan();
    clips.append(QJsonObject{
        {"id", id(s, i)},
        {"name", QString::fromUtf8(stack->GetName())},
        {"start", span.GetStart().GetSecondDouble()},
        {"end", span.GetStop().GetSecondDouble()},
        {"fps",
         FbxTime::GetFrameRate(s.scene->GetGlobalSettings().GetTimeMode())}});
  }
  return {{"clips", clips},
          {"has_mesh", s.scene->GetSrcObjectCount<FbxMesh>() > 0},
          {"nodes", s.scene->GetNodeCount()},
          {"meshes", s.scene->GetSrcObjectCount<FbxMesh>()},
          {"losses", s.losses},
          {"source_metadata", s.metadata},
          {"root_motion", "source"}};
}
QByteArray Document::encode(const QJsonObject &options, QJsonObject &result) {
  auto &s = impl_->source;
  const auto source = inspect();
  QJsonArray selected, exported;
  if (options.contains("clip_ids"))
    selected = options["clip_ids"].toArray();
  else if (options.contains("clip_id"))
    selected.append(options["clip_id"]);
  std::vector<FbxAnimStack *> remove;
  int index = 0;
  for (const auto &v : source["clips"].toArray()) {
    if (selected.contains(v.toObject()["id"]))
      exported.append(v);
    else
      remove.push_back(s.scene->GetSrcObject<FbxAnimStack>(index));
    ++index;
  }
  if (selected.size() != exported.size())
    invalid("Select existing source clip IDs");
  for (auto *stack : remove)
    stack->Destroy();
  if (s.scene->GetSrcObjectCount<FbxAnimStack>()) {
    auto *first = s.scene->GetSrcObject<FbxAnimStack>(0);
    s.scene->SetCurrentAnimationStack(first);
    s.scene->GetGlobalSettings().SetTimelineDefaultTimeSpan(
        first->GetLocalTimeSpan());
  }
  if (!options["include_mesh"].toBool(true)) {
    for (int i = 0; i < s.scene->GetNodeCount(); ++i)
      if (auto *node = s.scene->GetNode(i); node->GetMesh())
        node->SetNodeAttribute(nullptr);
    while (s.scene->GetSrcObjectCount<FbxMesh>()) {
      auto *mesh = s.scene->GetSrcObject<FbxMesh>(0);
      if (mesh->GetDeformerCount(FbxDeformer::eBlendShape))
        s.loss("morph_mesh_omitted",
               "Animation-only output omits mesh-based blend shapes.");
      mesh->Destroy();
    }
  }
  const auto axes = options["output_axes"].toString("source"),
             unit = options["output_unit"].toString("source");
  if (axes != "source")
    axis_system(axes).DeepConvertScene(s.scene);
  const QMap<QString, double> units{
      {"m", 1}, {"cm", .01}, {"mm", .001}, {"in", .0254}};
  if (unit != "source") {
    if (!units.contains(unit))
      invalid("Unknown output unit");
    FbxSystemUnit(units[unit] * 100).ConvertScene(s.scene);
  }
  s.check();
  Stream stream({}, s.limits.decoded_bytes, s.stop, true);
  const std::unique_ptr<FbxExporter, Destroy> exporter(
      FbxExporter::Create(s.manager.get(), "output"));
  const auto format =
      s.manager->GetIOPluginRegistry()->FindWriterIDByDescription(
          "FBX binary (*.fbx)");
  if (!exporter->Initialize(&stream, nullptr, format,
                            s.manager->GetIOSettings()))
    invalid(exporter->GetStatus().GetErrorString());
  auto *callback =
      FbxEmbeddedFileCallback::Create(s.manager.get(), "output media");
  callback->RegisterWriteFunction(write_media, &s);
  exporter->SetEmbeddedFileWriteCallback(callback);
  exporter->SetProgressCallback(progress, &s);
  if (!exporter->Export(s.scene)) {
    s.check();
    invalid(exporter->GetStatus().GetErrorString());
  }
  s.check();
  if (stream.GetError())
    throw sk::Failure(ErrorCode::ResourceLimit,
                      "FBX output byte budget exceeded");
  result = source;
  result["clips"] = exported;
  result["losses"] = s.losses;
  result["output_coordinates"] = QJsonObject{
      {"preset", axes},
      {"unit", unit},
      {"metres_per_unit",
       s.scene->GetGlobalSettings().GetSystemUnit().GetScaleFactor() / 100.0}};
  result["embedded_media"] = s.media.size();
  return stream.data;
}
QJsonObject execute(const QJsonObject &request, const AccessScope &scope,
                    sk::Limits limits, std::stop_token stop,
                    std::span<const InputSnapshot> protected_inputs,
                    const std::function<void(const QJsonObject &)> &observer,
                    std::span<const std::filesystem::path> reserved,
                    const std::filesystem::path &approved) {
  std::vector<InputLease> leases;
  std::vector<InputSnapshot> inputs(protected_inputs.begin(),
                                    protected_inputs.end());
  QJsonArray sources, references;
  std::uint64_t total{};
  const auto load = [&](const std::filesystem::path &path) {
    // Even missing or out-of-scope resources remain source paths. A tolerated
    // texture-read loss must never make that path eligible for publication.
    InputSnapshot referenced;
    referenced.resolved_path = path.lexically_normal();
    inputs.push_back(referenced);
    const auto reference = QString::fromStdWString(referenced.resolved_path.wstring());
    if (!references.contains(reference))
      references.append(reference);
    auto lease = InputLease::open(scope, path, limits.input_bytes, stop);
    const auto snapshot_value = lease.snapshot();
    if (snapshot_value.size_bytes > limits.input_bytes - total)
      throw sk::Failure(ErrorCode::ResourceLimit,
                        "Scene input byte budget exceeded");
    total += snapshot_value.size_bytes;
    QFile file(QString::fromStdWString(snapshot_value.resolved_path.wstring()));
    if (!file.open(QIODevice::ReadOnly))
      throw sk::Failure(ErrorCode::Io, "Cannot read scene input");
    const auto data = file.readAll();
    if (static_cast<std::uint64_t>(data.size()) != snapshot_value.size_bytes ||
        QCryptographicHash::hash(data, QCryptographicHash::Sha256)
                .toHex()
                .toStdString() != snapshot_value.content_sha256)
      throw sk::Failure(ErrorCode::InputChanged,
                        "Scene input changed while reading");
    inputs.push_back(snapshot_value);
    sources.append(snapshot(snapshot_value));
    leases.push_back(std::move(lease));
    return data;
  };
  const auto path =
      std::filesystem::path(request["input"].toString().toStdWString());
  const auto bytes = load(path);
  const auto input = leases.front().snapshot();
  auto document =
      Document::read(input.resolved_path, bytes, load, limits, stop);
  const auto operation = request["operation"].toString();
  if (operation == "inspect") {
    auto result = document.inspect();
    result["version"] = 1;
    result["input"] = snapshot(input);
    result["sources"] = sources;
    result["referenced_sources"] = references;
    return result;
  }
  const auto options = request["options"].toObject();
  model_export::validate_options(options);
  for (const auto key_name :
       {"reference", "animation", "companion", "pairing_policy"})
    if (options.contains(key_name))
      invalid("Pairing files are only supported for ActorX inputs");
  if (options.contains("coordinates") && options["coordinates"] != "source")
    invalid("Scene inputs use their source coordinate convention");
  const auto mode = request["conflict"].toString("skip");
  if (mode != "skip" && mode != "rename" && mode != "overwrite")
    invalid("Unknown conflict policy");
  const auto conflict = mode == "rename"      ? ConflictPolicy::Rename
                        : mode == "overwrite" ? ConflictPolicy::Overwrite
                                              : ConflictPolicy::Skip;
  const auto destination =
      std::filesystem::path(request["output"].toString().toStdWString());
  const auto output =
      approved.empty()
          ? allocate_output(scope, destination, input, conflict, reserved, stop)
          : inspect_output(scope, approved, input, conflict);
  if (QFileInfo(QString::fromStdWString(output.path.wstring()))
          .suffix()
          .compare("fbx", Qt::CaseInsensitive))
    invalid("Output requires .fbx suffix");
  if (conflict == ConflictPolicy::Rename && output.exists)
    throw sk::Failure(ErrorCode::Conflict,
                      "Approved name is occupied; preflight again");
  for (const auto &source : inputs)
    (void)inspect_output(scope, output.path, source, conflict);
  QJsonObject result;
  const auto encoded = document.encode(options, result);
  auto canonical = request;
  canonical.remove("operation");
  canonical.remove("approval_digest");
  canonical.remove("acknowledge_losses");
  QJsonArray roots_in, roots_out;
  for (const auto &root : scope.input_roots)
    roots_in.append(QString::fromStdWString(root.wstring()));
  for (const auto &root : scope.output_roots)
    roots_out.append(QString::fromStdWString(root.wstring()));
  const QJsonObject approval_data{
      {"request", canonical},
      {"sources", sources},
      {"referenced_sources", references},
      {"losses", result["losses"]},
      {"scope_inputs", roots_in},
      {"scope_outputs", roots_out},
      {"overwrite", scope.allow_overwrite},
      {"output", QString::fromStdWString(output.path.wstring())},
      {"policy", QString::fromStdString(scope.policy_revision)},
      {"implementation", "M08-scene-v2-reviewed"}};
  const auto approval = QString::fromLatin1(
      QCryptographicHash::hash(
          QJsonDocument(approval_data).toJson(QJsonDocument::Compact),
          QCryptographicHash::Sha256)
          .toHex());
  result["version"] = 1;
  result["input"] = snapshot(input);
  result["sources"] = sources;
  result["referenced_sources"] = references;
  result["target"] = "fbx";
  result["output"] = QString::fromStdWString(output.path.wstring());
  result["output_exists"] = output.exists;
  result["approval_digest"] = approval;
  result["encoded_bytes"] = encoded.size();
  if (operation == "preflight")
    return result;
  if (operation != "convert")
    invalid("Unknown scene operation");
  if (request["approval_digest"] != approval)
    throw sk::Failure(
        ErrorCode::InputChanged,
        "Scene sources or export options changed; preflight again");
  if (!result["losses"].toArray().isEmpty() &&
      request["acknowledge_losses"] != true)
    invalid("Scene export losses require acknowledgement");
  const auto hash = QString::fromLatin1(
      QCryptographicHash::hash(encoded, QCryptographicHash::Sha256).toHex());
  OutputTransaction transaction(scope, output.path, input, conflict, inputs);
  const auto notify = [&](const OutputIntent &intent) {
    if (observer)
      observer(
          {{"temporary", QString::fromStdWString(intent.temporary.wstring())},
           {"destination",
            QString::fromStdWString(intent.destination.wstring())},
           {"file_identity", QString::fromStdString(intent.file_identity)},
           {"output_sha256", hash},
           {"result", result}});
  };
  notify(transaction.temporary());
  transaction.write(
      std::as_bytes(
          std::span(encoded.constData(), static_cast<size_t>(encoded.size()))),
      stop);
  const auto committed = transaction.commit(stop, notify);
  result["status"] = committed.skipped ? "skipped" : "succeeded";
  result["committed_output"] =
      QString::fromStdWString(committed.path.wstring());
  if (!committed.skipped)
    result["output_sha256"] = hash;
  return result;
}
} // namespace formaview::scene_export
