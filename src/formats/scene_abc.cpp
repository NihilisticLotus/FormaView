// Alembic geometry cache -> FBX scene for one explicit timeline sample.
// The archive is read from the exact leased bytes, so the exported frame is
// the content identified by the input snapshot hash.
#include "scene_sdk.hpp"
#include "formaview/geometry_cache.hpp"
#include <QJsonObject>
#include <cmath>
#include <memory>
#include <set>

namespace formaview::scene_export {
namespace {
QString leaf(const std::string &path) {
  const auto full = QString::fromStdString(path);
  const auto name = full.mid(full.lastIndexOf('/') + 1);
  return name.isEmpty() ? QStringLiteral("mesh") : name;
}
template <class T>
const T *scoped(const std::vector<T> &values, int scope, std::size_t corner,
                std::size_t face, std::size_t point) {
  std::size_t at = 0;
  switch (static_cast<cache::Scope>(scope)) {
  case cache::Scope::Constant:
    at = 0;
    break;
  case cache::Scope::Uniform:
    at = face;
    break;
  case cache::Scope::Varying:
  case cache::Scope::Vertex:
    at = point;
    break;
  case cache::Scope::FaceVarying:
    at = corner;
    break;
  default:
    return nullptr;
  }
  return at < values.size() ? &values[at] : nullptr;
}
FbxVector4 transform_point(const Matrix4 &m, const skeletal::Vec3 &v) {
  return FbxVector4(m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3],
                    m[4] * v.x + m[5] * v.y + m[6] * v.z + m[7],
                    m[8] * v.x + m[9] * v.y + m[10] * v.z + m[11]);
}
} // namespace
void read_abc(Scene &s, const QByteArray &bytes, const QJsonObject &options,
              bool inventory_only) {
  auto owner = std::make_shared<QByteArray>(bytes);
  cache::Limits limits;
  limits.input_bytes = s.limits.input_bytes;
  const auto archive = cache::Archive::open(
      std::span<const char>(owner->constData(), static_cast<std::size_t>(owner->size())),
      owner, limits, s.stop);
  const auto &times = archive.times();
  const auto &infos = archive.meshes();
  QJsonObject abc{{"samples", static_cast<qint64>(times.size())},
                  {"start", times.front()},
                  {"end", times.back()},
                  {"meshes", static_cast<qint64>(infos.size())},
                  {"skipped", static_cast<qint64>(archive.skipped().size())}};
  s.declared_mesh = !infos.empty();
  if (inventory_only) {
    s.metadata["abc"] = abc;
    return;
  }
  const auto requested = options.value("abc_sample");
  if (!requested.isDouble() || requested.toDouble() < 0 ||
      requested.toDouble() != std::floor(requested.toDouble()))
    invalid("ABC export requires a non-negative integer abc_sample");
  if (requested.toDouble() >= static_cast<double>(times.size()))
    invalid("ABC sample index outside the timeline");
  const auto index = static_cast<std::size_t>(requested.toDouble());
  const double seconds = times[index];
  if (options.contains("abc_seconds")) {
    const auto expected = options["abc_seconds"].toDouble(
        std::numeric_limits<double>::quiet_NaN());
    if (!(std::abs(expected - seconds) <= 1e-9 * std::max(1.0, std::abs(seconds))))
      throw skeletal::Failure(ErrorCode::InputChanged,
                              "ABC timeline changed since the previewed frame");
  }
  abc["sample"] = static_cast<qint64>(index);
  abc["seconds"] = seconds;
  s.metadata["abc"] = abc;
  s.loss("abc_single_sample",
         QStringLiteral("sample %1 of %2 at %3 s")
             .arg(index)
             .arg(times.size())
             .arg(seconds, 0, 'g', 17));
  auto *material = FbxSurfaceLambert::Create(s.scene, "Default");
  material->Diffuse.Set(FbxDouble3(.7, .7, .7));
  std::set<QString> names;
  std::size_t written = 0, degenerate = 0;
  for (std::size_t mi = 0; mi < infos.size(); ++mi) {
    s.check();
    const auto sample = archive.read(mi, seconds, s.stop);
    s.reserve(sample.positions.size() * 32 + sample.corner_points.size() * 48);
    std::size_t faces = 0;
    for (const auto n : sample.face_sizes)
      faces += n >= 3 ? 1 : 0;
    degenerate += sample.face_sizes.size() - faces;
    if (!faces) {
      s.loss("abc_empty_mesh_omitted", QString::fromStdString(infos[mi].path));
      continue;
    }
    auto name = leaf(infos[mi].path);
    for (int suffix = 2; !names.insert(name).second; ++suffix)
      name = leaf(infos[mi].path) + "_" + QString::number(suffix);
    auto *node = FbxNode::Create(s.scene, name.toUtf8().constData());
    s.scene->GetRootNode()->AddChild(node);
    string_property(node, "AbcPath", QString::fromStdString(infos[mi].path));
    auto *mesh = FbxMesh::Create(s.scene, (name + " geometry").toUtf8().constData());
    node->SetNodeAttribute(mesh);
    node->AddMaterial(material);
    // Transforms are baked: the frame is exported in archive space.
    const auto &world = sample.world;
    mesh->InitControlPoints(static_cast<int>(sample.positions.size()));
    for (std::size_t p = 0; p < sample.positions.size(); ++p)
      mesh->SetControlPointAt(transform_point(world, sample.positions[p]),
                              static_cast<int>(p));
    FbxGeometryElementUV *uv{};
    if (!sample.uv.empty()) {
      uv = mesh->CreateElementUV("UV0");
      uv->SetMappingMode(FbxLayerElement::eByPolygonVertex);
      uv->SetReferenceMode(FbxLayerElement::eDirect);
    }
    FbxGeometryElementNormal *normal{};
    // Normal matrix: inverse transpose of the upper 3x3.
    double nm[9]{};
    if (!sample.normals.empty()) {
      normal = mesh->CreateElementNormal();
      normal->SetMappingMode(FbxLayerElement::eByPolygonVertex);
      normal->SetReferenceMode(FbxLayerElement::eDirect);
      const double a = world[0], b = world[1], c = world[2], d = world[4],
                   e = world[5], f = world[6], g = world[8], h = world[9],
                   i = world[10];
      const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
      if (std::abs(det) < 1e-300)
        invalid("ABC transform is singular");
      const double inv[9]{(e * i - f * h) / det, (c * h - b * i) / det,
                          (b * f - c * e) / det, (f * g - d * i) / det,
                          (a * i - c * g) / det, (c * d - a * f) / det,
                          (d * h - e * g) / det, (b * g - a * h) / det,
                          (a * e - b * d) / det};
      for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 3; ++col)
          nm[r * 3 + col] = inv[col * 3 + r];
    }
    auto *materials = mesh->CreateElementMaterial();
    materials->SetMappingMode(FbxLayerElement::eAllSame);
    materials->SetReferenceMode(FbxLayerElement::eIndexToDirect);
    materials->GetIndexArray().Add(0);
    std::size_t first = 0;
    for (std::size_t face = 0; face < sample.face_sizes.size(); ++face) {
      if ((face & 4095) == 0)
        s.check();
      const auto count = static_cast<std::size_t>(sample.face_sizes[face]);
      if (count >= 3) {
        mesh->BeginPolygon(0);
        // Alembic winds clockwise; FBX front faces are counter-clockwise.
        for (std::size_t k = 0; k < count; ++k) {
          const auto corner = first + (k ? count - k : 0);
          const auto point = static_cast<std::size_t>(sample.corner_points[corner]);
          mesh->AddPolygon(static_cast<int>(point));
          if (uv) {
            const auto *v = scoped(sample.uv, sample.uv_scope, corner, face, point);
            uv->GetDirectArray().Add(v ? FbxVector2(v->x, v->y) : FbxVector2());
          }
          if (normal) {
            FbxVector4 n;
            if (const auto *v = scoped(sample.normals, sample.normal_scope, corner, face, point)) {
              n = FbxVector4(nm[0] * v->x + nm[1] * v->y + nm[2] * v->z,
                             nm[3] * v->x + nm[4] * v->y + nm[5] * v->z,
                             nm[6] * v->x + nm[7] * v->y + nm[8] * v->z);
              n.Normalize();
            }
            normal->GetDirectArray().Add(n);
          }
        }
        mesh->EndPolygon();
      }
      first += count;
    }
    if (!normal)
      mesh->GenerateNormals();
    ++written;
  }
  if (degenerate)
    s.loss("abc_degenerate_faces",
           QString::number(degenerate) + " faces with fewer than three corners");
  for (const auto &item : archive.skipped())
    s.loss("abc_objects_omitted", QString::fromStdString(item.path) + " (" +
                                      QString::fromStdString(item.schema) + ")");
  if (!written)
    invalid("ABC sample has no polygon geometry");
  s.loss("abc_transforms_baked",
         "Transforms of this sample are baked into vertices; one node per mesh");
  s.scene->GetGlobalSettings().SetAxisSystem(FbxAxisSystem::MayaYUp);
  s.scene->GetGlobalSettings().SetSystemUnit(FbxSystemUnit::m);
  s.loss("abc_coordinate_assumption",
         "Alembic declares no units. Numeric coordinates are preserved and "
         "tagged as right-handed Y-up metres before optional output conversion.");
}
} // namespace formaview::scene_export
