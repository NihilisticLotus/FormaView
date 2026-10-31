#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <ufbx.h>

namespace {
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
QString name(ufbx_string s) {
  return QString::fromUtf8(s.data, static_cast<qsizetype>(s.length));
}
using ScenePtr = std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)>;
ScenePtr animationScene(const QString &path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "Read animation audit input");
  const auto bytes = file.readAll();
  ufbx_load_opts opts{};
  opts.load_external_files = false;
  opts.ignore_geometry = true;
  opts.ignore_embedded = true;
  opts.temp_allocator.memory_limit = 2ULL * 1024 * 1024 * 1024;
  opts.result_allocator.memory_limit = 2ULL * 1024 * 1024 * 1024;
  ufbx_error error{};
  ScenePtr result(ufbx_load_memory(bytes.constData(),
                                   static_cast<size_t>(bytes.size()), &opts,
                                   &error),
                  ufbx_free_scene);
  if (!result) {
    char text[2048];
    ufbx_format_error(text, sizeof(text), &error);
    throw std::runtime_error(text);
  }
  return result;
}
QString nodePath(const ufbx_node *node) {
  QString path;
  for (auto *p = node; p; p = p->parent)
    path.prepend('/' + name(p->name));
  return path;
}
QString propertyKey(const ufbx_anim_prop &p) {
  const auto *node = ufbx_as_node(p.element);
  return QString::number(p.element->type) + ':' +
         (node ? nodePath(node) : name(p.element->name)) + ':' +
         name(p.prop_name);
}
void auditAnimations(const QString &sourcePath, const QString &directory,
                     const QString &report) {
  const auto source = animationScene(sourcePath);
  std::map<QString, const ufbx_anim_stack *> clips;
  for (const auto *stack : source->anim_stacks)
    require(clips.emplace(name(stack->name), stack).second,
            "Unique source clip names required for audit");
  std::map<QString, const ufbx_node *> sourceNodes;
  for (const auto *node : source->nodes)
    require(sourceNodes.emplace(nodePath(node), node).second,
            "Unique source node paths required for audit");
  QJsonArray results;
  quint64 keys = 0;
  double maxKey = 0, maxTime = 0, maxSpan = 0, maxTangent = 0, maxActiveTangent = 0,
         maxCurve = 0, maxTransform = 0;
  std::set<QString> seen;
  for (const auto &file :
       QDir(directory).entryInfoList({"*.fbx"}, QDir::Files, QDir::Name)) {
    const auto output = animationScene(file.absoluteFilePath());
    require(output->anim_stacks.count == 1 && output->meshes.count == 0,
            "Animation output has one clip and no mesh");
    const auto *out = output->anim_stacks[0];
    const auto it = clips.find(name(out->name));
    require(it != clips.end(), "Original clip name retained");
    require(seen.insert(it->first).second, "No duplicate exported clip");
    const auto *original = it->second;
    maxSpan =
        std::max({maxSpan, std::abs(original->time_begin - out->time_begin),
                  std::abs(original->time_end - out->time_end)});
    if (maxSpan > 1.001 / 46186158000.0) {
      std::cerr << "span mismatch clip=" << it->first.toStdString()
                << " start=" << std::setprecision(17) << original->time_begin
                << '/' << out->time_begin << " end=" << original->time_end
                << '/' << out->time_end << '\n';
      throw std::runtime_error(
          "Clip span differs by more than one FBX clock tick");
    }
    require(source->settings.frames_per_second ==
                output->settings.frames_per_second,
            "Source frame rate retained");
    require(original->layers.count == out->layers.count,
            "Animation layer count retained");
    quint64 clipKeys = 0;
    for (size_t l = 0; l < original->layers.count; ++l) {
      const auto *a = original->layers[l], *b = out->layers[l];
      require(a->weight == b->weight && a->blended == b->blended &&
                  a->additive == b->additive &&
                  a->compose_rotation == b->compose_rotation &&
                  a->compose_scale == b->compose_scale,
              "Layer blending retained");
      std::map<QString, const ufbx_anim_prop *> properties;
      for (const auto &p : b->anim_props)
        require(properties.emplace(propertyKey(p), &p).second,
                "Unique output animation property");
      require(a->anim_props.count == b->anim_props.count,
              "Animation property count retained");
      for (const auto &p : a->anim_props) {
        const auto match = properties.find(propertyKey(p));
        require(match != properties.end(), "Animated node/property retained");
        for (int channel = 0; channel != 3; ++channel) {
          const auto *ac = p.anim_value->curves[channel],
                     *bc = match->second->anim_value->curves[channel];
          require(bool(ac) == bool(bc), "Curve channel presence retained");
          if (!ac)
            continue;
          require(ac->keyframes.count == bc->keyframes.count,
                  "All curve keys retained");
          require(ac->pre_extrapolation.mode == bc->pre_extrapolation.mode &&
                      ac->post_extrapolation.mode ==
                          bc->post_extrapolation.mode &&
                      ac->pre_extrapolation.repeat_count ==
                          bc->pre_extrapolation.repeat_count &&
                      ac->post_extrapolation.repeat_count ==
                          bc->post_extrapolation.repeat_count,
                  "Extrapolation retained");
          for (size_t k = 0; k < ac->keyframes.count; ++k) {
            const auto &ak = ac->keyframes[k], &bk = bc->keyframes[k];
            ++keys;
            ++clipKeys;
            maxTime = std::max(maxTime, std::abs(ak.time - bk.time));
            // Native SDK round-trip rounds legacy key times by at most one
            // FBX clock tick; the full-load baseline exhibits this too.
            if (std::abs(ak.time - bk.time) > 1.001 / 46186158000.0 ||
                ak.interpolation != bk.interpolation) {
              std::cerr << "key mismatch clip=" << it->first.toStdString()
                        << " property=" << propertyKey(p).toStdString()
                        << " channel=" << channel << " key=" << k << '/'
                        << ac->keyframes.count
                        << " time=" << std::setprecision(17) << ak.time << '/'
                        << bk.time << " interpolation=" << ak.interpolation
                        << '/' << bk.interpolation << '\n';
              throw std::runtime_error(
                  "Key time exceeds one FBX tick or interpolation changed");
            }
            maxKey = std::max(maxKey, std::abs(ak.value - bk.value));
            for (const auto delta :
                 {ak.left.dx - bk.left.dx, ak.left.dy - bk.left.dy,
                  ak.right.dx - bk.right.dx, ak.right.dy - bk.right.dy})
              maxTangent = std::max(maxTangent, std::abs(double(delta)));
            // Linear/constant segments do not evaluate stored Bezier handles.
            // Keep their raw differences in the report, but enforce accuracy
            // on active cubic/slope handles and on every segment's values.
            const auto tangentError = [](ufbx_tangent a, ufbx_tangent b) {
              return std::max(std::abs(double(a.dx) - b.dx),
                              std::abs(double(a.dy) - b.dy));
            };
            const bool leftActive =
                k ? ac->keyframes[k - 1].interpolation ==
                        UFBX_INTERPOLATION_CUBIC
                  : ac->pre_extrapolation.mode == UFBX_EXTRAPOLATION_SLOPE;
            const bool rightActive =
                k + 1 < ac->keyframes.count
                    ? ak.interpolation == UFBX_INTERPOLATION_CUBIC
                    : ac->post_extrapolation.mode == UFBX_EXTRAPOLATION_SLOPE;
            if (leftActive)
              maxActiveTangent =
                  std::max(maxActiveTangent, tangentError(ak.left, bk.left));
            if (rightActive)
              maxActiveTangent =
                  std::max(maxActiveTangent, tangentError(ak.right, bk.right));
            require(std::isfinite(ak.value) && std::isfinite(bk.value),
                    "Finite key values");
            if (k + 1 < ac->keyframes.count)
              for (int sample = 1; sample != 4; ++sample) {
                const auto time =
                    ak.time +
                    (ac->keyframes[k + 1].time - ak.time) * sample / 4;
                const auto valueA = ufbx_evaluate_curve(ac, time, 0);
                const auto valueB = ufbx_evaluate_curve(bc, time, 0);
                require(std::isfinite(valueA) && std::isfinite(valueB),
                        "Finite segment values");
                maxCurve = std::max(maxCurve, std::abs(valueA - valueB));
              }
          }
        }
      }
    }
    require(output->nodes.count == source->nodes.count,
            "Node hierarchy size retained");
    for (const auto *node : output->nodes) {
      const auto match = sourceNodes.find(nodePath(node));
      require(match != sourceNodes.end(), "Node hierarchy retained");
      for (int sample = 0; sample != 5; ++sample) {
        const auto t = original->time_begin +
                       (original->time_end - original->time_begin) * sample / 4;
        const auto a = ufbx_evaluate_transform(original->anim, match->second,
                                               t),
                   b = ufbx_evaluate_transform(out->anim, node, t);
        const auto am = ufbx_transform_to_matrix(&a),
                   bm = ufbx_transform_to_matrix(&b);
        for (int c = 0; c != 4; ++c)
          for (int r = 0; r != 3; ++r)
            maxTransform = std::max(
                maxTransform, std::abs(am.cols[c].v[r] - bm.cols[c].v[r]));
      }
    }
    results.append(QJsonObject{{"file", file.fileName()},
                               {"clip", it->first},
                               {"keys", static_cast<qint64>(clipKeys)}});
    std::cout << "PASS readback " << results.size() << '/'
              << source->anim_stacks.count << '\n';
  }
  const bool passed =
      results.size() == static_cast<qsizetype>(source->anim_stacks.count) &&
      maxKey <= 1e-6 && maxActiveTangent <= 1e-6 && maxCurve <= 1e-6 &&
      maxTransform <= 1e-5;
  QFile file(report);
  require(file.open(QIODevice::WriteOnly), "Write animation audit");
  file.write(
      QJsonDocument(
          QJsonObject{
              {"passed", passed},
              {"source_clips", static_cast<qint64>(source->anim_stacks.count)},
              {"source_curves", static_cast<qint64>(source->anim_curves.count)},
              {"source_nodes", static_cast<qint64>(source->nodes.count)},
              {"compared_keys", static_cast<qint64>(keys)},
              {"maximum_key_value_error", maxKey},
              {"maximum_key_time_error_seconds", maxTime},
              {"maximum_clip_boundary_error_seconds", maxSpan},
              {"maximum_tangent_error", maxTangent},
              {"maximum_active_tangent_error", maxActiveTangent},
              {"maximum_segment_value_error", maxCurve},
              {"maximum_local_transform_error", maxTransform},
              {"clips", results}})
          .toJson());
  require(passed, "Complete independent animation readback within unchanged "
                  "numeric tolerances");
}
QJsonArray vec(ufbx_vec3 v) { return {v.x, v.y, v.z}; }
QJsonArray matrix(const ufbx_matrix &m) {
  return {m.m00, m.m01, m.m02, m.m03, m.m10, m.m11, m.m12, m.m13,
          m.m20, m.m21, m.m22, m.m23, 0,     0,     0,     1};
}
QJsonArray geometry(const ufbx_scene *scene) {
  QJsonArray out;
  for (const auto *mesh : scene->meshes) {
    QJsonArray points, corners, uv, normals, clusters, shapes, deformed;
    for (const auto v : mesh->vertices)
      points.append(vec(v));
    for (const auto i : mesh->vertex_indices)
      corners.append(static_cast<qint64>(i));
    for (size_t i = 0; i < mesh->num_indices; ++i) {
      if (mesh->vertex_uv.exists) {
        const auto v = ufbx_get_vertex_vec2(&mesh->vertex_uv, i);
        uv.append(QJsonArray{v.x, v.y});
      }
      if (mesh->vertex_normal.exists)
        normals.append(vec(ufbx_get_vertex_vec3(&mesh->vertex_normal, i)));
      if (mesh->skinned_position.exists)
        deformed.append(vec(ufbx_get_vertex_vec3(&mesh->skinned_position, i)));
    }
    for (const auto *skin : mesh->skin_deformers)
      for (const auto *c : skin->clusters) {
        QJsonArray indices, weights;
        for (auto v : c->vertices)
          indices.append(static_cast<qint64>(v));
        for (auto v : c->weights)
          weights.append(v);
        clusters.append(
            QJsonObject{{"bone", name(c->bone_node->name)},
                        {"indices", indices},
                        {"weights", weights},
                        {"geometry_to_bone", matrix(c->geometry_to_bone)},
                        {"bind_to_world", matrix(c->bind_to_world)}});
      }
    for (const auto *blend : mesh->blend_deformers)
      for (const auto *channel : blend->channels) {
        QJsonArray offsets, indices, normalOffsets;
        if (channel->target_shape) {
          for (const auto index : channel->target_shape->offset_vertices)
            indices.append(static_cast<qint64>(index));
          for (const auto v : channel->target_shape->position_offsets)
            offsets.append(vec(v));
          for (const auto v : channel->target_shape->normal_offsets)
            normalOffsets.append(vec(v));
        }
        shapes.append(QJsonObject{{"name", name(channel->name)},
                                  {"weight", channel->weight},
                                  {"indices", indices},
                                  {"normal_offsets", normalOffsets},
                                  {"offsets", offsets}});
      }
    QJsonArray materials;
    for (const auto *m : mesh->materials)
      materials.append(
          QJsonObject{{"name", name(m->name)},
                      {"diffuse", vec(m->fbx.diffuse_color.value_vec3)}});
    out.append(QJsonObject{{"name", name(mesh->name)},
                           {"points", points},
                           {"corners", corners},
                           {"uv", uv},
                           {"normals", normals},
                           {"clusters", clusters},
                           {"shapes", shapes},
                           {"materials", materials},
                           {"deformed", deformed}});
  }
  return out;
}
} // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    if(app.arguments().size()==5 && app.arguments()[1]=="--audit-animations") {
      auditAnimations(app.arguments()[2],app.arguments()[3],app.arguments()[4]);return 0;
    }
    if (app.arguments().size() != 3)
      throw std::runtime_error(
          "Usage: scene_export_probe input.fbx report.json");
    QFile file(app.arguments()[1]);
    if (!file.open(QIODevice::ReadOnly))
      throw std::runtime_error("Cannot read source");
    const auto bytes = file.readAll();
    ufbx_load_opts options{};
    options.load_external_files = false;
    options.evaluate_skinning = true;
    options.temp_allocator.memory_limit = 512ULL * 1024 * 1024;
    options.result_allocator.memory_limit = 512ULL * 1024 * 1024;
    ufbx_error error{};
    const std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> scene(
        ufbx_load_memory(bytes.data(), static_cast<size_t>(bytes.size()),
                         &options, &error),
        ufbx_free_scene);
    if (!scene) {
      char text[2048];
      ufbx_format_error(text, sizeof(text), &error);
      throw std::runtime_error(text);
    }
    QJsonArray clips, media, nodes;
    for (const auto *node : scene->nodes)
      nodes.append(QJsonObject{{"name", name(node->name)},
                               {"world", matrix(node->node_to_world)}});
    for (const auto *texture : scene->textures)
      if (texture->content.size)
        media.append(QJsonObject{
            {"name", name(texture->filename)},
            {"sha256",
             QString::fromLatin1(
                 QCryptographicHash::hash(
                     QByteArrayView(
                         static_cast<const char *>(texture->content.data),
                         static_cast<qsizetype>(texture->content.size)),
                     QCryptographicHash::Sha256)
                     .toHex())}});
    for (const auto *stack : scene->anim_stacks) {
      QJsonArray samples;
      for (int i = 0; i <= 8; ++i) {
        const double time =
            stack->time_begin + (stack->time_end - stack->time_begin) * i / 8;
        ufbx_evaluate_opts eval{};
        eval.evaluate_skinning = true;
        const std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> pose(
            ufbx_evaluate_scene(scene.get(), stack->anim, time, &eval, &error),
            ufbx_free_scene);
        if (!pose)
          throw std::runtime_error("Cannot evaluate independent FBX scene");
        QJsonArray transforms;
        for (const auto *node : pose->nodes)
          transforms.append(
              QJsonObject{{"name", name(node->name)},
                          {"world", matrix(node->node_to_world)}});
        samples.append(QJsonObject{{"time", time},
                                   {"nodes", transforms},
                                   {"meshes", geometry(pose.get())}});
      }
      clips.append(QJsonObject{{"name", name(stack->name)},
                               {"start", stack->time_begin},
                               {"end", stack->time_end},
                               {"samples", samples}});
    }
    QFile out(app.arguments()[2]);
    if (!out.open(QIODevice::WriteOnly))
      throw std::runtime_error("Cannot write report");
    out.write(QJsonDocument(QJsonObject{{"meshes", geometry(scene.get())},
                                        {"nodes", nodes},
                                        {"clips", clips},
                                        {"media", media},
                                        {"unit", scene->settings.unit_meters}})
                  .toJson());
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
