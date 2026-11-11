#include "model_document.hpp"
#include "model_abc.hpp"
#include "formaview/preview.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <QDataStream>
#include <QIODevice>
namespace {
using namespace formaview;
void appendGuides(render::Frame &frame, const ModelDocument &document) {
  const auto origin = -document.center / document.radius;
  const auto guide = [&](QVector3D p, QVector3D tint) {
    frame.guides.push_back({p.x(), p.y(), p.z(), 0, 0, 0, 0, 1, 0, tint.x(),
                           tint.y(), tint.z(), 1, -2, .5F});
  };
  for (int i = -10; i <= 10; ++i) {
    const auto offset = static_cast<float>(i) * .2F;
    const auto tint = i == 0 ? QVector3D(.32F, .32F, .35F)
                             : QVector3D(.20F, .20F, .23F);
    guide(origin + QVector3D(-2, 0, offset), tint);
    guide(origin + QVector3D(2, 0, offset), tint);
    guide(origin + QVector3D(offset, 0, -2), tint);
    guide(origin + QVector3D(offset, 0, 2), tint);
  }
  frame.gridVertices = static_cast<std::uint32_t>(frame.guides.size());
  for (int axis = 0; axis < 3; ++axis) {
    QVector3D direction, side;
    direction[axis] = 1;
    side[(axis + 1) % 3] = 1;
    const auto tint = axis == 0 ? QVector3D(.95F, .25F, .25F)
                    : axis == 1 ? QVector3D(.3F, .85F, .35F)
                                : QVector3D(.3F, .55F, 1.F);
    const auto tip = origin + document.displayBasis.mapVector(direction) * .6F;
    const auto base = origin + document.displayBasis.mapVector(direction) * .48F;
    const auto wing = document.displayBasis.mapVector(side) * .06F;
    guide(origin, tint); guide(tip, tint);
    guide(tip, tint); guide(base + wing, tint);
    guide(tip, tint); guide(base - wing, tint);
  }
}
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
                                           std::stop_token stop,
                                           const render::Frame *pose, bool recordSources = false) {
  auto f = std::make_shared<render::Frame>();
  f->revision = revision;
  if(pose){f->clip=pose->clip;f->seconds=pose->seconds;}
  // ABC playback streams new geometry every sample. Keeping its old VBOs
  // resident would move all active resources into the LRU and recreate the
  // buffers, textures, bindings and pipelines on every cache miss. Reuse the
  // dynamic buffers/pipelines instead; the bounded CPU frame cache still
  // serves inspection and backward seeks.
  f->cacheable = !pose && !document.geometryCache;
  f->rgba = {1, 1, 1, 1};
  f->lit = mode == 0;
  f->heat = mode == 2;
  f->lines = mode == 1;
  f->uv = mode == 1;
  f->linearColour = document.linearColour;
  if (mode != 1) {
    appendGuides(*f, document);
    const auto guide = [&](QVector3D p, QVector3D tint) {
      f->guides.push_back({p.x(), p.y(), p.z(), 0, 0, 0, 0, 1, 0, tint.x(),
                           tint.y(), tint.z(), 1, -2, .5F});
    };
    if (!state.highlightedNode.isEmpty() && pose && !pose->worlds.empty()) {
      const auto selection=resolveGpuHighlight(document,state.highlightedNode);
      const auto position=[&](int index){const auto &m=pose->worlds.at(static_cast<std::size_t>(index));
        return (document.displayBasis.map(QVector3D(float(m[3]),float(m[7]),float(m[11])))-document.center)/std::max(document.radius,1e-6F);};
      if(selection.node>=0){const auto start=position(selection.node);
        for(int edge:selection.edges){guide(start,QVector3D(.95F,.48F,.18F));guide(position(edge),QVector3D(.95F,.48F,.18F));f->selectionVertices+=2;}}
    } else if (!state.highlightedNode.isEmpty() && !pose) {
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
  // UV adds a 44-vertex grid and at most six background vertices.
  constexpr std::size_t maxVertices = render::vertex_upload_budget / sizeof(render::Vertex);
  std::size_t capacity = mode == 1 ? 50 : 0;
  for (std::size_t mi = 0; mi < document.meshes.size(); ++mi) {
    if (!state.visible.empty() && !state.visible[mi])
      continue;
    if ((state.isolated || mode != 0) &&
        mi != static_cast<std::size_t>(state.mesh))
      continue;
    const auto corners = document.meshes[mi].triangles.size();
    const auto multiplier = mode == 1 ? 2U : 1U;
    if (corners > (maxVertices - capacity) / multiplier)
      throw skeletal::Failure(ErrorCode::ResourceLimit,
                              "Model frame exceeds the 256 MiB vertex upload budget");
    capacity += corners * multiplier;
  }
  f->vertices.reserve(capacity);
  std::size_t poseOffset = 0;
  for (std::size_t mi = 0; mi < document.meshes.size(); ++mi) {
    const auto meshPoseOffset = poseOffset;
    poseOffset += document.meshes[mi].triangles.size();
    if (!state.visible.empty() && !state.visible[mi])
      continue;
    if ((state.isolated || mode != 0) &&
        mi != static_cast<std::size_t>(state.mesh))
      continue;
    const auto &m = document.meshes[mi];
    std::vector<std::size_t> posedCorners;
    if (pose && mode != 1) {
      posedCorners.resize(m.corners.size());
      for (std::size_t t = 0; t < m.triangles.size(); ++t)
        posedCorners.at(m.triangles[t]) = meshPoseOffset + t;
    }
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
      if (recordSources && mode != 1)
        f->sourceCorners.push_back({static_cast<std::uint32_t>(mi), corner});
      if (!usedPoints.at(point)) {
        usedPoints[point] = true;
        ++f->sourcePoints;
      }
      auto pos = (m.points.at(point) - document.center) / document.radius;
      const auto *posed = pose && mode != 1
                              ? &pose->vertices.at(posedCorners.at(corner))
                              : nullptr;
      if (posed)
        pos = {posed->x, posed->y, posed->z};
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
      const auto n = posed ? QVector3D(posed->nx, posed->ny, posed->nz)
                    : m.unlit || collision ? QVector3D(.4F, .7F, 1)
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
        const auto &a = f->vertices.at(first);
        const auto &b = f->vertices.at(first + 1);
        const auto &c = f->vertices.at(first + 2);
        const auto center = QVector3D(a.x+b.x+c.x, a.y+b.y+c.y, a.z+b.z+c.z) / 3;
        if (!f->draws.empty() && f->draws.back().texture == texture &&
            f->draws.back().blend == blend &&
            (!recordSources || f->draws.back().mesh == static_cast<int>(mi)) &&
            f->draws.back().first + f->draws.back().count == first) {
          auto &draw = f->draws.back();
          draw.center =
              (draw.center * static_cast<float>(draw.count) + center * 3) /
              static_cast<float>(draw.count + 3);
          draw.count += 3;
        } else
          f->draws.push_back({first, 3, texture, blend, center,
                              recordSources ? static_cast<int>(mi) : -1});
      }
    }
  }
  return f;
}
} // namespace
void appendModelGuides(render::Frame &frame, const ModelDocument &document) {
  appendGuides(frame, document);
}
namespace {
void validateModelState(const ModelDocument &d, const ModelRenderState &s) {
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
}
} // namespace
ModelFrames buildModelFrames(const ModelDocument &d, const ModelRenderState &s,
                             std::uint64_t revision, std::stop_token stop,
                             const render::Frame *pose, bool recordSources) {
  validateModelState(d,s);
  ModelFrames result;
  result.mode = s.mode;
  auto modelState = s;
  if (s.mode == 1)
    modelState.isolated = true;
  result.model = build(d, modelState, s.mode == 2 ? 2 : 0, revision * 2, stop, pose, recordSources);
  if (s.mode == 1)
    result.uv = build(d, s, 1, revision * 2 + 1, stop, nullptr);
  return result;
}

