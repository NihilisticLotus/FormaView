#include "model_session.hpp"
#include "formaview/fbx.hpp"
#include "formaview/file_access.hpp"
#include "formaview/preview.hpp"
#include "model_document.hpp"
#include "formaview/skeleton_compare.hpp"
#include "model_abc.hpp"
#include "model_metadata.hpp"
#include "model_texture.hpp"
#include <QDir>
#include <QDebug>
#include <QCryptographicHash>
#include <QDataStream>
#include <QFileInfo>
#include <QFile>
#include <QRegularExpression>
#include <QUrl>
#include <QVector3D>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

QByteArray modelSourceDigest(const ModelDocument &document) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  // Stream bounded chunks into the hash; no whole-model serialized copy.
  const auto append = [&](const auto &value) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream << value;
    hash.addData(bytes);
  };
  append(document.nodes);
  append(quint64(document.meshes.size()));
  for (const auto &mesh : document.meshes) {
    append(mesh.id); append(mesh.nodeId);
    append(quint64(mesh.raw.size()));
    for (const auto &v : mesh.raw) { append(v.x); append(v.y); append(v.z); }
    append(quint64(mesh.skins.size()));
    for (const auto &skin : mesh.skins) {
      append(skin.id); append(skin.method); append(skin.bones); append(skin.palette);
      append(quint64(skin.weights.size()));
      for (const auto &point : skin.weights) {
        append(quint64(point.size()));
        for (const auto &weight : point) { append(weight.bone); append(weight.value); }
      }
    }
  }
  return hash.result();
}

QByteArray ModelSession::sourceDataDigest() const {
  return document_ ? modelSourceDigest(*document_) : QByteArray{};
}

