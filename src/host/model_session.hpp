#pragma once
#include <chrono>
#include <atomic>
#include "formaview/render_item.hpp"
#include "model_document.hpp"
#include "formaview/fbx.hpp"
#include "model_scene.hpp"
#include "model_skeleton.hpp"
#include "model_abc.hpp"
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QQuickWindow>
#include <QPointer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>
#include <future>
#include <deque>
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
  Q_PROPERTY(bool animated READ animated NOTIFY changed)
  Q_PROPERTY(bool hasPlayableAnimation READ hasPlayableAnimation NOTIFY changed)
  Q_PROPERTY(bool canPlayAnimation READ canPlayAnimation NOTIFY animChanged)
  Q_PROPERTY(QVariantList clips READ clips NOTIFY changed)
  Q_PROPERTY(int clipIndex READ clipIndex NOTIFY animChanged)
  Q_PROPERTY(double time READ time NOTIFY animChanged)
  Q_PROPERTY(bool playing READ playing NOTIFY animChanged)
  Q_PROPERTY(bool loop READ loop WRITE setLoop NOTIFY animChanged)
  Q_PROPERTY(double speed READ speed WRITE setSpeed NOTIFY animChanged)
  Q_PROPERTY(QVariantMap clipInfo READ clipInfo NOTIFY animChanged)
  Q_PROPERTY(bool pairMeshVisible READ pairMeshVisible NOTIFY animChanged)
  Q_PROPERTY(bool rootLockOn READ rootLockOn NOTIFY animChanged)
  Q_PROPERTY(int rootBase READ rootBase WRITE setRootBase NOTIFY animChanged)
  Q_PROPERTY(int rootIndex READ rootIndex NOTIFY animChanged)
  Q_PROPERTY(QVariantList rootCandidates READ rootCandidates NOTIFY animChanged)
  Q_PROPERTY(QVariantMap rootLockInfo READ rootLockInfo NOTIFY animChanged)
  Q_PROPERTY(bool canAttachExternalAnimation READ canAttachExternalAnimation NOTIFY changed)
  Q_PROPERTY(QVariantMap externalInfo READ externalInfo NOTIFY animChanged)
  Q_PROPERTY(bool externalBusy READ externalBusy NOTIFY changed)
  Q_PROPERTY(bool hasComparableSkeleton READ hasComparableSkeleton NOTIFY changed)
  Q_PROPERTY(bool compareBusy READ compareBusy NOTIFY changed)
  Q_PROPERTY(QVariantMap compareResult READ compareResult NOTIFY changed)
  Q_PROPERTY(QString compareError READ compareError NOTIFY changed)
  // Alembic geometry cache: timeline, sample statistics and cache state.
  Q_PROPERTY(bool geometryCache READ geometryCache NOTIFY changed)
  Q_PROPERTY(QVariantMap cacheInfo READ cacheInfo NOTIFY animChanged)
  // Per-mesh metadata changes only when a sample lands, not per clock tick.
  Q_PROPERTY(QVariantList cacheMeshes READ cacheMeshes NOTIFY changed)
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
  bool animated() const { return document_ && document_->animated(); }
  bool hasPlayableAnimation() const {
    return document_ && document_->firstPlayableAnimation() >= 0;
  }
  bool canPlayAnimation() const {
    return animated() && clipIndex_ >= 0 &&
           static_cast<std::size_t>(clipIndex_) < document_->animationClips.size() &&
           document_->animationClips[static_cast<std::size_t>(clipIndex_)].playable();
  }
  QVariantList clips() const {
    return document_ ? document_->clips : QVariantList{};
  }
  int clipIndex() const { return clipIndex_; }
  double time() const { return time_; }
  bool playing() const { return playing_; }
  bool loop() const { return loop_; }
  double speed() const { return speed_; }
  QVariantMap clipInfo() const;
  Q_INVOKABLE void togglePlay();
  Q_INVOKABLE void seek(double seconds);
  Q_INVOKABLE void stepFrame(int frames);
  Q_INVOKABLE void setClipIndex(int index);
  Q_INVOKABLE void setLoop(bool value);
  Q_INVOKABLE void setSpeed(double value);
  bool geometryCache() const { return abcActive(); }
  QVariantMap cacheInfo() const;
  QVariantList cacheMeshes() const;
  // Jump to a merged-timeline sample (random access; clamps the index).
  Q_INVOKABLE void seekSample(int index);
  // Stop an in-flight sample read and keep the presented sample.
  Q_INVOKABLE void cancelSample();
  // Verification/tuning: tighten the sample cache bounds.
  void setCacheLimits(std::uint64_t bytes, std::size_t samples) {
    abcCache_.setLimits(bytes, samples);
  }
  bool pairMeshVisible() const { return pairMeshVisible_; }
  Q_INVOKABLE void setPairMeshVisible(bool value);
  bool rootLockOn() const { return rootLockOn_; }
  int rootBase() const { return rootBase_; }
  Q_INVOKABLE void setRootBase(int base);
  int rootIndex() const { return rootIndex_; }
  Q_INVOKABLE void setRootIndex(int index);
  QVariantList rootCandidates() const;
  QVariantMap rootLockInfo() const;
  Q_INVOKABLE void setRootLock(bool value);
  bool canAttachExternalAnimation() const {
    return document_ && document_->canAttachExternal();
  }
  QVariantMap externalInfo() const;
  bool externalBusy() const { return !externalJobs_.empty() || !externalQueue_.empty(); }
  Q_INVOKABLE void attachExternalAnimation(const QString &url);
  Q_INVOKABLE void detachExternalAnimation();
  bool hasComparableSkeleton() const;
  bool compareBusy() const { return !compareJobs_.empty(); }
  QVariantMap compareResult() const { return compareResult_; }
  QString compareError() const { return compareError_; }
  QByteArray sourceDataDigest() const;
  Q_INVOKABLE void compareSkeletonWith(const QString &url);
  Q_INVOKABLE void selectMaterial(int);
  Q_INVOKABLE void associateTexture(const QString &);
  Q_INVOKABLE void removeTexture();
  Q_INVOKABLE void open(const QString &);
  Q_INVOKABLE void setAnimationReference(const QString &reference);
  Q_INVOKABLE void setAnimationCompanion(const QString &companion);
  Q_INVOKABLE void clear();
  Q_INVOKABLE void cancel();
  Q_INVOKABLE void selectMesh(int);
  Q_INVOKABLE void selectBinding(int);
  Q_INVOKABLE void selectBone(int);
  Q_INVOKABLE void selectUV(int);
  Q_INVOKABLE void setMode(int);
  Q_INVOKABLE void setIsolated(bool);
  void enableGpuSkinning(bool enabled);
  void attachRenderWindow(QQuickWindow *);
  void gpuSkinningFailed(const QString &, std::shared_ptr<const formaview::render::Frame>);
  void renderingFailed(const QString &, std::shared_ptr<const formaview::render::Frame>);
  void pick(const QMatrix4x4 &, QPointF normalized, bool uvView = false,
      std::shared_ptr<const formaview::render::Frame> presented = {});
  std::shared_ptr<const formaview::render::Frame> frame() const {
    return frame_;
  }
  // Verification oracles only: an attach may still extend this document in place.
  std::shared_ptr<const ModelDocument> documentSnapshot() const { return document_; }
  // True once the presented pose reflects the current view state (verification).
  ModelRenderState renderStateForTest() const { return renderState(); }
  QVariantMap performanceInfoForTest() const;
  bool renderedStateCurrent() const { return document_ && stateKey(renderState())==stateKey(renderedState_); }
  std::pair<QVector3D, float> focusBounds(bool selected) const;
  std::pair<QVector3D, float> uvBounds(bool reference = true) const;
  std::shared_ptr<const formaview::render::Frame> frameFor(bool uv) const {
    return uv ? frames_.uv : frames_.model;
  }
