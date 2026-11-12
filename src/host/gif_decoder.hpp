#pragma once
#include "formaview/skeletal.hpp"
#include <QFile>
#include <QImageReader>
#include <QBuffer>
#include <QRect>
#include <stop_token>
#include <algorithm>
#include <vector>

namespace formaview::host {
// Qt decodes isolated frame pixels. Canvas disposal belongs here so neither
// RGB32 canvas alpha nor the active local palette can alter the background.
class GifDecoder {
public:
  static constexpr quint64 maxCanvasPixels = 16ULL * 1024 * 1024;
  explicit GifDecoder(const QString &path, std::stop_token stop) : file_(path) {
    if (!file_.open(QIODevice::ReadOnly)) fail("Cannot open GIF input");
    if (file_.size() > 512LL * 1024 * 1024) limit();
    header_ = take(13);
    if (header_.left(6) != "GIF89a" && header_.left(6) != "GIF87a")
      fail("Invalid GIF header");
    size_ = QSize(word(header_, 6), word(header_, 8));
    if (size_.isEmpty() || size_.width() > 16384 || size_.height() > 16384 ||
        quint64(size_.width()) * size_.height() > maxCanvasPixels) limit();
    const auto packed = byte(header_, 10);
    if (packed & 128) {
      global_ = take(3 * (2 << (packed & 7)));
      const int background = byte(header_, 11);
      if (background * 3 + 2 >= global_.size()) fail("Invalid GIF background index");
      background_ = qRgb(byte(global_, background * 3),
                         byte(global_, background * 3 + 1),
                         byte(global_, background * 3 + 2));
    }
    QByteArray control;
    bool trailer = false;
    while (!file_.atEnd()) {
      cancelled(stop);
      const auto introducer = byte(take(1), 0);
      if (introducer == 0x3b) { trailer = true; break; }
      if (introducer == 0x21) {
        const auto label = byte(take(1), 0);
        if (label == 0xf9) {
          if (byte(take(1), 0) != 4) fail("Invalid GIF control extension");
          control = take(4);
          if (byte(take(1), 0) != 0) fail("Invalid GIF control terminator");
        } else {
          if (label == 1) fail("GIF plain-text rendering is unsupported");
          const auto firstSize = byte(take(1), 0);
          const auto first = take(firstSize);
          bool loopExtension = label == 0xff &&
              (first == "NETSCAPE2.0" || first == "ANIMEXTS1.0");
          if (firstSize) {
            for (;;) {
              cancelled(stop);
              const int length = byte(take(1), 0);
              if (!length) break;
              const auto data = take(length);
              if (loopExtension && length == 3 && byte(data, 0) == 1) {
                const int repeats = word(data, 1);
                loops_ = repeats == 0 ? -1 : repeats;
              }
            }
          }
        }
        continue;
      }
      if (introducer != 0x2c) fail("Invalid GIF block introducer");
      Frame frame;
      frame.offset = file_.pos();
      const auto descriptor = take(9);
      frame.rect = QRect(word(descriptor, 0), word(descriptor, 2),
                         word(descriptor, 4), word(descriptor, 6));
      if (frame.rect.isEmpty() || !QRect(QPoint(), size_).contains(frame.rect))
        fail("GIF frame rectangle exceeds its canvas");
      frame.control = control;
      if (!control.isEmpty()) {
        frame.disposal = (byte(control, 0) >> 2) & 7;
        if (frame.disposal > 3) fail("Unsupported GIF disposal method");
        const int delay = word(control, 1) * 10;
        frame.delay = delay < 20 ? 100 : delay;
      }
      control.clear();
      // Preserve the conventional transparent canvas when the first frame
      // marks the logical background index as transparent.
      if (frames_.empty() && !frame.control.isEmpty() &&
          (byte(frame.control, 0) & 1) &&
          byte(frame.control, 3) == byte(header_, 11))
        background_ = qRgba(0, 0, 0, 0);
      const int imageFlags = byte(descriptor, 8);
      if (imageFlags & 128) take(3 * (2 << (imageFlags & 7)));
      else if (global_.isEmpty()) fail("GIF frame has no color table");
      const int lzw = byte(take(1), 0);
      if (lzw < 2 || lzw > 8) fail("Invalid GIF LZW code size");
      for (;;) {
        cancelled(stop);
        const int length = byte(take(1), 0);
        if (!length) break;
        take(length);
      }
      frame.length = file_.pos() - frame.offset;
      if (frame.length > 64LL * 1024 * 1024) limit();
      frames_.push_back(frame);
      if (frames_.size() > 10000 || quint64(size_.width()) * size_.height() *
          frames_.size() > 4ULL * 1024 * 1024 * 1024) limit();
    }
    if (!trailer || frames_.empty()) fail("GIF is truncated or has no frames");
    canvas_ = QImage(size_, QImage::Format_ARGB32);
    if (canvas_.isNull()) limit();
    canvas_.fill(background_);
  }
  int imageCount() const { return static_cast<int>(frames_.size()); }
  QSize size() const { return size_; }
  int loopCount() const { return loops_; }
  int nextImageDelay() const { return frames_.at(next_ - 1).delay; }
  int nextFrame() const { return next_; }
  QImage read(std::stop_token stop) {
    cancelled(stop);
    if (next_ >= imageCount()) fail("GIF frame index exceeds the animation");
    const auto &frame = frames_[next_];
    if (!file_.seek(frame.offset)) fail("Cannot seek GIF frame");
    auto payload = take(frame.length);
    auto header = header_;
    putWord(header, 6, frame.rect.width());
    putWord(header, 8, frame.rect.height());
    QByteArray single = header + global_;
    if (!frame.control.isEmpty()) {
      auto control = frame.control;
      control[0] = char(byte(control, 0) & 1);
      single += QByteArray::fromHex("21f904") + control + char(0);
    }
    putWord(payload, 0, 0);
    putWord(payload, 2, 0);
    // Decode encoded rows in stream order. Qt's progressive interlace row
    // replication misorders short images; map the four passes ourselves.
    const bool interlaced = byte(payload, 8) & 0x40;
    payload[8] = char(byte(payload, 8) & ~0x40);
    single += ',';
    single += payload;
    single += ';';
    QBuffer buffer(&single);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "gif");
    const auto patch = reader.read().convertToFormat(QImage::Format_ARGB32);
    if (patch.isNull() || patch.size() != frame.rect.size())
      fail("GIF frame decode failed: " + reader.errorString().toStdString());
    if (next_) {
      const auto &previous = frames_[next_ - 1];
      if (previous.disposal == 2) {
        canvas_.detach();
        if (canvas_.isNull()) limit();
        for (int y = previous.rect.top(); y <= previous.rect.bottom(); ++y) {
          cancelled(stop);
          auto *row = reinterpret_cast<QRgb *>(canvas_.scanLine(y));
          std::fill(row + previous.rect.left(), row + previous.rect.right() + 1, background_);
        }
      } else if (previous.disposal == 3) canvas_ = saved_;
    }
    saved_ = frame.disposal == 3 ? canvas_ : QImage{};
    canvas_.detach();
    if (canvas_.isNull()) limit();
    int sourceRow = 0;
    const auto composeRow = [&](int y) {
      cancelled(stop);
      const auto *source = reinterpret_cast<const QRgb *>(patch.constScanLine(sourceRow++));
      auto *destination = reinterpret_cast<QRgb *>(canvas_.scanLine(y + frame.rect.y())) + frame.rect.x();
      for (int x = 0; x < patch.width(); ++x)
        if (qAlpha(source[x])) destination[x] = source[x];
    };
    if (interlaced) {
      static constexpr int starts[]{0, 4, 2, 1}, steps[]{8, 8, 4, 2};
      for (int pass = 0; pass < 4; ++pass)
        for (int y = starts[pass]; y < patch.height(); y += steps[pass]) composeRow(y);
    } else
      for (int y = 0; y < patch.height(); ++y) composeRow(y);
    ++next_;
    return canvas_;
  }
private:
  struct Frame { qint64 offset{}, length{}; QRect rect; QByteArray control; int disposal{}, delay{100}; };
  [[noreturn]] static void fail(const std::string &message) {
    throw skeletal::Failure(ErrorCode::InvalidArgument, message);
  }
  [[noreturn]] static void limit() {
    throw skeletal::Failure(ErrorCode::ResourceLimit, "GIF exceeds the animation memory, dimensions, frame count or decode budget");
  }
  static void cancelled(std::stop_token stop) {
    if (stop.stop_requested()) throw skeletal::Failure(ErrorCode::Cancelled, "GIF decoding cancelled");
  }
  QByteArray take(qint64 size) {
    auto data = file_.read(size);
    if (data.size() != size) fail("GIF data is truncated");
    return data;
  }
  static int byte(const QByteArray &data, int offset) { return static_cast<unsigned char>(data.at(offset)); }
  static int word(const QByteArray &data, int offset) { return byte(data, offset) | (byte(data, offset + 1) << 8); }
  static void putWord(QByteArray &data, int offset, int value) {
    data[offset] = char(value & 255); data[offset + 1] = char(value >> 8);
  }
  QFile file_;
  QByteArray header_, global_;
  QSize size_;
  QRgb background_{qRgba(0, 0, 0, 0)};
  std::vector<Frame> frames_;
  QImage canvas_, saved_;
  int next_{}, loops_{};
};
} // namespace formaview::host
