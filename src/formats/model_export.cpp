#include "formaview/model_export.hpp"
#include "formaview/fbx.hpp"
#include "scene_export.hpp"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace formaview::model_export {
namespace {
namespace sk = skeletal;
[[noreturn]] void fail(ErrorCode code, const char *detail) {
  throw sk::Failure(code, detail);
}
QString qpath(const std::filesystem::path &p) {
  return QString::fromStdWString(p.wstring());
}
std::filesystem::path path(const QJsonValue &v) {
  if (!v.isString() || v.toString().isEmpty())
    fail(ErrorCode::InvalidArgument, "Expected nonempty path");
  return std::filesystem::path(v.toString().toStdWString());
}
QString hash(const QJsonObject &o) {
  return QCryptographicHash::hash(
             QJsonDocument(o).toJson(QJsonDocument::Compact),
             QCryptographicHash::Sha256)
      .toHex();
}
QJsonObject snapshot(const InputSnapshot &s) {
  return {{"path", qpath(s.resolved_path)},
          {"sha256", QString::fromStdString(s.content_sha256)},
          {"file_identity", QString::fromStdString(s.file_identity)},
          {"size", static_cast<qint64>(s.size_bytes)},
          {"modified_ticks", QString::number(s.modified_ticks)}};
}
QString clip_id(const sk::Sequence &s) {
  return QString::fromStdString(s.source.identity.stable_id().value);
}
sk::FbxAxisPreset axes(const QString &value) {
  const std::map<QString, sk::FbxAxisPreset> presets{
      {"source", sk::FbxAxisPreset::Source},
      {"maya", sk::FbxAxisPreset::Maya},
      {"houdini", sk::FbxAxisPreset::Houdini},
      {"unreal", sk::FbxAxisPreset::Unreal},
      {"3ds-max", sk::FbxAxisPreset::Max},
      {"cinema4d", sk::FbxAxisPreset::Cinema4D},
      {"blender", sk::FbxAxisPreset::Blender},
      {"unity", sk::FbxAxisPreset::Unity}};
  const auto it = presets.find(value);
  if (it == presets.end())
    fail(ErrorCode::InvalidArgument, "Unknown output coordinate preset");
  return it->second;
}
double unit(const QString &value) {
  const std::map<QString, double> units{
      {"source", .01}, {"cm", .01}, {"m", 1}, {"mm", .001}, {"in", .0254}};
  const auto it = units.find(value);
  if (it == units.end())
    fail(ErrorCode::InvalidArgument, "Unknown output unit");
  return it->second;
}
std::filesystem::path companion_candidate(const QJsonObject &request) {
  const auto o = request["options"].toObject();
  if (o.contains("companion"))
    return path(o["companion"]);
  const auto animation =
      o.contains("animation") ? o["animation"] : request["input"];
  if (QFileInfo(animation.toString())
          .suffix()
          .compare("psa", Qt::CaseInsensitive) != 0)
    return {};
  auto candidate = path(animation);
  candidate.replace_extension(L".config");
  return candidate;
}
bool companion_exists(const AccessScope &scope, const std::filesystem::path &candidate) {
  const auto normalized = qpath(validated_local_path(candidate));
  for (const auto &unavailable : scope.unavailable_inputs)
    if (normalized.compare(qpath(validated_local_path(unavailable)), Qt::CaseInsensitive) == 0)
      return false; // Keep one batch's absent automatic config absent for every clip.
  std::error_code error;
  const auto status = std::filesystem::symlink_status(candidate, error);
  if (error && error != std::errc::no_such_file_or_directory)
    fail(ErrorCode::Io, "Cannot inspect automatic PSA companion");
  // A reparse point must reach InputLease and be rejected, not appear absent.
  return status.type() != std::filesystem::file_type::not_found;
}
std::vector<std::byte> bytes(const QString &p, std::uint64_t limit) {
  QFile f(p);
  if (!f.open(QIODevice::ReadOnly))
    fail(ErrorCode::Io, "Cannot read model conversion file");
  if (f.size() < 0 || static_cast<std::uint64_t>(f.size()) > limit)
    fail(ErrorCode::ResourceLimit, "Model conversion file exceeds byte budget");
  const auto b = f.readAll();
  if (b.size() != f.size())
    fail(ErrorCode::Io, "Incomplete model conversion read");
  std::vector<std::byte> result(static_cast<std::size_t>(b.size()));
  std::memcpy(result.data(), b.constData(), result.size());
  return result;
}
void loss(QJsonArray &out, const char *code, const QString &detail) {
  out.append(QJsonObject{{"code", code}, {"detail", detail}});
}
void actor_losses(const sk::ActorAsset &a, QJsonArray &out, QJsonArray &notices, bool include_mesh) {
  const std::set<std::string> supported{
      "ACTRHEAD",    "ANIMHEAD",  "PNTS0000", "VTXW0000",   "FACE0000",
      "FACE3200",    "MATT0000",  "REFSKELT", "RAWWEIGHTS", "VTXNORMS",
      "VERTEXCOLOR", "BONENAMES", "ANIMINFO", "ANIMKEYS",   "SCALEKEYS"};
  for (const auto &c : a.chunks) {
    if (!supported.contains(c.name) && !c.name.starts_with("EXTRAUVS"))
      loss(out, "actorx_chunk_omitted", QString::fromStdString(c.name));
  }
  if (a.mesh && include_mesh) {
    for (const auto &c : a.chunks)
      if (c.name == "MRPHINFO" || c.name == "MRPHDATA")
        fail(ErrorCode::Unsupported,
             "ActorX morph chunks cannot yet be represented by this writer");
    loss(notices, "actorx_material_slots",
         "ActorX material names and slots are retained; source files contain "
         "no resolved shading or texture graph");
    if (std::any_of(a.mesh->materials.begin(), a.mesh->materials.end(),
                    [](const auto &m) {
                      return std::any_of(m.properties.begin(),
                                         m.properties.end(),
                                         [](auto v) { return v != 0; });
                    }))
      loss(notices, "actorx_material_flags",
           "ActorX engine material flags are retained as custom properties, not interpreted as FBX shading");
    if (std::any_of(a.mesh->faces.begin(), a.mesh->faces.end(),
                    [](const auto &f) { return f.auxiliary_material != 0; }))
      loss(out, "actorx_auxiliary_material",
           "Auxiliary material indices are not exported");
  }
}
} // namespace
std::vector<std::filesystem::path>
auxiliary_inputs(const QJsonObject &request) {
  std::vector<std::filesystem::path> result;
  const auto o = request["options"].toObject();
  for (const auto key : {"reference", "animation"})
    if (o.contains(key))
      result.push_back(path(o[key]));
  const auto candidate = companion_candidate(request);
  if (!candidate.empty())
    result.push_back(candidate);
  return result;
}
bool handles(const QJsonObject &r) {
  if (r["options"].toObject()["target"] == "fbx")
    return true;
  const auto ext = QFileInfo(r["input"].toString()).suffix().toLower();
  return ext == "psk" || ext == "pskx" || ext == "psa" || scene_export::handles(ext);
}
void validate_options(const QJsonObject &o) {
  const std::set<QString> allowed{
      "target",       "coordinates", "clip_id",    "clip_ids",
      "reference",    "animation",   "companion",  "pairing_policy",
      "include_mesh", "output_axes", "output_unit", "abc_sample", "abc_seconds"};
  for (auto it = o.begin(); it != o.end(); ++it)
    if (!allowed.contains(it.key()))
      fail(ErrorCode::InvalidArgument, "Unknown model export option");
  if (o["target"] != "fbx" || (o.contains("coordinates") && o["coordinates"] != "actorx-z-up-x-forward-rh-cm" && o["coordinates"] != "source"))
    fail(ErrorCode::InvalidArgument,
         "Expected FBX target and a supported source coordinate convention");
  for (const auto key : {"clip_id", "reference", "animation", "companion",
                         "pairing_policy", "output_axes", "output_unit"})
    if (o.contains(key) && (!o[key].isString() || o[key].toString().isEmpty()))
      fail(ErrorCode::InvalidArgument,
           "Expected nonempty model export string option");
  (void)axes(o["output_axes"].toString("source"));
  (void)unit(o["output_unit"].toString("source"));
  if (o.contains("abc_sample") &&
      (!o["abc_sample"].isDouble() || o["abc_sample"].toDouble() < 0 ||
       o["abc_sample"].toDouble() != std::floor(o["abc_sample"].toDouble()) ||
       o["abc_sample"].toDouble() > 9007199254740991.0))
    fail(ErrorCode::InvalidArgument, "abc_sample must be a non-negative integer");
  if (o.contains("abc_seconds") &&
      (!o.contains("abc_sample") || !o["abc_seconds"].isDouble() ||
       !std::isfinite(o["abc_seconds"].toDouble())))
    fail(ErrorCode::InvalidArgument, "abc_seconds needs abc_sample and a finite value");
  if (o.contains("include_mesh") && !o["include_mesh"].isBool())
    fail(ErrorCode::InvalidArgument, "include_mesh must be boolean");
  if (o.contains("clip_ids")) {
    if (o.contains("clip_id") || !o["clip_ids"].isArray() ||
        o["clip_ids"].toArray().isEmpty())
      fail(ErrorCode::InvalidArgument,
           "Use clip_id or a nonempty clip_ids array, not both");
    std::set<QString> ids;
    for (const auto &id : o["clip_ids"].toArray())
      if (!id.isString() || id.toString().isEmpty() ||
          !ids.insert(id.toString()).second)
        fail(ErrorCode::InvalidArgument,
             "clip_ids must contain distinct nonempty source IDs");
  }
  if (o.contains("pairing_policy") && o["pairing_policy"] != "strict-records" &&
      o["pairing_policy"] != "reference-skeleton")
    fail(ErrorCode::InvalidArgument, "Unknown pairing policy");
}
QJsonObject capabilities() {
  QJsonArray formats{"psk", "pskx", "psa", "obj", "fbx", "pmx"};
#ifdef FORMAVIEW_GLTF_EXPORT
  formats.append("gltf"); formats.append("glb"); formats.append("vrm");
#endif
  return {
      {"id", "model.export_fbx"},
      {"input_formats", formats},
      {"output_formats", QJsonArray{"fbx"}},
      {"approval_required", true},
      {"clip_selection",
       "clip_id or clip_ids; one independent stack per selected source clip"},
      {"file_organization", QJsonArray{"per-clip", "multiple-stacks"}},
      {"output_axes", QJsonArray{"source", "maya", "houdini", "unreal",
                                 "3ds-max", "cinema4d", "blender", "unity"}},
      {"output_units", QJsonArray{"source", "cm", "m", "mm", "in"}},
      {"root_motion", "source"},
      {"psa_companion",
       "automatic same-basename .config; explicit companion overrides"},
      {"abc_frame_export",
       QJsonObject{{"input", "abc"},
                   {"options", QJsonArray{"abc_sample", "abc_seconds"}},
                   {"content", "one timeline sample; polygon meshes only; "
                               "transforms baked; no animation"}}},
      {"writer", QString::fromStdString(sk::fbx_writer_version())}};
}
QJsonObject
execute(const QJsonObject &request, const AccessScope &scope, sk::Limits limits,
        std::stop_token stop, std::span<const InputSnapshot> protected_inputs,
        const std::function<void(const QJsonObject &)> &commit_observer,
        std::span<const std::filesystem::path> reserved_outputs,
        const std::filesystem::path &approved_output, std::uint64_t export_working_bytes) {
  const auto operation = request["operation"].toString();
  if (operation != "inspect" && operation != "preflight" &&
      operation != "convert")
    fail(ErrorCode::InvalidArgument, "Unsupported model export operation");
  if (scene_export::handles(QFileInfo(request["input"].toString()).suffix().toLower()))
    return scene_export::execute(request, scope, limits, stop, protected_inputs, commit_observer, reserved_outputs, approved_output);
  if (operation != "inspect" && request["options"].toObject()["coordinates"] != "actorx-z-up-x-forward-rh-cm")
    fail(ErrorCode::InvalidArgument, "Explicit ActorX numerical convention required");
  sk::ReadOptions read;
  read.limits = limits;
  read.stop = stop;
  read.coordinates = {Axis::PositiveZ, Axis::PositiveX, Handedness::Right, .01,
                      "Explicit ActorX export convention"};
  std::vector<InputLease> leases;
  std::vector<InputSnapshot> inputs(protected_inputs.begin(),
                                    protected_inputs.end());
  QJsonArray source_snapshots;
  auto load = [&](const QJsonValue &value) {
    leases.push_back(
        InputLease::open(scope, path(value), limits.input_bytes, stop));
    const auto &s = leases.back().snapshot();
    inputs.push_back(s);
    source_snapshots.append(snapshot(s));
    auto asset = sk::read_actor(s.resolved_path, read);
    if (asset.source.identity.sha256 != s.content_sha256)
      fail(ErrorCode::InputChanged, "Model differs from held snapshot");
    return asset;
  };
  const auto source = load(request["input"]);
  const auto input = leases.front().snapshot();
  QJsonArray clips;
  for (const auto &s : source.sequences)
    clips.append(
        QJsonObject{{"id", clip_id(s)},
                    {"name", QString::fromStdString(s.source.original_name)},
                    {"start", s.time.source_start_seconds},
                    {"end", s.time.source_end_seconds},
                    {"fps", s.fps}});
  if (operation == "inspect")
    return {{"version", 1},
            {"input", snapshot(input)},
            {"clips", clips},
            {"root_motion", "source"},
            {"bones", static_cast<qint64>(source.bones.size())},
            {"has_mesh", source.mesh.has_value()}};
  const auto o = request["options"].toObject();
  validate_options(o);
  std::optional<sk::ActorAsset> secondary;
  const sk::ActorAsset *animation = source.animation ? &source : nullptr;
  const sk::ActorAsset *model = source.animation ? nullptr : &source;
  if (o.contains("reference")) {
    if (!source.animation || o.contains("animation"))
      fail(ErrorCode::InvalidArgument, "Reference is only valid for PSA input");
    secondary = load(o["reference"]);
    if (secondary->animation || !secondary->mesh)
      fail(ErrorCode::InvalidArgument, "Reference must be PSK or PSKX");
    model = &*secondary;
  }
  if (o.contains("animation")) {
    if (source.animation)
      fail(ErrorCode::InvalidArgument,
           "Animation companion is only valid for model input");
    secondary = load(o["animation"]);
    if (!secondary->animation)
      fail(ErrorCode::InvalidArgument, "Animation must be PSA");
    animation = &*secondary;
  }
  sk::FbxWriteOptions write;
  write.limits = limits;
  write.limits.decoded_bytes = export_working_bytes;
  write.stop = stop;
  write.output_axes = axes(o["output_axes"].toString("source"));
  write.output_metres_per_unit = unit(o["output_unit"].toString("source"));
  write.include_mesh = o["include_mesh"].toBool(true);
  write.pairing_policy = o["pairing_policy"] == "reference-skeleton"
                             ? sk::PairingPolicy::ReferenceSkeleton
                             : sk::PairingPolicy::StrictRecords;
  std::optional<sk::PsaConfig> companion;
  const auto candidate = companion_candidate(request);
  const bool explicit_companion = o.contains("companion");
  const bool automatic_companion =
      animation && !explicit_companion && !candidate.empty();
  const bool found_companion =
      !candidate.empty() &&
      (explicit_companion ||
       (automatic_companion && companion_exists(scope, candidate)));
  QJsonObject companion_info{
      {"mode", explicit_companion ? "explicit" : "automatic"},
      {"status", animation ? "not_found" : "not_applicable"}};
  if (animation && !candidate.empty())
    companion_info["candidate"] = qpath(candidate);
  if (found_companion) {
    if (!animation || !model)
      fail(ErrorCode::InvalidArgument,
           "Detected PSA config requires a reference PSK/PSKX; supply "
           "reference to apply RemoveTracks");
    leases.push_back(
        InputLease::open(scope, candidate, limits.input_bytes, stop));
    inputs.push_back(leases.back().snapshot());
    source_snapshots.append(snapshot(leases.back().snapshot()));
    companion =
        sk::read_psa_config(bytes(qpath(leases.back().snapshot().resolved_path),
                                  limits.input_bytes),
                            *animation, read);
    if (companion->source.identity.sha256 !=
        leases.back().snapshot().content_sha256)
      fail(ErrorCode::InputChanged, "PSA companion differs from held snapshot");
    companion_info["status"] = "applied";
    companion_info["input"] = snapshot(leases.back().snapshot());
    companion_info["reference"] =
        source.animation ? o["reference"] : request["input"];
    QJsonArray warnings;
    for (const auto &warning : companion->warnings)
      warnings.append(QString::fromStdString(warning));
    companion_info["warnings"] = warnings;
    write.psa_config = &*companion;
  }
  std::vector<std::size_t> selected;
  if (animation) {
    // Absent selection must not degrade to an empty id that fails matching
    // with an unrelated message: animation export always requires explicit
    // clip ids from inspect (the MCP contract tests rely on this).
    if (!o.contains("clip_ids") && !o.contains("clip_id"))
      fail(ErrorCode::InvalidArgument,
           "Animation export requires clip_id/clip_ids chosen from inspect");
    const auto ids = o.contains("clip_ids") ? o["clip_ids"].toArray()
                                            : QJsonArray{o["clip_id"]};
    if (static_cast<std::uint64_t>(ids.size()) > limits.clips)
      fail(ErrorCode::ResourceLimit, "Too many selected clips");
    for (const auto &id : ids) {
      const auto it = std::find_if(
          animation->sequences.begin(), animation->sequences.end(),
          [&](const auto &s) { return clip_id(s) == id.toString(); });
      if (it == animation->sequences.end())
        fail(ErrorCode::InvalidArgument,
             "Select existing stable clip IDs from inspect");
      selected.push_back(
          static_cast<std::size_t>(it - animation->sequences.begin()));
    }
  } else if (o.contains("clip_id") || o.contains("clip_ids") ||
             o.contains("pairing_policy"))
    fail(ErrorCode::InvalidArgument,
         "Model-only output cannot select an animation");
  if (companion) {
    QJsonArray clip_rules;
    for (const auto clip : selected) {
      QJsonArray rules;
      const auto &masks = companion->removed_tracks[clip];
      for (std::size_t i = 0; i < masks.size(); ++i)
        if (masks[i])
          rules.append(QJsonObject{
              {"bone_index", static_cast<qint64>(i)},
              {"bone_name", QString::fromStdString(
                                animation->bones[i].source.original_name)},
              {"translation", (masks[i] & 1) != 0},
              {"rotation", (masks[i] & 2) != 0}});
      clip_rules.append(QJsonObject{
          {"clip_id", clip_id(animation->sequences[clip])}, {"rules", rules}});
      if (selected.size() == 1)
        companion_info["selected_clip_rules"] = rules;
    }
    companion_info["clip_rules"] = clip_rules;
  }
  const auto conflict_text = request["conflict"].toString("skip");
  if (conflict_text != "skip" && conflict_text != "rename" &&
      conflict_text != "overwrite")
    fail(ErrorCode::InvalidArgument, "Unknown conflict policy");
  const auto conflict = conflict_text == "overwrite" ? ConflictPolicy::Overwrite
                        : conflict_text == "rename"  ? ConflictPolicy::Rename
                                                     : ConflictPolicy::Skip;
  const auto output =
      approved_output.empty()
          ? allocate_output(scope, path(request["output"]), input, conflict,
                            reserved_outputs, stop)
          : inspect_output(scope, approved_output, input, conflict);
  if (QFileInfo(qpath(output.path)).suffix().toLower() != "fbx")
    fail(ErrorCode::InvalidArgument, "FBX output requires .fbx suffix");
  if (conflict == ConflictPolicy::Rename && output.exists)
    fail(ErrorCode::Conflict, "Approved name is occupied; preflight again");
  for (const auto &s : inputs)
    (void)inspect_output(scope, output.path, s, conflict);
  QJsonArray losses, notices;
  actor_losses(source, losses, notices, write.include_mesh);
  if (secondary)
    actor_losses(*secondary, losses, notices, write.include_mesh);
  if (companion)
    for (const auto &warning : companion->warnings)
      loss(losses, "psa_companion_unmatched_rule",
           QString::fromStdString(warning));
  QJsonArray roots_in, roots_out;
  for (const auto &p : scope.input_roots)
    roots_in.append(qpath(p));
  for (const auto &p : scope.output_roots)
    roots_out.append(qpath(p));
  auto canonical = request;
  canonical.remove("operation");
  canonical.remove("approval_digest");
  canonical.remove("acknowledge_losses");
  const auto approval =
      hash({{"request", canonical},
            {"inputs", source_snapshots},
            {"psa_companion", companion_info},
            {"output", qpath(output.path)},
            {"losses", losses},
            {"scope_inputs", roots_in},
            {"scope_outputs", roots_out},
            {"overwrite", scope.allow_overwrite},
            {"revision", QString::fromStdString(scope.policy_revision)},
            {"implementation", "actorx-fbx-v7-fast-preflight"}});
  if (operation == "convert") {
    if (request["approval_digest"] != approval)
      fail(ErrorCode::InputChanged,
           "Model export approval no longer matches sources, options or scope");
    if (!losses.empty() && request["acknowledge_losses"] != true)
      fail(ErrorCode::InvalidArgument,
           "Model export losses require acknowledgement");
  }
  std::vector<sk::FbxWriteResult> bakes;
  std::vector<std::byte> encoded;
  if (operation == "preflight")
    sk::validate_fbx_export(animation ? *animation : source, selected, model, write);
  else encoded = animation ? sk::encode_fbx_clips(*animation, selected,
                                                        model, write, &bakes)
                                 : sk::encode_fbx_model(source, write);
  QJsonArray exported_clips;
  std::uint64_t bake_samples{};
  double audit_error{};
  for (std::size_t i = 0; i < selected.size(); ++i) {
    const auto &seq = animation->sequences[selected[i]];
    exported_clips.append(QJsonObject{
        {"id", clip_id(seq)},
        {"name", QString::fromStdString(seq.source.original_name)},
        {"start", seq.time.source_start_seconds},
        {"end", seq.time.source_end_seconds},
        {"fps", seq.fps}});
    if (i < bakes.size()) {
      auto clip = exported_clips.last().toObject();
      clip["bake_samples"] = static_cast<qint64>(bakes[i].samples);
      clip["audited_world_matrix_error"] = bakes[i].audited_world_matrix_error;
      exported_clips.replace(exported_clips.size() - 1, clip);
      bake_samples += bakes[i].samples;
      audit_error = std::max(audit_error, bakes[i].audited_world_matrix_error);
    }
  }
  QJsonObject result{
      {"version", 1},
      {"input", snapshot(input)},
      {"sources", source_snapshots},
      {"psa_companion", companion_info},
      {"output", qpath(output.path)},
      {"output_exists", output.exists},
      {"target", "fbx"},
      {"output_coordinates",
       QJsonObject{
           {"preset", o["output_axes"].toString("source")},
           {"unit", o["output_unit"].toString("source")},
           {"metres_per_unit", write.output_metres_per_unit},
           {"length_scale", .01 / write.output_metres_per_unit},
           {"converted", write.output_axes != sk::FbxAxisPreset::Source ||
                             write.output_metres_per_unit != .01}}},
      {"root_motion", "source"},
      {"losses", losses},
      {"notices", notices},
      {"approval_digest", approval},
      {"encoded_bytes", static_cast<qint64>(encoded.size())},
      {"clips", exported_clips},
      {"clip_id",
       selected.size() == 1
           ? QJsonValue(clip_id(animation->sequences[selected.front()]))
           : QJsonValue(QJsonValue::Null)},
      {"bake_samples", static_cast<qint64>(bake_samples)},
      {"audited_world_matrix_error", audit_error}};
  if (operation == "preflight") {
    result.remove("encoded_bytes");
    result.remove("bake_samples");
    result.remove("audited_world_matrix_error");
    result["validation_stage"] = "source-and-options";
    return result;
  }
  result["validation_stage"] = "encoded-and-audited";
  const auto output_hash = QString::fromStdString(sk::sha256(encoded, stop));
  OutputTransaction transaction(scope, output.path, input, conflict, inputs);
  auto observe = [&](const OutputIntent &intent) {
    if (commit_observer)
      commit_observer(
          {{"temporary", qpath(intent.temporary)},
           {"destination", qpath(intent.destination)},
           {"file_identity", QString::fromStdString(intent.file_identity)},
           {"output_sha256", output_hash},
           {"result", result}});
    if (automatic_companion && !found_companion && companion_exists(scope, candidate))
      fail(ErrorCode::InputChanged,
           "PSA config appeared after preflight; preflight again to apply it");
  };
  observe(transaction.temporary());
  transaction.write(encoded, stop);
  const auto committed = transaction.commit(stop, observe);
  result["status"] = committed.skipped ? "skipped" : "succeeded";
  result["committed_output"] = qpath(committed.path);
  if (!committed.skipped)
    result["output_sha256"] = output_hash;
  return result;
}
} // namespace formaview::model_export
