#include "video_index.hpp"
#include "flv_seek_index.hpp"
#include <QCryptographicHash>
#include <QFile>
#include <QtEndian>
#include <mpv/stream_cb.h>
extern "C" {
#include <libavformat/avformat.h>
}
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

namespace {
class Bits {
  QByteArray data;
  int offset = 0;

public:
  bool ok = true;
  explicit Bits(QByteArray d) : data(std::move(d)) {
    data.replace(QByteArray("\0\0\3", 3), QByteArray("\0\0", 2));
  }
  int take(int n) {
    if (n < 0 || n > 24 || offset + n > data.size() * 8) {
      ok = false;
      return 0;
    }
    int val = 0;
    while (n--) {
      val = 2 * val + ((quint8(data[offset / 8]) >> (7 - offset % 8)) & 1);
      ++offset;
    }
    return val;
  }
  int ue() {
    int n = 0;
    while (ok && !take(1)) {
      if (++n > 15) {
        ok = false;
        return 0;
      }
    }
    return (1 << n) - 1 + take(n);
  }
};
struct Picture {
  qint64 pts;
  VideoSpan nal;
  int poc;
  bool idr;
  QVector<int> refs;
};
bool configMatches(const quint8 *data, int size, QByteArray expected) {
  return QCryptographicHash::hash(
             QByteArray::fromRawData(reinterpret_cast<const char *>(data),
                                     size),
             QCryptographicHash::Sha256)
             .toHex() == expected;
}
std::optional<Picture> picture(const AVPacket *pkt) {
  std::optional<Picture> result;
  if (pkt->pos < 0 || pkt->pts == AV_NOPTS_VALUE)
    return {};
  for (int pos = 0; pos < pkt->size;) {
    if (pkt->size - pos < 4)
      return {};
    const quint32 n = qFromBigEndian<quint32>(pkt->data + pos);
    pos += 4;
    if (n < 3 || n > quint32(pkt->size - pos))
      return {};
    const auto *p = pkt->data + pos;
    int type = (p[0] >> 1) & 63;
    if (type >= 32 && type <= 34) {
      static const QByteArray hash[] = {
          "9f95636afe4b02605f2c3074d6937e176a6b46018dcdc4d8dd9cd256ef4510c0",
          "f52105ea939aaaf76b3d5c270bb770e32e983de91b7bb07b5a027de9b27919bd",
          "7b0bf33f8fab215a662a1f55a4e091ac1cbc8a59c912862fe16bf99ce016b561"};
      if (!configMatches(p, int(n), hash[type - 32]))
        return {};
    } else if (type <= 31) {
      bool idr = type == 19 || type == 20;
      if (result || (!idr && type != 0 && type != 1))
        return {};
      Bits b(QByteArray(reinterpret_cast<const char *>(p + 2),
                        std::min(96, int(n) - 2)));
      if (b.take(1) != 1)
        return {};
      if (idr)
        b.take(1);
      if (b.ue() != 0)
        return {};
      b.ue();
      Picture pic{pkt->pts, {pkt->pos + pos, n}, 0, idr, {}};
      if (!idr) {
        pic.poc = b.take(8);
        if (b.take(1))
          return {};
        int neg = b.ue(), positive = b.ue();
        if (neg + positive > 16)
          return {};
        for (int sign : {-1, 1}) {
          int delta = 0;
          for (int j = 0; j < (sign < 0 ? neg : positive); ++j) {
            delta += sign * (b.ue() + 1);
            if (b.take(1))
              pic.refs.append(delta);
          }
        }
      }
      if (!b.ok)
        return {};
      result = std::move(pic);
    }
    pos += int(n);
  }
  return result;
}
struct Reader {
  QFile file;
  VideoStream *owner;
  QByteArray metadata;
  qint64 position = 0;
};
int openStream(void *cookie, char *uri, mpv_stream_cb_info *info) {
  auto *stream = static_cast<VideoStream *>(cookie);
  if (QByteArray(uri) != (stream->flv ? "fvmedia://source.flv" : "fvmedia://source.mp4"))
    return MPV_ERROR_LOADING_FAILED;
  auto r = std::make_unique<Reader>();
  r->file.setFileName(stream->path);
  r->owner = stream;
  if (!r->file.open(QIODevice::ReadOnly))
    return MPV_ERROR_LOADING_FAILED;
  if (stream->flv) {
    r->metadata = stream->flvMetadata();
    if (!r->file.seek(0) || stream->cancelled.load()) return MPV_ERROR_LOADING_FAILED;
  }
  info->read_fn = [](void *c, char *p, uint64_t wanted) -> int64_t {
    auto *r = static_cast<Reader *>(c);
    qint64 wantedBytes = static_cast<qint64>(std::min<uint64_t>(wanted, INT_MAX));
    const qint64 off = r->position;
    if (!r->metadata.isEmpty()) {
      if (off < 13) wantedBytes = std::min(wantedBytes, 13 - off);
      else if (off < 13 + r->metadata.size()) {
        const auto n = std::min(wantedBytes, 13 + qint64(r->metadata.size()) - off);
        std::memcpy(p, r->metadata.constData() + (off - 13), size_t(n));
        r->position += n;
        return n;
      }
    }
    auto n = r->file.read(p, wantedBytes);
    if (n > 0) {
      if (!r->owner->flv) r->owner->filter(p, off, n);
      r->position += n;
    }
    return n;
  };
  info->seek_fn = [](void *c, int64_t off) -> int64_t {
    auto *r = static_cast<Reader *>(c);
    if (off < 0 || off > r->file.size() + r->metadata.size()) return -1;
    const auto physical = off <= 13 ? off : std::max<int64_t>(13, off - r->metadata.size());
    if (!r->file.seek(physical)) return -1;
    return r->position = off;
  };
  info->size_fn = [](void *c) -> int64_t {
    auto *r = static_cast<Reader *>(c);
    return r->file.size() + r->metadata.size();
  };
  info->close_fn = [](void *c) { delete static_cast<Reader *>(c); };
  info->cookie = r.release();
  return 0;
}
} // namespace
std::shared_ptr<VideoIndex> VideoIndex::open(const QString &path) {
  if (!path.endsWith(".mp4", Qt::CaseInsensitive))
    return {};
  auto index = std::make_shared<VideoIndex>();
  index->file = path;
  if (avformat_open_input(&index->format_, path.toUtf8().constData(), nullptr,
                          nullptr) < 0)
    return {};
  auto *f = index->format_;
  for (unsigned i = 0; i < f->nb_streams; ++i) {
    auto *s = f->streams[i];
    auto *c = s->codecpar;
    if (c->codec_type != AVMEDIA_TYPE_VIDEO)
      continue;
    if (index->video_ >= 0 || c->codec_id != AV_CODEC_ID_HEVC ||
        s->time_base.num != 1 || (s->start_time != 0 && s->start_time != AV_NOPTS_VALUE) ||
        !configMatches(
            c->extradata, c->extradata_size,
            "ade2edbb5dd2bfb4055b04f771d94d51776754159cbfe65ac0a59440fabb3421"))
      return {};
    index->video_ = int(i);
  }
  return index->video_ >= 0 ? index : nullptr;
}
VideoIndex::~VideoIndex() { avformat_close_input(&format_); }
VideoSeekPlan VideoIndex::plan(double seconds) {
  std::lock_guard lock(lock_);
  VideoSeekPlan plan;
  if (!std::isfinite(seconds) || seconds < 0 || seconds > 1e9)
    return {};
  const int scale = format_->streams[video_]->time_base.den;
  qint64 tick = qRound64(seconds * scale);
  if (av_seek_frame(format_, video_, tick, AVSEEK_FLAG_BACKWARD) < 0)
    return {};
  auto *pkt = av_packet_alloc();
  if (!pkt)
    return {};
  QVector<Picture> pics;
  bool valid = true;
  int bytes = 0;
  while (av_read_frame(format_, pkt) >= 0) {
    if (pkt->stream_index == video_) {
      if (!pics.empty() && (pkt->flags & AV_PKT_FLAG_KEY))
        break;
      auto pic = picture(pkt);
      if (!pic || pics.size() >= 2048 || pkt->size > 64 * 1024 * 1024 - bytes) {
        valid = false;
        break;
      }
      bytes += pkt->size;
      plan.ticks.append(pic->pts);
      pics.append(std::move(*pic));
    }
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
  if (!valid || pics.empty() || !pics.front().idr)
    return {};
  std::sort(plan.ticks.begin(), plan.ticks.end());
  if (std::adjacent_find(plan.ticks.begin(), plan.ticks.end()) !=
          plan.ticks.end() ||
      tick < plan.ticks.front())
    return {};
  auto it = std::upper_bound(plan.ticks.begin(), plan.ticks.end(), tick);
  int target = int(it - plan.ticks.begin()) - 1;
  // Retain the entire half-second review window used by the background
  // decoder, including dependencies. A shorter prefix can leave the demuxer's
  // already buffered packets without the pictures needed by a 24-step review.
  int floor = std::max(0, target - 32);
  QVector<bool> keep(pics.size());
  std::fill(keep.begin() + floor, keep.end(), true);
  for (auto p = pics.crbegin(); p != pics.crend(); ++p) {
    int i = int(std::lower_bound(plan.ticks.begin(), plan.ticks.end(), p->pts) -
                plan.ticks.begin());
    if ((i & 255) != p->poc)
      return {};
    if (keep[i]) {
      for (int delta : p->refs) {
        if (i + delta < 0 || i + delta >= keep.size())
          return {};
        keep[i + delta] = true;
      }
    } else {
      plan.skipped.append(p->nal);
      plan.omittedPts.append(p->pts);
    }
  }
  std::sort(plan.skipped.begin(), plan.skipped.end(),
            [](auto a, auto b) { return a.offset < b.offset; });
  plan.firstSafe = plan.ticks[floor];
  plan.scale = scale;plan.prefix=target;
  return plan;
}
void VideoStream::prepareFlvIndex() {
  flvIndex_ = std::async(std::launch::async, [this] {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? flvSeekMetadata(file, cancelled) : QByteArray{};
  }).share();
}
QByteArray VideoStream::flvMetadata() const {
  // A slow disk must still start playing. The worker can finish caching the
  // index during playback for the next open. Closing cancels both waits.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!cancelled && std::chrono::steady_clock::now() < deadline) {
    if (flvIndex_.wait_for(std::chrono::milliseconds(25)) == std::future_status::ready)
      return flvIndex_.get();
  }
  return {};
}
bool VideoStream::attach(mpv_handle *handle, void *fn) {
  auto reg = reinterpret_cast<decltype(&mpv_stream_cb_add_ro)>(fn);
  return reg && reg(handle, "fvmedia", this, openStream) >= 0;
}
void VideoStream::apply(const VideoSeekPlan &plan) {
  std::lock_guard lock(mutex_);
  spans_ = plan.skipped;
}
void VideoStream::filter(char *data, qint64 start, qint64 count) {
  std::lock_guard lock(mutex_);
  auto it = std::lower_bound(
      spans_.begin(), spans_.end(), start,
      [](auto s, qint64 pos) { return s.offset + s.length <= pos; });
  for (; it != spans_.end() && it->offset < start + count; ++it) {
    auto begin = std::max(start, it->offset),
         end = std::min(start + count, it->offset + it->length);
    std::memset(data + begin - start, 0xff, size_t(end - begin));
    auto byte = [&](qint64 pos, char v) {
      if (pos >= begin && pos < end)
        data[pos - start] = v;
    };
    byte(it->offset, 76);
    byte(it->offset + 1, 1);
    byte(it->offset + it->length - 1, char(-128));
  }
}

std::optional<QString> VideoIndex::caption(int stream, double seconds) {
  std::lock_guard guard(lock_);
  if (stream < 0 || unsigned(stream) >= format_->nb_streams)
    return {};
  auto *s = format_->streams[stream];
  if (s->codecpar->codec_id != AV_CODEC_ID_MOV_TEXT)
    return {};
  const auto timestamp = qRound64(seconds / av_q2d(s->time_base));
  int i = av_index_search_timestamp(s, timestamp,
                                    AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
  if (i < 0)
    return QString();
  const auto *entry = avformat_index_get_entry(s, i);
  if (!entry || entry->size < 2 || entry->size > 65536)
    return {};
  QFile f(file);
  if (!f.open(QIODevice::ReadOnly) || !f.seek(entry->pos))
    return {};
  auto data = f.read(entry->size);
  if (data.size() != entry->size)
    return {};
  auto n = qFromBigEndian<quint16>(data.constData());
  if (n > data.size() - 2)
    return {};
  return QString::fromUtf8(data.constData() + 2, n);
}