namespace {
using namespace formaview;
using Mesh = ModelMesh;
using Skin = ModelSkin;
QString text(const std::string &s) { return QString::fromStdString(s); }
bool validAnimationWeights(const ModelSkin &skin) {
  for (const auto &point : skin.weights) {
    double sum=0;
    for (const auto &weight : point) {
      if (!std::isfinite(weight.value) || weight.value<0 || weight.bone<0 ||
          weight.bone>=skin.bones.size()) return false;
      sum+=weight.value;
    }
    if (!std::isfinite(sum) || (!point.empty() && sum<=0)) return false;
  }
  return true;
}
double scalar(const Buffer &b, std::size_t index) {
  if (b.scalar == ScalarType::Float64) {
    double v;
    std::memcpy(&v, b.bytes.data() + index * 8, 8);
    return v;
  }
  if (b.scalar == ScalarType::Float32) {
    float v;
    std::memcpy(&v, b.bytes.data() + index * 4, 4);
    return v;
  }
  throw skeletal::Failure(ErrorCode::Unsupported,
                          "Unsupported model attribute scalar");
}
QMatrix4x4 matrix(const Matrix4 &v) {
  QMatrix4x4 m;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      m(r, c) = static_cast<float>(v[static_cast<std::size_t>(r * 4 + c)]);
  return m;
}
void check(std::stop_token stop) {
  if (stop.stop_requested())
    throw skeletal::Failure(ErrorCode::Cancelled, "Model load cancelled");
}
int initialMeshFor(const ModelDocument &document) {
  std::map<QString,int> levels;
  for(const auto &mesh:document.meshes) {
    const auto &c=mesh.classification;
    if(!c.group.isEmpty()) {
      const auto [it,inserted]=levels.try_emplace(c.group,c.level);
      if(!inserted) it->second=std::min(it->second,c.level);
    }
  }
  for(std::size_t i=0;i<document.meshes.size();++i) {
    const auto &c=document.meshes[i].classification;
    if(!c.collision && (c.group.isEmpty() || levels.at(c.group)==c.level))
      return static_cast<int>(i);
  }
  return 0;
}
QMatrix4x4 displayBasisFor(Axis up) {
  QMatrix4x4 basis;
  switch (up) {
  case Axis::PositiveZ: basis.rotate(-90, 1, 0, 0); break;
  case Axis::NegativeZ: basis.rotate(90, 1, 0, 0); break;
  case Axis::PositiveX: basis.rotate(90, 0, 0, 1); break;
  case Axis::NegativeX: basis.rotate(-90, 0, 0, 1); break;
  case Axis::NegativeY: basis.rotate(180, 1, 0, 0); break;
  default: break;
  }
  return basis;
}
skeletal::CompareInput modelCompareInput(const ModelDocument &d) {
  skeletal::CompareInput input;
  input.source = d.hash.toStdString();
  if (d.skeletonOnly) {
    for (qsizetype i = 0; i < d.boneNames.size(); ++i) {
      const auto parent = d.boneParents.at(static_cast<std::size_t>(i));
      input.bones.push_back({static_cast<std::uint32_t>(i), d.boneNames[i].toStdString(),
          parent >= 0 ? std::optional<std::uint32_t>(parent) : std::nullopt,
          d.referenceLocals.size() == static_cast<std::size_t>(d.boneNames.size()) ||
              d.nativeAnimated || d.coordinates.startsWith("FBX")});
    }
  } else {
    QHash<QString, std::uint32_t> indices;
    for (qsizetype i = 0; i < d.nodes.size(); ++i)
      indices.insert(d.nodes[i].toMap().value("id").toString(), static_cast<std::uint32_t>(i));
    for (qsizetype i = 0; i < d.nodes.size(); ++i) {
      const auto node = d.nodes[i].toMap();
      const auto parent = node.value("parent").toString();
      input.bones.push_back({static_cast<std::uint32_t>(i), node.value("name").toString().toStdString(),
          parent.isEmpty() ? std::nullopt : std::optional<std::uint32_t>(indices.value(parent, UINT32_MAX)),
          node.value("local").toList().size() == 16});
    }
  }
  return skeletal::named_compare_input(std::move(input));
}

QVariantMap transformDiagnostics(const skeletal::transform::Evidence &a,
    const skeletal::transform::Evidence &b, const skeletal::CompareInput &source,
    const skeletal::CompareInput &target, std::stop_token stop) {
  namespace tv = skeletal::transform;
  check(stop);
  std::vector<tv::Pair> mapping;
  if (a.nodes.size() <= tv::Policy{}.maxNodes && b.nodes.size() <= tv::Policy{}.maxNodes) {
    std::map<std::uint32_t,std::size_t> sourceIds, targetIds;
    for (std::size_t i=0;i<a.nodes.size();++i) sourceIds.emplace(a.nodes[i].fileIndex,i);
    for (std::size_t i=0;i<b.nodes.size();++i) targetIds.emplace(b.nodes[i].fileIndex,i);
    if (!a.sha256.empty() && a.sha256 == b.sha256) {
      // Same content maps the full raw domain, including unnamed helper nodes.
      for (const auto &[id,i] : sourceIds)
        if (const auto it=targetIds.find(id);it!=targetIds.end()) mapping.push_back({i,it->second});
    } else {
      const auto structure = skeletal::compare_skeletons(source,target);
      if (structure.applicability == skeletal::Applicability::StructuralDirect)
        for (const auto &pair : structure.matched) {
          const auto ai=sourceIds.find(source.bones[pair.a].index);
          const auto bi=targetIds.find(target.bones[pair.b].index);
          if (ai!=sourceIds.end() && bi!=targetIds.end()) mapping.push_back({ai->second,bi->second});
        }
      // FBX has one format-defined synthetic root. Its role, unlike an
      // arbitrary empty name, is an explicit full-domain correspondence.
      std::vector<std::size_t> ar,br;
      for(std::size_t i=0;i<a.nodes.size();++i)if(a.nodes[i].syntheticRoot)ar.push_back(i);
      for(std::size_t i=0;i<b.nodes.size();++i)if(b.nodes[i].syntheticRoot)br.push_back(i);
      if(ar.size()==1&&br.size()==1&&std::none_of(mapping.begin(),mapping.end(),[&](const auto &p){return p.source==ar[0]||p.target==br[0];}))mapping.push_back({ar[0],br[0]});
      // Other unnamed raw nodes cannot be guessed from a filtered name mapping.
    }
  }
  const auto report = tv::validate(a,b,mapping,{},stop);
  const char *verdicts[] = {"compatible","incompatible","insufficient","cancelled"};
  const char *reasons[] = {"none","identity","missingEvidence","unsupportedFormat","invalidInput",
      "mappingConflict","coordinateConflict","evaluationConflict","restConflict","bindingConflict",
      "budgetExceeded","cancelled"};
  const auto complete = [](const tv::Evidence &e) {
    return QVariantMap{{"fullNodeDomain",e.fullNodeDomain},{"coordinates",e.coordinates.has_value()},
      {"bindings",e.bindings.has_value()},{"nativeSkinBindings",e.nativeSkinBindings.has_value()},
      {"evaluationNodes",static_cast<int>(std::count_if(e.nodes.begin(),e.nodes.end(),
          [](const auto &n){return n.evaluation.has_value();}))}};
  };
  const bool proven=report.verdict==tv::Verdict::Compatible;
  // Dedicated binding-conflict locator (diagnostic only; indices are file-domain node ids).
  QVariantMap bindingFault;
  if(report.bindingFault) {
    const auto &d=*report.bindingFault;
    const char *faults[]={"none","closureResidual","countMismatch","missingPeer","operatorResidual"};
    const char *matrices[]={"none","bindWorld","geometryToBone","geometryWorld","inverseBind"};
    const auto id=[](const tv::Evidence &e,const std::optional<std::size_t> &at)->QVariant{
      return at&&*at<e.nodes.size()?QVariant(static_cast<uint>(e.nodes[*at].fileIndex)):QVariant();};
    bindingFault={{"fault",QString::fromLatin1(faults[static_cast<int>(d.fault)])},
      {"matrix",QString::fromLatin1(matrices[static_cast<int>(d.matrix)])},{"native",d.native},
      {"closureSide",d.closureSide==0?QStringLiteral("source"):QStringLiteral("target")},
      {"witness",d.native?QStringLiteral("inverseBind"):d.witness==0?QStringLiteral("bindPose"):QStringLiteral("clusterLink")},
      {"sourceInstance",id(a,d.sourceInstance)},{"sourceJoint",id(a,d.sourceJoint)},
      {"targetInstance",id(b,d.targetInstance)},{"targetJoint",id(b,d.targetJoint)},
      {"element",static_cast<uint>(d.element)},{"residual",d.residual},
      {"sourceCount",static_cast<qulonglong>(d.sourceCount)},{"targetCount",static_cast<qulonglong>(d.targetCount)}};
  }
  const auto ordinary=[](const tv::Evidence &e){return std::all_of(e.nodes.begin(),e.nodes.end(),[](const auto &n){return n.evaluation&&n.evaluation->inheritance==0;});};
  // Keep the established FBX same-source identity path. Cross-file direct
  // drive is qualified only where the current chainWorlds evaluator matches.
  const bool driverSupported=a.format==tv::Format::Fbx&&b.format==tv::Format::Fbx&&
      (report.reason==tv::Reason::Identity||(ordinary(a)&&ordinary(b)));
  QVariantList mappedPairs;
  for(const auto &p:report.mapping) {
    check(stop);
    mappedPairs.append(QVariantMap{{"source",static_cast<uint>(a.nodes[p.source].fileIndex)},
      {"target",static_cast<uint>(b.nodes[p.target].fileIndex)}});
  }
  QVariantMap published{{"verdict",QString::fromLatin1(verdicts[static_cast<int>(report.verdict)])},
    {"reason",QString::fromLatin1(reasons[static_cast<int>(report.reason)])},
    {"sourceSha",text(report.sourceSha)},{"targetSha",text(report.targetSha)},
    {"sourceAdapter",text(report.sourceAdapter)},{"targetAdapter",text(report.targetAdapter)},
    {"nodesA",static_cast<int>(a.nodes.size())},{"nodesB",static_cast<int>(b.nodes.size())},
    {"mapped",static_cast<int>(report.mapping.size())},{"evidenceVersion",report.evidenceVersion},
    {"sourceEvidence",complete(a)},{"targetEvidence",complete(b)},
    {"maxTranslationMetres",report.maxTranslationMetres},{"maxRotationRadians",report.maxRotationRadians},
    {"maxScaleResidual",report.maxScaleResidual},{"maxBindingResidual",report.maxBindingResidual},
    {"mapping",mappedPairs},
    {"sourceConflictNode",report.sourceNode?QVariant(static_cast<uint>(a.nodes[*report.sourceNode].fileIndex)):QVariant()},
    {"targetConflictNode",report.targetNode?QVariant(static_cast<uint>(b.nodes[*report.targetNode].fileIndex)):QVariant()},
    {"policy",QVariantMap{{"translationMetres",report.policy.translationMetres},{"rotationRadians",report.policy.rotationRadians},
      {"relativeScale",report.policy.relativeScale},{"bindingResidual",report.policy.bindingResidual}}},
    {"bindingProof",a.format==tv::Format::Gltf?"nativeOperators":"declaredBindPose"},
    {"driverSupported",driverSupported},
    {"promotionEnabled",proven&&driverSupported&&report.reason!=tv::Reason::Identity}};
  if(!bindingFault.isEmpty())published.insert("bindingFault",bindingFault);
  return published;
}

void initializeAnimationBounds(ModelDocument &document) {
  const auto worlds = document.worldAt(0, document.animationClips.front().start);
  if (worlds.empty())
    throw skeletal::Failure(ErrorCode::InvalidArgument, "Empty animation skeleton");
  QVector3D low, high;
  bool first = true;
  for (const auto &world : worlds) {
    const auto point = document.displayBasis.map(
        QVector3D(static_cast<float>(world[3]), static_cast<float>(world[7]),
                  static_cast<float>(world[11])));
    for (int axis = 0; axis < 3; ++axis) {
      if (!std::isfinite(point[axis]))
        throw skeletal::Failure(ErrorCode::InvalidArgument,
                                "Non-finite animation bounds");
      low[axis] = first ? point[axis] : std::min(low[axis], point[axis]);
      high[axis] = first ? point[axis] : std::max(high[axis], point[axis]);
    }
    first = false;
  }
  // One display space per document: seeking must not recenter source motion.
  document.center = (low + high) * .5F;
  document.radius = std::max((high - low).length() * .5F, 1e-6F);
  if (!std::isfinite(document.radius))
    throw skeletal::Failure(ErrorCode::InvalidArgument,
                            "Animation bounds exceed display precision");
}
} // namespace
void finalizeModelGeometry(ModelDocument &document, std::stop_token stop,
                           bool allowEmpty) {
  auto *const d = &document;
  QVector3D low(std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max()),
      high = -low;
  std::size_t renderCorners = 0;
  for (const auto &value : d->nodes) {
    const auto node = value.toMap();
    d->nodeById.insert(node.value("id").toString(), node);
  }
  for (auto &m : d->meshes) {
    check(stop);
    const auto lodName =
        QRegularExpression("(?:^|[_ .-])LOD[_ .-]?(\\d+)(?:$|[_ .-])",
                           QRegularExpression::CaseInsensitiveOption)
            .match(m.name);
    const auto collisionName =
        QRegularExpression("^(?:UCX|UBX|USP|UCP)_",
                           QRegularExpression::CaseInsensitiveOption)
            .match(m.name);
    if (m.classification.group.isEmpty() && lodName.hasMatch())
      m.classificationCandidate = "LOD name candidate " + lodName.captured(1) +
                                  "; not classified automatically";
    if (collisionName.hasMatch())
      m.classificationCandidate +=
          (m.classificationCandidate.isEmpty() ? "" : "; ") +
          QString("collision prefix candidate; source association unverified");
    for (std::size_t i = 0; i < m.uvs.size(); ++i) {
      std::array<double, 4> bounds{std::numeric_limits<double>::max(),
                                   std::numeric_limits<double>::max(),
                                   -std::numeric_limits<double>::max(),
                                   -std::numeric_limits<double>::max()};
      bool invalid = false;
      for (const auto &v : m.uvs[i]) {
        if (!qIsFinite(v.x()) || !qIsFinite(v.y()) || std::abs(v.x()) > 1e7 ||
            std::abs(v.y()) > 1e7) {
          invalid = true;
          continue;
        }
        bounds[0] = std::min(bounds[0], v.x());
        bounds[1] = std::min(bounds[1], v.y());
        bounds[2] = std::max(bounds[2], v.x());
        bounds[3] = std::max(bounds[3], v.y());
      }
      m.uvBounds.push_back(bounds);
      m.invalidUVs.push_back(invalid);
      if (invalid)
        d->warnings.append(m.name + ": " +
                           m.uvNames.at(static_cast<qsizetype>(i)) +
                           " has non-finite or unrepresentable preview "
                           "coordinates; raw values retained");
    }
    if (m.triangleFaces.empty())
      for (std::size_t t = 0; t < m.triangles.size(); t += 3) {
        m.triangleFaces.push_back(t / 3);
        m.triangleCorners.push_back({0, 1, 2});
      }
    for (auto &p : m.points)
      p = d->displayBasis.map(p);
    for (auto &n : m.normals)
      n = d->displayBasis.mapVector(n);
    d->points += m.points.size();
    d->triangles += m.triangles.size() / 3;
    renderCorners += m.triangles.size();
    if (renderCorners > 2500000)
      throw skeletal::Failure(ErrorCode::ResourceLimit,
                              "Model preview vertex budget exceeded");
    for (const auto &p : m.points) {
      if (!qIsFinite(p.x()) || !qIsFinite(p.y()) || !qIsFinite(p.z()))
        throw skeletal::Failure(ErrorCode::InvalidArgument,
                                "Non-finite model position");
      for (int a = 0; a < 3; ++a) {
        low[a] = std::min(low[a], p[a]);
        high[a] = std::max(high[a], p[a]);
      }
    }
    if (!m.points.empty()) {
      m.boundsLow = m.boundsHigh = m.points.front();
      for (const auto &p : m.points)
        for (int a = 0; a < 3; ++a) {
          m.boundsLow[a] = std::min(m.boundsLow[a], p[a]);
          m.boundsHigh[a] = std::max(m.boundsHigh[a], p[a]);
        }
    }
    for (std::size_t t = 0; t < m.triangles.size(); t += 3) {
      const auto a = m.triangles[t], b = m.triangles[t + 1],
                 c = m.triangles[t + 2];
      const auto n =
          QVector3D::crossProduct(
              m.points.at(m.corners.at(b)) - m.points.at(m.corners.at(a)),
              m.points.at(m.corners.at(c)) - m.points.at(m.corners.at(a)))
              .normalized();
      for (auto i : {a, b, c})
        if (m.normals.at(i).isNull())
          m.normals[i] = n;
    }
  }
  if (!d->triangles && !d->skeletonOnly && !allowEmpty)
    throw skeletal::Failure(ErrorCode::InvalidArgument,
                            "No visible triangles in model");
  if (d->skeletonOnly || low.x() > high.x()) {
    // Animation-only documents (standalone PSA, animation-only glTF) size
    // their display space from the animated skeleton, not from meshes; an
    // explicitly allowed empty sample has no bounds either.
    d->center = QVector3D();
    d->radius = 1;
    return;
  }
  d->center = (low + high) * .5F;
  d->radius = std::max((high - low).length() * .5F, 1e-6F);
}
std::shared_ptr<ModelDocument> readModelDocument(const QString &path,
                                                 std::stop_token stop,
                                                 const QString &reference,
                                                 const QString &companion) {
  if (QFileInfo(path).suffix().compare("abc", Qt::CaseInsensitive) == 0)
    // Geometry caches lease and index the whole archive, then present the
    // first timeline sample; later samples are read on demand.
    return ModelAbcSource::open(path, stop)->read(0, stop);
  auto d = std::make_shared<ModelDocument>();
  const std::filesystem::path source(path.toStdWString());
  AccessScope scope;
  scope.input_roots = {source.parent_path()};
  const auto lease =
      InputLease::open(scope, source, 512ULL * 1024 * 1024, stop);
  skeletal::ReadOptions options;
  options.stop = stop;
  const auto suffix = QFileInfo(path).suffix().toLower();
  if (suffix == "fbx" || suffix == "obj") {
    const auto holder =
        std::make_shared<skeletal::FbxDocument>(suffix == "obj"
                                                    ? skeletal::FbxDocument::read_obj(source, options)
                                                    : skeletal::FbxDocument::read(source, options));
    const auto &file = *holder;
    const auto &data = file.data();
    if (suffix == "fbx")
      d->transformEvidence = [holder](std::stop_token token) {
        return holder->transform_evidence(token);
      };
    d->displayBasis = displayBasisFor(data.source.coordinates.up);
    if (suffix == "fbx" && data.meshes.empty() && !data.clips.empty()) {
      // M07: animation-only FBX becomes a skeleton preview document instead
      // of being rejected; target-driven external animation stays a separate
      // flow. The closure keeps the parsed document alive.
      d->skeletonOnly = true;
      d->hash = text(data.source.identity.sha256);
      d->name = QFileInfo(path).completeBaseName();
      d->coordinates = QStringLiteral("FBX · source axes preserved · display Y up");
      std::vector<std::size_t> visibleNodes;
      std::vector<std::int32_t> visibleIndex(data.nodes.size(), -1);
      for (std::size_t i = 0; i < data.nodes.size(); ++i)
        if (!data.nodes[i].implicit_scene_root) {
          visibleIndex[i] = static_cast<std::int32_t>(visibleNodes.size());
          visibleNodes.push_back(i);
        }
      for (const auto i : visibleNodes) {
        d->boneNames.push_back(text(data.nodes[i].source.original_name));
        d->boneParents.push_back(
            data.nodes[i].parent
                ? visibleIndex.at(*data.nodes[i].parent)
                : std::int32_t{-1});
      }
      for (std::size_t c = 0; c < data.clips.size(); ++c) {
        const auto &clip = data.clips[c];
        ModelAnimationClip entry;
        entry.name = text(clip.source.original_name);
        entry.start = clip.start;
        entry.end = clip.end;
        entry.fps = clip.fps > 0 ? clip.fps : 30.0;
        entry.lastFrame = clip.fps > 0
                              ? static_cast<int>((clip.end - clip.start) *
                                                 clip.fps)
                              : 0;
        d->animationClips.push_back(entry);
        d->clips.push_back(QVariantMap{
            {"name", entry.name},
            {"start", entry.start},
            {"end", entry.end},
            {"fps", entry.fps},
            {"lastFrame", entry.lastFrame}});
      }
      d->worldAt = [holder, visibleNodes](std::size_t clip, double seconds) {
        const auto pose = holder->evaluate(clip, seconds);
        std::vector<Matrix4> worlds;
        for (const auto i : visibleNodes) worlds.push_back(pose.world.at(i));
        return worlds;
      };
      d->localAt = [holder, visibleNodes, visibleIndex](std::size_t clip, double seconds) {
        const auto pose = holder->evaluate(clip, seconds);
        std::vector<Matrix4> locals;
        for (const auto i : visibleNodes) {
          const auto parent = holder->data().nodes[i].parent;
          locals.push_back(parent && visibleIndex.at(*parent) >= 0
                               ? pose.local.at(i) : pose.world.at(i));
        }
        return locals;
      };
      initializeAnimationBounds(*d);
      lease.require_unchanged(lease.snapshot());
      if (d->hash != text(lease.snapshot().content_sha256))
        throw skeletal::Failure(ErrorCode::InputChanged,
                                "Model input changed during decoding");
      return d;
    }
    std::map<QString, std::shared_ptr<const render::Texture>> sourceTextures;
    std::map<QString, QString> textureFailures;
    std::vector<InputLease> textureLeases;
    std::uint64_t decodedTextureBytes = 0, encodedTextureBytes = 0;
    const auto loadSourceTexture = [&](const skeletal::FbxMaterial &material,
                                       Mesh &mesh, std::size_t slot) {
      if (!material.colour_content && material.colour_texture.empty())
        return;
      const auto reference = material.colour_content
                                 ? QString("Embedded base colour · ") +
                                       text(material.colour_texture)
                                 : text(material.colour_texture);
      QString resolved, key;
      if (material.colour_content)
        key = "embedded:" +
              text(skeletal::sha256(*material.colour_content, stop));
      else {
        resolved = QDir::fromNativeSeparators(text(material.colour_resolved));
        if (resolved.isEmpty())
          resolved = QDir::fromNativeSeparators(text(material.colour_texture));
        if (!QDir::isAbsolutePath(resolved))
          resolved = QFileInfo(path).absoluteDir().filePath(resolved);
        if (!QFileInfo::exists(resolved)) {
          const auto declared =
              QDir::fromNativeSeparators(text(material.colour_texture));
          const auto candidate =
              QDir::isAbsolutePath(declared)
                  ? declared
                  : QFileInfo(path).absoluteDir().filePath(declared);
          if (QFileInfo::exists(candidate))
            resolved = candidate;
        }
        key = QDir::cleanPath(resolved);
      }
      key += QString("|wrap:%1,%2").arg(material.wrap_u).arg(material.wrap_v);
      mesh.references[static_cast<qsizetype>(slot)] = reference;
      if (textureFailures.contains(key))
        return;
      if (const auto cached = sourceTextures.find(key);
          cached != sourceTextures.end()) {
        mesh.textures[slot] = cached->second;
        mesh.texturePaths[static_cast<qsizetype>(slot)] =
            resolved.isEmpty() ? reference : resolved;
        return;
      }
      try {
        check(stop);
        std::shared_ptr<render::Texture> pixels;
        if (material.colour_content) {
          pixels = decodeModelTexture(
              QByteArray(
                  reinterpret_cast<const char *>(
                      material.colour_content->data()),
                  static_cast<qsizetype>(material.colour_content->size())),
              stop);
        } else {
          const std::filesystem::path imagePath(resolved.toStdWString());
          AccessScope imageScope;
          imageScope.input_roots = {imagePath.parent_path()};
          auto textureLease = InputLease::open(imageScope, imagePath,
                                               64ULL * 1024 * 1024, stop);
          const auto encodedSize = textureLease.snapshot().size_bytes;
          if (encodedSize > 512ULL * 1024 * 1024 - encodedTextureBytes)
            throw skeletal::Failure(
                ErrorCode::ResourceLimit,
                "Referenced texture encoded budget exceeded");
          encodedTextureBytes += encodedSize;
          pixels = readModelTexture(resolved, stop);
          textureLease.require_unchanged(textureLease.snapshot());
          textureLeases.push_back(std::move(textureLease));
        }
        const auto decodedSize = pixels->rgba.size() * sizeof(float);
        if (decodedSize > 256ULL * 1024 * 1024 - decodedTextureBytes)
          throw skeletal::Failure(ErrorCode::ResourceLimit,
                                  "Referenced texture decoded budget exceeded");
        decodedTextureBytes += decodedSize;
        pixels->wrapU = material.wrap_u;
        pixels->wrapV = material.wrap_v;
        pixels->linearMin = pixels->linearMag = true;
        mesh.textures[slot] = pixels;
        sourceTextures.emplace(key, std::move(pixels));
        mesh.texturePaths[static_cast<qsizetype>(slot)] =
            resolved.isEmpty() ? reference : resolved;
        d->resources.append(
            QVariantMap{{"reference", reference},
                        {"resolved", resolved},
                        {"loaded", true},
                        {"detail", "Base colour image decoded"}});
      } catch (const skeletal::Failure &error) {
        if (error.error.code == ErrorCode::Cancelled)
          throw;
        textureFailures[key] = QString::fromUtf8(error.what());
        d->resources.append(
            QVariantMap{{"reference", reference},
                        {"resolved", resolved},
                        {"loaded", false},
                        {"detail", QString::fromUtf8(error.what())}});
      }
    };
    for (std::size_t i = 0; i < data.nodes.size(); ++i) {
      const auto &node = data.nodes[i];
      d->nodes.append(QVariantMap{
          {"id", QString("node/%1").arg(i)},
          {"stableId", text(node.source.identity.stable_id().value)},
          {"name", text(node.source.original_name)},
          {"parent",
           node.parent && !data.nodes.at(*node.parent).implicit_scene_root
               ? QString("node/%1").arg(*node.parent) : QString{}},
          {"bone", node.bone},
          {"local", modelMatrixValues(node.parent && data.nodes.at(*node.parent).implicit_scene_root
                                          ? node.default_world : node.default_local)},
          {"world", modelMatrixValues(node.default_world)},
          {"geometryWorld", modelMatrixValues(node.geometry_world)},
          {"active", true}});
    }
    d->hash = text(data.source.identity.sha256);
    d->coordinates =
        suffix == "obj"
            ? QString("OBJ · source axes and units unspecified · display Y up")
            : QString("FBX · source axes preserved · %1 m/unit")
                  .arg(data.metres_per_unit, 0, 'g', 8);
    d->animNodeParents.resize(data.nodes.size());
    d->animNodeLocals.resize(data.nodes.size());
    for (std::size_t i = 0; i < data.nodes.size(); ++i) {
      d->animNodeParents[i] = data.nodes[i].parent
                                  ? static_cast<std::int32_t>(
                                        *data.nodes[i].parent)
                                  : std::int32_t{-1};
      d->animNodeLocals[i] = data.nodes[i].default_local;
    }
    for (const auto &resource : data.resources)
      d->resources.append(QVariantMap{{"reference", text(resource.reference)},
                                      {"resolved", text(resource.resolved)},
                                      {"loaded", resource.loaded},
                                      {"detail", text(resource.detail)}});
    for (const auto &src : data.meshes) {
      check(stop);
      const auto &g = src.geometry;
      const auto instances = src.instance_nodes.empty()
                                 ? std::vector<std::uint32_t>{}
                                 : src.instance_nodes;
      // An uninstanced geometry is not a visible scene object.
      for (const auto node : instances) {
        Mesh m;
        m.id = text(g.source.identity.stable_id().value) +
               QString("/instance/%1").arg(node);
        m.nodeId = QString("node/%1").arg(node);
        m.name = text(data.nodes.at(node).source.original_name);
        if (m.name.isEmpty())
          m.name = text(g.source.original_name);
        m.name += QString(" [node/%1]").arg(node);
        auto ancestor = node;
        while (data.nodes.at(ancestor).parent) {
          const auto parent = *data.nodes.at(ancestor).parent;
          const auto &group = data.nodes.at(parent);
          for (std::size_t level = 0; level < group.lod_levels.size(); ++level)
            if (group.lod_levels[level].child == ancestor &&
                m.classification.group.isEmpty()) {
              m.classification.group =
                  QString("FBX LODGroup [node/%1]").arg(parent);
              m.classification.level = static_cast<int>(level);
              m.classificationEvidence =
                  QString("Explicit FBX LODGroup node/%1 · child node/%2 · "
                          "distance %3 %4")
                      .arg(parent)
                      .arg(ancestor)
                      .arg(group.lod_levels[level].distance, 0, 'g', 10)
                      .arg(group.relative_lod_distances ? "screen percentage"
                                                        : "source world units");
            }
          ancestor = parent;
        }
        const auto &sourceMaterials = data.nodes.at(node).materials.empty()
                                          ? src.materials
                                          : data.nodes.at(node).materials;
        for (const auto &mat : sourceMaterials) {
          m.materials.append(text(mat.name));
          m.references.append(text(mat.colour_texture));
        }
        if (m.materials.isEmpty()) {
          m.materials.append("Default");
          m.references.append("");
        }
        m.textures.resize(static_cast<std::size_t>(m.materials.size()));
        m.texturePaths = QStringList(m.materials.size(), QString{});
        const auto transform = matrix(data.nodes.at(node).geometry_world);
        for (std::size_t p = 0; p < g.positions.element_count; ++p) {
          if ((p & 4095) == 0)
            check(stop);
          const skeletal::Vec3 v{scalar(g.positions, p * 3),
                                 scalar(g.positions, p * 3 + 1),
                                 scalar(g.positions, p * 3 + 2)};
          m.raw.push_back(v);
          m.points.push_back(transform.map(QVector3D(static_cast<float>(v.x),
                                                     static_cast<float>(v.y),
                                                     static_cast<float>(v.z))));
        }
        for (auto p : g.corner_points)
          m.corners.push_back(static_cast<std::uint32_t>(p));
        m.triangles = src.triangle_corners;
        for (std::size_t t = 0; t < m.triangles.size(); t += 3) {
          m.triangleFaces.push_back(
              g.render_mapping.at(m.triangles[t]).source_face);
          m.triangleCorners.push_back(
              {g.render_mapping.at(m.triangles[t]).source_corner,
               g.render_mapping.at(m.triangles[t + 1]).source_corner,
               g.render_mapping.at(m.triangles[t + 2]).source_corner});
        }
        for (const auto &map : g.render_mapping)
          m.faces.push_back(map.source_face);
        for (const auto &[name, b] : g.corner_attributes)
          if (name.starts_with("uv/")) {
            m.uvNames.append(text(name));
            auto &uv = m.uvs.emplace_back();
            uv.reserve(m.corners.size());
            for (std::size_t i = 0; i < m.corners.size(); ++i)
              uv.emplace_back(scalar(b, i * 2), scalar(b, i * 2 + 1));
          }
        const auto normal = g.corner_attributes.find("normal");
        for (std::size_t slot = 0; slot < sourceMaterials.size(); ++slot) {
          const auto &mat = sourceMaterials[slot];
          int set = mat.colour_uv.empty() ? 0 : -1;
          if (!mat.colour_uv.empty()) {
            for (int index = 0; index < m.uvNames.size(); ++index) {
              const auto &name = m.uvNames[index];
              if (name.mid(name.indexOf('/', 3) + 1) != text(mat.colour_uv))
                continue;
              if (set >= 0) {
                set = -1;
                break;
              }
              set = index;
            }
          }
          m.materialUv.push_back(std::max(static_cast<int>(set), 0));
          m.alphaModes.push_back(mat.opacity < 1 ? 2 : 0);
          m.alphaCutoffs.push_back(.5F);
          std::array<float, 6> uvTransform;
          std::transform(
              mat.colour_uv_transform.begin(), mat.colour_uv_transform.end(),
              uvTransform.begin(),
              [](double value) { return static_cast<float>(value); });
          m.uvAffine.push_back(uvTransform);
          if (set >= 0 && !m.uvs.empty())
            loadSourceTexture(mat, m, slot);
          else if (mat.colour_content || !mat.colour_texture.empty())
            d->warnings.append(
                m.name + ": base colour has no usable UV set; source image "
                         "reference retained without inventing coordinates");
          if (set < 0)
            d->warnings.append(m.name +
                               ": source material references absent UV set " +
                               text(mat.colour_uv));
        }
        const auto normalMatrix = transform.normalMatrix();
        for (std::size_t i = 0; i < m.corners.size(); ++i) {
          QVector3D n;
          if (normal != g.corner_attributes.end()) {
            const auto &b = normal->second;
            const QVector3D v(static_cast<float>(scalar(b, i * 3)),
                              static_cast<float>(scalar(b, i * 3 + 1)),
                              static_cast<float>(scalar(b, i * 3 + 2)));
            m.animationNormals.push_back(v);
            n = {normalMatrix(0, 0) * v.x() + normalMatrix(0, 1) * v.y() +
                     normalMatrix(0, 2) * v.z(),
                 normalMatrix(1, 0) * v.x() + normalMatrix(1, 1) * v.y() +
                     normalMatrix(1, 2) * v.z(),
                 normalMatrix(2, 0) * v.x() + normalMatrix(2, 1) * v.y() +
                     normalMatrix(2, 2) * v.z()};
          }
          m.normals.push_back(n.normalized());
          const auto face = m.faces[i];
          const auto material =
              src.face_materials.at(static_cast<std::size_t>(face));
          m.cornerMaterial.push_back(
              material < static_cast<std::uint32_t>(m.materials.size())
                  ? material
                  : 0);
          const auto colour = material < sourceMaterials.size()
                                  ? sourceMaterials[material].colour
                                  : std::array<double, 3>{.7, .7, .7};
          m.colours.emplace_back(static_cast<float>(colour[0]),
                                 static_cast<float>(colour[1]),
                                 static_cast<float>(colour[2]));
          m.alphas.push_back(
              material < sourceMaterials.size()
                  ? static_cast<float>(sourceMaterials[material].opacity)
                  : 1.F);
        }
        for (const auto &sourceSkin : src.skins) {
          Skin skin;
          skin.name = text(sourceSkin.source.original_name);
          skin.id = text(sourceSkin.source.identity.stable_id().value);
          skin.method =
              QString("FBX source skin method %1").arg(sourceSkin.method);
          if (skin.name.isEmpty())
            skin.name = QString("Skin %1").arg(m.skins.size());
          skin.weights.resize(m.points.size());
          for (const auto &cluster : sourceSkin.clusters) {
            const int bone = static_cast<int>(skin.bones.size());
            skin.bones.append(
                text(data.nodes.at(cluster.bone_node).source.original_name) +
                QString(" [node/%1]").arg(cluster.bone_node));
            skin.palette.append(QVariantMap{
                {"nodeId", QString("node/%1").arg(cluster.bone_node)},
                {"clusterId", text(cluster.source.identity.stable_id().value)},
                {"geometryToBone", modelMatrixValues(cluster.geometry_to_bone)},
                {"bindWorld", modelMatrixValues(cluster.bind_to_world)}});
            for (std::size_t i = 0; i < cluster.points.size(); ++i)
              skin.weights.at(cluster.points[i])
                  .push_back({bone, cluster.weights[i]});
          }
          m.skins.push_back(std::move(skin));
        }
        d->meshes.push_back(std::move(m));
      }
    }
    // External-drive skinning inputs: first verified binding per mesh as
    // per-point influences (node + weight + geometry-to-bone), averaged
    // smooth point normals from the corner normals, and the first instance
    // node for the rigid fallback. Derived read-only; nothing above changes.
    d->skinClusters.resize(d->meshes.size());
    d->skinSlots.resize(d->meshes.size());
    d->pointNormals.resize(d->meshes.size());
    d->meshInstanceNodes.assign(d->meshes.size(), 0);
    for (std::size_t mi = 0; mi < d->meshes.size(); ++mi) {
      auto &mesh = d->meshes[mi];
      d->meshInstanceNodes[mi] =
          mesh.nodeId.mid(5).toUInt(); // "node/N"
      auto &clusters = d->skinClusters[mi];
      auto &pointSlots = d->skinSlots[mi];
      pointSlots.assign(mesh.points.size(), {});
      if (!mesh.skins.empty()) {
        const auto &skin = mesh.skins.front();
        mesh.skinDeformationValid = validAnimationWeights(skin);
        // One cluster per palette bone (first skin binding): the cluster
        // matrix world[bone] * geometryToBone is computed once per frame.
        clusters.reserve(static_cast<std::size_t>(skin.palette.size()));
        QHash<int, std::uint32_t> boneToCluster;
        for (int bone = 0; bone < skin.palette.size(); ++bone) {
          const auto entry = skin.palette[bone].toMap();
          const auto nodeId = entry.value("nodeId").toString();
          const auto matrix = entry.value("geometryToBone").toList();
          if (nodeId.mid(5).toUInt() >= data.nodes.size() ||
              matrix.size() != 16)
            continue;
          ModelSkinCluster cluster;
          cluster.node = nodeId.mid(5).toUInt();
          for (int e = 0; e < 16; ++e)
            cluster.geometryToBone[static_cast<std::size_t>(e)] =
                matrix[e].toDouble();
          boneToCluster.insert(bone,
                               static_cast<std::uint32_t>(clusters.size()));
          clusters.push_back(std::move(cluster));
        }
        std::vector<bool> weighted(clusters.size(), false);
        for (std::size_t p = 0; p < mesh.points.size(); ++p)
          for (const auto &w : skin.weights[p]) {
            const auto cluster = boneToCluster.value(w.bone,
                                                     std::numeric_limits<
                                                         std::uint32_t>::max());
            if (cluster != std::numeric_limits<std::uint32_t>::max()) {
              pointSlots[p].push_back({cluster, w.value});
              if (w.value > 0)
                weighted[cluster] = true;
            }
          }
        for (std::size_t c = 0; c < clusters.size(); ++c) {
          if (!weighted[c])
            continue;
          auto node = clusters[c].node;
          auto root = node; // Explicit cluster node if no bone flags exist.
          for (std::size_t depth = 0; depth < data.nodes.size(); ++depth) {
            if (data.nodes[node].bone)
              root = node;
            if (!data.nodes[node].parent)
              break;
            node = *data.nodes[node].parent;
          }
          if (std::find(d->animRootNodes.begin(), d->animRootNodes.end(),
                        static_cast<std::int32_t>(root)) == d->animRootNodes.end())
            d->animRootNodes.push_back(static_cast<std::int32_t>(root));
        }
      }
      auto &normals = d->pointNormals[mi];
      normals.assign(mesh.points.size(), skeletal::Vec3{});
      std::vector<bool> hasNormal(mesh.points.size(), false);
      if (mesh.normals.size() == mesh.corners.size()) {
        for (std::size_t c = 0; c < mesh.corners.size(); ++c) {
          const auto point = mesh.corners[c];
          if (point >= normals.size())
            continue;
          const auto &n = mesh.animationNormals.empty() ? mesh.normals[c] : mesh.animationNormals[c];
          normals[point].x += n.x();
          normals[point].y += n.y();
          normals[point].z += n.z();
          hasNormal[point] = true;
        }
        for (std::size_t p = 0; p < normals.size(); ++p)
          if (hasNormal[p]) {
            const auto length = std::sqrt(
                normals[p].x * normals[p].x + normals[p].y * normals[p].y +
                normals[p].z * normals[p].z);
            if (length > 1e-12) {
              normals[p].x /= length;
              normals[p].y /= length;
              normals[p].z /= length;
            }
          }
      }
    }
    if (suffix == "fbx" && !d->meshes.empty() && !data.clips.empty()) {
      d->nativeAnimated = true;
      d->nativeFbxAnimated = true;
      for (const auto &clip : data.clips) {
        ModelAnimationClip entry;
        entry.name = text(clip.source.original_name);
        entry.start = clip.start;
        entry.end = clip.end;
        entry.fps = clip.fps > 0 ? clip.fps : 30.0;
        entry.lastFrame = clip.fps > 0
                              ? static_cast<int>((clip.end - clip.start) * clip.fps)
                              : 0;
        d->animationClips.push_back(entry);
        d->clips.push_back(QVariantMap{{"name", entry.name}, {"start", entry.start},
                                     {"end", entry.end}, {"fps", entry.fps},
                                     {"lastFrame", entry.lastFrame}});
      }
      d->localAt = [holder](std::size_t clip, double seconds) {
        return holder->evaluate(clip, seconds).local;
      };
      d->worldAt = [holder](std::size_t clip, double seconds) {
        return holder->evaluate(clip, seconds).world;
      };
      d->nativeFbxLocalAt = d->localAt;
      d->nativeFbxWorldAt = d->worldAt;
    }
    for (const auto &textureLease : textureLeases)
      textureLease.require_unchanged(textureLease.snapshot());
  } else if (suffix == "psa") {
    // Without an explicit reference this is an unverified raw-record preview.
    const auto asset = std::make_shared<skeletal::ActorAsset>(
        skeletal::read_actor(source, options));
    if (!asset->animation || asset->sequences.empty())
      throw skeletal::Failure(ErrorCode::InvalidArgument,
                              "PSA has no animation sequences");
    d->skeletonOnly = true;
    d->hash = text(asset->source.identity.sha256);
    d->name = QFileInfo(path).completeBaseName();
    d->coordinates = QStringLiteral("ActorX PSA · skeleton preview · Z up");
    d->displayBasis.rotate(-90, 1, 0, 0);
    for (const auto &warning : asset->warnings)
      d->warnings.push_back(text(warning));
    std::shared_ptr<skeletal::Pairing> binding;
    std::shared_ptr<skeletal::PsaConfig> config;
    const auto rejectReference = [&d](const QString &reason,
                                      bool companionPresent) {
      // A reference that does not fit THIS file (missing, changed while
      // decoding, or incompatible names/counts) never silently applies a
      // wrong skeleton: the document previews unpaired with an explicit,
      // file-specific warning and keeps the manual entry points.
      d->referenceRejected = true;
      d->referenceRejectReason = reason;
      d->pairSource.reset();
      d->pairBoneMap.clear();
      d->warnings.push_back(
          QStringLiteral("Reference skeleton NOT applied for this file: %1 "
                         "- unpaired raw-record preview, hierarchy unverified")
              .arg(reason));
      if (companionPresent)
        d->warnings.push_back(
            QStringLiteral("Track config NOT applied: it depends on the "
                           "reference skeleton"));
    };
    if (!reference.isEmpty()) {
      try {
        const std::filesystem::path refPath(reference.toStdWString());
        AccessScope refScope;
        refScope.input_roots = {refPath.parent_path()};
        const auto refLease = InputLease::open(refScope, refPath,
                                              512ULL * 1024 * 1024, stop);
        const auto mesh = skeletal::read_actor(refPath, options);
        binding = std::make_shared<skeletal::Pairing>(skeletal::pair(
            *asset, mesh, 1e-4, skeletal::PairingPolicy::ReferenceSkeleton));
        if (!binding->compatible) {
          QStringList reasons;
          for (const auto &item : binding->reasons)
            reasons << text(item);
          throw skeletal::Failure(
              ErrorCode::InvalidArgument,
              ("Reference PSK does not match this PSA" +
               (reasons.isEmpty() ? QString{}
                                  : QStringLiteral(": ") + reasons.join(", ")))
                  .toStdString());
        }
        refLease.require_unchanged(refLease.snapshot());
        if (mesh.source.identity.sha256 != refLease.snapshot().content_sha256)
          throw skeletal::Failure(ErrorCode::InputChanged,
                                  "Reference PSK changed while decoding");
        d->referencePath = reference;
        // Keep the PSK for the optional skinned-model display and map its
        // bone indices to animation indices (evaluate_paired worlds are in
        // animation order; skinning expects mesh order).
        d->pairBoneMap.assign(mesh.bones.size(), 0);
        for (std::size_t j = 0; j < binding->animation_to_mesh.size(); ++j) {
          const auto meshBone = binding->animation_to_mesh[j];
          if (meshBone < d->pairBoneMap.size())
            d->pairBoneMap[meshBone] = static_cast<std::uint32_t>(j);
        }
        d->pairSource =
            std::make_shared<const skeletal::ActorAsset>(std::move(mesh));
        for (const auto &warning : binding->warnings)
          d->warnings.push_back(text(warning));
        if (!companion.isEmpty()) {
          try {
            const std::filesystem::path configPath(companion.toStdWString());
            AccessScope configScope;
            configScope.input_roots = {configPath.parent_path()};
            const auto configLease = InputLease::open(configScope, configPath,
                                                     16ULL * 1024 * 1024, stop);
            QFile input(companion);
            if (!input.open(QIODevice::ReadOnly))
              throw skeletal::Failure(ErrorCode::Io,
                                      "Cannot open PSA companion");
            const auto bytes = input.readAll();
            config = std::make_shared<skeletal::PsaConfig>(
                skeletal::read_psa_config(
                    std::span<const std::byte>(
                        reinterpret_cast<const std::byte *>(bytes.constData()),
                        static_cast<std::size_t>(bytes.size())),
                    *asset, options));
            configLease.require_unchanged(configLease.snapshot());
            if (config->source.identity.sha256 !=
                configLease.snapshot().content_sha256)
              throw skeletal::Failure(ErrorCode::InputChanged,
                                      "PSA companion changed while decoding");
            d->companionPath = companion;
          } catch (const std::exception &e) {
            // An inapplicable config only disables the config; the verified
            // pairing itself stays applied.
            config.reset();
            d->companionPath.clear();
            d->warnings.push_back(
                QStringLiteral("Track config NOT applied: %1")
                    .arg(QString::fromUtf8(e.what())));
          }
        }
      } catch (const std::exception &e) {
        rejectReference(QString::fromUtf8(e.what()), !companion.isEmpty());
        binding.reset();
        config.reset();
      }
    } else if (!companion.isEmpty()) {
      // A discovered config cannot apply without a reference: removed-track
      // poses come from the PSK. Distinct from a broken file: the PSA itself
      // stays a valid unpaired preview.
      d->warnings.push_back(
          QStringLiteral("Track config discovered but NOT applied: choose a "
                         "reference PSK first"));
    }
    for (std::size_t i = 0; i < asset->bones.size(); ++i) {
      d->boneNames.push_back(text(asset->bones[i].source.original_name));
      d->boneParents.push_back(
          (binding ? binding->parents[i] : asset->bones[i].parent)
              ? static_cast<std::int32_t>(*(binding ? binding->parents[i] : asset->bones[i].parent))
              : std::int32_t{-1});
    }
    for (std::size_t c = 0; c < asset->sequences.size(); ++c) {
      const auto &sequence = asset->sequences[c];
      ModelAnimationClip entry;
      entry.name = text(sequence.source.original_name);
      entry.start = sequence.time.source_start_seconds;
      entry.end = sequence.time.source_end_seconds;
      entry.fps = sequence.fps > 0 ? sequence.fps : 30.0;
      entry.lastFrame = sequence.frame_count > 0
                            ? sequence.frame_count - 1
                            : 0;
      d->animationClips.push_back(entry);
      d->clips.push_back(QVariantMap{
          {"name", entry.name},
          {"start", entry.start},
          {"end", entry.end},
          {"fps", entry.fps},
          {"lastFrame", entry.lastFrame}});
    }
    d->worldAt = [asset, binding, config](std::size_t clip, double seconds) {
      return binding ? skeletal::evaluate_paired(*asset, *binding, clip, seconds, config.get()).world
                     : skeletal::evaluate(*asset, clip, seconds).world;
    };
    d->localAt = [asset, binding, config](std::size_t clip, double seconds) {
      const auto pose = binding
                            ? skeletal::evaluate_paired(*asset, *binding, clip,
                                                        seconds, config.get())
                            : skeletal::evaluate(*asset, clip, seconds);
      std::vector<formaview::Matrix4> locals(pose.local.size());
      for (std::size_t i = 0; i < pose.local.size(); ++i)
        locals[i] = skeletal::matrix(pose.local[i]);
      return locals;
    };
    if (binding) {
      d->referenceLocals.resize(binding->reference_local.size());
      for (std::size_t i = 0; i < binding->reference_local.size(); ++i)
        d->referenceLocals[i] = skeletal::matrix(binding->reference_local[i]);
    }
    initializeAnimationBounds(*d);
    // Skeleton-only document: skip the mesh-oriented tail (triangles check)
    // after the same input-identity verification.
    check(stop);
    lease.require_unchanged(lease.snapshot());
    if (d->hash != text(lease.snapshot().content_sha256))
      throw skeletal::Failure(ErrorCode::InputChanged,
                              "Model input changed during decoding");
    return d;
  } else if (suffix == "psk" || suffix == "pskx") {
    d->pairSource = std::make_shared<const skeletal::ActorAsset>(
        skeletal::read_actor(source, options));
    const auto &file = *d->pairSource;
    if (!file.mesh)
      throw skeletal::Failure(ErrorCode::InvalidArgument,
                              "ActorX file has no mesh");
    const auto &src = *file.mesh;
    Mesh m;
    m.id = "actor/mesh";
    m.nodeId = "actor/mesh";
    m.name = QFileInfo(path).completeBaseName();
    d->hash = text(file.source.identity.sha256);
    d->coordinates = "ActorX · source axes/units unknown · display uses Z up";
    d->displayBasis.rotate(-90, 1, 0, 0);
    m.raw = src.points;
    for (const auto &p : src.points)
      m.points.emplace_back(static_cast<float>(p.x), static_cast<float>(p.y),
                            static_cast<float>(p.z));
    m.uvNames.append("UV0");
    m.uvs.emplace_back();
    for (const auto &w : src.wedges) {
      m.corners.push_back(w.point);
      m.uvs[0].emplace_back(w.uv.x, w.uv.y);
    }
    for (const auto &[name, values] : src.extra_uvs) {
      m.uvNames.append(text(name));
      auto &uv = m.uvs.emplace_back();
      for (const auto &v : values)
        uv.emplace_back(v.x, v.y);
    }
    for (std::size_t f = 0; f < src.faces.size(); ++f)
      for (auto c : src.faces[f].wedges)
        m.triangles.push_back(c);
    m.faces.resize(m.corners.size());
    m.cornerMaterial.resize(m.corners.size());
    for (const auto &mat : src.materials) {
      m.materials.append(text(mat.name));
      m.references.append("");
    }
    if (m.materials.isEmpty()) {
      m.materials.append("Default");
      m.references.append("");
    }
    for (std::size_t i = 0; i < src.wedges.size(); ++i)
      m.cornerMaterial[i] = src.wedges[i].material;
    m.textures.resize(static_cast<std::size_t>(m.materials.size()));
    m.texturePaths = QStringList(m.materials.size(), QString{});
    m.normals.resize(m.corners.size());
    m.colours.resize(m.corners.size(), QVector3D(.72F, .72F, .75F));
    Skin skin;
    skin.name = "ActorX source binding";
    skin.id = d->hash + ":actor:source-binding";
    skin.method = "ActorX source records";
    skin.weights.resize(m.points.size());
    for (std::size_t i = 0; i < file.bones.size(); ++i) {
      const auto &bone = file.bones[i];
      skin.bones.append(text(bone.source.original_name) +
                        QString(" [bone/%1]").arg(i));
      d->nodes.append(QVariantMap{
          {"id", QString("bone/%1").arg(i)},
          {"stableId", text(bone.source.identity.stable_id().value)},
          {"name", text(bone.source.original_name)},
          {"parent",
           bone.parent ? QString("bone/%1").arg(*bone.parent) : QString{}},
          {"bone", true},
          {"rawTranslation",
           QVariantList{bone.raw_translation.x, bone.raw_translation.y,
                        bone.raw_translation.z}},
          {"rawRotation",
           QVariantList{bone.raw_rotation.x, bone.raw_rotation.y,
                        bone.raw_rotation.z, bone.raw_rotation.w}},
          {"local",
           bone.default_local_valid
               ? modelMatrixValues(skeletal::matrix(bone.default_local))
               : QVariantList{}},
          {"active", true}});
      QVariantMap palette{{"nodeId", QString("bone/%1").arg(i)}};
      if (bone.bind_world)
        palette["bindWorld"] = modelMatrixValues(*bone.bind_world);
      skin.palette.append(palette);
    }
    for (const auto &w : src.weights)
      skin.weights.at(w.point).push_back({static_cast<int>(w.bone), w.value});
    if (!skin.bones.isEmpty())
      m.skins.push_back(std::move(skin));
    d->meshes.push_back(std::move(m));
  } else if (suffix == "gltf" || suffix == "glb") {
#ifdef FORMAVIEW_GLTF
    d = readGltfModel(path, stop);
#else
    throw skeletal::Failure(ErrorCode::Unsupported,
                            "glTF adapter unavailable in this build");
#endif
    // Native glTF animation skinning inputs, mirroring the external-drive
    // FBX tail: first verified binding per mesh as per-point influences
    // (joint node + inverse bind matrix), smooth point normals, and the
    // weighted cluster ancestor chain for root candidates. Derived
    // read-only; the document's own bind data is never overwritten.
    if (d->nativeAnimated && !d->meshes.empty()) {
      d->skinClusters.resize(d->meshes.size());
      d->skinSlots.resize(d->meshes.size());
      d->pointNormals.resize(d->meshes.size());
      d->meshInstanceNodes.assign(d->meshes.size(), 0);
      const auto boneFlag = [&](std::size_t node) {
        return node < static_cast<std::size_t>(d->nodes.size()) &&
               d->nodes.at(static_cast<int>(node))
                   .toMap()
                   .value("bone")
                   .toBool();
      };
      const auto nodeCount = d->animNodeParents.size();
      for (std::size_t mi = 0; mi < d->meshes.size(); ++mi) {
        auto &mesh = d->meshes[mi];
        d->meshInstanceNodes[mi] = mesh.nodeId.mid(5).toUInt(); // "node/N"
        auto &clusters = d->skinClusters[mi];
        auto &pointSlots = d->skinSlots[mi];
        pointSlots.assign(mesh.points.size(), {});
        if (!mesh.skins.empty()) {
          const auto &skin = mesh.skins.front();
        mesh.skinDeformationValid = validAnimationWeights(skin);
          clusters.reserve(static_cast<std::size_t>(skin.palette.size()));
          QHash<int, std::uint32_t> boneToCluster;
          for (int bone = 0; bone < skin.palette.size(); ++bone) {
            const auto entry = skin.palette[bone].toMap();
            const auto nodeId = entry.value("nodeId").toString();
            const auto inverse = entry.value("inverseBind").toList();
            if (nodeId.mid(5).toUInt() >= nodeCount || inverse.size() != 16)
              continue;
            ModelSkinCluster cluster;
            cluster.node = nodeId.mid(5).toUInt();
            for (int e = 0; e < 16; ++e)
              cluster.geometryToBone[static_cast<std::size_t>(e)] =
                  inverse[e].toDouble();
            boneToCluster.insert(bone,
                                 static_cast<std::uint32_t>(clusters.size()));
            clusters.push_back(std::move(cluster));
          }
          std::vector<bool> weighted(clusters.size(), false);
          for (std::size_t p = 0; p < mesh.points.size(); ++p)
            for (const auto &w : skin.weights[p]) {
              const auto cluster = boneToCluster.value(
                  w.bone, std::numeric_limits<std::uint32_t>::max());
              if (cluster != std::numeric_limits<std::uint32_t>::max()) {
                pointSlots[p].push_back({cluster, w.value});
                if (w.value > 0)
                  weighted[cluster] = true;
              }
            }
          for (std::size_t c = 0; c < clusters.size(); ++c) {
            if (!weighted[c])
              continue;
            auto node = clusters[c].node;
            auto root = node; // Explicit cluster node if no bone flags exist.
            for (std::size_t depth = 0; depth < nodeCount; ++depth) {
              if (boneFlag(node))
                root = node;
              const auto parent = d->animNodeParents[node];
              if (parent < 0 ||
                  static_cast<std::size_t>(parent) >= nodeCount)
                break;
              node = static_cast<std::size_t>(parent);
            }
            if (std::find(d->animRootNodes.begin(), d->animRootNodes.end(),
                          static_cast<std::int32_t>(root)) ==
                d->animRootNodes.end())
              d->animRootNodes.push_back(static_cast<std::int32_t>(root));
          }
        }
        // glTF normals are per-vertex: use them directly as smooth point
        // normals (the FBX tail averages corner normals instead).
        auto &normals = d->pointNormals[mi];
        normals.assign(mesh.points.size(), skeletal::Vec3{});
        // Animated-morph meshes animate from the UNMORPHED base normals; the
        // statically morphed normals serve meshes without animated targets.
        if (mesh.morphAnimated &&
            mesh.morphBaseNormals.size() == mesh.points.size())
          for (std::size_t p = 0; p < normals.size(); ++p)
            normals[p] = mesh.morphBaseNormals[p];
        else if (mesh.animationNormals.size() == mesh.points.size())
          for (std::size_t p = 0; p < normals.size(); ++p)
            normals[p] = {mesh.animationNormals[p].x(),
                          mesh.animationNormals[p].y(),
                          mesh.animationNormals[p].z()};
      }
    }
  } else
    throw skeletal::Failure(ErrorCode::Unsupported,
                            "This model format is not connected yet");
  d->name = QFileInfo(path).fileName();
  finalizeModelGeometry(*d, stop);
  check(stop);
  lease.require_unchanged(lease.snapshot());
  if (d->hash != text(lease.snapshot().content_sha256))
    throw skeletal::Failure(ErrorCode::InputChanged,
                            "Model input changed during decoding");
  return d;
}
ModelSession::ModelSession(QObject *parent)
    : QObject(parent), scene_(this), skeleton_(this) {
  timer_.setInterval(20);
  timer_.setObjectName("modelSessionPoll");
  connect(&timer_, &QTimer::timeout, this, &ModelSession::poll);
  timer_.start();
  // Playback drives inline pose evaluation on the GUI thread; the closed
  // loop measurement showed sub-millisecond evaluation for real samples,
  // while the 20 ms poll path cannot sustain 60 Hz publication.
  animTimer_.setInterval(16);
  animTimer_.setObjectName("modelSessionAnimation");
  connect(&animTimer_, &QTimer::timeout, this, &ModelSession::tickAnimation);
}
ModelSession::~ModelSession() {
  if (abcJob_) abcJob_->stop.request_stop();
  if (pickJob_) pickJob_->stop.request_stop();
  if (gpuJob_) gpuJob_->stop.request_stop();
  if (asyncFrame_) asyncFrame_->stop.request_stop();
  for (auto &j : externalJobs_) j.stop.request_stop();
  for (auto &j : compareJobs_) j.stop.request_stop();
  for (auto &j : frameJobs_)
    j.stop.request_stop();
  for (auto &j : jobs_)
    j.stop.request_stop();
  for (auto &j : textureJobs_)
    j.stop.request_stop();
}
void ModelSession::clearSelection() {
  picked_.clear();
  influences_.clear();
  emit selectionChanged();
}
void ModelSession::clear() {
  reset(false);
}
void ModelSession::reset(bool opening) {
  mode_ = 0;
  ++generation_;
  ++frameRequest_;
  ++poseRequest_;
  pendingAnimationFrame_ = false;
  // The old archive's sample worker keeps running until poll() collects it;
  // its generation is stale, so it is neither cached nor presented.
  if (abcJob_) abcJob_->stop.request_stop();
  abcSource_.reset();
  abcCache_.clear();
  abcFrameDocument_.reset();
  abcRequested_ = 0;
  abcDirection_ = 1;
  abcError_.clear();
  // A std::async future blocks in its destructor. Keep the single cancelled
  // worker until poll() collects it; its old generation can never publish.
  if (pickJob_) pickJob_->stop.request_stop();
  if (gpuJob_) gpuJob_->stop.request_stop();
  if (asyncFrame_) asyncFrame_->stop.request_stop();
  for (auto &j : externalJobs_) j.stop.request_stop();
  externalQueue_.clear();
  externalRequested_ = externalApplied_ = 0;
  externalFailed_.clear();
  for (auto &j : compareJobs_) j.stop.request_stop();
  compareResult_.clear();
  compareError_.clear();
  animTimer_.stop();
  playing_ = false;
  time_ = 0;
  clipIndex_ = 0;
  renderedState_ = {};
  frameCache_.clear();
  cachedBytes_ = 0;
  globalFrameFailure_ = false;
  pendingFrame_ = preparing_ = false;
  frameError_.clear();
  frames_ = {};
  gpuGeometry_.reset(); gpuKey_.clear(); gpuDisabled_=false;
  gpuViewOwner_.reset(); gpuHighlightOwner_.reset();
  pendingPick_.reset(); ++pickRequest_;
  textureLinks_.clear();
  classes_.clear();
  levels_.clear();
  lodGroup_ = collisionMode_ = 0;
  materialOnly_ = false;
  for (auto &j : frameJobs_)
    j.stop.request_stop();
  pending_.clear();
  ++textureRequest_;
  textureBusy_ = false;
  textureError_.clear();
  for (auto &j : textureJobs_)
    j.stop.request_stop();
  for (auto &j : jobs_)
    j.stop.request_stop();
  document_.reset();
  scene_.replace(nullptr);
  skeleton_.replace(nullptr, 0, -1);
  skeletonNode_.clear();
  highlightedNode_.clear();
  skeletonJoint_ = -1;
  frame_.reset();
  // Opening is one transaction. Publishing an intermediate idle/empty state
  // destroys the preview Loader and recreates its entire UI on every file.
  busy_ = opening;
  path_.clear();
  referencePath_.clear();
  companionPath_.clear();
  selectedNode_.clear();
  error_.clear();
  clearSelection();
  emit animChanged();
  emit frameChanged();
  emit changed();
  emit documentChanged();
}
void ModelSession::cancel() {
  if (abcActive() &&
      ((abcJob_ && !abcJob_->prefetch) ||
       abcRequested_ != document_->cacheSample ||
       (abcFrameDocument_ && abcFrameDocument_ != document_))) {
    // Cancelling a sample read keeps the open archive and its presented
    // sample; only an idle cancel closes the document as before.
    cancelSample();
    if (!preparing_)
      return;
  }
  if ((!preparing_ && frameError_.isEmpty()) || !frames_.model) {
    clear();
    return;
  }
  ++frameRequest_;
  ++poseRequest_;
  if (asyncFrame_) asyncFrame_->stop.request_stop();
  pendingAnimationFrame_ = false;
  if(gpuJob_){gpuJob_->stop.request_stop();gpuJob_->normKey.clear();gpuJob_->fullKey.clear();++poseRequest_;}
  for (auto &job : frameJobs_)
    job.stop.request_stop();
  pendingFrame_ = preparing_ = false;
  frameError_.clear();
  globalFrameFailure_ = false;
  mode_ = renderedState_.mode;
  mesh_ = renderedState_.mesh;
  bone_ = renderedState_.bone;
  material_ = renderedState_.material;
  materialOnly_ = renderedState_.materialOnly;
  isolated_ = renderedState_.isolated;
  bindingSelections_ = renderedState_.bindings;
  highlightedNode_ = renderedState_.highlightedNode;
  uvSelections_ = renderedState_.uvs;
  textureLinks_ = renderedState_.textures;
  classes_ = renderedState_.classes;
  levels_ = renderedState_.levels;
  collisionMode_ = renderedState_.collisionMode;
  selectedNode_ =
      document_->meshes.empty()
          ? QString{}
          : document_->meshes.at(static_cast<std::size_t>(mesh_)).nodeId;
  updateSkeleton(false);
  clearSelection();
  emit changed();
  emit frameChanged();
}
void ModelSession::retry() {
  if (document_ && !frameError_.isEmpty()) {
    globalFrameFailure_ = false;
    rebuild();
    emit changed();
    return;
  }
  const auto path = path_;
  if (!path.isEmpty())
    openInputs(path, referencePath_, companionPath_);
}
QString ModelSession::companionCandidate(const QString &psa) const {
  // Deterministic per-file match only: same directory, same base name.
  // Never a directory-wide or recursive search, never "the first config".
  const auto info = QFileInfo(psa);
  const auto candidate =
      info.dir().filePath(info.completeBaseName() + ".config");
  return QFileInfo::exists(candidate) ? candidate : QString{};
}

