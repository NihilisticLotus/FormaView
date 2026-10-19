#pragma once
#include "formaview/render_item.hpp"
#include "formaview/skeletal.hpp"
#include <QHash>
#include <QVariantList>
#include <QVector3D>
#include <map>
#include <stop_token>

struct ModelWeight {
  int bone;
  double value;
};
struct ModelSkin {
  QString name, id, method;
  QStringList bones;
  QVariantList palette;
  std::vector<std::vector<ModelWeight>> weights;
};
struct ModelClassification {
  QString group;
  int level{};
  bool collision{};
  bool operator==(const ModelClassification &) const = default;
};
struct ModelMesh {
  QString name, id, nodeId;
  bool uvTopOrigin{};
  bool unlit{};
  ModelClassification classification;
  QString classificationEvidence, classificationCandidate;
  QVector3D boundsLow, boundsHigh;
  std::vector<formaview::skeletal::Vec3> raw;
  std::vector<QVector3D> points;
  std::vector<std::uint32_t> corners, triangles;
  std::vector<std::uint64_t> faces;
  std::vector<std::uint64_t> triangleFaces;
  std::vector<std::array<std::uint64_t, 3>> triangleCorners;
  QStringList uvNames;
  std::vector<std::vector<QPointF>> uvs;
  std::vector<std::array<double, 4>> uvBounds;
  std::vector<bool> invalidUVs;
  std::vector<QVector3D> normals, colours;
  std::vector<float> alphas;
  std::vector<int> alphaModes;
  std::vector<float> alphaCutoffs;
  std::vector<ModelSkin> skins;
  QStringList materials, references, texturePaths;
  std::vector<int> materialUv;
  std::vector<std::array<float, 5>>
      uvTransforms; // offset U/V, scale U/V, rotation.
  std::vector<std::array<float, 6>> uvAffine;
  std::vector<std::uint32_t> cornerMaterial;
  std::vector<std::shared_ptr<const formaview::render::Texture>> textures;
};
struct ModelDocument {
  std::vector<ModelMesh> meshes;
  QVariantList nodes, resources;
  QHash<QString, QVariantMap> nodeById;
  QStringList warnings;
  bool linearColour{};
  QString hash, coordinates, name;
  QMatrix4x4 displayBasis;
  QVector3D center;
  float radius{1};
  std::uint64_t points{}, triangles{};
};

std::shared_ptr<ModelDocument> readGltfModel(const QString &, std::stop_token);
std::shared_ptr<ModelDocument> readModelDocument(const QString &,
                                                 std::stop_token);
struct ModelTextureLink {
  std::shared_ptr<const formaview::render::Texture> pixels;
  QString path;
  int uv{-1};
};
struct ModelRenderState {
  QString highlightedNode;
  int mode{}, mesh{}, bone{}, material{};
  bool materialOnly{};
  bool isolated{};
  int collisionMode{};
  std::vector<ModelClassification> classes;
  std::map<QString, int> levels;
  std::vector<bool> visible;
  std::vector<int> bindings, uvs;
  std::vector<std::map<int, ModelTextureLink>> textures;
};
struct ModelFrames {
  std::shared_ptr<const formaview::render::Frame> model, uv;
  int mode{};
};
ModelFrames buildModelFrames(const ModelDocument &, const ModelRenderState &,
                             std::uint64_t, std::stop_token);
