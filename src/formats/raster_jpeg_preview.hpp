#pragma once
#include "formaview/image_preview.hpp"
#include <QByteArray>
#include <QImage>
#include <QtJpeg/jpeglib.h>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

namespace formaview::imaging {
// Do not ask QImageReader to allocate a clipped rectangle before resizing it.
// Decode scaled scanlines and retain only the requested output samples. The
// decoder still owns its codec working set (notably progressive coefficients).
inline QImage preview_jpeg(const QByteArray &bytes, const PreviewRequest &r,
                           std::stop_token stop) {
  struct Decoder {
    jpeg_decompress_struct codec{};
    jpeg_error_mgr error{};
    Decoder() {
      codec.err = jpeg_std_error(&error);
      error.error_exit = [](j_common_ptr c) {
        char message[JMSG_LENGTH_MAX];
        c->err->format_message(c, message);
        throw skeletal::Failure(ErrorCode::InvalidArgument,
                                std::string("JPEG: ") + message);
      };
      jpeg_create_decompress(&codec);
    }
    ~Decoder() { jpeg_destroy_decompress(&codec); }
  } decoder;
  auto &c = decoder.codec;
  if (static_cast<std::uint64_t>(bytes.size()) > ULONG_MAX)
    throw skeletal::Failure(ErrorCode::ResourceLimit,
                            "JPEG input exceeds codec address range");
  jpeg_mem_src(&c, reinterpret_cast<const unsigned char *>(bytes.constData()),
               static_cast<unsigned long>(bytes.size()));
  jpeg_read_header(&c, TRUE);
  if (c.jpeg_color_space == JCS_CMYK || c.jpeg_color_space == JCS_YCCK)
    throw skeletal::Failure(ErrorCode::Unsupported,
                            "CMYK JPEG preview is unsupported");
  c.out_color_space =
      c.jpeg_color_space == JCS_GRAYSCALE ? JCS_GRAYSCALE : JCS_RGB;
  // Pick a DCT scale which supplies at least the requested resolution.
  c.scale_num = static_cast<unsigned int>(std::clamp<std::uint64_t>(
      std::max((r.output_width * 8 + r.width - 1) / r.width,
               (r.output_height * 8 + r.height - 1) / r.height),
      1, 8));
  c.scale_denom = 8;
  jpeg_start_decompress(&c);
  const auto components = static_cast<std::size_t>(c.output_components);
  QImage output(
      static_cast<int>(r.output_width), static_cast<int>(r.output_height),
      components == 1 ? QImage::Format_Grayscale8 : QImage::Format_RGB888);
  if (output.isNull())
    throw std::bad_alloc();
  std::vector<unsigned char> row(static_cast<std::size_t>(c.output_width) *
                                 components);
  for (std::uint64_t y = 0; y < r.output_height; ++y) {
    preview_cancel(stop);
    const auto sy =
        std::min<std::uint64_t>(r.sy(y) * c.scale_num / 8, c.output_height - 1);
    while (c.output_scanline <= sy) {
      preview_cancel(stop);
      auto *line = row.data();
      if (jpeg_read_scanlines(&c, &line, 1) != 1)
        throw skeletal::Failure(ErrorCode::InvalidArgument,
                                "Incomplete JPEG scanline");
    }
    auto *out = output.scanLine(static_cast<int>(y));
    for (std::uint64_t x = 0; x < r.output_width; ++x) {
      const auto sx = std::min<std::uint64_t>(r.sx(x) * c.scale_num / 8,
                                              c.output_width - 1);
      std::memcpy(out + x * components, row.data() + sx * components,
                  components);
    }
  }
  // The ROI can end before the last scanline. No need to decode the tail.
  jpeg_abort_decompress(&c);
  return output;
}
} // namespace formaview::imaging