signals:
  void changed();
  void animChanged();
  void selectionChanged();
  void frameChanged();
  void documentChanged();

private:
  struct Result {
    std::shared_ptr<ModelDocument> document;
    ModelScene::Prepared scene;
    ModelSkeleton::Prepared skeleton;
    int initialMesh{};
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
  void openInputs(QString, QString, QString);
  void reset(bool opening);
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
  void tickAnimation();
  void refreshAnimationFrame();
  ModelRenderState renderState() const;
  QByteArray stateKey(const ModelRenderState &) const;
  void cacheFrame();
  void poll();
  void rebuild();
  void clearSelection();
  void updateSkeleton(bool resetHighlight = true,ModelSkeleton::Prepared *prepared = nullptr);
  const ModelMesh *selectedMesh() const;
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
  bool gpuEnabled_{}, gpuDisabled_{};
  QPointer<QQuickWindow> gpuWindow_;
  QMetaObject::Connection renderTickConnection_, renderTickSequenceConnection_;
  std::shared_ptr<std::atomic<std::uint64_t>> renderTickSequence_;
  QByteArray gpuKey_;
  // Presentation of resident GPU geometry: shown draws and highlight, recomputed
  // only when the selection changes, never per pose.
  GpuView gpuView_;
  QByteArray gpuViewKey_;
  std::shared_ptr<const ModelGpuGeometry> gpuViewOwner_, gpuHighlightOwner_;
  GpuHighlight gpuHighlight_;
  QString gpuHighlightNode_;
  QByteArray gpuStateKey(const ModelRenderState &) const;
  QByteArray gpuViewKey(const ModelRenderState &) const;
  std::shared_ptr<const ModelGpuGeometry> gpuGeometry_;
  struct GpuJob {
    quint64 generation;
    QByteArray normKey, fullKey; // Residency key / complete state key
    std::stop_source stop;
    std::future<std::shared_ptr<const ModelGpuGeometry>> future;
  };
  std::optional<GpuJob> gpuJob_;
  bool refreshGpuFrame();
  void finishGpuGeometry();
  struct PendingPick {
    quint64 generation,poseRequest,request;
    std::shared_ptr<const ModelDocument> document;
    ModelRenderState state;
    std::shared_ptr<const formaview::render::Frame> frame;
    QMatrix4x4 view;QPointF normalized;
  };
  struct PickJob {
    quint64 generation,poseRequest,request;
    std::stop_source stop;std::future<ModelPickHit> future;
  };
  quint64 pickRequest_{};
  std::optional<PendingPick> pendingPick_;
  std::optional<PickJob> pickJob_;
  void startGpuPick();void finishGpuPick();void applyPick(const ModelPickHit &,bool);
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
  QTimer animTimer_;
  QElapsedTimer animClock_;
  QString path_, pending_, error_;
  QString referencePath_, companionPath_;
  // Session-level reference memory (process lifetime only, never persisted):
  // the user's last explicit PSK choice, re-verified per PSA on every open.
  QString sessionReferencePath_;
  QString companionCandidate(const QString &psa) const;
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
  int clipIndex_{};
  double time_{};
  bool playing_{}, loop_{true};
  bool pairMeshVisible_{};
  bool resumePlaybackOnLanding_{};
  bool rootLockOn_{};
  int rootBase_{}; // 0 first frame, 1 reference pose, 2 zero.
  int rootIndex_{-1}; // -1 auto (unique root).
  int rootBaseClip_{-1};
  std::vector<formaview::Matrix4> rootBaseLocals_;
  struct ExternalResult {
    std::shared_ptr<formaview::skeletal::FbxDocument> holder;
    std::shared_ptr<ModelDocument> actor;
    QString error, path;
  };
  struct ExternalJob {
    quint64 generation;
    std::stop_source stop;
    std::future<ExternalResult> future;
  };
  std::vector<ExternalJob> externalJobs_;
  // Multi-select batches: files beyond the concurrent read budget wait here
  // (never dropped); results apply in selection order with a per-file outcome.
  std::deque<QString> externalQueue_;
  int externalRequested_{}, externalApplied_{};
  QStringList externalFailed_;
  QString externalAppliedStatus_, externalAppliedNotice_;
  void startExternalJob(const QString &path);
  // GPU skinning applies to this document in its current view state.
  bool gpuSkinActive() const;
  struct CompareResult {
    QVariantMap report;
    QString error;
    double readMs{}; // Worker: lease + parse + report, off the GUI thread.
  };
  struct CompareJob {
    quint64 generation;
    std::stop_source stop;
    std::future<CompareResult> future;
    std::chrono::steady_clock::time_point started{std::chrono::steady_clock::now()};
  };
  std::vector<CompareJob> compareJobs_;
  // Heavy externally-driven mesh frames are built off the GUI thread; the
  // generation token rejects stale results after switches/cancels.
  struct AsyncFrame {
    quint64 generation;
    quint64 poseRequest;
    int clip;
    double time;
    ModelRenderState state;
    std::stop_source stop;
    std::future<ModelFrames> future;
  };
  std::optional<AsyncFrame> asyncFrame_;
  // Geometry cache state. Sample documents are immutable; the GUI thread
  // owns the bounded cache. Explicit seeks land exactly; forward playback
  // may present completed samples behind the clock to avoid read starvation.
  bool abcActive() const { return document_ && document_->geometryCache; }
  struct AbcResult {
    std::shared_ptr<ModelDocument> document;
    QString error;
    bool cancelled{};
  };
  struct AbcJob {
    quint64 generation;
    std::size_t index;
    bool prefetch;
    std::stop_source stop;
    std::future<AbcResult> future;
  };
  std::shared_ptr<const ModelAbcSource> abcSource_;
  ModelAbcCache abcCache_;
  std::shared_ptr<ModelDocument> abcFrameDocument_; // Sample of the presented frame.
  std::optional<AbcJob> abcJob_;
  std::size_t abcRequested_{};
  std::size_t abcPlaybackFloor_{}; // Explicit seek/restart boundary for catch-up.
  std::pair<QVector3D, float> abcDisplay_{{}, 1.F};
  int abcDirection_{1};
  std::uint64_t abcPrefetched_{}, abcDiscarded_{};
  QString abcError_;
  void requestAbcSample(std::size_t index);
  void scheduleAbc();
  void startAbcJob(std::size_t index, bool prefetch);
  void finishAbcJob();
  void landAbcSample(std::shared_ptr<ModelDocument>);
  void tickAbc();
  quint64 poseRequest_{};
  bool pendingAnimationFrame_{};
  void finishAsyncFrame();
  QVariantMap compareResult_;
  QString compareError_;
  void applyExternalDrive(std::shared_ptr<formaview::skeletal::FbxDocument> holder,
                          const QString &path);
  void applyExternalActor(std::shared_ptr<ModelDocument> source,
                          const QString &path);
  const std::vector<formaview::Matrix4> *rootBaseFor(int clip);
  std::vector<formaview::Matrix4> rootLockedWorlds();
  int resolvedRoot() const;
  double speed_{1};
  std::vector<int> bindingSelections_, uvSelections_;
  std::vector<ModelClassification> classes_;
  std::map<QString, int> levels_;
  int lodGroup_{}, collisionMode_{};
  QVariantMap picked_;
  QVariantList influences_;
};
