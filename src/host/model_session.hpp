#pragma once
#include "formaview/render_item.hpp"
#include "model_document.hpp"
#include "model_scene.hpp"
#include "model_skeleton.hpp"
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>
#include <future>
#include <list>
#include <stop_token>

struct ModelDocument;
class ModelSession : public QObject {
  Q_OBJECT
  QML_ANONYMOUS
  Q_PROPERTY(bool available READ available NOTIFY changed)
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(bool preparing READ preparing NOTIFY changed)
  Q_PROPERTY(bool inspectable READ inspectable NOTIFY changed)
  Q_PROPERTY(QString frameError READ frameError NOTIFY changed)
  Q_PROPERTY(bool canClearTexture READ canClearTexture NOTIFY changed)
  Q_PROPERTY(QAbstractItemModel *scene READ scene CONSTANT)
  Q_PROPERTY(ModelSkeleton *skeleton READ skeleton CONSTANT)
  Q_PROPERTY(
      QVariantMap skeletonSelection READ skeletonSelection NOTIFY changed)
  Q_PROPERTY(QStringList warnings READ warnings NOTIFY changed)
  Q_PROPERTY(QVariantList resources READ resources NOTIFY changed)
  Q_PROPERTY(QStringList lodGroups READ lodGroups NOTIFY changed)
  Q_PROPERTY(QVariantList lodLevels READ lodLevels NOTIFY changed)
  Q_PROPERTY(int lodGroupIndex READ lodGroupIndex NOTIFY changed)
  Q_PROPERTY(int collisionMode READ collisionMode NOTIFY changed)
  Q_PROPERTY(QVariantMap classification READ classification NOTIFY changed)
  Q_PROPERTY(QVariantMap sourceNode READ sourceNode NOTIFY changed)
  Q_PROPERTY(QVariantMap bindingInfo READ bindingInfo NOTIFY changed)
  Q_PROPERTY(QString error READ error NOTIFY changed)
  Q_PROPERTY(QString path READ path NOTIFY changed)
  Q_PROPERTY(QVariantMap info READ info NOTIFY changed)
  Q_PROPERTY(QStringList meshes READ meshes NOTIFY changed)
  Q_PROPERTY(QStringList uvSets READ uvSets NOTIFY changed)
  Q_PROPERTY(QStringList bindings READ bindings NOTIFY changed)
  Q_PROPERTY(QStringList bones READ bones NOTIFY changed)
  Q_PROPERTY(int meshIndex READ meshIndex NOTIFY changed)
  Q_PROPERTY(int bindingIndex READ bindingIndex NOTIFY changed)
  Q_PROPERTY(int boneIndex READ boneIndex NOTIFY changed)
  Q_PROPERTY(int uvIndex READ uvIndex NOTIFY changed)
  Q_PROPERTY(int mode READ mode NOTIFY changed)
  Q_PROPERTY(bool isolated READ isolated NOTIFY changed)
  Q_PROPERTY(QVariantMap picked READ picked NOTIFY selectionChanged)
  Q_PROPERTY(QVariantList influences READ influences NOTIFY selectionChanged)
  Q_PROPERTY(QStringList materials READ materials NOTIFY changed)
  Q_PROPERTY(int materialIndex READ materialIndex NOTIFY changed)
  Q_PROPERTY(bool materialOnly READ materialOnly NOTIFY changed)
  Q_PROPERTY(QVariantMap uvBackground READ uvBackground NOTIFY changed)
  Q_PROPERTY(QString texturePath READ texturePath NOTIFY changed)
  Q_PROPERTY(QString textureReference READ textureReference NOTIFY changed)
  Q_PROPERTY(QString textureError READ textureError NOTIFY changed)
  Q_PROPERTY(bool textureBusy READ textureBusy NOTIFY changed)
public:
  explicit ModelSession(QObject *parent = nullptr);
  ~ModelSession() override;
  QStringList lodGroups() const;
  QVariantList lodLevels() const;
  int lodGroupIndex() const { return lodGroup_; }
  int collisionMode() const { return collisionMode_; }
  QVariantMap classification() const;
  QVariantMap sourceNode() const;
  QVariantMap bindingInfo() const;
  Q_INVOKABLE void selectLodGroup(int);
  Q_INVOKABLE void requestLod(int);
  Q_INVOKABLE void setCollisionMode(int);
  Q_INVOKABLE void classifyMesh(const QString &group, int level,
                                bool collision);
  Q_INVOKABLE void restoreClassification();
  bool available() const { return bool(document_); }
  QAbstractItemModel *scene() { return &scene_; }
  ModelSkeleton *skeleton() { return &skeleton_; }
  QVariantMap skeletonSelection() const;
  Q_INVOKABLE void selectSkeletonIndex(const QModelIndex &);
  QStringList warnings() const {
    return document_ ? document_->warnings : QStringList{};
  }
  QVariantList resources() const {
    return document_ ? document_->resources : QVariantList{};
  }
  Q_INVOKABLE void selectTreeIndex(const QModelIndex &index);
  Q_INVOKABLE QModelIndex treeSelection() const {
    return scene_.meshIndex(mesh_);
  }
  Q_INVOKABLE void retry();
  bool busy() const { return busy_; }
  bool preparing() const { return preparing_; }
  bool inspectable() const {
    return available() && !busy_ && !preparing_ && frameError_.isEmpty();
  }
  QString frameError() const { return frameError_; }
  QString error() const { return error_; }
  QString path() const { return path_; }
  QVariantMap info() const;
  QStringList meshes() const;
  QStringList uvSets() const;
  QStringList bindings() const;
  QStringList bones() const;
  int meshIndex() const { return mesh_; }
  int bindingIndex() const;
  int boneIndex() const { return bone_; }
  int uvIndex() const;
  int mode() const { return mode_; }
  bool isolated() const { return isolated_; }
  QVariantMap picked() const { return picked_; }
  QVariantList influences() const { return influences_; }
  QStringList materials() const;
  int materialIndex() const { return material_; }
  bool materialOnly() const { return materialOnly_; }
  QVariantMap uvBackground() const;
  Q_INVOKABLE void setMaterialOnly(bool);
  QString texturePath() const;
  QString textureReference() const;
  QString textureError() const { return textureError_; }
  bool canClearTexture() const;
  bool textureBusy() const { return textureBusy_; }
  Q_INVOKABLE void selectMaterial(int);
  Q_INVOKABLE void associateTexture(const QString &);
  Q_INVOKABLE void removeTexture();
  Q_INVOKABLE void open(const QString &);
  Q_INVOKABLE void clear();
  Q_INVOKABLE void cancel();
  Q_INVOKABLE void selectMesh(int);
  Q_INVOKABLE void selectBinding(int);
  Q_INVOKABLE void selectBone(int);
  Q_INVOKABLE void selectUV(int);
  Q_INVOKABLE void setMode(int);
  Q_INVOKABLE void setIsolated(bool);
  void pick(const QMatrix4x4 &, QPointF normalized, bool uvView = false);
  std::shared_ptr<const formaview::render::Frame> frame() const {
    return frame_;
  }
  std::pair<QVector3D, float> focusBounds(bool selected) const;
  std::pair<QVector3D, float> uvBounds(bool reference = true) const;
  std::shared_ptr<const formaview::render::Frame> frameFor(bool uv) const {
    return uv ? frames_.uv : frames_.model;
  }
signals:
  void changed();
  void selectionChanged();
  void frameChanged();
  void documentChanged();

private:
  struct Result {
    std::shared_ptr<ModelDocument> document;
    QString error;
  };
  struct Job {
    quint64 generation;
    std::stop_source stop;
    std::future<Result> future;
  };
  struct TextureResult {
    std::shared_ptr<formaview::render::Texture> pixels;
    QString error, path;
  };
  struct TextureJob {
    quint64 generation, request;
    int mesh, material;
    std::stop_source stop;
    std::future<TextureResult> future;
    int uv{-1};
  };
  void startPending();
  struct FrameResult {
    ModelFrames frames;
    ModelRenderState state;
    QString error;
  };
  struct FrameJob {
    quint64 generation, request;
    std::stop_source stop;
    std::future<FrameResult> future;
  };
  void startFrame();
  ModelRenderState renderState() const;
  QByteArray stateKey(const ModelRenderState &) const;
  void cacheFrame();
  void poll();
  void rebuild();
  void clearSelection();
  void updateSkeleton(bool resetHighlight = true);
  std::vector<bool> visibleMeshes() const;
  void chooseVisibleMesh();
  std::shared_ptr<ModelDocument> document_;
  ModelScene scene_;
  ModelSkeleton skeleton_;
  std::shared_ptr<const formaview::render::Frame> frame_;
  std::vector<Job> jobs_;
  std::vector<TextureJob> textureJobs_;
  std::vector<FrameJob> frameJobs_;
  ModelFrames frames_;
  ModelRenderState renderedState_;
  struct CachedFrame {
    QByteArray key;
    ModelFrames frames;
    ModelRenderState state;
    std::uint64_t bytes{};
  };
  std::list<CachedFrame> frameCache_;
  std::uint64_t cachedBytes_{};
  bool globalFrameFailure_{};
  std::vector<std::map<int, ModelTextureLink>> textureLinks_;
  QTimer timer_;
  QString path_, pending_, error_;
  QString selectedNode_;
  QString skeletonNode_, highlightedNode_;
  int skeletonJoint_{-1};
  quint64 generation_{}, frameRevision_{};
  quint64 frameRequest_{};
  bool preparing_{}, pendingFrame_{};
  QString frameError_;
  bool busy_{}, isolated_{}, materialOnly_{};
  bool textureBusy_{};
  QString textureError_;
  quint64 textureRequest_{};
  int mesh_{}, bone_{}, mode_{}, material_{};
  std::vector<int> bindingSelections_, uvSelections_;
  std::vector<ModelClassification> classes_;
  std::map<QString, int> levels_;
  int lodGroup_{}, collisionMode_{};
  QVariantMap picked_;
  QVariantList influences_;
};