void ModelSession::open(const QString &p) {
  // Session reference memory: every PSA open in this process retries the
  // user's remembered PSK choice; each file re-verifies compatibility and
  // rejects with a warning instead of silently reusing a wrong skeleton.
  // The companion is discovered per file and never inherited.
  QString companion;
  if (QFileInfo(p).suffix().compare("psa", Qt::CaseInsensitive) == 0)
    companion = companionCandidate(p);
  openInputs(p, sessionReferencePath_, companion);
}
void ModelSession::setAnimationReference(const QString &input) {
  const auto reference =
      QUrl(input).isLocalFile() ? QUrl(input).toLocalFile() : input;
  // An explicit choice replaces the session memory; an explicit clear also
  // forgets it, so a cleared pairing is not immediately re-applied by the
  // memory on the next open.
  sessionReferencePath_ = reference;
  if (QFileInfo(path_).suffix().compare("psa", Qt::CaseInsensitive) == 0 &&
      !busy_ && !path_.isEmpty())
    openInputs(path_, reference,
               reference.isEmpty() ? QString{}
                                   : companionCandidate(path_));
}
void ModelSession::setAnimationCompanion(const QString &companion) {
  if (QFileInfo(path_).suffix().compare("psa", Qt::CaseInsensitive) == 0 &&
      !referencePath_.isEmpty() && !busy_)
    openInputs(path_, referencePath_, QUrl(companion).isLocalFile() ? QUrl(companion).toLocalFile() : companion);
}
void ModelSession::openInputs(QString p, QString reference, QString companion) {
  if (p.isEmpty()) {
    clear();
    return;
  }
  // An open/repair transaction while playing resumes playback on the new
  // document landing; an explicit clear/close does not.
  resumePlaybackOnLanding_ = playing_;
  reset(true);
  path_ = p;
  referencePath_ = reference;
  companionPath_ = companion;
  pending_ = p;
  busy_ = true;
  startPending();
  emit changed();
}
void ModelSession::startPending() {
  if (pending_.isEmpty() || jobs_.size() >= 2)
    return;
  const auto p = std::exchange(pending_, {});
  const auto reference = referencePath_, companion = companionPath_;
  std::stop_source stop;
  const auto token = stop.get_token();
  auto future = std::async(std::launch::async, [p, reference, companion, token] {
    Result r;
    try {
      auto document = readModelDocument(p, token, reference, companion);
      check(token);
      const int mesh=document ? initialMeshFor(*document) : 0;
      const int binding=document && !document->meshes.empty() &&
        document->meshes[static_cast<std::size_t>(mesh)].skins.size()==1 ? 0 : -1;
      auto scene=ModelScene::prepare(document.get());
      check(token);
      auto skeleton=ModelSkeleton::prepare(document.get(),mesh,binding);
      check(token);
      // No live QObject/model crosses threads. Publish the detached trees only
      // after all preparation succeeds; cancellation destroys them here.
      r.document=std::move(document);
      r.scene=std::move(scene);
      r.skeleton=std::move(skeleton);
      r.initialMesh=mesh;
    } catch (const std::exception &e) {
      r.error = QString::fromUtf8(e.what());
    }
    return r;
  });
  jobs_.push_back({generation_, std::move(stop), std::move(future)});
}
void ModelSession::poll() {
  finishAbcJob();
  finishGpuPick();
  finishGpuGeometry();
  finishAsyncFrame();
  for (auto it = frameJobs_.begin(); it != frameJobs_.end();) {
    if (it->future.wait_for(std::chrono::seconds(0)) !=
        std::future_status::ready) {
      ++it;
      continue;
    }
    auto r = it->future.get();
    if (it->generation == generation_ && it->request == frameRequest_ &&
        document_) {
      preparing_ = false;
      frameError_ = r.error;
      globalFrameFailure_ =
          !r.error.isEmpty() && (levels_ != renderedState_.levels ||
                                 classes_ != renderedState_.classes);
      if (r.error.isEmpty()) {
        frames_ = std::move(r.frames);
        renderedState_ = std::move(r.state);
        frame_ = frames_.mode == 1 ? frames_.uv : frames_.model;
        cacheFrame();
        if (abcActive())
          abcFrameDocument_ = document_;
      }
      emit frameChanged();
      emit changed();
    }
    it = frameJobs_.erase(it);
  }
  for (auto it = textureJobs_.begin(); it != textureJobs_.end();) {
    if (it->future.wait_for(std::chrono::seconds(0)) !=
        std::future_status::ready) {
      ++it;
      continue;
    }
    auto r = it->future.get();
    if (it->generation == generation_ && it->request == textureRequest_ &&
        document_) {
      textureBusy_ = false;
      textureError_ = r.error;
      if (r.pixels) {
        textureLinks_.at(static_cast<std::size_t>(it->mesh))[it->material] = {
            std::move(r.pixels), r.path, it->uv};
        rebuild();
      }
      emit changed();
    }
    it = textureJobs_.erase(it);
  }
  for (auto it = jobs_.begin(); it != jobs_.end();) {
    if (it->future.wait_for(std::chrono::seconds(0)) !=
        std::future_status::ready) {
      ++it;
      continue;
    }
    auto r = it->future.get();
    if (it->generation == generation_) {
      busy_ = false;
      error_ = r.error;
      document_ = std::move(r.document);
      scene_.replace(std::move(r.scene));
      mesh_=r.initialMesh;
      bone_ = mode_ = 0;
      material_ = 0;
      isolated_ = false;
      bindingSelections_.clear();
      uvSelections_.clear();
      textureLinks_.clear();
      classes_.clear();
      levels_.clear();
      clipIndex_ = 0;
      playing_ = false;
      animTimer_.stop();
      if (document_ && !document_->meshes.empty())
        for (const auto &m : document_->meshes) {
          classes_.push_back(m.classification);
          if (!m.classification.group.isEmpty()) {
            const auto [entry, inserted] = levels_.try_emplace(
                m.classification.group, m.classification.level);
            if (!inserted)
              entry->second = std::min(entry->second, m.classification.level);
          }
          textureLinks_.emplace_back();
          bindingSelections_.push_back(m.skins.size() == 1 ? 0 : -1);
          uvSelections_.push_back(0);
        }
      if (document_ && !document_->meshes.empty())
        selectedNode_ =
            document_->meshes.empty()
                ? QString{}
                : document_->meshes.at(static_cast<std::size_t>(mesh_)).nodeId;
      updateSkeleton(true,&r.skeleton);
      rootIndex_ = -1; // Resolve and cache against the newly landed document.
      rootBaseClip_ = -1;
      rootBaseLocals_.clear();
      if (rootBase_ == 1 &&
          (!document_ || document_->referenceLocals.empty()))
        rootBase_ = 0;
      if (document_ && document_->animated()) {
        time_ = document_->animationClips.front().start;
        if (resumePlaybackOnLanding_) {
          playing_ = true;
          animClock_.restart();
          animTimer_.start();
        }
        refreshAnimationFrame();
      } else if (document_ && document_->geometryCache) {
        abcSource_ = document_->geometryCache;
        abcCache_.clear();
        abcCache_.insert(document_);
        abcRequested_ = document_->cacheSample;
        abcDisplay_ = {document_->center, document_->radius};
        abcPrefetched_ = abcDiscarded_ = 0;
        time_ = abcSource_->times().at(abcRequested_);
        if (resumePlaybackOnLanding_ && abcSource_->times().size() > 1) {
          playing_ = true;
          animClock_.restart();
          animTimer_.start();
        }
      } else {
        time_ = 0;
      }
      resumePlaybackOnLanding_ = false;
      rebuild();
      emit animChanged();
      emit changed();
      emit documentChanged();
    }
    it = jobs_.erase(it);
  }
  bool externalJobsFinished = false;
  // Apply in selection order: a later file waits for every earlier one, so
  // clip order and the final status do not depend on read completion order.
  while (!externalJobs_.empty() &&
         externalJobs_.front().future.wait_for(std::chrono::seconds(0)) ==
             std::future_status::ready) {
    const auto it = externalJobs_.begin();
    auto r = it->future.get();
    if (it->generation == generation_ && document_ && !busy_) {
      const auto sourcesBefore = document_->externalHolders.size() +
                                 document_->externalActorSources.size();
      if (r.holder)
        applyExternalDrive(std::move(r.holder), r.path);
      else if (r.actor)
        applyExternalActor(std::move(r.actor), r.path);
      else {
        // The external source could not be read: surface a file-specific
        // warning without disturbing the target preview.
        document_->externalStatus = QStringLiteral("refused");
        document_->externalNotice =
            QStringLiteral("External animation NOT applied (%1): %2").arg(r.path, r.error);
        emit animChanged();
        emit changed();
      }
      if (document_->externalHolders.size() +
              document_->externalActorSources.size() > sourcesBefore) {
        ++externalApplied_;
        externalAppliedStatus_ = document_->externalStatus;
        externalAppliedNotice_ = document_->externalNotice;
      } else {
        externalFailed_.append(QFileInfo(r.path).fileName());
      }
    }
    externalJobs_.erase(it);
    externalJobsFinished = true;
  }
  while (!externalQueue_.empty() && externalJobs_.size() < 8) {
    const auto path = externalQueue_.front();
    externalQueue_.pop_front();
    startExternalJob(path);
  }
  // A settled batch with refusals keeps the drive's own status and names
  // every file that was not applied, not only the last one.
  if (externalJobsFinished && !externalBusy() && document_ &&
      !externalFailed_.isEmpty() && externalApplied_ > 0) {
    document_->externalStatus = externalAppliedStatus_;
    document_->externalNotice =
        externalAppliedNotice_ +
        QStringLiteral(" · %1 of %2 file(s) NOT applied: %3")
            .arg(externalFailed_.size())
            .arg(externalRequested_)
            .arg(externalFailed_.join(QStringLiteral(", ")));
    emit animChanged();
  }
  // Busy bindings must be notified after erasing completed jobs, when the
  // getter can observe the new count (applyExternalDrive notifies earlier).
  if (externalJobsFinished)
    emit changed();
  bool compareJobsFinished=false;
  for (auto it = compareJobs_.begin(); it != compareJobs_.end();) {
    if (it->future.wait_for(std::chrono::seconds(0)) !=
        std::future_status::ready) {
      ++it;
      continue;
    }
    auto r = it->future.get();
    if (it->generation == generation_ && document_) {
      compareError_ = r.error;
      compareResult_ = {};
      if (r.error.isEmpty()) {
        // Build the target refs and run the audited comparison engine on
        // the GUI thread (name/chain matching is sub-millisecond).
        const auto guiBegin = std::chrono::steady_clock::now();
        auto targetInput = modelCompareInput(*document_);
        skeletal::CompareInput externalInput;
        externalInput.source = "external";
        for (const auto &value : r.report.value("bones").toList()) {
          const auto entry = value.toMap();
          const auto parent = entry.value("parent").toInt();
          externalInput.bones.push_back({entry.value("index").toUInt(),
              entry.value("name").toString().toStdString(),
              parent >= 0 ? std::optional<std::uint32_t>(parent) : std::nullopt,
              entry.value("hasDefaultTransform").toBool()});
        }
        externalInput = skeletal::named_compare_input(std::move(externalInput));
        const auto report =
            skeletal::compare_skeletons(externalInput, targetInput);
        // Applicability verdict (R13): the engine judges structure and
        // mapping uniqueness only; the caller combines reference-data
        // sufficiency. Both sides here come from real decoded files, so the
        // transform evidence is checked explicitly rather than assumed.
        // A decoded default matrix alone cannot establish cross-file bind,
        // axis, unit and local-pose semantics. Identical source content is a
        // sufficient identity proof; cross-file proof also checks the current
        // driver capability, separately from format transform compatibility.
        const auto transform=r.report.value("transformValidation").toMap();
        const bool transformProven=transform.value("verdict")=="compatible";
        const bool referenceSufficient =
            transformProven &&
            std::all_of(externalInput.bones.begin(), externalInput.bones.end(),
                        [](const skeletal::BoneRef &bone) {
                          return bone.has_default_transform;
                        }) &&
            std::all_of(targetInput.bones.begin(), targetInput.bones.end(),
                        [](const skeletal::BoneRef &bone) {
                          return bone.has_default_transform;
                        });
        QString applicabilityVerdict;
        switch (report.applicability) {
        case skeletal::Applicability::StructuralDirect:
          if(transform.value("verdict")=="incompatible")applicabilityVerdict=QStringLiteral("notApplicable");
          else if(referenceSufficient)applicabilityVerdict=transform.value("driverSupported").toBool()
              ?QStringLiteral("directApplicable"):QStringLiteral("compatibleNotDrivable");
          else applicabilityVerdict=QStringLiteral("structureOnly");
          break;
        case skeletal::Applicability::Partial:
          applicabilityVerdict = QStringLiteral("trialOnly");
          break;
        case skeletal::Applicability::Ambiguous:
          applicabilityVerdict = QStringLiteral("ambiguousMapping");
          break;
        case skeletal::Applicability::Incompatible:
          applicabilityVerdict = QStringLiteral("notApplicable");
          break;
        }
        const auto verdict =
            [](skeletal::Applicability value) {
              switch (value) {
              case skeletal::Applicability::Incompatible:
                return QStringLiteral("incompatible");
              case skeletal::Applicability::Ambiguous:
                return QStringLiteral("ambiguous");
              case skeletal::Applicability::Partial:
                return QStringLiteral("partial");
              case skeletal::Applicability::StructuralDirect:
                return QStringLiteral("structuralDirect");
              }
              return QString();
            };
        // Merged difference rows over the target skeleton, plus extras from
        // the external side, in stable input order.
        QVariantList rows;
        QSet<std::uint32_t> matchedTargets, matchedExternal, conflictingTargets;
        QSet<QString> ambiguousNames;
        for (const auto &duplicate : report.duplicates)
          if (duplicate.ambiguous) ambiguousNames.insert(QString::fromStdString(duplicate.name));
        for (const auto &pair : report.matched) {
          if (!pair.parents_verified) conflictingTargets.insert(pair.b);
          matchedTargets.insert(pair.b);
          matchedExternal.insert(pair.a);
        }
        const auto row = [](const std::string &name, const QString &status) {
          return QVariantMap{{"name", QString::fromStdString(name)}, {"status", status}};
        };
        for (std::uint32_t i = 0; i < targetInput.bones.size(); ++i)
          rows.append(row(targetInput.bones[i].name,
              ambiguousNames.contains(QString::fromStdString(targetInput.bones[i].name))
                  ? QStringLiteral("ambiguous") : matchedTargets.contains(i)
                  ? (conflictingTargets.contains(i) ? QStringLiteral("parentConflict") : QStringLiteral("matched"))
                  : QStringLiteral("missingInExternal")));
        for (std::uint32_t i = 0; i < externalInput.bones.size(); ++i)
          if (!matchedExternal.contains(i))
            rows.append(row(externalInput.bones[i].name,
                ambiguousNames.contains(QString::fromStdString(externalInput.bones[i].name))
                    ? QStringLiteral("ambiguous") : QStringLiteral("externalOnly")));
        QVariantList duplicates;
        for (const auto &duplicate : report.duplicates)
          duplicates.append(QVariantMap{
              {"name", QString::fromStdString(duplicate.name)},
              {"countA", duplicate.count_a},
              {"countB", duplicate.count_b},
              {"resolvedByChain", duplicate.resolved_by_chain},
              {"ambiguous", duplicate.ambiguous}});
        QVariantList conflicts;
        for (const auto &conflict : report.conflicts)
          conflicts.append(QString::fromStdString(conflict));
        compareResult_ = QVariantMap{
            {"transformValidation", r.report.value("transformValidation")},
            {"structureVerdict", verdict(report.applicability)},
            {"applicabilityVerdict", applicabilityVerdict},
            {"structureMatches", report.hierarchy_matches},
            {"referenceSufficient", referenceSufficient},
            {"bonesA", static_cast<int>(report.bones_a)},
            {"bonesB", static_cast<int>(report.bones_b)},
            {"matched", static_cast<int>(report.matched.size())},
            {"missingInExternal", report.extra_in_b.size()},
            {"externalOnly", report.missing_in_b.size()},
            {"duplicates", duplicates},
            {"conflicts", conflicts},
            {"rows", rows},
            {"externalSha256", r.report.value("sha256")}};
        // Stage timings (verification only; the UI ignores this key): parse
        // and engine/rows are separate budgets, end-to-end is job start to publish.
        const auto now = std::chrono::steady_clock::now();
        compareResult_.insert("timing", QVariantMap{
            {"readMs", r.readMs},
            {"guiCompareMs", std::chrono::duration<double, std::milli>(now - guiBegin).count()},
            {"endToEndMs", std::chrono::duration<double, std::milli>(now - it->started).count()}});
      }
      emit changed();
    }
    it = compareJobs_.erase(it);
    compareJobsFinished=true;
  }
  if (compareJobsFinished) emit changed();
  // A sample cached while the previous frame was prepared during playback is
  // presented once that frame is ready (never while iterating frame jobs).
  scheduleAbc();
  startPending();
  startFrame();
}
void ModelSession::attachRenderWindow(QQuickWindow *window) {
  if(gpuWindow_==window)return;
  disconnect(renderTickConnection_);disconnect(renderTickSequenceConnection_);
  gpuWindow_=window;renderTickSequence_.reset();
  if(!window)return;
  const auto sequence=std::make_shared<std::atomic<std::uint64_t>>(0);
  renderTickSequence_=sequence;
  // Render thread touches only a shared atomic, never GUI/session state.
  renderTickSequenceConnection_=connect(window,&QQuickWindow::frameSwapped,this,[sequence] {
    sequence->fetch_add(1,std::memory_order_relaxed);
  },Qt::DirectConnection);
  renderTickConnection_=connect(window,&QQuickWindow::frameSwapped,this,[this,sequence,handled=std::uint64_t{}]() mutable {
    if(renderTickSequence_!=sequence)return; // Queued work from a replaced window.
    const auto newest=sequence->load(std::memory_order_relaxed);
    if(newest==handled)return;
    handled=newest;
    if(playing_ && gpuEnabled_ && !gpuDisabled_ && gpuSkinActive() && gpuGeometry_) {
      // Native modal loops may present the same pose repeatedly. Consume only
      // the latest swap epoch, rather than evaluating every queued old tick.
      animTimer_.stop();tickAnimation();
    }
  },Qt::QueuedConnection);
}

