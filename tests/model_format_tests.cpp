#include "formaview/fbx.hpp"
#include "model_document.hpp"
#include <QCoreApplication>
#include <QDir>
#include <cmath>
#include <iostream>

namespace {
void require(bool test, const char *reason) {
  if (!test)
    throw std::runtime_error(reason);
}
bool close(double a, double b) { return std::abs(a - b) < 1e-6; }
} // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
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
