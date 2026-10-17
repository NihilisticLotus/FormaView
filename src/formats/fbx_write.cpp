#include "formaview/fbx.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
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
} // namespace
std::string fbx_writer_version() { return FBXSDK_VERSION_STRING; }
FbxWriteResult write_fbx_clip(const ActorAsset &a, std::size_t clip, const std::filesystem::path &output,
                              const ActorAsset *paired, const FbxWriteOptions &options) {
    cancelled(options.stop);
    if (!a.animation || clip >= a.sequences.size() || a.bones.empty())
        throw Failure(ErrorCode::InvalidArgument, "Writer requires an ActorX animation clip");
    convention(a.source.coordinates);
    if (!std::isfinite(options.max_world_matrix_error) || options.max_world_matrix_error <= 0 ||
        !std::has_single_bit(options.max_sample_subdivisions) || options.max_sample_subdivisions > 1024)
        throw Failure(ErrorCode::InvalidArgument, "Invalid FBX bake accuracy or subdivision limit");
    const auto &seq = a.sequences[clip];
    if (seq.frame_count <= 0 || !std::isfinite(seq.fps) || seq.fps <= 0 || !seq.uniform_key_deltas)
        throw Failure(ErrorCode::Unsupported, "Invalid or unsupported clip timing");
    const auto maximum_seconds = static_cast<double>(FBXSDK_TC_INFINITY) / FBXSDK_TC_SECOND;
    if (!std::isfinite(seq.time.source_end_seconds) || seq.time.source_end_seconds <= 0 ||
        seq.time.source_end_seconds >= maximum_seconds)
        throw Failure(ErrorCode::Unsupported, "Clip duration exceeds FBX tick range");
    const auto name_supported = [](const std::string &name) {
        return !name.empty() &&
               std::all_of(name.begin(), name.end(), [](unsigned char c) { return c >= 32 && c < 127; });
    };
    if (!name_supported(seq.source.original_name))
        throw Failure(ErrorCode::Unsupported,
                      "ActorX writer currently requires ASCII names; source bytes are retained by reader");
    const auto pairing = paired ? pair(a, *paired, 1e-4, options.pairing_policy) : Pairing{};
    if (options.psa_config && !paired)
        throw Failure(ErrorCode::InvalidArgument, "PSA companion requires a paired reference PSK");
    if (paired) {
        convention(paired->source.coordinates);
        if (!pairing.compatible)
            throw Failure(ErrorCode::InvalidArgument,
                          "PSK pairing failed: " +
                              (pairing.reasons.empty() ? std::string{} : pairing.reasons.front()));
        (void)evaluate_paired(a, pairing, clip, 0, options.psa_config, options.stop);
    }
    std::set<std::string> names;
    for (const auto &b : a.bones)
        if (!name_supported(b.source.original_name) || !names.insert(b.source.original_name).second)
            throw Failure(ErrorCode::Unsupported,
                          "Writer requires nonempty unique ASCII bone names; reader retains source bytes");
    const auto samples = static_cast<std::uint64_t>(seq.frame_count) + 1;
    if (samples > options.limits.elements || a.bones.size() > options.limits.bones ||
        samples > options.limits.decoded_bytes / (a.bones.size() * 9 * 256))
        throw Failure(ErrorCode::ResourceLimit, "FBX curve memory estimate exceeds export budget");
    std::uint64_t estimate = samples * a.bones.size() * 9 * 256;
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
    if (std::filesystem::exists(output))
        throw Failure(ErrorCode::Conflict, "FBX output already exists");
    if (!std::filesystem::is_directory(output.parent_path()))
        throw Failure(ErrorCode::Io, "FBX output directory does not exist");
    // Reserve a unique sibling without racing another writer. SDK writes only this private file.
    Temporary temporary;
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
    const auto destroy = [](FbxManager *p) {
        if (p)
            p->Destroy();
    };
    std::unique_ptr<FbxManager, decltype(destroy)> manager{FbxManager::Create(), destroy};
    if (!manager)
        throw Failure(ErrorCode::ResourceLimit, "FBX SDK manager allocation failed");
    auto *io = FbxIOSettings::Create(manager.get(), IOSROOT);
    manager->SetIOSettings(io);
    auto *scene = FbxScene::Create(manager.get(), "FormaView ActorX clip");
    scene->GetGlobalSettings().SetSystemUnit(FbxSystemUnit::cm);
    scene->GetGlobalSettings().SetAxisSystem(FbxAxisSystem(
        FbxAxisSystem::eZAxis, static_cast<FbxAxisSystem::EFrontVector>(-FbxAxisSystem::eParityEven),
        FbxAxisSystem::eRightHanded));
    scene->GetGlobalSettings().SetTimeMode(FbxTime::eCustom);
    scene->GetGlobalSettings().SetCustomFrameRate(seq.fps);
    auto *info = FbxDocumentInfo::Create(manager.get(), "Source information");
    info->mTitle = seq.source.original_name.c_str();
    info->mAuthor = "FormaView";
    const auto provenance =
        "ActorX bounded-error Euler bake. Original source samples and real seconds retained. "
        "Terminal hold at exclusive source end. PSA SHA256=" +
        a.source.identity.sha256 +
        "; reference PSK SHA256=" + (paired ? paired->source.identity.sha256 : "none") +
        "; RemoveTracks companion SHA256=" +
        (options.psa_config ? options.psa_config->source.identity.sha256 : "none") + "; pairing policy=" +
        (options.pairing_policy == PairingPolicy::ReferenceSkeleton ? "reference-skeleton"
                                                                    : "strict-records");
    info->mComment = provenance.c_str();
    scene->SetSceneInfo(info);
    std::vector<fbxsdk::FbxNode *> nodes;
    std::vector<FbxAMatrix> reference_rotations;
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
        const auto r = rotation(t);
        FbxAMatrix reference_rotation;
        reference_rotation.SetR(r);
        reference_rotations.push_back(reference_rotation);
        n->LclTranslation.Set(FbxDouble3(t.translation.x, t.translation.y, t.translation.z));
        // Keep the static bone orientation out of the animated Euler channels.
        // ActorX rest orientations often lie on an XYZ singularity; animation
        // relative to that rest basis requires far fewer accurate bake samples.
        n->SetRotationActive(true);
        n->SetPreRotation(fbxsdk::FbxNode::eSourcePivot, r);
        n->LclRotation.Set(FbxDouble3(0, 0, 0));
        n->LclScaling.Set(FbxDouble3(t.scale.x, t.scale.y, t.scale.z));
        n->SetRotationOrder(fbxsdk::FbxNode::eSourcePivot, eEulerXYZ);
        n->SetTransformationInheritType(FbxTransform::eInheritRSrs);
        nodes.push_back(n);
    }
    for (std::size_t j = 0; j < nodes.size(); ++j) {
        const auto parent = paired ? pairing.parents[j] : a.bones[j].parent;
        (parent ? nodes[*parent] : scene->GetRootNode())->AddChild(nodes[j]);
    }
    auto *stack = FbxAnimStack::Create(scene, seq.source.original_name.c_str());
    auto *layer = FbxAnimLayer::Create(scene, "BaseLayer");
    stack->AddMember(layer);
    FbxTime begin, end;
    begin.SetSecondDouble(0);
    end.SetSecondDouble(seq.time.source_end_seconds);
    FbxTimeSpan span(begin, end);
    stack->SetLocalTimeSpan(span);
    stack->SetReferenceTimeSpan(span);
    scene->GetGlobalSettings().SetTimelineDefaultTimeSpan(span);
    std::vector<std::array<FbxAnimCurve *, 9>> curves(nodes.size());
    constexpr const char *channels[]{FBXSDK_CURVENODE_COMPONENT_X, FBXSDK_CURVENODE_COMPONENT_Y,
                                     FBXSDK_CURVENODE_COMPONENT_Z};
    for (std::size_t j = 0; j < nodes.size(); ++j)
        for (std::size_t c = 0; c < 3; ++c) {
            curves[j][c] = nodes[j]->LclTranslation.GetCurve(layer, channels[c], true);
            curves[j][c + 3] = nodes[j]->LclRotation.GetCurve(layer, channels[c], true);
            curves[j][c + 6] = nodes[j]->LclScaling.GetCurve(layer, channels[c], true);
        }
    FbxWriteResult result;
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
        const auto sample_bytes = a.bones.size() * 9 * 256;
        if (bake_samples > options.limits.elements ||
            bake_samples - samples > (options.limits.decoded_bytes - estimate) / sample_bytes)
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
                    auto *curve = curves[j][c];
                    const auto key = curve->KeyAdd(time);
                    if (key < 0 || static_cast<std::uint64_t>(key) != frame)
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
                            interval_errors[source] = std::max(interval_errors[source], delta);
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
                                      "; error=" + std::to_string(interval_errors[source]));
                interval_subdivisions[source] *= 2;
            }
    }
    const auto metadata = [&](const char *name, double value) {
        auto property = FbxProperty::Create(info, FbxDoubleDT, name);
        property.ModifyFlag(FbxPropertyFlags::eUserDefined, true);
        property.Set(value);
    };
    metadata("FormaViewSourceFPS", seq.fps);
    metadata("FormaViewSourceFrames", seq.frame_count);
    metadata("FormaViewBakeFPS", result.bake_fps);
    metadata("FormaViewBakeSubdivisions", result.sample_subdivisions);
    metadata("FormaViewAuditedWorldMatrixError", result.audited_world_matrix_error);
    // FBX importers may resample at the declared file rate before running any
    // import pipeline. Declare the finest bake grid, preserving the original
    // ActorX rate/count separately rather than allowing that resample to discard keys.
    scene->GetGlobalSettings().SetCustomFrameRate(result.bake_fps);
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
        for (const auto &face : m.faces) {
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
    auto *exporter = FbxExporter::Create(manager.get(), "FormaView exporter");
    auto stop = options.stop;
    exporter->SetProgressCallback(progress, &stop);
    const auto format = manager->GetIOPluginRegistry()->FindWriterIDByDescription("FBX binary (*.fbx)");
    if (format < 0 || !exporter->Initialize(utf8(temporary.path).c_str(), format, io) ||
        !exporter->SetFileExportVersion(FBX_2014_00_COMPATIBLE))
        throw Failure(ErrorCode::Io, "FBX SDK initialize/version failed: " +
                                         std::string(exporter->GetStatus().GetErrorString()));
    if (!exporter->Export(scene)) {
        cancelled(stop);
        throw Failure(ErrorCode::Io,
                      "FBX SDK export failed: " + std::string(exporter->GetStatus().GetErrorString()));
    }
    exporter->Destroy();
    cancelled(stop);
    // MoveFileEx without REPLACE_EXISTING atomically refuses an output created meanwhile.
    if (!MoveFileExW(temporary.path.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH))
        throw Failure(std::filesystem::exists(output) ? ErrorCode::Conflict : ErrorCode::Io,
                      "Cannot commit FBX output");
    temporary.path.clear();
    return result;
}
} // namespace formaview::skeletal
