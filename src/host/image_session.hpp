#pragma once
#include "formaview/image_conversion.hpp"
#include "formaview/scheduler.hpp"
#include <QObject>
#include <QTimer>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <array>
#include <atomic>
#include <memory>

struct ImagePixels {
  formaview::ImageSubresource source;
  std::vector<float> rgba;
  std::array<int, 4> mapping{-1, -1, -1, -1};
  QVariantMap info;
  QVariantList subresources, channels;
  QString path;
  quint64 revision{};
  std::size_t bytes{};
};

class ImageSession : public QObject {
  Q_OBJECT
  QML_ANONYMOUS
  Q_PROPERTY(bool available READ available NOTIFY changed)
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(QString error READ error NOTIFY changed)
  Q_PROPERTY(QString requested READ requested NOTIFY changed)
  Q_PROPERTY(QString path READ path NOTIFY changed)
  Q_PROPERTY(QVariantMap info READ info NOTIFY changed)
  Q_PROPERTY(QVariantList subresources READ subresources NOTIFY changed)
  Q_PROPERTY(int subresourceCount READ subresourceCount NOTIFY changed)
  Q_PROPERTY(int subresourceOffset READ subresourceOffset NOTIFY changed)
  Q_PROPERTY(QVariantList channels READ channels NOTIFY changed)
  Q_PROPERTY(int selected READ selected NOTIFY changed)
  Q_PROPERTY(QVariantMap pixel READ pixel NOTIFY pixelChanged)
  Q_PROPERTY(bool samplePinned READ samplePinned NOTIFY pixelChanged)
  Q_PROPERTY(QVariantMap display READ display NOTIFY displayChanged)
  Q_PROPERTY(QVariantMap dimensions READ dimensions NOTIFY changed)
  Q_PROPERTY(bool animated READ animated NOTIFY animationChanged)
  Q_PROPERTY(bool playing READ playing NOTIFY animationChanged)
  Q_PROPERTY(bool loop READ loop NOTIFY animationChanged)
  Q_PROPERTY(int frame READ frame NOTIFY animationChanged)
  Q_PROPERTY(int frameCount READ frameCount NOTIFY animationChanged)
  Q_PROPERTY(int frameDelay READ frameDelay NOTIFY animationChanged)
  Q_PROPERTY(bool exporting READ exporting NOTIFY exportChanged)
  Q_PROPERTY(int exportProgress READ exportProgress NOTIFY exportChanged)
  Q_PROPERTY(QString exportError READ exportError NOTIFY exportChanged)
public:
  explicit ImageSession(QObject *parent = nullptr);
  ~ImageSession() override;
  bool available() const { return !!pixels_; }
  bool busy() const { return busy_; }
  QString error() const { return error_; }
  QString requested() const { return requested_; }
  QString path() const { return pixels_ ? pixels_->path : QString{}; }
  QVariantMap info() const { return pixels_ ? pixels_->info : QVariantMap{}; }
  QVariantList subresources() const {
    return pixels_ ? pixels_->subresources.mid(resourceOffset_, 100)
                   : QVariantList{};
  }
  int subresourceCount() const {
    return pixels_ ? static_cast<int>(pixels_->subresources.size()) : 0;
  }
  int subresourceOffset() const { return resourceOffset_; }
  QVariantList channels() const {
    return pixels_ ? pixels_->channels : QVariantList{};
  }
  int selected() const { return selected_; }
  QVariantMap pixel() const { return pixel_; }
  bool samplePinned() const { return samplePinned_; }
  QVariantMap display() const { return display_; }
  QVariantMap dimensions() const;
  bool animated() const { return frameCount_ > 1; }
  bool playing() const { return playing_; }
  bool loop() const {
    return loopOverride_ ? loop_ : sourceLoopCount_ != 0;
  }
  int frame() const { return frame_; }
  int frameCount() const { return frameCount_; }
  int frameDelay() const { return frameDelay_; }
  bool exporting() const { return exporting_; }
  int exportProgress() const { return exportProgress_; }
  QString exportError() const { return exportError_; }
  std::shared_ptr<const ImagePixels> pixels() const { return pixels_; }
  Q_INVOKABLE void open(const QString &path, int index = 0);
  Q_INVOKABLE void clear();
  Q_INVOKABLE void selectSubresource(int index);
  Q_INVOKABLE void resourcePage(int offset);
  Q_INVOKABLE void mapChannels(const QStringList &names);
  Q_INVOKABLE void selectLayer(const QString &name);
  Q_INVOKABLE void selectDimension(const QString &name, int value);
  Q_INVOKABLE void setting(const QString &name, const QVariant &value);
  Q_INVOKABLE void sample(double x, double y, int tileX = 0, int tileY = 0);
  Q_INVOKABLE void setSamplePinned(bool pinned);
  Q_INVOKABLE QString pixelReport() const;
  Q_INVOKABLE void seekFrame(int frame);
  Q_INVOKABLE void stepFrame(int frames);
  Q_INVOKABLE void togglePlay();
  Q_INVOKABLE void setLoop(bool loop);
  Q_INVOKABLE void exportFrames(const QString &directory,
                                const QString &prefix,
                                const QString &conflict);
  Q_INVOKABLE void cancelExport();
signals:
  void changed();
  void displayChanged();
  void pixelChanged();
  void animationChanged();
  void exportChanged();

private:
  struct Pending;
  struct Cache;
  void load(const QString &, int, QStringList);
  void poll();
  formaview::Scheduler scheduler_;
  std::unique_ptr<Cache> cache_;
  std::vector<std::unique_ptr<Pending>> pending_;
  std::optional<formaview::TaskHandle> cleanup_;
  std::shared_ptr<const ImagePixels> pixels_;
  QTimer timer_;
  QTimer playbackTimer_;
  QVariantMap display_, pixel_;
  QString error_, requested_;
  quint64 generation_{};
  int selected_{}, resourceOffset_{};
  int frame_{}, frameCount_{1}, frameDelay_{100}, sourceLoopCount_{1};
  int playbackWraps_{};
  int exportProgress_{};
  bool busy_{};
  bool samplePinned_{};
  bool playing_{}, loop_{true}, loopOverride_{}, exporting_{};
  QString exportError_;
  std::optional<formaview::TaskHandle> exportTask_;
  std::shared_ptr<std::atomic_int> exportProgressShared_;
};