ModelFrames buildAbcStreamFrames(const ModelDocument &d, const ModelRenderState &s,
    std::uint64_t revision, std::stop_token stop,
    std::shared_ptr<const formaview::render::Frame> previous) {
  validateModelState(d,s);
  // The normal builder remains the oracle/fallback for UV inspection, heat,
  // collision overlays, selected-node guides and session texture overrides.
  const bool plain = d.geometryCache && s.mode == 0 && s.highlightedNode.isEmpty() &&
      d.cacheMeshKeys.size() == d.meshes.size() &&
      std::none_of(s.classes.begin(),s.classes.end(),[](const auto &c){return c.collision;}) &&
      std::all_of(s.textures.begin(),s.textures.end(),[](const auto &t){return t.empty();});
  if (!plain) return buildModelFrames(d,s,revision,stop);
  QByteArray signature;
  QDataStream out(&signature,QIODevice::WriteOnly);
  bool reusable = true;
  out << d.hash << d.center << d.radius << d.displayBasis << s.mesh << s.isolated;
  for (std::size_t mi=0;mi<d.meshes.size();++mi) {
    const auto &m=d.meshes[mi]; const auto &keys=d.cacheMeshKeys[mi];
    reusable = reusable && !keys.face_indices.empty() && !keys.face_counts.empty() &&
        (m.uvs.empty() || !keys.uv.empty());
    out << m.id << quint64(m.points.size()) << quint64(m.corners.size())
        << quint64(m.triangles.size()) << (s.visible.empty() || s.visible.at(mi))
        << s.uvs.at(mi) << QString::fromStdString(keys.face_indices)
        << QString::fromStdString(keys.face_counts) << QString::fromStdString(keys.uv)
        << QString::fromStdString(keys.uv_indices);
  }
  std::shared_ptr<const formaview::render::Frame> base;
  if (reusable && previous && previous->streamGeometry && previous->streamSignature == signature)
    base=previous->streamGeometry;
  else
    base=buildModelFrames(d,s,revision,stop,nullptr,true).model;
  auto f=std::make_shared<formaview::render::Frame>();
  f->streamGeometry=base; f->streamSignature=std::move(signature);
  f->revision=revision*2; f->seconds=d.geometryCache->times().at(d.cacheSample);
  f->lit=base->lit; f->linearColour=base->linearColour;
  f->sourcePoints=base->sourcePoints; f->sourceTriangles=base->sourceTriangles;
  f->deformation.reserve(base->sourceCorners.size());
  for (std::size_t vi=0;vi<base->sourceCorners.size();++vi) {
    if ((vi&4095)==0 && stop.stop_requested())
      throw formaview::skeletal::Failure(formaview::ErrorCode::Cancelled,"ABC frame cancelled");
    const auto [mi,corner]=base->sourceCorners[vi];
    const auto &m=d.meshes.at(mi);
    const auto p=(m.points.at(m.corners.at(corner))-d.center)/d.radius;
    const auto n=m.unlit?QVector3D(.4F,.7F,1):m.normals.at(corner);
    f->deformation.push_back({p.x(),p.y(),p.z(),n.x(),n.y(),n.z()});
    if (!f->hasDisplayBounds) { f->displayLow=f->displayHigh=p; f->hasDisplayBounds=true; }
    else for(int axis=0;axis<3;++axis) {
      f->displayLow[axis]=std::min(f->displayLow[axis],p[axis]);
      f->displayHigh[axis]=std::max(f->displayHigh[axis],p[axis]);
    }
  }
  ModelFrames result; result.model=std::move(f); return result;
}

