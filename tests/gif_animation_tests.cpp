#include "image_session.hpp"
#include <QFile>
#include <QDir>
#include <QImageReader>
#include <QTemporaryDir>
#include <QtTest>
#include <array>

namespace {
const auto twoFrame = QByteArrayLiteral("R0lGODlhAwACAIEAAP8AAAAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQFBAAAACwAAAAAAwACAAAIBgABCBwYEAAh+QQJDAAAACwAAAAAAwACAIEAAAAA/wAAAAAAAAAICAABBABAEEBAADs=");
const auto disposalFrames = QByteArrayLiteral("R0lGODlhBAADAIEAAP8AAAAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwECAAAh+QQEAwAAACwAAAAABAADAAAICAABCBxIUGBAACH5BAUHAAAALAAAAAAEAAMAgQAAAAD/AAAAAAAAAAgLAAEIFBggwECBAQEAIfkECQsAAAAsAAAAAAMAAgCBAAAAAAD/AAAAAAAACAcAAwAYODAgADs=");
const auto singleFrame = QByteArrayLiteral("R0lGODlhAgACAIEAAAoUHgAAAAAAAAAAACH5BAEAAAAALAAAAAACAAIAAAgGAAEIBBAQADs=");

QString fixture(QTemporaryDir &root, QString name, const QByteArray &base64) {
  const auto path = root.filePath(name);
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(QByteArray::fromBase64(base64)) < 1)
    return {};
  return path;
}
QColor pixel(const std::shared_ptr<const ImagePixels> &frame, int x, int y) {
  const auto width = static_cast<int>(frame->source.data_window.width);
  const auto i = static_cast<std::size_t>(y * width + x) * 4;
  return QColor::fromRgbF(frame->rgba[i], frame->rgba[i + 1], frame->rgba[i + 2], frame->rgba[i + 3]);
}
} // namespace

