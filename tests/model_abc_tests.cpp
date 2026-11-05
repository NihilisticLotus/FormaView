// ABC geometry-cache preview through ModelSession without a window:
// on-demand reads, bounded cache, random access equal to direct reads,
// stale-result isolation across seeks and file switches, cancellation,
// unsupported variants and non-blocking playback.
// Usage: formaview_model_abc_tests OUTPUT_DIR [REAL_SAMPLE.abc ...]
#include "model_session.hpp"
#include "formaview/geometry_cache.hpp"
#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <set>

namespace {
namespace A = Alembic::Abc;
namespace G = Alembic::AbcGeom;
namespace O = Alembic::AbcCoreOgawa;
using namespace formaview;
void require(bool ok, const std::string &text) {
  if (!ok)
    throw std::runtime_error(text);
}
double maxGapMs = 0;
void waitFor(const std::function<bool()> &done, const char *what,
             int timeout = 60000) {
  QElapsedTimer deadline, gap;
  deadline.start();
  gap.start();
  while (!done()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    maxGapMs = std::max(maxGapMs, static_cast<double>(gap.nsecsElapsed()) / 1e6);
    gap.restart();
    if (deadline.elapsed() > timeout)
      throw std::runtime_error(std::string("Timeout: ") + what);
    QThread::msleep(1);
  }
}
QVariantMap info(const ModelSession &s) { return s.cacheInfo(); }
bool settled(const ModelSession &s, int sample) {
  const auto i = info(s);
  return !s.busy() && !s.preparing() && i.value("sample").toInt() == sample &&
         i.value("requested").toInt() == sample && i.value("framed").toBool() &&
         !i.value("loading").toBool() && s.frame();
}
// The presented frame was built from the presented sample, not a stale one.
void requireFrameMatches(const ModelSession &s, const char *label) {
  const auto i = s.info();
  require(i.value("visibleTriangles").toULongLong() ==
                  s.cacheInfo().value("triangles").toULongLong() &&
              i.value("visiblePoints").toULongLong() ==
                  s.cacheInfo().value("points").toULongLong(),
          std::string(label) + ": frame statistics do not match the sample");
}
// Fixed topology: 12 samples of a moving, bending quad pair with
// face-varying UVs and normals that change per sample.
void writeFixed(const std::filesystem::path &path, int samples = 12,
                int badSample = -1) {
  A::OArchive out(O::WriteArchive(), path.string());
  const auto ts = out.addTimeSampling(A::TimeSampling(1.0 / 24, 1.0 / 24));
  G::OXform xform(out.getTop(), "root", ts);
  G::OPolyMesh mesh(xform, "sheet", ts);
  for (int i = 0; i < samples; ++i) {
    G::XformSample x;
    x.setTranslation(A::V3d(0, 0.5 * i, 0));
    xform.getSchema().set(x);
    const float f = static_cast<float>(i);
    std::vector<A::V3f> p{{0, 0, 0}, {1, 0, f * .1F}, {1, 1, 0}, {0, 1, 0},
                          {2, 0, -f * .1F}, {2, 1, 0}};
    std::vector<std::int32_t> idx{0, 3, 2, 1, 1, 2, 5, 4}, counts{4, 4};
    std::vector<A::V2f> uv;
    for (std::size_t c = 0; c < idx.size(); ++c)
      uv.push_back({f + static_cast<float>(c) * .01F, .5F});
    std::vector<A::N3f> n(idx.size(), {0, f * .05F, 1});
    if (i == badSample)
      idx.back() = 99; // Corner references a point that does not exist.
    mesh.getSchema().set(G::OPolyMeshSchema::Sample(
        A::P3fArraySample(p), A::Int32ArraySample(idx),
        A::Int32ArraySample(counts),
        G::OV2fGeomParam::Sample(A::V2fArraySample(uv), G::kFacevaryingScope),
        G::ON3fGeomParam::Sample(A::N3fArraySample(n), G::kFacevaryingScope)));
  }
}
// Varying topology: point/face counts change every sample; vertex UVs.
void writeVarying(const std::filesystem::path &path, bool emptyFirst = false) {
  A::OArchive out(O::WriteArchive(), path.string());
  const auto ts = out.addTimeSampling(A::TimeSampling(1.0 / 30, 0));
  G::OPolyMesh mesh(out.getTop(), "fluid", ts);
  for (int i = 0; i < 8; ++i) {
    std::vector<A::V3f> p;
    std::vector<std::int32_t> idx, counts;
    std::vector<A::V2f> uv;
    // Reserved storage keeps empty samples non-null: Alembic writes them as
    // real zero-length arrays instead of "unchanged" samples.
    p.reserve(1), idx.reserve(1), counts.reserve(1), uv.reserve(1);
    const int strips = emptyFirst && i == 0 ? 0 : 1 + i;
    for (int s = 0; strips && s <= strips; ++s) {
      p.push_back({static_cast<float>(s), 0, 0});
      p.push_back({static_cast<float>(s), 1, 0});
      uv.push_back({static_cast<float>(s) / strips, 0});
      uv.push_back({static_cast<float>(s) / strips, 1});
    }
    for (int s = 0; s < strips; ++s) {
      idx.insert(idx.end(), {2 * s, 2 * s + 1, 2 * s + 3, 2 * s + 2});
      counts.push_back(4);
    }
    mesh.getSchema().set(G::OPolyMeshSchema::Sample(
        A::P3fArraySample(p.data(), p.size()),
        A::Int32ArraySample(idx.data(), idx.size()),
        A::Int32ArraySample(counts.data(), counts.size()),
        G::OV2fGeomParam::Sample(A::V2fArraySample(uv.data(), uv.size()),
                                 G::kVertexScope)));
  }
}
// One static mesh beside a fixed-topology animated mesh.
void writeTwoMeshes(const std::filesystem::path &path) {
  A::OArchive out(O::WriteArchive(), path.string());
  const auto ts = out.addTimeSampling(A::TimeSampling(1.0 / 24, 0));
  G::OPolyMesh still(out.getTop(), "still");
  {
    std::vector<A::V3f> p{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    std::vector<std::int32_t> idx{0, 1, 2}, counts{3};
    still.getSchema().set(G::OPolyMeshSchema::Sample(
        A::P3fArraySample(p), A::Int32ArraySample(idx), A::Int32ArraySample(counts)));
  }
  G::OPolyMesh wave(out.getTop(), "wave", ts);
  for (int i = 0; i < 6; ++i) {
    std::vector<A::V3f> p{{0, 0, 0}, {1, 0, static_cast<float>(i)}, {1, 1, 0}, {0, 1, 0}};
    std::vector<std::int32_t> idx{0, 3, 2, 1}, counts{4};
    std::vector<A::V2f> uv{{0, 0}, {0, 1}, {1, 1}, {static_cast<float>(i), 0}};
    wave.getSchema().set(G::OPolyMeshSchema::Sample(
        A::P3fArraySample(p), A::Int32ArraySample(idx), A::Int32ArraySample(counts),
        G::OV2fGeomParam::Sample(A::V2fArraySample(uv), G::kFacevaryingScope)));
  }
}
void writeCurvesOnly(const std::filesystem::path &path, bool withMesh) {
  A::OArchive out(O::WriteArchive(), path.string());
  G::OCurves curves(out.getTop(), "hair");
  std::vector<A::V3f> cp{{0, 0, 0}, {0, 1, 0}};
  std::vector<std::int32_t> cv{2};
  curves.getSchema().set(
      G::OCurvesSchema::Sample(A::P3fArraySample(cp), A::Int32ArraySample(cv)));
  if (withMesh) {
    G::OPolyMesh mesh(out.getTop(), "body");
    std::vector<A::V3f> p{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    std::vector<std::int32_t> idx{0, 1, 2}, counts{3};
    mesh.getSchema().set(G::OPolyMeshSchema::Sample(
        A::P3fArraySample(p), A::Int32ArraySample(idx),
        A::Int32ArraySample(counts)));
  }
}
// Compares a presented sample document with a direct archive read.
void compareDirect(const ModelDocument &d, const std::filesystem::path &path,
                   std::size_t index, const char *label) {
  auto archive = cache::Archive::open(path);
  const auto seconds = archive.times().at(index);
  require(d.cacheSample == index, std::string(label) + ": wrong sample");
  require(d.meshes.size() == archive.meshes().size(),
          std::string(label) + ": mesh count");
  std::uint64_t faces = 0, corners = 0, points = 0;
  for (std::size_t mi = 0; mi < d.meshes.size(); ++mi) {
    const auto s = archive.read(mi, seconds);
    const auto &m = d.meshes[mi];
    require(d.cacheMeshSamples.at(mi) == s.index,
            std::string(label) + ": per-mesh sample index");
    require(m.raw.size() == s.positions.size(),
            std::string(label) + ": point count");
    for (std::size_t p = 0; p < s.positions.size(); ++p)
      require(m.raw[p].x == s.positions[p].x && m.raw[p].y == s.positions[p].y &&
                  m.raw[p].z == s.positions[p].z,
              std::string(label) + ": position differs");
    require(m.corners.size() == s.corner_points.size(),
            std::string(label) + ": corner count");
    for (std::size_t c = 0; c < s.corner_points.size(); ++c)
      require(m.corners[c] == static_cast<std::uint32_t>(s.corner_points[c]),
              std::string(label) + ": corner differs");
    require(m.uvs.empty() == s.uv.empty(), std::string(label) + ": UV presence");
    if (!s.uv.empty()) {
      std::size_t c = 0;
      for (std::size_t f = 0; f < s.face_sizes.size(); ++f)
        for (int k = 0; k < s.face_sizes[f]; ++k, ++c) {
          const auto point = static_cast<std::size_t>(s.corner_points[c]);
          const auto at = s.uv_scope == 4 ? c : s.uv_scope == 1 ? f
                          : s.uv_scope == 0 ? 0 : point;
          require(std::abs(m.uvs[0][c].x() - s.uv[at].x) < 1e-12 &&
                      std::abs(m.uvs[0][c].y() - s.uv[at].y) < 1e-12,
                  std::string(label) + ": UV differs");
        }
    }
    if (!s.normals.empty() && s.normal_scope == 4) {
      // Normals are presented through the sample's world normal matrix.
      QMatrix4x4 world;
      for (int r = 0; r < 4; ++r)
        for (int col = 0; col < 4; ++col)
          world(r, col) = static_cast<float>(s.world[static_cast<std::size_t>(r * 4 + col)]);
      const auto normalMatrix = world.normalMatrix();
      for (std::size_t c = 0; c < s.normals.size(); ++c) {
        QVector3D n;
        for (int r = 0; r < 3; ++r)
          n[r] = normalMatrix(r, 0) * static_cast<float>(s.normals[c].x) +
                 normalMatrix(r, 1) * static_cast<float>(s.normals[c].y) +
                 normalMatrix(r, 2) * static_cast<float>(s.normals[c].z);
        n = d.displayBasis.mapVector(n.normalized());
        require(n.isNull() || (m.normals.at(c) - n).length() < 1e-4,
                std::string(label) + ": normal differs");
      }
    }
    faces += s.face_sizes.size();
    corners += s.corner_points.size();
    points += s.positions.size();
  }
  require(d.cacheFaces == faces && d.cacheCorners == corners && d.points == points,
          std::string(label) + ": statistics");
}
} // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    require(argc >= 2, "Usage: formaview_model_abc_tests OUTPUT_DIR [REAL.abc ...]");
    const std::filesystem::path dir(app.arguments()[1].toStdWString());
    std::filesystem::create_directories(dir);
    const auto fixed = dir / "fixed.abc", varying = dir / "varying.abc",
               badSample = dir / "bad-sample.abc", curves = dir / "curves-only.abc",
               mixed = dir / "mixed.abc", hdf5 = dir / "hdf5.abc",
               staticOne = dir / "static.abc";
    writeFixed(fixed);
    writeVarying(varying);
    const auto twoMeshes = dir / "two-meshes.abc";
    writeTwoMeshes(twoMeshes);
    const auto emptyFirst = dir / "empty-first.abc";
    writeVarying(emptyFirst, true);
    writeFixed(badSample, 6, 3);
    writeFixed(staticOne, 1);
    writeCurvesOnly(curves, false);
    writeCurvesOnly(mixed, true);
    {
      std::ofstream out(hdf5, std::ios::binary);
      out.write("\x89HDF\r\n\x1a\n", 8);
      out << std::string(512, '\0');
    }
    const auto qpath = [](const std::filesystem::path &p) {
      return QString::fromStdWString(p.wstring());
    };
    QJsonObject evidence;

    // 1. Open: first sample, metadata, no skeleton/animation entry points.
    std::cerr << "stage 1" << std::endl;
    ModelSession session;
    session.open(qpath(fixed));
    waitFor([&] { return settled(session, 0); }, "fixed open");
    {
      const auto i = info(session);
      require(session.geometryCache() && !session.animated() &&
                  session.bones().isEmpty() && session.bindings().isEmpty() &&
                  !session.canAttachExternalAnimation() &&
                  !session.hasComparableSkeleton(),
              "ABC must not expose skeleton or animation-drive entry points");
      require(i.value("samples").toInt() == 12, "fixed sample count");
      const auto mesh = i.value("meshes").toList().value(0).toMap();
      require(mesh.value("path").toString() == "/root/sheet" &&
                  mesh.value("topology").toString() == "fixed" &&
                  mesh.value("samples").toInt() == 12 &&
                  mesh.value("uv").toBool() && mesh.value("normals").toBool(),
              "fixed mesh metadata");
      require(i.value("points").toULongLong() == 6 &&
                  i.value("faces").toULongLong() == 2 &&
                  i.value("corners").toULongLong() == 8 &&
                  i.value("triangles").toULongLong() == 4,
              "fixed statistics");
      require(i.value("reads").toULongLong() == 1, "open must read one sample");
      compareDirect(*session.documentSnapshot(), fixed, 0, "fixed open");
      evidence["open"] = QJsonObject::fromVariantMap(i);
    }

    // 2. Random access equals direct reads; reads only on cache misses.
    std::cerr << "stage 2" << std::endl;
    {
      std::mt19937 random(20261105);
      std::set<int> visited{0};
      QJsonArray jumps;
      for (int n = 0; n < 24; ++n) {
        const int target = static_cast<int>(random() % 12);
        const auto before = info(session).value("reads").toULongLong();
        const bool cached = visited.count(target) != 0;
        session.seekSample(target);
        waitFor([&] { return settled(session, target); }, "random seek");
        const auto after = info(session).value("reads").toULongLong();
        require(after - before == (cached ? 0 : 1),
                "on-demand read count (cached=" + std::to_string(cached) + ")");
        visited.insert(target);
        compareDirect(*session.documentSnapshot(), fixed,
                      static_cast<std::size_t>(target), "random seek");
        requireFrameMatches(session, "random seek");
        require(std::abs(session.time() -
                         info(session).value("time").toDouble()) < 1e-12,
                "seekSample time");
        jumps.append(QJsonObject{{"sample", target}, {"cached", cached},
                                 {"reads", static_cast<double>(after)}});
      }
      evidence["random_jumps"] = jumps;
      // Time-based seeks pick the nearest sample.
      session.seek(5.4 / 24);
      waitFor([&] { return settled(session, 4); }, "seek time");
      session.stepFrame(1);
      waitFor([&] { return settled(session, 5); }, "step forward");
      session.stepFrame(-3);
      waitFor([&] { return settled(session, 2); }, "step back");
    }

    // 3. Bounded cache.
    std::cerr << "stage 3" << std::endl;
    {
      session.setCacheLimits(1ULL << 30, 3);
      for (int target : {7, 8, 9, 10, 11, 0})
        session.seekSample(target), waitFor([&] { return settled(session, target); }, "bounded walk");
      const auto i = info(session);
      require(i.value("cachedSamples").toInt() <= 3 &&
                  i.value("cacheEvictions").toULongLong() > 0,
              "sample cache exceeded its count bound");
      session.setCacheLimits(1, 64);
      require(info(session).value("cachedSamples").toInt() == 1,
              "byte bound must keep only the presented sample");
      session.setCacheLimits(256ULL << 20, 64);
      evidence["bounded_cache"] = QJsonObject::fromVariantMap(i);
    }

    // 4. Stale isolation across seeks: an overtaken read never lands.
    std::cerr << "stage 4" << std::endl;
    {
      ModelSession s;
      s.open(qpath(fixed));
      waitFor([&] { return settled(s, 0); }, "isolation open");
      std::vector<int> landed;
      QObject::connect(&s, &ModelSession::animChanged, &s, [&] {
        const auto shown = s.cacheInfo().value("sample").toInt();
        if (landed.empty() || landed.back() != shown)
          landed.push_back(shown);
      });
      s.seekSample(5);
      s.seekSample(9);
      s.seekSample(3);
      s.seekSample(10);
      waitFor([&] { return settled(s, 10); }, "isolation final");
      for (const int shown : landed)
        require(shown == 0 || shown == 10,
                "superseded sample " + std::to_string(shown) + " was presented");
      compareDirect(*s.documentSnapshot(), fixed, 10, "isolation");
      evidence["isolation_landed"] = QJsonArray::fromVariantList(
          QVariantList(landed.begin(), landed.end()));
      evidence["isolation_discarded"] =
          static_cast<double>(s.cacheInfo().value("discarded").toULongLong());

      // 5. Cancel an in-flight read: the presented sample stays.
      s.seekSample(1);
      s.cancelSample();
      require(s.cacheInfo().value("requested").toInt() == 10 && s.available(),
              "cancel must keep the presented sample and the document");
      waitFor([&] { return settled(s, 10); }, "after cancel");
      QThread::msleep(50);
      QCoreApplication::processEvents();
      require(s.cacheInfo().value("sample").toInt() == 10,
              "cancelled read landed");

      // 6. File switch while a read is in flight: old results are dropped.
      s.seekSample(6);
      s.open(qpath(varying));
      waitFor([&] { return settled(s, 0) && s.path() == qpath(varying); },
              "switch open");
      QThread::msleep(50);
      QCoreApplication::processEvents();
      const auto snapshot = s.documentSnapshot();
      require(snapshot->geometryCache->path() == qpath(varying) &&
                  s.cacheInfo().value("samples").toInt() == 8,
              "old archive result leaked into the new file");
      for (const int target : {7, 2, 5}) {
        s.seekSample(target);
        waitFor([&] { return settled(s, target); }, "varying seek");
        compareDirect(*s.documentSnapshot(), varying,
                      static_cast<std::size_t>(target), "varying");
        requireFrameMatches(s, "varying");
        require(s.cacheInfo().value("points").toULongLong() ==
                    static_cast<qulonglong>(2 * (target + 2)),
                "varying topology statistics stale");
      }
      require(s.cacheInfo().value("meshes").toList().value(0).toMap()
                      .value("topology").toString() == "varying",
              "varying topology classification");
    }

    // 7. Playback on the GUI thread: non-blocking, reaches the end,
    std::cerr << "stage 7" << std::endl;
    // prefetches ahead, presents samples in order.
    {
      ModelSession s;
      s.open(qpath(fixed));
      waitFor([&] { return settled(s, 0); }, "playback open");
      s.setLoop(false);
      s.setSpeed(2);
      std::vector<int> order;
      QObject::connect(&s, &ModelSession::animChanged, &s, [&] {
        const auto shown = s.cacheInfo().value("sample").toInt();
        if (order.empty() || order.back() != shown)
          order.push_back(shown);
      });
      maxGapMs = 0;
      s.togglePlay();
      require(s.playing(), "playback did not start");
      waitFor([&] { return !s.playing() && settled(s, 11); }, "playback end");
      for (std::size_t k = 1; k < order.size(); ++k)
        require(order[k] > order[k - 1], "playback presented samples out of order");
      evidence["playback_order"] =
          QJsonArray::fromVariantList(QVariantList(order.begin(), order.end()));
      evidence["playback_prefetched"] =
          static_cast<double>(s.cacheInfo().value("prefetched").toULongLong());
      evidence["playback_max_event_gap_ms"] = maxGapMs;
      require(maxGapMs < 250, "GUI event loop blocked during playback");
      // Play again restarts from the first sample.
      s.togglePlay();
      require(s.playing() && s.cacheInfo().value("requested").toInt() == 0,
              "replay must restart at the first sample");
      s.togglePlay();
    }

    // 8. A malformed later sample fails alone; the archive stays usable.
    std::cerr << "stage 8" << std::endl;
    {
      ModelSession s;
      s.open(qpath(badSample));
      waitFor([&] { return settled(s, 0); }, "bad open");
      s.seekSample(3);
      waitFor([&] { return !s.cacheInfo().value("error").toString().isEmpty(); },
              "bad sample error");
      require(s.cacheInfo().value("sample").toInt() == 0 &&
                  s.cacheInfo().value("requested").toInt() == 0,
              "failed sample replaced the presented one");
      evidence["bad_sample_error"] = s.cacheInfo().value("error").toString();
      s.seekSample(4);
      waitFor([&] { return settled(s, 4); }, "after bad sample");
      require(s.cacheInfo().value("error").toString().isEmpty(),
              "error not cleared by a good sample");
    }

    // 9. Unsupported variants are explicit; a single static sample works.
    std::cerr << "stage 9" << std::endl;
    {
      ModelSession s;
      s.open(qpath(curves));
      waitFor([&] { return !s.busy(); }, "curves open");
      require(!s.available() &&
                  s.error().contains("no polygon meshes; unsupported schemas"),
              "curves-only archive: " + s.error().toStdString());
      evidence["curves_error"] = s.error();
      s.open(qpath(hdf5));
      waitFor([&] { return !s.busy(); }, "hdf5 open");
      require(!s.available() && s.error().contains("HDF5"),
              "HDF5 archive: " + s.error().toStdString());
      evidence["hdf5_error"] = s.error();
      s.open(qpath(mixed));
      waitFor([&] { return settled(s, 0); }, "mixed open");
      require(s.warnings().join("\n").contains("ABC object not previewed: /hair"),
              "skipped object not reported");
      evidence["mixed_warnings"] = QJsonArray::fromStringList(s.warnings());
      s.open(qpath(staticOne));
      waitFor([&] { return settled(s, 0); }, "static open");
      s.togglePlay();
      require(!s.playing() && s.cacheInfo().value("samples").toInt() == 1,
              "a single sample cannot play");
    }

    // 9a. Unchanged data is reused: a static mesh is copied from the
    // neighbouring sample and a fixed topology keeps its triangulation,
    // while every presented sample still equals a direct read.
    {
      ModelSession s;
      s.open(qpath(twoMeshes));
      waitFor([&] { return settled(s, 0); }, "two meshes open");
      for (int target = 1; target < 6; ++target) {
        s.seekSample(target);
        waitFor([&] { return settled(s, target); }, "two meshes seek");
        compareDirect(*s.documentSnapshot(), twoMeshes,
                      static_cast<std::size_t>(target), "reuse");
        requireFrameMatches(s, "reuse");
      }
      const auto i = s.cacheInfo();
      require(i.value("meshReuses").toULongLong() == 5 &&
                  i.value("topologyReuses").toULongLong() == 5 &&
                  i.value("meshDecodes").toULongLong() == 7,
              "reuse counters: reuses=" + i.value("meshReuses").toString().toStdString() +
                  " topology=" + i.value("topologyReuses").toString().toStdString() +
                  " decodes=" + i.value("meshDecodes").toString().toStdString());
      // Revisiting a recent sample reuses its cached frame.
      require(s.info().value("cachedFrameCount").toInt() > 1,
              "frames of recent samples must stay cached");
      evidence["reuse"] = QJsonObject{{"meshReuses", static_cast<double>(i.value("meshReuses").toULongLong())},
                                      {"topologyReuses", static_cast<double>(i.value("topologyReuses").toULongLong())},
                                      {"meshDecodes", static_cast<double>(i.value("meshDecodes").toULongLong())},
                                      {"cachedFrames", s.info().value("cachedFrameCount").toInt()}};
    }

    // 9b. A varying cache may start (or pass through) an empty sample.
    {
      ModelSession s;
      s.open(qpath(emptyFirst));
      waitFor([&] { return !s.busy(); }, "empty-first open");
      require(s.available(), "empty first sample open: " + s.error().toStdString());
      waitFor([&] { return !s.preparing(); }, "empty-first frame");
      require(s.error().isEmpty() && s.cacheInfo().value("points").toULongLong() == 0 &&
                  s.cacheInfo().value("faces").toULongLong() == 0,
              "empty first sample: " + s.error().toStdString() + " " +
                  s.frameError().toStdString());
      evidence["empty_first_frame_error"] = s.frameError();
      s.seekSample(3);
      waitFor([&] { return settled(s, 3); }, "after empty sample");
      require(s.frameError().isEmpty() &&
                  s.cacheInfo().value("faces").toULongLong() == 4,
              "sample after an empty one");
      s.seekSample(0);
      waitFor([&] { return !s.preparing() && s.cacheInfo().value("sample").toInt() == 0; },
              "back to empty sample");
    }

    // 10. Real samples (read only): random access against direct reads.
    std::cerr << "stage 10" << std::endl;
    QJsonArray real;
    for (int a = 2; a < app.arguments().size(); ++a) {
      const auto path = app.arguments()[a];
      ModelSession s;
      QElapsedTimer opened;
      opened.start();
      s.open(path);
      waitFor([&] { return (settled(s, 0)) || (!s.busy() && !s.error().isEmpty()); },
              "real open", 300000);
      require(s.error().isEmpty(), "real sample: " + s.error().toStdString());
      const auto openMs = opened.elapsed();
      const auto count = s.cacheInfo().value("samples").toInt();
      std::mt19937 random(7);
      QJsonArray seeks;
      std::set<int> sampled;
      for (int n = 0; n < std::min(count, 6); ++n) {
        const int target = n == 0 ? count - 1 : static_cast<int>(random() % count);
        QElapsedTimer t;
        t.start();
        s.seekSample(target);
        waitFor([&] { return settled(s, target); }, "real seek", 300000);
        const auto ms = t.elapsed();
        compareDirect(*s.documentSnapshot(), std::filesystem::path(path.toStdWString()),
                      static_cast<std::size_t>(target), "real");
        requireFrameMatches(s, "real");
        const auto i = s.cacheInfo();
        seeks.append(QJsonObject{{"sample", target}, {"ms", static_cast<double>(ms)},
                                 {"points", static_cast<double>(i.value("points").toULongLong())},
                                 {"faces", static_cast<double>(i.value("faces").toULongLong())},
                                 {"corners", static_cast<double>(i.value("corners").toULongLong())}});
        sampled.insert(target);
      }
      auto i = s.cacheInfo();
      i.remove("meshes");
      real.append(QJsonObject{{"path", path}, {"open_ms", static_cast<double>(openMs)},
                              {"samples", count},
                              {"meshes", QJsonArray::fromVariantList(s.cacheInfo().value("meshes").toList())},
                              {"final", QJsonObject::fromVariantMap(i)},
                              {"seeks", seeks}});
    }
    evidence["real_samples"] = real;
    QFile out(QString::fromStdWString((dir / "model_abc_tests.json").wstring()));
    require(out.open(QIODevice::WriteOnly), "evidence write");
    out.write(QJsonDocument(evidence).toJson());
    std::cout << "PASS model ABC session (" << real.size() << " real samples)\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
