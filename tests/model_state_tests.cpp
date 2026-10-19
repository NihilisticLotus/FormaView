#include "model_session.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <iostream>

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
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
      tree.replace(nullptr, 0, 0);
      test(tree.rowCount() == 0 && tree.details("leaf", 2).isEmpty(),
           "unloading clears skeleton indices and details");
    }
    {
      bool unsupported = false;
      try {
        readModelDocument(
            QString::fromUtf8("B:/Model/克劳德/An/Battle00/B_AAttack01_0.fbx"),
            {});
      } catch (const formaview::skeletal::Failure &failure) {
        unsupported =
            QString::fromUtf8(failure.what()).startsWith("ANIMATION_ONLY_FBX:");
      }
      test(unsupported, "actual animation-only FBX reports M07 capability "
                        "boundary instead of corrupt mesh");
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
