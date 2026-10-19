#include "model_session.hpp"
#include "formaview/fbx.hpp"
#include "formaview/file_access.hpp"
#include "formaview/preview.hpp"
#include "model_document.hpp"
#include "model_metadata.hpp"
#include "model_texture.hpp"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>
#include <QVector3D>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace {
using namespace formaview;
using Mesh = ModelMesh;
using Skin = ModelSkin;
QString text(const std::string &s) { return QString::fromStdString(s); }
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
} // namespace
std::shared_ptr<ModelDocument> readModelDocument(const QString &path,
                                                 std::stop_token stop) {
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
    const auto file = suffix == "obj"
                          ? skeletal::FbxDocument::read_obj(source, options)
                          : skeletal::FbxDocument::read(source, options);
    const auto &data = file.data();
    if (suffix == "fbx" && data.meshes.empty() && !data.clips.empty())
      throw skeletal::Failure(
          ErrorCode::Unsupported,
          "ANIMATION_ONLY_FBX: This FBX contains animation without mesh "
          "geometry. "
          "Animation-only and external animation preview are planned for M07.");
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
           node.parent ? QString("node/%1").arg(*node.parent) : QString{}},
          {"bone", node.bone},
          {"local", modelMatrixValues(node.default_local)},
          {"world", modelMatrixValues(node.default_world)},
          {"geometryWorld", modelMatrixValues(node.geometry_world)},
          {"active", true}});
    }
    switch (data.source.coordinates.up) {
    case Axis::PositiveZ:
      d->displayBasis.rotate(-90, 1, 0, 0);
      break;
    case Axis::NegativeZ:
      d->displayBasis.rotate(90, 1, 0, 0);
      break;
    case Axis::PositiveX:
      d->displayBasis.rotate(90, 0, 0, 1);
      break;
    case Axis::NegativeX:
      d->displayBasis.rotate(-90, 0, 0, 1);
      break;
    case Axis::NegativeY:
      d->displayBasis.rotate(180, 1, 0, 0);
      break;
    default:
      break;
    }
    d->hash = text(data.source.identity.sha256);
    d->coordinates =
        suffix == "obj"
            ? QString("OBJ · source axes and units unspecified · display Y up")
            : QString("FBX · source axes preserved · %1 m/unit")
                  .arg(data.metres_per_unit, 0, 'g', 8);
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
    for (const auto &textureLease : textureLeases)
      textureLease.require_unchanged(textureLease.snapshot());
  } else if (suffix == "psk" || suffix == "pskx") {
    const auto file = skeletal::read_actor(source, options);
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
  } else
    throw skeletal::Failure(ErrorCode::Unsupported,
                            "This model format is not connected yet");
  QVector3D low(std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max()),
      high = -low;
  std::size_t renderCorners = 0;
  d->name = QFileInfo(path).fileName();
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
  if (!d->triangles)
    throw skeletal::Failure(ErrorCode::InvalidArgument,
                            "No visible triangles in model");
  d->center = (low + high) * .5F;
  d->radius = std::max((high - low).length() * .5F, 1e-6F);
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
  connect(&timer_, &QTimer::timeout, this, &ModelSession::poll);
  timer_.start();
}
ModelSession::~ModelSession() {
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
  ++generation_;
  ++frameRequest_;
  renderedState_ = {};
  frameCache_.clear();
  cachedBytes_ = 0;
  globalFrameFailure_ = false;
  pendingFrame_ = preparing_ = false;
  frameError_.clear();
  frames_ = {};
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
  busy_ = false;
  path_.clear();
  selectedNode_.clear();
  error_.clear();
  clearSelection();
  emit frameChanged();
  emit changed();
  emit documentChanged();
}
void ModelSession::cancel() {
  if ((!preparing_ && frameError_.isEmpty()) || !frames_.model) {
    clear();
    return;
  }
  ++frameRequest_;
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
  selectedNode_ = document_->meshes.at(static_cast<std::size_t>(mesh_)).nodeId;
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
    open(path);
}
void ModelSession::open(const QString &p) {
  clear();
  path_ = p;
  pending_ = p;
  busy_ = true;
  startPending();
  emit changed();
}
void ModelSession::startPending() {
  if (pending_.isEmpty() || jobs_.size() >= 2)
    return;
  const auto p = std::exchange(pending_, {});
  std::stop_source stop;
  const auto token = stop.get_token();
  auto future = std::async(std::launch::async, [p, token] {
    Result r;
    try {
      r.document = readModelDocument(p, token);
    } catch (const std::exception &e) {
      r.error = QString::fromUtf8(e.what());
    }
    return r;
  });
  jobs_.push_back({generation_, std::move(stop), std::move(future)});
}
void ModelSession::poll() {
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
      scene_.replace(document_.get());
      mesh_ = bone_ = mode_ = 0;
      material_ = 0;
      isolated_ = false;
      bindingSelections_.clear();
      uvSelections_.clear();
      textureLinks_.clear();
      classes_.clear();
      levels_.clear();
      if (document_)
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
      chooseVisibleMesh();
      if (document_)
        selectedNode_ =
            document_->meshes.at(static_cast<std::size_t>(mesh_)).nodeId;
      updateSkeleton();
      rebuild();
      emit changed();
      emit documentChanged();
    }
    it = jobs_.erase(it);
  }
  startPending();
  startFrame();
}
QVariantMap ModelSession::info() const {
  if (!document_)
    return {};
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
  return document_
             ? document_->meshes.at(static_cast<std::size_t>(mesh_)).uvNames
             : QStringList{};
}
QStringList ModelSession::bindings() const {
  QStringList v;
  if (document_)
    for (const auto &s :
         document_->meshes.at(static_cast<std::size_t>(mesh_)).skins)
      v.append(s.name);
  return v;
}
int ModelSession::bindingIndex() const {
  return document_ ? bindingSelections_.at(static_cast<std::size_t>(mesh_))
                   : -1;
}
int ModelSession::uvIndex() const {
  return document_ ? uvSelections_.at(static_cast<std::size_t>(mesh_)) : 0;
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
  selectedNode_ = document_->meshes.at(static_cast<std::size_t>(mesh_)).nodeId;
  material_ = 0;
  bone_ = 0;
  updateSkeleton();
  clearSelection();
  rebuild();
  emit changed();
}
void ModelSession::selectBinding(int i) {
  if (!document_ || i < -1 || i >= bindings().size())
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
void ModelSession::updateSkeleton(bool resetHighlight) {
  skeleton_.replace(document_.get(), mesh_, bindingIndex());
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
  return document_
             ? document_->meshes.at(static_cast<std::size_t>(mesh_)).materials
             : QStringList{};
}
QString ModelSession::texturePath() const {
  if (!document_)
    return {};
  const auto &links = textureLinks_.at(static_cast<std::size_t>(mesh_));
  const auto it = links.find(material_);
  return it == links.end()
             ? document_->meshes.at(static_cast<std::size_t>(mesh_))
                   .texturePaths.at(material_)
             : it->second.path;
}
bool ModelSession::canClearTexture() const {
  return document_ &&
         textureLinks_.at(static_cast<std::size_t>(mesh_)).contains(material_);
}
QString ModelSession::textureReference() const {
  return document_ ? document_->meshes.at(static_cast<std::size_t>(mesh_))
                         .references.at(material_)
                   : QString{};
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
QVariantMap ModelSession::uvBackground() const {
  if (!document_)
    return {};
  const auto &m = document_->meshes.at(static_cast<std::size_t>(mesh_));
  const auto slot = static_cast<std::size_t>(material_);
  const auto &links = textureLinks_.at(static_cast<std::size_t>(mesh_));
  const auto link = links.find(material_);
  const auto uv = link != links.end()    ? link->second.uv
                  : m.materialUv.empty() ? 0
                                         : m.materialUv.at(slot);
  const auto pixels =
      link != links.end() ? link->second.pixels : m.textures.at(slot);
  return {{"material", m.materials.value(material_)},
          {"uv", m.uvNames.value(uv)},
          {"decoded", bool(pixels)},
          {"matching", uv == uvIndex()},
          {"shown", bool(pixels) && uv == uvIndex() && !m.uvs.empty() &&
                        !m.invalidUVs.at(static_cast<std::size_t>(uv))}};
}
void ModelSession::removeTexture() {
  if (!document_)
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
void ModelSession::pick(const QMatrix4x4 &view, QPointF normalized,
                        bool uvView) {
  if (!inspectable() || (mode_ == 2 && bindingIndex() < 0))
    return;
  bool invertible = false;
  const auto inv = view.inverted(&invertible);
  if (!invertible)
    return;
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
  for (std::size_t mi = 0; mi < document_->meshes.size(); ++mi) {
    if (!renderedState_.visible.empty() && !renderedState_.visible[mi])
      continue;
    if ((isolated_ || (mode_ != 0 && mode_ != 3) || uvView) &&
        mi != static_cast<std::size_t>(mesh_))
      continue;
    const auto &m = document_->meshes[mi];
    if (uvView && m.uvs.empty())
      continue;
    for (std::size_t t = 0; t < m.triangles.size(); t += 3) {
      if (mode_ == 1 && materialOnly_ &&
          m.cornerMaterial.at(m.triangles[t]) !=
              static_cast<std::uint32_t>(material_))
        continue;
      QVector3D p[3];
      for (int i = 0; i < 3; ++i) {
        if (uvView) {
          const auto uv = m.uvs.at(static_cast<std::size_t>(uvIndex()))
                              .at(m.triangles[t + static_cast<std::size_t>(i)]);
          p[i] = {static_cast<float>(uv.x() * 2 - 1),
                  static_cast<float>(uv.y() * 2 - 1), 0};
        } else
          p[i] =
              (m.points
                   [m.corners[m.triangles[t + static_cast<std::size_t>(i)]]] -
               document_->center) /
              document_->radius;
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
