#include "thumbnail.hpp"
#ifdef FORMAVIEW_IMAGE_UI
#include "formaview/image_conversion.hpp"
#endif
#include <QCryptographicHash>
#include <QFile>
#include <QGuiApplication>
#include <QImageReader>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <cstring>
#include <png.h>

namespace {
QString id(const QString &path) {
  return QString::fromLatin1(QUrl::toPercentEncoding(path).toBase64(
      QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
QByteArray hash(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return {};
  return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}
QImage load(ThumbnailProvider &provider, const QString &path,
            QString *error = nullptr) {
  const auto response = std::unique_ptr<QQuickImageResponse>(
      provider.requestImageResponse(id(path), QSize(8192, 8192)));
  QSignalSpy done(response.get(), &QQuickImageResponse::finished);
  if (!done.wait(10000)) {
    if (error)
      *error = "timeout";
    return {};
  }
  if (error)
    *error = response->errorString();
  if (!response->errorString().isEmpty())
    return {};
  const auto texture =
      std::unique_ptr<QQuickTextureFactory>(response->textureFactory());
  return texture ? texture->image() : QImage{};
}
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4611) // Only POD locals in the fixture jump frame.
#endif
bool writePngFixture(QFile *file, int type, bool interlace, bool transparency,
                     int depth) {
  auto *png =
      png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!png)
    return false;
  auto *info = png_create_info_struct(png);
  auto *jump = png_set_longjmp_fn(png, longjmp, sizeof(jmp_buf));
  if (!jump) {
    qWarning("PNG jump buffer unavailable (version=%s, sizeof=%zu)",
             png_get_libpng_ver(png), sizeof(jmp_buf));
    png_destroy_write_struct(&png, &info);
    return false;
  }
  if (setjmp(*jump)) {
    png_destroy_write_struct(&png, &info);
    return false;
  }
  png_set_write_fn(
      png, file,
      [](png_structp p, png_bytep bytes, png_size_t n) {
        auto *f = static_cast<QFile *>(png_get_io_ptr(p));
        if (f->write(reinterpret_cast<char *>(bytes), n) !=
            static_cast<qint64>(n))
          png_error(p, "write failed");
      },
      [](png_structp) {});
  png_set_IHDR(png, info, 56, 40, depth, type,
               interlace ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
  png_color palette[2]{{40, 170, 80}, {160, 30, 190}};
  png_byte alpha[2]{0, 127};
  png_color_16 transparent{};
  transparent.red = transparent.green = transparent.blue = transparent.gray =
      128;
  if (type == PNG_COLOR_TYPE_PALETTE)
    png_set_PLTE(png, info, palette, 2);
  if (transparency)
    png_set_tRNS(png, info, type == PNG_COLOR_TYPE_PALETTE ? alpha : nullptr,
                 type == PNG_COLOR_TYPE_PALETTE ? 2 : 0,
                 type == PNG_COLOR_TYPE_PALETTE ? nullptr : &transparent);
  png_write_info(png, info);
  const int passes = png_set_interlace_handling(png);
  png_byte row[56 * 8]{};
  const int channels = type == PNG_COLOR_TYPE_RGBA         ? 4
                       : type == PNG_COLOR_TYPE_RGB        ? 3
                       : type == PNG_COLOR_TYPE_GRAY_ALPHA ? 2
                                                           : 1;
  for (int pass = 0; pass < passes; ++pass)
    for (int y = 0; y < 40; ++y) {
      for (int x = 0; x < 56; ++x)
        for (int c = 0; c < channels; ++c) {
          const auto sample = static_cast<png_byte>(
              type == PNG_COLOR_TYPE_PALETTE ? (x + y) % 2
                                             : (x * 17 + y * 3 + c * 19) % 256);
          row[(x * channels + c) * (depth / 8)] = sample;
          if (depth == 16)
            row[(x * channels + c) * 2 + 1] = sample;
        }
      png_write_row(png, row);
    }
  png_write_end(png, info);
  png_destroy_write_struct(&png, &info);
  return true;
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif
bool pngFixture(const QString &path, int type, bool interlace,
                bool transparency, int depth = 8) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) &&
         writePngFixture(&file, type, interlace, transparency, depth);
}
} // namespace
class ThumbnailTests final : public QObject {
  Q_OBJECT
private slots:
  void pngMatrix() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    ThumbnailProvider provider;
    for (const int type :
         {PNG_COLOR_TYPE_GRAY, PNG_COLOR_TYPE_GRAY_ALPHA, PNG_COLOR_TYPE_RGB,
          PNG_COLOR_TYPE_RGBA, PNG_COLOR_TYPE_PALETTE}) {
      for (const bool interlace : {false, true}) {
        for (const int depth : {8, 16}) {
          if (type == PNG_COLOR_TYPE_PALETTE && depth == 16)
            continue;
          const bool transparency =
              depth == 8 &&
              (type == PNG_COLOR_TYPE_PALETTE || type == PNG_COLOR_TYPE_RGB ||
               type == PNG_COLOR_TYPE_GRAY);
          const auto path = temp.path() + QString("/matrix-%1-%2-%3.png")
                                              .arg(type)
                                              .arg(interlace)
                                              .arg(depth);
          QVERIFY(pngFixture(path, type, interlace, transparency, depth));
          const auto before = hash(path);
          const auto actual = load(provider, path);
          QVERIFY2(!actual.isNull(), qPrintable(path));
          QImageReader reader(path);
          // Qt's texture factory premultiplies for rendering. Compare in the
          // same storage format, including zero-alpha RGB canonicalization.
          const auto reference = reader.read()
                                     .convertToFormat(QImage::Format_RGBA8888)
                                     .convertToFormat(actual.format());
          QCOMPARE(actual.size(), reference.size());
          for (int y = 0; y < actual.height(); ++y)
            for (int x = 0; x < actual.width(); ++x)
              QCOMPARE(actual.pixel(x, y), reference.pixel(x, y));
          QCOMPARE(hash(path), before);
        }
      }
    }
  }
  void largePngAndPath() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const auto path = temp.path() + QString::fromUtf8("/中文 #100%25 空格.png");
    QImage image(4096, 4096, QImage::Format_RGBA8888);
    image.fill(QColor(40, 170, 80, 200));
    QVERIFY(image.save(path));
    image = {};
    QImageReader oldReader(path);
    oldReader.setScaledSize(QSize(120, 120));
    QVERIFY(
        oldReader.read().isNull()); // Reproduces the old full-decode rejection.
    ThumbnailProvider provider;
    const auto before = hash(path);
    QString error;
    const auto result = load(provider, path, &error);
    QVERIFY2(!result.isNull(), qPrintable(error));
    QCOMPARE(result.size(), QSize(120, 120));
    QCOMPARE(result.pixelColor(60, 60).rgba(), QColor(40, 170, 80, 200).rgba());
    QCOMPARE(hash(path), before);
    QCOMPARE(QImageReader::allocationLimit(), 32);