bool ModelSession::gpuSkinActive() const {
  return document_ && (document_->gpuSkinDomain() ||
                       (pairMeshVisible_ && document_->pairPreviewDomain()));
}
void ModelSession::enableGpuSkinning(bool enabled) {
  enabled=enabled && qEnvironmentVariable("FORMAVIEW_SKINNING")!="cpu";
  if (gpuEnabled_==enabled) return;
  gpuEnabled_=enabled;
  if(!enabled && playing_)animTimer_.start();
  if (gpuSkinActive()) rebuild();
}
void ModelSession::renderingFailed(const QString &reason, std::shared_ptr<const formaview::render::Frame> failed) {
  if (!failed) return;
  const auto geometry = failed->geometry ? failed->geometry : failed;
  const auto matches = [&](const auto &published) {
    return published && (published->geometry ? published->geometry : published) == geometry;
  };
  if (!matches(frames_.model) && !matches(frames_.uv) && !matches(frame_)) return;
  frameError_ = "resource_limit: " + reason;
  emit changed();
}
void ModelSession::gpuSkinningFailed(const QString &reason, std::shared_ptr<const formaview::render::Frame> failed) {
  if (!failed || !failed->geometry) return;
  const auto matches = [&](const auto &published) {
    return published && published->geometry == failed->geometry;
  };
  if (!matches(frames_.model) && !matches(frames_.uv)) return;
  qWarning().noquote()<<"Model GPU fallback:"<<reason;
  gpuDisabled_=true; gpuGeometry_.reset(); gpuKey_.clear(); gpuViewOwner_.reset(); gpuHighlightOwner_.reset();
  rebuild();
  if(playing_)animTimer_.start();
}
bool ModelSession::refreshGpuFrame() {
  if (!gpuEnabled_ || gpuDisabled_ || !gpuSkinActive()) return false;
  const auto state=renderState(); const auto normKey=gpuStateKey(state), fullKey=stateKey(state);
  // Resident geometry serves any state with the same payload key; geometry that
  // could not keep every mesh resident needs the complete state key.
  if (!gpuGeometry_ || (gpuGeometry_->coversAllMeshes?normKey:fullKey)!=gpuKey_) {
    if (gpuJob_) { if (gpuJob_->generation!=generation_ || gpuJob_->normKey!=normKey) gpuJob_->stop.request_stop(); return true; }
    const auto document=document_; const auto revision=++frameRevision_;
    std::stop_source stop; const auto token=stop.get_token();
    gpuJob_=GpuJob{generation_,normKey,fullKey,std::move(stop),std::async(std::launch::async,[document,state,revision,token] {
      return buildModelGpuGeometry(*document,state,revision,token);
    })};
    preparing_=true; emit changed(); return true;
  }
  try {
    const auto worlds=rootLockOn_ && document_->localAt?rootLockedWorlds():document_->worldAt(static_cast<std::size_t>(clipIndex_),time_);
    const auto viewKey=gpuViewKey(state);
    if (gpuViewOwner_!=gpuGeometry_ || viewKey!=gpuViewKey_) { gpuView_=makeGpuView(*document_,*gpuGeometry_,state); gpuViewKey_=viewKey; gpuViewOwner_=gpuGeometry_; }
    if (gpuHighlightOwner_!=gpuGeometry_ || gpuHighlightNode_!=state.highlightedNode) { gpuHighlight_=resolveGpuHighlight(*document_,state.highlightedNode); gpuHighlightNode_=state.highlightedNode; gpuHighlightOwner_=gpuGeometry_; }
    frames_=buildModelGpuPose(*document_,*gpuGeometry_,gpuGeometry_->frames.uv,worlds,++frameRevision_*2,clipIndex_,time_,&gpuView_,&gpuHighlight_);
    // The PSA-first paired frame serves both views, as on the CPU path.
    if (document_->pairPreviewDomain()) frames_.uv=frames_.model;
    frame_=frames_.mode==1?frames_.uv:frames_.model;
    renderedState_=state; renderedState_.clip=clipIndex_; renderedState_.time=time_;
    preparing_=false;
    if (!std::exchange(frameError_, {}).isEmpty()) emit changed();
    emit frameChanged();
  } catch (const std::exception &e) {
    frameError_=QString::fromUtf8(e.what()); playing_=false; animTimer_.stop(); emit changed(); emit animChanged();
  }
  return true;
}
void ModelSession::finishGpuGeometry() {
  if (!gpuJob_ || gpuJob_->future.wait_for(std::chrono::seconds(0))!=std::future_status::ready) return;
  const bool alive=gpuJob_->generation==generation_ && document_;
  const auto stateNow=alive?renderState():ModelRenderState{};
  const bool current=alive && gpuJob_->normKey==gpuStateKey(stateNow);
  try {
    auto result=gpuJob_->future.get();
    // Resident geometry is usable when its payload key matches; geometry that could
    // not keep every mesh resident must match the complete state.
    const bool usable=current && (result->coversAllMeshes || gpuJob_->fullKey==stateKey(stateNow));
    if (usable && gpuEnabled_ && !gpuDisabled_) { const auto key=result->coversAllMeshes?gpuJob_->normKey:gpuJob_->fullKey; gpuGeometry_=std::move(result); gpuKey_=key; }
  } catch (const std::exception &e) {
    if (current) { qWarning()<<"GPU geometry CPU fallback:"<<e.what(); gpuDisabled_=true; }
  }
  gpuJob_.reset();
  if(gpuDisabled_ && playing_)animTimer_.start();
  if (document_ && document_->animated()) { preparing_=false; refreshAnimationFrame(); emit changed(); }
}