formaview::Matrix4 lockRootLocal(const formaview::Matrix4 &source,
                                     const formaview::Matrix4 &base) {
  // Root lock must not guess a TRS decomposition. A faithful lock needs a
  // clean orthogonal positive-scale basis on both sides: the base supplies
  // rotation and translation, the source the per-axis scale. Mirrored,
  // sheared or degenerate transforms return the source unchanged so the
  // animation keeps its true motion instead of a corrupted lock.
  const auto lockable = [](const formaview::Matrix4 &m) {
    double columns[3][3];
    for (int column = 0; column < 3; ++column)
      for (int row = 0; row < 3; ++row)
        columns[column][row] = m[static_cast<std::size_t>(row * 4 + column)];
    const auto determinant =
        columns[0][0] * (columns[1][1] * columns[2][2] -
                         columns[1][2] * columns[2][1]) -
        columns[1][0] * (columns[0][1] * columns[2][2] -
                         columns[0][2] * columns[2][1]) +
        columns[2][0] * (columns[0][1] * columns[1][2] -
                         columns[0][2] * columns[1][1]);
    if (determinant <= 1e-12)
      return false; // Degenerate basis (zero axis) or a mirrored one.
    for (int i = 0; i < 3; ++i)
      for (int j = i + 1; j < 3; ++j) {
        double dot = 0, left = 0, right = 0;
        for (int row = 0; row < 3; ++row) {
          dot += columns[i][row] * columns[j][row];
          left += columns[i][row] * columns[i][row];
          right += columns[j][row] * columns[j][row];
        }
        if (std::abs(dot) > 1e-6 * std::sqrt(left * right))
          return false; // Sheared basis: no unique rotation to lock onto.
      }
    return true;
  };
  if (!lockable(source) || !lockable(base))
    return source;
  // Keep the source scale, take translation/rotation from the base: scale
  // each base basis column to the source column length. Row-major storage.
  formaview::Matrix4 result = base;
  for (int column = 0; column < 3; ++column) {
    const auto sourceLength = std::hypot(
        source[static_cast<std::size_t>(column)],
        source[static_cast<std::size_t>(4 + column)],
        source[static_cast<std::size_t>(8 + column)]);
    const auto baseLength = std::hypot(
        base[static_cast<std::size_t>(column)],
        base[static_cast<std::size_t>(4 + column)],
        base[static_cast<std::size_t>(8 + column)]);
    const auto factor = sourceLength / baseLength;
    result[static_cast<std::size_t>(column)] *= factor;
    result[static_cast<std::size_t>(4 + column)] *= factor;
    result[static_cast<std::size_t>(8 + column)] *= factor;
  }
  return result;
}

