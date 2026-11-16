#pragma once
#include "formaview/image_preview.hpp"
#include <QRect>
#include <QSize>
#include <cmath>

// One sizing policy for viewport coverage checks and worker allocations.
inline QSize imagePreviewSize(const formaview::ImageSubresource &source,
                              QRect region, QSize target, bool detail = false) {
  auto physical =
      region.size()
          .scaled(target.boundedTo(QSize(2048, 2048)), Qt::KeepAspectRatio)
          .boundedTo(region.size())
          .expandedTo(QSize(1, 1));
  std::uint64_t channelBytes = 0;
  for (const auto &c : source.channels)
    channelBytes += formaview::imaging::preview_component(c.samples.scalar);
  const auto maxPixels =
      (detail ? 64ULL : 32ULL) * 1024 * 1024 /
      std::max<std::uint64_t>(channelBytes, 16);
  if (quint64(physical.width()) * physical.height() > maxPixels) {
    const auto scale = std::sqrt(
        double(maxPixels) / (double(physical.width()) * physical.height()));
    physical = QSize(std::max(1, int(physical.width() * scale)),
                     std::max(1, int(physical.height() * scale)));
  }
  return physical;
}

inline bool imagePreviewCovers(QRect cached, QSize decoded, QRect wanted,
                               QSize desired) {
  // Allow one output pixel of rounding, never stretch a low-resolution
  // overview into a substitute for native detail.
  return cached.contains(wanted) &&
         quint64(decoded.width()) * wanted.width() + cached.width() >=
             quint64(desired.width()) * cached.width() &&
         quint64(decoded.height()) * wanted.height() + cached.height() >=
             quint64(desired.height()) * cached.height();
}
