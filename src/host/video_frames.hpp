#pragma once
#include "video_index.hpp"
#include <QImage>
#include <condition_variable>
#include <array>
#include <map>
#include <thread>
#include <stop_token>

QImage videoThumbnail(const QString &file, QSize bounds, std::stop_token stop);

// Software review of the certified SDR stream. Every cached image carries
// AVFrame::best_effort_timestamp from the decoder which produced its pixels.
using VideoSignature=std::array<float,192>;
struct VideoReviewFrame;
class VideoFrames {
public:
  explicit VideoFrames(std::shared_ptr<VideoIndex> index);
  ~VideoFrames();
  void cancel();
  void prepare(double seconds, QSize viewport, const VideoSeekPlan &plan);
  std::shared_ptr<const VideoReviewFrame> frame(qint64 tick);
  static QImage image(const VideoReviewFrame &frame);
  qint64 identify(const VideoSignature &luma);
  void track(double seconds);

private:
  void run();
  std::shared_ptr<VideoIndex> index_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::map<qint64, std::shared_ptr<const VideoReviewFrame>> frames_;
  qsizetype bytes_ = 0;
  quint64 generation_ = 0;
  quint64 cacheGeneration_ = 0;
  bool stop_ = false;
  double time_ = 0, clock_ = 0;
  QSize size_;
  VideoSeekPlan plan_;
  std::thread thread_;
};
