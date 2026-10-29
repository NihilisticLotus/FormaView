#include "formaview/fbx.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fbxsdk.h>
#include <limits>
#include <memory>
#include <set>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace formaview::skeletal {
namespace {
// The product service exports to bounded memory and lets OutputTransaction own
// the only filesystem publication. A killed preflight cannot leave SDK files.
class ExportStream final : public FbxStream {
public:
    std::vector<std::byte> data;
    ExportStream(std::uint64_t limit, std::stop_token stop)
        : limit_(std::min<std::uint64_t>(limit, LONG_MAX)), stop_(stop) {}
    EState GetState() override { return open_ ? eOpen : eClosed; }
    bool Open(void *) override { open_ = true; position_ = 0; return true; }
    bool Close() override { open_ = false; position_ = 0; return true; }
    bool Flush() override { return !error_; }
    int Write(const void *source, int size) override {
        if (error_ || stop_.stop_requested() || size < 0 ||
            static_cast<std::uint64_t>(size) > limit_ - position_) {
            error_ = 1;
            return 0;
        }
        try {
            const auto end = position_ + static_cast<std::size_t>(size);
            if (end > data.size()) data.resize(end);
            if (size) std::memcpy(data.data() + position_, source, static_cast<std::size_t>(size));
            position_ = end;
            return size;
        } catch (const std::bad_alloc &) { error_ = 1; return 0; }
    }
    int Read(void *destination, int size) const override {
        if (size < 0 || position_ > data.size()) return 0;
        const auto count = std::min(static_cast<std::size_t>(size), data.size() - position_);
        if (count) std::memcpy(destination, data.data() + position_, count);
        position_ += count;
        return static_cast<int>(count);
    }
    int GetReaderID() const override { return -1; }
    int GetWriterID() const override { return -1; }
    void Seek(const FbxInt64 &offset, const FbxFile::ESeekPos &origin) override {
        const auto base = origin == FbxFile::eBegin ? 0LL : origin == FbxFile::eEnd
            ? static_cast<FbxInt64>(data.size()) : static_cast<FbxInt64>(position_);
        if (offset < -base || offset > static_cast<FbxInt64>(limit_) - base) { error_ = 1; return; }
        position_ = static_cast<std::size_t>(base + offset);
    }
    long GetPosition() const override { return static_cast<long>(position_); }
    void SetPosition(long value) override {
        if (value < 0 || static_cast<std::uint64_t>(value) > limit_) { error_ = 1; return; }
        position_ = static_cast<std::size_t>(value);
    }
    int GetError() const override { return error_; }
    void ClearError() override {} // Budget/cancellation errors are sticky.
private:
    std::uint64_t limit_;
    std::stop_token stop_;
    mutable std::size_t position_{};
    bool open_{};
    int error_{};
};
void cancelled(std::stop_token stop) {
    if (stop.stop_requested())
        throw Failure(ErrorCode::Cancelled, "FBX export cancelled");
}
bool progress(void *user, float, const char *) {
    return !static_cast<std::stop_token *>(user)->stop_requested();
}
FbxAMatrix sdk_matrix(const Matrix4 &value) {
    FbxAMatrix m;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            m[row][col] = value[static_cast<std::size_t>(col * 4 + row)];
    return m;
}
FbxVector4 rotation(const Transform &t) {
    FbxAMatrix m;
    m.SetQ(FbxQuaternion(t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w));
    return m.GetR();
}
std::string utf8(const std::filesystem::path &p) {
    const auto s = p.u8string();
    return {reinterpret_cast<const char *>(s.data()), s.size()};
}
struct Temporary {
    std::filesystem::path path;
    ~Temporary() {
        std::error_code ec;
        if (!path.empty())
            std::filesystem::remove(path, ec);
    }
};
void convention(const Coordinates &c) {
    if (c.up != Axis::PositiveZ || c.forward != Axis::PositiveX || c.handedness != Handedness::Right ||
        !c.metres_per_unit || std::abs(*c.metres_per_unit - 0.01) > 1e-12)
        throw Failure(
            ErrorCode::Unsupported,
            "Writer requires explicit verified Z-up/X-forward right-handed centimetre numerical convention");
}
FbxAxisSystem output_axes(FbxAxisPreset preset) {
    using A = FbxAxisSystem;
    switch (preset) {
    case FbxAxisPreset::Source:
        return A(A::eZAxis, static_cast<A::EFrontVector>(-A::eParityEven), A::eRightHanded);
    case FbxAxisPreset::Maya:
    case FbxAxisPreset::Houdini: return A::MayaYUp;
    case FbxAxisPreset::Unreal:
        return A(A::eZAxis, static_cast<A::EFrontVector>(-A::eParityEven), A::eLeftHanded);
    case FbxAxisPreset::Max:
    case FbxAxisPreset::Blender: return A::Max;
    case FbxAxisPreset::Cinema4D:
    case FbxAxisPreset::Unity:
        return A(A::eYAxis, static_cast<A::EFrontVector>(-A::eParityOdd), A::eLeftHanded);
    }
    throw Failure(ErrorCode::InvalidArgument, "Invalid output axis preset");
}
} // namespace
std::string fbx_writer_version() { return FBXSDK_VERSION_STRING; }
static FbxWriteResult write_actor_fbx(const ActorAsset &a, std::size_t first_clip, const std::filesystem::path &output,
                              const ActorAsset *paired, const FbxWriteOptions &options, bool model_only,
                              std::vector<std::byte> *encoded = nullptr,
                              std::span<const std::size_t> clips = {},
                              std::vector<FbxWriteResult> *clip_results = nullptr) {
    cancelled(options.stop);
    if (!model_only && (!a.animation || first_clip >= a.sequences.size() || a.bones.empty()))
        throw Failure(ErrorCode::InvalidArgument, "Writer requires an ActorX animation first_clip");
    convention(a.source.coordinates);
    const auto target_axes = output_axes(options.output_axes);
    if (!std::isfinite(options.output_metres_per_unit) || options.output_metres_per_unit <= 0 ||
        options.output_metres_per_unit > 1e6 || options.output_metres_per_unit < 1e-9)
        throw Failure(ErrorCode::InvalidArgument, "Invalid output length unit");
    if (!std::isfinite(options.max_world_matrix_error) || options.max_world_matrix_error <= 0 ||
        !std::has_single_bit(options.max_sample_subdivisions) || options.max_sample_subdivisions > 1024)
        throw Failure(ErrorCode::InvalidArgument, "Invalid FBX bake accuracy or subdivision limit");
    if (model_only && (a.animation || !a.mesh || !options.include_mesh || options.psa_config))
        throw Failure(ErrorCode::InvalidArgument, "Model export requires a PSK mesh without animation options");
    std::vector<std::size_t> selection;
    if (!model_only) {
        if (clips.empty()) selection.push_back(first_clip);
        else selection.assign(clips.begin(), clips.end());
    }
    if (selection.size() > options.limits.clips)
        throw Failure(ErrorCode::ResourceLimit, "Too many selected clips");
    std::set<std::size_t> unique_clips;
    for (const auto selected : selection)
        if (selected >= a.sequences.size() || !unique_clips.insert(selected).second)
            throw Failure(ErrorCode::InvalidArgument, "Clip indices must exist and be distinct");
    if (clip_results) clip_results->clear();
    const Sequence model_sequence{};
    const auto &first_sequence = model_only ? model_sequence : a.sequences[first_clip];
    for (const auto selected : selection) {
    const auto &seq = a.sequences[selected];
    if (seq.frame_count <= 0 || !std::isfinite(seq.fps) || seq.fps <= 0 || !seq.uniform_key_deltas)
        throw Failure(ErrorCode::Unsupported, "Invalid or unsupported first_clip timing");
    const auto maximum_seconds = static_cast<double>(FBXSDK_TC_INFINITY) / FBXSDK_TC_SECOND;
    if (!std::isfinite(seq.time.source_end_seconds) || seq.time.source_end_seconds <= 0 ||
        seq.time.source_end_seconds >= maximum_seconds)
        throw Failure(ErrorCode::Unsupported, "Clip duration exceeds FBX tick range");
    }
    const auto name_supported = [](const std::string &name) {
        return !name.empty() &&
               std::all_of(name.begin(), name.end(), [](unsigned char c) { return c >= 32 && c < 127; });
    };
    for (const auto selected : selection)
        if (!name_supported(a.sequences[selected].source.original_name))
            throw Failure(ErrorCode::Unsupported,
                          "ActorX writer currently requires ASCII names; source bytes are retained by reader");
    auto pairing = !model_only && paired ? pair(a, *paired, 1e-4, options.pairing_policy) : Pairing{};
    if (model_only) {
        paired = &a;
        pairing.compatible = true;
        for (std::size_t j = 0; j < a.bones.size(); ++j) {
            if (!a.bones[j].default_local_valid || !a.bones[j].bind_world)
                throw Failure(ErrorCode::Unsupported, "Model skeleton requires valid reference transforms");
            pairing.animation_to_mesh.push_back(static_cast<std::uint32_t>(j));
            pairing.parents.push_back(a.bones[j].parent);
        }
    }
    if (!paired && std::any_of(a.bones.begin(), a.bones.end(),
                               [](const auto &bone) { return !bone.default_local_valid; }))
        throw Failure(ErrorCode::Unsupported,
                      "PSA reference records are placeholders; supply an explicit reference PSK");
    if (options.psa_config && !paired)
        throw Failure(ErrorCode::InvalidArgument, "PSA companion requires a paired reference PSK");
    if (paired) {
        convention(paired->source.coordinates);
        if (!pairing.compatible)
            throw Failure(ErrorCode::InvalidArgument,
                          "PSK pairing failed: " +
                              (pairing.reasons.empty() ? std::string{} : pairing.reasons.front()));
        for (const auto selected : selection)
            (void)evaluate_paired(a, pairing, selected, 0, options.psa_config, options.stop);
    }
    std::set<std::string> names;
    for (const auto &b : a.bones)
        if (!name_supported(b.source.original_name) || !names.insert(b.source.original_name).second)
            throw Failure(ErrorCode::Unsupported,
                          "Writer requires nonempty unique ASCII bone names; reader retains source bytes");
    std::uint64_t initial_samples{};
    for (const auto selected : selection) {
        const auto count = static_cast<std::uint64_t>(a.sequences[selected].frame_count) + 1;
        if (count > options.limits.elements - initial_samples)
            throw Failure(ErrorCode::ResourceLimit, "Combined animation sample budget exceeded");
        initial_samples += count;
    }
    auto total_curve_samples = initial_samples;
    if (initial_samples > options.limits.elements || a.bones.size() > options.limits.bones)
        throw Failure(ErrorCode::ResourceLimit, "FBX animation work budget exceeded");
    struct CurveProfile {
        std::vector<std::array<bool, 3>> constant; // translation, rotation, scale
        std::uint64_t rotation_channels{}, source_keys{};
    };
    std::vector<CurveProfile> profiles;
    std::uint64_t estimate{};
    const auto charge = [&](std::uint64_t count, std::uint64_t bytes) {
        if (count > (options.limits.decoded_bytes - estimate) / bytes)
            throw Failure(ErrorCode::ResourceLimit, "FBX curve memory estimate exceeds export budget");
        estimate += count * bytes;
    };
    for (const auto selected : selection) {
        const auto &seq = a.sequences[selected];
        const auto pose_at = [&](double seconds) {
            return paired ? evaluate_paired(a, pairing, selected, seconds, options.psa_config, options.stop)
                          : evaluate(a, selected, seconds, options.stop);
        };
        const auto first = pose_at(0);
        CurveProfile profile;
        profile.constant.assign(a.bones.size(), {true, true, true});
        const auto same_vector = [](const Vec3 &x, const Vec3 &y) {
            return x.x == y.x && x.y == y.y && x.z == y.z;
        };
        for (std::int32_t frame = 1; frame < seq.frame_count; ++frame) {
            const auto pose = pose_at(static_cast<double>(frame) / seq.fps);
            for (std::size_t j = 0; j < a.bones.size(); ++j) {
                const auto &x = first.local[j], &y = pose.local[j];
                auto &constant = profile.constant[j];
                constant[0] &= same_vector(x.translation, y.translation);
                constant[2] &= same_vector(x.scale, y.scale);
                const auto &q = x.rotation, &r = y.rotation;
                constant[1] &= (q.x == r.x && q.y == r.y && q.z == r.z && q.w == r.w) ||
                               (q.x == -r.x && q.y == -r.y && q.z == -r.z && q.w == -r.w);
            }
        }
        // Each stack owns its constants: different clips may have different held poses.
        // Reserve fixed curve/profile overhead as well as 256 bytes per actual key.
        charge(a.bones.size(), 9 * 4096);
        for (const auto &constant : profile.constant)
            for (std::size_t group = 0; group < 3; ++group) {
                const auto keys = 3 * (constant[group] ? 1ULL : static_cast<std::uint64_t>(seq.frame_count) + 1);
                charge(keys, 256);
                profile.source_keys += keys;
                if (group == 1 && !constant[group]) profile.rotation_channels += 3;
            }
        profiles.push_back(std::move(profile));
    }
    if (paired && options.include_mesh) {
        const auto &m = *paired->mesh;
        const auto mesh_estimate = m.points.size() * 512 + m.faces.size() * 2048 + m.weights.size() * 256;
        if (mesh_estimate > options.limits.decoded_bytes - estimate)
            throw Failure(ErrorCode::ResourceLimit, "FBX mesh estimate exceeds export budget");
        estimate += mesh_estimate;
        for (const auto &w : m.weights)
            if (w.value < 0)
                throw Failure(ErrorCode::Unsupported,
                              "Negative source weights need a separately verified FBX link mode");
        for (const auto &material : m.materials)
            if (!name_supported(material.name))
                throw Failure(ErrorCode::Unsupported,
                              "ActorX material name encoding is not verified for writing");
        for (const auto &c : paired->chunks)
            if (c.name == "MRPHINFO" || c.name == "MRPHDATA")
                throw Failure(ErrorCode::Unsupported,
                              "Morph chunks are retained by reader but not supported by this writer");
    }
    (void)estimate; // Conservative cooperative admission, not an SDK allocator hard cap.
    Temporary temporary;
    if (!encoded) {
    if (std::filesystem::exists(output))
        throw Failure(ErrorCode::Conflict, "FBX output already exists");
    if (!std::filesystem::is_directory(output.parent_path()))
        throw Failure(ErrorCode::Io, "FBX output directory does not exist");
    // Reserve a unique sibling without racing another writer. SDK writes only this private file.
    for (unsigned i = 0; i < 100; ++i) {
        auto candidate = output;
        candidate +=
            L".formaview-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(i) + L".tmp";
        HANDLE handle = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            temporary.path = std::move(candidate);
            break;
        }
    }
    if (temporary.path.empty())
        throw Failure(ErrorCode::Io, "Cannot reserve FBX temporary output");
    }
    ExportStream stream(options.limits.decoded_bytes, options.stop);
    const auto destroy = [](FbxManager *p) {
        if (p)
            p->Destroy();
    };
    std::unique_ptr<FbxManager, decltype(destroy)> manager{FbxManager::Create(), destroy};
    if (!manager)
        throw Failure(ErrorCode::ResourceLimit, "FBX SDK manager allocation failed");
    auto *io = FbxIOSettings::Create(manager.get(), IOSROOT);
    manager->SetIOSettings(io);
    auto *scene = FbxScene::Create(manager.get(), "FormaView ActorX first_clip");
    scene->GetGlobalSettings().SetSystemUnit(FbxSystemUnit::cm);
    scene->GetGlobalSettings().SetAxisSystem(FbxAxisSystem(
        FbxAxisSystem::eZAxis, static_cast<FbxAxisSystem::EFrontVector>(-FbxAxisSystem::eParityEven),
        FbxAxisSystem::eRightHanded));
    scene->GetGlobalSettings().SetTimeMode(FbxTime::eCustom);
    scene->GetGlobalSettings().SetCustomFrameRate(model_only ? 30 : first_sequence.fps);
    auto *info = FbxDocumentInfo::Create(manager.get(), "Source information");
    info->mTitle = model_only ? "ActorX model" : first_sequence.source.original_name.c_str();
    info->mAuthor = "FormaView";
    const auto provenance =
        "ActorX bounded-error Euler bake. Original source key samples and real seconds retained. "
        "Terminal hold at exclusive source end. PSA SHA256=" +
        a.source.identity.sha256 +
        "; reference PSK SHA256=" + (paired ? paired->source.identity.sha256 : "none") +
        "; RemoveTracks companion SHA256=" +
        (options.psa_config ? options.psa_config->source.identity.sha256 : "none") + "; pairing policy=" +
        (options.pairing_policy == PairingPolicy::ReferenceSkeleton ? "reference-skeleton"
                                                                    : "strict-records");
    info->mComment = model_only ? "ActorX source model; no animation generated" : provenance.c_str();
    scene->SetSceneInfo(info);
    std::vector<fbxsdk::FbxNode *> nodes;
    std::vector<FbxAMatrix> reference_rotations;
    const auto first_pose = model_only ? Pose{} : (paired
        ? evaluate_paired(a, pairing, selection.front(), 0, options.psa_config, options.stop)
        : evaluate(a, selection.front(), 0, options.stop));
    // Choose a fixed Euler chart per bone from equivalent axis bases. A rest
    // pose (or even the first frame) may put later motion near XYZ's Y=90
    // singularity. Minimise the worst pitch over source frames and midpoints;
    // this only conditions the representation, the world-space audit below
    // still decides whether the result meets the unchanged accuracy contract.
    std::vector<std::array<FbxAMatrix, 7>> basis_candidates(model_only ? 0 : a.bones.size());
    std::vector<std::array<double, 7>> basis_scores(model_only ? 0 : a.bones.size());
    const std::array<FbxVector4, 7> offsets{{{0,0,0}, {90,0,0}, {-90,0,0},
        {0,90,0}, {0,-90,0}, {0,0,90}, {0,0,-90}}};
    if (!model_only) {
        for (std::size_t j = 0; j < a.bones.size(); ++j) {
            FbxAMatrix first; first.SetR(rotation(first_pose.local[j]));
            for (std::size_t c = 0; c < offsets.size(); ++c) {
                FbxAMatrix offset; offset.SetR(offsets[c]);
                basis_candidates[j][c] = (first * offset).Inverse();
            }
        }
        for (const auto selected : selection) {
            const auto &seq = a.sequences[selected];
            for (std::int64_t frame = 0; frame < 2LL * seq.frame_count; ++frame) {
                const auto seconds = frame * .5 / seq.fps;
                const auto pose = paired ? evaluate_paired(a, pairing, selected, seconds, options.psa_config, options.stop)
                                         : evaluate(a, selected, seconds, options.stop);
                for (std::size_t j = 0; j < a.bones.size(); ++j) {
                    FbxAMatrix r;
                    const auto &q = pose.local[j].rotation; r.SetQ(FbxQuaternion(q.x,q.y,q.z,q.w));
                    for (std::size_t c = 0; c < offsets.size(); ++c)
                        basis_scores[j][c] = std::max(basis_scores[j][c],
                            std::abs((basis_candidates[j][c] * r).GetR()[1]));
                }
            }
        }
    }
    nodes.reserve(a.bones.size());
    for (std::size_t j = 0; j < a.bones.size(); ++j) {
        cancelled(options.stop);
        const auto &b = a.bones[j];
        auto *n = fbxsdk::FbxNode::Create(scene, b.source.original_name.c_str());
        // Blender treats FBX "Root" model nodes as armature containers, dropping the
        // joint from its bone collection. LimbNode preserves the named root joint.
        auto *attribute = FbxSkeleton::Create(scene, b.source.original_name.c_str());
        attribute->SetSkeletonType(FbxSkeleton::eLimbNode);
        n->SetNodeAttribute(attribute);
        const auto &t = paired ? paired->bones[pairing.animation_to_mesh[j]].default_local : b.default_local;
        const auto best = model_only ? 0 : static_cast<std::size_t>(std::min_element(basis_scores[j].begin(), basis_scores[j].end()) - basis_scores[j].begin());
        const auto r = model_only ? rotation(t) : basis_candidates[j][best].Inverse().GetR();
        FbxAMatrix reference_rotation;
        reference_rotation.SetR(r);
        reference_rotations.push_back(reference_rotation);
        n->LclTranslation.Set(FbxDouble3(t.translation.x, t.translation.y, t.translation.z));
        // PreRotation * LclRotation retains both animation and the unanimated
        // reference pose, irrespective of the fixed basis chosen above.
        n->SetRotationActive(true);
        n->SetPreRotation(fbxsdk::FbxNode::eSourcePivot, r);
        FbxAMatrix rest_rotation;
        rest_rotation.SetQ(FbxQuaternion(t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w));
        const auto rest_relative = (reference_rotation.Inverse() * rest_rotation).GetR();
        n->LclRotation.Set(FbxDouble3(rest_relative[0], rest_relative[1], rest_relative[2]));
        n->LclScaling.Set(FbxDouble3(t.scale.x, t.scale.y, t.scale.z));
        n->SetRotationOrder(fbxsdk::FbxNode::eSourcePivot, eEulerXYZ);
        n->SetTransformationInheritType(FbxTransform::eInheritRSrs);
        nodes.push_back(n);
    }
    for (std::size_t j = 0; j < nodes.size(); ++j) {
        const auto parent = paired ? pairing.parents[j] : a.bones[j].parent;
        (parent ? nodes[*parent] : scene->GetRootNode())->AddChild(nodes[j]);
    }
    FbxWriteResult result;
    double scene_bake_fps{}, scene_end{};
    std::size_t profile_index{};
    for (const auto selected : selection) {
    const auto &profile = profiles[profile_index++];
    const auto clip = selected;
    const auto &seq = a.sequences[clip];
    const auto samples = static_cast<std::uint64_t>(seq.frame_count) + 1;
    result = {};
    auto *stack = FbxAnimStack::Create(scene, seq.source.original_name.c_str());
    auto *layer = FbxAnimLayer::Create(scene, "BaseLayer");
    stack->AddMember(layer);
    FbxTime begin, end;
    begin.SetSecondDouble(0);
    end.SetSecondDouble(seq.time.source_end_seconds);
    FbxTimeSpan span(begin, end);
    stack->SetLocalTimeSpan(span);
    stack->SetReferenceTimeSpan(span);
    scene_end = std::max(scene_end, seq.time.source_end_seconds);
    FbxTime scene_stop;
    scene_stop.SetSecondDouble(scene_end);
    FbxTimeSpan scene_span(begin, scene_stop);
    scene->GetGlobalSettings().SetTimelineDefaultTimeSpan(scene_span);
    std::vector<std::array<FbxAnimCurve *, 9>> curves(nodes.size());
    constexpr const char *channels[]{FBXSDK_CURVENODE_COMPONENT_X, FBXSDK_CURVENODE_COMPONENT_Y,
                                     FBXSDK_CURVENODE_COMPONENT_Z};
    for (std::size_t j = 0; j < nodes.size(); ++j)
        for (std::size_t c = 0; c < 3; ++c) {
            curves[j][c] = nodes[j]->LclTranslation.GetCurve(layer, channels[c], true);
            curves[j][c + 3] = nodes[j]->LclRotation.GetCurve(layer, channels[c], true);
            curves[j][c + 6] = nodes[j]->LclScaling.GetCurve(layer, channels[c], true);
        }
    result.source_fps = seq.fps;
    const auto pose_at = [&](double seconds) {
        return paired ? evaluate_paired(a, pairing, clip, seconds, options.psa_config, options.stop)
                      : evaluate(a, clip, seconds, options.stop);
    };
    std::vector<std::uint32_t> interval_subdivisions(static_cast<std::size_t>(seq.frame_count), 1);
    for (;;) {
        std::uint64_t bake_samples = 1;
        for (const auto count : interval_subdivisions)
            bake_samples += count;
        const auto subdivisions =
            *std::max_element(interval_subdivisions.begin(), interval_subdivisions.end());
        const auto sample_bytes = profile.rotation_channels * 256;
        if (bake_samples - samples > options.limits.elements - total_curve_samples ||
            (sample_bytes && bake_samples - samples > (options.limits.decoded_bytes - estimate) / sample_bytes))
            throw Failure(ErrorCode::ResourceLimit,
                          "FBX bake accuracy exceeds export memory budget; previous subdivisions=" +
                              std::to_string(result.sample_subdivisions) +
                              "; audit error=" + std::to_string(result.audited_world_matrix_error));
        const auto bake_fps = seq.fps * subdivisions;
        std::vector<double> key_seconds;
        std::vector<std::size_t> source_intervals;
        key_seconds.reserve(static_cast<std::size_t>(bake_samples));
        source_intervals.reserve(static_cast<std::size_t>(bake_samples - 1));
        for (std::size_t source = 0; source < interval_subdivisions.size(); ++source)
            for (std::uint32_t cell = 0; cell < interval_subdivisions[source]; ++cell) {
                key_seconds.push_back((static_cast<double>(source) +
                                       static_cast<double>(cell) / interval_subdivisions[source]) /
                                      seq.fps);
                source_intervals.push_back(source);
            }
        key_seconds.push_back(seq.time.source_end_seconds);
        for (auto &cs : curves)
            for (auto *c : cs) {
                c->KeyClear();
                c->KeyModifyBegin();
            }
        std::vector<FbxVector4> previous(nodes.size());
        for (std::uint64_t frame = 0; frame < bake_samples; ++frame) {
            cancelled(options.stop);
            const auto seconds = key_seconds[static_cast<std::size_t>(frame)];
            const auto pose = pose_at(seconds);
            FbxTime time;
            time.SetSecondDouble(seconds);
            const bool source_key = frame == 0 || frame + 1 == bake_samples ||
                source_intervals[static_cast<std::size_t>(frame)] != source_intervals[static_cast<std::size_t>(frame - 1)];
            for (std::size_t j = 0; j < nodes.size(); ++j) {
                const auto &t = pose.local[j];
                FbxAMatrix animated_rotation;
                animated_rotation.SetQ(FbxQuaternion(t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w));
                auto r = (reference_rotations[j].Inverse() * animated_rotation).GetR();
                if (frame)
                    for (int c = 0; c < 3; ++c)
                        r[c] += 360 * std::round((previous[j][c] - r[c]) / 360);
                previous[j] = r;
                const std::array<double, 9> values{
                    t.translation.x, t.translation.y, t.translation.z, r[0], r[1], r[2],
                    t.scale.x,       t.scale.y,       t.scale.z};
                for (std::size_t c = 0; c < 9; ++c) {
                    const auto group = c / 3;
                    if (profile.constant[j][group] ? frame != 0 : (group != 1 && !source_key))
                        continue;
                    auto *curve = curves[j][c];
                    const auto expected_key = curve->KeyGetCount();
                    const auto key = curve->KeyAdd(time);
                    if (key < 0 || key != expected_key)
                        throw Failure(ErrorCode::Unsupported, "FBX key times collide at FBX time resolution");
                    if (!std::isfinite(values[c]) || std::abs(values[c]) > std::numeric_limits<float>::max())
                        throw Failure(ErrorCode::Unsupported, "FBX curve value exceeds finite float range");
                    curve->KeySetValue(key, static_cast<float>(values[c]));
                    curve->KeySetInterpolation(key, FbxAnimCurveDef::eInterpolationLinear);
                }
            }
        }
        for (auto &cs : curves)
            for (auto *c : cs)
                c->KeyModifyEnd();
        // XYZ has equivalent branches (x,y,z) and (x+180,180-y,z+180).
        // Per-axis 360-degree wrapping alone jumps branches near Y=+/-90.
        // Unroll chooses a continuous equivalent representation without adding keys.
        // This fixes branch discontinuities, not the general Euler-versus-SLERP difference.
        for (auto &cs : curves) {
            cancelled(options.stop);
            FbxAnimCurve *rotations[]{cs[3], cs[4], cs[5]};
            FbxAnimCurveFilterUnroll unroll;
            if (!unroll.Apply(rotations, 3))
                throw Failure(ErrorCode::Unsupported, "FBX Euler continuity filter failed");
        }
        scene->SetCurrentAnimationStack(stack);
        scene->GetAnimationEvaluator()->Reset();
        double error{};
        std::vector<double> interval_errors(interval_subdivisions.size());
        std::vector<std::string> interval_diagnostics(interval_subdivisions.size());
        for (std::uint64_t interval = 0; interval + 1 < bake_samples; ++interval) {
            const auto source = source_intervals[static_cast<std::size_t>(interval)];
            if (interval_errors[source] > options.max_world_matrix_error)
                continue;
            for (const auto fraction : {0.25, 0.5, 0.75}) {
                cancelled(options.stop);
                const auto index = static_cast<std::size_t>(interval);
                const auto seconds =
                    key_seconds[index] + fraction * (key_seconds[index + 1] - key_seconds[index]);
                const auto expected = pose_at(seconds);
                FbxTime time;
                time.SetSecondDouble(seconds);
                for (std::size_t j = 0; j < nodes.size(); ++j) {
                    const auto actual = nodes[j]->EvaluateGlobalTransform(time);
                    for (int row = 0; row < 4; ++row)
                        for (int col = 0; col < 4; ++col) {
                            const auto delta = std::abs(expected.world[j][row * 4 + col] - actual[col][row]);
                            if (!std::isfinite(delta))
                                throw Failure(ErrorCode::Unsupported, "Nonfinite FBX bake audit matrix");
                            if (delta > interval_errors[source]) {
                                interval_errors[source] = delta;
                                if (interval_subdivisions[source] == options.max_sample_subdivisions)
                                    interval_diagnostics[source] = "; bone=" + a.bones[j].source.original_name +
                                        "; time=" + std::to_string(seconds) + "; row=" + std::to_string(row) +
                                        "; col=" + std::to_string(col);
                            }
                        }
                }
            }
            error = std::max(error, interval_errors[source]);
        }
        result = {subdivisions, seq.fps, bake_fps, error, bake_samples};
        if (error <= options.max_world_matrix_error) {
            break;
        }
        for (std::size_t source = 0; source < interval_subdivisions.size(); ++source)
            if (interval_errors[source] > options.max_world_matrix_error) {
                if (interval_subdivisions[source] == options.max_sample_subdivisions)
                    throw Failure(ErrorCode::Unsupported,
                                  "FBX rotation bake cannot meet requested world-matrix accuracy; interval=" +
                                      std::to_string(source) +
                                      "; error=" + std::to_string(interval_errors[source]) + interval_diagnostics[source]);
                interval_subdivisions[source] *= 2;
            }
    }
    const auto metadata = [&](const char *name, double value) {
        auto property = FbxProperty::Create(stack, FbxDoubleDT, name);
        if (selection.size() == 1) {
            auto legacy = FbxProperty::Create(info, FbxDoubleDT, name);
            legacy.ModifyFlag(FbxPropertyFlags::eUserDefined, true);
            legacy.Set(value);
        }
        property.ModifyFlag(FbxPropertyFlags::eUserDefined, true);
        property.Set(value);
    };
    metadata("FormaViewSourceFPS", seq.fps);
    metadata("FormaViewSourceFrames", seq.frame_count);
    metadata("FormaViewBakeFPS", result.bake_fps);
    metadata("FormaViewBakeSubdivisions", result.sample_subdivisions);
    metadata("FormaViewAuditedWorldMatrixError", result.audited_world_matrix_error);
    metadata("FormaViewCurveKeys", static_cast<double>(profile.source_keys +
        (result.samples - samples) * profile.rotation_channels));
    // FBX importers may resample at the declared file rate before running any
    // import pipeline. Declare the finest bake grid, preserving the original
    // ActorX rate/count separately rather than allowing that resample to discard keys.
    auto source_id = FbxProperty::Create(stack, FbxStringDT, "FormaViewSourceClipID");
    source_id.ModifyFlag(FbxPropertyFlags::eUserDefined, true);
    source_id.Set(FbxString(seq.source.identity.stable_id().value.c_str()));
    scene_bake_fps = std::max(scene_bake_fps, result.bake_fps);
    scene->GetGlobalSettings().SetCustomFrameRate(scene_bake_fps);
    estimate += (result.samples - samples) * profile.rotation_channels * 256;
    total_curve_samples += result.samples - samples;
    if (clip_results) clip_results->push_back(result);
    } // Pure models contain no animation stacks or curves.
    if (!selection.empty()) {
        scene->SetCurrentAnimationStack(scene->GetSrcObject<FbxAnimStack>(0));
        scene->GetAnimationEvaluator()->Reset();
    }
    if (paired && options.include_mesh) {
        cancelled(options.stop);
        const auto &m = *paired->mesh;
        auto *mesh = fbxsdk::FbxMesh::Create(scene, "ActorXMesh");
        auto *mesh_node = fbxsdk::FbxNode::Create(scene, "ActorXMesh");
        mesh_node->SetNodeAttribute(mesh);
        scene->GetRootNode()->AddChild(mesh_node);
        mesh->InitControlPoints(static_cast<int>(m.points.size()));
        for (std::size_t p = 0; p < m.points.size(); ++p) {
            const auto v = m.points[p];
            mesh->SetControlPointAt(FbxVector4(v.x, v.y, v.z), static_cast<int>(p));
        }
        for (const auto &material : m.materials) {
            auto *mat = FbxSurfacePhong::Create(scene, material.name.c_str());
            mesh_node->AddMaterial(mat);
        }
        auto *mats = mesh->CreateElementMaterial();
        mats->SetMappingMode(FbxLayerElement::eByPolygon);
        mats->SetReferenceMode(FbxLayerElement::eIndexToDirect);
        auto *smoothing = mesh->CreateElementSmoothing();
        smoothing->SetMappingMode(FbxLayerElement::eByPolygon);
        smoothing->SetReferenceMode(FbxLayerElement::eDirect);
        for (const auto &face : m.faces) {
            cancelled(options.stop);
            smoothing->GetDirectArray().Add(static_cast<int>(face.smoothing));
            mesh->BeginPolygon(face.material);
            for (auto it = face.wedges.rbegin(); it != face.wedges.rend(); ++it)
                mesh->AddPolygon(static_cast<int>(m.wedges[*it].point));
            mesh->EndPolygon();
        }
        auto uv = [&](const char *name, const std::vector<Vec2> *extra) {
            auto *set = mesh->CreateElementUV(name);
            set->SetMappingMode(FbxLayerElement::eByPolygonVertex);
            set->SetReferenceMode(FbxLayerElement::eDirect);
            for (const auto &face : m.faces)
                for (auto it = face.wedges.rbegin(); it != face.wedges.rend(); ++it) {
                    const auto value = extra ? (*extra)[*it] : m.wedges[*it].uv;
                    set->GetDirectArray().Add(FbxVector2(value.x, 1 - value.y));
                }
        };
        uv("UV0", nullptr);
        for (const auto &[name, values] : m.extra_uvs)
            uv(name.c_str(), &values);
        if (!m.normals.empty()) {
            auto *ns = mesh->CreateElementNormal();
            ns->SetMappingMode(FbxLayerElement::eByControlPoint);
            ns->SetReferenceMode(FbxLayerElement::eDirect);
            for (const auto v : m.normals)
                ns->GetDirectArray().Add(FbxVector4(v.x, v.y, v.z));
        }
        if (!m.colours.empty()) {
            auto *cs = mesh->CreateElementVertexColor();
            cs->SetMappingMode(FbxLayerElement::eByPolygonVertex);
            cs->SetReferenceMode(FbxLayerElement::eDirect);
            for (const auto &face : m.faces)
                for (auto it = face.wedges.rbegin(); it != face.wedges.rend(); ++it) {
                    const auto c = m.colours[*it];
                    cs->GetDirectArray().Add(
                        FbxColor(c[0] / 255.0, c[1] / 255.0, c[2] / 255.0, c[3] / 255.0));
                }
        }
        if (!nodes.empty()) {
        auto *skin = fbxsdk::FbxSkin::Create(scene, "ActorXSkin");
        skin->SetSkinningType(fbxsdk::FbxSkin::eLinear);
        std::vector<std::uint32_t> mesh_to_animation(nodes.size());
        for (std::size_t j = 0; j < nodes.size(); ++j)
            mesh_to_animation[pairing.animation_to_mesh[j]] = static_cast<std::uint32_t>(j);
        std::vector<fbxsdk::FbxCluster *> clusters(nodes.size());
        for (std::size_t j = 0; j < nodes.size(); ++j) {
            auto *c = fbxsdk::FbxCluster::Create(scene, paired->bones[j].source.original_name.c_str());
            c->SetLink(nodes[mesh_to_animation[j]]);
            c->SetLinkMode(fbxsdk::FbxCluster::eNormalize);
            c->SetTransformMatrix(FbxAMatrix());
            c->SetTransformLinkMatrix(sdk_matrix(*paired->bones[j].bind_world));
            skin->AddCluster(c);
            clusters[j] = c;
        }
        for (const auto &w : m.weights) {
            cancelled(options.stop);
            clusters[w.bone]->AddControlPointIndex(static_cast<int>(w.point), w.value);
        }
        mesh->AddDeformer(skin);
        auto *bind = FbxPose::Create(scene, "ActorXBindPose");
        bind->SetIsBindPose(true);
        bind->Add(mesh_node, FbxMatrix(FbxAMatrix()));
        for (std::size_t j = 0; j < nodes.size(); ++j)
            bind->Add(nodes[mesh_to_animation[j]], FbxMatrix(sdk_matrix(*paired->bones[j].bind_world)));
        scene->AddPose(bind);
        FbxStatus bind_status;
        if (!bind->IsValidBindPose(scene->GetRootNode(), 1e-4, &bind_status))
            throw Failure(ErrorCode::InvalidArgument,
                          "FBX SDK rejected bind pose: " + std::string(bind_status.GetErrorString()));
    }
    }
    cancelled(options.stop);
    // Convert the entire hierarchy, curves, geometry, bind poses and clusters.
    // ConvertScene alone cannot represent a handedness change.
    if (target_axes != scene->GetGlobalSettings().GetAxisSystem())
        target_axes.DeepConvertScene(scene);
    const FbxSystemUnit target_unit(options.output_metres_per_unit * 100.0);
    if (target_unit != scene->GetGlobalSettings().GetSystemUnit())
        target_unit.ConvertScene(scene);
    scene->GetAnimationEvaluator()->Reset();
    cancelled(options.stop);
    auto *exporter = FbxExporter::Create(manager.get(), "FormaView exporter");
    auto stop = options.stop;
    exporter->SetProgressCallback(progress, &stop);
    const auto format = manager->GetIOPluginRegistry()->FindWriterIDByDescription("FBX binary (*.fbx)");
    if (format < 0 || !(encoded ? exporter->Initialize(&stream, nullptr, format, io)
                               : exporter->Initialize(utf8(temporary.path).c_str(), format, io)) ||
        !exporter->SetFileExportVersion(FBX_2014_00_COMPATIBLE))
        throw Failure(ErrorCode::Io, "FBX SDK initialize/version failed: " +
                                         std::string(exporter->GetStatus().GetErrorString()));
    if (!exporter->Export(scene)) {
        cancelled(stop);
        if (stream.GetError())
            throw Failure(ErrorCode::ResourceLimit, "FBX encoded output exceeded memory stream budget");
        throw Failure(ErrorCode::Io,
                      "FBX SDK export failed: " + std::string(exporter->GetStatus().GetErrorString()));
    }
    exporter->Destroy();
    cancelled(stop);
    if (encoded) {
        if (stream.GetError() || stream.data.empty())
            throw Failure(ErrorCode::ResourceLimit, "FBX encoded output stream failed");
        *encoded = std::move(stream.data);
        return result;
    }
    // MoveFileEx without REPLACE_EXISTING atomically refuses an output created meanwhile.
    if (!MoveFileExW(temporary.path.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH))
        throw Failure(std::filesystem::exists(output) ? ErrorCode::Conflict : ErrorCode::Io,
                      "Cannot commit FBX output");
    temporary.path.clear();
    return result;
}
FbxWriteResult write_fbx_clip(const ActorAsset &animation, std::size_t clip,
                              const std::filesystem::path &output, const ActorAsset *paired,
                              const FbxWriteOptions &options) {
    return write_actor_fbx(animation, clip, output, paired, options, false);
}
void write_fbx_model(const ActorAsset &model, const std::filesystem::path &output,
                     const FbxWriteOptions &options) {
    (void)write_actor_fbx(model, 0, output, nullptr, options, true);
}
std::vector<std::byte> encode_fbx_model(const ActorAsset &model, const FbxWriteOptions &options) {
    std::vector<std::byte> encoded;
    (void)write_actor_fbx(model, 0, {}, nullptr, options, true, &encoded);
    return encoded;
}
std::vector<std::byte> encode_fbx_clip(const ActorAsset &animation, std::size_t clip,
    const ActorAsset *paired, const FbxWriteOptions &options, FbxWriteResult *result) {
    std::vector<std::byte> encoded;
    const auto bake = write_actor_fbx(animation, clip, {}, paired, options, false, &encoded);
    if (result) *result = bake;
    return encoded;
}
std::vector<std::byte> encode_fbx_clips(const ActorAsset &animation, std::span<const std::size_t> clips,
    const ActorAsset *paired, const FbxWriteOptions &options, std::vector<FbxWriteResult> *results) {
    if (clips.empty()) throw Failure(ErrorCode::InvalidArgument, "Select at least one animation clip");
    std::vector<std::byte> encoded;
    (void)write_actor_fbx(animation, clips.front(), {}, paired, options, false, &encoded, clips, results);
    return encoded;
}
} // namespace formaview::skeletal
