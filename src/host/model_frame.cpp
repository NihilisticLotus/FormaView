#include "model_document.hpp"
#include <cmath>
#include <limits>
#include <optional>
namespace {
using namespace formaview;
void checkUv(const QPointF &uv) {
  if (!std::isfinite(uv.x()) || !std::isfinite(uv.y()) ||
      std::abs(uv.x()) > std::numeric_limits<float>::max() ||
      std::abs(uv.y()) > std::numeric_limits<float>::max())
    throw skeletal::Failure(ErrorCode::InvalidArgument,
                            "Material UV transform is not representable; "
                            "source values retained");
}
std::shared_ptr<const render::Frame> build(const ModelDocument &document,
                                           const ModelRenderState &state,
                                           int mode, std::uint64_t revision,
                                           std::stop_token stop) {
  auto f = std::make_shared<render::Frame>();
  f->revision = revision;
  f->cacheable = true;
  f->rgba = {1, 1, 1, 1};
  f->lit = mode == 0;
  f->heat = mode == 2;
  f->lines = mode == 1;
  f->uv = mode == 1;
  f->linearColour = document.linearColour;
  if (mode != 1) {
    const auto origin = -document.center / document.radius;
    const auto guide = [&](QVector3D p, QVector3D tint) {
      f->guides.push_back({p.x(), p.y(), p.z(), 0, 0, 0, 0, 1, 0, tint.x(),
                           tint.y(), tint.z(), 1, -2, .5F});
    };
    for (int i = -10; i <= 10; ++i) {
      const auto offset = static_cast<float>(i) * .2F;
      const auto tint =
          i == 0 ? QVector3D(.32F, .32F, .35F) : QVector3D(.20F, .20F, .23F);
      guide(origin + QVector3D(-2, 0, offset), tint);
      guide(origin + QVector3D(2, 0, offset), tint);
      guide(origin + QVector3D(offset, 0, -2), tint);
      guide(origin + QVector3D(offset, 0, 2), tint);
    }
    f->gridVertices = static_cast<std::uint32_t>(f->guides.size());
    for (int axis = 0; axis < 3; ++axis) {
      QVector3D direction, side;
      direction[axis] = 1;
      side[(axis + 1) % 3] = 1;
      const auto tint = axis == 0   ? QVector3D(.95F, .25F, .25F)
                        : axis == 1 ? QVector3D(.3F, .85F, .35F)
                                    : QVector3D(.3F, .55F, 1.F);
      const auto tip =
          origin + document.displayBasis.mapVector(direction) * .6F;
      const auto base =
          origin + document.displayBasis.mapVector(direction) * .48F;
      const auto wing = document.displayBasis.mapVector(side) * .06F;
      guide(origin, tint);
      guide(tip, tint);
      guide(tip, tint);
      guide(base + wing, tint);
      guide(tip, tint);
      guide(base - wing, tint);
    }
    if (!state.highlightedNode.isEmpty()) {
      const auto &mesh =
          document.meshes.at(static_cast<std::size_t>(state.mesh));
      QHash<QString, QVariantMap> palette;
      bool bindingSpace = false;
      if (state.bindings.at(static_cast<std::size_t>(state.mesh)) >= 0) {
        const auto &skin = mesh.skins.at(static_cast<std::size_t>(
            state.bindings.at(static_cast<std::size_t>(state.mesh))));
        for (const auto &entry : skin.palette) {
          const auto record = entry.toMap();
          palette.insert(record.value("nodeId").toString(), record);
          bindingSpace |= record.contains("geometryToBone");
        }
        if (state.bone >= 0 && state.bone < skin.palette.size()) {
          const auto record = skin.palette[state.bone].toMap();
          if (record.value("nodeId").toString() == state.highlightedNode)
            palette.insert(state.highlightedNode, record);
        }
      }
      const auto matrix =
          [](const QVariantList &values) -> std::optional<QMatrix4x4> {
        if (values.size() != 16)
          return {};
        QMatrix4x4 result;
        for (int r = 0; r < 4; ++r)
          for (int c = 0; c < 4; ++c) {
            const double value = values[r * 4 + c].toDouble();
            if (!std::isfinite(value) ||
                std::abs(value) > std::numeric_limits<float>::max())
              return {};
            result(r, c) = static_cast<float>(value);
          }
        return result;
      };
      const auto position =
          [&](const QVariantMap &node) -> std::optional<QVector3D> {
        if (node.isEmpty())
          return {};
        const auto record = palette.value(node.value("id").toString());
        QVector3D raw;
        if (record.contains("geometryToBone")) {
          const auto toBone = matrix(record.value("geometryToBone").toList());
          const auto toWorld = matrix(document.nodeById.value(mesh.nodeId)
                                          .value("geometryWorld")
                                          .toList());
          if (!toBone || !toWorld)
            return {};
          bool invertible = false;
          const auto fromBone = toBone->inverted(&invertible);
          if (!invertible)
            return {};
          raw = toWorld->map(fromBone.map(QVector3D{}));
        } else {
          if (bindingSpace)
            return {};
          const auto values = record.contains("defaultJointWorld")
                                  ? record.value("defaultJointWorld")
                              : record.contains("bindWorld")
                                  ? record.value("bindWorld")
                                  : node.value("world");
          const auto world = matrix(values.toList());
          if (!world)
            return {};
          raw = world->map(QVector3D{});
        }
        const auto result = (document.displayBasis.map(raw) - document.center) /
                            document.radius;
        if (!std::isfinite(result.x()) || !std::isfinite(result.y()) ||
            !std::isfinite(result.z()))
          return {};
        return result;
      };
      const auto selected = document.nodeById.value(state.highlightedNode);
      const auto start = position(selected);
      const auto connect = [&](const QVariantMap &other) {
        const auto end = position(other);
        if (!start || !end || (*start - *end).lengthSquared() < 1e-12F)
          return;
        guide(*start, QVector3D(.95F, .48F, .18F));
        guide(*end, QVector3D(.95F, .48F, .18F));
        f->selectionVertices += 2;
      };
      for (const auto &value : document.nodes) {
        const auto node = value.toMap();
        if (node.value("parent").toString() == state.highlightedNode &&
            node.value("bone").toBool())
          connect(node);
      }
      if (!f->selectionVertices)
        connect(document.nodeById.value(selected.value("parent").toString()));
    }
  }
  std::size_t capacity = 0;
  for (std::size_t mi = 0; mi < document.meshes.size(); ++mi) {
    if (!state.visible.empty() && !state.visible[mi])
      continue;
    if ((state.isolated || mode != 0) &&
        mi != static_cast<std::size_t>(state.mesh))
      continue;
    capacity += document.meshes[mi].triangles.size() * (mode == 1 ? 2 : 1);
  }
  f->vertices.reserve(capacity);
  for (std::size_t mi = 0; mi < document.meshes.size(); ++mi) {
    if (!state.visible.empty() && !state.visible[mi])
      continue;
    if ((state.isolated || mode != 0) &&
        mi != static_cast<std::size_t>(state.mesh))
      continue;
    const auto &m = document.meshes[mi];
    const bool collision =
        !state.classes.empty() && state.classes.at(mi).collision;
    if (mode == 1 && !m.uvs.empty() &&
        m.invalidUVs.at(static_cast<std::size_t>(state.uvs[mi])))
      throw skeletal::Failure(
          ErrorCode::InvalidArgument,
          "Selected UV set has non-finite or unrepresentable coordinates; "
          "source values are retained");
    std::vector<int> textureIndices(m.textures.size(), -1);
    static const std::map<int, ModelTextureLink> empty;
    const auto &links = mi < state.textures.size() ? state.textures[mi] : empty;
    if (mode == 0 && !collision)
      for (std::size_t i = 0; i < m.textures.size(); ++i)
        if (const auto source = links.find(static_cast<int>(i));
            source != links.end() || m.textures[i]) {
          const auto set = source != links.end() && source->second.uv >= 0
                               ? source->second.uv
                           : m.materialUv.empty() ? 0
                                                  : m.materialUv.at(i);
          if (m.uvs.empty() || m.invalidUVs.at(static_cast<std::size_t>(set)))
            continue;
          textureIndices[i] = static_cast<int>(f->textures.size());
          f->textures.push_back(source != links.end() ? source->second.pixels
                                                      : m.textures[i]);
        }
    if (mode == 1 && m.uvs.empty())
      continue;
    if (mode == 2 && state.bindings[mi] < 0)
      continue;
    if (mode == 1) {
      const auto slot = static_cast<std::size_t>(state.material);
      const auto link = links.find(state.material);
      const auto textureUvSet = link != links.end()    ? link->second.uv
                                : m.materialUv.empty() ? 0
                                                       : m.materialUv.at(slot);
      const auto pixels =
          link != links.end() ? link->second.pixels : m.textures.at(slot);
      if (pixels && textureUvSet == state.uvs[mi]) {
        f->backgroundTexture = static_cast<int>(f->textures.size());
        f->textures.push_back(pixels);
        for (const auto &p : {QPointF(0, 0), QPointF(1, 0), QPointF(0, 1),
                              QPointF(0, 1), QPointF(1, 0), QPointF(1, 1)}) {
          auto uv = p;
          if (!m.uvAffine.empty()) {
            const auto &t = m.uvAffine.at(slot);
            uv = {t[0] * p.x() + t[1] * p.y() + t[2],
                  t[3] * p.x() + t[4] * p.y() + t[5]};
          } else if (!m.uvTransforms.empty()) {
            const auto &t = m.uvTransforms.at(slot);
            const auto u = p.x() * t[2], v = p.y() * t[3];
            uv = {t[0] + std::cos(t[4]) * u - std::sin(t[4]) * v,
                  t[1] + std::sin(t[4]) * u + std::cos(t[4]) * v};
          }
          checkUv(uv);
          f->vertices.push_back({float(p.x() * 2 - 1), float(p.y() * 2 - 1),
                                 -.02F, float(uv.x()),
                                 float(m.uvTopOrigin ? uv.y() : 1 - uv.y()), 0,
                                 0, 0, 1, 1, 1, 1, 1, 0, .5F});
        }
        f->backgroundVertices = 6;
      }
      for (int line = 0; line <= 10; ++line) {
        const float p = -1 + float(line) * .2F;
        const float tint = (line == 0 || line == 10) ? .48F : .24F;
        for (const auto &v : {QVector3D(-1, p, -.01F), QVector3D(1, p, -.01F),
                              QVector3D(p, -1, -.01F), QVector3D(p, 1, -.01F)})
          f->vertices.push_back({v.x(), v.y(), v.z(), 0, 0, 0, 0, 0, 1, tint,
                                 tint, tint, 1, -1, .5F});
      }
    }
    std::vector<bool> usedPoints(m.points.size());
    const ModelSkin *skin =
        mode == 2 ? &m.skins.at(static_cast<std::size_t>(state.bindings[mi]))
                  : nullptr;
    auto vertex = [&](std::uint32_t corner) {
      const auto point = m.corners.at(corner);
      if (!usedPoints.at(point)) {
        usedPoints[point] = true;
        ++f->sourcePoints;
      }
      auto pos = (m.points.at(point) - document.center) / document.radius;
      QPointF uv;
      const auto material = m.cornerMaterial.at(corner);
      if (!m.uvs.empty()) {
        const auto link = links.find(static_cast<int>(material));
        const int selected =
            mode == 0 && link != links.end() && link->second.uv >= 0
                ? link->second.uv
            : mode == 0 && !m.materialUv.empty() ? m.materialUv.at(material)
                                                 : state.uvs[mi];
        uv = m.uvs.at(static_cast<std::size_t>(selected)).at(corner);
        if (m.invalidUVs.at(static_cast<std::size_t>(selected)))
          uv = {};
        if (mode == 0 && !m.uvAffine.empty()) {
          const auto &t = m.uvAffine.at(material);
          uv = {t[0] * uv.x() + t[1] * uv.y() + t[2],
                t[3] * uv.x() + t[4] * uv.y() + t[5]};
        } else if (mode == 0 && !m.uvTransforms.empty()) {
          const auto &t = m.uvTransforms.at(material);
          const double u = uv.x() * t[2], v = uv.y() * t[3];
          uv = {t[0] + std::cos(t[4]) * u - std::sin(t[4]) * v,
                t[1] + std::sin(t[4]) * u + std::cos(t[4]) * v};
        }
      }
      if (mode == 1)
        pos = {static_cast<float>(uv.x() * 2 - 1),
               static_cast<float>(uv.y() * 2 - 1), 0};
      checkUv(uv);
      if (!f->hasDisplayBounds) {
        f->displayLow = f->displayHigh = pos;
        f->hasDisplayBounds = true;
      } else
        for (int axis = 0; axis < 3; ++axis) {
          f->displayLow[axis] = std::min(f->displayLow[axis], pos[axis]);
          f->displayHigh[axis] = std::max(f->displayHigh[axis], pos[axis]);
        }
      const auto n = m.unlit || collision ? QVector3D(.4F, .7F, 1)
                                          : m.normals.at(corner),
                 c = mode == 1   ? QVector3D(.92F, .92F, .95F)
                     : collision ? QVector3D(1.F, .3F, .05F)
                                 : m.colours.at(corner);
      float weight = std::numeric_limits<float>::quiet_NaN();
      if (skin) {
        const auto &values = skin->weights.at(point);
        if (!values.empty()) {
          double sum = 0;
          bool invalid = false;
          for (const auto &w : values)
            if (!std::isfinite(w.value) || w.value < 0)
              invalid = true;
            else if (w.bone == state.bone)
              sum += w.value;
          weight =
              !invalid && std::isfinite(sum) ? static_cast<float>(sum) : -1.F;
        }
      }
      return render::Vertex{
          pos.x(),
          pos.y(),
          pos.z(),
          static_cast<float>(uv.x()),
          static_cast<float>(m.uvTopOrigin ? uv.y() : 1 - uv.y()),
          weight,
          n.x(),
          n.y(),
          n.z(),
          c.x(),
          c.y(),
          c.z(),
          collision          ? (state.collisionMode == 1 ? .35F : 1.F)
          : m.alphas.empty() ? 1.F
                             : m.alphas.at(corner),
          mode == 1   ? -1.F
          : collision ? (state.collisionMode == 1 ? 2.F : 0.F)
          : m.alphaModes.empty()
              ? 0.F
              : static_cast<float>(m.alphaModes.at(material)),
          m.alphaCutoffs.empty() ? .5F : m.alphaCutoffs.at(material)};
    };
    for (std::size_t t = 0; t < m.triangles.size(); t += 3) {
      if ((t & 4095) == 0 && stop.stop_requested())
        throw skeletal::Failure(ErrorCode::Cancelled,
                                "Model frame preparation cancelled");
      if (state.mode == 1 && state.materialOnly &&
          m.cornerMaterial.at(m.triangles[t]) !=
              static_cast<std::uint32_t>(state.material))
        continue;
      ++f->sourceTriangles;
      const auto first = static_cast<std::uint32_t>(f->vertices.size());
      if (mode == 1) {
        for (int e = 0; e < 3; ++e) {
          f->vertices.push_back(
              vertex(m.triangles[t + static_cast<std::size_t>(e)]));
          f->vertices.push_back(
              vertex(m.triangles[t + static_cast<std::size_t>((e + 1) % 3)]));
        }
      } else
        for (int e = 0; e < 3; ++e)
          f->vertices.push_back(
              vertex(m.triangles[t + static_cast<std::size_t>(e)]));
      if (mode == 0) {
        const int texture =
            textureIndices.at(m.cornerMaterial.at(m.triangles[t]));
        const auto slot = m.cornerMaterial.at(m.triangles[t]);
        const bool blend =
            collision ? state.collisionMode == 1
                      : !m.alphaModes.empty() && m.alphaModes.at(slot) == 2;
        const auto center = (m.points.at(m.corners.at(m.triangles[t])) +
                             m.points.at(m.corners.at(m.triangles[t + 1])) +
                             m.points.at(m.corners.at(m.triangles[t + 2])) -
                             document.center * 3) /
                            (document.radius * 3);
        if (!f->draws.empty() && f->draws.back().texture == texture &&
            f->draws.back().blend == blend &&
            f->draws.back().first + f->draws.back().count == first) {
          auto &draw = f->draws.back();
          draw.center =
              (draw.center * static_cast<float>(draw.count) + center * 3) /
              static_cast<float>(draw.count + 3);
          draw.count += 3;
        } else
          f->draws.push_back({first, 3, texture, blend, center});
      }
    }
  }
  return f;
}
} // namespace
ModelFrames buildModelFrames(const ModelDocument &d, const ModelRenderState &s,
                             std::uint64_t revision, std::stop_token stop) {
  if (s.mode < 0 || s.mode > 3 || s.mesh < 0 ||
      s.mesh >= static_cast<int>(d.meshes.size()) ||
      s.bindings.size() != d.meshes.size() || s.uvs.size() != d.meshes.size() ||
      (!s.visible.empty() && s.visible.size() != d.meshes.size()))
    throw formaview::skeletal::Failure(formaview::ErrorCode::InvalidArgument,
                                       "Invalid model view state");
  if (s.material < 0 ||
      s.material >=
          d.meshes.at(static_cast<std::size_t>(s.mesh)).materials.size())
    throw formaview::skeletal::Failure(formaview::ErrorCode::InvalidArgument,
                                       "Invalid material range");
  if (!s.classes.empty() && s.classes.size() != d.meshes.size())
    throw formaview::skeletal::Failure(formaview::ErrorCode::InvalidArgument,
                                       "Invalid model classification state");
  for (std::size_t i = 0; i < d.meshes.size(); ++i) {
    const auto &m = d.meshes[i];
    if (s.bindings[i] < -1 ||
        s.bindings[i] >= static_cast<int>(m.skins.size()) || s.uvs[i] < 0 ||
        (!m.uvs.empty() && s.uvs[i] >= static_cast<int>(m.uvs.size())))
      throw formaview::skeletal::Failure(
          formaview::ErrorCode::InvalidArgument,
          "Model selection is outside source data");
  }
  ModelFrames result;
  result.mode = s.mode;
  auto modelState = s;
  if (s.mode == 1)
    modelState.isolated = true;
  result.model = build(d, modelState, s.mode == 2 ? 2 : 0, revision * 2, stop);
  if (s.mode == 1)
    result.uv = build(d, s, 1, revision * 2 + 1, stop);
  return result;
}
