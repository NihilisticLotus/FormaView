#include "scene_sdk.hpp"
#include <QRegularExpression>
#include <QStringDecoder>
#include <QSet>
#include <cmath>
#include <vector>

namespace formaview::scene_export {
namespace {
QString decoded(const QByteArray &bytes) {
  QStringDecoder decoder(QStringDecoder::Utf8);
  QString text = decoder.decode(bytes);
  if (decoder.hasError())
    invalid("OBJ/MTL text must be UTF-8");
  text.replace("\\\r\n", " ").replace("\\\n", " ");
  return text;
}
double number(const QString &text) {
  bool ok{};
  const auto value = text.toDouble(&ok);
  if (!ok || !std::isfinite(value))
    invalid("Invalid OBJ/MTL number");
  return value;
}
int index(const QString &text, size_t count) {
  bool ok{};
  const auto value = text.toLongLong(&ok);
  const auto resolved =
      value < 0 ? static_cast<qint64>(count) + value : value - 1;
  if (!ok || value == 0 || resolved < 0 ||
      resolved >= static_cast<qint64>(count))
    invalid("OBJ index outside source array");
  return static_cast<int>(resolved);
}
QStringList fields(const QString &line) {
  return line.section('#', 0, 0).trimmed().split(QRegularExpression("\\s+"),
                                                 Qt::SkipEmptyParts);
}
struct Corner {
  int point{}, uv{-1}, normal{-1};
};
struct Face {
  std::vector<Corner> corners;
  QString material;
  int smoothing{};
};
bool materials(Scene &s, const QString &uri,
               QMap<QString, FbxSurfacePhong *> &out, QSet<QString> &loaded,
               bool reportMissing = true) {
  const auto path = s.relative(uri);
  const auto key = QString::fromStdWString(path.wstring()).toCaseFolded();
  if (loaded.contains(key)) return true;
  QByteArray bytes;
  try { bytes = s.load(path); }
  catch (const skeletal::Failure &error) {
    if (error.error.code != ErrorCode::Io && error.error.code != ErrorCode::OutOfScope) throw;
    if (reportMissing) s.loss("obj_missing_mtl", uri + ": " + QString::fromUtf8(error.what()));
    return false;
  }
  s.reserve(static_cast<std::uint64_t>(bytes.size()) * 4);
  FbxSurfacePhong *material{};
  for (const auto &line : decoded(bytes).split('\n')) {
    s.check();
    const auto f = fields(line);
    if (f.isEmpty())
      continue;
    const auto command = f[0];
    if (command == "newmtl") {
      const auto name = f.mid(1).join(' ');
      if (name.isEmpty() || out.contains(name))
        invalid("Empty or duplicate MTL material");
      material = FbxSurfacePhong::Create(s.scene, name.toUtf8().constData());
      out.insert(name, material);
    } else if (!material)
      invalid("MTL property precedes material");
    else if (command == "Kd" || command == "Ka" || command == "Ks" ||
             command == "Ke") {
      if (f.size() != 4)
        invalid("MTL colour requires three components");
      const FbxDouble3 value(number(f[1]), number(f[2]), number(f[3]));
      if (command == "Kd")
        material->Diffuse.Set(value);
      if (command == "Ka")
        material->Ambient.Set(value);
      if (command == "Ks")
        material->Specular.Set(value);
      if (command == "Ke")
        material->Emissive.Set(value);
    } else if (command == "Ns" || command == "d" || command == "Tr") {
      if (f.size() != 2)
        invalid("MTL scalar requires one value");
      if (command == "Ns")
        material->Shininess.Set(number(f[1]));
      else
        material->TransparencyFactor.Set(command == "d" ? 1 - number(f[1])
                                                        : number(f[1]));
    } else if (command == "map_Kd" || command == "map_Ks" ||
               command == "map_Ke" || command == "map_d" ||
               command == "map_Bump" || command == "bump") {
      if (f.size() < 2)
        invalid("MTL texture requires a filename");
      if (f[1].startsWith('-')) {
        s.loss("obj_texture_options", line.trimmed());
        continue;
      }
      const auto name = f.mid(1).join(' ');
      const auto texture_path =
          (path.parent_path() / std::filesystem::path(name.toStdWString()))
              .lexically_normal();
      try {
        const auto content = s.load(texture_path);
        const auto media_name =
            QString::fromStdWString(texture_path.wstring()).replace('\\', '/');
        auto *texture = s.texture(s.image(media_name, content));
        if (command == "map_Kd")
          material->Diffuse.ConnectSrcObject(texture);
        else if (command == "map_Ks")
          material->Specular.ConnectSrcObject(texture);
        else if (command == "map_Ke")
          material->Emissive.ConnectSrcObject(texture);
        else if (command == "map_d")
          material->TransparentColor.ConnectSrcObject(texture);
        else
          material->Bump.ConnectSrcObject(texture);
      } catch (const skeletal::Failure &error) {
        if (error.error.code == ErrorCode::Cancelled ||
            error.error.code == ErrorCode::ResourceLimit)
          throw;
        s.loss("external_texture_unavailable",
               name + ": " + QString::fromUtf8(error.what()));
      }
    } else if (command != "illum")
      s.loss("obj_material_property", command);
  }
  loaded.insert(key);
  return true;
}
} // namespace
void read_obj(Scene &s, const QByteArray &bytes) {
  s.reserve(static_cast<std::uint64_t>(bytes.size()) * 4);
  std::vector<FbxVector4> points, normals;
  std::vector<FbxVector2> uvs;
  std::vector<Face> faces;
  std::vector<FbxColor> colours;
  QMap<QString, FbxSurfacePhong *> material_map;
  QSet<QString> loaded_materials;
  QString material;
  int smoothing{};
  std::uint64_t elements{};
  for (const auto &line : decoded(bytes).split('\n')) {
    s.check();
    const auto f = fields(line);
    if (f.isEmpty())
      continue;
    if (elements > s.limits.elements)
      throw skeletal::Failure(ErrorCode::ResourceLimit,
                              "OBJ element limit exceeded");
    const auto command = f[0];
    if (command == "v" || command == "vn") {
      if (f.size() < 4)
        invalid("OBJ vertex requires three coordinates");
      FbxVector4 value(number(f[1]), number(f[2]), number(f[3]));
      if (command == "v") {
        if (f.size() == 5) {
          const auto w = number(f[4]);
          if (!w)
            invalid("OBJ vertex has zero homogeneous weight");
          value /= w;
          if (!std::isfinite(value[0]) || !std::isfinite(value[1]) ||
              !std::isfinite(value[2]))
            invalid("OBJ homogeneous vertex normalization overflow");
        } else if (f.size() != 4 && f.size() != 7)
          invalid("Unsupported OBJ vertex layout");
        points.push_back(value);
        colours.push_back(
            f.size() == 7 ? FbxColor(number(f[4]), number(f[5]), number(f[6]))
                          : FbxColor(1, 1, 1));
      } else {
        if (f.size() != 4)
          invalid("Invalid OBJ normal");
        normals.push_back(value);
      }
      s.reserve(128);
      ++elements;
    } else if (command == "vt") {
      if (f.size() < 2 || f.size() > 4)
        invalid("Invalid OBJ texture coordinate");
      uvs.emplace_back(number(f[1]), f.size() > 2 ? number(f[2]) : 0);
      if (f.size() == 4 && number(f[3]) != 0)
        s.loss("obj_uv_third_component",
               "FBX UV sets retain the first two components.");
      s.reserve(32);
      ++elements;
    } else if (command == "f") {
      if (f.size() < 4)
        invalid("OBJ polygon requires at least three corners");
      Face face;
      face.material = material;
      face.smoothing = smoothing;
      for (qsizetype i = 1; i < f.size(); ++i) {
        const auto parts = f[i].split('/');
        if (parts.size() > 3)
          invalid("Invalid OBJ face corner");
        Corner c;
        c.point = index(parts[0], points.size());
        if (parts.size() > 1 && !parts[1].isEmpty())
          c.uv = index(parts[1], uvs.size());
        if (parts.size() > 2 && !parts[2].isEmpty())
          c.normal = index(parts[2], normals.size());
        face.corners.push_back(c);
        ++elements;
        s.reserve(96);
      }
      faces.push_back(std::move(face));
    } else if (command == "usemtl")
      material = f.mid(1).join(' ');
    else if (command == "mtllib") {
      const auto whole = line.section('#', 0, 0).trimmed().mid(command.size()).trimmed();
      if (f.size() <= 2)
        materials(s, whole, material_map, loaded_materials);
      else if (!materials(s, whole, material_map, loaded_materials, false))
        for (qsizetype i = 1; i < f.size(); ++i)
          materials(s, f[i], material_map, loaded_materials);
    } else if (command == "s") {
      if (f.size() != 2)
        invalid("Invalid OBJ smoothing group");
      bool ok{};
      smoothing = (f[1] == "off" || f[1] == "0") ? 0
                  : f[1] == "on"                 ? 1
                                                 : f[1].toInt(&ok);
      if (smoothing < 0 || (smoothing == 0 && f[1] != "off" && f[1] != "0"))
        invalid("Invalid OBJ smoothing group");
      if (smoothing > 31)
        s.loss("obj_smoothing_groups",
               "Smoothing group IDs above 31 cannot be represented as FBX bit "
               "masks; explicit normals are retained.");
    } else if (command == "o" || command == "g")
      s.loss("obj_grouping",
             "OBJ object/group labels are flattened into one mesh; polygons, "
             "material assignments and corner attributes are retained.");
    else
      throw skeletal::Failure(ErrorCode::Unsupported,
                              "Unsupported OBJ geometry record: " +
                                  command.toStdString());
  }
  if (points.empty() || faces.empty())
    invalid("OBJ requires polygon geometry");
  auto *node = FbxNode::Create(
      s.scene,
      QString::fromStdWString(s.path.stem().wstring()).toUtf8().constData());
  s.scene->GetRootNode()->AddChild(node);
  auto *mesh = FbxMesh::Create(s.scene, "OBJ geometry");
  node->SetNodeAttribute(mesh);
  mesh->InitControlPoints(static_cast<int>(points.size()));
  for (size_t i = 0; i < points.size(); ++i)
    mesh->SetControlPointAt(points[i], static_cast<int>(i));
  auto *uv = mesh->CreateElementUV("UV0");
  uv->SetMappingMode(FbxLayerElement::eByPolygonVertex);
  uv->SetReferenceMode(FbxLayerElement::eDirect);
  auto *normal = mesh->CreateElementNormal();
  normal->SetMappingMode(FbxLayerElement::eByPolygonVertex);
  normal->SetReferenceMode(FbxLayerElement::eDirect);
  auto *colour = mesh->CreateElementVertexColor();
  colour->SetMappingMode(FbxLayerElement::eByControlPoint);
  colour->SetReferenceMode(FbxLayerElement::eDirect);
  for (const auto &v : colours)
    colour->GetDirectArray().Add(v);
  auto *mats = mesh->CreateElementMaterial();
  mats->SetMappingMode(FbxLayerElement::eByPolygon);
  mats->SetReferenceMode(FbxLayerElement::eIndexToDirect);
  auto *smooth = mesh->CreateElementSmoothing();
  smooth->SetMappingMode(FbxLayerElement::eByPolygon);
  smooth->SetReferenceMode(FbxLayerElement::eDirect);
  QMap<QString, int> indices;
  bool missing_normals{}, missing_uv{};
  for (const auto &face : faces) {
    s.check();
    if (!indices.contains(face.material)) {
      if (!material_map.contains(face.material)) {
        material_map[face.material] = FbxSurfacePhong::Create(
            s.scene, face.material.toUtf8().constData());
        if (!face.material.isEmpty())
          s.loss("obj_missing_material", face.material);
      }
      indices[face.material] = node->AddMaterial(material_map[face.material]);
    }
    mesh->BeginPolygon(indices[face.material]);
    for (const auto &c : face.corners) {
      mesh->AddPolygon(c.point);
      uv->GetDirectArray().Add(c.uv < 0 ? FbxVector2()
                                        : uvs[static_cast<size_t>(c.uv)]);
      normal->GetDirectArray().Add(
          c.normal < 0 ? FbxVector4() : normals[static_cast<size_t>(c.normal)]);
      missing_normals |= c.normal < 0;
      missing_uv |= c.uv < 0;
    }
    smooth->GetDirectArray().Add(face.smoothing > 0 && face.smoothing <= 31
                                     ? 1 << (face.smoothing - 1)
                                     : 0);
    mesh->EndPolygon();
  }
  if (normals.empty()) {
    mesh->RemoveElementNormal(normal);
    mesh->GenerateNormals();
  } else if (missing_normals)
    s.loss("obj_missing_corner_normals",
           "Missing corner normals are zero; existing source normals are "
           "preserved.");
  if (uvs.empty())
    mesh->RemoveElementUV(uv);
  else if (missing_uv)
    s.loss("obj_missing_corner_uv",
           "Missing corner UVs use (0,0); existing source UVs are preserved.");
  s.scene->GetGlobalSettings().SetAxisSystem(FbxAxisSystem::MayaYUp);
  s.scene->GetGlobalSettings().SetSystemUnit(FbxSystemUnit::m);
  s.loss(
      "obj_coordinate_assumption",
      "OBJ declares no axes or units. Numeric coordinates are preserved and "
      "tagged as right-handed Y-up metres before optional output conversion.");
}
} // namespace formaview::scene_export
