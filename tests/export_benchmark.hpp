#pragma once
#include <vector>

inline void preparedExportSafety(const QString &root) {
  using namespace formaview;
  require(!QFileInfo::exists(root) && QDir().mkpath(root),"Fresh prepared-output test root");
  const auto path=root+"/input.fbx"; fixture(path,3);
  AccessScope scope{{std::filesystem::path(root.toStdWString())},{std::filesystem::path(root.toStdWString())},true,"prepared-output"};
  const auto inspect=imaging::execute_image_request({{"version",1},{"operation","inspect"},{"input",path}},scope);
  const auto clip=inspect["clips"].toArray().first().toObject().value("id");
  const auto request=[&](int n) { return QJsonObject{{"version",1},{"operation","preflight"},{"input",path},
    {"output",root+"/output-"+QString::number(n)+".fbx"},{"conflict","skip"},
    {"options",QJsonObject{{"target","fbx"},{"clip_id",clip},{"include_mesh",false}}}}; };
  for (int scenario=0;scenario!=9;++scenario) {
    auto q=request(scenario);
    if (scenario==7) q["conflict"]="rename";
    if (scenario==8) {
      q["conflict"]="overwrite";
      QFile f(q["output"].toString());require(f.open(QIODevice::WriteOnly),"Create overwrite target");f.write("replace");
    }
    const auto plan=imaging::execute_image_request(q,scope);
    q["operation"]="convert";q["approval_digest"]=plan["approval_digest"];
    QFile source(path);require(source.open(QIODevice::ReadOnly),"Read controlled source");
    const auto original=source.readAll();source.close();
    auto changedScope=scope;
    if (scenario==1) {
      require(source.open(QIODevice::Append),"Change controlled source");source.write(" ");source.close();
    }
    if (scenario==2) { auto options=q["options"].toObject();options["output_axes"]="blender";q["options"]=options; }
    if (scenario==3) changedScope.policy_revision="changed-policy";
    const auto destination=q["output"].toString();
    if (scenario==4 || scenario==7) { QFile f(destination);require(f.open(QIODevice::WriteOnly),"Create collision");f.write("keep"); }
    std::stop_source stop;
    if (scenario==5) stop.request_stop();
    imaging::Limits limits;
    if (scenario==6) limits.decoded_bytes=1;
    bool rejected=false;
    try {
      const auto result=imaging::execute_image_request(q,changedScope,limits,stop.get_token());
      require(scenario==0 || scenario==4 || scenario==8,"Stale or cancelled prepared output must reject");
      require(result["status"]==(scenario==4 ? "skipped" : "succeeded"),"Prepared-output commit status");
    } catch (const skeletal::Failure &e) {
      // Source-content-derived clip IDs may become invalid before approval is checked.
      rejected=e.error.code==ErrorCode::InputChanged || (scenario==1 && e.error.code==ErrorCode::InvalidArgument) ||
        (scenario==5 && e.error.code==ErrorCode::Cancelled) || (scenario==6 && e.error.code==ErrorCode::ResourceLimit) ||
        (scenario==7 && e.error.code==ErrorCode::Conflict);
      require(((scenario>=1 && scenario<=3) || (scenario>=5 && scenario<=7)) && rejected,"Expected safety rejection");
    }
    if (scenario>=1 && scenario<=3) require(rejected && !QFileInfo::exists(destination),"Rejected conversion must not publish output");
    if (scenario==5 || scenario==6) require(rejected && !QFileInfo::exists(destination),"Cancelled/limited conversion must not publish output");
    if (scenario==4 || scenario==7) { QFile f(destination);require(f.open(QIODevice::ReadOnly) && f.readAll()=="keep","Collision preserves existing file"); }
    if (scenario==8) { QFile f(destination);require(f.open(QIODevice::ReadOnly) && f.readAll().startsWith("Kaydara"),"Explicit overwrite publishes FBX"); }
    if (scenario==1) { require(source.open(QIODevice::WriteOnly),"Restore controlled source");require(source.write(original)==original.size(),"Restore bytes"); }
  }
  std::cout << "PASS prepared-output source/options/policy changes, cancel, limits, skip/rename/overwrite\n";
}

