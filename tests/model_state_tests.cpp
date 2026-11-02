#include "model_session.hpp"
#include "formaview/fbx.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaProperty>
#include <QThread>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <iostream>

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  if (app.arguments().size() == 3 && app.arguments()[1] == "--morph-range") {
    try {
      auto document = readModelDocument(app.arguments()[2], {});
      if (!document->gpuSkinDomain() || !document->morphWeightsAt || document->meshes.empty())
        throw std::runtime_error("Morph regression needs an animated mesh fixture");
      auto &targets = document->meshes.front().morphTargets;
      targets.resize(65535);
      ModelRenderState state;
      state.bindings.assign(document->meshes.size(), -1);
      state.uvs.assign(document->meshes.size(), 0);
      const auto boundary = buildModelGpuGeometry(*document, state, 1, {});
      const auto &ranges = boundary->frames.model->skin->morphRanges;
      if (ranges.empty() || (ranges.front()[1] & 65535u) != 65535u)
        throw std::runtime_error("Representable morph count was not retained");
      targets.resize(65536);
      bool rejected = false;
      try { (void)buildModelGpuGeometry(*document, state, 2, {}); }
      catch (const std::runtime_error &error) {
        if (std::string(error.what()).find("GPU morph packed range") == std::string::npos) throw;
        rejected = true;
      }
      if (!rejected) throw std::runtime_error("65536 morph targets silently wrapped the packed range");
      if (!buildModelFrames(*document, state, 3, {}).model)
        throw std::runtime_error("CPU frame unavailable after GPU range rejection");
      std::cout << "PASS morph 65535 boundary, 65536 rejection and CPU frame\n";
      return 0;
    } catch (const std::exception &error) {
      std::cerr << error.what() << '\n'; return 1;
    }
  }
  if (app.arguments().size() == 4 && app.arguments()[1] == "--frame-notify") {
    try {
      ModelSession session;
      const auto waitFor = [&](auto done) {
        QElapsedTimer deadline; deadline.start();
        while (!done() && deadline.elapsed() < 30000) {
          QCoreApplication::processEvents(); QThread::msleep(1);
        }
        if (!done()) throw std::runtime_error("Frame notification fixture timeout");
      };
      if (app.arguments()[3] == "gpu") session.enableGpuSkinning(true);
      session.open(app.arguments()[2]);
      waitFor([&] { return session.inspectable() && session.frame() && session.animated(); });
      session.seek(.1);
      waitFor([&] { return session.renderedStateCurrent(); });
      session.renderingFailed("notification recovery probe", session.frame());
      if (session.frameError().isEmpty()) throw std::runtime_error("Error probe did not reach the published frame");
      int clearedNotifications = 0;
      QObject::connect(&session, &ModelSession::changed, &session, [&] {
        if (session.frameError().isEmpty()) ++clearedNotifications;
      });
      session.seek(.2);
      waitFor([&] { return session.frameError().isEmpty(); });
      if (!clearedNotifications) throw std::runtime_error("frameError cleared without its changed notification");
      std::cout << "PASS frameError recovery notification " << app.arguments()[3].toStdString() << '\n';
      return 0;
    } catch (const std::exception &error) {
      std::cerr << error.what() << '\n'; return 1;
    }
  }
  if (app.arguments().size() == 2 && app.arguments()[1] == "--uv-budget") {
    try {
      ModelDocument document;
      ModelMesh mesh;
      mesh.points = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
      mesh.corners = {0, 1, 2};
      mesh.normals.assign(3, QVector3D(0, 0, 1));
      mesh.colours.assign(3, QVector3D(1, 1, 1));
      mesh.cornerMaterial.assign(3, 0);
      mesh.materials = {"material"};
      mesh.textures.resize(1);
      mesh.uvs = {{{0, 0}, {1, 0}, {0, 1}}};
      mesh.invalidUVs = {false};
      mesh.triangles.resize(2300001);
      for (std::size_t i = 0; i < mesh.triangles.size(); ++i)
        mesh.triangles[i] = static_cast<std::uint32_t>(i % 3);
      document.meshes.push_back(std::move(mesh));
      ModelRenderState state;
      state.bindings = {-1}; state.uvs = {0};
      {
        const auto frames = buildModelFrames(document, state, 1, {});
        if (!frames.model || frames.model->vertices.size() != 2300001)
          throw std::runtime_error("Large ordinary frame must remain supported");
      }
      state.mode = 1;
      bool rejected = false;
      try { (void)buildModelFrames(document, state, 2, {}); }
      catch (const formaview::skeletal::Failure &failure) {
        if (failure.error.code != formaview::ErrorCode::ResourceLimit) throw;
        rejected = true;
      }
      if (!rejected) throw std::runtime_error("Oversized UV frame did not report ResourceLimit");
      document.meshes[0].triangles.resize(3);
      if (!buildModelFrames(document, state, 3, {}).uv)
        throw std::runtime_error("Frame builder did not recover after budget rejection");
      std::cout << "PASS UV ResourceLimit, ordinary frame and subsequent small frame\n";
      return 0;
    } catch (const std::exception &error) {
      std::cerr << error.what() << '\n'; return 1;
    }
  }
  if (app.arguments().size() != 4)
    return 64;
  const QDir fixtures(app.arguments()[1]), ufbx(app.arguments()[2]),
      blender(app.arguments()[3]);
  int assertions = 0;
  const auto test = [&](bool value, const char *message) {
    ++assertions;
    if (!value)
      throw std::runtime_error(message);
  };
  ModelSession session;
  const auto wait = [&] {
    QElapsedTimer deadline;
    deadline.start();
    while ((session.busy() || session.preparing() || session.textureBusy()) &&
           deadline.elapsed() < 30000) {
      QCoreApplication::processEvents();
      QThread::msleep(1);
    }
    test(session.inspectable(),
         qPrintable(session.error() + session.frameError()));
  };
  try {
    {
      ModelDocument document;
      const auto node = [&](const QString &id, const QString &parent,
                            const QString &name) {
        const QVariantMap value{
            {"id", id}, {"parent", parent}, {"name", name}, {"bone", true}};
        document.nodes.append(value);
        document.nodeById.insert(id, value);
      };
      node("root", "", "Root");
      node("a", "root", "Joint");
      node("b", "root", "Joint");
      node("leaf", "a", "Hand");
      ModelMesh mesh;
      ModelSkin skin;
      skin.bones = {"a", "b", "leaf"};
      skin.palette = {QVariantMap{{"nodeId", "a"}},
                      QVariantMap{{"nodeId", "b"}},
                      QVariantMap{{"nodeId", "leaf"}}};
      skin.weights = {{{2, .5}, {2, .25}}, {{2, 1}}, {{0, 0}}};
      mesh.skins.push_back(skin);
      document.meshes.push_back(mesh);
      ModelSkeleton tree;
      tree.replace(&document, 0, 0);
      auto hand = tree.indexForNode("leaf", 2);
      test(hand.isValid() &&
               hand.parent().data(ModelSkeleton::NodeKey) == "a" &&
               hand.parent().parent().data(ModelSkeleton::NodeKey) == "root",
           "skeleton preserves real ancestors");
      test(tree.details("leaf", 2).value("points").toULongLong() == 2,
           "affected points count distinct source points, not duplicate "
           "influences");
      test(tree.indexForNode("a").data().toString() !=
               tree.indexForNode("b").data().toString(),
           "duplicate sibling names retain concise disambiguation");
      tree.setWeightedOnly(true);
      test(tree.indexForNode("a").isValid() &&
               !tree.indexForNode("b").isValid() &&
               tree.indexForNode("root").isValid(),
           "weighted filter retains zero-weight ancestors only when needed");
      tree.setSearchText("hand");
      test(tree.indexForNode("leaf").isValid() &&
               tree.indexForNode("root").isValid(),
           "case-insensitive search preserves full parent chain");
      tree.setSearchText("absent");
      test(tree.rowCount() == 0, "search has genuine empty result");
      tree.setSearchText("");
      tree.setWeightedOnly(false);
      test(!(tree.flags(tree.indexForNode("leaf")) & Qt::ItemIsEditable),
           "skeleton is read-only");
      auto &duplicatePalette=document.meshes[0].skins[0];
      duplicatePalette.bones.append("a second palette entry");
      duplicatePalette.palette.append(QVariantMap{{"nodeId","a"}});
      duplicatePalette.weights[0].push_back({0,.2});
      duplicatePalette.weights[0].push_back({3,.3});
      tree.replace(&document,0,0);
      test(tree.details("a",-1).value("points").toULongLong()==1 &&
               tree.details("a",0).value("points").toULongLong()==1 &&
               tree.details("a",3).value("points").toULongLong()==1,
           "shared node palette entries count source points once while retaining per-entry counts");
      tree.replace(nullptr, 0, 0);
      test(tree.rowCount() == 0 && tree.details("leaf", 2).isEmpty(),
           "unloading clears skeleton indices and details");
    }
    {
      // M07: the animation-only FBX boundary is replaced by the skeleton
      // preview document; the real source must load with clips and evaluate.
      session.open(QString::fromUtf8("B:/Model/克劳德/An/Battle00/B_AAttack01_0.fbx"));
      wait();
      test(session.animated() && session.meshes().isEmpty(),
           "animation-only session has no mesh capability");
      for (int i = QObject::staticMetaObject.propertyCount();
           i < session.metaObject()->propertyCount(); ++i) {
        const auto property = session.metaObject()->property(i);
        if (property.isReadable())
          test(property.read(&session).isValid(), property.name());
      }
      test(session.uvBackground().isEmpty(),
           "animation-only session has no UV background");
      test(session.frame() && session.frame()->hasDisplayBounds,
           "animation publishes camera-space display bounds");
      for (const auto &vertex : session.frame()->vertices)
        test(std::abs(vertex.x) < 2 && std::abs(vertex.y) < 2 &&
                 std::abs(vertex.z) < 2,
             "animation vertices use the model viewport normalized space");
      session.selectBinding(-1);
      session.removeTexture();
      session.classifyMesh("not-a-mesh", 0, false);
      session.restoreClassification();
      test(session.meshes().isEmpty() && session.classification().isEmpty(),
           "mesh-only commands do not create animation mesh state");
      const auto clipStart = session.clipInfo().value("start").toDouble();
      session.setLoop(false);
      session.setSpeed(1);
      QElapsedTimer clock;
      clock.start();
      session.togglePlay();
      while (clock.elapsed() < 120) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
      }
      if (session.playing())
        session.togglePlay();
      const auto wallSeconds = clock.nsecsElapsed() / 1e9;
      const auto playedSeconds = session.time() - clipStart;
      std::cout << "Animation clock: wall=" << wallSeconds
                << "s, played=" << playedSeconds << "s\n";
      test(playedSeconds > 0 && playedSeconds <= wallSeconds + .05,
           "playback timer converts elapsed milliseconds to real seconds");
      session.clear();
      std::shared_ptr<ModelDocument> document;
      QString failure;
      try {
        document = readModelDocument(
            QString::fromUtf8("B:/Model/克劳德/An/Battle00/B_AAttack01_0.fbx"),
            {});
      } catch (const formaview::skeletal::Failure &error) {
        failure = error.what();
      } catch (const std::exception &error) {
        failure = error.what();
      }
      const QByteArray message =
          failure.isEmpty()
              ? QByteArrayLiteral(
                    "actual animation-only FBX loads as M07 skeleton preview")
              : failure.toUtf8();
      test(bool(document) && document->skeletonOnly &&
               !document->animationClips.empty() && document->worldAt,
           message.constData());
      if (document && document->worldAt) {
        const auto source = formaview::skeletal::FbxDocument::read(
            std::filesystem::path(L"B:/Model/克劳德/An/Battle00/B_AAttack01_0.fbx"));
        const auto up = source.data().source.coordinates.up;
        QVector3D axis;
        switch (up) {
        case formaview::Axis::PositiveX: axis = {1, 0, 0}; break;
        case formaview::Axis::NegativeX: axis = {-1, 0, 0}; break;
        case formaview::Axis::PositiveZ: axis = {0, 0, 1}; break;
        case formaview::Axis::NegativeZ: axis = {0, 0, -1}; break;
        case formaview::Axis::NegativeY: axis = {0, -1, 0}; break;
        default: axis = {0, 1, 0}; break;
        }
        test((document->displayBasis.mapVector(axis) - QVector3D(0, 1, 0)).length() < 1e-5,
             "animation FBX maps declared source up to display Y exactly once");
        std::cout << "FBX source up enum: " << static_cast<int>(up) << '\n';
        const auto worlds = document->worldAt(
            0, document->animationClips.front().start);
        test(worlds == source.evaluate(0, source.data().clips.front().start).world,
             "display basis does not mutate FBX source world transforms");
        test(worlds.size() == document->boneParents.size() &&
                 !worlds.empty(),
             "animation-only FBX evaluates world matrices at clip start");
      }
    }
    {
      const auto psa = QString::fromUtf8("B:/Model/鸣潮/WutheringWaves_Unpack/Game/Aki/Character/Role/Female121/Jueyuan/CommonAnim/AirSkill01_Start.psa");
      const auto psk = QString::fromUtf8("B:/Model/鸣潮/WutheringWaves_Unpack/Game/Aki/Character/Role/Female121/Jueyuan/R2T1JueyuanMd10011/Model/R2T1JueyuanMd10011.psk");
      auto configPath = psa;
      configPath.chop(3);
      configPath += "config";
      const auto raw = readModelDocument(psa, {});
      const auto paired = readModelDocument(psa, {}, psk, configPath);
      test(raw->referencePath.isEmpty() && paired->referencePath == psk &&
               paired->companionPath == configPath,
           "PSA reference and companion are explicit, never automatically discovered");
      test(raw->boneParents[2] == 0 && paired->boneParents[2] == 1,
           "real PSA placeholder parents are replaced only by explicit PSK topology");
      test(raw->animationClips[0].name == "AirSkill01_Start",
           "PSA uses sequence name rather than group None");
      const auto asset = formaview::skeletal::read_actor(std::filesystem::path(psa.toStdWString()));
      const auto mesh = formaview::skeletal::read_actor(std::filesystem::path(psk.toStdWString()));
      const auto binding = formaview::skeletal::pair(asset, mesh, 1e-4,
          formaview::skeletal::PairingPolicy::ReferenceSkeleton);
      QFile configFile(configPath);
      test(configFile.open(QIODevice::ReadOnly), "explicit companion input exists");
      const auto bytes = configFile.readAll();
      const auto config = formaview::skeletal::read_psa_config(
          std::span<const std::byte>(reinterpret_cast<const std::byte *>(bytes.constData()),
                                     static_cast<std::size_t>(bytes.size())), asset);
      for (const double t : {0., .1, .3}) {
        const auto expected = formaview::skeletal::evaluate_paired(asset, binding, 0, t, &config);
        const auto actual = paired->worldAt(0, t);
        test(actual == expected.world, "host forwards PSK hierarchy and companion to paired source evaluator");
        const auto frame = buildAnimationFrame(*paired, 0, t, 1);
        test(frame->gridVertices == 84 && frame->guides.size() == 102,
             "animation frames carry grid and three source axes");
        const auto origin = -paired->center / paired->radius;
        const auto &vertex = frame->guides[84];
        test((QVector3D(vertex.x, vertex.y, vertex.z) - origin).length() < 1e-5,
             "animation guides remain at normalized source origin while seeking");
      }
      session.open(psa); wait();
      test(session.info().value("psa").toBool() &&
               session.info().value("referencePath").toString().isEmpty(),
           "unpaired PSA exposes provisional state");
      session.setAnimationReference(QUrl::fromLocalFile(psk).toString()); wait();
      session.setAnimationCompanion(QUrl::fromLocalFile(configPath).toString()); wait();
      test(session.info().value("referencePath").toString() == psk &&
               session.info().value("companionPath").toString() == configPath,
           "PSA UI accepts file URLs and publishes reference state after async load");
      session.setAnimationReference(""); wait();
      test(session.info().value("referencePath").toString().isEmpty() &&
               session.info().value("companionPath").toString().isEmpty(),
           "clearing PSA pairing also removes companion");
      // New session-memory contract: a companion without a reference is a
      // warned, unapplied state - never an open failure, and never a silent
      // substitution of reference-dependent channels.
      bool safe = false;
      try {
        const auto document = readModelDocument(psa, {}, {}, configPath);
        safe = document->referencePath.isEmpty() &&
               document->companionPath.isEmpty() &&
               document->warnings.join('|')
                   .contains("NOT applied", Qt::CaseInsensitive);
      } catch (const std::exception &) { safe = false; }
      test(safe, "companion without a reference warns and is never applied");
    }
    session.open(fixtures.filePath("external.gltf"));
    wait();
    test(session.sourceNode().value("id").toString() == "node/1" &&
             session.sourceNode().value("local").toList()[0].toDouble() == 2 &&
             session.sourceNode().value("world").toList()[3].toDouble() == 3,
         "source node metadata retains auxiliary parent transforms");
    const auto original = session.frame()->textures.at(0);
    const auto sourceDocument =
        readModelDocument(fixtures.filePath("external.gltf"), {});
    const auto origin = -sourceDocument->center / sourceDocument->radius;
    const auto &axisStart =
        session.frame()->guides.at(session.frame()->gridVertices);
    test(session.frame()->gridVertices == 84 &&
             session.frame()->guides.size() == 102 &&
             QVector3D(axisStart.x, axisStart.y, axisStart.z) == origin &&
             session.frame()->sourceTriangles == 1,
         "reference grid and source-origin axes are separate from source mesh "
         "geometry and counts");
    session.setMode(1);
    wait();
    test(session.frameFor(true)->uv && session.frameFor(true)->guides.empty() &&
             session.frameFor(true)->backgroundVertices == 0 &&
             !session.uvBackground().value("matching").toBool(),
         "inspected UV never borrows a texture from a different declared UV "
         "set");
    session.selectUV(1);
    wait();
    test(session.uvBackground().value("shown").toBool() &&
             session.frameFor(true)->backgroundVertices == 6 &&
             session.frameFor(true)->textures.at(0) == original,
         "matching declared UV uses the actual immutable image behind the raw "
         "wireframe");
    test(session.frameFor(true)->hasDisplayBounds &&
             session.frameFor(true)->displayLow == QVector3D(-.5F, -.5F, 0) &&
             session.frameFor(true)->displayHigh == QVector3D(.5F, .5F, 0) &&
             session.uvBounds(true).second > session.uvBounds(false).second,
         "frame all includes the reference square while focus uses raw UV "
         "bounds");
    session.selectUV(0);
    session.setMode(0);
    wait();
    session.associateTexture(fixtures.filePath("colour.png"));
    wait();
    test(session.canClearTexture() &&
             session.frame()->textures.at(0) != original,
         "manual association uses separate immutable texture");
    session.removeTexture();
    wait();
    test(!session.canClearTexture() &&
             session.frame()->textures.at(0) == original,
         "clear restores exact source texture");
    session.setMode(1);
    wait();
    session.pick(QMatrix4x4{}, {-.6, -.6}, false);
    const auto picked = session.picked();
    session.pick(QMatrix4x4{}, {-.9, -1.1}, true);
    test(!picked.isEmpty() &&
             session.picked().value("point") == picked.value("point"),
         "3D and unwrapped UV resolve same source point");
    test(session.picked().value("meshId") == picked.value("meshId") &&
             session.picked().value("faceCorner") == picked.value("faceCorner"),
         "source instance and face corner are preserved across views");
    test(session.picked().value("u").toDouble() == 0 &&
             session.picked().value("v").toDouble() == 0,
         "source UV not folded or interpolated into point value");
    test(!session.scene()
              ->flags(session.treeSelection())
              .testFlag(Qt::ItemIsEditable),
         "source tree is not an asset editor");
    const auto retained = session.frameFor(false);
    session.setMode(2);
    test(session.preparing() && !session.inspectable(),
         "new frame transaction pauses exact reads immediately");
    session.cancel();
    wait();
    test(session.mode() == 1 && session.frameFor(false) == retained,
         "cancel restores previous actual frame and mode");
    for (int i = 0; i < 15; ++i)
      session.setMode(i % 4);
    session.setMode(0);
    wait();
    test(session.mode() == 0 && !session.frame()->lines &&
             !session.frame()->heat,
         "latest asynchronous mode wins");
    const auto warmFrame = session.frame();
    session.setMode(1);
    wait();
    session.setMode(0);
    wait();
    test(session.frame() == warmFrame,
         "warm view reuses immutable prepared frame");
    test(session.info().value("cachedFrameBytes").toULongLong() <=
                 96ULL * 1024 * 1024 &&
             session.info().value("cachedFrameCount").toInt() <= 4,
         "prepared frame cache obeys byte and entry limits");
    session.open(fixtures.filePath("two-materials.obj"));
    wait();
    test(session.materials().size() == 2,
         "original OBJ retains two face material ranges in one mesh");
    session.setMode(1);
    wait();
    test(session.info().value("visibleTriangles").toInt() == 2 &&
             session.frameFor(true)->sourceTriangles == 2,
         "UV reference guides do not count as source faces");
    const auto blue = session.materials().indexOf("Blue"),
               red = session.materials().indexOf("Red");
    test(blue >= 0 && red >= 0,
         "source material names identify the requested range");
    session.selectMaterial(blue);
    session.setMaterialOnly(true);
    wait();
    test(session.info().value("visibleTriangles").toInt() == 1 &&
             session.info().value("visiblePoints").toInt() == 3 &&
             session.frameFor(true)->sourceTriangles == 1,
         "3D and UV share exact material-filtered source counts");
    session.pick(QMatrix4x4{}, QPointF(-.5, -.3), false);
    test(session.picked().isEmpty(),
         "3D picking rejects hidden material triangles");
    session.pick(QMatrix4x4{}, QPointF(.5, -.3), false);
    test(!session.picked().isEmpty(),
         "3D picking retains visible material triangles");
    session.pick(QMatrix4x4{}, QPointF(-.5, -.5), true);
    test(session.picked().isEmpty(),
         "UV picking rejects hidden material triangles");
    session.pick(QMatrix4x4{}, QPointF(.5, -.5), true);
    test(!session.picked().isEmpty(),
         "UV picking retains visible material triangles");
    const auto blueFrame = session.frameFor(false);
    session.selectMaterial(red);
    session.cancel();
    wait();
    test(session.materialIndex() == blue && session.materialOnly() &&
             session.frameFor(false) == blueFrame,
         "cancel restores the actual material range together with both views");
    session.open(fixtures.filePath("nonfinite-lod.gltf"));
    wait();
    test(session.lodGroups().isEmpty(),
         "LOD name candidate never silently classifies geometry");
    session.selectMesh(0);
    session.classifyMesh("Explicit test session", 0, false);
    wait();
    session.selectMesh(1);
    session.classifyMesh("Explicit test session", 1, false);
    wait();
    session.setMode(1);
    wait();
    const auto beforeFailure = session.frameFor(false);
    session.requestLod(1);
    QElapsedTimer failureDeadline;
    failureDeadline.start();
    while (session.preparing() && failureDeadline.elapsed() < 10000) {
      QCoreApplication::processEvents();
      QThread::msleep(1);
    }
    test(!session.inspectable() && !session.frameError().isEmpty(),
         "invalid UV pauses exact inspection during LOD transition");
    for (int mode : {0, 2, 3, 1}) {
      session.setMode(mode);
      QCoreApplication::processEvents();
      test(!session.inspectable() && !session.frameError().isEmpty() &&
               session.frameFor(false) == beforeFailure,
           "failed LOD remains globally paused across inspection modes");
    }
    session.cancel();
    wait();
    test(session.lodLevels()[0].toMap().value("current").toBool() &&
             session.mode() == 1,
         "failed LOD cancellation restores last actual context");
    session.open(ufbx.filePath("maya_lod_group_7500_ascii.fbx"));
    wait();
    test(session.lodGroups().size() == 2 && session.lodLevels().size() == 3,
         "actual FBX contains two explicit three-level groups");
    const auto levels = session.lodLevels();
    test(levels[0].toMap().value("current").toBool(),
         "explicit group defaults to one actual level");
    const auto oldFrame = session.frame();
    session.requestLod(2);
    test(!session.inspectable() && session.preparing(),
         "LOD request is a global preparation transaction");
    session.setMode(1);
    session.setMode(2);
    session.setMode(3);
    wait();
    test(session.lodLevels()[2].toMap().value("current").toBool(),
         "LOD request persists across all inspection modes");
    test(session.frame() != oldFrame, "new actual level publishes a new frame");
    session.requestLod(0);
    session.cancel();
    wait();
    test(session.lodLevels()[2].toMap().value("current").toBool() &&
             session.lodLevels()[2].toMap().value("requested").toBool(),
         "cancel restores both actual and requested level");
    const auto ordinaryTriangles = session.frame()->vertices.size();
    const auto collisionMesh = session.meshIndex();
    session.classifyMesh("", 0, true);
    wait();
    test(session.frame()->vertices.size() < ordinaryTriangles,
         "hidden collision is removed from drawn geometry");
    session.setCollisionMode(2);
    wait();
    test(session.meshIndex() == collisionMesh &&
             !session.frame()->vertices.empty(),
         "collision-only excludes ordinary geometry");
    test(std::all_of(session.frame()->vertices.begin(),
                     session.frame()->vertices.end(),
                     [](const auto &v) {
                       return v.r == 1 && std::abs(v.g - .3F) < 1e-6F;
                     }),
         "collision geometry has explicit display tint");
    session.restoreClassification();
    session.setCollisionMode(0);
    wait();
    test(!session.classification().value("corrected").toBool(),
         "session classification restores source without writing");
    session.open(fixtures.filePath("eight-normalized.gltf"));
    wait();
    test(session.bindingInfo().value("paletteSize").toInt() == 8 &&
             !session.bindingInfo().value("inverseBindProvided").toBool() &&
             session.bindingInfo()
                     .value("inverseBind")
                     .toList()[15]
                     .toDouble() == 1,
         "complete binding palette and explicit identity default");
    session.selectBone(1);
    wait();
    test(session.bindingInfo().value("nodeId").toString() == "node/2" &&
             session.bindingInfo()
                     .value("defaultJointWorld")
                     .toList()[3]
                     .toDouble() == 2,
         "duplicate joint names expose exact palette node and default world "
         "matrix");
    {
      const auto embedded =
          readModelDocument(blender.filePath("cross-embedded.fbx"), {});
      bool decoded = false;
      for (const auto &mesh : embedded->meshes)
        for (qsizetype slot = 0; slot < mesh.texturePaths.size(); ++slot)
          decoded |=
              mesh.texturePaths[slot].startsWith("Embedded base colour") &&
              bool(mesh.textures[static_cast<std::size_t>(slot)]);
      if (!decoded) {
        QJsonArray details;
        for (const auto &mesh : embedded->meshes)
          details.append(QJsonObject{
              {"name", mesh.name},
              {"uv", QJsonArray::fromStringList(mesh.uvNames)},
              {"references", QJsonArray::fromStringList(mesh.references)},
              {"paths", QJsonArray::fromStringList(mesh.texturePaths)}});
        std::cerr << QJsonDocument(
                         QJsonObject{{"meshes", details},
                                     {"resources", QJsonArray::fromVariantList(
                                                       embedded->resources)}})
                         .toJson()
                         .constData();
      }
      test(decoded, "actual FBX embedded base-colour image decoded without "
                    "extracting or saving it");
    }
    {
      const auto doc = readModelDocument(
          ufbx.filePath("max_instanced_material_7700_ascii.fbx"), {});
      test(doc->meshes.size() == 3, "one geometry has three actual instances");
      test(doc->meshes[0].materials != doc->meshes[1].materials &&
               doc->meshes[1].materials != doc->meshes[2].materials,
           "instance-specific material override is retained");
    }
    QFile expected(blender.filePath("expected.json"));
    test(expected.open(QIODevice::ReadOnly),
         "Blender independent golden exists");
    const auto golden = QJsonDocument::fromJson(expected.readAll()).object();
    for (const auto &name : {"cross.gltf", "cross.glb", "cross.obj"}) {
      const auto doc = readModelDocument(blender.filePath(name), {});
      test(doc->meshes.size() == 2 &&
               doc->triangles ==
                   static_cast<std::uint64_t>(golden["triangles"].toInteger()),
           "Blender exported instances and triangle count");
      QVector3D low = doc->meshes[0].boundsLow,
                high = doc->meshes[0].boundsHigh;
      for (const auto &mesh : doc->meshes)
        for (int axis = 0; axis < 3; ++axis) {
          low[axis] = std::min(low[axis], mesh.boundsLow[axis]);
          high[axis] = std::max(high[axis], mesh.boundsHigh[axis]);
        }
      for (int axis = 0; axis < 3; ++axis)
        test(std::abs(low[axis] - golden["low"].toArray()[axis].toDouble()) <
                     2e-5 &&
                 std::abs(high[axis] -
                          golden["high"].toArray()[axis].toDouble()) < 2e-5,
             "Blender golden world bounds including auxiliary parent and "
             "instance scale");
      test(doc->meshes[0].uvs.size() == (QString(name).endsWith("obj") ? 1 : 2),
           "exported UV cardinality includes OBJ single-set boundary");
    }
    session.clear();
    QElapsedTimer settle;
    settle.start();
    while (settle.elapsed() < 200) {
      QCoreApplication::processEvents();
      QThread::msleep(1);
    }
    test(!session.available() && !session.frame() &&
             session.scene()->rowCount() == 0 && !session.preparing(),
         "close releases source scene and current frames without stale "
         "publication");
  } catch (const std::exception &e) {
    std::cerr << "FAILED after " << assertions << " assertions: " << e.what()
              << '\n';
    return 1;
  }
  std::cout << "PASSED " << assertions
            << " model state and independent export assertions\n";
}
