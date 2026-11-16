#pragma once
#include "formaview/skeletal.hpp"
#include <QFile>
#include <QImage>
#include <QRect>
#include <algorithm>
#include <array>
#include <cstdint>
#include <stop_token>
#include <vector>

namespace formaview::host {
// Stream LZW rows into a bounded canvas sampled in logical coordinates.
// Disposal and transparent palette entries apply before publishing each frame.
class GifDecoder {
public:
  static constexpr quint64 maxCanvasPixels = 16ULL * 1024 * 1024;
  explicit GifDecoder(const QString &path, std::stop_token stop,
                      QRect region = {}, QSize target = QSize(2048, 2048),
                      bool bounded = true)
      : file_(path) {
    if (!file_.open(QIODevice::ReadOnly)) fail("Cannot open GIF input");
    if (!bounded && file_.size() > 512LL * 1024 * 1024)
      limit();
    header_ = take(13);
    if (header_.left(6) != "GIF89a" && header_.left(6) != "GIF87a")
      fail("Invalid GIF header");
    size_ = QSize(word(header_, 6), word(header_, 8));
    if (size_.isEmpty())
      limit();
    if (!bounded && (size_.width() > 16384 || size_.height() > 16384 ||
                     quint64(size_.width()) * size_.height() > maxCanvasPixels))
      limit();
    region_ = region.isEmpty() ? QRect(QPoint(), size_)
                               : region.intersected(QRect(QPoint(), size_));
    if (region_.isEmpty())
      fail("Empty GIF preview region");
    physical_ = bounded ? region_.size()
                              .scaled(target.boundedTo(QSize(2048, 2048)),
                                      Qt::KeepAspectRatio)
                              .boundedTo(region_.size())
                              .expandedTo(QSize(1, 1))
                        : size_;
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
      frames_.push_back(frame);
      if (frames_.size() > 10000)
        limit();
    }
    if (!trailer || frames_.empty()) fail("GIF is truncated or has no frames");
    if (!bounded && quint64(size_.width()) * size_.height() * frames_.size() >
                        4ULL * 1024 * 1024 * 1024)
      limit();
    canvas_ = QImage(physical_, QImage::Format_ARGB32);
    if (canvas_.isNull()) limit();
    canvas_.fill(background_);
  }
  int imageCount() const { return static_cast<int>(frames_.size()); }
  QSize size() const { return size_; }
  QRect region() const { return region_; }
  int loopCount() const { return loops_; }
  int nextImageDelay() const { return frames_.at(next_ - 1).delay; }
  int nextFrame() const { return next_; }
  QImage read(std::stop_token stop) {
    cancelled(stop);
    if (next_ >= imageCount()) fail("GIF frame index exceeds the animation");
    const auto &frame = frames_[next_];
    if (!file_.seek(frame.offset)) fail("Cannot seek GIF frame");
    if (next_) {
      const auto &previous = frames_[next_ - 1];
      if (previous.disposal == 2) {
        canvas_.detach();
        if (canvas_.isNull()) limit();
        for (int y = 0; y < physical_.height(); ++y) {
          cancelled(stop);
          auto *row = reinterpret_cast<QRgb *>(canvas_.scanLine(y));
          for (int x = 0; x < physical_.width(); ++x)
            if (previous.rect.contains(sourcePoint(x, y)))
              row[x] = background_;
        }
      } else if (previous.disposal == 3) canvas_ = saved_;
    }
    saved_ = frame.disposal == 3 ? canvas_ : QImage{};
    canvas_.detach();
    if (canvas_.isNull()) limit();
    decodeRows(frame, stop);
    ++next_;
    return canvas_;
  }
private:
  struct Frame;
  QPoint sourcePoint(int x, int y) const {
    return {region_.x() + int(qint64(x) * region_.width() / physical_.width()),
            region_.y() +
                int(qint64(y) * region_.height() / physical_.height())};
  }
  void decodeRows(const Frame &frame, std::stop_token stop);
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
  QFile file_;
  QByteArray header_, global_;
  QSize size_;
  QSize physical_;
  QRect region_;
  QRgb background_{qRgba(0, 0, 0, 0)};
  std::vector<Frame> frames_;
  QImage canvas_, saved_;
  int next_{}, loops_{};
};
inline void GifDecoder::decodeRows(const Frame &frame, std::stop_token stop) {
  const auto descriptor = take(9);
  const int flags = byte(descriptor, 8);
  QByteArray palette = global_;
  if (flags & 128)
    palette = take(3 * (2 << (flags & 7)));
  if (palette.isEmpty())
    fail("Truncated GIF palette");
  const int minimum = byte(take(1), 0);
  if (minimum < 2 || minimum > 8)
    fail("Invalid GIF code size");
  QByteArray block;
  int at = 0;
  const auto compressedByte = [&]() {
    if (at == block.size()) {
      const int length = byte(take(1), 0);
      if (!length)
        fail("Truncated GIF LZW block");
      block = take(length);
      at = 0;
    }
    return byte(block, at++);
  };
  std::uint32_t reservoir = 0;
  int available = 0;
  const auto code = [&](int bits) {
    while (available < bits) {
      reservoir |= std::uint32_t(compressedByte()) << available;
      available += 8;
    }
    const int value = int(reservoir & ((1U << bits) - 1));
    reservoir >>= bits;
    available -= bits;
    return value;
  };
  std::array<std::uint16_t, 4096> prefix{};
  std::array<unsigned char, 4096> suffix{}, stack{};
  for (int i = 0; i < 256; ++i)
    suffix[i] = static_cast<unsigned char>(i);
  std::vector<unsigned char> row(static_cast<std::size_t>(frame.rect.width()));
  std::vector<int> rows;
  if (flags & 64) {
    static constexpr int starts[]{0, 4, 2, 1}, steps[]{8, 8, 4, 2};
    for (int p = 0; p < 4; ++p)
      for (int y = starts[p]; y < frame.rect.height(); y += steps[p])
        rows.push_back(y);
  } else
    for (int y = 0; y < frame.rect.height(); ++y)
      rows.push_back(y);
  const int transparent =
      !frame.control.isEmpty() && (byte(frame.control, 0) & 1)
          ? byte(frame.control, 3)
          : -1;
  const auto total = quint64(frame.rect.width()) * frame.rect.height();
  quint64 emitted = 0;
  const auto emitPixel = [&](unsigned char value) {
    if (emitted >= total || int(value) * 3 + 2 >= palette.size())
      fail("Invalid GIF pixel stream");
    row[static_cast<std::size_t>(emitted % frame.rect.width())] = value;
    ++emitted;
    if (emitted % frame.rect.width())
      return;
    cancelled(stop);
    const int sy =
        frame.rect.y() +
        rows[static_cast<std::size_t>(emitted / frame.rect.width() - 1)];
    for (int y = 0; y < physical_.height(); ++y) {
      if (sourcePoint(0, y).y() != sy)
        continue;
      auto *dst = reinterpret_cast<QRgb *>(canvas_.scanLine(y));
      for (int x = 0; x < physical_.width(); ++x) {
        const auto sx = sourcePoint(x, y).x() - frame.rect.x();
        if (sx < 0 || sx >= frame.rect.width())
          continue;
        const int colour = row[static_cast<std::size_t>(sx)];
        if (colour != transparent)
          dst[x] =
              qRgb(byte(palette, colour * 3), byte(palette, colour * 3 + 1),
                   byte(palette, colour * 3 + 2));
      }
    }
  };
  const int clear = 1 << minimum, end = clear + 1;
  int next = end + 1, width = minimum + 1, old = -1, first = 0;
  bool cleared = false;
  for (;;) {
    cancelled(stop);
    const int current = code(width);
    if (current == clear) {
      next = end + 1;
      width = minimum + 1;
      old = -1;
      cleared = true;
      continue;
    }
    if (!cleared)
      fail("GIF stream lacks initial clear code");
    if (current == end)
      break;
    if (current > next || current >= 4096 || (old < 0 && current >= clear))
      fail("Invalid GIF LZW code");
    int value = current, count = 0;
    if (value == next) {
      stack[count++] = static_cast<unsigned char>(first);
      value = old;
    }
    while (value >= clear) {
      if (value >= next || count >= 4095)
        fail("Invalid GIF LZW dictionary chain");
      stack[count++] = suffix[value];
      value = prefix[value];
    }
    first = value;
    stack[count++] = static_cast<unsigned char>(first);
    while (count)
      emitPixel(stack[--count]);
    if (old >= 0 && next < 4096) {
      prefix[next] = static_cast<std::uint16_t>(old);
      suffix[next++] = static_cast<unsigned char>(first);
      if (next == (1 << width) && width < 12)
        ++width;
    }
    old = current;
  }
  if (emitted != total)
    fail("GIF LZW stream ended before all pixels");
}
} // namespace formaview::host
