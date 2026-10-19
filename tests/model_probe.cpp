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
      for (int mesh = 0; mesh < session.meshes().size(); ++mesh) {
        session.selectMesh(mesh);
        session.setMode(0);
        wait();
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
        int selected=0;
        for(int joint=0;joint<session.bones().size();++joint) if(session.bones()[joint].contains("L_Shoulder_a")) selected=joint;
        session.selectBone(selected); session.setMode(0); wait();
        QJsonArray lines;
        const auto frame=session.frame();
        for(std::size_t guide=frame->guides.size()-frame->selectionVertices;guide<frame->guides.size();++guide) {
          const auto &v=frame->guides[guide]; lines.append(QJsonArray{v.x,v.y,v.z});
        }
        skeletonEvidence={{"selection",QJsonObject::fromVariantMap(session.skeletonSelection())},{"binding",QJsonObject::fromVariantMap(session.bindingInfo())},{"lines",lines}};
      }
      records.append(QJsonObject{{"skeleton",skeletonEvidence},{"path", args[i]},
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