std::vector<formaview::Matrix4>
chainWorlds(const std::vector<formaview::Matrix4> &locals,
            const std::vector<std::int32_t> &parents) {
  if (parents.size() != locals.size())
    throw skeletal::Failure(ErrorCode::InvalidArgument, "Parent/local count mismatch");
  std::vector<formaview::Matrix4> worlds(locals.size());
  std::vector<unsigned char> state(locals.size());
  for (std::size_t start = 0; start < locals.size(); ++start) {
    if (state[start] == 2) continue;
    std::vector<std::size_t> path;
    auto at = static_cast<std::int64_t>(start);
    while (at >= 0) {
      if (static_cast<std::size_t>(at) >= locals.size() || state[at] == 1)
        throw skeletal::Failure(ErrorCode::InvalidArgument, "Invalid/cyclic animation parent chain");
      if (state[at] == 2) break;
      state[at] = 1;
      path.push_back(static_cast<std::size_t>(at));
      at = parents[at];
    }
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
      const auto index = *it;
      const auto parent = parents[index];
      worlds[index] = parent < 0 ? locals[index] : skeletal::multiply(worlds[parent], locals[index]);
      state[index] = 2;
    }
  }
  return worlds;
}

std::shared_ptr<const render::Frame>
buildAnimationFrame(const ModelDocument &d, std::size_t clip, double seconds,
                    std::uint64_t revision, bool pairMesh,
                    const std::vector<formaview::Matrix4> *overrideWorlds,
                    std::stop_token stop) {
  const auto cancelled = [&] {
    if (stop.stop_requested())
      throw skeletal::Failure(ErrorCode::Cancelled, "Animation frame cancelled");
  };
  cancelled();
  if (!d.worldAt && !overrideWorlds)
    throw skeletal::Failure(ErrorCode::InvalidArgument,
                            "Document has no animation evaluator");
  const auto lockedWorlds =
      overrideWorlds ? *overrideWorlds : d.worldAt(clip, seconds);
  const auto &worlds = lockedWorlds;
  cancelled();
  if (std::any_of(d.meshes.begin(),d.meshes.end(),[](const auto &mesh) { return !mesh.skinDeformationValid; }))
    throw skeletal::Failure(ErrorCode::InvalidArgument,
                            "Invalid source skin weights; animation paused, source records preserved");
  if (pairMesh && d.pairSource && !d.pairBoneMap.empty()) {
    // Skinned paired-model display: remap animation-order worlds into the
    // PSK mesh bone order and CPU-skin the source mesh. The PSK stays a
    // read-only reference; only preview vertices are produced. Vertices are
    // normalized exactly like the skeleton path (ModelView supplies the
    // camera matrix); flat per-triangle normals give lit shading.
    if (worlds.size() != d.boneParents.size())
      throw skeletal::Failure(ErrorCode::Internal,
                              "Animation pose does not match skeleton");
    std::vector<Matrix4> meshWorlds(d.pairBoneMap.size());
    for (std::size_t i = 0; i < d.pairBoneMap.size(); ++i)
      meshWorlds[i] = worlds[d.pairBoneMap[i]];
    const auto &skinned = preview::evaluate_mesh(
        *d.pairSource, meshWorlds, {}, preview::WeightPolicy::Normalize, stop);
    if (skinned.corners.size() % 3)
      throw skeletal::Failure(ErrorCode::Internal,
                              "Skinned corners are not triangles");
    auto f = std::make_shared<render::Frame>();
    f->worlds = worlds;
    f->revision = revision;
    f->clip=static_cast<int>(clip);f->seconds=seconds;
    f->lines = false;
    f->lit = true;
    f->cacheable = false;
    f->rgba = {0.62F, 0.66F, 0.71F, 1.F};
    f->sourcePoints = d.pairSource->mesh
                          ? d.pairSource->mesh->points.size()
                          : std::uint64_t{0};
    const auto display = d.displayBasis;
    const auto normalize = [&](const skeletal::Vec3 &p) {
      const auto mapped =
          display.map(QVector3D(static_cast<float>(p.x),
                                static_cast<float>(p.y),
                                static_cast<float>(p.z)));
      return (mapped - d.center) / std::max(d.radius, 1e-6F);
    };
    QVector3D mappedLow(1e30F, 1e30F, 1e30F), mappedHigh(-1e30F, -1e30F, -1e30F);
    f->vertices.reserve(skinned.corners.size());
    // Smooth skinned normals from the source point domain when available;
    // flat per-triangle normals only as the fallback.
    const auto smooth = [&](const preview::Vertex &corner,
                            QVector3D fallback) {
      const auto n = corner.normal;
      return n.x == 0 && n.y == 0 && n.z == 0
                 ? fallback
                 : display.mapVector(QVector3D(static_cast<float>(n.x),
                                               static_cast<float>(n.y),
                                               static_cast<float>(n.z)))
                       .normalized();
    };
    for (std::size_t t = 0; t + 2 < skinned.corners.size(); t += 3) {
      if (t % 3072 == 0) cancelled();
      const auto a = normalize(skinned.corners[t].position);
      const auto b = normalize(skinned.corners[t + 1].position);
      const auto c = normalize(skinned.corners[t + 2].position);
      auto flat = QVector3D::crossProduct(b - a, c - a);
      const auto length = flat.length();
      flat = length > 1e-12F ? flat / length : QVector3D{0, 0, 1};
      for (std::size_t k = 0; k < 3; ++k) {
        const auto &corner = skinned.corners[t + k];
        const auto p = k == 0 ? a : k == 1 ? b : c;
        const auto normal = smooth(corner, flat);
        for (int e = 0; e < 3; ++e)
          if (!std::isfinite(p[e]))
            throw skeletal::Failure(ErrorCode::InvalidArgument,
                                    "Non-finite skinned display position");
        mappedLow.setX(std::min(mappedLow.x(), p.x()));
        mappedLow.setY(std::min(mappedLow.y(), p.y()));
        mappedLow.setZ(std::min(mappedLow.z(), p.z()));
        mappedHigh.setX(std::max(mappedHigh.x(), p.x()));
        mappedHigh.setY(std::max(mappedHigh.y(), p.y()));
        mappedHigh.setZ(std::max(mappedHigh.z(), p.z()));
        f->vertices.push_back(
            {p.x(), p.y(), p.z(), static_cast<float>(corner.uv.x),
             static_cast<float>(corner.uv.y), 0, normal.x(), normal.y(),
             normal.z()});
      }
    }
    f->matrix.setToIdentity();
    f->displayLow = mappedLow;
    f->displayHigh = mappedHigh;
    f->hasDisplayBounds = true;
    appendGuides(*f, d);
    return f;
  }
  if (!d.meshes.empty()) {
    // Externally driven target FBX: skin every mesh through its own
    // per-point influences (target bind data, never overwritten), blend
    // the averaged smooth normals with the same weights, keep meshes
    // without influences static. Normalization, display bounds and guides
    // mirror the paired-model path.
    if (worlds.size() != d.animNodeParents.size())
      throw skeletal::Failure(ErrorCode::Internal,
                              "Driven pose does not match target nodes");
    auto f = std::make_shared<render::Frame>();
    f->worlds = worlds;
    f->revision = revision;
    f->clip=static_cast<int>(clip);f->seconds=seconds;
    f->lines = false;
    f->lit = true;
    f->cacheable = false;
    f->rgba = {0.64F, 0.67F, 0.72F, 1.F};
    const auto display = d.displayBasis;
    // Cluster matrix cache: world[bone] * geometryToBone is computed ONCE
    // per cluster per frame (~bones), the per-point loops only do
    // matrix-vector products. This is the main playback cost lever.
    std::vector<std::vector<formaview::Matrix4>> clusterMatrices(
        d.meshes.size());
    for (std::size_t mi = 0; mi < d.meshes.size(); ++mi) {
      const auto &clusters = d.skinClusters.at(mi);
      auto &matrices = clusterMatrices[mi];
      matrices.reserve(clusters.size());
      for (const auto &cluster : clusters)
        matrices.push_back(skeletal::multiply(
            worlds.at(cluster.node),
            cluster.geometryToBone));
    }
    // Rigid fallback (node-animated meshes without skin weights): unweighted
    // points follow their instance node's animated world matrix relative to
    // the static pose. Skinned points never touch this path.
    const auto toQMatrix = [](const formaview::Matrix4 &m) {
      QMatrix4x4 out;
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
          out(r, c) = static_cast<float>(m[static_cast<std::size_t>(r * 4 + c)]);
      return out;
    };
    const auto inverseDisplay = display.inverted();
    std::vector<QMatrix4x4> rigidDeltas(d.meshes.size());
    if (!d.animNodeParents.empty() &&
        d.animNodeLocals.size() == d.animNodeParents.size()) {
      const auto staticWorlds =
          chainWorlds(d.animNodeLocals, d.animNodeParents);
      for (std::size_t mi = 0; mi < d.meshes.size(); ++mi) {
        const auto node = d.meshInstanceNodes.at(mi);
        if (node >= worlds.size() || node >= staticWorlds.size())
          continue;
        bool invertible = false;
        const auto rest = toQMatrix(staticWorlds[node]).inverted(&invertible);
        if (invertible)
          rigidDeltas[mi] = toQMatrix(worlds[node]) * rest;
      }
    }
    // Animated morph weights: per mesh, morphed local positions and morphed
    // point normals (base + weighted deltas), evaluated for this pose.
    std::vector<std::vector<double>> morphWeights;
    if (d.morphWeightsAt) morphWeights = d.morphWeightsAt(clip, seconds);
    std::vector<std::vector<skeletal::Vec3>> morphedPositions(d.meshes.size());
    std::vector<std::vector<skeletal::Vec3>> morphedNormals(d.meshes.size());
    for (std::size_t mi = 0; mi < d.meshes.size(); ++mi) {
      const auto &mesh = d.meshes[mi];
      if (!mesh.morphAnimated || mi >= morphWeights.size() ||
          morphWeights[mi].size() != mesh.morphTargets.size())
        continue;
      morphedPositions[mi].resize(mesh.raw.size());
      morphedNormals[mi].resize(mesh.raw.size());
      for (std::size_t p = 0; p < mesh.raw.size(); ++p) {
        skeletal::Vec3 position = mesh.raw[p];
        skeletal::Vec3 normal = p < d.pointNormals.at(mi).size()
                                    ? d.pointNormals.at(mi).at(p)
                                    : skeletal::Vec3{};
        for (std::size_t t = 0; t < mesh.morphTargets.size(); ++t) {
          const auto weight = static_cast<float>(morphWeights[mi][t]);
          if (weight == 0.f) continue;
          if (t < mesh.morphTargets.size() &&
              mesh.morphTargets[t].positionDeltas.size() == mesh.raw.size()) {
            const auto &delta = mesh.morphTargets[t].positionDeltas[p];
            position.x += delta.x * weight;
            position.y += delta.y * weight;
            position.z += delta.z * weight;
          }
          if (!mesh.morphTargets[t].normalDeltas.empty() &&
              mesh.morphTargets[t].normalDeltas.size() == mesh.raw.size() &&
              p < d.pointNormals.at(mi).size()) {
            const auto &delta = mesh.morphTargets[t].normalDeltas[p];
            normal.x += delta.x * weight;
            normal.y += delta.y * weight;
            normal.z += delta.z * weight;
          }
        }
        morphedPositions[mi][p] = position;
        morphedNormals[mi][p] = normal;
      }
    }
    const auto skinPoint = [&](std::size_t mesh, std::size_t point) {
      skeletal::Vec3 result{};
      double sum = 0;
      const auto &source = d.meshes.at(mesh);
      const skeletal::Vec3 &p =
          !morphedPositions.at(mesh).empty()
              ? morphedPositions.at(mesh).at(point)
              : source.animationPositions.empty()
                    ? source.raw.at(point)
                    : source.animationPositions.at(point);
      for (const auto &slot : d.skinSlots.at(mesh).at(point)) {
        const auto &skin = clusterMatrices.at(mesh).at(slot.first);
        const skeletal::Vec3 moved{
            skin[0] * p.x + skin[1] * p.y + skin[2] * p.z + skin[3],
            skin[4] * p.x + skin[5] * p.y + skin[6] * p.z + skin[7],
            skin[8] * p.x + skin[9] * p.y + skin[10] * p.z + skin[11]};
        result.x += moved.x * slot.second;
        result.y += moved.y * slot.second;
        result.z += moved.z * slot.second;
        sum += slot.second;
      }
      if (sum > 1e-15) {
        result.x /= sum;
        result.y /= sum;
        result.z /= sum;
        return result;
      }
      // Unweighted point: rigid node animation moves the static world
      // position by the instance node's animated delta; a static node
      // yields the identity delta and the point stays as decoded.
      if (!morphedPositions.at(mesh).empty()) {
        const auto node = d.meshInstanceNodes.at(mesh);
        QMatrix4x4 world;
        for (int r = 0; r < 4; ++r)
          for (int c = 0; c < 4; ++c)
            world(r, c) = static_cast<float>(
                worlds.at(node)[static_cast<std::size_t>(r * 4 + c)]);
        const auto &local = morphedPositions.at(mesh).at(point);
        const auto moved = world.map(QVector3D(static_cast<float>(local.x),
                                               static_cast<float>(local.y),
                                               static_cast<float>(local.z)));
        return skeletal::Vec3{moved.x(), moved.y(), moved.z()};
      }
      const auto &rest = d.meshes.at(mesh).points.at(point);
      const auto moved = rigidDeltas.at(mesh).map(inverseDisplay.map(rest));
      return skeletal::Vec3{moved.x(), moved.y(), moved.z()};
    };
    const auto skinNormal = [&](std::size_t mesh, std::size_t point,
                                QVector3D fallback) {
      QVector3D n(0, 0, 0);
      if (!morphedNormals.at(mesh).empty()) {
        const auto &morphed = morphedNormals.at(mesh).at(point);
        n = QVector3D(static_cast<float>(morphed.x),
                      static_cast<float>(morphed.y),
                      static_cast<float>(morphed.z));
      } else
        n = QVector3D(d.pointNormals.at(mesh).at(point).x,
                      d.pointNormals.at(mesh).at(point).y,
                      d.pointNormals.at(mesh).at(point).z);
      if (d.skinSlots.at(mesh).at(point).empty() ||
          (n.x() == 0 && n.y() == 0 && n.z() == 0))
        return fallback;
      QMatrix4x4 blended;
      blended.fill(0);
      double sum = 0;
      for (const auto &slot : d.skinSlots.at(mesh).at(point)) {
        const auto &skin = clusterMatrices.at(mesh).at(slot.first);
        for (int r = 0; r < 4; ++r)
          for (int c = 0; c < 4; ++c)
            blended(r,c) += static_cast<float>(skin[static_cast<std::size_t>(r*4+c)] * slot.second);
        sum += slot.second;
      }
      if (sum <= 1e-15) return fallback;
      blended /= static_cast<float>(sum);
      bool invertible = false;
      const auto inverse = blended.inverted(&invertible);
      if (!invertible) return fallback;
      const auto mapped = display.mapVector(inverse.transposed().mapVector(
          n));
      const auto length = mapped.length();
      return length > 1e-12F ? mapped / length : fallback;
    };
    QVector3D mappedLow(1e30F, 1e30F, 1e30F), mappedHigh(-1e30F, -1e30F, -1e30F);
    std::uint64_t sourcePoints = 0;
    for (std::size_t mi = 0; mi < d.meshes.size(); ++mi) {
      const auto &mesh = d.meshes[mi];
      sourcePoints += mesh.raw.size();
      f->vertices.reserve(f->vertices.size() + mesh.triangles.size());
      for (std::size_t t = 0; t + 2 < mesh.triangles.size(); t += 3) {
        if (t % 3072 == 0) cancelled();
        const std::size_t corners[3] = {
            static_cast<std::size_t>(mesh.triangles[t]),
            static_cast<std::size_t>(mesh.triangles[t + 1]),
            static_cast<std::size_t>(mesh.triangles[t + 2])};
        QVector3D positions[3];
        for (int k = 0; k < 3; ++k) {
          const auto point = mesh.corners.at(corners[k]);
          const auto &moved = skinPoint(mi, point);
          positions[k] = (display.map(QVector3D(static_cast<float>(moved.x),
                                                static_cast<float>(moved.y),
                                                static_cast<float>(moved.z))) -
                          d.center) /
                         std::max(d.radius, 1e-6F);
          for (int e = 0; e < 3; ++e)
            if (!std::isfinite(positions[k][e]))
              throw skeletal::Failure(
                  ErrorCode::InvalidArgument,
                  "Non-finite driven display position");
          mappedLow.setX(std::min(mappedLow.x(), positions[k].x()));
          mappedLow.setY(std::min(mappedLow.y(), positions[k].y()));
          mappedLow.setZ(std::min(mappedLow.z(), positions[k].z()));
          mappedHigh.setX(std::max(mappedHigh.x(), positions[k].x()));
          mappedHigh.setY(std::max(mappedHigh.y(), positions[k].y()));
          mappedHigh.setZ(std::max(mappedHigh.z(), positions[k].z()));
        }
        auto flat = QVector3D::crossProduct(positions[1] - positions[0],
                                            positions[2] - positions[0]);
        const auto length = flat.length();
        flat = length > 1e-12F ? flat / length : QVector3D{0, 0, 1};
        for (int k = 0; k < 3; ++k) {
          const auto point = mesh.corners.at(corners[k]);
          const auto normal = skinNormal(mi, point, flat);
          f->vertices.push_back({positions[k].x(), positions[k].y(),
                                 positions[k].z(), 0, 0, 0, normal.x(),
                                 normal.y(), normal.z()});
        }
      }
    }
    if (f->vertices.empty())
      throw skeletal::Failure(ErrorCode::InvalidArgument,
                              "Driven model has no renderable triangles");
    f->matrix.setToIdentity();
    f->displayLow = mappedLow;
    f->displayHigh = mappedHigh;
    f->hasDisplayBounds = true;
    f->sourcePoints = sourcePoints;
    appendGuides(*f, d);
    return f;
  }
  if (worlds.size() != d.boneParents.size())
    throw skeletal::Failure(ErrorCode::Internal,
                            "Animation pose does not match skeleton");
  auto f = std::make_shared<render::Frame>();
    f->worlds = worlds;
  f->revision = revision;
  f->clip=static_cast<int>(clip);f->seconds=seconds;
  f->lines = true;
  f->cacheable = false; // Per-frame payload never enters the frame cache.
  f->rgba = {0.874F, 0.675F, 0.384F, 1.F};
  const auto position = [&](std::size_t bone) {
    const auto &m = worlds[bone];
    return QVector3D(static_cast<float>(m[3]), static_cast<float>(m[7]),
                     static_cast<float>(m[11]));
  };
  for (const auto &m : worlds)
    for (int e : {0, 1, 2, 3, 7, 11, 15})
      if (!std::isfinite(m[static_cast<std::size_t>(e)]))
        throw skeletal::Failure(ErrorCode::InvalidArgument,
                                "Non-finite animation pose");
  const auto push = [&](const QVector3D &a, const QVector3D &b) {
    f->vertices.push_back({a.x(), a.y(), a.z(), 0, 0, 0});
    f->vertices.push_back({b.x(), b.y(), b.z(), 0, 0, 0});
  };
  const float cross = std::max(d.radius, 1e-6F) * 0.012F;
  for (std::size_t i = 0; i < worlds.size(); ++i) {
    const auto joint = position(i);
    const auto parent = d.boneParents[i];
    if (parent >= 0 && static_cast<std::size_t>(parent) < worlds.size())
      push(position(static_cast<std::size_t>(parent)), joint);
    push(joint + QVector3D(cross, 0, 0), joint - QVector3D(cross, 0, 0));
    push(joint + QVector3D(0, cross, 0), joint - QVector3D(0, cross, 0));
  }
  if (f->vertices.empty())
    throw skeletal::Failure(ErrorCode::InvalidArgument,
                            "Animation skeleton has no renderable segments");
  const auto display = d.displayBasis;
  QVector3D mappedLow(1e30F, 1e30F, 1e30F), mappedHigh(-1e30F, -1e30F, -1e30F);
  for (auto &vertex : f->vertices) {
    // ModelView supplies its own camera matrix, so normalization belongs in
    // the immutable vertex payload, exactly as it does for mesh documents.
    const auto mapped =
        (display.map(QVector3D(vertex.x, vertex.y, vertex.z)) - d.center) /
        std::max(d.radius, 1e-6F);
    vertex.x = mapped.x();
    vertex.y = mapped.y();
    vertex.z = mapped.z();
    for (int e = 0; e < 3; ++e) {
      if (!std::isfinite(mapped[e]))
        throw skeletal::Failure(ErrorCode::InvalidArgument,
                                "Non-finite animation display position");
      mappedLow[e] = std::min(mappedLow[e], mapped[e]);
      mappedHigh[e] = std::max(mappedHigh[e], mapped[e]);
    }
  }
  f->matrix.setToIdentity();
  f->displayLow = mappedLow;
  f->displayHigh = mappedHigh;
  f->hasDisplayBounds = true;
  f->sourcePoints = worlds.size();
  appendGuides(*f, d);
  return f;
}
