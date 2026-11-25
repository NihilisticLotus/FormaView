#pragma once
#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>
#include <QPointer>
#include <QTimer>
#include <QSet>
#include <QVariantMap>
#include <memory>
#include <optional>

class VideoStatistics;
struct VideoEngine;
struct VideoReviewFrame;
class VideoSession : public QQuickItem {
  Q_OBJECT
  Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
  Q_PROPERTY(QVariantMap status READ status NOTIFY statusChanged)
  Q_PROPERTY(QVariantMap playbackInfo READ playbackInfo NOTIFY playbackInfoChanged)
  Q_PROPERTY(QString error READ error NOTIFY errorChanged)
  Q_PROPERTY(bool playbackSpaceEnabled MEMBER playbackSpaceEnabled_)
public:
  explicit VideoSession(QQuickItem *parent = nullptr);
  ~VideoSession() override;
  QString source() const { return source_; }
  QVariantMap status() const { return status_; }
  QVariantMap playbackInfo() const { return playbackInfo_; }
  Q_INVOKABLE void samplePlaybackInfo();
  Q_INVOKABLE void clearPlaybackInfo();
  QString error() const { return error_; }
  void enableBackgroundPlayback() { background_ = true; }
  bool seekPending() const { return pendingSeekTarget_.has_value(); }
  double indexedStepTarget() const { return requestedStep_; }
  bool optionPending(const QString &name) const { return pendingPlaybackOptions_.contains(name); }
  void setSource(const QString &path);
  Q_INVOKABLE QByteArray windowPlacement(QQuickWindow *window) const;
  Q_INVOKABLE bool restoreWindowPlacement(QQuickWindow *window,
                                          const QByteArray &saved) const;
  Q_INVOKABLE QQuickWindow *prepareHdrWindow();
  Q_INVOKABLE void leaveHdr();
  Q_INVOKABLE void togglePause();
  Q_INVOKABLE void seek(double seconds, bool relative);
  Q_INVOKABLE void step(bool forward);
  Q_INVOKABLE void setOption(const QString &name, const QVariant &value);
  Q_INVOKABLE void addSubtitle(const QUrl &url);
  Q_INVOKABLE void screenshot(const QUrl &directory, bool subtitles);
  Q_INVOKABLE void loop(double first, double last);
  Q_INVOKABLE void clearLoop();
signals:
  void sourceChanged();
  void statusChanged();
  void playbackInfoChanged();
  void errorChanged();
  void screenshotSaved(const QString &path);
  void screenshotFailed(const QString &path, const QString &reason);

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;
  QSGNode *updatePaintNode(QSGNode *, UpdatePaintNodeData *) override;

private:
  void releaseSession();
  void openSession(const QString &path, bool switchingWindow, bool hdr);
  void clearReview();
  void prepareReviewFrames();
  void alignReview();
  void reviewReady();
  void loadVideo();
  bool playbackSpaceEnabled_ = false;
  QPointer<QQuickWindow> shortcutWindow_;
  bool background_ = false;
  std::unique_ptr<QWindow> backgroundTarget_;
  QTimer alignmentTimer_;
  QTimer prefetchTimer_;
  bool prefetchDeferred_ = false;
  quint64 videoEpoch_=0;
  qint64 reviewTick_ = -1, alignTick_ = -1;
  std::shared_ptr<const VideoReviewFrame> reviewFrame_;
  bool resumeAfterAlign_ = false;
  bool alignAck_=false, alignPainted_=false;
  quint64 alignRequest_=0;
  bool customOutput_ = false;
  QSet<QString> outputAdjustments_;
  double requestedStep_ = -1;
  std::optional<double> pendingSeekTarget_;
  quint64 pendingSeekRequest_ = 0;
  quint64 positionQuery_ = 0;
  QList<QPair<QUrl, bool>> pendingShots_;
  void events();
  void fail(const QString &message);
  bool command(const QList<QByteArray> &arguments, quint64 id = 0);
  void requestExactSeek(double seconds);
  std::shared_ptr<VideoEngine> engine_;
  QString source_, error_;
  QPointer<QQuickWindow> hdrWindow_;
  QVariantMap status_;
  std::unique_ptr<VideoStatistics> statistics_;
  QVariantMap playbackInfo_, pendingInfo_;
  quint64 infoRequest_ = 0;
  int infoReplies_ = 0;
  QMap<QString, quint64> pendingPlaybackOptions_;
  QMap<quint64, QString> screenshots_;
  quint64 serial_ = 0;
  QTimer timer_;
};
