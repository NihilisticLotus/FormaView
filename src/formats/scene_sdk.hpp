#pragma once
#include "scene_export.hpp"
#include <QMap>
#include <fbxsdk.h>

namespace formaview::scene_export {
struct ManagerDelete {
  void operator()(FbxManager *p) const {
    if (p)
      p->Destroy();
  }
};
struct Scene {
  // Manager dies before callback data, streams and source buffers.
  std::filesystem::path path;
  Loader load;
  skeletal::Limits limits;
  std::stop_token stop;
  QMap<QString, QByteArray> media;
  QMap<QString, QString> media_sources;
  QString callback_error;
  QJsonArray losses;
  QJsonObject metadata;
  std::uint64_t allocated{};
  std::unique_ptr<FbxManager, ManagerDelete> manager;
  FbxScene *scene{};
  explicit Scene(const std::filesystem::path &, Loader, skeletal::Limits,
                 std::stop_token);
  void check() const;
  void reserve(std::uint64_t bytes);
  void loss(const QString &code, const QString &detail);
  std::filesystem::path relative(const QString &uri) const;
  QString image(const QString &uri, const std::optional<QByteArray> &content = {});
  FbxFileTexture *texture(const QString &key, const QString &uv = "UV0");
  void preload_media();
};
void read_obj(Scene &, const QByteArray &);
void read_gltf(Scene &, const QByteArray &);
void read_pmx(Scene &, const QByteArray &);
[[noreturn]] void invalid(const char *message);
FbxAxisSystem axis_system(const QString &);
FbxAMatrix matrix(const formaview::Matrix4 &);
void string_property(FbxObject *, const char *, const QString &);
} // namespace formaview::scene_export