// Controlled SDK fixture: no user assets or external dependencies.
inline void exportBenchmarkFixture(const QString &path) {
  require(!QFileInfo::exists(path), "Benchmark fixture must be new");
  std::unique_ptr<FbxManager, FbxDestroy> manager(FbxManager::Create());
  manager->SetIOSettings(FbxIOSettings::Create(manager.get(), IOSROOT));
  auto *scene = FbxScene::Create(manager.get(), "Controlled export benchmark");
  constexpr int boneCount = 32, clipCount = 138, keyCount = 60, vertexCount = 30000;
  std::vector<FbxNode *> bones;
  for (int b = 0; b < boneCount; ++b) {
    auto *node = FbxNode::Create(scene, ("Bone_" + std::to_string(b)).c_str());
    auto *bone = FbxSkeleton::Create(scene, node->GetName());
    bone->SetSkeletonType(b ? FbxSkeleton::eLimbNode : FbxSkeleton::eRoot);
    node->SetNodeAttribute(bone);
    (b ? bones.back() : scene->GetRootNode())->AddChild(node);
    bones.push_back(node);
  }
  auto *meshNode = FbxNode::Create(scene, "Mesh");
  auto *mesh = FbxMesh::Create(scene, "Mesh");
  scene->GetRootNode()->AddChild(meshNode); meshNode->SetNodeAttribute(mesh);
  mesh->InitControlPoints(vertexCount);
  for (int v = 0; v < vertexCount; ++v)
    mesh->GetControlPoints()[v] = FbxVector4(v / 3, v % 3 == 1 ? 1 : 0, v % 3 == 2 ? 1 : 0);
  for (int v = 0; v < vertexCount; v += 3) {
    mesh->BeginPolygon(); mesh->AddPolygon(v); mesh->AddPolygon(v+1); mesh->AddPolygon(v+2); mesh->EndPolygon();
  }
  auto *skin = FbxSkin::Create(scene, "Skin"); mesh->AddDeformer(skin);
  auto *pose = FbxPose::Create(scene, "Bind pose"); pose->SetIsBindPose(true);
  FbxAMatrix identity; identity.SetIdentity(); pose->Add(meshNode, identity);
  for (int b = 0; b < boneCount; ++b) {
    auto *cluster = FbxCluster::Create(scene, bones[b]->GetName());
    cluster->SetLink(bones[b]); cluster->SetLinkMode(FbxCluster::eNormalize);
    cluster->SetTransformMatrix(identity); cluster->SetTransformLinkMatrix(identity);
    for (int v = b; v < vertexCount; v += boneCount) cluster->AddControlPointIndex(v, 1);
    skin->AddCluster(cluster); pose->Add(bones[b], identity);
  }
  scene->AddPose(pose);
  for (int c = 0; c < clipCount; ++c) {
    auto *stack = FbxAnimStack::Create(scene, ("Clip_" + std::to_string(c)).c_str());
    auto *layer = FbxAnimLayer::Create(scene, "Base"); stack->AddMember(layer);
    FbxTime start, end; start.SetSecondDouble(0); end.SetSecondDouble(2);
    FbxTimeSpan span(start,end); stack->SetLocalTimeSpan(span);
    for (int b = 0; b < boneCount; ++b) {
      auto *curve = bones[b]->LclTranslation.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_X, true);
      curve->KeyModifyBegin();
      for (int k = 0; k < keyCount; ++k) {
        FbxTime time; time.SetSecondDouble(2.0*k/(keyCount-1));
        const auto index = curve->KeyAdd(time);
        curve->KeySetValue(index, static_cast<float>(std::sin(k*.1+b)*(.01+c*.001)));
        curve->KeySetInterpolation(index,FbxAnimCurveDef::eInterpolationLinear);
      }
      curve->KeyModifyEnd();
    }
  }
  std::unique_ptr<FbxExporter,FbxDestroy> writer(FbxExporter::Create(manager.get(), "Benchmark"));
  require(writer->Initialize(path.toUtf8().constData(),manager->GetIOPluginRegistry()->FindWriterIDByDescription("FBX binary (*.fbx)"),manager->GetIOSettings()),"Initialize benchmark writer");
  require(writer->Export(scene),"Write benchmark fixture");
}

inline void exportBenchmarkRun(const QString &root, const QString &input) {
  require(!QFileInfo::exists(root) && QDir().mkpath(root), "Benchmark output must be new");
  for (const QString &mode : {QString("model"),QString("animation")}) {
    const auto folder = root + '/' + mode;
    require(QDir().mkpath(folder + "/out"),"Create benchmark output");
    qputenv("FORMAVIEW_IMAGE_STATE_ROOT",(folder + "/state").toUtf8());
    ImageJobs jobs;
    const QVariantMap scope{{"input_roots",QStringList{QFileInfo(input).absolutePath()}},
                           {"output_roots",QStringList{folder + "/out"}},{"allow_overwrite",false}};
    const QVariantMap options{{"target","fbx"},{"export_content",mode},{"file_organization","per-clip"}};
    QElapsedTimer timer; timer.start(); qint64 first = -1;
    jobs.startExport({input},scope,options,"{name}-{clip}.{ext}","skip");
    wait([&] {
      if (first < 0 && !QDir(folder + "/out").entryList({"*.fbx"},QDir::Files).isEmpty()) first=timer.elapsed();
      return !jobs.busy() && ((!jobs.job().isEmpty() && !active(jobs.job())) || !jobs.error().isEmpty());
    },600000);
    const auto elapsed=timer.elapsed();
    save(folder + "/result.json",{{"elapsed_ms",elapsed},{"first_file_ms",first},
      {"input_bytes",QFileInfo(input).size()},{"job",jobs.job()},{"error",jobs.error()}});
    require(jobs.error().isEmpty() && jobs.job().value("state")=="succeeded","All benchmark exports must succeed");
    require(jobs.job().value("items").toList().size()==(mode=="model" ? 1 : 138),"Expected benchmark item count");
    std::cout << "benchmark mode=" << mode.toStdString() << " elapsed_ms=" << elapsed << " first_ms=" << first << std::endl;
  }
}
