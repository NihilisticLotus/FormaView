#include "video_frames.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}
#include <QColorSpace>
#include <QSet>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <windows.h>
namespace {
struct ThumbnailBudget {
  std::stop_token stop;
  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);

  bool expired() const {
    return stop.stop_requested() || std::chrono::steady_clock::now() >= deadline;
  }
  static int interrupt(void *opaque) {
    return static_cast<const ThumbnailBudget *>(opaque)->expired();
  }
};
struct Decoder {
  AVFormatContext *media = nullptr;
  AVCodecContext *codec = nullptr;
  AVPacket *pkt = av_packet_alloc();
  AVFrame *raw = av_frame_alloc();
  SwsContext *convert = nullptr;
  ~Decoder() {
    sws_freeContext(convert);
    av_frame_free(&raw);
    av_packet_free(&pkt);
    avcodec_free_context(&codec);
    avformat_close_input(&media);
  }
  int open(const QString &file, ThumbnailBudget *budget = nullptr) {
    if (!pkt || !raw)
      return -1;
    AVDictionary *options = nullptr;
    if (budget) {
      media = avformat_alloc_context();
      if (!media)
        return -1;
      media->interrupt_callback = {ThumbnailBudget::interrupt, budget};
      media->probesize = 512 * 1024;
      media->max_analyze_duration = AV_TIME_BASE / 2;
      av_dict_set(&options, "protocol_whitelist", "file", 0);
      av_dict_set(&options, "format_whitelist",
                  "mov,matroska,avi,mpegts,mpeg,mpegvideo,flv", 0);
    }
    const int opened = avformat_open_input(&media, file.toUtf8().constData(),
                                           nullptr, &options);
    av_dict_free(&options);
    if (opened < 0)
      return -1;
    int i = av_find_best_stream(media, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (budget && (i < 0 || media->streams[i]->codecpar->width <= 0)) {
      avformat_find_stream_info(media, nullptr);
      i = av_find_best_stream(media, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    }
    if (i < 0)
      return -1;
    auto *par = media->streams[i]->codecpar;
    const auto *impl = avcodec_find_decoder(par->codec_id);
    if (!impl)
      return -1;
    codec = avcodec_alloc_context3(impl);
    if (!codec || avcodec_parameters_to_context(codec, par) < 0)
      return -1;
    codec->thread_count = budget ? 1 : 10;
    if (budget)
      codec->max_pixels = 8192LL * 4320;
    return avcodec_open2(codec, impl, nullptr) < 0 ? -1 : i;
  }
  QImage image(QSize bounds, QImage::Format format = QImage::Format_RGBA8888) {
    auto size =
        QSize(raw->width, raw->height).scaled(bounds, Qt::KeepAspectRatio);
    if (size.isEmpty())
      return {};
    const auto rgbFormat = format == QImage::Format_RGBA64 ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGBA;
    convert = sws_getCachedContext(convert, raw->width, raw->height,
                                   static_cast<AVPixelFormat>(raw->format),
                                   size.width(), size.height(), rgbFormat,
                                   SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!convert)
      return {};
    const int *matrix = sws_getCoefficients(SWS_CS_ITU709);
    sws_setColorspaceDetails(convert, matrix,
                             raw->color_range == AVCOL_RANGE_JPEG, matrix, 1, 0,
                             1 << 16, 1 << 16);
    QImage result(size, format);
    if (result.isNull())
      return {};
    uint8_t *planes[4] = {result.bits()};
    int stride[4] = {int(result.bytesPerLine())};
    if (sws_scale(convert, raw->data, raw->linesize, 0, raw->height, planes,
                  stride) != size.height())
      return {};
    return result;
  }
  QImage reviewImage(QSize bounds) {
    QImage result = image(bounds, QImage::Format_RGBA64);
    if (result.isNull())
      return {};
    // swscale expands video levels and applies the YUV matrix, but its RGB
    // values are still BT.1886 display signals. Match mpv's sRGB SDR target.
    static const auto toPlaybackOutput =
        QColorSpace(QColorSpace::Primaries::SRgb, 2.4f)
            .transformationToColorSpace(QColorSpace::SRgb);
    // Quantize once, after transfer conversion, including on ten-bit sources.
    result.applyColorTransform(toPlaybackOutput, QImage::Format_RGBA8888);
    return result;
  }
};
} // namespace

QImage videoThumbnail(const QString &file, QSize bounds, std::stop_token stop) {
  ThumbnailBudget budget{stop};
  Decoder decoder;
  const int stream = decoder.open(file, &budget);
  if (stream < 0)
    return {};
  // Stop at the first decoded picture. No full-file index or playback session
  // is needed, including for multi-gigabyte recordings.
  for (int n = 0; n < 512 && !budget.expired(); ++n) {
    if (decoder.media->pb && decoder.media->pb->bytes_read > 16 * 1024 * 1024)
      break;
    const int read = av_read_frame(decoder.media, decoder.pkt);
    if (read < 0 && read != AVERROR_EOF)
      break;
    if (read == AVERROR_EOF || decoder.pkt->stream_index == stream) {
      const int sent = avcodec_send_packet(
          decoder.codec, read == AVERROR_EOF ? nullptr : decoder.pkt);
      if (sent == 0 && avcodec_receive_frame(decoder.codec, decoder.raw) == 0)
        return decoder.image(bounds);
    }
    av_packet_unref(decoder.pkt);
    if (read == AVERROR_EOF)
      break;
  }
  return {};
}

struct VideoReviewFrame {
  VideoSignature signature{};
  QSize original,display;
  QImage rgb;
  QByteArray packed;
  qsizetype bytes() const {return rgb.sizeInBytes()+packed.size();}
  static std::shared_ptr<VideoReviewFrame> from(Decoder &d,QSize size) {
    auto out=std::make_shared<VideoReviewFrame>();const AVFrame *f=d.raw;
    // Other transfer curves, primaries and ranges stay on the native renderer.
    if (f->colorspace != AVCOL_SPC_BT709 || f->color_primaries != AVCOL_PRI_BT709 ||
        f->color_trc != AVCOL_TRC_BT709 || f->color_range != AVCOL_RANGE_MPEG)
      return out;
    for(int y=0;y<8;++y){
      int row=(y+2)*f->height/12;auto *src=f->data[0]+row*f->linesize[0];
      for(int x=0;x<24;++x){
        int col=(x+1)*f->width/26;
        out->signature[24*y+x]=f->format==AV_PIX_FMT_YUV420P10LE?reinterpret_cast<const quint16*>(src)[col]/4.f:src[col];
      }
    }
    out->display=size;out->original=QSize(f->width,f->height);
    if(qint64(size.width())*size.height()<3000000 || f->format!=AV_PIX_FMT_YUV420P10LE || (f->width%8) || (f->height%2)){
      out->rgb=d.reviewImage(size);return out;
    }
    // Four ten-bit samples occupy five bytes. Preserve all source bits and omit
    // decoder line padding to retain over 30 UHD frames within the raw budget.
    out->packed.resize(qsizetype(f->width)*f->height*15/8);
    auto *dst=reinterpret_cast<quint8*>(out->packed.data());
    for(int plane=0;plane<3;++plane){
      const int w=f->width>>(plane ? 1 : 0),h=f->height>>(plane ? 1 : 0);
      for(int y=0;y<h;++y){
        auto *row=reinterpret_cast<const quint16*>(f->data[plane]+y*f->linesize[plane]);
        for(int x=0;x<w;x+=4){
          dst[0]=quint8(row[x]);dst[1]=quint8(row[x+1]);dst[2]=quint8(row[x+2]);dst[3]=quint8(row[x+3]);
          dst[4]=quint8((row[x]>>8)|((row[x+1]>>8)<<2)|((row[x+2]>>8)<<4)|((row[x+3]>>8)<<6));dst+=5;
        }
      }
    }
    return out;
  }
  QImage image()const{
    if(!rgb.isNull()||packed.isEmpty())return rgb;
    Decoder d;if(!d.raw)return {};
    d.raw->format=AV_PIX_FMT_YUV420P10LE;d.raw->width=original.width();d.raw->height=original.height();d.raw->color_range=AVCOL_RANGE_MPEG;
    if(av_frame_get_buffer(d.raw,32)<0)return {};
    auto *src=reinterpret_cast<const quint8*>(packed.constData());
    for(int plane=0;plane<3;++plane){
      const int w=original.width()>>(plane ? 1 : 0),h=original.height()>>(plane ? 1 : 0);
      for(int y=0;y<h;++y){
        auto *row=reinterpret_cast<quint16*>(d.raw->data[plane]+y*d.raw->linesize[plane]);
        for(int x=0;x<w;x+=4){
          for(int i=0;i<4;++i)row[x+i]=src[i]|(((src[4]>>(2*i))&3)<<8);
          src+=5;
        }
      }
    }
    return d.reviewImage(display);
  }
};
void VideoFrames::track(double seconds){
  {std::lock_guard guard(mutex_);clock_=seconds;}
  wake_.notify_one();
}
VideoFrames::VideoFrames(std::shared_ptr<VideoIndex> index)
    : index_(std::move(index)), thread_([this] { run(); }) {}
VideoFrames::~VideoFrames() {
  {
    std::lock_guard guard(mutex_);
    stop_ = true;
  }
  wake_.notify_one();
  thread_.join();
}
void VideoFrames::cancel() {
  prepare(0, {}, {});
}
void VideoFrames::prepare(double t, QSize size, const VideoSeekPlan &plan) {
  std::lock_guard guard(mutex_);
  ++generation_;
  time_ = clock_ = t;
  size_ = size;
  plan_ = plan;
  wake_.notify_one();
}
std::shared_ptr<const VideoReviewFrame> VideoFrames::frame(qint64 tick) {
  std::lock_guard guard(mutex_);
  if (cacheGeneration_ != generation_)
    return nullptr;
  const auto it = frames_.find(tick);
  return it == frames_.end() ? nullptr : it->second;
}
QImage VideoFrames::image(const VideoReviewFrame &frame) { return frame.image(); }
qint64 VideoFrames::identify(const VideoSignature &input){
  // Correlate independent source luma samples against the retained GPU picture.
  // Flat/ambiguous scenes use the decoder's timestamped picture instead.
  std::lock_guard guard(mutex_);
  if(cacheGeneration_ != generation_ || frames_.size()<3)return -1;
  double sum=0,sq=0;for(float a:input){sum+=a;sq+=a*a;}
  const double n=double(input.size()),energy=sq-sum*sum/n;
  if(energy<n*36)return -1;
  double best=1,second=1;qint64 result=-1;
  for(const auto &[t,img]:frames_){
    double sy=0,sy2=0,sxy=0;
    for(size_t i=0;i<input.size();++i){double y=img->signature[i];sy+=y;sy2+=y*y;sxy+=input[i]*y;}
    double var=sy2-sy*sy/n;if(var<n*36)continue;
    double cost=1-(sxy-sum*sy/n)/std::sqrt(energy*var);
    if(cost<best){second=best;best=cost;result=t;}else if(cost<second)second=cost;
  }
  return best<.006 && second-best>qMax(.00035,best*.35)?result:-1;
}
void VideoFrames::run() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  Decoder decoder;
  int stream = decoder.open(index_->file);
  if (stream < 0)
    return;
  quint64 processed = 0;
  while (true) {
    std::unique_lock guard(mutex_);
    wake_.wait(guard, [&] { return stop_ || generation_ != processed; });
    if (stop_)
      return;
    processed = generation_;
    // Invalidate on the caller's thread, release the old UHD pictures here.
    decltype(frames_) retired;
    frames_.swap(retired);
    bytes_ = 0;
    cacheGeneration_ = processed;
    auto plan = plan_;
    auto size = size_;
    double seconds = time_;
    guard.unlock();
    retired.clear();
    if (!plan.valid() || size.isEmpty())
      continue;
    QSet<qint64> omit(plan.omittedPts.cbegin(), plan.omittedPts.cend());
    const auto anchor = qRound64(seconds * plan.scale);
    const auto start = qMax(plan.firstSafe, anchor - plan.scale / 2);
    const auto end = anchor + plan.scale * 2 / 3;
    if (av_seek_frame(decoder.media, stream, start, AVSEEK_FLAG_BACKWARD) < 0)
      continue;
    avcodec_flush_buffers(decoder.codec);
    bool done = false;
    while (!done) {
      {
        std::lock_guard check(mutex_);
        if (stop_ || generation_ != processed)
          break;
      }
      av_packet_unref(decoder.pkt);
      const bool eof = av_read_frame(decoder.media, decoder.pkt) < 0;
      if (!eof && (decoder.pkt->stream_index != stream ||
                   omit.contains(decoder.pkt->pts)))
        continue;
      if (avcodec_send_packet(decoder.codec, eof ? nullptr : decoder.pkt) < 0)
        break;
      while (avcodec_receive_frame(decoder.codec, decoder.raw) >= 0) {
        auto tick = decoder.raw->best_effort_timestamp;
        if (tick > end) {
          done = true;
          break;
        }
        if (tick < start)
          continue;
        {
          std::unique_lock lock(mutex_);
          wake_.wait(lock,[&]{return stop_||generation_!=processed||tick<=qRound64((clock_+.08)*plan.scale);});
          if(stop_||generation_!=processed){done=true;break;}
        }
        auto image = VideoReviewFrame::from(decoder,size);
        std::lock_guard check(mutex_);
        if (stop_ || generation_ != processed) {
          done = true;
          break;
        }
        if (!image->bytes())
          continue;
        const auto cost = image->bytes();
        while (!frames_.empty() && bytes_ + cost > 384LL * 1024 * 1024) {
          bytes_ -= frames_.begin()->second->bytes();
          frames_.erase(frames_.begin());
        }
        if (!frames_.contains(tick)) {
          bytes_ += cost;
          frames_.emplace(tick, std::move(image));
        }
      }
      if (eof)
        break;
    }
  }
}