void ModelSession::finishAsyncFrame() {
  if (!asyncFrame_.has_value())
    return;
  if (asyncFrame_->future.wait_for(std::chrono::seconds(0)) !=
      std::future_status::ready)
    return;
  try {
    auto frames = asyncFrame_->future.get();
    if (asyncFrame_->generation == generation_ &&
        asyncFrame_->poseRequest == poseRequest_ && document_ && frames.model) {
      frames_ = std::move(frames);
      frame_ = frames_.mode == 1 ? frames_.uv : frames_.model;
      renderedState_ = std::move(asyncFrame_->state);
      // Material selection/filter edits outside UV mode do not change the
      // geometry, so they need no new pose job. Keep their current inspection
      // state when publishing a pose sampled before the edit.
      if (renderedState_.mode != 1) {
        renderedState_.material=material_;
        renderedState_.materialOnly=materialOnly_;
      }
      const bool clearedError = !std::exchange(frameError_, {}).isEmpty();
      const bool wasPreparing = std::exchange(preparing_, false);
      emit frameChanged();
      if (wasPreparing || clearedError) emit changed();
    }
  } catch (const std::exception &e) {
    if (asyncFrame_->generation == generation_ &&
        asyncFrame_->poseRequest == poseRequest_) {
      frameError_ = QString::fromUtf8(e.what());
      preparing_ = false;
      globalFrameFailure_ = levels_ != renderedState_.levels || classes_ != renderedState_.classes;
      playing_=false;
      animTimer_.stop();
      pendingAnimationFrame_=false;
      emit animChanged();
      emit changed();
    }
  }
  asyncFrame_.reset();
  if (std::exchange(pendingAnimationFrame_, false))
    refreshAnimationFrame();
}

void ModelSession::tickAnimation() {
  if (abcActive()) {
    tickAbc();
    return;
  }
  if (!playing_ || !document_ || !document_->animated()) {
    animTimer_.stop();
    playing_ = false;
    emit animChanged();
    return;
  }
  const auto &clip = document_->animationClips[
      static_cast<std::size_t>(clipIndex_)];
  const bool gpuClock=gpuEnabled_ && !gpuDisabled_ && gpuGeometry_ && gpuSkinActive();
  const double dt = gpuClock ? static_cast<double>(animClock_.nsecsElapsed()) / 1e9
                             : static_cast<double>(animClock_.restart()) / 1000.0;
  if(gpuClock)animClock_.restart();
  const double previousTime = time_;
  time_ += dt * speed_;
  if (time_ > clip.end) {
    if (loop_) {
      const double duration = std::max(1e-9, clip.end - clip.start);
      if (std::isfinite(time_)) {
        time_ = clip.start + std::fmod(time_ - clip.start, duration);
      } else {
        // Reduce elapsed time before multiplication when a finite speed would
        // overflow the clock step (e.g. after a long UI stall).
        const double advance = std::fmod(dt, duration / speed_) * speed_;
        time_ = clip.start +
                std::fmod(previousTime - clip.start + advance, duration);
      }
    } else {
      time_ = clip.end;
      playing_ = false;
      animTimer_.stop();
    }
  }
  refreshAnimationFrame();
  emit animChanged();
}

void ModelSession::refreshAnimationFrame() {
  if (!document_ || !document_->animated())
    return;
  if (clipIndex_ < 0 ||
      static_cast<std::size_t>(clipIndex_) >= document_->animationClips.size())
    return;
  if (refreshGpuFrame()) return;
  // Explicit changes invalidate in-flight frames; clock ticks may publish
  // their sampled time while coalescing the next pose into one pending job.
  if ((document_->externallyDriven || document_->nativeAnimated) &&
      !document_->meshes.empty() && asyncFrame_) {
    pendingAnimationFrame_ = true;
    return;
  }
  try {
    std::optional<std::vector<formaview::Matrix4>> lockedWorlds;
    if (rootLockOn_ && document_->localAt)
      lockedWorlds = rootLockedWorlds();
    const bool pairMesh = (pairMeshVisible_ || !document_->externalActorSources.empty()) && document_->pairSource &&
                           !document_->pairBoneMap.empty();
    if ((document_->externallyDriven || document_->nativeAnimated) &&
        !document_->meshes.empty()) {
      // Full-mesh CPU skinning belongs off the GUI thread: the published
      // frame lags at most one build behind the clock; the UI never blocks.
      const auto document = document_;
      const auto clip = static_cast<std::size_t>(clipIndex_);
      const auto seconds = time_;
      // Static model/UV payloads reserve revisions 2*N and 2*N+1.
      // Animation must use the same domain, including after many opens.
      const auto revision = ++frameRevision_;
      const auto state = renderState();
      const auto worlds =
          lockedWorlds
              ? std::make_shared<std::vector<formaview::Matrix4>>(
                    std::move(*lockedWorlds))
              : nullptr;
      std::stop_source stop;
      const auto token = stop.get_token();
      asyncFrame_ = AsyncFrame{
          generation_, poseRequest_, clipIndex_, seconds, state, std::move(stop),
          std::async(std::launch::async,
                     [document, clip, seconds, revision, pairMesh, worlds, state, token] {
                       const auto pose = buildAnimationFrame(
                           *document, clip, seconds, revision * 2, pairMesh,
                           worlds ? worlds.get() : nullptr, token);
                       ModelFrames result;
                       if (state.mode != 0 || state.isolated || state.materialOnly ||
                           std::any_of(document->meshes.begin(), document->meshes.end(), [](const auto &mesh) {
                             return !mesh.materials.empty() || !mesh.colours.empty() || !mesh.textures.empty();
                           }) ||
                           std::any_of(state.textures.begin(), state.textures.end(), [](const auto &links) { return !links.empty(); }) ||
                           !state.highlightedNode.isEmpty() ||
                           std::find(state.visible.begin(), state.visible.end(), false) != state.visible.end())
                         result = buildModelFrames(*document, state, revision, token, pose.get());
                       else
                         result.model = pose;
                       result.pose = pose;
                       return result;
                     }),
      };
      return;
    }
    const auto frame = buildAnimationFrame(
        *document_, static_cast<std::size_t>(clipIndex_), time_,
        ++frameRevision_ * 2, pairMesh,
        lockedWorlds ? &*lockedWorlds : nullptr);
    frames_.model = frame;
    frames_.uv = frame;
    frame_ = frame;
    renderedState_.clip = clipIndex_;
    renderedState_.time = time_;
    if (!std::exchange(frameError_, {}).isEmpty()) emit changed();
    emit frameChanged();
  } catch (const std::exception &e) {
    frameError_ = QString::fromUtf8(e.what());
    playing_ = false;
    animTimer_.stop();
    emit animChanged();
    emit changed();
  }
}

void ModelSession::togglePlay() {
  if (abcActive()) {
    const auto &times = abcSource_->times();
    if (times.size() < 2)
      return;
    playing_ = !playing_;
    if (playing_) {
      abcDirection_ = 1;
      if (time_ >= times.back()) {
        time_ = times.front();
        requestAbcSample(0);
      }
      animClock_.restart();
      animTimer_.start();
    } else {
      animTimer_.stop();
    }
    scheduleAbc();
    emit animChanged();
    return;
  }
  if (!document_ || !document_->animated())
    return;
  playing_ = !playing_;
  if (playing_) {
    const auto &clip =
        document_->animationClips[static_cast<std::size_t>(clipIndex_)];
    if (time_ >= clip.end)
      time_ = clip.start;
    animClock_.restart();
    animTimer_.start();
  } else {
    animTimer_.stop();
  }
  emit animChanged();
}

void ModelSession::seek(double seconds) {
  if (abcActive() && std::isfinite(seconds)) {
    const auto &times = abcSource_->times();
    time_ = std::clamp(seconds, times.front(), times.back());
    requestAbcSample(abcSource_->nearest(time_));
    return;
  }
  if (!document_ || !document_->animated() || !std::isfinite(seconds))
    return;
  const auto &clip =
      document_->animationClips[static_cast<std::size_t>(clipIndex_)];
  time_ = std::clamp(seconds, clip.start, clip.end);
  ++poseRequest_;
  refreshAnimationFrame();
  emit animChanged();
}

void ModelSession::stepFrame(int frames) {
  if (abcActive() && frames) {
    abcDirection_ = frames > 0 ? 1 : -1;
    const auto last = static_cast<long long>(abcSource_->times().size()) - 1;
    seekSample(static_cast<int>(std::clamp(
        static_cast<long long>(abcRequested_) + frames, 0LL, last)));
    return;
  }
  if (!document_ || !document_->animated() || !frames)
    return;
  const auto &clip =
      document_->animationClips[static_cast<std::size_t>(clipIndex_)];
  const double step = 1.0 / clip.fps;
  time_ = std::clamp(time_ + step * frames, clip.start, clip.end);
  ++poseRequest_;
  refreshAnimationFrame();
  emit animChanged();
}

void ModelSession::setClipIndex(int index) {
  if (!document_ || !document_->animated() || index < 0 ||
      static_cast<std::size_t>(index) >= document_->animationClips.size())
    return;
  clipIndex_ = index;
  time_ = document_->animationClips[static_cast<std::size_t>(index)].start;
  ++poseRequest_;
  refreshAnimationFrame();
  emit animChanged();
  emit changed();
}

void ModelSession::setLoop(bool value) {
  if (loop_ == value)
    return;
  loop_ = value;
  emit animChanged();
}

std::vector<formaview::Matrix4> ModelSession::rootLockedWorlds() {
  // Root lock: replace the selected root's local translation/rotation with
  // the base (source scale kept), re-chain the worlds, and never mutate the
  // source pose. Returns the unlocked worlds when the lock cannot apply.
  const auto clip = static_cast<std::size_t>(clipIndex_);
  const auto &parents = document_->nodeAnimDomain()
                            ? document_->animNodeParents
                            : document_->boneParents;
  const int root = resolvedRoot();
  if (root < 0)
    return document_->worldAt(clip, time_); // Multi-root unselected.
  if (!rootLockInfo().value("rootMapped", true).toBool())
    return document_->worldAt(clip, time_);
  const auto base = rootBaseFor(static_cast<int>(clip));
  auto locals = document_->localAt(clip, time_);
  if (static_cast<std::size_t>(root) >= locals.size())
    return document_->worldAt(clip, time_);
  locals[static_cast<std::size_t>(root)] =
      lockRootLocal(locals[static_cast<std::size_t>(root)],
                    static_cast<std::size_t>(root) < base->size()
                        ? (*base)[static_cast<std::size_t>(root)]
                        : formaview::Matrix4{1, 0, 0, 0, 0, 1, 0, 0,
                                             0, 0, 1, 0, 0, 0, 0, 1});
  return chainWorlds(locals, parents);
}