class GifAnimationTests final : public QObject {
  Q_OBJECT
private slots:
  void singleAndMalformedInputs() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    ImageSession session;
    const auto still = fixture(root, "single.gif", singleFrame);
    QVERIFY(!still.isEmpty());
    session.open(still);
    QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 5000);
    QVERIFY2(session.error().isEmpty(), qPrintable(session.error()));
    QVERIFY(session.available());
    QVERIFY(!session.animated());
    QCOMPARE(session.frameCount(), 1);
    const auto broken = root.filePath("broken.gif");
    QFile file(broken);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("GIF89a\x01", 7);
    file.close();
    session.open(broken);
    QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 5000);
    QVERIFY(!session.error().isEmpty());
  }

  void composedFramesDelaysLoopsAndSeekGeneration() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto source = fixture(root, "disposal.gif", disposalFrames);
    QVERIFY(!source.isEmpty());
    ImageSession session;
    session.open(source);
    QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 5000);
    QVERIFY2(session.error().isEmpty(), qPrintable(session.error()));
    QCOMPARE(session.frameCount(), 3);
    QCOMPARE(session.frameDelay(), 30);
    QCOMPARE(session.info().value("loopCount").toInt(), 2);
    QCOMPARE(pixel(session.pixels(), 3, 2), QColor(Qt::red));
    session.seekFrame(1);
    QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 5000);
    QCOMPARE(session.frameDelay(), 70);
    QCOMPARE(pixel(session.pixels(), 1, 1), QColor(Qt::green));
    QCOMPARE(pixel(session.pixels(), 3, 2), QColor(Qt::red));
    session.seekFrame(2);
    session.seekFrame(0);
    session.seekFrame(2);
    QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 5000);
    QCOMPARE(session.frame(), 2);
    QCOMPARE(session.frameDelay(), 110);
    QCOMPARE(pixel(session.pixels(), 0, 0), QColor(Qt::blue));
    QCOMPARE(pixel(session.pixels(), 1, 1), QColor(Qt::green));
    // No UI override yet: the embedded loop count is two repeats after the
    // initial pass, then playback stops on the last frame.
    session.togglePlay();
    QTRY_COMPARE_WITH_TIMEOUT(session.frame(), 0, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!session.playing(), 5000);
    QCOMPARE(session.frame(), 2);
    // Re-enabling the UI loop is an explicit infinite-loop override.
    session.setLoop(true);
    session.togglePlay();
    QTest::qWait(900);
    QVERIFY(session.playing());
    session.togglePlay();
    session.setLoop(false);
    session.seekFrame(2);
    QTRY_COMPARE_WITH_TIMEOUT(session.frame(), 2, 5000);
    session.togglePlay();
    QTRY_VERIFY_WITH_TIMEOUT(!session.playing(), 5000);
    QCOMPARE(session.frame(), 2);
  }

  void repeatedSeekKeepsPresentedStorageBounded() {
    QTemporaryDir root;
    const auto source = fixture(root, "animated.gif", twoFrame);
    ImageSession session;
    session.open(source);
    QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 5000);
    std::vector<std::weak_ptr<const ImagePixels>> oldFrames;
    for (int i = 0; i < 40; ++i) {
      oldFrames.push_back(session.pixels());
      session.seekFrame(i % 2);
      QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 5000);
      QCOMPARE(session.frame(), i % 2);
    }
    int retained = 0;
    for (const auto &frame : oldFrames)
      retained += !frame.expired();
    QVERIFY2(retained <= 1, "Old GIF presentation frames remained resident");
  }

  void disposalRestoreModes_data() {
    QTest::addColumn<int>("disposal");
    QTest::newRow("restore-background") << 2;
    QTest::newRow("restore-previous") << 3;
  }

  void disposalRestoreModes() {
    QFETCH(int, disposal);
    QTemporaryDir root;
    auto bytes = QByteArray::fromBase64(disposalFrames);
    const auto marker = QByteArray::fromHex("21f904");
    const auto first = bytes.indexOf(marker);
    // Give this fixture an unambiguous white background in both palettes,
    // while its first frame retains a local red palette.
    const auto redPalette = bytes.mid(13, 12);
    bytes.replace(13, 3, QByteArray::fromHex("ffffff"));
    bytes[first + 17] = static_cast<char>(0x81);
    bytes.insert(first + 18, redPalette);
    const auto second = bytes.indexOf(marker, first + marker.size());
    QVERIFY(second > first);
    bytes[second + 3] = static_cast<char>((disposal << 2) | 1);
    bytes.replace(second + 18, 3, QByteArray::fromHex("ffffff"));
    const auto source = fixture(root, "restore.gif", bytes.toBase64());
    ImageSession session;
    session.open(source);
    QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 5000);
    session.seekFrame(2);
    QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 5000);
    QVERIFY2(session.error().isEmpty(), qPrintable(session.error()));
    QCOMPARE(pixel(session.pixels(), 0, 0), QColor(Qt::blue));
    QCOMPARE(pixel(session.pixels(), 1, 1),
             QColor(disposal == 2 ? Qt::white : Qt::red));
  }

  void naturalPlaybackPauseSwitchAndReopen() {
    QTemporaryDir root;
    const auto source = fixture(root, "loop.gif", twoFrame);
    const auto still = fixture(root, "still.gif", singleFrame);
    ImageSession session;
    session.open(source);
    QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 5000);
    QCOMPARE(session.info().value("loopCount").toInt(), -1);
    std::vector<std::weak_ptr<const ImagePixels>> frames;
    int wraps = 0, previous = 0;
    connect(&session, &ImageSession::animationChanged, &session, [&] {
      if (!session.busy() && session.animated()) {
        if (previous == 1 && session.frame() == 0) ++wraps;
        previous = session.frame();
        frames.push_back(session.pixels());
      }
    });
    session.togglePlay();
    QTRY_VERIFY_WITH_TIMEOUT(wraps >= 12, 10000);
    session.togglePlay();
    QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 5000);
    const auto pausedFrame = session.pixels();
    QTest::qWait(250);
    QCOMPARE(session.pixels(), pausedFrame);
    int retained = 0;
    for (const auto &frame : frames) retained += !frame.expired();
    QVERIFY(retained <= 3);
    session.togglePlay();
    session.open(still);
    QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 5000);
    QCOMPARE(session.path(), still);
    QVERIFY(!session.playing());
    QVERIFY(!session.animated());
    session.open(source);
    QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 5000);
    QCOMPARE(session.frame(), 0);
    QVERIFY(!session.playing());
    session.clear();
    QVERIFY(!session.available());
  }

  void nonwhiteDisposalBackgroundPreviewAndExport() {
    QTemporaryDir root;
    auto bytes = QByteArray::fromBase64(disposalFrames);
    const auto marker = QByteArray::fromHex("21f904");
    const auto first = bytes.indexOf(marker);
    const auto second = bytes.indexOf(marker, first + marker.size());
    // Original failing variant: opaque red logical screen; the second frame
    // uses a local palette with transparent black and disposal 2.
    bytes[second + 3] = 9;
    const auto source = fixture(root, "nonwhite.gif", bytes.toBase64());
    ImageSession session;
    session.open(source);
    QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 5000);
    session.seekFrame(2);
    QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 5000);
    QVERIFY2(session.error().isEmpty(), qPrintable(session.error()));
    QCOMPARE(pixel(session.pixels(), 0, 0), QColor(Qt::blue));
    QCOMPARE(pixel(session.pixels(), 1, 1), QColor(Qt::red));
    const auto output = root.filePath("output");
    session.exportFrames(output, "frame-", "skip");
    QTRY_VERIFY_WITH_TIMEOUT(!session.exporting(), 5000);
    QVERIFY2(session.exportError().isEmpty(), qPrintable(session.exportError()));
    const QImage exported(output + "/frame-0002.png");
    QVERIFY(!exported.isNull());
    QCOMPARE(exported.pixelColor(1, 1), QColor(Qt::red));
    QCOMPARE(exported.pixelColor(0, 0), QColor(Qt::blue));
  }

  void localRectangleDisposal_data() {
    QTest::addColumn<int>("disposal");
    QTest::addColumn<bool>("transparentBackground");
    QTest::newRow("unspecified-blue-background") << 0 << false;
    QTest::newRow("keep-blue-background") << 1 << false;
    QTest::newRow("clear-blue-background") << 2 << false;
    QTest::newRow("restore-blue-background") << 3 << false;
    QTest::newRow("clear-transparent-background") << 2 << true;
    QTest::newRow("restore-transparent-background") << 3 << true;
  }

  void localRectangleDisposal() {
    QFETCH(int, disposal);
    QFETCH(bool, transparentBackground);
    QTemporaryDir root;
    // Hand-authored 3x2 logical screen, four-entry global palette. Each
    // frame is a 1x1 rectangle. LZW payloads are clear,index,end (3-bit codes).
    auto bytes = QByteArray::fromHex("47494638396103000200810000"
                                    "000000ff000000ff000000ff");
    bytes[11] = transparentBackground ? 0 : 3;
    const auto append = [&](int x, int y, int color, int mode, bool local) {
      bytes += QByteArray::fromHex("21f904");
      bytes += char((mode << 2) | 1);
      bytes += QByteArray::fromHex("03000000");
      bytes += ',';
      bytes += char(x); bytes += char(0); bytes += char(y); bytes += char(0);
      bytes += QByteArray::fromHex("01000100");
      bytes += char(local ? 0x81 : 0);
      // Deliberately conflicting local palette background: index 3 magenta.
      if (local) bytes += QByteArray::fromHex("000000ff000000ff00ff00ff");
      const int codes = 4 | (color << 3) | (5 << 6);
      bytes += char(2); bytes += char(2);
      bytes += char(codes & 255); bytes += char(codes >> 8); bytes += char(0);
    };
    append(0, 0, 1, 1, false);
    append(1, 1, 2, disposal, true);
    append(2, 0, 1, 1, false);
    bytes += ';';
    const auto source = fixture(root, "rectangles.gif", bytes.toBase64());
    ImageSession session;
    session.open(source);
    QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 5000);
    const QColor background = transparentBackground ? QColor(0, 0, 0, 0) : QColor(Qt::blue);
    const auto assertFrame = [&](int frame, const QImage &image) {
      for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 3; ++x) {
          QColor expected = background;
          if ((x == 0 && y == 0) || (frame == 2 && x == 2 && y == 0)) expected = QColor(Qt::red);
          if (x == 1 && y == 1 && (frame == 1 || (frame == 2 && disposal <= 1))) expected = QColor(Qt::green);
          QCOMPARE(image.pixelColor(x, y), expected);
        }
    };
    for (int i : {0, 1, 2, 0, 2}) {
      session.seekFrame(i);
      QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 5000);
      QVERIFY2(session.error().isEmpty(), qPrintable(session.error()));
      QCOMPARE(session.frame(), i);
      const auto pixels = session.pixels();
      QImage image(3, 2, QImage::Format_ARGB32);
      for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 3; ++x) image.setPixelColor(x, y, pixel(pixels, x, y));
      assertFrame(i, image);
    }
    const auto output = root.filePath("frames");
    session.exportFrames(output, "rect-", "skip");
    QTRY_VERIFY_WITH_TIMEOUT(!session.exporting(), 5000);
    QVERIFY2(session.exportError().isEmpty(), qPrintable(session.exportError()));
    for (int i = 0; i < 3; ++i) {
      const QImage image(output + QString("/rect-%1.png").arg(i, 4, 10, QLatin1Char('0')));
      QVERIFY(!image.isNull());
      assertFrame(i, image);
    }
    QFile unchanged(source);
    QVERIFY(unchanged.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged.readAll(), bytes);
  }

  void paletteAndInterlace_data() {
    QTest::addColumn<bool>("local");
    QTest::addColumn<bool>("interlaced");
    QTest::newRow("global-sequential") << false << false;
    QTest::newRow("global-interlaced") << false << true;
    QTest::newRow("local-sequential") << true << false;
    QTest::newRow("local-interlaced") << true << true;
  }

  void paletteAndInterlace() {
    QFETCH(bool, local);
    QFETCH(bool, interlaced);
    QTemporaryDir root;
    // GIF87a without a graphic-control extension. Its four rows exercise
    // the isolated-frame path with either palette scope and interlace order.
    const auto palette = QByteArray::fromHex("000000ff000000ff000000ff");
    auto bytes = QByteArray::fromHex("47494638376101000400000000");
    if (!local) { bytes[10] = static_cast<char>(0x81); bytes += palette; }
    bytes += QByteArray::fromHex("2c0000000001000400");
    bytes += char((local ? 0x81 : 0) | (interlaced ? 0x40 : 0));
    if (local) bytes += palette;
    const std::array sequential{1, 2, 3, 1};
    const std::array interlaceOrder{1, 3, 2, 1};
    quint64 codes = 0;
    int bits = 0;
    for (int index : interlaced ? interlaceOrder : sequential) {
      codes |= quint64(4) << bits; bits += 3; // Clear keeps every code 3 bits.
      codes |= quint64(index) << bits; bits += 3;
    }
    codes |= quint64(5) << bits; bits += 3;
    bytes += char(2); bytes += char((bits + 7) / 8);
    for (int i = 0; i < (bits + 7) / 8; ++i) bytes += char((codes >> (i * 8)) & 255);
    bytes += QByteArray::fromHex("003b");
    ImageSession session;
    session.open(fixture(root, "palette.gif", bytes.toBase64()));
    QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 5000);
    QVERIFY2(session.error().isEmpty(), qPrintable(session.error()));
    QCOMPARE(session.frameCount(), 1);
    QCOMPARE(session.frameDelay(), 100);
    const std::array colors{QColor(Qt::red), QColor(Qt::green), QColor(Qt::blue), QColor(Qt::red)};
    for (int y = 0; y < 4; ++y) QCOMPARE(pixel(session.pixels(), 0, y), colors[y]);
  }

  void malformedDisposalAndBudgetRejected() {
    QTemporaryDir root;
    auto bytes = QByteArray::fromBase64(disposalFrames);
    const auto marker = QByteArray::fromHex("21f904");
    const auto first = bytes.indexOf(marker);
    bytes[first + 3] = char(7 << 2);
    const auto badDisposal = fixture(root, "reserved.gif", bytes.toBase64());
    bytes = QByteArray::fromBase64(disposalFrames);
    bytes[6] = char(1); bytes[7] = char(0x10);
    bytes[8] = char(1); bytes[9] = char(0x10);
    const auto overBudget = fixture(root, "budget.gif", bytes.toBase64());
    bytes = QByteArray::fromBase64(disposalFrames);
    bytes.chop(2);
    const auto truncated = fixture(root, "truncated.gif", bytes.toBase64());
    ImageSession session;
    for (const auto &source : {badDisposal, overBudget, truncated}) {
      session.open(source);
      QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 5000);
      QVERIFY(!session.error().isEmpty());
      QVERIFY(!session.playing());
    }
  }

  void cancelActiveStreamingExport() {
    QTemporaryDir root;
    auto bytes = QByteArray::fromBase64(twoFrame);
    const auto first = bytes.indexOf(QByteArray::fromHex("21f904"));
    QVERIFY(first > 0);
    const auto pair = bytes.mid(first, bytes.size() - first - 1);
    const auto header = bytes.left(first);
    bytes = header;
    for (int i = 0; i < 1000; ++i) bytes += pair;
    bytes += ';';
    const auto source = fixture(root, "long.gif", bytes.toBase64());
    ImageSession session;
    session.open(source);
    QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 5000);
    QCOMPARE(session.frameCount(), 2000);
    const auto output = root.filePath("cancelled");
    session.exportFrames(output, "frame-", "skip");
    QTRY_VERIFY_WITH_TIMEOUT(session.exportProgress() > 0 || !session.exporting(), 5000);
    QVERIFY2(session.exporting(), "Fixture must still be exporting for a real mid-stream cancellation");
    session.cancelExport();
    QTRY_VERIFY_WITH_TIMEOUT(!session.exporting(), 5000);
    QVERIFY(session.exportProgress() < 2000);
    QVERIFY(!session.exportError().isEmpty());
    const auto outputs = QDir(output).entryList({"*.png"}, QDir::Files);
    QVERIFY(!outputs.empty());
    QVERIFY(outputs.size() < 2000);
    for (const auto &name : outputs) QVERIFY(!QImage(output + '/' + name).isNull());
    QCOMPARE(QDir(output).entryList({".formaview-*"}, QDir::Files | QDir::Hidden).size(), 0);
  }

  void completedExportPublishesFinalProgressAfterDelayedPoll() {
    QTemporaryDir root;
    auto bytes = QByteArray::fromBase64(twoFrame);
    const auto first = bytes.indexOf(QByteArray::fromHex("21f904"));
    const auto pair = bytes.mid(first, bytes.size() - first - 1);
    bytes = bytes.left(first);
    for (int i = 0; i < 64; ++i) bytes += pair;
    bytes += ';';
    ImageSession session;
    session.open(fixture(root, "progress.gif", bytes.toBase64()));
    QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 5000);
    const auto output = root.filePath("frames");
    bool delayed = false, committed = false;
    connect(&session, &ImageSession::exportChanged, &session, [&] {
      if (delayed || !session.exporting() || session.exportProgress() <= 0 ||
          session.exportProgress() >= 128) return;
      delayed = true;
      // Simulate a busy consumer while the worker finishes, between poll's
      // progress sample and terminal snapshot. Do not dispatch nested events.
      for (int i = 0; i < 5000 && !QFileInfo::exists(output + "/frame-0127.png"); ++i)
        QTest::qSleep(1);
      committed = QFileInfo::exists(output + "/frame-0127.png");
      QTest::qSleep(30);
    });
    session.exportFrames(output, "frame-", "skip");
    QTRY_VERIFY_WITH_TIMEOUT(!session.exporting(), 10000);
    QVERIFY(delayed);
    QVERIFY(committed);
    QVERIFY2(session.exportError().isEmpty(), qPrintable(session.exportError()));
    QCOMPARE(session.exportProgress(), 128);
  }

  void exportPoliciesCancellationAndFailures() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto source = fixture(root, "animated.gif", twoFrame);
    ImageSession session;
    session.open(source);
    QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 5000);
    const auto output = root.filePath("frames");
    session.exportFrames(output, "tile-", "skip");
    QTRY_VERIFY_WITH_TIMEOUT(!session.exporting(), 5000);
    QVERIFY2(session.exportError().isEmpty(), qPrintable(session.exportError()));
    QCOMPARE(session.exportProgress(), 2);
    const auto first = output + "/tile-0000.png";
    {
      QImageReader png(first);
      const auto image = png.read();
      QVERIFY2(!image.isNull(), qPrintable(png.errorString()));
      QCOMPARE(image.size(), QSize(3, 2));
      QVERIFY(image.hasAlphaChannel());
    }
    QFile existing(first);
    QVERIFY(existing.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(existing.write("keep"), 4);
    existing.close();
    session.exportFrames(output, "tile-", "skip");
    QTRY_VERIFY_WITH_TIMEOUT(!session.exporting(), 5000);
    QVERIFY(existing.open(QIODevice::ReadOnly));
    QCOMPARE(existing.readAll(), QByteArray("keep"));
    existing.close();
    session.exportFrames(output, "tile-", "rename");
    QTRY_VERIFY_WITH_TIMEOUT(!session.exporting(), 5000);
    QVERIFY2(session.exportError().isEmpty(), qPrintable(session.exportError()));
    QVERIFY(QFileInfo::exists(output + "/tile-0000 (1).png"));
    session.exportFrames(output, "tile-", "overwrite");
    QTRY_VERIFY_WITH_TIMEOUT(!session.exporting(), 5000);
    QVERIFY2(session.exportError().isEmpty(), qPrintable(session.exportError()));
    QImageReader replaced(first);
    QVERIFY(!replaced.read().isNull());
    session.exportFrames(output, "cancel-", "skip");
    session.cancelExport();
    QTRY_VERIFY_WITH_TIMEOUT(!session.exporting(), 5000);
    QVERIFY(session.exportError().contains("cancel", Qt::CaseInsensitive) || session.exportProgress() == session.frameCount());
    const auto blocked = root.filePath("not-a-directory");
    QFile blocker(blocked);
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.write("x");
    blocker.close();
    session.exportFrames(blocked, "fail-", "skip");
    QTRY_VERIFY_WITH_TIMEOUT(!session.exporting(), 5000);
    QVERIFY(!session.exportError().isEmpty());

    // A PNG destination can be a hard link to the source despite its suffix.
    std::error_code linkError;
    std::filesystem::create_hard_link(std::filesystem::path(source.toStdWString()),
        std::filesystem::path((output + "/source-0000.png").toStdWString()), linkError);
    QVERIFY2(!linkError, linkError.message().c_str());
    session.exportFrames(output, "source-", "overwrite");
    QTRY_VERIFY_WITH_TIMEOUT(!session.exporting(), 5000);
    QVERIFY(!session.exportError().isEmpty());
    QFile unchanged(source);
    QVERIFY(unchanged.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged.readAll(), QByteArray::fromBase64(twoFrame));
  }
};

QTEST_GUILESS_MAIN(GifAnimationTests)
#include "gif_animation_tests.moc"
