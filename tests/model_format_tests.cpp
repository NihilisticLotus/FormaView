#include "formaview/fbx.hpp"
#include "model_document.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QCryptographicHash>
#include <cmath>
#include <iostream>
#include <utility>

namespace {
void require(bool test, const char *reason) {
  if (!test)
    throw std::runtime_error(reason);
}
bool close(double a, double b) { return std::abs(a - b) < 1e-6; }
} // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  if (app.arguments().size() == 3 && app.arguments()[1] == "--mtl-scope") {
    try {
      const auto root = app.arguments()[2];
      require(!QFileInfo::exists(root) && QDir().mkpath(root + "/input") &&
                  QDir().mkpath(root + "/outside"), "Create isolated MTL fixture");
      const QByteArray material("newmtl Test\nKd 0.2 0.7 0.4\n");
      const auto hash = QString::fromLatin1(QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex());
      const auto save = [](const QString &path, const QByteArray &bytes) {
        QFile file(path);
        require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(bytes) == bytes.size(), "Write MTL fixture");
      };
      save(root + "/input/local.mtl", material);
      save(root + "/outside/secret.mtl", material);
      bool passed = true;
      int index = 0;
      for (const auto &reference : QStringList{"local.mtl", "../outside/secret.mtl", root + "/outside/secret.mtl", root + "/input/local.mtl"}) {
        const auto obj = root + "/input/source" + QString::number(index) + ".obj";
        save(obj, "mtllib " + reference.toUtf8() + "\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl Test\nf 1 2 3\n");
        const auto document = readModelDocument(obj, {});
        bool loaded = false, hashed = false;
        for (const auto &value : document->resources) {
          const auto resource = value.toMap();
          loaded |= resource.value("loaded").toBool();
          hashed |= resource.value("detail").toString() == hash;
        }
        const bool legal = index++ == 0;
        const bool valid = !document->resources.isEmpty() && (legal ? loaded && hashed : !loaded && !hashed);
        std::cout << reference.toStdString() << " loaded=" << loaded << " hashed=" << hashed << " valid=" << valid << '\n';
        passed &= valid;
      }
      require(passed, "OBJ material references escaped the input directory or absolute-path policy");
      return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
  }
  if (app.arguments().size() != 2)
    return 64;
  const QDir root(app.arguments()[1]);
  int assertions = 0;
  const auto test = [&](bool value, const char *reason) {
    ++assertions;
    require(value, reason);
  };
  try {
    for (const auto &name : {"external.gltf", "embedded.glb"}) {
      const auto d = readModelDocument(root.filePath(name), {});
      test(d->meshes.size() == 1, "one visible glTF primitive");
      const auto &m = d->meshes[0];
      test(m.raw.size() == 3 && m.triangles.size() == 3,
           "triangle cardinality");
      test(close(m.raw[0].x, 0), "source position unchanged");
      test(close(m.points[0].x(), 3) && close(m.points[0].y(), 4) &&
               close(m.points[0].z(), 5),
           "hierarchy world translation");
      test(close(m.points[1].x(), 5), "hierarchy scale");
      test(m.uvNames.size() == 2 && close(m.uvs[0][1].x(), 2) &&
               close(m.uvs[0][2].y(), -1),
           "all UV sets and out-of-range coordinates");
      test(close(m.colours[0].x(), .25) && close(m.colours[0].y(), .5),
           "linear source material factors");
      test(m.textures[0] && m.textures[0]->width == 2 &&
               m.textures[0]->rgba[0] == 1,
           "embedded image actually decoded");
      test(m.materialUv[0] == 1 && m.uvTopOrigin && m.unlit && d->linearColour,
           "source texture UV and colour meaning");
      test(d->nodes.size() == 2, "source auxiliary parent retained");
    }
    {
      const auto d =
          readModelDocument(root.filePath("sparse-interleaved.gltf"), {});
      test(close(d->meshes[0].raw[1].x, 2), "sparse first value");
      test(close(d->meshes[0].raw[2].y, 2),
           "sparse packed values over interleaved base");
    }
    for (const auto &name : {"coverage-mask.gltf", "coverage-blend.gltf"}) {
      const auto d = readModelDocument(root.filePath(name), {});
      const auto &m = d->meshes[0];
      test(close(m.colours[0].x(), .25 * 128 / 255) &&
               close(m.colours[0].y(), .5),
           "linear vertex colour multiplied by material factor");
      test(close(m.alphas[0], .5 * 128 / 255) &&
               close(m.alphaCutoffs[0],
                     QString(name).contains("mask") ? .3 : .5),
           "source alpha and cutoff");
      test(m.alphaModes[0] == (QString(name).contains("mask") ? 1 : 2),
           "opaque/mask/blend separated");
      const auto &texture = *m.textures[0];
      test(texture.wrapU == 1 && texture.wrapV == 2 && !texture.linearMag &&
               texture.linearMin && texture.mipmaps && !texture.linearMip,
           "declared sampler including mip filter");
      ModelRenderState state;
      state.bindings = {-1};
      state.uvs = {0};
      const auto frames = buildModelFrames(*d, state, 1, {});
      test(frames.model->draws[0].blend == (m.alphaModes[0] == 2) &&
               close(frames.model->vertices[0].alpha, m.alphas[0]),
           "GPU frame retains alpha semantics");
    }
    {
      const auto d = readModelDocument(root.filePath("morph.gltf"), {});
      const auto &m = d->meshes[0];
      test(close(m.raw[0].z, 0) && close(m.points[0].z(), 1),
           "default morph does not overwrite source position");
      test(close(m.normals[0].y(), std::sqrt(.5)) &&
               close(m.normals[0].z(), std::sqrt(.5)),
           "default morph normal before world transform");
    }
    {
      const auto d = readModelDocument(root.filePath("morph-multiple-primitives.gltf"), {});
      test(d->meshes.size() == 4 && bool(d->morphWeightsAt), "animated and instanced morph primitives loaded");
      for (const auto [time, expected] : {std::pair{-1.0, .25}, std::pair{.5, .5},
                                          std::pair{1.0, .75}, std::pair{2.0, .75}}) {
        const auto weights = d->morphWeightsAt(0, time);
        test(weights.size() == 4 && weights[0].size() == 1 && weights[1].size() == 1 &&
                 weights[2].size() == 1 && weights[3].size() == 1 && close(weights[0][0], expected) &&
                 close(weights[1][0], expected) && close(weights[2][0], .6) && close(weights[3][0], .4),
             "morph animation drives all target primitives and retains other defaults");
      }
      const auto defaults = d->morphWeightsAt(1, .5);
      test(defaults.size() == 4 && defaults[0].size() == 1 && defaults[1].size() == 1 &&
               defaults[2].size() == 1 && defaults[3].size() == 1 && close(defaults[0][0], .5) &&
               close(defaults[1][0], .5) && close(defaults[2][0], .6) && close(defaults[3][0], .4),
           "clip without morph channels retains every static default");
    }
    {
      const auto d = readModelDocument(root.filePath("morph-large-normal.gltf"), {});
      const auto &base = d->meshes[0].morphBaseNormals[0];
      test(close(base.x, 0) && close(base.y, 0) && close(base.z, 1),
           "unmorphed base normal survives a large static morph delta");
    }
    {
      // A sparse visible triangle can coexist with many source points. The
      // dense GPU morph texture must be rejected before allocating it.
      auto d = readModelDocument(root.filePath("morph.gltf"), {});
      auto &mesh = d->meshes[0];
      constexpr std::size_t points = 140000;
      mesh.raw.resize(points);
      mesh.morphTargets.clear();
      mesh.morphTargets.resize(32);
      mesh.morphAnimated = true;
      d->nativeAnimated = true;
      d->animNodeParents = {-1};
      d->animNodeLocals = {formaview::Matrix4{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}};
      d->skinClusters.resize(1);
      d->skinSlots.resize(1);
      d->skinSlots[0].resize(points);
      d->pointNormals.resize(1);
      d->pointNormals[0].resize(points);
      d->meshInstanceNodes = {0};
      d->morphWeightsAt = [](std::size_t, double) { return std::vector<std::vector<double>>{}; };
      ModelRenderState state;
      state.bindings = {-1}; state.uvs = {0}; state.visible = {true};
      bool rejected = false;
      try {
        (void)buildModelGpuGeometry(*d, state, 1, {});
      } catch (const std::runtime_error &e) {
        rejected = std::string(e.what()).find("GPU morph memory budget exceeded") != std::string::npos;
      }
      test(rejected, "GPU morph budget is enforced before allocation");
      mesh.raw.resize(128);
      d->skinSlots[0].resize(128);
      d->pointNormals[0].resize(128);
      const auto geometry = buildModelGpuGeometry(*d, state, 2, {});
      test(geometry && geometry->frames.model && geometry->frames.model->skin &&
               geometry->frames.model->skin->morph.size() == 128 * 32 * 2 * 4,
           "GPU morph payload within budget is retained");
    }
    {
      const auto d = readModelDocument(root.filePath("all-eight.gltf"), {});
      const auto &m = d->meshes[0];
      test(m.skins[0].weights[0].size() == 8, "all eight source influences");
      test(close(m.skins[0].weights[0][0].value, 32768.0 / 65535),
           "source UNorm interpretation");
      double sum = 0;
      for (const auto &w : m.skins[0].weights[0])
        sum += w.value;
      test(close(sum, 65280.0 / 65535), "source sum not normalized");
      test(close(m.points[0].x(), 2) && close(m.points[1].x(), 3),
           "default skin uses joint world and inverse bind, not mesh node "
           "transform");
      test(m.skins[0].bones[0] != m.skins[0].bones[1],
           "repeated joint names remain distinguishable");
    }
    for (const auto &name :
         {"invalid-pair.gltf", "invalid-range.gltf", "required-draco.gltf"}) {
      bool rejected = false;
      try {
        readModelDocument(root.filePath(name), {});
      } catch (const std::exception &) {
        rejected = true;
      }
      test(rejected, "invalid/unsupported glTF explicitly rejected");
    }
    for (const auto *name : {"morph-budget.gltf", "morph-normal-budget.gltf"}) {
      bool budgetRejected = false;
      try {
        readModelDocument(root.filePath(name), {});
      } catch (const formaview::skeletal::Failure &e) {
        budgetRejected = e.error.code == formaview::ErrorCode::ResourceLimit;
      }
      test(budgetRejected, "morph position and normal arrays are charged before allocation");
    }
    {
      std::stop_source stop;
      stop.request_stop();
      bool rejected = false;
      try {
        readModelDocument(root.filePath("external.gltf"), stop.get_token());
      } catch (const formaview::skeletal::Failure &e) {
        rejected = e.error.code == formaview::ErrorCode::Cancelled;
      }
      test(rejected, "cancelled input is not published");
    }
    {
      const auto d = readModelDocument(root.filePath("concave.obj"), {});
      test(d->meshes.size() == 1 && d->meshes[0].triangles.size() == 9,
           "OBJ concave polygon triangulated");
      const auto &m = d->meshes[0];
      double area = 0;
      for (std::size_t i = 0; i < m.triangles.size(); i += 3) {
        const auto a = m.points[m.corners[m.triangles[i]]],
                   b = m.points[m.corners[m.triangles[i + 1]]],
                   c = m.points[m.corners[m.triangles[i + 2]]];
        area += QVector3D::crossProduct(b - a, c - a).length() * .5;
      }
      test(close(area, 3), "OBJ concave polygon area");
      bool colour = false;
      for (const auto &c : m.colours)
        colour |= close(c.x(), .25) && close(c.y(), .5) && close(c.z(), .75);
      test(colour, "MTL diffuse factors decoded");
      test(!d->resources.empty() && d->resources[0].toMap()["loaded"].toBool(),
           "actual MTL read recorded");
      test(m.textures[0] && m.textures[0]->width == 2 &&
               m.textures[0]->height == 2,
           "declared MTL base-colour image is actually decoded");
    }
    {
      const auto d = readModelDocument(root.filePath("missing-mtl.obj"), {});
      test(!d->meshes.empty() && !d->resources.empty() &&
               !d->resources[0].toMap()["loaded"].toBool(),
           "missing MTL preserves geometry with explicit issue");
    }
  } catch (const std::exception &e) {
    std::cerr << "FAILED after " << assertions << " assertions: " << e.what()
              << '\n';
    return 1;
  }
  std::cout << "PASSED " << assertions << " model format assertions\n";
  return 0;
}