const std::vector<formaview::Matrix4> *
ModelSession::rootBaseFor(int clip) {
  // Cached per clip: first-frame locals evaluated once; reference locals and
  // zero (identity) are static. Never re-evaluated per frame.
  if (rootBase_ == 1 && !document_->referenceLocals.empty())
    return &document_->referenceLocals;
  if (rootBase_ == 2) {
    static const std::vector<formaview::Matrix4> identityOne{
        formaview::Matrix4{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
    return &identityOne; // Index 0 only; zero base applies to the root which
                         // uses identity via the bounds check below.
  }
  if (clip != rootBaseClip_ || rootBaseLocals_.empty()) {
    rootBaseClip_ = clip;
    rootBaseLocals_ = document_->localAt(
        static_cast<std::size_t>(clip),
        document_->animationClips[static_cast<std::size_t>(clip)].start);
  }
  return &rootBaseLocals_;
}

void ModelSession::setRootLock(bool value) {
  if (!document_ || !document_->animated() || !document_->localAt ||
      rootLockOn_ == value)
    return;
  rootLockOn_ = value;
  ++poseRequest_;
  refreshAnimationFrame();
  emit animChanged();
}

void ModelSession::setRootBase(int base) {
  const auto clamped = std::clamp(base, 0, 2);
  if (rootBase_ == clamped)
    return;
  if (clamped == 1 && (!document_ || document_->referenceLocals.empty()))
    return; // Reference pose stays disabled without verified data.
  rootBase_ = clamped;
  rootBaseClip_ = -1;
  rootBaseLocals_.clear();
  if (rootLockOn_) {
    ++poseRequest_;
    refreshAnimationFrame();
  }
  emit animChanged();
}

void ModelSession::setRootIndex(int index) {
  const auto candidates = rootCandidates();
  if (index != -1 && std::none_of(candidates.cbegin(), candidates.cend(),
                                [index](const QVariant &candidate) {
                                  return candidate.toMap().value("index").toInt() == index;
                                }))
    return;
  if (index == rootIndex_)
    return;
  rootIndex_ = index;
  if (rootLockOn_) {
    ++poseRequest_;
    refreshAnimationFrame();
  }
  emit animChanged();
}

QVariantList ModelSession::rootCandidates() const {
  QVariantList result;
  if (!document_)
    return result;
  const bool nodeDomain = document_->nodeAnimDomain();
  const auto &parents = nodeDomain ? document_->animNodeParents
                                   : document_->boneParents;
  const auto name = [&](std::size_t i) {
    return nodeDomain ? document_->nodes.at(static_cast<int>(i))
                          .toMap()
                          .value("name")
                          .toString()
                      : document_->boneNames.value(static_cast<int>(i));
  };
  for (std::size_t i = 0; i < parents.size(); ++i)
    if (nodeDomain
            ? std::find(document_->animRootNodes.begin(), document_->animRootNodes.end(),
                        static_cast<std::int32_t>(i)) != document_->animRootNodes.end()
            : parents[i] < 0 || static_cast<std::size_t>(parents[i]) >= parents.size())
      result.append(QVariantMap{{"index", static_cast<int>(i)},
                                {"name", name(i)}});
  return result;
}

int ModelSession::resolvedRoot() const {
  const auto candidates = rootCandidates();
  for (const auto &candidate : candidates)
    if (candidate.toMap().value("index").toInt() == rootIndex_)
      return rootIndex_;
  return candidates.size() == 1
             ? candidates.front().toMap().value("index").toInt()
             : -1;
}

QVariantMap ModelSession::rootLockInfo() const {
  if (!document_ || !document_->animated())
    return {};
  const auto candidates = rootCandidates();
  const int root = resolvedRoot();
  const bool referenceAvailable = !document_->referenceLocals.empty();
  bool rootMapped = true;
  if (document_->externallyDriven && !document_->animNodeParents.empty() && root >= 0) {
    const auto source = document_->animationClips.at(
        static_cast<std::size_t>(clipIndex_)).source;
    // Native clips address this target's own node domain; appended external
    // clips alone require proof that their mapping includes the selected root.
    rootMapped = (source < 0 && document_->nativeFbxAnimated) ||
        (source >= 0 &&
         static_cast<std::size_t>(source) < document_->externalMaps.size() &&
         std::find(document_->externalMaps[source].begin(),
                   document_->externalMaps[source].end(), root) !=
             document_->externalMaps[source].end());
  }
  QString status;
  if (!rootLockOn_)
    status = QStringLiteral("off");
  else if (root < 0)
    status = QStringLiteral("multiRoot");
  else if (!rootMapped)
    status = QStringLiteral("unmappedRoot");
  else
    status = QStringLiteral("locked");
  QString baseLabel = rootBase_ == 1   ? QStringLiteral("reference")
                      : rootBase_ == 2 ? QStringLiteral("zero")
                                       : QStringLiteral("firstFrame");
  QString reason;
  if (rootBase_ == 1 && !referenceAvailable)
    reason = QStringLiteral("noReference");
  if (!rootMapped)
    reason = QStringLiteral("unmappedRoot");
  const auto rootName = [&]() {
    if (root < 0)
      return QString{};
    if (document_->nodeAnimDomain())
      return document_->nodes.at(root).toMap().value("name").toString();
    return document_->boneNames.value(root);
  }();
  return {{"on", rootLockOn_},
          {"root", root},
          {"rootName", rootName},
          {"rootCount", candidates.size()},
          {"rootMapped", rootMapped},
          {"base", baseLabel},
          {"referenceAvailable", referenceAvailable},
          {"status", status},
          {"reason", reason}};
}

void ModelSession::setPairMeshVisible(bool value) {
  if (pairMeshVisible_ == value || !document_ || !document_->pairSource)
    return;
  pairMeshVisible_ = value;
  // Leaving the GPU paired preview hands the clock back to the CPU timer; the
  // swap-driven GPU clock stops it again once the preview geometry is ready.
  if (playing_) animTimer_.start();
  refreshAnimationFrame();
  emit animChanged();
}

void ModelSession::setSpeed(double value) {
  if (!std::isfinite(value) || value <= 0 || speed_ == value)
    return;
  speed_ = value;
  emit animChanged();
}


void ModelSession::attachExternalAnimation(const QString &input) {
  const bool attachable =
      document_ && (!document_->nativeAnimated || document_->nativeFbxAnimated) && !document_->meshes.empty() &&
      ((document_->pairSource && !document_->skeletonOnly &&
        !document_->pairSource->bones.empty()) ||
       (document_->animNodeParents.size() == document_->animNodeLocals.size() &&
        !document_->animNodeParents.empty()));
  if (!attachable || busy_)
    return;
  const auto path = QUrl(input).isLocalFile() ? QUrl(input).toLocalFile()
                                              : input;
  const bool actor = document_->pairSource && !document_->skeletonOnly;
  if (path.isEmpty() || QFileInfo(path).suffix().compare(
                            actor ? "psa" : "fbx", Qt::CaseInsensitive) != 0)
    return;
  // A new dialog batch starts once the previous one has fully settled.
  if (!externalBusy()) {
    externalRequested_ = externalApplied_ = 0;
    externalFailed_.clear();
    externalAppliedStatus_.clear();
    externalAppliedNotice_.clear();
  }
  ++externalRequested_;
  // One multi-select dialog submits every file at once. A bounded number are
  // read concurrently; the rest wait in order and are never dropped.
  if (externalJobs_.size() >= 8) {
    externalQueue_.push_back(path);
    emit changed();
    return;
  }
  startExternalJob(path);
}

void ModelSession::startExternalJob(const QString &path) {
  const bool actor = document_->pairSource && !document_->skeletonOnly;
  const auto targetPath = path_;
  const auto companion = actor ? companionCandidate(path) : QString{};
  std::stop_source stop;
  const auto token = stop.get_token();
  auto future = std::async(std::launch::async, [path, token, actor, targetPath, companion]()
                               -> ModelSession::ExternalResult {
    ModelSession::ExternalResult result;
    result.path = path;
    try {
      if (actor) {
        auto paired = readModelDocument(path, token, targetPath, companion);
        if (paired->referenceRejected || !paired->pairSource ||
            paired->pairBoneMap.empty())
          throw skeletal::Failure(ErrorCode::InvalidArgument,
              (QStringLiteral("PSA does not match the current PSK: ") +
               paired->referenceRejectReason).toStdString());
        result.actor = std::move(paired);
        return result;
      }
      const std::filesystem::path source(path.toStdWString());
      AccessScope scope;
      scope.input_roots = {source.parent_path()};
      const auto lease = InputLease::open(scope, source,
                                          512ULL * 1024 * 1024, token);
      skeletal::ReadOptions options;
      options.stop = token;
      auto holder = std::make_shared<skeletal::FbxDocument>(
          skeletal::FbxDocument::read(source, options));
      lease.require_unchanged(lease.snapshot());
      if (text(holder->data().source.identity.sha256) !=
          text(lease.snapshot().content_sha256))
        throw skeletal::Failure(ErrorCode::InputChanged,
                                "External animation changed while decoding");
      result.holder = std::move(holder);
    } catch (const std::exception &e) {
      result.error = QString::fromUtf8(e.what());
    }
    return result;
  });
  externalJobs_.push_back({generation_, std::move(stop), std::move(future)});
  emit changed();
}

void ModelSession::detachExternalAnimation() {
  if (!document_ || !document_->externallyDriven || busy_)
    return;
  const auto path = path_;
  open(path);
}

QVariantMap ModelSession::externalInfo() const {
  if (!document_)
    return {};
  return {{"driven", document_->externallyDriven},
          {"format", document_->pairSource && !document_->skeletonOnly ? "psa" : "fbx"},
          {"status", document_->externalStatus},
          {"notice", document_->externalNotice},
          {"path", document_->externalPath},
          {"requested", externalRequested_},
          {"applied", externalApplied_},
          {"failedFiles", externalFailed_},
          {"pending", static_cast<int>(externalJobs_.size() + externalQueue_.size())}};
}

void ModelSession::applyExternalActor(std::shared_ptr<ModelDocument> source,
                                      const QString &path) {
  // The worker re-reads the target under an input lease. Never attach a pose
  // evaluated against a different on-disk revision of the displayed mesh.
  if (!document_->pairSource || !source->pairSource ||
      source->pairSource->source.identity.sha256 !=
          document_->pairSource->source.identity.sha256) {
    document_->externalStatus = QStringLiteral("refused");
    document_->externalNotice = QStringLiteral("PSK changed since opening; reopen the model before attaching PSA");
    emit animChanged();
    return;
  }
  if (asyncFrame_ || gpuJob_ || pickJob_)
    document_ = std::make_shared<ModelDocument>(*document_);
  auto &d = *document_;
  const auto firstClip = d.externallyDriven ? d.animationClips.size() : 0;
  if (!d.externallyDriven) {
    d.animationClips.clear();
    d.clips.clear();
    d.boneNames.clear();
    d.boneParents.clear();
    d.pairBoneMap.resize(d.pairSource->bones.size());
    d.referenceLocals.resize(d.pairSource->bones.size());
    for (std::size_t i = 0; i < d.pairSource->bones.size(); ++i) {
      const auto &bone = d.pairSource->bones[i];
      d.boneNames.push_back(text(bone.source.original_name));
      d.boneParents.push_back(bone.parent ? static_cast<std::int32_t>(*bone.parent) : -1);
      d.pairBoneMap[i] = static_cast<std::uint32_t>(i);
      d.referenceLocals[i] = source->referenceLocals.at(source->pairBoneMap.at(i));
    }
  }
  const auto sourceIndex = static_cast<int>(d.externalActorSources.size());
  d.externalActorSources.push_back(source);
  for (std::size_t i = 0; i < source->animationClips.size(); ++i) {
    auto entry = source->animationClips[i];
    entry.source = sourceIndex;
    d.animationClips.push_back(entry);
    d.clips.push_back(source->clips.at(static_cast<int>(i)));
  }
  const auto sources = d.externalActorSources;
  const auto clips = d.animationClips;
  d.localAt = [sources, clips](std::size_t clip, double seconds) {
    const auto index = clips.at(clip).source;
    const auto &source = sources.at(static_cast<std::size_t>(index));
    std::size_t localClip = 0;
    for (std::size_t i = 0; i < clip; ++i)
      if (clips[i].source == index)
        ++localClip;
    const auto locals = source->localAt(localClip, seconds);
    std::vector<formaview::Matrix4> target(source->pairBoneMap.size());
    for (std::size_t i = 0; i < target.size(); ++i)
      target[i] = locals.at(source->pairBoneMap[i]);
    return target;
  };
  d.worldAt = [localAt = d.localAt, parents = d.boneParents](std::size_t clip, double seconds) {
    return chainWorlds(localAt(clip, seconds), parents);
  };
  d.externallyDriven = true;
  d.externalPath = path;
  d.externalStatus = QStringLiteral("verified");
  d.externalNotice = QStringLiteral("PSA paired with current PSK · %1 bones · %2")
      .arg(d.boneParents.size())
      .arg(source->companionPath.isEmpty() ? QStringLiteral("no track config")
                                            : QStringLiteral("track config applied"));
  if (!source->warnings.isEmpty())
    d.externalNotice += QStringLiteral(" · ") + source->warnings.join(QStringLiteral("; "));
  clipIndex_ = static_cast<int>(firstClip);
  time_ = d.animationClips.at(firstClip).start;
  playing_ = false;
  rootIndex_ = -1;
  rootBaseClip_ = -1;
  rootBaseLocals_.clear();
  ++poseRequest_;
  rebuild();
  // Publish the enlarged clip model before its new current index. Otherwise
  // ComboBox clamps the index against the old count and keeps a stale label.
  emit changed();
  emit animChanged();
  emit documentChanged();
}

void ModelSession::applyExternalDrive(
    std::shared_ptr<skeletal::FbxDocument> holder, const QString &path) {
  const auto &data = holder->data();
  // Both sides use their FULL node domains: parent chains must align through
  // intermediate (non-bone) nodes, and the skinning worlds need them anyway.
  // Unnamed synthetic roots (e.g. the ufbx scene root) are excluded with a
  // full index remap: two empty names matching by accident must never soften
  // an incompatible verdict, and parent chains lift through removed nodes so
  // the engine still resolves chain names correctly.
  auto targetInput = modelCompareInput(*document_);
  skeletal::CompareInput externalInput;
  externalInput.source = path.toStdString();
  for (std::size_t i = 0; i < data.nodes.size(); ++i) {
    const auto &node = data.nodes[i];
    externalInput.bones.push_back({static_cast<std::uint32_t>(i), node.source.original_name,
        node.parent, true});
  }
  externalInput = skeletal::named_compare_input(std::move(externalInput));
  const auto report = skeletal::compare_skeletons(externalInput, targetInput);
  const bool verified = report.applicability ==
                        skeletal::Applicability::StructuralDirect;
  // A partial match is only a trial drive when at least one real BONE
  // matches: container-only alignment (scene roots, armature holders that
  // happen to share a name) must stay refused.
  bool boneMatched = false;
  for (const auto &pair : report.matched)
    if (data.nodes.at(externalInput.bones.at(pair.a).index).bone &&
        document_->nodes.at(static_cast<int>(targetInput.bones.at(pair.b).index)).toMap().value("bone").toBool()) {
      boneMatched = true;
      break;
    }
  const bool partial =
      report.applicability == skeletal::Applicability::Partial && boneMatched;
  if (!verified && !partial) {
    document_->externalStatus = QStringLiteral("refused");
    document_->externalNotice =
        report.applicability == skeletal::Applicability::Ambiguous
            ? QStringLiteral("Ambiguous bone mapping; drive refused")
            : QStringLiteral("Incompatible skeletons; drive refused");
    emit animChanged();
    emit changed();
    return;
  }
  std::vector<std::int32_t> mapping(data.nodes.size(), -1);
  for (const auto &pair : report.matched) {
    const auto external = externalInput.bones.at(pair.a).index;
    const auto target = targetInput.bones.at(pair.b).index;
    mapping[external] = static_cast<std::int32_t>(target);
  }
  // Multi-source accumulate: a new source appends its clips; the clip's
  // source index dispatches evaluation to the right holder and map. The
  // first successful source resets any previous notice; detach clears all.
  // Workers retain immutable metadata. Copy only when an attachment changes
  // a document being read by an in-flight worker, never on playback ticks.
  if (asyncFrame_ || gpuJob_ || pickJob_)
    document_ = std::make_shared<ModelDocument>(*document_);
  const auto source = static_cast<int>(document_->externalHolders.size());
  if (!document_->externallyDriven && !document_->nativeFbxAnimated) {
    document_->animationClips.clear();
    document_->clips.clear();
  }
  document_->externalHolders.push_back(holder);
  document_->externalMaps.push_back(std::move(mapping));
  for (std::size_t c = 0; c < data.clips.size(); ++c) {
    const auto &clip = data.clips[c];
    ModelAnimationClip entry;
    entry.name = text(clip.source.original_name);
    entry.source = source;
    entry.start = clip.start;
    entry.end = clip.end;
    entry.fps = clip.fps > 0 ? clip.fps : 30.0;
    entry.lastFrame = clip.fps > 0
                          ? static_cast<int>((clip.end - clip.start) * clip.fps)
                          : 0;
    document_->animationClips.push_back(entry);
    document_->clips.push_back(QVariantMap{
        {"name", entry.name},
        {"start", entry.start},
        {"end", entry.end},
        {"fps", entry.fps},
        {"lastFrame", entry.lastFrame}});
  }
  const auto parents = document_->animNodeParents;
  const auto staticLocals = document_->animNodeLocals;
  std::vector<int> sources(document_->animationClips.size());
  for (std::size_t i = 0; i < document_->animationClips.size(); ++i)
    sources[i] = document_->animationClips[i].source;
  const auto holders = document_->externalHolders;
  const auto maps = document_->externalMaps;
  // Native indices address the original source, independent of appended maps.
  const auto nativeLocalAt = document_->nativeFbxLocalAt;
  document_->localAt = [holders, maps, sources, staticLocals, nativeLocalAt](
                           std::size_t clip, double seconds) {
    const auto sourceIndex =
        clip < sources.size() ? sources[clip] : 0;
    if (sourceIndex < 0)
      return nativeLocalAt(clip, seconds);
    const auto &holder = holders.at(static_cast<std::size_t>(sourceIndex));
    const auto &mapping = maps.at(static_cast<std::size_t>(sourceIndex));
    // The external clip index within its own source.
    std::size_t externalClip = 0, seen = 0;
    for (std::size_t i = 0; i <= clip && i < sources.size(); ++i)
      if (sources[i] == sourceIndex)
        externalClip = seen++;
    const auto external = holder->evaluate(externalClip, seconds).local;
    std::vector<formaview::Matrix4> locals = staticLocals;
    for (std::size_t e = 0; e < mapping.size(); ++e) {
      const auto target = mapping[e];
      if (target >= 0 && static_cast<std::size_t>(target) < locals.size() &&
          e < external.size())
        locals[static_cast<std::size_t>(target)] = external[e];
    }
    return locals;
  };
  document_->worldAt = [localAt = document_->localAt, parents, sources,
                        nativeWorldAt = document_->nativeFbxWorldAt](
                           std::size_t clip, double seconds) {
    if (sources.at(clip) < 0) return nativeWorldAt(clip, seconds);
    return chainWorlds(localAt(clip, seconds), parents);
  };
  document_->externallyDriven = true;
  if (document_->externalPath.isEmpty())
    document_->externalPath = path;
  else if (!document_->externalPath.contains(path))
    document_->externalPath += QStringLiteral(" ; ") + path;
  document_->externalStatus =
      verified ? QStringLiteral("verified") : QStringLiteral("trial");
  document_->externalNotice =
      verified
          ? QStringLiteral("Verified mapping: %1/%2 bones")
                .arg(report.matched.size())
                .arg(targetInput.bones.size())
          : QStringLiteral(
                "Trial drive (unverified compatibility): %1/%2 bones mapped; "
                "unmapped target bones keep their static pose")
                .arg(report.matched.size())
                .arg(targetInput.bones.size());
  // Keep native clips first, but attaching an external source must select
  // the first external clip, as it did before native clips were exposed.
  const auto firstExternal = std::find_if(document_->animationClips.begin(),
      document_->animationClips.end(), [](const auto &clip) { return clip.source >= 0; });
  clipIndex_ = firstExternal != document_->animationClips.end()
      ? static_cast<int>(firstExternal - document_->animationClips.begin()) : 0;
  time_ = document_->animationClips.empty() ? 0.0
      : document_->animationClips.at(static_cast<std::size_t>(clipIndex_)).start;
  playing_ = false;
  rootIndex_ = -1;
  rootBaseClip_ = -1;
  rootBaseLocals_.clear();
  ++poseRequest_;
  rebuild();
  emit animChanged();
  emit changed();
  emit documentChanged();
}

bool ModelSession::hasComparableSkeleton() const {
  if (!document_)
    return false;
  if (document_->skeletonOnly)
    return !document_->boneNames.empty();
  const auto suffix=QFileInfo(path_).suffix().toLower();
  if((suffix=="gltf"||suffix=="glb")&&document_->transformEvidence&&!document_->nodes.empty())return true;
  return QFileInfo(path_).suffix().compare("psk", Qt::CaseInsensitive) == 0 ||
         QFileInfo(path_).suffix().compare("fbx", Qt::CaseInsensitive) == 0;
}

void ModelSession::compareSkeletonWith(const QString &input) {
  if (!hasComparableSkeleton() || busy_ || !compareJobs_.empty())
    return;
  const auto path = QUrl(input).isLocalFile() ? QUrl(input).toLocalFile()
                                              : input;
  const auto suffix=QFileInfo(path).suffix().toLower();
  if(path.isEmpty()||(suffix!="fbx"&&suffix!="gltf"&&suffix!="glb"))return;
  compareResult_ = {};
  compareError_.clear();
  std::stop_source stop;
  const auto token = stop.get_token();
  const auto targetDecoder = document_->decoderSnapshot;
  const auto targetEvidence = document_->transformEvidence;
  const auto targetInput = modelCompareInput(*document_);
  auto future = std::async(std::launch::async, [path, token, targetEvidence, targetInput, targetDecoder]()
                               -> ModelSession::CompareResult {
    ModelSession::CompareResult result;
    const auto readBegin = std::chrono::steady_clock::now();
    try {
      const auto suffix=QFileInfo(path).suffix().toLower();
      if(suffix=="gltf"||suffix=="glb") {
        const auto decoded=readModelDocument(path,token);
        const auto a=decoded->transformEvidence?decoded->transformEvidence(token):skeletal::transform::Evidence{};
        const auto b=targetEvidence?targetEvidence(token):skeletal::transform::Evidence{};
        const auto sourceInput=modelCompareInput(*decoded);
        QVariantList bones;
        for(std::size_t i=0;i<static_cast<std::size_t>(decoded->nodes.size());++i) {
          const auto n=decoded->nodes[static_cast<qsizetype>(i)].toMap();
          const auto &e=a.nodes.at(i);
          bones.append(QVariantMap{{"index",static_cast<uint>(i)},{"name",n.value("name")},
            {"parent",e.parent?static_cast<int>(*e.parent):-1},{"hasDefaultTransform",e.restLocal.has_value()||e.restMatrix.has_value()}});
        }
        auto diagnostics=transformDiagnostics(a,b,sourceInput,targetInput,token);
        diagnostics.insert("fixedDecoder",compareFixedDecoder(decoded->decoderSnapshot,targetDecoder,token));
        result.report={{"bones",bones},{"sha256",text(a.sha256)},
          {"transformValidation",diagnostics}};
        check(token);
        result.readMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-readBegin).count();
        return result;
      }
      const std::filesystem::path source(path.toStdWString());
      AccessScope scope;
      scope.input_roots = {source.parent_path()};
      const auto lease = InputLease::open(scope, source,
                                          512ULL * 1024 * 1024, token);
      skeletal::ReadOptions options;
      options.stop = token;
      auto holder = std::make_shared<skeletal::FbxDocument>(
          skeletal::FbxDocument::read(source, options));
      lease.require_unchanged(lease.snapshot());
      QVariantList bones;
      for (std::size_t i = 0; i < holder->data().nodes.size(); ++i) {
        const auto &node = holder->data().nodes[i];
        bones.append(QVariantMap{
            {"index", static_cast<uint>(i)},
            {"name", text(node.source.original_name)},
            {"hasDefaultTransform", std::all_of(node.default_local.begin(), node.default_local.end(), [](double v) { return std::isfinite(v); })},
            {"parent",
             node.parent ? QVariant(static_cast<int>(*node.parent))
                         : QVariant(-1)}});
      }
      result.report = {{"bones", bones},
                       {"sha256",
                        text(holder->data().source.identity.sha256)}};
      skeletal::CompareInput sourceInput;
      for (const auto &value : bones) {
        const auto entry=value.toMap(); const int parent=entry.value("parent").toInt();
        sourceInput.bones.push_back({entry.value("index").toUInt(),entry.value("name").toString().toStdString(),
          parent>=0?std::optional<std::uint32_t>(parent):std::nullopt,entry.value("hasDefaultTransform").toBool()});
      }
      sourceInput=skeletal::named_compare_input(std::move(sourceInput));
      const auto a=holder->transform_evidence(token);
      const auto b=targetEvidence?targetEvidence(token):skeletal::transform::Evidence{};
      result.report.insert("transformValidation",transformDiagnostics(a,b,sourceInput,targetInput,token));
      check(token);
    } catch (const std::exception &e) {
      result.error = QString::fromUtf8(e.what());
    }
    result.readMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - readBegin).count();
    return result;
  });
  compareJobs_.push_back({generation_, std::move(stop), std::move(future)});
  emit changed();
}

