#include "formaview/image_batch.hpp"
#include "formaview/image_groups.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QUuid>
#include <chrono>
#include <io.h>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>
#include <windows.h>
#ifdef FORMAVIEW_MODEL_EXPORT
#include <fbxsdk.h>
#endif

using namespace formaview;
using namespace formaview::imaging;
namespace {
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
QString qp(const std::filesystem::path &p) {
  return QString::fromStdWString(p.wstring());
}
AccessScope scope(const std::filesystem::path &root) {
  return {{root}, {root}, true, "batch-unit-v1"};
}
void write_json(const std::filesystem::path &file, const QJsonObject &data) {
  QFile output(qp(file));
  require(output.open(QIODevice::WriteOnly), "Open test journal");
  const auto bytes = QJsonDocument(data).toJson(QJsonDocument::Compact);
  require(output.write(bytes) == bytes.size() && output.flush(),
          "Write test journal");
}
QJsonObject item(const std::filesystem::path &root) {
  QJsonArray channels;
  for (auto c : {"R", "G", "B"})
    channels.append(QJsonObject{{"source", c}, {"destination", c}});
  return {{"input", qp(root / "input.png")},
          {"output", qp(root / "output.tiff")},
          {"options", QJsonObject{{"target", "tiff"}, {"channels", channels}}}};
}
QJsonObject call(ImageBatchService &service, QJsonObject request) {
  request["version"] = 1;
  return service.request(request);
}
QString new_id() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
#ifdef FORMAVIEW_MODEL_EXPORT
struct FbxDestroy {
  template <class T> void operator()(T *value) const {
    if (value)
      value->Destroy();
  }
};
void make_many_clip_fbx(const std::filesystem::path &path, int count) {
  std::unique_ptr<FbxManager, FbxDestroy> manager(FbxManager::Create());
  require(static_cast<bool>(manager), "Create FBX fixture manager");
  manager->SetIOSettings(FbxIOSettings::Create(manager.get(), IOSROOT));
  std::unique_ptr<FbxScene, FbxDestroy> scene(
      FbxScene::Create(manager.get(), "Repeated clip preflight"));
  auto *root = FbxNode::Create(scene.get(), "Root");
  scene->GetRootNode()->AddChild(root);
  for (int clip = 0; clip < count; ++clip) {
    const auto name = "Clip_" + std::to_string(clip);
    auto *stack = FbxAnimStack::Create(scene.get(), name.c_str());
    auto *layer = FbxAnimLayer::Create(scene.get(), (name + "_Layer").c_str());
    stack->AddMember(layer);
    FbxTime start, end;
    start.SetSecondDouble(0);
    end.SetSecondDouble(1);
    FbxTimeSpan span;
    span.Set(start, end);
    stack->SetLocalTimeSpan(span);
    auto *curve = root->LclTranslation.GetCurve(
        layer, FBXSDK_CURVENODE_COMPONENT_X, true);
    curve->KeyModifyBegin();
    for (int key = 0; key < 2; ++key) {
      FbxTime time;
      time.SetSecondDouble(key);
      const auto index = curve->KeyAdd(time);
      curve->KeySetValue(index, static_cast<float>(clip + key));
      curve->KeySetInterpolation(index, FbxAnimCurveDef::eInterpolationLinear);
    }
    curve->KeyModifyEnd();
  }
  std::unique_ptr<FbxExporter, FbxDestroy> exporter(
      FbxExporter::Create(manager.get(), "Repeated clip fixture"));
  const auto format = manager->GetIOPluginRegistry()->FindWriterIDByDescription(
      "FBX binary (*.fbx)");
  const auto path_text = path.string();
  require(
      exporter->Initialize(path_text.c_str(), format, manager->GetIOSettings()),
      "Initialize FBX fixture writer");
  require(exporter->Export(scene.get()), "Write repeated clip FBX fixture");
}
void repeated_scene_clip_preflight(const std::filesystem::path &root) {
  const auto directory = root / "repeated-scene-preflight";
  std::filesystem::create_directories(directory / "journal");
  const auto input = directory / "many-clips.fbx";
  make_many_clip_fbx(input, 4);
  auto policy = scope(directory);
  ImageBatchService service(policy, directory / "journal");
  const auto inspected =
      execute_image_request({{"version", 1},
                             {"operation", "inspect"},
                             {"input", qp(input)},
                             {"options", QJsonObject{{"target", "fbx"}}}},
                            policy);
  const auto clips = inspected["clips"].toArray();
  require(clips.size() == 4, "Multi-animation fixture clip inventory");
  QJsonArray requests;
  for (qsizetype index = 0; index < clips.size(); ++index)
    requests.append(QJsonObject{
        {"input", qp(input)},
        {"output", qp(directory / ("clip-" + std::to_string(index) + ".fbx"))},
        {"conflict", "skip"},
        {"options", QJsonObject{{"target", "fbx"},
                                {"clip_id", clips[index].toObject()["id"]},
                                {"include_mesh", true}}}});
  auto created =
      call(service, {{"operation", "create_plan"}, {"items", requests}});
  require(created.contains("plan_id"), "Create repeated scene clip plan");
  QJsonObject plan;
  for (int attempt = 0; attempt < 3000; ++attempt) {
    plan =
        call(service, {{"operation", "get_plan"}, {"id", created["plan_id"]}});
    const auto state = plan["state"].toString();
    if (state != "planning" && state != "queued" && state != "running")
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(plan["state"] == "ready", "Repeated scene clip preflight readiness");
  const auto items = plan["items"].toArray();
  require(items.size() == clips.size(), "Repeated clip result count");
  for (qsizetype index = 0; index < items.size(); ++index) {
    const auto item_value = items[index].toObject();
    const auto preflight = item_value["preflight"].toObject();
    const auto exported = preflight["clips"].toArray();
    require(item_value["status"] == "ready" && exported.size() == 1 &&
                exported[0].toObject()["id"] == clips[index].toObject()["id"] &&
                preflight["encoded_bytes"].toInteger() > 0 &&
                !preflight["approval_digest"].toString().isEmpty() &&
                preflight["input"].toObject()["sha256"] ==
                    inspected["input"].toObject()["sha256"] &&
                preflight["losses"].isArray(),
            "Each repeated-source preflight preserves its selected clip and "
            "evidence");
  }
  std::cout << "PASS repeated-source multi-clip scene preflight\n";
}
#endif
void continuous_history(const std::filesystem::path &root) {
  const auto journal = root / "continuous-history";
  std::filesystem::create_directory(journal);
  QJsonObject first_submission;
  QString first_job;
  const auto terminal = [&](ImageBatchService &service, const char *op,
                            const QJsonValue &id) {
    for (int attempt = 0; attempt < 1000; ++attempt) {
      auto result = call(service, {{"operation", op}, {"id", id}});
      require(!result.contains("error"), "Continuous history query failed");
      const auto status = result["state"].toString();
      if (status != "planning" && status != "queued" && status != "running")
        return result;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("Continuous history deadline");
  };
  {
    ImageBatchService service(scope(root), journal);
    // Every completed batch retains both its plan and its job. Batch 65 used
    // to fail even though no conversion was queued and all allocations ended.
    for (int i = 0; i < 66; ++i) {
      auto request = item(root);
      request["output"] = qp(root / ("history-" + std::to_string(i) + ".tiff"));
      auto created = call(service, {{"operation", "create_plan"},
                                    {"items", QJsonArray{request}}});
      require(created.contains("plan_id"), "History blocked a later batch");
      const auto plan = terminal(service, "get_plan", created["plan_id"]);
      require(plan["state"] == "ready", "History plan failed");
      QJsonObject submission{{"operation", "submit"}, {"plan_id", plan["id"]},
                             {"approval_digest", plan["approval_digest"]},
                             {"idempotency_key", "history-" + QString::number(i)},
                             {"acknowledge_losses", true}};
      const auto submitted = call(service, submission);
      require(submitted.contains("job_id"), "History blocked submission");
      const auto job = terminal(service, "get_job", submitted["job_id"]);
      require(job["state"] == "succeeded", "History conversion failed");
      if (i == 0) { first_submission = submission; first_job = job["id"].toString(); }
    }
  }
  ImageBatchService recovered(scope(root), journal);
  const auto pagedHistory = call(recovered, {{"operation", "list_jobs"}});
  require(pagedHistory["total"].toInt() == 66,
          "Restart lost retained history");
  require(recovered.job_summaries() == pagedHistory["items"].toArray(),
          "In-process history snapshot differs from the paged service response");
  require(call(recovered, {{"operation", "get_job"}, {"id", first_job}})["state"] == "succeeded",
          "Restart lost earliest job");
  const auto repeated = call(recovered, first_submission);
  require(repeated["reused"].toBool() && repeated["job_id"] == first_job,
          "Restart lost old submission idempotency");
  require(call(recovered, {{"operation", "create_plan"},
                           {"items", QJsonArray{item(root)}}}).contains("plan_id"),
          "Restarted history still blocks new work");
}
void nonlocal_journals(const std::filesystem::path &root) {
  std::vector<std::filesystem::path> paths{
      LR"(\\localhost\formaview-no-such-share\journal)",
      LR"(//localhost/formaview-no-such-share/journal)",
      LR"(\\?\UNC\localhost\formaview-no-such-share\journal)"};
  const auto mapped = qEnvironmentVariable("FORMAVIEW_TEST_NETWORK_DRIVE").toStdWString();
  if (!mapped.empty()) {
    require(GetDriveTypeW(mapped.c_str()) == DRIVE_REMOTE,
            "Configured network-drive fixture is not a mapped network drive");
    paths.emplace_back(std::filesystem::path(mapped) / L"formaview-no-such-journal");
  }
  for (const auto &path : paths) {
    const auto rejects = [&](const auto &create, const char *label) {
      try { create(); }
      catch (const skeletal::Failure &error) {
        if (error.error.code != ErrorCode::OutOfScope)
          throw std::runtime_error(std::string(label) +
              " must reject a non-local journal before directory I/O: " + error.what());
        return;
      }
      throw std::runtime_error(std::string(label) + " accepted a non-local journal");
    };
    rejects([&] { ImageBatchService service(scope(root), path); }, "Single-batch service");
    rejects([&] { ImageTaskGroups groups(scope(root), path); }, "Task-group service");
  }
  std::cout << "PASS non-local journal rejection: " << paths.size()
            << " path forms, single-batch and task-group services\n";
}
QJsonObject crash_record(const QString &id, const QJsonObject &request,
                         const QJsonObject &intent) {
  return {{"version", 1},
          {"id", id},
          {"kind", "job"},
          {"state", "running"},
          {"items", QJsonArray{QJsonObject{{"index", 0},
                                           {"status", "running"},
                                           {"request", request},
                                           {"commit_intent", intent}}}}};
}
void child(const std::filesystem::path &root, bool unlogged) {
  if (!unlogged) {
    // Logged normal file models the window after auto-delete was cleared.
    const auto name = ".formaview-{" + new_id() + "}.tmp";
    QFile file(qp(root / name.toStdWString()));
    require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly),
            "Create recovery fixture");
    require(file.write(QByteArray(4096, '*')) == 4096 && file.flush(),
            "Write recovery fixture");
    BY_HANDLE_FILE_INFORMATION information{};
    require(GetFileInformationByHandle(
                reinterpret_cast<HANDLE>(_get_osfhandle(file.handle())),
                &information) != FALSE,
            "Read fixture file identity");
    std::ostringstream identity;
    identity << std::hex << information.dwVolumeSerialNumber << ':'
             << information.nFileIndexHigh << ':' << information.nFileIndexLow;
    const auto id = new_id();
    write_json(root / "temp-journal" / (id.toStdString() + ".json"),
               crash_record(id, item(root),
                            {{"temporary", file.fileName()},
                             {"destination", ""},
                             {"file_identity",
                              QString::fromStdString(identity.str())}}));
    std::cout << "ready" << std::endl;
    for (;;)
      std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  auto input = InputLease::open(scope(root), root / "input.png", 1024 * 1024);
  OutputTransaction transaction(scope(root), root / "not-committed.tiff",
                                input.snapshot(), ConflictPolicy::Skip);
  std::vector<std::byte> bytes(4096, std::byte{42});
  transaction.write(bytes);
  // No intent journal is written: kernel cleanup must handle process death.
  std::cout << "ready" << std::endl;
  for (;;)
    std::this_thread::sleep_for(std::chrono::seconds(1));
}
void run(const std::filesystem::path &root) {
  nonlocal_journals(root);
  std::filesystem::create_directories(root / "journal");
  std::filesystem::create_directories(root / "commit-journal");
  std::filesystem::create_directories(root / "temp-journal");
  std::filesystem::create_directories(root / "limited-journal");
  ImageSubresource pixels;
  pixels.data_window = pixels.display_window = {0, 0, 8, 8};
  for (auto name : {"R", "G", "B"}) {
    Buffer b;
    b.element_count = 64;
    b.bytes.assign(64, std::byte{64});
    pixels.channels.push_back({name, std::move(b), SampleEncoding::UNorm});
  }
  {
    ImageBatchLimits limits;
    limits.records = 2;
    ImageBatchService service(scope(root), root / "limited-journal", limits);
    for (int i = 0; i < 2; ++i)
      require(call(service, {{"operation", "create_plan"},
                             {"items", QJsonArray{item(root)}}})
                  .contains("plan_id"),
              "Bounded plan admission failed");
    const auto rejected = call(service, {{"operation", "create_plan"},
                                         {"items", QJsonArray{item(root)}}});
    require(rejected["error"].toObject()["code"] == "resource_limit",
            "Retained record cap not enforced");
  }
  const auto encoded = encode_raster(pixels, {});
  QFile file(qp(root / "input.png"));
  require(file.open(QIODevice::WriteOnly), "Create input");
  require(file.write(reinterpret_cast<const char *>(encoded.data()),
                     static_cast<qint64>(encoded.size())) ==
              static_cast<qint64>(encoded.size()),
          "Write input");
  file.close();
#ifdef FORMAVIEW_MODEL_EXPORT
  repeated_scene_clip_preflight(root);
#endif
  continuous_history(root);
  {
    const auto journal = root / "mixed-path-journal";
    std::filesystem::create_directory(journal);
    ImageBatchService service(scope(root), journal);
    auto bad = item(root);
    bad["input"] = "relative.png";
    bad["output"] = qp(root / "bad.tiff");
    const auto created = call(service, {{"operation","create_plan"},
        {"items",QJsonArray{bad,item(root)}}});
    require(created.contains("plan_id"), "Mixed plan rejected before per-item validation");
    QJsonObject plan;
    for (int attempt = 0; attempt < 1000; ++attempt) {
      plan = call(service, {{"operation","get_plan"},{"id",created["plan_id"]}});
      if (plan["state"] != "planning" && plan["state"] != "queued") break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto items = plan["items"].toArray();
    require(items.size() == 2 && items[0].toObject()["status"] == "failed" &&
                items[1].toObject()["status"] == "ready",
            "Malformed input poisoned the unrelated valid plan item");
  }
  {
    ImageBatchService service(scope(root), root / "journal");
    bool rejected = false;
    try {
      ImageBatchService duplicate(scope(root), root / "journal");
    } catch (const skeletal::Failure &error) {
      rejected = error.error.code == ErrorCode::Conflict;
    }
    require(rejected, "Two owners admitted");
    require(
        call(service, {{"operation", "create_plan"}, {"items", QJsonArray{}}})
            .contains("error"),
        "Empty plan accepted");
    require(call(service, {{"operation", "list_jobs"}, {"limit", 1.5}})
                .contains("error"),
            "Fractional page accepted");
    require(call(service, {{"operation", "create_plan"},
                           {"items", QJsonArray{item(root)}},
                           {"unknown", true}})
                .contains("error"),
            "Unknown field accepted");
  }
  // Deliberately leave the last durable entry at the pre-rename state after a
  // real commit: this deterministically exercises the commit/journal gap.
  const auto record_id = new_id();
  auto request = item(root);
  request["version"] = 1;
  request["operation"] = "preflight";
  const auto preview = execute_image_request(request, scope(root));
  request["operation"] = "convert";
  request["approval_digest"] = preview["approval_digest"];
  request["acknowledge_losses"] = true;
  QJsonObject intent;
  const auto done = execute_image_request(
      request, scope(root), {}, {}, {}, [&](const QJsonObject &observed) {
        intent = observed;
        write_json(root / "commit-journal" /
                       (record_id.toStdString() + ".json"),
                   crash_record(record_id, item(root), observed));
      });
  require(done["status"] == "succeeded", "Real output did not commit");
  {
    ImageBatchService recovered(scope(root), root / "commit-journal");
    const auto state =
        call(recovered, {{"operation", "get_job"}, {"id", record_id}});
    const auto first = state["items"].toArray()[0].toObject();
    require(first["status"] == "succeeded" &&
                first["recovered_commit"].toBool(),
            "Committed output not reconciled by identity and hash");
    require(first["result"].toObject()["output_sha256"] ==
                done["output_sha256"],
            "Recovered hash mismatch");
  }
  // A file with the same bytes but a different identity must not be attributed
  // to our unrecorded commit.
  const auto foreign_id = new_id();
  intent["file_identity"] = "not-the-output-identity";
  write_json(root / "commit-journal" / (foreign_id.toStdString() + ".json"),
             crash_record(foreign_id, item(root), intent));
  {
    ImageBatchService recovered(scope(root), root / "commit-journal");
    const auto state =
        call(recovered, {{"operation", "get_job"}, {"id", foreign_id}});
    require(state["items"].toArray()[0].toObject()["status"] == "interrupted",
            "Hash-only recovery accepted foreign output");
  }
  // Persist a real intent, then create an input alias during the commit
  // callback. Recovery must not attribute that alias to the aborted output.
  {
    const auto id = new_id();
    auto raced = item(root);
    const auto destination = root / "late-source-alias.tiff";
    raced["output"] = qp(destination);
    raced["conflict"] = "overwrite";
    raced["version"] = 1;
    raced["operation"] = "preflight";
    const auto racePreview = execute_image_request(raced, scope(root));
    raced["operation"] = "convert";
    raced["approval_digest"] = racePreview["approval_digest"];
    raced["acknowledge_losses"] = true;
    bool injected = false, rejected = false;
    try {
      (void)execute_image_request(raced, scope(root), {}, {}, {},
          [&](const QJsonObject &observed) {
            if (observed["destination"].toString().isEmpty()) return;
            write_json(root / "commit-journal" / (id.toStdString() + ".json"),
                       crash_record(id, raced, observed));
            require(CreateHardLinkW(destination.c_str(), (root / "input.png").c_str(), nullptr),
                    "Create late source alias");
            injected = true;
          });
    } catch (const skeletal::Failure &e) { rejected = e.error.code == ErrorCode::Conflict; }
    require(injected && rejected, "Late source alias did not reject commit");
    ImageBatchService recovered(scope(root), root / "commit-journal");
    const auto state = call(recovered, {{"operation", "get_job"}, {"id", id}});
    const auto entry = state["items"].toArray()[0].toObject();
    require(entry["status"] == "interrupted" && !entry["recovered_commit"].toBool(),
            "Aborted source alias incorrectly recovered as committed output");
  }
  QProcess process;
  process.start(QCoreApplication::applicationFilePath(),
                {"--unlogged-temp", qp(root)});
  require(process.waitForStarted(5000) && process.waitForReadyRead(10000),
          "Unlogged child did not create temporary");
  require(process.readAllStandardOutput().contains("ready"),
          "Unlogged child not at crash point");
  process.kill();
  require(process.waitForFinished(5000), "Unlogged child not killed");
  for (const auto &entry : std::filesystem::directory_iterator(root))
    require(entry.path().extension() != ".tmp",
            "Unlogged temporary survived actual process kill");
  process.start(QCoreApplication::applicationFilePath(),
                {"--leave-temp", qp(root)});
  require(process.waitForStarted(5000) && process.waitForReadyRead(10000),
          "Child did not create temporary");
  require(process.readAllStandardOutput().contains("ready"),
          "Child not at crash point");
  process.kill();
  require(process.waitForFinished(5000), "Child not killed");
  std::size_t before = 0;
  for (const auto &entry : std::filesystem::directory_iterator(root))
    if (entry.path().extension() == ".tmp")
      ++before;
  require(before == 1, "Crash did not leave one actual temporary");
  {
    ImageBatchService recovered(scope(root), root / "temp-journal");
  }
  for (const auto &entry : std::filesystem::directory_iterator(root))
    require(entry.path().extension() != ".tmp",
            "Owned crash temporary not removed");
  const auto journal = root / "save-crash-journal";
  std::filesystem::create_directory(journal);
  process.start(QCoreApplication::applicationFilePath(),
                {"--leave-journal-save", qp(journal)});
  require(process.waitForStarted(5000) && process.waitForReadyRead(10000),
          "Journal child did not open save file");
  require(process.readAllStandardOutput().contains("ready"), "Journal child not ready");
  process.kill();
  require(process.waitForFinished(5000), "Journal child not killed");
  std::filesystem::path remainder;
  for (const auto &entry : std::filesystem::directory_iterator(journal)) {
    std::cout << "journal crash remainder: " << entry.path().filename().string() << '\n';
    remainder = entry.path();
  }
  require(!remainder.empty(), "Expected actual QSaveFile crash remainder");
  {
    ImageBatchService reopened(scope(root), journal);
    const auto listing = call(reopened, {{"operation","list_jobs"}});
    require(listing["preserved_journal_temporaries"].toArray().size() == 1,
            "Uncommitted journal remainder not reported");
    QFile preserved(qp(remainder));
    require(preserved.open(QIODevice::ReadOnly) && preserved.readAll() == "{incomplete",
            "Uncommitted journal bytes changed or were adopted");
    require(call(reopened, {{"operation","create_plan"},
                           {"items",QJsonArray{item(root)}}}).contains("plan_id"),
            "Crash remainder prevented new work");
  }
  write_json(journal / "unrelated.json.tmp", {});
  bool unrelatedRejected = false;
  try { ImageBatchService unrelated(scope(root), journal); }
  catch (const skeletal::Failure &error) {
    unrelatedRejected = error.error.code == ErrorCode::InvalidArgument;
  }
  require(unrelatedRejected && std::filesystem::exists(remainder) &&
              std::filesystem::exists(journal / "unrelated.json.tmp"),
          "Unknown journal entries were accepted or removed");
}
} // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    if (app.arguments().size() == 3 && app.arguments()[1] == "--non-local-journals") {
      nonlocal_journals(std::filesystem::path(app.arguments()[2].toStdWString()));
      return 0;
    }
    if (app.arguments().size() == 3 && app.arguments()[1] == "--leave-journal-save") {
      QSaveFile save(app.arguments()[2] + '/' + new_id() + ".json");
      save.setDirectWriteFallback(false);
      require(save.open(QIODevice::WriteOnly), "Open actual QSaveFile");
      require(save.write("{incomplete") > 0 && save.flush(), "Write actual QSaveFile");
      std::cout << "ready" << std::endl;
      std::this_thread::sleep_for(std::chrono::seconds(60));
      return 0;
    }
    if (app.arguments().size() == 3 &&
        (app.arguments()[1] == "--leave-temp" ||
         app.arguments()[1] == "--unlogged-temp")) {
      child(std::filesystem::path(app.arguments()[2].toStdWString()),
            app.arguments()[1] == "--unlogged-temp");
      return 0;
    }
    require(app.arguments().size() == 2, "External test root required");
    run(std::filesystem::path(app.arguments()[1].toStdWString()));
    std::cout << "batch ownership, strict requests, commit-gap recovery and "
                 "actual killed-process cleanup passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
