#include "image_jobs.hpp"
#include "../src/formats/fbx_selection.hpp"
#include "formaview/geometry_cache.hpp"
#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>
#include <fbxsdk.h>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
#include <QUuid>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <windows.h>

namespace {
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
bool active(const QVariantMap &job) {
  return QStringList{"queued", "running", "paused", "pausing", "planning"}.contains(job.value("state").toString());
}
template<class F> void wait(F done, int timeout = 90000) {
  QElapsedTimer t; t.start();
  while (!done() && t.elapsed() < timeout) { QCoreApplication::processEvents(); QThread::msleep(2); }
  require(done(), "Noninteractive export-flow timeout");
}
struct FbxDestroy { template<class T> void operator()(T *p) const { if (p) p->Destroy(); } };
void fixture(const QString &path, int clips, const char *version = nullptr) {
  std::unique_ptr<FbxManager, FbxDestroy> manager(FbxManager::Create());
  manager->SetIOSettings(FbxIOSettings::Create(manager.get(), IOSROOT));
  std::unique_ptr<FbxScene, FbxDestroy> scene(FbxScene::Create(manager.get(), "Export flow"));
  auto *node = FbxNode::Create(scene.get(), "Root"); scene->GetRootNode()->AddChild(node);
  for (int i = 0; i < clips; ++i) {
    const auto clipName = i == 0 ? std::string("Model|Clip_0") : "Clip_" + std::to_string(i);
    auto *stack = FbxAnimStack::Create(scene.get(), clipName.c_str());
    auto *layer = FbxAnimLayer::Create(scene.get(), "Layer"); stack->AddMember(layer);
    FbxTime start, end; start.SetSecondDouble(0); end.SetSecondDouble(1);
    FbxTimeSpan span(start, end); stack->SetLocalTimeSpan(span);
    auto *curve = node->LclTranslation.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_X, true);
    curve->KeyModifyBegin();
    for (int k = 0; k != 2; ++k) {
      FbxTime time; time.SetSecondDouble(k); const auto index = curve->KeyAdd(time);
      curve->KeySetValue(index, static_cast<float>(i + k));
      curve->KeySetInterpolation(index, FbxAnimCurveDef::eInterpolationLinear);
    }
    curve->KeyModifyEnd();
  }
  std::unique_ptr<FbxExporter, FbxDestroy> writer(FbxExporter::Create(manager.get(), "Fixture"));
  require(writer->Initialize(path.toUtf8().constData(), manager->GetIOPluginRegistry()->FindWriterIDByDescription("FBX binary (*.fbx)"), manager->GetIOSettings()), "Initialize fixture");
  if (version) require(writer->SetFileExportVersion(version), "Set binary fixture version");
  require(writer->Export(scene.get()), "Write multi-animation fixture");
}
void save(const QString &path, const QVariantMap &data) {
  QFile file(path); require(file.open(QIODevice::WriteOnly), "Create evidence");
  file.write(QJsonDocument::fromVariant(data).toJson());
}
void cleanActorFixture(const QString &source, const QString &destination) {
  QFile input(source); require(input.open(QIODevice::ReadOnly), "Read synthetic ActorX fixture");
  const auto bytes = input.readAll(); QByteArray clean;
  for (qsizetype offset = 0; offset < bytes.size();) {
    require(bytes.size() - offset >= 32, "Complete ActorX header");
    qint32 size{}, count{};
    std::memcpy(&size, bytes.constData() + offset + 24, 4);
    std::memcpy(&count, bytes.constData() + offset + 28, 4);
    require(size >= 0 && count >= 0, "Nonnegative fixture chunk");
    const auto length = 32 + qint64(size) * count;
    require(length <= bytes.size() - offset, "Complete fixture chunk");
    if (!bytes.mid(offset, 20).startsWith("FUTURE")) clean.append(bytes.mid(offset, length));
    offset += length;
  }
  QFile out(destination); require(out.open(QIODevice::WriteOnly) && out.write(clean) == clean.size(), "Write lossless synthetic ActorX fixture");
}
void binarySelection(const QString &root) {
  using namespace formaview;
  require(QDir().mkpath(root),"Create binary selection fixture");
  const AccessScope scope{{std::filesystem::path(root.toStdWString())},{std::filesystem::path(root.toStdWString())},false,"binary-selection"};
  for (const auto *version : {FBX_2014_00_COMPATIBLE,FBX_2020_00_COMPATIBLE}) {
    const auto path=root+'/'+version+".fbx";
    fixture(path,3,version);
    const auto info=imaging::execute_image_request({{"version",1},{"operation","inspect"},{"input",path}},scope);
    const auto clips=info["clips"].toArray();require(clips.size()==3,"Binary source clip inventory");
    QFile file(path);require(file.open(QIODevice::ReadOnly),"Read binary selection fixture");const auto bytes=file.readAll();
    const auto selected=scene_export::select_fbx_takes(bytes,clips,{clips[1].toObject()["id"]},{});
    require(selected.has_value() && selected->size()<bytes.size(),"Binary take selection reduces data without fallback");
    const auto output=root+'/'+version+"-selected.fbx";
    QFile result(output);require(result.open(QIODevice::WriteOnly) && result.write(*selected)==selected->size(),"Write isolated selected binary");result.close();
    std::unique_ptr<FbxManager,FbxDestroy> manager(FbxManager::Create());manager->SetIOSettings(FbxIOSettings::Create(manager.get(),IOSROOT));
    std::unique_ptr<FbxScene,FbxDestroy> scene(FbxScene::Create(manager.get(),"readback"));
    std::unique_ptr<FbxImporter,FbxDestroy> reader(FbxImporter::Create(manager.get(),"readback"));
    require(reader->Initialize(output.toUtf8().constData(),-1,manager->GetIOSettings()) && reader->Import(scene.get()),"SDK reads rewritten binary offsets");
    require(scene->GetSrcObjectCount<FbxAnimStack>()==1 && scene->GetSrcObjectCount<FbxAnimCurve>()==1,"Only selected animation dependency graph retained");
    require(QString::fromUtf8(scene->GetSrcObject<FbxAnimStack>()->GetName())=="Clip_1","Selected clip identity retained");
    const auto *curve=scene->GetSrcObject<FbxAnimCurve>();
    require(curve->KeyGetCount()==2 && curve->KeyGetValue(0)==1 && curve->KeyGetValue(1)==2 && curve->KeyGetTime(1).GetSecondDouble()==1,"Original curve keys retained exactly");
    {
      // A forged property count backed by no property bytes must be rejected
      // before the SDK allocates from it (a 7 KB file reached 6.6 GB commit).
      auto forged=bytes; const bool wide=QByteArray(version)!=FBX_2014_00_COMPATIBLE;
      // First Properties70 "P" record; its declared count becomes ~1.7e9.
      const auto name=forged.indexOf(QByteArray("\x01PS",3));require(name>0,"Fixture has a P property record");
      const auto begin=name-(wide?24:12);
      if(wide) qToLittleEndian<quint64>(0x67000005ULL,forged.data()+begin+8); else qToLittleEndian<quint32>(0x67000005U,forged.data()+begin+4);
      const auto forgedPath=root+'/'+version+"-forged-count.fbx";
      QFile out(forgedPath);require(out.open(QIODevice::WriteOnly) && out.write(forged)==forged.size(),"Write forged FBX");out.close();
      ErrorCode code{};std::string message;
      try { (void)imaging::execute_image_request({{"version",1},{"operation","inspect"},{"input",forgedPath}},scope); }
      catch(const skeletal::Failure &e) { code=e.error.code; message=e.what(); }
      catch(const std::exception &e) { code=ErrorCode::Internal; message=e.what(); }
      if(code!=ErrorCode::InvalidArgument || message.find("Malformed FBX binary")==std::string::npos)
        throw std::runtime_error("Forged FBX property count not rejected before SDK import: "+message);
    }
    auto malformed=bytes;for(int i=27;i<35;++i) malformed[i]=char(-1);
    require(!scene_export::select_fbx_takes(malformed,clips,{clips[1].toObject()["id"]},{}),"Malformed offsets safely use original decoder");
    require(!scene_export::select_fbx_takes(QByteArray("ASCII FBX"),clips,{clips[1].toObject()["id"]},{}),"Nonbinary files use original decoder");
    std::stop_source stop;stop.request_stop();bool cancelled=false;
    try { (void)scene_export::select_fbx_takes(bytes,clips,{clips[1].toObject()["id"]},stop.get_token()); }
    catch(const skeletal::Failure &e) { cancelled=e.error.code==ErrorCode::Cancelled; }
    require(cancelled,"Binary selection respects cancellation");
  }
  std::cout << "PASS 32/64-bit FBX binary take graphs, exact SDK keys, malformed fallback and cancellation\n";
}
void queuedSourceReplacement(const QString &root, QJsonObject first, int variant) {
  using namespace formaview;
  using namespace formaview::imaging;
  require(QDir().mkpath(root + "/journal"), "Create queue race fixture");
  const auto write = [](const QString &path, const QByteArray &bytes) {
    QFile file(path); require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "Write race fixture");
  };
  const float points[]{0,0,0, 1,0,0, 0,1,0};
  const QByteArray geometry(reinterpret_cast<const char *>(points), sizeof(points));
  write(root + "/old.bin", geometry); write(root + "/new.fbx", geometry);
  const auto model = [&](const char *buffer) {
    return QByteArray("{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"") + buffer +
      "\",\"byteLength\":36}],\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]}],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
  };
  const auto modelPath = root + (variant == 3 || variant == 5 ? "/model.obj" : "/model.gltf");
  write(modelPath, variant == 3 || variant == 5 ? QByteArray("mtllib missing.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl mat\nf 1 2 3\n") : model("old.bin"));
  if (variant == 5) write(root + "/missing.mtl", "newmtl mat\nmap_Kd late.png\n");
  const auto parent = std::filesystem::path(QFileInfo(root).absolutePath().toStdWString());
  ImageTaskGroups groups({{parent}, {parent}, true, "queue-race"}, std::filesystem::path((root + "/journal").toStdWString()));
  const auto request = [&](const char *op, const QString &id = QString{}, QJsonArray items = {}) {
    QJsonObject q{{"version",1}, {"operation",op}};
    if (std::string_view(op) == "cancel") q["kind"] = "job";
    if (!id.isEmpty()) q["id"] = id;
    if (!items.isEmpty()) q["items"] = items;
    const auto result = groups.request(q);
    if (result.contains("error") && !result.contains("kind")) throw std::runtime_error(std::string(op) + ": " + QJsonDocument(result).toJson().toStdString());
    return result;
  };
  QJsonArray block;
  for (int i = 0; i != 16; ++i) { auto item = first; item["output"] = root + "/block-" + QString::number(i) + ".fbx"; block.append(item); }
  const auto blocker = request("create_job", {}, block)["job_id"].toString();
  request("pause", blocker);
  wait([&] { return request("get_job", blocker)["state"] == "paused"; });
  first["output"] = root + "/new.fbx"; first["conflict"] = "overwrite";
  if (variant == 1) {
    require(CreateHardLinkW((root + "/alias.fbx").toStdWString().c_str(), (root + "/new.fbx").toStdWString().c_str(), nullptr), "Create queued output hardlink");
    first["output"] = root + "/alias.fbx";
  }
  const QJsonObject second{{"input",modelPath}, {"output",root + "/model.fbx"}, {"conflict","skip"},
    {"options",QJsonObject{{"target","fbx"},{"coordinates","source"},{"include_mesh",true}}}};
  const auto target = request("create_job", {}, {first, second})["job_id"].toString();
  require(request("get_job", target)["state"] == "queued", "Race target must remain behind paused worker");
  if (variant == 4) {
    require(request("cancel", target)["accepted"].toBool(), "Cancel queued guarded job");
  } else if (variant == 0) {
    require(QFile::rename(modelPath, root + "/before.gltf"), "Scan lease released before worker");
    write(modelPath, model("new.fbx"));
  } else if (variant == 3) {
    write(root + "/missing.mtl", "newmtl mat\nmap_Kd new.fbx\n");
  } else if (variant == 5) {
    write(root + "/late.png", geometry);
  } else {
    const auto changed = variant == 1 ? modelPath : root + "/old.bin";
    const auto handle = CreateFileW(changed.toStdWString().c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "Open original timestamp");
    FILETIME original{}; require(GetFileTime(handle, nullptr, nullptr, &original), "Read original timestamp");
    CloseHandle(handle);
    auto replacement = variant == 1 ? model("new.fbx") : geometry;
    if (variant == 2) replacement[0] = char(1);
    write(changed, replacement);
    const auto restored = CreateFileW(changed.toStdWString().c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE |
      FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    require(restored != INVALID_HANDLE_VALUE && SetFileTime(restored, nullptr, nullptr, &original), "Restore timestamp after equal-size in-place edit");
    CloseHandle(restored);
  }
  request("resume", blocker);
  wait([&] { return !active(request("get_job", target).toVariantMap()); });
  const auto result = request("get_job", target);
  save(root + "/result.json", result.toVariantMap());
  QFile victim(root + "/new.fbx"); require(victim.open(QIODevice::ReadOnly), "Read protected replacement dependency");
  require(victim.readAll() == geometry, "Queued replacement must not overwrite newly referenced source");
  require(result["state"] == (variant == 4 ? "cancelled" : "failed"), "Changed/cancelled inventory stops before any output");
  // Missing MTL is an inventoried loss; its later appearance changes that
  // inventory, just as a newly readable texture does. It is not a parse error.
  require(result["error"].toObject()["code"] == (variant == 4 ? "cancelled" : "input_changed"), "Inventory change retains structured diagnosis");
  if (variant == 3)
    require(result["error"].toObject()["message"].toString().contains("became readable"), "Appearing MTL must be rejected by the dependency guard");
  if (variant == 1) { QFile alias(root + "/alias.fbx"); require(alias.open(QIODevice::ReadOnly) && alias.readAll() == geometry, "Queued hardlink output not replaced"); }
  std::cout << "PASS scan/queue dependency barrier variant=" << variant << '\n';
}
void recoveredSourceGuard(const QString &root, QJsonObject item, int variant) {
  using namespace formaview;
  using namespace formaview::imaging;
  require(QDir().mkpath(root + "/journal"), "Create guard recovery fixture");
  const auto source = root + "/source.fbx", output = root + "/out.fbx";
  require(QFile::copy(item["input"].toString(), source), "Copy guard recovery source");
  item["input"] = source; item["output"] = output;
  const auto path = std::filesystem::path(root.toStdWString());
  const AccessScope scope{{path}, {path}, true, "guard-recovery"};
  const auto journal = path / "journal";
  auto groups = std::make_unique<ImageTaskGroups>(scope, journal);
  const auto call = [&](QJsonObject q) {
    q["version"] = 1;
    const auto result = groups->request(q);
    if (result.contains("error") && !result.contains("kind")) throw std::runtime_error(QJsonDocument(result).toJson().toStdString());
    return result;
  };
  const auto planId = call({{"operation","create_plan"},{"items",QJsonArray{item}}})["plan_id"].toString();
  QJsonObject plan;
  wait([&] { plan = call({{"operation","get_plan"},{"id",planId}}); return !active(plan.toVariantMap()); });
  require(plan["state"] == "ready", "Recovery plan ready");
  groups.reset();
  if (variant == 1) {
    require(QFile::rename(source, root + "/previous.fbx") && QFile::copy(root + "/previous.fbx", source), "Replace source identity after shutdown");
  } else if (variant == 2) {
    QFile file(root + "/journal/" + planId.mid(6) + "/index.json");
    require(file.open(QIODevice::ReadOnly), "Read legacy fixture index");
    auto manifest = QJsonDocument::fromJson(file.readAll()).object(); file.close();
    manifest.remove("source_guard_fragments");
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Write legacy fixture index");
    file.write(QJsonDocument(manifest).toJson()); file.close();
  }
  groups = std::make_unique<ImageTaskGroups>(scope, journal);
  const auto jobId = call({{"operation","submit"},{"plan_id",planId},{"approval_digest",plan["approval_digest"]},
    {"idempotency_key",QUuid::createUuid().toString()},{"acknowledge_losses",true}})["job_id"].toString();
  QJsonObject job;
  wait([&] { job = call({{"operation","get_job"},{"id",jobId}}); return !active(job.toVariantMap()); });
  save(root + "/result.json", job.toVariantMap());
  require(job["state"] == (variant == 0 ? "succeeded" : "failed"), "Recovered guards validate before writing");
  require(QFileInfo::exists(output) == (variant == 0), "Changed/legacy recovered plan cannot publish");
  if (variant != 0) {
    require(job["error"].toObject()["code"] == "input_changed", "Recovery asks for fresh preflight");
    const auto fresh = call({{"operation","retry_plan"},{"id",jobId}})["plan_id"].toString();
    QJsonObject retried;
    wait([&] { retried = call({{"operation","get_plan"},{"id",fresh}}); return !active(retried.toVariantMap()); });
    require(retried["state"] == "ready", "Retry rebuilds dependency inventory");
  }
  std::cout << "PASS persistent source guard / retry variant=" << variant << '\n';
}
void actorSourceGuard(const QString &root, const QString &fixtures, int variant) {
  using namespace formaview;
  using namespace formaview::imaging;
  require(QDir().mkpath(root + "/journal"), "Create ActorX guard fixture");
  const auto psa = root + "/source.psa", psk = root + "/source.psk";
  const auto config = root + (variant == 5 ? "/explicit.config" : "/source.config");
  cleanActorFixture(fixtures + "/source.psa", psa);
  cleanActorFixture(fixtures + "/source.psk", psk);
  const auto write = [](const QString &p, const QByteArray &bytes) {
    QFile f(p); require(f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size(), "Write ActorX guard fixture");
  };
  const QByteArray original("[RemoveTracks]\nclip0.0=trans\n");
  const bool existing = variant == 1 || variant == 5 || variant == 6 || variant == 8 || variant == 10 || variant == 12;
  if (existing) write(config, original);
  const auto path = std::filesystem::path(root.toStdWString());
  const AccessScope scope{{path}, {path}, true, "actor-guards"};
  const auto journal = path / "journal";
  auto groups = std::make_unique<ImageTaskGroups>(scope, journal);
  const auto call = [&](QJsonObject q) {
    q["version"] = 1;
    const auto result = groups->request(q);
    if (result.contains("error") && !result.contains("kind")) throw std::runtime_error(QJsonDocument(result).toJson().toStdString());
    return result;
  };
  const auto inspect = execute_image_request({{"version",1},{"operation","inspect"},{"input",psa},{"options",QJsonObject{{"target","fbx"}}}}, scope);
  require(!inspect["clips"].toArray().isEmpty(), "Actor guard clip inventory");
  QJsonObject options{{"target","fbx"},{"coordinates","actorx-z-up-x-forward-rh-cm"},
    {"include_mesh",false},{"reference",psk},{"pairing_policy","reference-skeleton"},
    {"clip_id",inspect["clips"].toArray().first().toObject()["id"]}};
  if (variant == 5 || variant == 17) options["companion"] = config;
  if (variant == 14 || variant == 15) {
    options.remove("reference"); options["include_mesh"] = true;
    if (variant == 14) { options.remove("clip_id"); options.remove("pairing_policy"); }
    else options["animation"] = psa;
  }
  const QJsonObject item{{"input",variant == 14 || variant == 15 ? psk : psa},{"output",root + "/out.fbx"},{"conflict","skip"},{"options",options}};
  const auto get = [&](const char *op, const QString &id) { return call({{"operation",op},{"id",id}}); };
  const auto replace = [&](const QString &p) {
    require(QFile::rename(p, p + ".previous") && QFile::copy(p + ".previous", p), "Replace queued ActorX source identity");
  };
  QString id;
  const bool recovery = variant >= 9 && variant <= 13;
  if (recovery) {
    const auto planId = call({{"operation","create_plan"},{"items",QJsonArray{item}}})["plan_id"].toString();
    QJsonObject plan;
    wait([&] { plan = get("get_plan",planId); return !active(plan.toVariantMap()); });
    require(plan["state"] == "ready", "ActorX recovery plan ready");
    groups.reset();
    if (variant == 9) write(config, original);
    if (variant == 10) write(config, "[RemoveTracks]\nclip0.0=all\n");
    if (variant == 11) {
      QFile f(root + "/journal/" + planId.mid(6) + "/index.json");
      require(f.open(QIODevice::ReadOnly), "Read pre-ActorX guard manifest");
      auto manifest = QJsonDocument::fromJson(f.readAll()).object(); f.close();
      manifest.remove("actor_source_guards");
      write(f.fileName(), QJsonDocument(manifest).toJson());
    }
    groups = std::make_unique<ImageTaskGroups>(scope,journal);
    id = call({{"operation","submit"},{"plan_id",planId},{"approval_digest",plan["approval_digest"]},
      {"idempotency_key",QUuid::createUuid().toString()},{"acknowledge_losses",true}})["job_id"].toString();
  } else {
    const auto blockFile = root + "/block.fbx"; fixture(blockFile,1);
    QJsonArray blockers;
    for (int n = 0; n != 16; ++n) blockers.append(QJsonObject{{"input",blockFile},{"output",root + "/block-" + QString::number(n) + ".fbx"},
      {"conflict","skip"},{"options",QJsonObject{{"target","fbx"},{"coordinates","source"},{"include_mesh",true}}}});
    const auto blocker = call({{"operation","create_job"},{"items",blockers}})["job_id"].toString();
    get("pause",blocker);
    wait([&] { return get("get_job",blocker)["state"] == "paused"; });
    QJsonArray items;
    for (int n = 0; n != (variant == 7 || variant == 8 ? 24 : 2); ++n) {
      auto r = item; r["output"] = root + "/out-" + QString::number(n) + ".fbx"; items.append(r);
    }
    id = call({{"operation","create_job"},{"items",items}})["job_id"].toString();
    require(get("get_job",id)["state"] == "queued", "ActorX target stays behind paused worker");
    if (variant == 0 || variant == 15 || variant == 17) write(config,original);
    if (variant == 16) require(QDir().mkpath(config), "A directory appearing at absent config is not silently ignored");
    if (variant == 1 || variant == 5) {
      // Same identity, size and restored timestamp: only the content hash differs.
      const auto h = CreateFileW(config.toStdWString().c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,OPEN_EXISTING,0,nullptr);
      FILETIME t{}; require(h != INVALID_HANDLE_VALUE && GetFileTime(h,nullptr,nullptr,&t), "Read config time"); CloseHandle(h);
      auto changed = original; changed.replace("clip0.0", "clip0.1"); write(config,changed);
      const auto restored = CreateFileW(config.toStdWString().c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
      require(restored != INVALID_HANDLE_VALUE && SetFileTime(restored,nullptr,nullptr,&t), "Restore config time"); CloseHandle(restored);
    }
    if (variant == 2) replace(psa);
    if (variant == 3 || variant == 14) replace(psk);
    if (variant == 4) call({{"operation","cancel"},{"kind","job"},{"id",id}});
    if (variant == 6) require(QFile::remove(config), "Remove queued config");
    get("resume",blocker);
    if (variant == 7 || variant == 8) {
      wait([&] { return QFileInfo::exists(root + "/out-0.fbx"); });
      get("pause",id);
      wait([&] { return get("get_job",id)["state"] == "paused"; });
      const auto outputs = QDir(root).entryList({"out-*.fbx"},QDir::Files).size();
      require(outputs > 0 && outputs < 24, "Pause strictly between ActorX outputs");
      if (variant == 7) write(config,original);
      else { QFile f(config); require(!f.open(QIODevice::WriteOnly), "Existing config stays pinned between clips"); }
      get("resume",id);
    }
  }
  QJsonObject result;
  wait([&] { result = get("get_job",id); return !active(result.toVariantMap()); });
  save(root + "/result.json",result.toVariantMap());
  const bool success = variant == 7 || variant == 8 || variant == 12 || variant == 13;
  require(result["state"] == (success ? "succeeded" : variant == 4 ? "cancelled" : "failed"), "ActorX batch must preserve captured inputs/config semantics");
  if (!success) {
    require(result["error"].toObject()["code"] == (variant == 4 ? "cancelled" : variant == 17 ? "invalid_argument" : "input_changed"), "ActorX snapshot change diagnosed before output");
    require(QDir(root).entryList({"out*.fbx"},QDir::Files).isEmpty(), "ActorX changed inventory writes no output");
    if (variant != 4) {
      if (variant == 16) require(QDir().rmdir(config), "Remove synthetic empty config directory before retry");
      const auto retry = call({{"operation","retry_plan"},{"id",id}})["plan_id"].toString();
      QJsonObject plan;
      wait([&] { plan = get("get_plan",retry); return !active(plan.toVariantMap()); });
      require(plan["state"] == "ready", "ActorX retry captures new configuration");
      const auto next = call({{"operation","submit"},{"plan_id",retry},{"approval_digest",plan["approval_digest"]},
        {"idempotency_key",QUuid::createUuid().toString()},{"acknowledge_losses",true}})["job_id"].toString();
      QJsonObject retried;
      wait([&] { retried = get("get_job",next); return !active(retried.toVariantMap()); });
      save(root + "/retry.json",retried.toVariantMap());
      require(retried["state"] == "succeeded", "ActorX retry actually exports updated inventory");
    }
  } else {
    for (const auto &v : result["items"].toArray()) {
      const auto preflight = v.toObject()["preflight"].toObject();
      require(preflight["psa_companion"].toObject()["status"] == (existing ? "applied" : "not_found"), "All clips use the same captured config presence");
    }
  }
  std::cout << "PASS ActorX queue/config/recovery variant=" << variant << '\n';
}
}

// Current-frame FBX export of an Alembic geometry cache: the exact sample is
// written (transforms baked, clockwise polygons reversed for FBX), losses are
// declared, and sample selection is validated and bound to the timeline.
void abcFrameExport(const QString &root) {
  using namespace formaview;
  namespace A = Alembic::Abc;
  namespace G = Alembic::AbcGeom;
  require(!QFileInfo::exists(root) && QDir().mkpath(root + "/out"), "New ABC export root required");
  const auto input = root + "/sheet.abc";
  {
    auto out = A::CreateArchiveWithInfo(Alembic::AbcCoreOgawa::WriteArchive(), input.toStdString(),
                                        "Maya 2024 AbcExport v1.0", "frame export fixture");
    const auto ts = out.addTimeSampling(A::TimeSampling(1.0 / 24, 0));
    G::OXform xform(out.getTop(), "root", ts);
    G::OPolyMesh mesh(xform, "sheet", ts);
    for (int i = 0; i < 4; ++i) {
      G::XformSample x;
      x.setTranslation(A::V3d(10.0 * i, 0, 0));
      x.setScale(A::V3d(1, i == 3 ? 0 : 2, 1)); // Sample 3 is singular.
      xform.getSchema().set(x);
      std::vector<A::V3f> p{{0, 0, 0}, {1, 0, static_cast<float>(i)}, {1, 1, 0}, {0, 1, 0}, {2, 0, 0}};
      std::vector<std::int32_t> idx{0, 3, 2, 1, 1, 2, 4}, counts{4, 3};
      std::vector<A::V2f> uv;
      for (int c = 0; c < 7; ++c) uv.push_back({static_cast<float>(i), c * .1F});
      std::vector<A::N3f> n(7, {0, 1, 1});
      mesh.getSchema().set(G::OPolyMeshSchema::Sample(
          A::P3fArraySample(p), A::Int32ArraySample(idx), A::Int32ArraySample(counts),
          G::OV2fGeomParam::Sample(A::V2fArraySample(uv), G::kFacevaryingScope),
          G::ON3fGeomParam::Sample(A::N3fArraySample(n), G::kFacevaryingScope)));
    }
    G::OCurves curves(out.getTop(), "hair");
    std::vector<A::V3f> cp{{0, 0, 0}, {0, 1, 0}};
    std::vector<std::int32_t> cv{2};
    curves.getSchema().set(G::OCurvesSchema::Sample(A::P3fArraySample(cp), A::Int32ArraySample(cv)));
  }
  const AccessScope scope{{std::filesystem::path(root.toStdWString())},
                          {std::filesystem::path((root + "/out").toStdWString())}, false, "abc-frame"};
  const auto run = [&](const QJsonObject &request) { return imaging::execute_image_request(request, scope); };
  const auto inspect = run({{"version", 1}, {"operation", "inspect"}, {"input", input},
                            {"options", QJsonObject{{"target", "fbx"}}}});
  require(inspect["has_mesh"].toBool() && inspect["clips"].toArray().isEmpty() &&
              inspect["source_metadata"].toObject()["abc"].toObject()["samples"].toInt() == 4,
          "ABC inventory: mesh, no clips, four samples");
  auto direct = cache::Archive::open(std::filesystem::path(input.toStdWString()));
  const double seconds = direct.times()[2];
  const auto sourceSha = inspect["input"].toObject()["sha256"].toString();
  require(sourceSha.size() == 64, "Inspect reports the source hash");
  const QJsonObject options{{"target", "fbx"}, {"coordinates", "source"}, {"include_mesh", true},
                            {"abc_sample", 2}, {"abc_seconds", seconds}, {"abc_source_sha256", sourceSha}};
  const auto output = root + "/out/sheet-frame2.fbx";
  QJsonObject request{{"version", 1}, {"operation", "preflight"}, {"input", input},
                      {"output", output}, {"options", options}, {"conflict", "skip"}};
  const auto preflight = run(request);
  require(!preflight.contains("error"), "ABC frame preflight");
  QStringList codes;
  for (const auto &v : preflight["losses"].toArray()) codes << v.toObject()["code"].toString();
  for (const auto *code : {"abc_single_sample", "abc_transforms_baked", "abc_objects_omitted", "abc_unit_from_application"})
    require(codes.contains(code), "ABC frame loss declared");
  require(preflight["source_metadata"].toObject()["abc"].toObject()["sample"].toInt() == 2, "Preflight names the sample");
  request["operation"] = "convert";
  request["approval_digest"] = preflight["approval_digest"];
  request["acknowledge_losses"] = true;
  const auto converted = run(request);
  require(converted["status"] == "succeeded" && QFileInfo::exists(output), "ABC frame FBX written");
  // Independent SDK readback against a direct archive read of sample 2.
  const auto sample = direct.read(0, seconds);
  std::unique_ptr<FbxManager, FbxDestroy> manager(FbxManager::Create());
  manager->SetIOSettings(FbxIOSettings::Create(manager.get(), IOSROOT));
  std::unique_ptr<FbxScene, FbxDestroy> scene(FbxScene::Create(manager.get(), "abc readback"));
  std::unique_ptr<FbxImporter, FbxDestroy> reader(FbxImporter::Create(manager.get(), "abc readback"));
  require(reader->Initialize(output.toUtf8().constData(), -1, manager->GetIOSettings()) && reader->Import(scene.get()),
          "SDK reads ABC frame FBX");
  require(scene->GetSrcObjectCount<FbxMesh>() == 1 && scene->GetSrcObjectCount<FbxAnimStack>() == 0,
          "One mesh, no animation");
  auto *mesh = scene->GetSrcObject<FbxMesh>(0);
  require(mesh->GetControlPointsCount() == 5 && mesh->GetPolygonCount() == 2, "Point/face counts");
  // Maya writes centimetres; groups survive as identity-transform nodes.
  require(scene->GetGlobalSettings().GetSystemUnit() == FbxSystemUnit::cm, "Writer unit declared");
  auto *meshNode = mesh->GetNode();
  require(meshNode && QString::fromUtf8(meshNode->GetName()) == "sheet" && meshNode->GetParent() &&
              QString::fromUtf8(meshNode->GetParent()->GetName()) == "root" &&
              meshNode->GetParent()->GetParent() == scene->GetRootNode(),
          "Alembic group hierarchy kept");
  for (auto *node : {meshNode, meshNode->GetParent()}) {
    auto local = node->EvaluateLocalTransform();
    require(local.IsIdentity(), "Group nodes carry no transform");
  }
  for (int p = 0; p < 5; ++p) {
    const auto &v = sample.positions[static_cast<std::size_t>(p)];
    const auto &w = sample.world;
    const FbxVector4 expected(w[0] * v.x + w[1] * v.y + w[2] * v.z + w[3],
                              w[4] * v.x + w[5] * v.y + w[6] * v.z + w[7],
                              w[8] * v.x + w[9] * v.y + w[10] * v.z + w[11]);
    const auto actual = mesh->GetControlPointAt(p);
    for (int a = 0; a < 3; ++a)
      require(std::abs(actual[a] - expected[a]) < 1e-9, "Baked control point");
  }
  require(std::abs(mesh->GetControlPointAt(1)[0] - 21) < 1e-9 && std::abs(mesh->GetControlPointAt(3)[1] - 2) < 1e-9,
          "Sample 2 transform (translate 20, scale y 2)");
  // Polygon order reverses Alembic's clockwise winding, keeping the first corner.
  const std::vector<std::vector<int>> polygons{{0, 1, 2, 3}, {1, 4, 2}};
  const std::vector<std::vector<int>> sourceCorners{{0, 3, 2, 1}, {4, 6, 5}};
  auto *uv = mesh->GetElementUV("UV0");
  auto *normals = mesh->GetElementNormal();
  require(uv && normals && uv->GetMappingMode() == FbxLayerElement::eByPolygonVertex, "UV and normals present");
  int polygonVertex = 0;
  for (int f = 0; f < 2; ++f)
    for (int k = 0; k < mesh->GetPolygonSize(f); ++k, ++polygonVertex) {
      require(mesh->GetPolygonVertex(f, k) == polygons[f][k], "Reversed polygon order");
      const auto corner = sourceCorners[f][k];
      const auto value = uv->GetDirectArray().GetAt(polygonVertex);
      require(value[0] == 2 && std::abs(value[1] - corner * .1) < 1e-6, "UV follows its source corner");
      const auto n = normals->GetDirectArray().GetAt(polygonVertex);
      // Normal matrix of scale (1,2,1): (0,1,1) -> (0,.5,1) normalised.
      require(std::abs(n[0]) < 1e-9 && std::abs(n[1] - .5 / std::sqrt(1.25)) < 1e-9 &&
                  std::abs(n[2] - 1 / std::sqrt(1.25)) < 1e-9,
              "Normal transformed by the inverse transpose");
    }
  // Rejections: out-of-range sample, stale timeline, non-ABC input, missing sample.
  const auto rejected = [&](QJsonObject o, const QString &in, ErrorCode code) {
    QJsonObject r{{"version", 1}, {"operation", "preflight"}, {"input", in},
                  {"output", root + "/out/rejected.fbx"}, {"options", o}, {"conflict", "skip"}};
    try {
      (void)run(r);
    } catch (const skeletal::Failure &error) {
      return error.error.code == code;
    }
    return false;
  };
  auto bad = options; bad["abc_sample"] = 4; bad.remove("abc_seconds");
  require(rejected(bad, input, ErrorCode::InvalidArgument), "Sample outside the timeline");
  bad = options; bad["abc_seconds"] = seconds + 1;
  require(rejected(bad, input, ErrorCode::InputChanged), "Timeline mismatch");
  bad = options; bad["abc_source_sha256"] = QString(64, '0');
  require(rejected(bad, input, ErrorCode::InputChanged), "Previewed content hash mismatch");
  bad = options; bad.remove("abc_sample"); bad.remove("abc_seconds"); bad.remove("abc_source_sha256");
  require(rejected(bad, input, ErrorCode::InvalidArgument), "ABC export requires a sample");
  {
    // A singular transform keeps the frame exportable with regenerated normals.
    auto singular = options; singular["abc_sample"] = 3; singular["abc_seconds"] = direct.times()[3];
    const auto result = run({{"version", 1}, {"operation", "preflight"}, {"input", input},
                             {"output", root + "/out/singular.fbx"}, {"options", singular}, {"conflict", "skip"}});
    bool regenerated = false;
    for (const auto &v : result["losses"].toArray())
      regenerated |= v.toObject()["code"] == "abc_normals_regenerated";
    require(regenerated, "Singular transform reports regenerated normals");
  }
  {
    // Without writer metadata the unit stays an explicit metre convention.
    const auto unknown = root + "/unknown-writer.abc";
    {
      A::OArchive out(Alembic::AbcCoreOgawa::WriteArchive(), unknown.toStdString());
      G::OPolyMesh plain(out.getTop(), "tri");
      std::vector<A::V3f> p{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
      std::vector<std::int32_t> idx{0, 1, 2}, counts{3};
      plain.getSchema().set(G::OPolyMeshSchema::Sample(
          A::P3fArraySample(p), A::Int32ArraySample(idx), A::Int32ArraySample(counts)));
    }
    const auto result = run({{"version", 1}, {"operation", "preflight"}, {"input", unknown},
                             {"output", root + "/out/unknown.fbx"},
                             {"options", QJsonObject{{"target", "fbx"}, {"coordinates", "source"}, {"abc_sample", 0}}},
                             {"conflict", "skip"}});
    bool assumed = false;
    for (const auto &v : result["losses"].toArray())
      assumed |= v.toObject()["code"] == "abc_coordinate_assumption";
    require(assumed && result["source_metadata"].toObject()["abc"].toObject()["metres_per_unit"].toDouble() == 1,
            "Unknown writer uses the metre convention");
  }
  QFile obj(root + "/plain.obj");
  require(obj.open(QIODevice::WriteOnly) && obj.write("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n") > 0, "OBJ fixture");
  obj.close();
  auto plainOptions = options; plainOptions.remove("abc_source_sha256");
  require(rejected(plainOptions, root + "/plain.obj", ErrorCode::InvalidArgument), "abc_sample only for ABC");
  // GUI flow: direct export names the output after the frame.
  qputenv("FORMAVIEW_IMAGE_STATE_ROOT", (root + "/state").toUtf8());
  ImageJobs jobs;
  const QVariantMap guiScope{{"input_roots", QStringList{root}}, {"output_roots", QStringList{root + "/gui"}}, {"allow_overwrite", false}};
  require(QDir().mkpath(root + "/gui"), "GUI output");
  const QVariantMap guiOptions{{"target", "fbx"}, {"export_content", "model"}, {"file_organization", "per-clip"},
                               {"abc_sample", 1}, {"abc_seconds", direct.times()[1]}};
  // A direct export never writes unreviewed losses.
  jobs.startExport({input}, guiScope, guiOptions, "{name}-{clip}.{ext}", "skip");
  wait([&] { return !jobs.busy() && ((!jobs.job().value("id").toString().isEmpty() && !active(jobs.job())) || !jobs.error().isEmpty()); });
  save(root + "/gui-direct-job.json", jobs.job());
  require(jobs.job().value("state") == "failed" && !QFileInfo::exists(root + "/gui/sheet-frame1.fbx") &&
              jobs.job().value("items").toList().value(0).toMap().value("error").toMap().value("message")
                  .toString().contains("loss acknowledgement"),
          "Direct ABC export waits for loss review");
  // The preview's frame export preflights, shows the losses, then submits.
  jobs.preflight({input}, guiScope, guiOptions, "{name}-{clip}.{ext}", "skip");
  wait([&] { return !jobs.busy() && (jobs.plan().value("state") == "ready" || !jobs.error().isEmpty()); });
  save(root + "/gui-plan.json", jobs.plan());
  require(jobs.plan().value("state") == "ready" &&
              !jobs.plan().value("items").toList().value(0).toMap().value("preflight").toMap()
                   .value("losses").toList().isEmpty(),
          "Frame preflight lists losses for review");
  jobs.submit(true);
  wait([&] { return !jobs.busy() && !jobs.job().value("id").toString().isEmpty() && !active(jobs.job()) &&
                    jobs.job().value("state") != "failed"; });
  save(root + "/gui-job.json", jobs.job());
  require(jobs.job().value("state") == "succeeded", "Reviewed frame export succeeds");
  require(QFileInfo::exists(root + "/gui/sheet-frame1.fbx"), "GUI frame export named after the sample");
  std::cout << "PASS ABC current-frame FBX export, readback, losses, validation and GUI naming\n";
}

class ExportFlowTests {
public:
  static void realExport(const QString &root, const QString &input) {
    require(!QFileInfo::exists(root) && QDir().mkpath(root + "/out"), "New isolated real-export evidence root required");
    qputenv("FORMAVIEW_IMAGE_STATE_ROOT", (root + "/state").toUtf8());
    ImageJobs jobs;
    const QVariantMap scope{{"input_roots", QStringList{QFileInfo(input).absolutePath()}}, {"output_roots",QStringList{root + "/out"}}, {"allow_overwrite",false}};
    const QVariantMap options{{"target","fbx"},{"export_content","animation"},{"file_organization","per-clip"}};
    QElapsedTimer clock; clock.start();
    qint64 submitted = -1, firstFile = -1, nextReport = 1000;
    QObject::connect(&jobs,&ImageJobs::directExportFinished,&jobs,[&](const QString &id) {
      submitted = clock.elapsed();
      std::cout << "job_submitted_ms=" << submitted << " id=" << id.toStdString() << std::endl;
    });
    jobs.startExport({input},scope,options,"{name}-{clip}.{ext}","skip");
    wait([&] {
      if (firstFile < 0 && !QDir(root + "/out").entryList({"*.fbx"},QDir::Files).isEmpty()) {
        firstFile = clock.elapsed(); std::cout << "first_file_ms=" << firstFile << std::endl;
      }
      if (clock.elapsed() >= nextReport) {
        save(root + "/progress.json",{{"elapsed_ms",clock.elapsed()},{"busy",jobs.busy()},{"error",jobs.error()},
          {"job",jobs.job()},{"submitted_ms",submitted},{"first_file_ms",firstFile}});
        nextReport = clock.elapsed() + 1000;
      }
      return !jobs.busy() && ((!jobs.job().isEmpty() && !active(jobs.job())) || !jobs.error().isEmpty());
    },1800000);
    save(root + "/result.json",{{"elapsed_ms",clock.elapsed()},{"submitted_ms",submitted},{"first_file_ms",firstFile},
      {"input_bytes",QFileInfo(input).size()},{"input_info",jobs.inputInfo()},{"error",jobs.error()},{"job",jobs.job()}});
    std::cout << "complete_ms=" << clock.elapsed() << " state=" << jobs.job().value("state").toString().toStdString() << std::endl;
  }
  static void run(const QString &root) {
    require(!QFileInfo::exists(root) && QDir().mkpath(root + "/out"), "Use a new external test directory");
    qputenv("FORMAVIEW_IMAGE_STATE_ROOT", (root + "/state").toUtf8());
    binarySelection(root + "/binary-selection");
    abcFrameExport(root + "/abc-frame");
    ImageJobs jobs;
    const QVariantMap scope{{"input_roots", QStringList{root}}, {"output_roots", QStringList{root + "/out"}}, {"allow_overwrite", false}};
    const QVariantMap options{{"target", "fbx"}, {"export_content", "animation"}, {"file_organization", "per-clip"}};
    const auto input = root + "/multi.fbx"; fixture(input, 12);
    QStringList repeated; for (int i = 0; i < 4; ++i) repeated.append(input);
    QElapsedTimer observed; observed.start(); qint64 firstFile = -1;
    jobs.startExport(repeated, scope, options, "{index}-{clip}.{ext}", "skip");
    wait([&] {
      if (firstFile < 0 && !QDir(root + "/out").entryList({"*.fbx"}, QDir::Files).isEmpty()) firstFile = observed.elapsed();
      return !jobs.busy() && !jobs.job().value("id").toString().isEmpty() && !active(jobs.job());
    });
    const auto baselineJob = jobs.job();
    save(root + "/multi-job.json", baselineJob);
    require(baselineJob.value("state") == "succeeded", "48 multi-animation outputs succeed");
    require(baselineJob.value("items").toList().size() == 48, "Every selected clip is retained");
    require(QFileInfo::exists(root + "/out/1-Model_Clip_0.fbx"), "FBX metadata separators are safe filename tokens");
    const auto timing = baselineJob.value("timings").toMap();
    require(timing.value("source_inspections").toInt() == 1, "Prepare parses repeated source once");
    require(timing.value("protection_inspections").toInt() == 1, "Protection parses multi-clip source once");
    require(firstFile >= 0 && timing.value("first_output_from_request_ms").toLongLong() >= timing.value("preparation_ms").toLongLong(), "End-to-end timing includes preparation");
    save(root + "/observed-timing.json", {{"first_file_from_call_ms", firstFile}, {"total_from_call_ms", observed.elapsed()}, {"reported", timing}, {"synthetic", true}});
    std::cout << "PASS multi-clip deduplication and end-to-end timing: first=" << firstFile << "ms total=" << observed.elapsed() << "ms\n";

    // Queue A behind a deterministic scheduler barrier, then select B while A
    // is preparing. Its completion must still be published without selecting it.
    std::promise<void> release, entered;
    auto barrier = release.get_future().share(); auto started = entered.get_future();
    formaview::TaskRequest blocker; blocker.priority = formaview::Priority::Foreground;
    blocker.work = [&](std::stop_token) { entered.set_value(); barrier.wait(); return formaview::TaskResult{}; };
    require(jobs.control_.submit(std::move(blocker)).task.has_value(), "Queue scheduler barrier");
    started.wait();
    QString a;
    auto connection = QObject::connect(&jobs, &ImageJobs::directExportFinished, &jobs, [&](const QString &id) { a = id; });
    jobs.startExport({input}, scope, options, "a-{clip}.{ext}", "skip");
    const auto b = baselineJob.value("id").toString(); jobs.selectJob(b);
    QThread::msleep(220); release.set_value();
    wait([&] { return !jobs.busy() && !a.isEmpty() && !active(jobs.jobSnapshot(a)); });
    QObject::disconnect(connection);
    require(jobs.job().value("id") == b, "A completion cannot replace selected B");
    require(jobs.jobSnapshot(a).value("state") == "succeeded", "A continues while B is selected");
    require(jobs.jobSnapshot(a).value("timings").toMap().value("dispatch_wait_ms").toLongLong() >= 200, "End-to-end clock includes scheduler wait");
    std::cout << "PASS delayed A / selected B isolation and queued timing\n";

    // Missing input is repaired after failure. Retry must expand the original
    // selection, naming, content and output options, not the display placeholder.
    const auto missing = root + "/missing.fbx";
    auto retryOptions = options; retryOptions["file_organization"] = "multiple-stacks";
    jobs.startExport({missing}, scope, retryOptions, "restored-{name}.{ext}", "skip");
    wait([&] { return !jobs.busy() && !jobs.job().value("id").toString().isEmpty() && !active(jobs.job()); });
    require(jobs.job().value("state") == "failed", "Missing source fails explicitly");
    const auto failedId = jobs.job().value("id").toString();
    require(QFile::copy(input, missing), "Restore missing input");
    jobs.retry(failedId);
    wait([&] { return !jobs.busy() && !jobs.plan().value("id").toString().isEmpty() && !active(jobs.plan()); });
    save(root + "/retry-plan.json", jobs.plan());
    require(jobs.plan().value("state") == "ready", "Restored source can be retried");
    const auto requests = jobs.plan().value("items").toList();
    require(requests.size() == 1, "Retry retains multiple-stacks organization");
    const auto request = requests.first().toMap().value("request").toMap();
    const auto restored = request.value("options").toMap();
    require(request.value("output").toString().endsWith("restored-missing.fbx"), "Retry preserves naming");
    require(restored.value("include_mesh") == false && restored.value("clip_ids").toList().size() == 12 && restored.value("coordinates") == "source", "Retry preserves content/coordinates/clip selection");
    jobs.submit(false);
    wait([&] { return !jobs.busy() && !active(jobs.job()) && jobs.job().value("id") != failedId; });
    require(jobs.job().value("state") == "succeeded", "Repaired source actually exports");
    std::cout << "PASS missing-source retry retains export semantics and writes output\n";
    const auto actorFixtures = qEnvironmentVariable("FORMAVIEW_MODEL_EXPORT_FIXTURES");
    for (int variant = 0; variant != 6; ++variant)
      queuedSourceReplacement(root + "/queue-race-" + QString::number(variant), QJsonObject::fromVariantMap(baselineJob.value("items").toList().first().toMap().value("request").toMap()), variant);
    for (int variant = 0; variant != 3; ++variant)
      recoveredSourceGuard(root + "/guard-recovery-" + QString::number(variant), QJsonObject::fromVariantMap(baselineJob.value("items").toList().first().toMap().value("request").toMap()), variant);
    if (actorFixtures.isEmpty()) { std::cout << "SKIP optional synthetic ActorX fixture regression\n"; return; }
    for (int variant = 0; variant != 18; ++variant)
      actorSourceGuard(root + "/actor-guard-" + QString::number(variant),actorFixtures,variant);
    const auto psk = root + "/reference.psk", psa = root + "/repaired.psa";
    cleanActorFixture(actorFixtures + "/source.psk", psk);
    const QVariantMap actorOptions{{"target", "fbx"}, {"export_content", "combined"},
      {"file_organization", "multiple-stacks"}, {"paired_file", psk}, {"pairing_policy", "reference-skeleton"},
      {"output_axes", "blender"}, {"output_unit", "m"}};
    jobs.startExport({psa}, scope, actorOptions, "pair-{name}.{ext}", "skip");
    wait([&] { return !jobs.busy() && !jobs.job().value("id").toString().isEmpty() && !active(jobs.job()); });
    require(jobs.job().value("state") == "failed", "Missing PSA is a failed item");
    const auto failedActor = jobs.job().value("id").toString();
    cleanActorFixture(actorFixtures + "/source.psa", psa);
    jobs.retry(failedActor);
    wait([&] { return !jobs.busy() && !jobs.plan().value("id").toString().isEmpty() && !active(jobs.plan()); });
    save(root + "/actor-retry-plan.json", jobs.plan());
    require(jobs.plan().value("state") == "ready", "Restored PSA preflight ready");
    const auto actorRequest = jobs.plan().value("items").toList().first().toMap().value("request").toMap();
    const auto actor = actorRequest.value("options").toMap();
    require(actor.value("reference") == psk && actor.value("pairing_policy") == "reference-skeleton" && actor.value("include_mesh") == true, "Retry preserves paired model and content");
    require(actor.value("coordinates") == "actorx-z-up-x-forward-rh-cm" && actor.value("output_axes") == "blender" && actor.value("output_unit") == "m", "Retry preserves ActorX source and output coordinates");
    require(actorRequest.value("output").toString().endsWith("pair-repaired.fbx"), "Retry preserves ActorX naming");
    jobs.submit(true); // The synthetic preflight above is the explicit loss review.
    wait([&] { return !jobs.busy() && !active(jobs.job()) && jobs.job().value("id") != failedActor; });
    require(jobs.job().value("state") == "succeeded", "Repaired ActorX pair actually exports");
    std::cout << "PASS restored PSA retry preserves pairing, model content, axes and units\n";
  }
};
#include "export_benchmark.hpp"
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    require(argc == 2 || argc == 3 || argc == 4, "External evidence root required");
    if (argc == 3 && app.arguments()[2] == "--benchmark-fixture") {
      exportBenchmarkFixture(app.arguments()[1]); return 0;
    }
    if (argc == 3 && app.arguments()[2] == "--prepared-safety") {
      preparedExportSafety(app.arguments()[1]); return 0;
    }
    if (argc == 4 && app.arguments()[2] == "--benchmark") {
      exportBenchmarkRun(app.arguments()[1],app.arguments()[3]); return 0;
    }
    if (argc == 4 && app.arguments()[2] == "--real-inspect") {
      const auto root = app.arguments()[1], input = app.arguments()[3];
      require(!QFileInfo::exists(root) && QDir().mkpath(root), "New isolated inspection evidence root required");
      const formaview::AccessScope scope{{std::filesystem::path(QFileInfo(input).absolutePath().toStdWString())},
        {std::filesystem::path(root.toStdWString())},false,"real-inspect"};
      const auto result = formaview::imaging::execute_image_request({{"version",1},{"operation","inspect"},{"input",input}},scope);
      save(root + "/result.json",result.toVariantMap()); return 0;
    }
    if (argc == 3 && app.arguments()[2] == "--abc-frame") {
      abcFrameExport(app.arguments()[1]); return 0;
    }
    if (argc == 4 && app.arguments()[2] == "--real-export") {
      ExportFlowTests::realExport(app.arguments()[1],app.arguments()[3]); return 0;
    }
    if (argc == 4 && app.arguments()[2] == "--actor-guard") {
      require(!QFileInfo::exists(app.arguments()[1]), "New isolated ActorX test root required");
      actorSourceGuard(app.arguments()[1],qEnvironmentVariable("FORMAVIEW_MODEL_EXPORT_FIXTURES"),app.arguments()[3].toInt()); return 0;
    }
    ExportFlowTests::run(app.arguments()[1]); return 0;
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
