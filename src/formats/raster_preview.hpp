#pragma once
#include "formaview/image_preview.hpp"
#include <QBuffer>
#include <QColorSpace>
#include <QImageReader>
#include <QtEndian>
#include <QtPng/png.h>

namespace formaview::imaging {
struct PngRead {
  const QByteArray &bytes;
  std::size_t offset{};
  std::stop_token stop;
};
template <class F>
auto with_png_header(const QByteArray &bytes, std::stop_token stop, F body) {
  PngRead input{bytes, 0, stop};
  auto *png = png_create_read_struct(
      PNG_LIBPNG_VER_STRING, nullptr,
      [](png_structp, png_const_charp message) {
        throw skeletal::Failure(ErrorCode::InvalidArgument,
                                std::string("PNG: ") + message);
      },
      [](png_structp, png_const_charp) {});
  if (!png)
    throw std::bad_alloc();
  struct Cleanup {
    png_structp png;
    png_infop info{};
    ~Cleanup() { png_destroy_read_struct(&png, &info, nullptr); }
  } cleanup{png, png_create_info_struct(png)};
  if (!cleanup.info)
    throw std::bad_alloc();
  png_set_user_limits(png, INT_MAX, INT_MAX);
  png_set_chunk_malloc_max(png, 1024 * 1024);
  png_set_chunk_cache_max(png, 128);
  png_set_read_fn(png, &input, [](png_structp p, png_bytep out, png_size_t n) {
    auto &in = *static_cast<PngRead *>(png_get_io_ptr(p));
    preview_cancel(in.stop);
    if (n > static_cast<std::size_t>(in.bytes.size()) - in.offset)
      png_error(p, "Truncated stream");
    std::memcpy(out, in.bytes.constData() + in.offset, n);
    in.offset += n;
  });
  png_read_info(png, cleanup.info);
  return body(png, cleanup.info);
}
inline QColorSpace preview_png_space(const QByteArray &bytes,
                                     std::stop_token stop) {
  return with_png_header(bytes, stop, [](png_structp png, png_infop info) {
    png_charp name{};
    int compression{};
    png_bytep profile{};
    png_uint_32 length{};
    if (png_get_iCCP(png, info, &name, &compression, &profile, &length) &&
        length) {
      auto cs = QColorSpace::fromIccProfile(
          QByteArray(reinterpret_cast<const char *>(profile), length));
      if (cs.description().isEmpty())
        cs.setDescription(QString::fromLatin1(name));
      return cs;
    }
    int intent{};
    if (png_get_sRGB(png, info, &intent) && intent >= 0 && intent <= 3)
      return QColorSpace(QColorSpace::SRgb);
    double gamma{};
    if (!png_get_gAMA(png, info, &gamma) || gamma <= 0)
      return QColorSpace{};
    double wx{}, wy{}, rx{}, ry{}, gx{}, gy{}, bx{}, by{};
    if (png_get_cHRM(png, info, &wx, &wy, &rx, &ry, &gx, &gy, &bx, &by)) {
      QColorSpace custom(QPointF(wx, wy), QPointF(rx, ry), QPointF(gx, gy),
                         QPointF(bx, by), QColorSpace::TransferFunction::Gamma,
                         1.F / float(gamma));
      if (custom.isValid())
        return custom;
    }
    return QColorSpace(QColorSpace::Primaries::SRgb,
                       QColorSpace::TransferFunction::Gamma,
                       1.F / float(gamma));
  });
}
inline QImage preview_png(const QByteArray &bytes, const PreviewRequest &r,
                          std::stop_token stop) {
  return with_png_header(bytes, stop, [&](png_structp png, png_infop info) {
    const auto width = png_get_image_width(png, info),
               height = png_get_image_height(png, info);
    const auto bits = png_get_bit_depth(png, info),
               colour = png_get_color_type(png, info);
    if (colour == PNG_COLOR_TYPE_PALETTE)
      png_set_palette_to_rgb(png);
    if (colour == PNG_COLOR_TYPE_GRAY && bits < 8)
      png_set_expand_gray_1_2_4_to_8(png);
    const bool transparent = png_get_valid(png, info, PNG_INFO_tRNS) != 0;
    if (transparent)
      png_set_tRNS_to_alpha(png);
    const bool alpha = (colour & PNG_COLOR_MASK_ALPHA) || transparent;
    const bool gray =
        !(colour & PNG_COLOR_MASK_COLOR) && colour != PNG_COLOR_TYPE_PALETTE;
    if (gray)
      png_set_gray_to_rgb(png);
    if (!alpha)
      png_set_add_alpha(png, bits == 16 ? 65535 : 255, PNG_FILLER_AFTER);
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    if (bits == 16)
      png_set_swap(png);
#endif
    const int passes = png_set_interlace_handling(png);
    png_read_update_info(png, info);
    const auto rowBytes = png_get_rowbytes(png, info);
    if (rowBytes > 64ULL * 1024 * 1024)
      throw skeletal::Failure(ErrorCode::ResourceLimit,
                              "PNG scanline exceeds 64 MiB");
    const auto component = bits == 16 ? 2U : 1U;
    if (rowBytes != std::uint64_t(width) * 4 * component)
      png_error(png, "Unexpected transformed row layout");
    std::vector<png_byte> row(rowBytes);
    QImage result(static_cast<int>(r.output_width),
                  static_cast<int>(r.output_height),
                  bits == 16 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    if (result.isNull())
      throw std::bad_alloc();
    result.fill(Qt::transparent);
    static constexpr unsigned xStart[]{0, 4, 0, 2, 0, 1, 0},
        yStart[]{0, 0, 4, 0, 2, 0, 1};
    static constexpr unsigned xStep[]{8, 8, 4, 4, 2, 2, 1},
        yStep[]{8, 8, 8, 4, 4, 2, 2};
    for (int pass = 0; pass < passes; ++pass)
      for (std::uint64_t y = 0; y < height; ++y) {
        preview_cancel(stop);
        png_read_row(png, row.data(), nullptr);
        if (passes > 1 &&
            (y < yStart[pass] || (y - yStart[pass]) % yStep[pass]))
          continue;
        for (auto py = r.first_y(y); py < r.first_y(y + 1); ++py) {
          auto *dst = result.scanLine(static_cast<int>(py));
          for (std::uint64_t px = 0; px < r.output_width; ++px) {
            const auto x = r.sx(px);
            if (passes > 1 &&
                (x < xStart[pass] || (x - xStart[pass]) % xStep[pass]))
              continue;
            std::memcpy(dst + px * 4 * component,
                        row.data() + x * 4 * component, 4 * component);
          }
        }
      }
    png_read_end(png, nullptr);
    return result;
  });
}
// Native BMP/DIB rows: palettes and arbitrary contiguous bitfields, no full
// QImage allocation. RLE BMP uses a bounded command stream below.
inline QImage preview_bmp(const QByteArray &bytes, bool dib,
                          const PreviewRequest &r, std::stop_token stop) {
  const auto *data = reinterpret_cast<const unsigned char *>(bytes.constData());
  const auto size = static_cast<std::uint64_t>(bytes.size());
  const std::uint64_t base = dib ? 0 : 14;
  const auto need = [&](std::uint64_t at, std::uint64_t n) {
    if (at > size || n > size - at)
      throw skeletal::Failure(ErrorCode::InvalidArgument, "Truncated BMP data");
  };
  const auto u16 = [&](std::uint64_t at) {
    need(at, 2);
    return qFromLittleEndian<quint16>(data + at);
  };
  const auto u32 = [&](std::uint64_t at) {
    need(at, 4);
    return qFromLittleEndian<quint32>(data + at);
  };
  const auto header = u32(base);
  const bool core = header == 12;
  if (!core && header != 40 && header != 64 && header != 108 && header != 124)
    throw skeletal::Failure(ErrorCode::Unsupported,
                            "BMP preview header is not supported");
  need(base, header);
  const auto w = core ? std::uint32_t(u16(base + 4)) : u32(base + 4);
  const auto rawHeight = core ? std::uint32_t(u16(base + 6)) : u32(base + 8);
  const bool top = (rawHeight & 0x80000000U) != 0;
  const auto height = top ? (~rawHeight + 1) : rawHeight;
  const auto bits = u16(base + (core ? 10 : 14));
  const auto compression = core ? 0U : u32(base + 16),
             used = core ? 0U : u32(base + 32);
  if (u16(base + (core ? 8 : 12)) != 1 || compression > 3 ||
      (compression == 3 && bits != 16 && bits != 32) ||
      (compression == 1 && bits != 8) || (compression == 2 && bits != 4))
    throw skeletal::Failure(ErrorCode::InvalidArgument,
                            "Invalid BMP planes or compression layout");
  if (!w || !height ||
      (bits != 1 && bits != 4 && bits != 8 && bits != 16 && bits != 24 &&
       bits != 32))
    throw skeletal::Failure(ErrorCode::Unsupported, "Unsupported BMP depth");
  std::uint32_t masks[4]{0x7c00, 0x3e0, 0x1f, 0};
  if (bits == 32) {
    masks[0] = 0xff0000;
    masks[1] = 0xff00;
    masks[2] = 0xff;
  }
  auto paletteAt = base + header;
  if (compression == 3 || compression == 6) {
    const auto maskAt = header == 40 ? paletteAt : base + 40;
    const int count = compression == 6 || header >= 108 ? 4 : 3;
    for (int c = 0; c < count; ++c)
      masks[c] = u32(maskAt + c * 4);
    if (header == 40)
      paletteAt += std::uint64_t(count) * 4;
  } else if (compression != 0 && compression != 1 && compression != 2)
    throw skeletal::Failure(ErrorCode::Unsupported,
                            "Unsupported BMP compression");
  if (header >= 108 && compression == 0 && bits == 32 &&
      u32(base + 52) == 0xff000000)
    masks[3] = 0xff000000;
  const auto colours = bits <= 8 ? (used ? used : (1U << bits)) : used;
  if (colours > 256)
    throw skeletal::Failure(ErrorCode::InvalidArgument, "Invalid BMP palette");
  const auto paletteStride = core ? 3U : 4U;
  need(paletteAt, std::uint64_t(colours) * paletteStride);
  const auto start =
      dib ? paletteAt + std::uint64_t(colours) * paletteStride : u32(10);
  const auto stride = ((std::uint64_t(w) * bits + 31) / 32) * 4;
  if (compression != 1 && compression != 2)
    need(start, stride * height);
  QImage result(static_cast<int>(r.output_width),
                static_cast<int>(r.output_height), QImage::Format_RGBA8888);
  if (result.isNull())
    throw std::bad_alloc();
  if (compression == 1 || compression == 2) {
    if (top || (compression == 1 && bits != 8) ||
        (compression == 2 && bits != 4) || !colours)
      throw skeletal::Failure(ErrorCode::InvalidArgument,
                              "Invalid BMP RLE layout");
    result.fill(
        qRgb(data[paletteAt + 2], data[paletteAt + 1], data[paletteAt]));
    std::uint64_t at = start, sx = 0, sy = 0;
    const auto next = [&]() {
      need(at, 1);
      return data[at++];
    };
    const auto pixel = [&](unsigned index) {
      if (sx >= w || sy >= height || index >= colours)
        throw skeletal::Failure(ErrorCode::InvalidArgument,
                                "BMP RLE pixel out of range");
      const auto y = height - 1 - sy;
      for (auto py = r.first_y(y); py < r.first_y(y + 1); ++py) {
        auto *dst = result.scanLine(static_cast<int>(py));
        for (auto px = r.first_x(sx); px < r.first_x(sx + 1); ++px) {
          const auto *p = data + paletteAt + index * paletteStride;
          dst[px * 4] = p[2];
          dst[px * 4 + 1] = p[1];
          dst[px * 4 + 2] = p[0];
          dst[px * 4 + 3] = 255;
        }
      }
      ++sx;
    };
    for (;;) {
      preview_cancel(stop);
      const auto count = next(), value = next();
      if (count) {
        for (unsigned i = 0; i < count; ++i)
          pixel(bits == 8 ? value : ((i & 1) ? value & 15 : value >> 4));
      } else if (value == 0) {
        sx = 0;
        ++sy;
        if (sy > height)
          throw skeletal::Failure(ErrorCode::InvalidArgument,
                                  "BMP RLE row out of range");
      } else if (value == 1)
        break;
      else if (value == 2) {
        sx += next();
        sy += next();
        if (sx > w || sy >= height)
          throw skeletal::Failure(ErrorCode::InvalidArgument,
                                  "BMP RLE delta out of range");
      } else {
        unsigned packed = 0;
        for (unsigned i = 0; i < value; ++i) {
          if (bits == 8 || !(i & 1))
            packed = next();
          pixel(bits == 8 ? packed : ((i & 1) ? packed & 15 : packed >> 4));
        }
        const auto bytesRead = bits == 8 ? value : (value + 1) / 2;
        if (bytesRead & 1)
          static_cast<void>(next());
      }
    }
    return result;
  }
  const auto masked = [](std::uint32_t value,
                         std::uint32_t mask) -> unsigned char {
    if (!mask)
      return 0;
    unsigned shift = 0;
    // Match Qt's BMP bitfield expansion (repeat significant bits), including
    // truncation of masks wider than eight bits, rather than rounding UNorm.
    // https://github.com/qt/qtbase/blob/dev/src/gui/image/qbmphandler.cpp
    while (mask >= 256 || !(mask & 1)) {
      mask >>= 1;
      ++shift;
    }
    value = (value >> shift) & mask;
    unsigned bits = 0;
    for (auto n = mask; n; n >>= 1)
      ++bits;
    if (bits >= 8)
      return static_cast<unsigned char>(value);
    value <<= 8 - bits;
    for (auto filled = bits; filled < 8; filled *= 2)
      value |= value >> filled;
    return static_cast<unsigned char>(value);
  };
  for (std::uint64_t y = 0; y < r.output_height; ++y) {
    preview_cancel(stop);
    const auto sy = r.sy(y);
    const auto *row = data + start + (top ? sy : height - 1 - sy) * stride;
    auto *dst = result.scanLine(static_cast<int>(y));
    for (std::uint64_t x = 0; x < r.output_width; ++x) {
      const auto sx = r.sx(x);
      unsigned char red{}, green{}, blue{}, alpha = 255;
      if (bits <= 8) {
        const auto index =
            (row[sx * bits / 8] >> (8 - bits - (sx * bits % 8))) &
            ((1U << bits) - 1);
        if (index >= colours)
          throw skeletal::Failure(ErrorCode::InvalidArgument,
                                  "BMP palette index exceeds table");
        const auto *p = data + paletteAt + index * paletteStride;
        red = p[2];
        green = p[1];
        blue = p[0];
      } else if (bits == 24) {
        const auto *p = row + sx * 3;
        red = p[2];
        green = p[1];
        blue = p[0];
      } else {
        const auto value =
            bits == 16 ? std::uint32_t(qFromLittleEndian<quint16>(row + sx * 2))
                       : qFromLittleEndian<quint32>(row + sx * 4);
        red = masked(value, masks[0]);
        green = masked(value, masks[1]);
        blue = masked(value, masks[2]);
        alpha = masks[3] ? masked(value, masks[3]) : 255;
      }
      dst[x * 4] = red;
      dst[x * 4 + 1] = green;
      dst[x * 4 + 2] = blue;
      dst[x * 4 + 3] = alpha;
    }
  }
  return result;
}
} // namespace formaview::imaging
