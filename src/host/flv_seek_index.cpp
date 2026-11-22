#include "flv_seek_index.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtEndian>
#include <bit>
#include <vector>

namespace {
struct Anchor { quint32 ms; qint64 offset; };
constexpr int maxAnchors = 100000;
constexpr int maxTagSize = 18 * maxAnchors + 128;

void append32(QByteArray &out, quint32 value) {
    const auto be = qToBigEndian(value);
    out.append(reinterpret_cast<const char *>(&be), sizeof(be));
}
void number(QByteArray &out, double value) {
    const auto bits = qToBigEndian(std::bit_cast<quint64>(value));
    out.append(char(0));
    out.append(reinterpret_cast<const char *>(&bits), sizeof(bits));
}
QByteArray scriptTag(const std::vector<Anchor> &anchors) {
    QByteArray body = QByteArray::fromHex("02000a") + "onMetaData";
    body += QByteArray::fromHex("08000000010009") + "keyframes";
    body += QByteArray::fromHex("030005") + "times" + char(10);
    append32(body, quint32(anchors.size()));
    for (const auto &anchor : anchors) number(body, anchor.ms / 1000.);
    body += QByteArray::fromHex("000d") + "filepositions" + char(10);
    append32(body, quint32(anchors.size()));
    // Each remaining AMF number takes nine bytes. Two object terminators,
    // an 11-byte tag header and a four-byte previous-tag size complete the tag.
    const qint64 extra = body.size() + anchors.size() * 9 + 6 + 15;
    for (const auto &anchor : anchors) number(body, double(anchor.offset + extra));
    body += QByteArray::fromHex("000009000009");
    QByteArray tag;
    append32(tag, (18u << 24) | quint32(body.size()));
    tag.append(7, char(0));
    tag += body;
    append32(tag, quint32(body.size() + 11));
    return tag;
}
unsigned size24(const char *p) {
    return (quint8(p[0]) << 16) | (quint8(p[1]) << 8) | quint8(p[2]);
}
}

QByteArray flvSeekMetadata(QFile &source, const std::atomic_bool &cancelled) {
    const QFileInfo info(source);
    const auto length = info.size();
    if (length < minIndexedFlvSize || !source.seek(0)) return {};
    const QByteArray head = source.read(13);
    if (head.size() != 13 || !head.startsWith("FLV\x01") ||
        qFromBigEndian<quint32>(head.constData()+5) != 9 ||
        qFromBigEndian<quint32>(head.constData()+9) != 0) return {};

    const auto identity = info.absoluteFilePath().toUtf8() + '\0' + QByteArray::number(length)
        + '/' + QByteArray::number(info.lastModified().toMSecsSinceEpoch());
    QDir cache(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    const auto key = QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex();
    const auto path = cache.filePath("flv-seek-v1/" + QString::fromLatin1(key));
    QFile cached(path);
    if (cached.open(QIODevice::ReadOnly) && cached.size() <= maxTagSize + 32) {
        const auto digest = cached.read(32);
        const auto tag = cached.readAll();
        if (!tag.isEmpty() && QCryptographicHash::hash(tag, QCryptographicHash::Sha256) == digest)
            return tag;
    }
    cached.close();

    std::vector<Anchor> anchors;
    QByteArray block;
    qint64 blockStart = -1;
    qint64 pos = 13;
    quint32 previousSize = 0;
    while (pos < length) {
        // Only tag headers and trailing sizes are needed. Large payloads can
        // be skipped entirely. Buffered sequential reads also suit HDDs.
        if (pos - 4 < blockStart || pos + 13 > blockStart + block.size()) {
            if (cancelled.load() || !source.seek(pos - 4)) return {};
            blockStart = pos - 4;
            block = source.read(512 * 1024);
            if (block.size() < 17) return {};
        }
        const char *tag = block.constData() + (pos - blockStart);
        const unsigned size = size24(tag + 1);
        if (qFromBigEndian<quint32>(tag-4) != previousSize || size24(tag+8) ||
            length - pos < qint64(size) + 15) return {};
        if (tag[0] == 18) {
            if (size > 512 * 1024 || !source.seek(pos + 11)) return {};
            const auto script = source.read(size);
            if (script.size() != size || script.contains("keyframes")) return {};
        } else if (tag[0] == 9) {
            // Only conventional AVC coded keyframes (not sequence headers).
            // Enhanced/mixed codecs and timestamp discontinuities use libav's
            // normal demuxer behaviour.
            if (size < 2 || (quint8(tag[11]) & 0x8f) != 7) return {};
            if (quint8(tag[11]) == 0x17 && tag[12] == 1) {
                const quint32 ms = (quint32(quint8(tag[7])) << 24) | size24(tag+4);
                if ((!anchors.empty() && ms <= anchors.back().ms) || anchors.size() >= maxAnchors)
                    return {};
                anchors.push_back({ms,pos});
            }
        } else if (tag[0] != 8) return {};
        previousSize = size + 11;
        pos += size + 15;
    }
    if (pos != length || anchors.size() < 2 || !source.seek(length - 4)) return {};
    const auto tail = source.read(4);
    if (tail.size() != 4 || qFromBigEndian<quint32>(tail.constData()) != previousSize ||
        source.size() != length || QFileInfo(source).lastModified() != info.lastModified() || cancelled.load())
        return {};
    const auto tag = scriptTag(anchors);
    if (cache.mkpath("flv-seek-v1")) {
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly) &&
            file.write(QCryptographicHash::hash(tag, QCryptographicHash::Sha256)) == 32 &&
            file.write(tag) == tag.size()) file.commit();
    }
    return tag;
}