QVariantMap ModelSession::clipInfo() const {
  if (!document_ || !document_->animated() || clipIndex_ < 0 ||
      static_cast<std::size_t>(clipIndex_) >= document_->animationClips.size())
    return {};
  const auto &clip =
      document_->animationClips[static_cast<std::size_t>(clipIndex_)];
  const int frame = clip.fps > 0
                        ? static_cast<int>(std::llround(
                              (time_ - clip.start) * clip.fps))
                        : 0;
  return {{"name", clip.name},
          {"start", clip.start},
          {"end", clip.end},
          {"fps", clip.fps},
          {"lastFrame", clip.lastFrame},
          {"frame", std::clamp(frame, 0, clip.lastFrame)},
          {"time", time_}};
}

QVariantMap ModelSession::info() const {
  if (!document_)
    return {};
  if (document_->skeletonOnly) {
    const auto &clip = document_->animated()
                           ? document_->animationClips[
                                 static_cast<std::size_t>(
                                     std::clamp(clipIndex_, 0,
                                                static_cast<int>(
                                                    document_->animationClips
                                                        .size()) -
                                                    1))]
                           : ModelAnimationClip{};
    return {{"sha256", document_->hash},
            {"path", path_},
            {"name", document_->name},
            {"coordinates", document_->coordinates},
            {"skeletonOnly", true},
            {"psa", QFileInfo(path_).suffix().compare("psa", Qt::CaseInsensitive) == 0},
            {"referencePath", document_->referencePath},
            {"companionPath", document_->companionPath},
            {"referenceRejected", document_->referenceRejected},
            {"referenceRejectReason", document_->referenceRejectReason},
            {"bones", static_cast<int>(document_->boneNames.size())},
            {"clips", static_cast<int>(document_->animationClips.size())},
            {"clip", clip.name},
            {"clipStart", clip.start},
            {"clipEnd", clip.end},
            {"clipFps", clip.fps}};
  }
  const auto &m = document_->meshes.at(static_cast<std::size_t>(mesh_));
  const qulonglong visiblePoints =
      frames_.model ? frames_.model->sourcePoints : 0;
  const qulonglong visibleTriangles =
      frames_.model ? frames_.model->sourceTriangles : 0;
  return {{"sha256", document_->hash},
          {"visiblePoints", visiblePoints},
          {"visibleTriangles", visibleTriangles},
          {"cachedFrameBytes", QVariant::fromValue<qulonglong>(cachedBytes_)},
          {"cachedFrameCount", static_cast<int>(frameCache_.size())},
          {"coordinates", document_->coordinates},
          {"points", QVariant::fromValue<qulonglong>(document_->points)},
          {"triangles", QVariant::fromValue<qulonglong>(document_->triangles)},
          {"meshPoints", QVariant::fromValue<qulonglong>(m.points.size())},
          {"meshTriangles",
           QVariant::fromValue<qulonglong>(m.triangles.size() / 3)}};
}
QStringList ModelSession::meshes() const {
  QStringList v;
  if (document_)
    for (const auto &m : document_->meshes)
      v.append(m.name);
  return v;
}
QStringList ModelSession::uvSets() const {
  return document_ && !document_->meshes.empty()
             ? document_->meshes.at(static_cast<std::size_t>(mesh_)).uvNames
             : QStringList{};
}
QStringList ModelSession::bindings() const {
  QStringList v;
  if (document_ && !document_->meshes.empty())
    for (const auto &s :
         document_->meshes.at(static_cast<std::size_t>(mesh_)).skins)
      v.append(s.name);
  return v;
}
int ModelSession::bindingIndex() const {
  return document_ && !bindingSelections_.empty()
             ? bindingSelections_.at(static_cast<std::size_t>(mesh_))
             : -1;
}
int ModelSession::uvIndex() const {
  return document_ && !uvSelections_.empty()
             ? uvSelections_.at(static_cast<std::size_t>(mesh_))
             : 0;
}
QStringList ModelSession::bones() const {
  if (!document_ || bindingIndex() < 0)
    return {};
  return document_->meshes.at(static_cast<std::size_t>(mesh_))
      .skins.at(static_cast<std::size_t>(bindingIndex()))
      .bones;
}
void ModelSession::selectTreeIndex(const QModelIndex &index) {
  if (index.model() != &scene_)
    return;
  selectedNode_ = index.data(ModelScene::NodeId).toString();
  emit changed();
  const int mesh = index.data(ModelScene::MeshIndex).toInt();
  if (mesh >= 0) {
    selectMesh(mesh);
    return;
  }
  if (!index.data(ModelScene::Bone).toBool())
    return;
  const auto suffix = " [" + index.data(ModelScene::NodeId).toString() + "]";
  const auto names = bones();
  int selected = -1;
  for (int i = 0; i < names.size(); ++i) {
    if (!names[i].endsWith(suffix))
      continue;
    if (selected >= 0)
      return;
    selected = i;
  }
  if (selected >= 0)
    selectBone(selected);
}
void ModelSession::selectMesh(int i) {
  if (!document_ || i < 0 || i >= static_cast<int>(document_->meshes.size()) ||
      i == mesh_)
    return;
  mesh_ = i;
  selectedNode_ =
      document_->meshes.empty()
          ? QString{}
          : document_->meshes.at(static_cast<std::size_t>(mesh_)).nodeId;
  material_ = 0;
  bone_ = 0;
  updateSkeleton();
  clearSelection();
  rebuild();
  emit changed();
}
void ModelSession::selectBinding(int i) {
  if (!selectedMesh() ||
      static_cast<std::size_t>(mesh_) >= bindingSelections_.size() ||
      i < -1 || i >= bindings().size())
    return;
  bindingSelections_[static_cast<std::size_t>(mesh_)] = i;
  bone_ = 0;
  updateSkeleton();
  clearSelection();
  rebuild();
  emit changed();
}
void ModelSession::selectBone(int i) {
  if (i < 0 || i >= bones().size())
    return;
  bone_ = i;
  const auto &skin = document_->meshes[static_cast<std::size_t>(mesh_)]
                         .skins[static_cast<std::size_t>(bindingIndex())];
  skeletonNode_ = skin.palette[i].toMap().value("nodeId").toString();
  skeletonJoint_ = i;
  selectedNode_ = highlightedNode_ = skeletonNode_;
  rebuild();
  emit changed();
}
void ModelSession::updateSkeleton(bool resetHighlight,ModelSkeleton::Prepared *prepared) {
  if(prepared) skeleton_.replace(std::move(*prepared));
  else skeleton_.replace(document_.get(), mesh_, bindingIndex());
  if (resetHighlight)
    highlightedNode_.clear();
  skeletonNode_.clear();
  skeletonJoint_ = -1;
  if (!resetHighlight && !highlightedNode_.isEmpty()) {
    auto index = skeleton_.indexForNode(highlightedNode_, bone_);
    if (!index.isValid() ||
        index.data(ModelSkeleton::NodeKey).toString() != highlightedNode_)
      index = skeleton_.indexForNode(highlightedNode_);
    if (index.isValid()) {
      skeletonNode_ = highlightedNode_;
      skeletonJoint_ = index.data(ModelSkeleton::JointIndex).toInt();
      return;
    }
  }
  if (!document_ || bindingIndex() < 0)
    return;
  const auto &skin = document_->meshes[static_cast<std::size_t>(mesh_)]
                         .skins[static_cast<std::size_t>(bindingIndex())];
  if (!resetHighlight && document_->nodeById.contains(highlightedNode_)) {
    skeletonNode_ = highlightedNode_;
    selectedNode_ = highlightedNode_;
    skeletonJoint_ = (bone_ >= 0 && bone_ < skin.palette.size() &&
                      skin.palette[bone_].toMap().value("nodeId").toString() ==
                          highlightedNode_)
                         ? bone_
                         : -1;
    return;
  }
  if (bone_ >= 0 && bone_ < skin.palette.size()) {
    skeletonNode_ = skin.palette[bone_].toMap().value("nodeId").toString();
    skeletonJoint_ = bone_;
  }
}
QVariantMap ModelSession::skeletonSelection() const {
  return skeleton_.details(skeletonNode_, skeletonJoint_);
}
void ModelSession::selectSkeletonIndex(const QModelIndex &index) {
  if (index.model() != &skeleton_ || !index.isValid())
    return;
  const int joint = index.data(ModelSkeleton::JointIndex).toInt();
  if (joint >= 0) {
    selectBone(joint);
    return;
  }
  skeletonNode_ = index.data(ModelSkeleton::NodeKey).toString();
  selectedNode_ = highlightedNode_ = skeletonNode_;
  skeletonJoint_ = -1;
  if (mode_ == 2)
    mode_ = 0;
  rebuild();
  emit changed();
}
void ModelSession::selectUV(int i) {
  if (i < 0 || i >= uvSets().size())
    return;
  uvSelections_[static_cast<std::size_t>(mesh_)] = i;
  clearSelection();
  rebuild();
  emit changed();
}
void ModelSession::setMode(int i) {
  if (i < 0 || i > 3 || mode_ == i)
    return;
  mode_ = i;
  clearSelection();
  rebuild();
  emit changed();
}
void ModelSession::setIsolated(bool v) {
  if (isolated_ == v)
    return;
  isolated_ = v;
  rebuild();
  emit changed();
}
void ModelSession::rebuild() {
  if (globalFrameFailure_ && document_) {
    emit changed();
    return;
  }
  ++frameRequest_;
  for (auto &job : frameJobs_)
    job.stop.request_stop();
  if (!document_) {
    frame_.reset();
    frames_ = {};
    preparing_ = pendingFrame_ = false;
    emit frameChanged();
    return;
  }
  if (document_->animated()) {
    // Animated meshes use an immutable pose + current inspection state on
    // the animation worker; invalidate any in-flight view on explicit edits.
    // Native FBX can carry LOD/classification edits: publish readiness only
    // after the worker has landed the requested geometry, as for static FBX.
    preparing_ = document_->nativeFbxAnimated;
    pendingFrame_ = false;
    ++poseRequest_;
    refreshAnimationFrame();
    return;
  }
  preparing_ = pendingFrame_ = true;
  frameError_.clear();
  startFrame();
}
void ModelSession::startFrame() {
  if (!pendingFrame_ || !document_ || frameJobs_.size() >= 2)
    return;
  pendingFrame_ = false;
  auto state = renderState();
  const auto key = stateKey(state);
  const auto cached =
      std::find_if(frameCache_.begin(), frameCache_.end(),
                   [&](const auto &entry) { return entry.key == key; });
  if (cached != frameCache_.end()) {
    frames_ = cached->frames;
    renderedState_ = std::move(state);
    frameCache_.splice(frameCache_.begin(), frameCache_, cached);
    preparing_ = false;
    if (abcActive())
      abcFrameDocument_ = document_;
    frame_ = frames_.mode == 1 ? frames_.uv : frames_.model;
    emit frameChanged();
    emit changed();
    return;
  }
  const auto document = document_;
  const auto revision = ++frameRevision_;
  std::stop_source stop;
  const auto token = stop.get_token();
  auto future =
      std::async(std::launch::async, [document, state = std::move(state),
                                      revision, token] {
        FrameResult r;
        try {
          r.frames = buildModelFrames(*document, state, revision, token);
          r.state = state;
        } catch (const std::exception &e) {
          r.error = QString::fromUtf8(e.what());
        }
        return r;
      });
  frameJobs_.push_back(
      {generation_, frameRequest_, std::move(stop), std::move(future)});
}
QStringList ModelSession::materials() const {
  return document_ && !document_->meshes.empty()
             ? document_->meshes.at(static_cast<std::size_t>(mesh_)).materials
             : QStringList{};
}
QString ModelSession::texturePath() const {
  if (!document_ || document_->meshes.empty() ||
      textureLinks_.size() <= static_cast<std::size_t>(mesh_))
    return {};
  const auto &links = textureLinks_.at(static_cast<std::size_t>(mesh_));
  const auto it = links.find(material_);
  if (it != links.end())
    return it->second.path;
  const auto &mesh = document_->meshes.at(static_cast<std::size_t>(mesh_));
  return material_ >= 0 && material_ < mesh.texturePaths.size()
             ? mesh.texturePaths.at(material_)
             : QString{};
}
bool ModelSession::canClearTexture() const {
  return document_ && !document_->meshes.empty() &&
         textureLinks_.at(static_cast<std::size_t>(mesh_)).contains(material_);
}
QString ModelSession::textureReference() const {
  if (!document_ || document_->meshes.empty() || material_ < 0)
    return {};
  const auto &mesh = document_->meshes.at(static_cast<std::size_t>(mesh_));
  if (material_ >= mesh.references.size())
    return {};
  return mesh.references.at(material_);
}
void ModelSession::selectMaterial(int i) {
  if (!document_ || i < 0 || i >= materials().size() || i == material_)
    return;
  material_ = i;
  if (mode_ == 1 || preparing_) {
    clearSelection();
    rebuild();
  } else if (frameError_.isEmpty())
    renderedState_.material = material_;
  emit changed();
}
void ModelSession::setMaterialOnly(bool value) {
  if (materialOnly_ == value)
    return;
  materialOnly_ = value;
  clearSelection();
  if (mode_ == 1 || preparing_)
    rebuild();
  else if (frameError_.isEmpty())
    renderedState_.materialOnly = materialOnly_;
  emit changed();
}
const ModelMesh *ModelSession::selectedMesh() const {
  if (!document_ || mesh_ < 0 ||
      static_cast<std::size_t>(mesh_) >= document_->meshes.size())
    return nullptr;
  return &document_->meshes[static_cast<std::size_t>(mesh_)];
}
QVariantMap ModelSession::uvBackground() const {
  const auto *mesh = selectedMesh();
  if (!mesh || material_ < 0 || material_ >= mesh->materials.size() ||
      static_cast<std::size_t>(mesh_) >= textureLinks_.size())
    return {};
  const auto &m = *mesh;
  const auto slot = static_cast<std::size_t>(material_);
  const auto &links = textureLinks_.at(static_cast<std::size_t>(mesh_));
  const auto link = links.find(material_);
  const auto uv = link != links.end()    ? link->second.uv
                  : slot < m.materialUv.size() ? m.materialUv[slot] : 0;
  const auto pixels =
      link != links.end() ? link->second.pixels
                         : slot < m.textures.size() ? m.textures[slot] : nullptr;
  const bool validUV = uv >= 0 &&
                       static_cast<std::size_t>(uv) < m.uvs.size() &&
                       static_cast<std::size_t>(uv) < m.invalidUVs.size() &&
                       !m.invalidUVs[static_cast<std::size_t>(uv)];
  return {{"material", m.materials.value(material_)},
          {"uv", m.uvNames.value(uv)},
          {"decoded", bool(pixels)},
          {"matching", uv == uvIndex()},
          {"shown", bool(pixels) && uv == uvIndex() && validUV}};
}
void ModelSession::removeTexture() {
  if (!selectedMesh() ||
      static_cast<std::size_t>(mesh_) >= textureLinks_.size())
    return;
  ++textureRequest_;
  textureBusy_ = false;
  textureLinks_.at(static_cast<std::size_t>(mesh_)).erase(material_);
  textureError_.clear();
  rebuild();
  emit changed();
}
void ModelSession::associateTexture(const QString &input) {
  if (!document_ || busy_)
    return;
  if (uvSets().isEmpty() ||
      document_->meshes.at(static_cast<std::size_t>(mesh_))
          .invalidUVs.at(static_cast<std::size_t>(uvIndex()))) {
    textureError_ = "This mesh has no usable UV set for a texture preview";
    emit changed();
    return;
  }
  if (textureJobs_.size() >= 2) {
    textureError_ = "Texture queue is busy; retry shortly";
    emit changed();
    return;
  }
  for (auto &j : textureJobs_)
    j.stop.request_stop();
  const QUrl url(input);
  const QString path = url.isLocalFile() ? url.toLocalFile() : input;
  std::stop_source stop;
  const auto token = stop.get_token();
  auto future = std::async(std::launch::async, [path, token] {
    TextureResult r;
    r.path = path;
    try {
      r.pixels = readModelTexture(path, token);
    } catch (const std::exception &e) {
      r.error = QString::fromUtf8(e.what());
    }
    return r;
  });
  textureBusy_ = true;
  textureError_.clear();
  textureJobs_.push_back({generation_, ++textureRequest_, mesh_, material_,
                          std::move(stop), std::move(future), uvIndex()});
  emit changed();
}
ModelPickHit pickModelDocument(const ModelDocument &document, const ModelRenderState &state,
    const formaview::render::Frame *pose, const QMatrix4x4 &view, QPointF normalized, bool uvView, std::stop_token stop) {
  bool invertible = false;
  const auto inv = view.inverted(&invertible);
  if (!invertible)
    return {};
  const auto origin =
      inv.map(QVector3D(static_cast<float>(normalized.x()),
                        static_cast<float>(normalized.y()), -1));
  const auto direction =
      (inv.map(QVector3D(static_cast<float>(normalized.x()),
                         static_cast<float>(normalized.y()), 1)) -
       origin)
          .normalized();
  double closest = std::numeric_limits<double>::infinity();
  int hitMesh = -1;
  std::size_t hitTriangle = 0;
  std::uint32_t hitCorner = 0;
  std::size_t hitCornerInTriangle = 0, overlaps = 0;
  float baryU = 0, baryV = 0;
  std::size_t poseOffset = 0;
  for (std::size_t mi = 0; mi < document.meshes.size(); ++mi) {
    const auto meshPoseOffset = poseOffset;
    poseOffset += document.meshes[mi].triangles.size();
    if (!state.visible.empty() && !state.visible[mi])
      continue;
    if ((state.isolated || (state.mode != 0 && state.mode != 3) || uvView) &&
        mi != static_cast<std::size_t>(state.mesh))
      continue;
    const auto &m = document.meshes[mi];
    if (uvView && m.uvs.empty())
      continue;
    for (std::size_t t = 0; t < m.triangles.size(); t += 3) {
      if ((t&4095)==0 && stop.stop_requested()) throw std::runtime_error("Picking cancelled");
      if (state.mode == 1 && state.materialOnly &&
          m.cornerMaterial.at(m.triangles[t]) !=
              static_cast<std::uint32_t>(state.material))
        continue;
      QVector3D p[3];
      for (int i = 0; i < 3; ++i) {
        if (uvView) {
          const auto uv = m.uvs.at(static_cast<std::size_t>(state.uvs.at(mi)))
                              .at(m.triangles[t + static_cast<std::size_t>(i)]);
          p[i] = {static_cast<float>(uv.x() * 2 - 1),
                  static_cast<float>(uv.y() * 2 - 1), 0};
        } else if (pose) {
          const auto &vertex = pose->vertices.at(meshPoseOffset + t + static_cast<std::size_t>(i));
          p[i] = {vertex.x, vertex.y, vertex.z};
        } else
          p[i] =
              (m.points
                   [m.corners[m.triangles[t + static_cast<std::size_t>(i)]]] -
               document.center) /
              document.radius;
      }
      const auto e1 = p[1] - p[0], e2 = p[2] - p[0],
                 h = QVector3D::crossProduct(direction, e2);
      const auto det = QVector3D::dotProduct(e1, h);
      if (std::abs(det) < 1e-8F)
        continue;
      const auto s = origin - p[0];
      const float u = QVector3D::dotProduct(s, h) / det;
      if (u < 0 || u > 1)
        continue;
      const auto q = QVector3D::crossProduct(s, e1);
      const float v = QVector3D::dotProduct(direction, q) / det;
      if (v < 0 || u + v > 1)
        continue;
      const double distance = QVector3D::dotProduct(e2, q) / det;
      if (uvView && distance >= 0)
        ++overlaps;
      if (distance < 0 || distance >= closest)
        continue;
      closest = distance;
      hitMesh = static_cast<int>(mi);
      hitTriangle = t / 3;
      const float b[3] = {1 - u - v, u, v};
      const auto at = static_cast<std::size_t>(std::max_element(b, b + 3) - b);
      hitCorner = m.triangles[t + at];
      hitCornerInTriangle = at;
      baryU = u;
      baryV = v;
    }
  }
  return {hitMesh,hitTriangle,hitCornerInTriangle,overlaps,hitCorner,baryU,baryV};
}
void ModelSession::pick(const QMatrix4x4 &view,QPointF normalized,bool uvView,
                        std::shared_ptr<const formaview::render::Frame> presented) {
  if (!inspectable() || (mode_==2 && bindingIndex()<0)) return;
  // A geometry-cache frame may still show the previous sample.
  if (abcActive() && abcFrameDocument_!=document_) return;
  if ((document_->externallyDriven || document_->nativeAnimated) && stateKey(renderState())!=stateKey(renderedState_)) return;
  if (!uvView && frames_.model && frames_.model->geometry) {
    const auto snapshot=presented?std::move(presented):frames_.model;
    if (snapshot->geometry!=frames_.model->geometry) return;
    ++pickRequest_; if (pickJob_) pickJob_->stop.request_stop();
    pendingPick_=PendingPick{generation_,poseRequest_,pickRequest_,document_,renderedState_,snapshot,view,normalized};
    startGpuPick(); return;
  }
  applyPick(pickModelDocument(*document_,renderedState_,frames_.pose.get(),view,normalized,uvView),uvView);
}
void ModelSession::startGpuPick() {
  if (!pendingPick_ || pickJob_) return;
  auto request=std::move(*pendingPick_); pendingPick_.reset();
  std::stop_source stop;const auto token=stop.get_token();
  pickJob_=PickJob{request.generation,request.poseRequest,request.request,std::move(stop),std::async(std::launch::async,[request,token] {
    const auto pose=buildAnimationFrame(*request.document,static_cast<std::size_t>(request.frame->clip),request.frame->seconds,request.frame->revision,request.document->actorSkinDomain(),&request.frame->worlds,token);
    return pickModelDocument(*request.document,request.state,pose.get(),request.view,request.normalized,false,token);
  })};
}
void ModelSession::finishGpuPick() {
  if (pickJob_ && pickJob_->future.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
    const bool current=pickJob_->generation==generation_ && pickJob_->poseRequest==poseRequest_ && pickJob_->request==pickRequest_ && document_;
    try {const auto hit=pickJob_->future.get();if(current)applyPick(hit,false);}catch(const std::exception &e){if(current)qWarning()<<"GPU snapshot pick:"<<e.what();}
    pickJob_.reset();
  }
  startGpuPick();
}
void ModelSession::applyPick(const ModelPickHit &hit,bool uvView) {
  const auto [hitMesh,hitTriangle,hitCornerInTriangle,overlaps,hitCorner,baryU,baryV]=hit;
  clearSelection();
  if (hitMesh < 0)
    return;
  if (hitMesh != mesh_)
    selectMesh(hitMesh);
  const auto &m = document_->meshes[static_cast<std::size_t>(mesh_)];
  const auto point = m.corners[hitCorner];
  const auto &p = m.raw[point];
  picked_ = {{"point", point},
             {"corner", hitCorner},
             {"triangle", QVariant::fromValue<qulonglong>(hitTriangle)},
             {"x", p.x},
             {"y", p.y},
             {"z", p.z}};
  picked_["meshId"] = m.id;
  picked_["face"] =
      QVariant::fromValue<qulonglong>(m.triangleFaces.at(hitTriangle));
  picked_["faceCorner"] = QVariant::fromValue<qulonglong>(
      m.triangleCorners.at(hitTriangle)[hitCornerInTriangle]);
  if (!m.uvs.empty()) {
    const auto &set = m.uvs.at(static_cast<std::size_t>(uvIndex()));
    const auto source = set.at(hitCorner);
    picked_["u"] = source.x();
    picked_["v"] = source.y();
    picked_["uvSet"] = m.uvNames.at(uvIndex());
    const auto at = hitTriangle * 3;
    const auto interpolated = set[m.triangles[at]] * (1 - baryU - baryV) +
                              set[m.triangles[at + 1]] * baryU +
                              set[m.triangles[at + 2]] * baryV;
    picked_["interpolatedU"] = interpolated.x();
    picked_["interpolatedV"] = interpolated.y();
  }
  if (uvView)
    picked_["overlappingTriangles"] = QVariant::fromValue<qulonglong>(overlaps);
  if (bindingIndex() >= 0) {
    const auto &skin = m.skins.at(static_cast<std::size_t>(bindingIndex()));
    double sum = 0;
    for (const auto &w : skin.weights.at(point)) {
      sum += w.value;
      influences_.append(
          QVariantMap{{"bone", skin.bones.at(w.bone)},
                      {"index", w.bone},
                      {"weight", w.value},
                      {"raw", QString::number(w.value, 'g', 17)}});
    }
    picked_["sum"] = sum;
    picked_["count"] = influences_.size();
  }
  emit selectionChanged();
}
std::pair<QVector3D, float> ModelSession::focusBounds(bool selected) const {
  if (!document_)
    return {{}, 1};
  // Geometry-cache samples share the first sample's display space; framing
  // all keeps that space so motion stays visible instead of being re-centred.
  if (abcActive() && !selected)
    return {{}, 1};
  // Skeleton documents have no selectable mesh: like the CPU path they focus
  // the presented frame bounds below.
  if(selected && gpuGeometry_ && frames_.model && frames_.model->geometry==gpuGeometry_->frames.model &&
     !document_->skeletonOnly && mesh_>=0 && static_cast<std::size_t>(mesh_)<gpuGeometry_->meshBounds.size()) {
    QVector3D low,high;bool valid=false;
    const auto &frame=*frames_.model;
    const auto &boxes=gpuGeometry_->meshBounds.at(static_cast<std::size_t>(mesh_));
    for(std::size_t i=0;i<boxes.size();++i)if(boxes[i].valid) {
      QMatrix4x4 matrix;
      for(int r=0;r<4;++r)for(int c=0;c<4;++c)matrix(r,c)=frame.palette.at(i*16+static_cast<std::size_t>(r*4+c));
      const auto &box=boxes[i];
      for(int mask=0;mask<8;++mask) {
        const auto p=frame.geometry->skin->display.map(matrix.map(QVector3D((mask&1)?box.high.x():box.low.x(),(mask&2)?box.high.y():box.low.y(),(mask&4)?box.high.z():box.low.z())));
        if(!valid){low=high=p;valid=true;}else for(int axis=0;axis<3;++axis){low[axis]=std::min(low[axis],p[axis]);high[axis]=std::max(high[axis],p[axis]);}
      }
    }
    if(valid)return {(low+high)*.5F,std::max((high-low).length()*.5F,.0001F)};
    return {{},1};
  }
  if ((document_->skeletonOnly || (frames_.model && frames_.model->geometry)) && frames_.model && frames_.model->hasDisplayBounds)
    return {(frames_.model->displayLow + frames_.model->displayHigh) * .5F,
            std::max((frames_.model->displayHigh - frames_.model->displayLow)
                             .length() * .5F,
                     .0001F)};
  if (selected && renderedState_.mode == 1 && frames_.model &&
      frames_.model->hasDisplayBounds)
    return {
        (frames_.model->displayLow + frames_.model->displayHigh) * .5F,
        std::max(
            (frames_.model->displayHigh - frames_.model->displayLow).length() *
                .5F,
            .0001F)};
  QVector3D low, high;
  bool first = true;
  const auto visible = visibleMeshes();
  for (std::size_t i = 0; i < document_->meshes.size(); ++i) {
    if ((!visible.empty() && !visible[i]) ||
        (selected && i != static_cast<std::size_t>(mesh_)))
      continue;
    const auto &m = document_->meshes[i];
    if (m.points.empty())
      continue;
    if (first) {
      low = m.boundsLow;
      high = m.boundsHigh;
      first = false;
    } else
      for (int axis = 0; axis < 3; ++axis) {
        low[axis] = std::min(low[axis], m.boundsLow[axis]);
        high[axis] = std::max(high[axis], m.boundsHigh[axis]);
      }
  }
  if (first)
    return {{}, 1};
  return {((low + high) * .5F - document_->center) / document_->radius,
          std::max((high - low).length() * .5F / document_->radius, .0001F)};
}
std::pair<QVector3D, float> ModelSession::uvBounds(bool reference) const {
  if (!document_ || uvSets().isEmpty())
    return {{}, 1};
  const auto &m = document_->meshes.at(static_cast<std::size_t>(mesh_));
  const auto at = static_cast<std::size_t>(uvIndex());
  if (m.invalidUVs.at(at))
    return {{}, 1};
  const auto &b = m.uvBounds.at(at);
  auto low = QVector3D(float(b[0] * 2 - 1), float(b[1] * 2 - 1), 0);
  auto high = QVector3D(float(b[2] * 2 - 1), float(b[3] * 2 - 1), 0);
  if (frames_.uv && frames_.uv->hasDisplayBounds) {
    low = frames_.uv->displayLow;
    high = frames_.uv->displayHigh;
  }
  if (reference)
    for (int axis = 0; axis < 2; ++axis) {
      low[axis] = std::min(low[axis], -1.F);
      high[axis] = std::max(high[axis], 1.F);
    }
  return {(low + high) * .5F, std::max((high - low).length() * .5F, .0001F)};
}

