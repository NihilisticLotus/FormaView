#include "model_session.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <stdexcept>

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const auto args = app.arguments();
  if (args.size() < 3)
    return 64;
  ModelSession session;
  QJsonArray records;
  const auto wait = [&] {
    QElapsedTimer deadline;
    deadline.start();
    while ((session.busy() || session.preparing()) &&
           deadline.elapsed() < 30000) {
      QCoreApplication::processEvents();
      QThread::msleep(2);
    }
    if (!session.inspectable())
      throw std::runtime_error(
          (session.error() + session.frameError()).toStdString());
  };
  try {
    for (int i = 2; i < args.size(); ++i) {
      QElapsedTimer elapsed;
      elapsed.start();
      session.open(args[i]);
      wait();
      const auto ms = elapsed.elapsed();
      const auto info = session.info();
      // Record the actual UI hierarchy, including zero-weight ancestors. A
      // cluster-count-only check misses parser containers displayed as bones.
      QJsonArray skeletonTree;
      const auto treeDocument = readModelDocument(args[i], {});
      if (treeDocument->skeletonOnly && args[i].endsWith(".fbx", Qt::CaseInsensitive)) {
        const auto source = formaview::skeletal::FbxDocument::read(
            std::filesystem::path(args[i].toStdWString()));
        for (std::size_t c = 0; c < source.data().clips.size(); ++c) {
          const auto &clip = source.data().clips[c];
          for (const double fraction : {0.0, .137, .5, .731, 1.0}) {
            const auto time = clip.start + fraction * (clip.end - clip.start);
            const auto expected = source.evaluate(c, time);
            const auto actual = treeDocument->worldAt(c, time);
            std::size_t j = 0;
            for (std::size_t nodeIndex = 0; nodeIndex < source.data().nodes.size(); ++nodeIndex)
              if (!source.data().nodes[nodeIndex].implicit_scene_root) {
                if (actual.at(j++) != expected.world.at(nodeIndex))
                  throw std::runtime_error("Scene root removal changed animation worlds");
              }
            if (j != actual.size()) throw std::runtime_error("Animation remap cardinality");
          }
        }
      }
      ModelSkeleton tree;
      tree.replace(treeDocument.get(), 0, 0);
      const auto visit = [&](auto &&self, const QModelIndex &parent) -> void {
        for (int row = 0; row < tree.rowCount(parent); ++row) {
          const auto index = tree.index(row, 0, parent);
          skeletonTree.append(QJsonObject{
              {"id", index.data(ModelSkeleton::NodeKey).toString()},
              {"name", index.data().toString()},
              {"parent", parent.data(ModelSkeleton::NodeKey).toString()}});
          self(self, index);
        }
      };
      visit(visit, QModelIndex{});
      QJsonArray sourceMeshes;
      QJsonArray sourceNodes;
      if (qEnvironmentVariableIsSet("FORMAVIEW_PROBE_SOURCE_DETAILS")) {
        const auto document = readModelDocument(args[i], {});
        sourceNodes = QJsonArray::fromVariantList(document->nodes);
        for (const auto &mesh : document->meshes) {
          const auto vector = [](const QVector3D &v) {
            return QJsonArray{v.x(), v.y(), v.z()};
          };
          QJsonArray uv, skins, points;
          for (std::size_t point = 0;
               point < std::min<std::size_t>(mesh.points.size(), 2048); ++point)
            points.append(vector(mesh.points[point]));
          for (const auto &bounds : mesh.uvBounds)
            uv.append(QJsonArray{bounds[0], bounds[1], bounds[2], bounds[3]});
          QJsonObject materialTriangles;
          for (std::size_t triangle = 0; triangle < mesh.triangles.size() / 3;
               ++triangle) {
            const auto corner = mesh.triangles[triangle * 3];
            const auto slot = mesh.cornerMaterial.empty()
                                  ? 0
                                  : mesh.cornerMaterial.at(corner);
            const auto material =
                mesh.materials.value(static_cast<qsizetype>(slot));
            materialTriangles[material] =
                materialTriangles.value(material).toInt() + 1;
          }
          for (const auto &skin : mesh.skins) {
            QJsonArray weights;
            for (std::size_t point = 0;
                 point < std::min<std::size_t>(skin.weights.size(), 2048);
                 ++point) {
              QJsonArray influences;
              for (const auto &weight : skin.weights[point])
                influences.append(QJsonObject{{"bone", weight.bone},
                                              {"value", weight.value}});
              weights.append(influences);
            }
            skins.append(QJsonObject{
                {"name", skin.name},
                {"bones", QJsonArray::fromStringList(skin.bones)},
                {"palette", QJsonArray::fromVariantList(skin.palette)},
                {"raw_weights_first_2048", weights}});
          }
          sourceMeshes.append(QJsonObject{
              {"name", mesh.name},
              {"node", mesh.nodeId},
              {"points", static_cast<qint64>(mesh.points.size())},
              {"display_points_first_2048", points},
              {"triangles", static_cast<qint64>(mesh.triangles.size() / 3)},
              {"display_basis_low", vector(mesh.boundsLow)},
              {"display_basis_high", vector(mesh.boundsHigh)},
              {"uv_bounds", uv},
              {"material_triangles", materialTriangles},
              {"skins", skins}});
        }
      }
      for (int mesh = 0; mesh < session.meshes().size(); ++mesh) {
        session.selectMesh(mesh);
        session.setMode(0);
        wait();
        // Animated non-FBX sessions publish their first pose asynchronously
        // without setting preparing(). Wait for that payload before checking it.
        QElapsedTimer poseDeadline;
        poseDeadline.start();
        while ((!session.frame() || session.frame()->vertices.empty()) &&
               poseDeadline.elapsed() < 30000) {
          QCoreApplication::processEvents();
          QThread::msleep(2);
        }
        if (!session.frame() || session.frame()->vertices.empty())
          throw std::runtime_error("No model vertices");
        for (int uv = 0; uv < session.uvSets().size(); ++uv) {
          session.selectUV(uv);
          session.setMode(1);
          wait();
          if (session.frame()->vertices.empty())
            throw std::runtime_error("No UV vertices");
        }
        for (int skin = 0; skin < session.bindings().size(); ++skin) {
          session.selectBinding(skin);
          session.setMode(2);
          wait();
          if (session.frame()->vertices.empty())
            throw std::runtime_error("No weight vertices");
          if (!session.bones().isEmpty())
            session.selectBone(static_cast<int>(session.bones().size() - 1));
          wait();
        }
      }
      QJsonObject skeletonEvidence;
      if (!session.bones().isEmpty()) {
        int selected = 0;
        for (int joint = 0; joint < session.bones().size(); ++joint)
          if (session.bones()[joint].contains("L_Shoulder_a"))
            selected = joint;
        session.selectBone(selected);
        session.setMode(0);
        wait();
        QJsonArray lines;
        const auto frame = session.frame();
        for (std::size_t guide =
                 frame->guides.size() - frame->selectionVertices;
             guide < frame->guides.size(); ++guide) {
          const auto &v = frame->guides[guide];
          lines.append(QJsonArray{v.x, v.y, v.z});
        }
        skeletonEvidence = {
            {"selection",
             QJsonObject::fromVariantMap(session.skeletonSelection())},
            {"binding", QJsonObject::fromVariantMap(session.bindingInfo())},
            {"lines", lines}};
      }
      records.append(QJsonObject{{"source_nodes", sourceNodes},
                                 {"skeleton_tree", skeletonTree},
                                 {"source_meshes", sourceMeshes},
                                 {"skeleton", skeletonEvidence},
                                 {"path", args[i]},
                                 {"load_ms", ms},
                                 {"info", QJsonObject::fromVariantMap(info)}});
    }
    for (int i = 0; i < 8; ++i) {
      session.open(args[2]);
      session.cancel();
    }
    QCoreApplication::processEvents();
    if (session.available() || session.busy())
      throw std::runtime_error("Cancelled result became current");
    session.open(args[2]);
    wait();
    session.clear();
  } catch (const std::exception &e) {
    qCritical("%s", e.what());
    return 1;
  }
  QFile output(args[1]);
  if (!output.open(QIODevice::WriteOnly))
    return 2;
  output.write(QJsonDocument(QJsonObject{{"samples", records},
                                         {"qt", qVersion()},
                                         {"result", "passed"}})
                   .toJson());
  return 0;
}
