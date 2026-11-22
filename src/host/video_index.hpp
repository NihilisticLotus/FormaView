#pragma once
#include <QString>
#include <QVector>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
struct AVFormatContext;
struct mpv_handle;

// Closed-IDR HEVC media view. All offsets refer to the original, read-only
// file.
struct VideoSpan {
  qint64 offset, length;
};
struct VideoSeekPlan {
  QVector<VideoSpan> skipped;
  QVector<qint64> omittedPts;
  QVector<qint64> ticks;
  qint64 firstSafe = 0;
  int scale = 0;
  int prefix = 0;
  bool valid() const { return scale && !ticks.empty(); }
};
class VideoIndex {
public:
  static std::shared_ptr<VideoIndex> open(const QString &path);
  ~VideoIndex();
  VideoSeekPlan plan(double seconds);
  std::optional<QString> caption(int stream, double seconds);
  QString file;

private:
  AVFormatContext *format_ = nullptr;
  int video_ = -1;
  std::mutex lock_;
};
class VideoStream {
public:
  QString path;
  void apply(const VideoSeekPlan &plan);
  bool attach(mpv_handle *handle, void *registerFunction);
  void filter(char *data, qint64 start, qint64 count);

private:
  QVector<VideoSpan> spans_;
  std::mutex mutex_;
};
