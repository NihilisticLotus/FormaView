#include "model_session.hpp"
#include "model_document.hpp"
#include "formaview/geometry_cache.hpp"
#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>
#include <fbxsdk.h>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void need(bool ok, const char *reason) {
  if (!ok) throw std::runtime_error(reason);
}
bool closeEnough(double a, double b, double tolerance = .05) {
  return std::abs(a - b) <= tolerance;
}
void checkFrustumBasis() {
  ModelDocument document;
  document.cameraOnly = true;
  document.cameras.resize(1);
  document.cameras[0].sample.world = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  document.cameras[0].sample.has_fov_x = true;
  document.cameras[0].sample.has_fov_y = true;
  document.cameras[0].sample.fov_x_deg = 60;
  document.cameras[0].sample.fov_y_deg = 40;
  const auto corner = [](const formaview::render::Frame &frame, int index) {
    const auto &v = frame.vertices.at(static_cast<std::size_t>(3 + index*2));
    return QVector3D(v.x,v.y,v.z);
  };
  const auto check = [&](const char *axes, QVector3D up, QVector3D right) {
    document.cameras[0].sample.axes = axes;
    const auto frame = buildModelFrames(document, {}, 1, {}).model;
    need(frame && frame->vertices.size() == 18, "Camera frustum line count");
    const auto a = corner(*frame,0), b = corner(*frame,1);
    const auto c = corner(*frame,2), d = corner(*frame,3);
    need((b-a).length() > .01F && (d-a).length() > .01F &&
         (c-a).length() > .01F, "Camera frustum corners remain distinct");
    need(QVector3D::dotProduct((d+c-a-b)*.5F,up) > .01F,
         "Camera frustum follows source up axis");
    need(QVector3D::dotProduct(b-a,right) > .01F,
         "Camera frustum follows right-handed source basis");
  };
  check("+X forward, +Y up", QVector3D(0,1,0), QVector3D(0,0,1));
  check("+X forward, -Y up", QVector3D(0,-1,0), QVector3D(0,0,-1));
  check("+X forward, +Z up", QVector3D(0,0,1), QVector3D(0,-1,0));
  for (const char *axes : {"unknown", "+X forward, +X up", "+X forward, nonsense up"}) {
    document.cameras[0].sample.axes = axes;
    const auto frame = buildModelFrames(document, {}, 2, {}).model;
    need(frame && frame->vertices.size() == 6,
         "Invalid camera basis renders a position marker only");
    for (const auto &v : frame->vertices)
      need(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z),
           "Invalid camera basis does not create non-finite geometry");
  }
  document.cameras[0].sample.axes = "+X forward, +Y up";
  document.cameras[0].sample.world = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1};
  const auto singular = buildModelFrames(document, {}, 3, {}).model;
  need(singular && singular->vertices.size() == 6,
       "Singular camera transform renders a position marker only");
}
void wait(ModelSession &session) {
  QElapsedTimer deadline; deadline.start();
  while (session.busy() || session.preparing() || !session.frame()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    if (deadline.elapsed() > 30000)
      throw std::runtime_error("Camera session did not become ready: " +
                               session.error().toStdString() + " / " +
                               session.frameError().toStdString());
    QThread::msleep(1);
  }
  need(session.error().isEmpty() && session.frameError().isEmpty(),
       "Camera session reported an error");
}
QString writeAbc(const QString &directory, bool mesh) {
  namespace A = Alembic::Abc;
  namespace G = Alembic::AbcGeom;
  const QString path = directory + (mesh ? "/mixed.abc" : "/cameras.abc");
  A::OArchive archive(Alembic::AbcCoreOgawa::WriteArchive(), path.toStdString());
  const auto ts = archive.addTimeSampling(A::TimeSampling(1.0 / 2, 1.0));
  G::OXform rig(archive.getTop(), "rig", ts);
  G::OCamera main(rig, "main", ts);
  G::OCamera staticCamera(archive.getTop(), "static", ts);
  for (int i = 0; i < 3; ++i) {
    G::XformSample x;
    x.setTranslation(A::V3d(i * 2, 0, 0));
    rig.getSchema().set(x);
    G::CameraSample camera;
    camera.setFocalLength(40 + 10 * i);
    camera.setHorizontalAperture(3.6);
    camera.setVerticalAperture(2.4);
    camera.setNearClippingPlane(10);
    camera.setFarClippingPlane(1000);
    main.getSchema().set(camera);
    if (i == 0) staticCamera.getSchema().set(camera);
  }
  if (mesh) {
    G::OPolyMesh triangle(archive.getTop(), "triangle");
    std::vector<A::V3f> points{{0,0,0},{1,0,0},{0,1,0}};
    std::vector<std::int32_t> indices{0,1,2}, counts{3};
    triangle.getSchema().set(G::OPolyMeshSchema::Sample(
        A::P3fArraySample(points), A::Int32ArraySample(indices),
        A::Int32ArraySample(counts)));
  }
  return path;
}
QString writeFbx(const QString &directory, bool mesh) {
  const QString path = directory + (mesh ? "/mixed.fbx" : "/cameras.fbx");
  auto *manager = FbxManager::Create();
  need(manager != nullptr, "FBX manager");
  manager->SetIOSettings(FbxIOSettings::Create(manager, IOSROOT));
  auto *scene = FbxScene::Create(manager, "cameras");
  auto *rig = FbxNode::Create(scene, "rig");
  auto *lens = FbxCamera::Create(scene, "lens");
  lens->SetApertureWidth(1.41732283464567);
  lens->SetApertureHeight(.94488188976378);
  lens->FocalLength.Set(40);
  auto *cameraNode = FbxNode::Create(scene, "moving");
  cameraNode->SetNodeAttribute(lens);
  rig->AddChild(cameraNode);
  scene->GetRootNode()->AddChild(rig);
  auto *second = FbxNode::Create(scene, "static");
  second->SetNodeAttribute(FbxCamera::Create(scene, "staticLens"));
  scene->GetRootNode()->AddChild(second);
  if (mesh) {
    auto *geometry = FbxMesh::Create(scene, "triangle");
    geometry->InitControlPoints(3);
    geometry->SetControlPointAt(FbxVector4(0,0,0),0);
    geometry->SetControlPointAt(FbxVector4(1,0,0),1);
    geometry->SetControlPointAt(FbxVector4(0,1,0),2);
    geometry->BeginPolygon(); geometry->AddPolygon(0); geometry->AddPolygon(1);
    geometry->AddPolygon(2); geometry->EndPolygon();
    auto *node = FbxNode::Create(scene,"mesh");
    node->SetNodeAttribute(geometry);
    scene->GetRootNode()->AddChild(node);
  }
  auto *stack = FbxAnimStack::Create(scene,"camera movement");
  auto *layer = FbxAnimLayer::Create(scene,"base");
  stack->AddMember(layer);
  auto *curve = rig->LclTranslation.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_X, true);
  curve->KeyModifyBegin();
  for (int i = 0; i < 3; ++i) {
    FbxTime time; time.SetSecondDouble(i);
    curve->KeySet(curve->KeyAdd(time),time,float(i * 5.0),
                  FbxAnimCurveDef::eInterpolationLinear);
  }
  curve->KeyModifyEnd();
  auto *focal = lens->FocalLength.GetCurve(layer,true);
  focal->KeyModifyBegin();
  for (int i = 0; i < 3; ++i) {
    FbxTime time; time.SetSecondDouble(i);
    focal->KeySet(focal->KeyAdd(time),time,float(40 + i * 10.0),
                  FbxAnimCurveDef::eInterpolationLinear);
  }
  focal->KeyModifyEnd();
  auto *exporter = FbxExporter::Create(manager, "export");
  need(exporter->Initialize(path.toUtf8().constData(),-1,manager->GetIOSettings()),
       "FBX exporter initialization");
  need(exporter->Export(scene), "FBX export");
  exporter->Destroy(); manager->Destroy();
  return path;
}
void checkDocument(const QString &path, bool mixed, bool abc) {
  auto document = readModelDocument(path, {});
  need(document && document->cameras.size() == 2, "Camera count");
  need(document->cameraOnly == !mixed, "Camera-only document state");
  need(document->meshes.empty() == !mixed, "Geometry presence");
  need(document->cameraTrails.size() > 0, "Camera trajectory available");
  const auto first = document->cameras[0].sample;
  need(first.projection == "perspective" && first.has_fov_x && first.has_focal,
       "Perspective optics");
  need(closeEnough(first.focal_mm,40), "Focal length 40 mm");
  if (abc) {
    need(closeEnough(first.fov_x_deg,48.46), "ABC FOV derived from 36 mm / 40 mm");
    need(first.axes == "-Z forward, +Y up", "ABC camera local axes");
  } else {
    need(first.axes == "+X forward, +Y up" || first.axes == "unknown",
         "FBX camera local axes explicitly known or unavailable");
    need(document->cameraAt && document->cameraAt(0,1).size()==2,
         "FBX camera evaluator");
    need(closeEnough(document->cameraAt(0,1)[0].focal_mm,50),
         "Animated FBX focal length");
    need(closeEnough(document->cameraAt(0,1)[0].world[3],5),
         "Animated FBX parent transform");
  }
  ModelSession session;
  session.open(path);
  wait(session);
  need(session.cameras().size() == 2 && session.inspectable(), "Camera session ready");
  need(session.frame() && (!session.frame()->vertices.empty() || mixed),
       "Camera-only line frame");
  need(!session.frame()->guides.empty(), "Camera guides");
  session.selectCamera(1);
  need(session.cameraInfo().value("name").toString()==QStringLiteral("static"),
       "Camera selection");
  if (abc) {
    session.seekSample(2);
    QElapsedTimer deadline; deadline.start();
    while (session.cacheInfo().value("sample").toInt()!=2 ||
           !session.cacheInfo().value("framed").toBool()) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
      if (deadline.elapsed()>30000) throw std::runtime_error("ABC camera seek");
      QThread::msleep(1);
    }
    need(session.cameraIndex()==1, "ABC camera selection preserved");
    session.selectCamera(0);
    need(closeEnough(session.cameraInfo().value("focalMm").toDouble(),60),
         "ABC sampled focal length");
    session.seekSample(1);
    session.cancelSample();
    if (session.cacheInfo().value("sample").toInt()!=2 ||
        !closeEnough(session.cameraInfo().value("focalMm").toDouble(),60))
      throw std::runtime_error("ABC camera cancel keeps the presented sample: sample=" +
          std::to_string(session.cacheInfo().value("sample").toInt()) +
          " focal=" + std::to_string(session.cameraInfo().value("focalMm").toDouble()));
  } else {
    need(session.hasPlayableAnimation(), "FBX camera animation playable");
    session.selectCamera(0);
    const auto previousFrame = session.frame();
    session.seek(1);
    need(closeEnough(session.cameraInfo().value("focalMm").toDouble(),50),
         "FBX camera session seek");
    QElapsedTimer frameDeadline; frameDeadline.start();
    while (session.frame() == previousFrame && frameDeadline.elapsed() < 30000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
      QThread::msleep(1);
    }
    need(session.frame() && session.frame() != previousFrame,
         "FBX camera seek publishes a new frame");
    session.seek(0);
    session.togglePlay();
    QElapsedTimer playDeadline; playDeadline.start();
    while (session.time() < .05 && playDeadline.elapsed() < 5000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
      QThread::msleep(1);
    }
    need(session.time() >= .05 &&
         session.cameraInfo().value("focalMm").toDouble() > 40,
         "FBX camera focal animation follows playback");
    session.togglePlay();
  }
}
} // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc,argv);
  try {
    need(argc==2, "Usage: camera_preview_tests OUTPUT_DIR");
    checkFrustumBasis();
    const QString root = QString::fromLocal8Bit(argv[1]);
    need(QDir().mkpath(root), "Create camera test directory");
    checkDocument(writeAbc(root,false),false,true);
    checkDocument(writeAbc(root,true),true,true);
    checkDocument(writeFbx(root,false),false,false);
    checkDocument(writeFbx(root,true),true,false);
    std::cout << "camera_preview: ABC/FBX camera-only and mixed, two cameras, "
                 "animation, optics, session seek and line frames passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "camera_preview failed: " << e.what() << '\n';
    return 1;
  }
}
