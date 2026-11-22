#pragma once
#include <QByteArray>
#include <QFile>
#include <atomic>

inline constexpr qint64 minIndexedFlvSize = 64LL * 1024 * 1024;

// An extra FLV script tag, inserted after the 13-byte header in the player's
// read-only view. Empty means to pass the original file through unchanged.
QByteArray flvSeekMetadata(QFile &source, const std::atomic_bool &cancelled);
