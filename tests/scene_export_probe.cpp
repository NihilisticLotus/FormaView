#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>
#include <memory>
#include <ufbx.h>

namespace {
QString name(ufbx_string s) {
  return QString::fromUtf8(s.data, static_cast<qsizetype>(s.length));
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