    QQmlEngine engine;
    engine.addImageProvider("thumb", new ThumbnailProvider);
    QQmlComponent component(&engine);
    component.setData(
        "import QtQuick; Image { property string path; source: "
        "'image://thumb/'+String(Qt.btoa(Array.from(encodeURIComponent(path), "
        "c => "
        "c.charCodeAt(0)))).replace(/\\+/g,'-').replace(/\\//g,'_').replace(/=/"
        "g,'')+'?v'; cache: false }",
        QUrl());
    auto object = std::unique_ptr<QObject>(
        component.createWithInitialProperties({{"path", path}}));
    QVERIFY2(object, qPrintable(component.errorString()));
    QTRY_COMPARE_WITH_TIMEOUT(object->property("status").toInt(), 1, 10000);
  }
  void cacheAndFailure() {
    QTemporaryDir temp;
    ThumbnailProvider provider;
    QImage image(160, 120, QImage::Format_RGBA8888);
    image.fill(Qt::red);
    const auto original = temp.path() + "/source.png";
    QVERIFY(image.save(original));
    QVERIFY(!load(provider, original).isNull());
    const auto cost = provider.cacheKiB();
    for (int i = 0; i < 10; ++i)
      QVERIFY(!load(provider, original).isNull());
    QCOMPARE(provider.cacheKiB(), cost);
    for (int i = 0; i < 460; ++i) {
      const auto path = temp.path() + '/' + QString::number(i) + ".png";
      QVERIFY(QFile::copy(original, path));
      QVERIFY(!load(provider, path).isNull());
      QVERIFY(provider.cacheKiB() <= 32768);
    }
    QVERIFY(provider.cacheKiB() > 32000);
    provider.clear();
    QCOMPARE(provider.cacheKiB(), 0);
    image.fill(Qt::green);
    QVERIFY(image.save(original));
    QCOMPARE(load(provider, original).pixelColor(80, 60), QColor(Qt::green));
    QString error;
    QVERIFY(load(provider, temp.path() + "/missing.png", &error).isNull());
    QVERIFY(!error.isEmpty());
    QFile bad(temp.path() + "/bad.png");
    QVERIFY(bad.open(QIODevice::WriteOnly));
    bad.write("\x89PNG\r\n\x1a\n", 8);
    bad.close();
    QVERIFY(load(provider, bad.fileName(), &error).isNull());
    QVERIFY(!error.isEmpty());
    for (int i = 0; i < 12; ++i) {
      auto *response = provider.requestImageResponse(id(original), {});
      QSignalSpy done(response, &QQuickImageResponse::finished);
      response->cancel();
      QVERIFY(done.wait(10000));
      QVERIFY(!response->errorString().isEmpty());
      delete response;
    }
    QCOMPARE(QImageReader::allocationLimit(), 32);
  }
  void boundsAndShutdown() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    ThumbnailProvider provider;
    const auto tooWide = temp.path() + "/too-wide.png";
    QImage wide(32769, 1, QImage::Format_RGB32);
    wide.fill(Qt::red);
    QVERIFY(wide.save(tooWide));
    QString error;
    QVERIFY(load(provider, tooWide, &error).isNull());
    QVERIFY(!error.isEmpty());
    const auto huge = temp.path() + "/input-budget.png";
    QFile file(huge);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.resize(128LL * 1024 * 1024 + 1));
    file.close();
    QVERIFY(load(provider, huge, &error).isNull());
    QVERIFY(error.contains("budget"));
    // Cancel both running and queued work; shutdown must finish all responses
    // without reading stale files and without requiring a GUI wait/join cycle.
    auto closing = std::make_unique<ThumbnailProvider>();
    std::vector<std::unique_ptr<QQuickImageResponse>> responses;
    std::vector<std::unique_ptr<QSignalSpy>> completions;
    for (int i = 0; i < 180; ++i) {
      responses.emplace_back(closing->requestImageResponse(id(tooWide), {}));
      completions.emplace_back(std::make_unique<QSignalSpy>(
          responses.back().get(), &QQuickImageResponse::finished));
      responses.back()->cancel();
    }
    closing->clear();
    QCOMPARE(closing->cacheKiB(), 0);
    closing.reset();
    for (std::size_t i = 0; i < responses.size(); ++i) {
      if (completions[i]->isEmpty())
        QVERIFY(completions[i]->wait(10000));
      QCOMPARE(completions[i]->size(), 1);
      QVERIFY(!responses[i]->errorString().isEmpty());
    }
    QCOMPARE(QImageReader::allocationLimit(), 32);
  }
  void rasterAndProfessionalFormats() {
    QTemporaryDir temp;
    ThumbnailProvider provider;
    QImage image(96, 64, QImage::Format_RGB32);
    image.fill(QColor(40, 170, 80));
    for (const auto &format : {"jpg", "bmp", "tiff", "webp", "ico"}) {
      const auto path = temp.path() + "/image." + format;
      QVERIFY(image.save(path));
      QString error;
      QVERIFY2(!load(provider, path, &error).isNull(),
               qPrintable(QString(format) + ": " + error));
    }
    QFile gif(temp.path() + "/first-frame.gif");
    QVERIFY(gif.open(QIODevice::WriteOnly));
    gif.write(QByteArray::fromBase64(
        "R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7"));
    gif.close();
    QVERIFY(!load(provider, gif.fileName()).isNull());
#ifdef FORMAVIEW_IMAGE_UI
    using namespace formaview;
    using namespace formaview::imaging;
    QFile bmp(temp.path() + "/image.bmp");
    QVERIFY(bmp.open(QIODevice::ReadOnly));
    QFile dib(temp.path() + "/image.dib");
    QVERIFY(dib.open(QIODevice::WriteOnly));
    dib.write(bmp.readAll().mid(14));
    dib.close();
    bmp.close();
    QVERIFY(!load(provider, dib.fileName()).isNull());
    const auto raster = ImageDocument::open(
        std::filesystem::path(bmp.fileName().toStdWString()));
    const auto tgaBytes =
        encode_professional(raster.decode(0), {ProfessionalTarget::Tga, false,
                                               false, 0, AlphaMode::Absent});
    QFile tga(temp.path() + "/image.tga");
    QVERIFY(tga.open(QIODevice::WriteOnly));
    tga.write(reinterpret_cast<const char *>(tgaBytes.data()), tgaBytes.size());
    tga.close();
    QVERIFY(!load(provider, tga.fileName()).isNull());
    ImageSubresource source;
    source.data_window = source.display_window = {0, 0, 96, 64};
    for (const char *name : {"R", "G", "B"}) {
      ImageChannel c;
      c.name = name;
      c.samples.scalar = ScalarType::Float32;
      c.samples.element_count = 96 * 64;
      c.samples.bytes.resize(96 * 64 * sizeof(float));
      const float value = name[0] == 'G' ? 0.7f : 0.2f;
      for (std::size_t p = 0; p < 96 * 64; ++p)
        std::memcpy(c.samples.bytes.data() + p * sizeof(float), &value,
                    sizeof(float));
      source.channels.push_back(std::move(c));
    }
    for (const auto target : {ProfessionalTarget::Dds, ProfessionalTarget::Exr,
                              ProfessionalTarget::Hdr}) {
      const auto path =
          temp.path() +
          QString("/professional-%1.bin").arg(static_cast<int>(target));
      const auto bytes = encode_professional(
          source, {target, true, false, 0, AlphaMode::Absent});
      QFile file(path);
      QVERIFY(file.open(QIODevice::WriteOnly));
      QCOMPARE(file.write(reinterpret_cast<const char *>(bytes.data()),
                          bytes.size()),
               static_cast<qint64>(bytes.size()));
      file.close();
      QString error;
      const auto thumbnail = load(provider, path, &error);
      QVERIFY2(!thumbnail.isNull(), qPrintable(error));
      QVERIFY(thumbnail.pixelColor(50, 50).green() >
              thumbnail.pixelColor(50, 50).red());
    }
#endif
  }
};
int main(int argc, char **argv) {
  QImageReader::setAllocationLimit(32);
  QGuiApplication app(argc, argv);
  ThumbnailTests tests;
  return QTest::qExec(&tests, argc, argv);
}
#include "thumbnail_tests.moc"