void ModelSession::requestAbcSample(std::size_t index) {
  if (!abcActive())
    return;
  abcRequested_ = std::min(index, abcSource_->times().size() - 1);
  abcError_.clear();
  scheduleAbc();
  emit animChanged();
}
void ModelSession::seekSample(int index) {
  if (!abcActive())
    return;
  const auto &times = abcSource_->times();
  const auto clamped = static_cast<std::size_t>(
      std::clamp(index, 0, static_cast<int>(times.size()) - 1));
  time_ = times[clamped];
  requestAbcSample(clamped);
}
void ModelSession::cancelSample() {
  if (!abcActive())
    return;
  if (abcJob_)
    abcJob_->stop.request_stop();
  playing_ = false;
  animTimer_.stop();
  // Keep the sample whose frame is on screen; a newer document that has no
  // frame yet is dropped back to it.
  if (abcFrameDocument_ && abcFrameDocument_ != document_)
    document_ = abcFrameDocument_;
  abcRequested_ = document_->cacheSample;
  time_ = abcSource_->times().at(abcRequested_);
  emit animChanged();
  emit changed();
}
void ModelSession::startAbcJob(std::size_t index, bool prefetch) {
  const auto source = abcSource_;
  const auto display = abcDisplay_;
  // The nearest cached sample shares the most unchanged meshes/topology.
  std::shared_ptr<const ModelDocument> reference = abcCache_.nearest(index);
  if (!reference)
    reference = document_;
  std::stop_source stop;
  const auto token = stop.get_token();
  abcJob_ = AbcJob{
      generation_, index, prefetch, std::move(stop),
      std::async(std::launch::async, [source, index, display, reference, token] {
        AbcResult r;
        try {
          r.document = source->read(index, token, display, reference);
        } catch (const skeletal::Failure &e) {
          r.cancelled = e.error.code == ErrorCode::Cancelled;
          r.error = QString::fromUtf8(e.what());
        } catch (const std::exception &e) {
          r.error = QString::fromUtf8(e.what());
        }
        return r;
      })};
}
void ModelSession::finishAbcJob() {
  if (!abcJob_ || abcJob_->future.wait_for(std::chrono::seconds(0)) !=
                      std::future_status::ready)
    return;
  auto job = std::move(*abcJob_);
  abcJob_.reset();
  auto r = job.future.get();
  // Results of a previous archive (or after a close) are dropped entirely.
  if (job.generation != generation_ || !abcActive() ||
      (r.document && r.document->geometryCache != abcSource_)) {
    ++abcDiscarded_;
    scheduleAbc(); // The current archive may be waiting for this worker.
    return;
  }
  if (r.document) {
    abcCache_.insert(r.document);
    if (job.prefetch)
      ++abcPrefetched_;
    else if (job.index != abcRequested_)
      ++abcDiscarded_; // Superseded: cached for reuse, never presented.
  } else if (!r.cancelled && !job.prefetch && job.index == abcRequested_) {
    abcError_ = r.error;
    playing_ = false;
    animTimer_.stop();
    abcRequested_ = document_->cacheSample;
  }
  scheduleAbc();
  emit animChanged();
}
void ModelSession::scheduleAbc() {
  if (!abcActive() || busy_)
    return;
  if (abcRequested_ != document_->cacheSample) {
    if (abcCache_.contains(abcRequested_)) {
      // During playback keep the presented sample until its frame is ready,
      // so frame building is never starved by back-to-back landings.
      if (!(playing_ && preparing_))
        landAbcSample(abcCache_.find(abcRequested_));
      return;
    }
    if (!abcJob_)
      startAbcJob(abcRequested_, false);
    else if (abcJob_->prefetch && abcJob_->index != abcRequested_)
      abcJob_->stop.request_stop();
    return;
  }
  // Limited prefetch while playing: the next two samples in play order.
  if (!playing_ || abcJob_ || !abcError_.isEmpty())
    return;
  const auto count = static_cast<long long>(abcSource_->times().size());
  for (long long step = 1; step <= 2; ++step) {
    auto next = static_cast<long long>(abcRequested_) + step * abcDirection_;
    if (loop_)
      next = ((next % count) + count) % count;
    if (next < 0 || next >= count)
      break;
    if (!abcCache_.contains(static_cast<std::size_t>(next))) {
      startAbcJob(static_cast<std::size_t>(next), true);
      break;
    }
  }
}
void ModelSession::landAbcSample(std::shared_ptr<ModelDocument> document) {
  if (!document || document == document_)
    return;
  document_ = std::move(document);
  // Same archive, same mesh list: selections and view state carry over.
  // Frames stay cached: their keys include the sample, so revisiting a
  // recent sample with the same view state reuses its built frame.
  globalFrameFailure_ = false;
  clearSelection();
  rebuild();
  emit changed();
  emit animChanged();
}
void ModelSession::tickAbc() {
  const auto &times = abcSource_->times();
  if (!playing_ || times.size() < 2) {
    animTimer_.stop();
    playing_ = false;
    emit animChanged();
    return;
  }
  const double dt = static_cast<double>(animClock_.restart()) / 1000.0;
  const double start = times.front(), end = times.back();
  time_ += dt * speed_;
  if (!std::isfinite(time_))
    time_ = start;
  if (time_ > end) {
    if (loop_) {
      time_ = start + std::fmod(time_ - start, std::max(1e-9, end - start));
    } else {
      time_ = end;
      playing_ = false;
      animTimer_.stop();
    }
  }
  abcDirection_ = 1;
  const auto index = abcSource_->nearest(time_);
  if (index != abcRequested_)
    requestAbcSample(index);
  else
    scheduleAbc();
  emit animChanged();
}
QVariantMap ModelSession::cacheInfo() const {
  if (!abcActive() || !abcSource_)
    return {};
  const auto &times = abcSource_->times();
  const auto &infos = abcSource_->meshes();
  const auto shown = document_->cacheSample;
  QVariantList meshes;
  bool uv = false, normals = false;
  for (std::size_t i = 0; i < infos.size(); ++i) {
    const auto &info = infos[i];
    uv = uv || info.uv;
    normals = normals || info.normals;
    QVariantMap entry{
        {"path", QString::fromStdString(info.path)},
        {"topology", info.constant ? QStringLiteral("static")
                     : info.topology == formaview::Topology::Varying
                         ? QStringLiteral("varying")
                         : QStringLiteral("fixed")},
        {"samples", static_cast<int>(info.times.size())},
        {"uv", info.uv},
        {"normals", info.normals}};
    if (i < document_->cacheMeshSamples.size())
      entry["sample"] = static_cast<int>(document_->cacheMeshSamples[i]);
    if (i < document_->meshes.size()) {
      const auto &m = document_->meshes[i];
      entry["points"] = QVariant::fromValue<qulonglong>(m.points.size());
      entry["corners"] = QVariant::fromValue<qulonglong>(m.corners.size());
      entry["triangles"] = QVariant::fromValue<qulonglong>(m.triangles.size() / 3);
    }
    meshes.append(entry);
  }
  const bool loading =
      abcRequested_ != shown || (abcJob_ && !abcJob_->prefetch);
  return {{"active", true},
          {"samples", static_cast<int>(times.size())},
          {"sample", static_cast<int>(shown)},
          {"requested", static_cast<int>(abcRequested_)},
          {"time", times.at(shown)},
          {"clock", time_},
          {"start", times.front()},
          {"end", times.back()},
          {"loading", loading},
          {"framed", abcFrameDocument_ == document_},
          {"points", QVariant::fromValue<qulonglong>(document_->points)},
          {"faces", QVariant::fromValue<qulonglong>(document_->cacheFaces)},
          {"corners", QVariant::fromValue<qulonglong>(document_->cacheCorners)},
          {"triangles", QVariant::fromValue<qulonglong>(document_->triangles)},
          {"uv", uv},
          {"normals", normals},
          {"meshes", meshes},
          {"skipped", static_cast<int>(abcSource_->skipped().size())},
          {"cachedSamples", static_cast<int>(abcCache_.size())},
          {"cachedBytes", QVariant::fromValue<qulonglong>(abcCache_.bytes())},
          {"cacheLimitBytes", QVariant::fromValue<qulonglong>(abcCache_.maxBytes())},
          {"cacheLimitSamples", static_cast<int>(abcCache_.maxSamples())},
          {"cacheHits", QVariant::fromValue<qulonglong>(abcCache_.hits())},
          {"cacheEvictions", QVariant::fromValue<qulonglong>(abcCache_.evictions())},
          {"reads", QVariant::fromValue<qulonglong>(abcSource_->reads())},
          {"meshDecodes", QVariant::fromValue<qulonglong>(abcSource_->meshDecodes())},
          {"meshReuses", QVariant::fromValue<qulonglong>(abcSource_->meshReuses())},
          {"topologyReuses", QVariant::fromValue<qulonglong>(abcSource_->topologyReuses())},
          {"prefetched", QVariant::fromValue<qulonglong>(abcPrefetched_)},
          {"discarded", QVariant::fromValue<qulonglong>(abcDiscarded_)},
          {"error", abcError_}};
}
