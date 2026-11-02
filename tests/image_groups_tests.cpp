#include "formaview/image_groups.hpp"
#include "formaview/image_endpoint.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QUuid>
#include <chrono>
#include <iostream>
#include <set>
#include <thread>
#include <windows.h>
using namespace formaview;
using namespace formaview::imaging;
namespace {
// Functional completeness is unchanged under instrumentation; ASan needs
// the same 3x wait budget used by the desktop correctness regressions.
#if defined(__SANITIZE_ADDRESS__)
constexpr int groupWaitMinutes = 30;
constexpr int producerReadyMinutes = 24;
#else
constexpr int groupWaitMinutes = 10;
constexpr int producerReadyMinutes = 8;
#endif
void require(bool v, const char *m) { if (!v) throw std::runtime_error(m); }
QJsonObject query(const char *op, const QString &id = {}) {
  QJsonObject q{{"version", 1}, {"operation", op}};
  if (!id.isEmpty()) q["id"] = id;
  return q;
}
QJsonObject checked(QJsonObject r) {
  if (r.contains("error") && !r.contains("kind")) throw std::runtime_error(QJsonDocument(r).toJson().toStdString());
  return r;
}
QJsonObject wait(ImageTaskGroups &g, const char *op, const QString &id) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(groupWaitMinutes);
  for (;;) {
    const auto r = checked(g.request(query(op, id))); const auto s = r["state"].toString();
    for (const auto &value : checked(g.request(query("list_jobs")))["items"].toArray()) {
      const auto row = value.toObject();
      require(!row["id"].toString().isEmpty() && row["name"] == "Named export batch" &&
              row["category"] == "Regression" && !row["created_at"].toString().isEmpty(),
              "Concurrent task listing lost published batch metadata");
    }
    if (s != "planning" && s != "running" && s != "queued" && s != "paused" && s != "pausing") return r;
    require(std::chrono::steady_clock::now() < deadline, "Group wait timed out");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}
QJsonObject start(ImageTaskGroups &g, const QJsonArray &items) {
  auto q = query("create_plan"); q["items"] = items;
  q["name"] = "Named export batch"; q["category"] = "Regression";
  const auto plan = wait(g, "get_plan", checked(g.request(q))["plan_id"].toString());
  const auto before = checked(g.request(query("list_jobs")))["items"].toArray().size();
  require(!g.contains("missing-job"), "Unknown task is absent");
  require(g.request(query("get_job", "missing-job")).contains("error"), "Unknown task lookup fails");
  require(checked(g.request(query("list_jobs")))["items"].toArray().size() == before,
          "Read-only lookups cannot manufacture a submitted task");
  return plan;
}
QJsonObject submission(const QJsonObject &plan) {
  return {{"version", 1}, {"operation", "submit"}, {"plan_id", plan["id"]},
    {"approval_digest", plan["approval_digest"]},
    {"idempotency_key", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"acknowledge_losses", true}};
}
void fixture(const QString &path) {
  QByteArray b(18, '\0'); b[2] = 2; b[12] = 2; b[14] = 2; b[16] = 32; b[17] = 8;
  for (int n = 0; n < 4; ++n) { b.append(char(20)); b.append(char(40)); b.append(char(80)); b.append(char(n * 85)); }
  b.append(QByteArray(8, '\0')); b.append("TRUEVISION-XFILE.", 18);
  QFile f(path); require(f.open(QIODevice::WriteOnly) && f.write(b) == b.size(), "Write TGA fixture");
}
QJsonArray requests(const QString &root, int count, const QString &prefix) {
  QJsonArray items, channels;
  for (const auto *c : {"R", "G", "B", "A"}) channels.append(QJsonObject{{"source", c}, {"destination", c}});
  for (int i = 0; i < count; ++i) items.append(QJsonObject{
    {"input", root + "/source.tga"}, {"output", root + '/' + prefix + QString::number(i + 1) + ".tga"},
    {"options", QJsonObject{{"target", "tga"}, {"channels", channels}, {"alpha_channel", "A"}, {"source_alpha", "unknown"}}}, {"conflict", "skip"}});
  return items;
}
AccessScope scope(const QString &root) {
  return {{std::filesystem::path(root.toStdWString())}, {std::filesystem::path(root.toStdWString())}, true, "groups-test-v1"};
}
void endpointBoundary(const QString &root) {
  require(!QFileInfo::exists(root) && QDir().mkpath(root), "Create endpoint boundary root");
  fixture(root + "/source.tga");
  require(QDir().mkpath(root + "/groups") && QDir().mkpath(root + "/service"), "Create boundary journals");
  auto groups = std::make_shared<ImageTaskGroups>(scope(root), std::filesystem::path((root + "/groups").toStdWString()));
  auto service = std::make_shared<ImageBatchService>(scope(root), std::filesystem::path((root + "/service").toStdWString()));
  const auto name = "formaview-boundary-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
  ImageEndpoint endpoint(service, name, nullptr, groups);
  const auto send = [&](const QJsonObject &request) {
    QLocalSocket socket;
    socket.connectToServer(name);
    require(socket.waitForConnected(5000), "Connect boundary endpoint");
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n'); socket.flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!socket.canReadLine() && std::chrono::steady_clock::now() < deadline) {
      QCoreApplication::processEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(socket.canReadLine(), "Boundary endpoint timed out");
    return QJsonDocument::fromJson(socket.readLine()).object();
  };
  const auto plan = start(*groups, requests(root, 1, "approved-"));
  const auto planId = plan["id"].toString();
  const auto jobId = checked(send(submission(plan)))["job_id"].toString();
  require(wait(*groups, "get_job", jobId)["state"] == "succeeded", "Approved endpoint submit succeeds");
  bool rejected = true;
  int attempt = 0;
  for (const auto *operation : {"create_job", "create_plan"}) {
    for (const auto &id : {planId, jobId}) {
      for (const auto *key : {"id", "plan_id"}) {
        auto request = query(operation); request[key] = id;
        request["items"] = requests(root, 1, "forbidden-" + QString::number(++attempt) + '-');
        const auto response = send(request);
        const bool denied = response["error"].toObject()["code"] == "invalid_argument";
        std::cout << operation << ' ' << key << ' ' << id.toStdString() << " rejected=" << denied << std::endl;
        rejected = rejected && denied;
      }
    }
  }
  require(rejected, "Endpoint accepted desktop-only grouped operations");
  require(QDir(root).entryList({"forbidden-*"}, QDir::Files).isEmpty(), "Rejected requests must not write files");
  require(checked(send(query("get_plan", planId)))["id"] == planId, "Endpoint get_plan remains available");
  require(checked(send(query("get_job", jobId)))["id"] == jobId, "Endpoint get_job remains available");
  require(checked(send(query("list_jobs")))["total"].toInt() == 1, "Rejected requests create no grouped jobs");
  for (const auto *operation : {"pause", "resume"}) {
    const auto request = query(operation, jobId);
    require(send(request) == groups->request(request), "Endpoint preserves terminal pause/resume errors");
  }
  auto cancel = query("cancel", jobId); cancel["kind"] = "job";
  require(send(cancel) == groups->request(cancel), "Endpoint cancellation remains available");
  const auto retry = query("retry_plan", jobId);
  require(send(retry) == groups->request(retry), "Endpoint preserves retry with no remaining items");
  std::cout << "PASS grouped endpoint allowlist and approved submission" << std::endl;
}
void unrelatedJournal(const QString &root) {
  require(QDir().mkpath(root), "Create unrelated journal fixture");
  QFile sentinel(root + "/unrelated.txt");
  if (!sentinel.exists()) {
    require(sentinel.open(QIODevice::WriteOnly | QIODevice::NewOnly) && sentinel.write("preserve") == 8,
            "Create unrelated journal sentinel");
    sentinel.close();
  }
  const auto security = [&] {
    DWORD size = 0;
    const auto path = root.toStdWString();
    (void)GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &size);
    require(size != 0, "Get journal ACL size");
    std::vector<std::byte> data(size);
    require(GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION,
                             data.data(), size, &size) != FALSE,
            "Read journal ACL");
    return data;
  };
  const auto originalSecurity = security();
  bool rejected = false;
  try { ImageTaskGroups groups(scope(root), std::filesystem::path(root.toStdWString())); }
  catch (const skeletal::Failure &e) { rejected = e.error.code == ErrorCode::InvalidArgument; }
  require(rejected, "Group journal with unrelated file was not rejected");
  require(security() == originalSecurity, "Unrelated journal ACL changed");
  require(sentinel.open(QIODevice::ReadOnly) && sentinel.readAll().startsWith("preserve"),
          "Unrelated journal sentinel changed");
}
void failedRecovery(const QString &root) {
  require(!QFileInfo::exists(root) && QDir().mkpath(root + "/groups"), "Create isolated recovery root");
  const auto uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const auto folder = root + "/groups/" + uuid;
  require(QDir().mkpath(folder + "/0/records"), "Create recovery journal");
  auto save = [](const QString &path, const QJsonObject &data) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(data).toJson()) > 0,
            "Write recovery journal");
  };
  const auto jobId = "group-" + uuid + "-job";
  save(folder + "/index.json", {{"version", 1}, {"id", "group-" + uuid},
    {"job_id", jobId}, {"plan_state", "interrupted"}, {"job_state", "failed"},
    {"batches", QJsonArray{QJsonObject{{"count", 1}}}},
    {"error", QJsonObject{{"code", "invalid_argument"}, {"message", "Missing dependency"}}}});
  save(folder + "/0/requests.json", {{"items", requests(root, 1, "out-")}});
  ImageTaskGroups groups(scope(root), std::filesystem::path((root + "/groups").toStdWString()));
  const auto job = checked(groups.request(query("get_job", jobId)));
  require(job["state"] == "failed", "Recovery must retain terminal failed group state");
  require(job["items"].toArray()[0].toObject()["status"] == "interrupted", "Unstarted item is interrupted");
  require(job["items"].toArray()[0].toObject()["error"].toObject()["message"] == "Missing dependency",
          "Interrupted item retains group failure reason");
  require(job["error"].toObject()["message"] == "Missing dependency", "Group error preserved");
}
void cancelledRecovery(const QString &root) {
  require(!QFileInfo::exists(root) && QDir().mkpath(root + "/groups"), "Create isolated recovery root");
  const auto uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const auto folder = root + "/groups/" + uuid;
  require(QDir().mkpath(folder + "/0/records"), "Create recovery journal");
  auto save = [](const QString &path, const QJsonObject &data) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(data).toJson()) > 0,
            "Write recovery journal");
  };
  const auto jobId = "group-" + uuid + "-job";
  save(folder + "/index.json", {{"version", 1}, {"id", "group-" + uuid},
    {"job_id", jobId}, {"plan_state", "interrupted"}, {"job_state", "cancelled"},
    {"batches", QJsonArray{QJsonObject{{"count", 1}}}}});
  save(folder + "/0/requests.json", {{"items", requests(root, 1, "out-")}});
  ImageTaskGroups groups(scope(root), std::filesystem::path((root + "/groups").toStdWString()));
  const auto job = checked(groups.request(query("get_job", jobId)));
  require(job["state"] == "cancelled", "Recovery must retain terminal cancelled group state");
  require(job["items"].toArray()[0].toObject()["status"] == "cancelled", "Unstarted item remains cancelled");
}
void crashProducer(const QString &root) {
  fixture(root + "/source.tga"); QDir().mkpath(root + "/groups");
  ImageTaskGroups groups(scope(root), std::filesystem::path((root + "/groups").toStdWString()));
  auto images = requests(root, 4096, "crash-");
  for (qsizetype i = 0; i < images.size(); ++i) {
    auto item = images[i].toObject();
    const auto input = root + "/input-" + QString::number(i) + ".tga";
    fixture(input); item["input"] = input; images[i] = item;
  }
  const auto plan = start(groups, images);
  require(plan["state"] == "ready", "4096-item crash fixture fully preflighted");
  const auto submitted = checked(groups.request(submission(plan)));
  for (;;) {
    const auto job = checked(groups.request(query("get_job", submitted["job_id"].toString())));
    const auto items = job["items"].toArray();
    int succeeded = 0;
    for (auto v : items) if (v.toObject()["status"] == "succeeded") ++succeeded;
    if (succeeded > 0 && succeeded < 4096 && job["state"] == "running") {
      std::cout << "READY " << succeeded << std::endl;
      for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    require(job["state"] == "running" || job["state"] == "queued", "Producer reached an active job");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
void killedRecovery(const QString &root) {
  QDir().mkpath(root);
  QProcess child;
  child.start(QCoreApplication::applicationFilePath(), {"--crash-producer", root});
  require(child.waitForStarted(10000), "Start crash producer");
  QByteArray output;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(producerReadyMinutes);
  while (!output.contains("READY ") && child.state() != QProcess::NotRunning && std::chrono::steady_clock::now() < deadline) {
    child.waitForReadyRead(100); output += child.readAllStandardOutput();
  }
  if (!output.contains("READY ")) {
    child.kill(); child.waitForFinished(10000);
    throw std::runtime_error("Crash producer did not become ready within " +
        std::to_string(producerReadyMinutes) + " minutes: " +
        (output + child.readAllStandardError()).toStdString());
  }
  child.kill(); require(child.waitForFinished(10000), "Wait for killed producer");
  require(child.exitStatus() == QProcess::CrashExit, "Producer was actually killed");
  QFile evidence(root + "/producer.txt");
  require(evidence.open(QIODevice::WriteOnly), "Save crash evidence"); evidence.write(output); evidence.close();
  ImageTaskGroups recovered(scope(root), std::filesystem::path((root + "/groups").toStdWString()));
  const auto listing = checked(recovered.request(query("list_jobs")))["items"].toArray();
  require(listing.size() == 1, "Recover interrupted group job");
  const auto job = checked(recovered.request(query("get_job", listing[0].toObject()["id"].toString())));
  require(job["state"] == "interrupted" && job["total"] == 4096, "Interrupted group retains all 4096 members");
  int succeeded = 0, interrupted = 0;
  for (auto v : job["items"].toArray()) {
    const auto status = v.toObject()["status"];
    if (status == "succeeded") ++succeeded;
    if (status == "interrupted") ++interrupted;
  }
  require(succeeded > 0 && interrupted > 0 && succeeded + interrupted == 4096, "Recover successful and pending child batches");
  const auto files = QDir(root).entryList({"crash-*.tga"}, QDir::Files);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  require(QDir(root).entryList({"crash-*.tga"}, QDir::Files) == files, "Recovery must not automatically resume writes");
  const auto retry = checked(recovered.request(query("retry_plan", job["id"].toString())));
  const auto fresh = checked(recovered.request(query("get_plan", retry["plan_id"].toString())));
  require(fresh["total"] == 4096 - succeeded, "Recovered retry excludes completed outputs");
  auto cancel = query("cancel", retry["plan_id"].toString()); cancel["kind"] = "plan"; checked(recovered.request(cancel));
  wait(recovered, "get_plan", retry["plan_id"].toString());
  std::cout << "PASS actual killed-process group recovery: succeeded=" << succeeded << " interrupted=" << interrupted << std::endl;
}
void longJournal(const QString &root) {
  auto directory = root + "/long-journal";
  while (directory.size() < 280) directory += '/' + QString(64, 'x');
  const auto batchPath = directory + "/batch";
  const auto groupPath = directory + "/groups";
  require(QDir().mkpath(batchPath) && QDir().mkpath(groupPath), "Create long journal paths");
  const auto nativeBatch = std::filesystem::path(batchPath.toStdWString());
  {
    ImageBatchService owner(scope(root), nativeBatch);
    bool refused = false;
    try { ImageBatchService duplicate(scope(root), nativeBatch); }
    catch (const skeletal::Failure &e) { refused = e.error.code == ErrorCode::Conflict; }
    require(refused, "Long journal still excludes concurrent owners");
  }
  require(!QFileInfo::exists(batchPath + "/service.lock"), "Long service lock removed on normal close");
  { ImageBatchService reopened(scope(root), std::filesystem::path((batchPath + '/').toStdWString())); }
  const auto nativeGroup = std::filesystem::path(groupPath.toStdWString());
  {
    ImageTaskGroups groups(scope(root), nativeGroup);
    const auto plan = start(groups, requests(root, 65, "long-journal-"));
    require(plan["state"] == "ready", "Long journal preflight");
    const auto submitted = checked(groups.request(submission(plan)));
    require(wait(groups, "get_job", submitted["job_id"].toString())["state"] == "succeeded", "Long journal child locks can be reacquired for execution");
  }
  require(!QFileInfo::exists(groupPath + "/groups.lock"), "Long group lock removed on normal close");
  {
    ImageTaskGroups recovered(scope(root), std::filesystem::path((groupPath + '/').toStdWString()));
    const auto jobs = checked(recovered.request(query("list_jobs")))["items"].toArray();
    require(jobs.size() == 1 && jobs[0].toObject()["state"] == "succeeded", "Long journal recovery retains successful group");
  }
  std::cout << "PASS long journal ownership, release, execution and recovery" << std::endl;
}
void run(const QString &root) {
  fixture(root + "/source.tga"); QDir().mkpath(root + "/groups");
  longJournal(root);
  {
    ImageTaskGroups groups(scope(root), std::filesystem::path((root + "/groups").toStdWString()));
    for (const int count : {64, 65, 129}) {
      auto plan = start(groups, requests(root, count, QString("count-%1-").arg(count)));
      require(plan["state"] == "ready", "Every batch must preflight");
      require(plan["total"] == count && plan["batch_count"] == (count + 63) / 64, "Group membership/batch count");
      const auto q = submission(plan); const auto r = checked(groups.request(q));
      if (count == 129) {
        const auto id = r["job_id"].toString();
        require(checked(groups.request(query("pause", id)))["accepted"].toBool(), "Pause accepted");
        QJsonObject paused;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        do {
          paused = checked(groups.request(query("get_job", id)));
          if (paused["state"] == "paused") break;
          require(std::chrono::steady_clock::now() < deadline, "Pause deadline");
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (true);
        require(paused["state"] == "paused", "Pause reaches file boundary");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        require(checked(groups.request(query("get_job", id)))["items"] == paused["items"], "Paused outputs do not advance");
        require(checked(groups.request(query("resume", id)))["accepted"].toBool(), "Resume accepted");
      }
      require(groups.request(q)["job_id"] == r["job_id"], "Idempotent group submission");
      const auto job = wait(groups, "get_job", r["job_id"].toString());
      require(job["state"] == "succeeded", "Every child must execute");
      int index = 0;
      for (const auto &v : job["items"].toArray()) {
        const auto item = v.toObject(); require(item["index"] == index++, "Global item index");
        require(item["status"] == "succeeded" && QFileInfo::exists(item["result"].toObject()["committed_output"].toString()), "No missing group outputs");
      }
      std::cout << "PASS full preflight/submit " << count << std::endl;
    }
    auto duplicates = requests(root, 65, "duplicates-");
    for (qsizetype i = 0; i < duplicates.size(); ++i) {
      auto item = duplicates[i].toObject(); item["output"] = root + "/same.tga"; item["conflict"] = "rename"; duplicates[i] = item;
    }
    auto plan = start(groups, duplicates); require(plan["state"] == "ready", "Cross-batch rename preflight");
    std::set<QString> outputs;
    for (auto v : plan["items"].toArray()) require(outputs.insert(v.toObject()["preflight"].toObject()["output"].toString().toCaseFolded()).second, "Cross-batch names must be unique");
    auto r = checked(groups.request(submission(plan)));
    require(wait(groups, "get_job", r["job_id"].toString())["state"] == "succeeded", "Names stay approved across child commits");
    std::cout << "PASS cross-batch duplicate rename" << std::endl;
    fixture(root + "/later-input.tga");
    auto protectedItems = requests(root, 65, "protected-");
    auto first = protectedItems[0].toObject(); first["output"] = root + "/later-input.tga"; first["conflict"] = "overwrite"; protectedItems[0] = first;
    auto last = protectedItems[64].toObject(); last["input"] = root + "/later-input.tga"; protectedItems[64] = last;
    plan = start(groups, protectedItems);
    require(plan["state"] != "ready" && plan["items"].toArray()[0].toObject()["status"] == "failed", "Protect later-batch source");
    require(groups.request(submission(plan)).contains("error"), "Partial preflight must not submit");
    std::cout << "PASS cross-batch source protection" << std::endl;
    std::filesystem::create_hard_link(std::filesystem::path((root + "/later-input.tga").toStdWString()), std::filesystem::path((root + "/input-alias.tga").toStdWString()));
    first["output"] = root + "/input-alias.tga"; protectedItems[0] = first;
    plan = start(groups, protectedItems);
    require(plan["state"] != "ready" && plan["items"].toArray()[0].toObject()["status"] == "failed", "Protect later-batch hard link");
    std::cout << "PASS cross-batch hard-link source protection" << std::endl;
    fixture(root + "/movable-input.tga");
    auto movedItems = requests(root, 65, "moved-");
    first = movedItems[0].toObject(); first["output"] = root + "/approved-source-moved.tga"; first["conflict"] = "overwrite"; movedItems[0] = first;
    last = movedItems[64].toObject(); last["input"] = root + "/movable-input.tga"; movedItems[64] = last;
    plan = start(groups, movedItems); require(plan["state"] == "ready", "Moved-source fixture preflight");
    const auto originalBytes = QFileInfo(root + "/movable-input.tga").size();
    require(QFile::rename(root + "/movable-input.tga", root + "/approved-source-moved.tga"), "Move a future input after approval");
    r = checked(groups.request(submission(plan)));
    const auto movedJob = wait(groups, "get_job", r["job_id"].toString());
    require(movedJob["items"].toArray()[0].toObject()["status"] == "failed", "Protect approved future input identity after rename");
    require(QFileInfo(root + "/approved-source-moved.tga").size() == originalBytes, "Moved source was not replaced");
    std::cout << "PASS approved future source identity survives external rename" << std::endl;
    auto retryItems = requests(root, 65, "retry-"); auto tail = retryItems[64].toObject(); tail["conflict"] = "rename"; retryItems[64] = tail;
    plan = start(groups, retryItems); require(plan["state"] == "ready", "Retry fixture preflight");
    fixture(root + "/retry-65.tga");
    r = checked(groups.request(submission(plan))); const auto job = wait(groups, "get_job", r["job_id"].toString());
    require(job["state"] == "partially_succeeded", "Late output conflict stays visible");
    const auto retry = checked(groups.request(query("retry_plan", job["id"].toString())));
    const auto retryPlan = wait(groups, "get_plan", retry["plan_id"].toString());
    require(retryPlan["total"] == 1, "Retry must omit successful inputs");
    std::cout << "PASS retry includes only unfinished items" << std::endl;
    auto q = query("create_plan"); q["items"] = requests(root, 4097, "over-");
    r = checked(groups.request(q));
    require(groups.request(query("get_plan", r["plan_id"].toString()))["total"] == 4097, "4097 inputs accepted without a fixed cap");
    q = query("cancel", r["plan_id"].toString()); q["kind"] = "plan"; checked(groups.request(q));
    require(wait(groups, "get_plan", r["plan_id"].toString())["state"] == "cancelled", "Cancel spans all pending child batches");
    std::cout << "PASS 4097 inputs accepted, group cancellation" << std::endl;
    auto longInputs = requests(root, 4096, "long-");
    for (qsizetype i = 0; i < longInputs.size(); ++i) {
      auto item = longInputs[i].toObject();
      item["input"] = root + '/' + QString(200, 'x') + '/' + QString::number(i) + ".tga";
      longInputs[i] = item;
    }
    q = query("create_plan"); q["items"] = longInputs; r = checked(groups.request(q));
    q = query("cancel", r["plan_id"].toString()); q["kind"] = "plan"; checked(groups.request(q));
    wait(groups, "get_plan", r["plan_id"].toString());
    const auto protectionRoot = root + "/groups/" + r["plan_id"].toString().mid(6);
    const auto fragments = QDir(protectionRoot).entryList({"protected-*.json"}, QDir::Files);
    require(fragments.size() == 64, "4096 long source paths use 64 small fragments");
    for (const auto &file : fragments) require(QFileInfo(protectionRoot + '/' + file).size() <= 1024 * 1024, "Protection fragment stays bounded");
    std::cout << "PASS 4096 long paths use bounded protection fragments" << std::endl;
  }
  {
    ImageTaskGroups recovered(scope(root), std::filesystem::path((root + "/groups").toStdWString()));
    const auto jobs = checked(recovered.request(query("list_jobs")))["items"].toArray();
    require(jobs.size() == 6, "Recover group job membership");
    for (auto v : jobs) {
      const auto job = checked(recovered.request(query("get_job", v.toObject()["id"].toString())));
      require(job["state"] != "running" && job["state"] != "queued", "Recovery never resumes writes");
      require(job["total"].toInt() > 0, "Recover full item snapshots");
      require(job["name"] == "Named export batch" && job["category"] == "Regression" &&
              !job["created_at"].toString().isEmpty(), "Recover batch name, category and submission time");
    }
    std::cout << "PASS group journal recovery" << std::endl;
  }
}
void directExport(const QString &root) {
  require(QDir().mkpath(root), "Create direct test root");
  require(QDir().mkpath(root + "/direct-groups") && QDir().mkpath(root + "/public-service"), "Create direct journals");
  fixture(root + "/source.tga");
  const auto sourceBytes = [&] { QFile f(root + "/source.tga"); require(f.open(QIODevice::ReadOnly), "Read protected source"); return f.readAll(); }();
  QJsonObject completed;
  QString cancelledId;
  {
    ImageTaskGroups groups(scope(root), std::filesystem::path((root + "/direct-groups").toStdWString()));
    auto pending = query("create_plan"); pending["items"] = requests(root, 64, "invalid-kind-");
    const auto pendingId = checked(groups.request(pending))["plan_id"].toString();
    auto invalidKind = query("cancel", pendingId); invalidKind["kind"] = "typo";
    const auto invalidKindResult = groups.request(invalidKind);
    if (!invalidKindResult.contains("error"))
      std::cerr << "invalid kind accepted=" << invalidKindResult["accepted"].toBool() << '\n';
    require(invalidKindResult["error"].toObject()["code"] == "invalid_argument",
            "Invalid cancellation kind must not cancel a grouped plan");
    auto extraCancel = query("cancel", pendingId); extraCancel["kind"] = "plan"; extraCancel["unexpected"] = true;
    const auto extraCancelResult = groups.request(extraCancel);
    if (!extraCancelResult.contains("error"))
      std::cerr << "unknown field accepted=" << extraCancelResult["accepted"].toBool() << '\n';
    require(extraCancelResult["error"].toObject()["code"] == "invalid_argument",
            "Unknown cancellation fields must not cancel a grouped plan");
    auto stopPending = query("cancel", pendingId); stopPending["kind"] = "plan";
    checked(groups.request(stopPending));
    auto mismatchJob = query("create_job"); mismatchJob["items"] = requests(root, 64, "mismatch-");
    mismatchJob["name"] = "Named export batch"; mismatchJob["category"] = "Regression";
    const auto mismatchJobId = checked(groups.request(mismatchJob))["job_id"].toString();
    const auto mismatchPlanId = checked(groups.request(query("get_job", mismatchJobId)))["plan_id"].toString();
    require(!mismatchPlanId.isEmpty(), "Direct job exposes its plan identifier");
    const auto wrongPause = groups.request(query("pause", mismatchPlanId));
    if (!wrongPause.contains("error"))
      std::cerr << "plan ID paused job=" << wrongPause["accepted"].toBool() << '\n';
    require(wrongPause["error"].toObject()["code"] == "invalid_argument",
            "A plan ID must not pause its associated grouped job");
    checked(groups.request(query("pause", mismatchJobId)));
    require(groups.request(query("resume", mismatchPlanId))["error"].toObject()["code"] == "invalid_argument",
            "A plan ID must not resume its associated grouped job");
    const auto wrongJobLookup = groups.request(query("get_job", mismatchPlanId));
    if (!wrongJobLookup.contains("error"))
      std::cerr << "plan ID returned job=" << wrongJobLookup["id"].toString().toStdString() << '\n';
    require(wrongJobLookup["error"].toObject()["code"] == "invalid_argument",
            "A plan ID must not retrieve the associated grouped job");
    require(groups.request(query("get_plan", mismatchJobId))["error"].toObject()["code"] == "invalid_argument",
            "A job ID must not retrieve the associated grouped plan");
    auto mismatchedCancel = query("cancel", mismatchPlanId); mismatchedCancel["kind"] = "job";
    const auto mismatchedResult = groups.request(mismatchedCancel);
    if (!mismatchedResult.contains("error"))
      std::cerr << "mismatched kind accepted=" << mismatchedResult["accepted"].toBool() << '\n';
    require(mismatchedResult["error"].toObject()["code"] == "invalid_argument",
            "A plan ID must not cancel its associated grouped job");
    auto stopMismatch = query("cancel", mismatchJobId); stopMismatch["kind"] = "job";
    checked(groups.request(stopMismatch));
    auto items = requests(root, 10, "direct-");
    auto missing = items[1].toObject(); missing["input"] = root + "/missing.tga"; items[1] = missing;
    auto protectedItem = items[2].toObject(); protectedItem["output"] = root + "/source.tga"; protectedItem["conflict"] = "overwrite"; items[2] = protectedItem;
    fixture(root + "/direct-4.tga");
    auto lossy = items[4].toObject(); auto options = lossy["options"].toObject();
    auto channels = options["channels"].toArray(); channels.removeLast(); options["channels"] = channels;
    options["alpha_action"] = "drop"; lossy["options"] = options; items[4] = lossy;
    auto q = query("create_job"); q["items"] = items; q["name"] = "Named export batch"; q["category"] = "Regression";
    const auto id = checked(groups.request(q))["job_id"].toString(); require(!id.isEmpty(), "Direct export immediately returns a durable job");
    auto wrongVersion = query("get_job", id); wrongVersion["version"] = 2;
    require(groups.request(wrongVersion)["error"].toObject()["code"] == "invalid_argument",
            "Grouped requests must reject unsupported protocol versions");
    wrongVersion.remove("version");
    require(groups.request(wrongVersion)["error"].toObject()["code"] == "invalid_argument",
            "Grouped requests must require a protocol version");
    wrongVersion["version"] = "1";
    require(groups.request(wrongVersion)["error"].toObject()["code"] == "invalid_argument",
            "Grouped requests must reject nonnumeric versions");
    auto wrongCancel = query("cancel", id); wrongCancel["kind"] = "job"; wrongCancel["version"] = 2;
    require(groups.request(wrongCancel)["error"].toObject()["code"] == "invalid_argument",
            "Invalid-version cancellation must not affect a grouped job");
    {
      require(QDir().mkpath(root + "/endpoint-service"), "Create isolated endpoint journal");
      for (int i = 0; i < 3; ++i) {
        const auto recordId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QFile record(root + "/endpoint-service/" + recordId + ".json");
        require(record.open(QIODevice::WriteOnly), "Create synthetic history record");
        const QJsonObject history{{"version", 1}, {"id", recordId}, {"kind", "job"},
                                  {"state", "succeeded"}, {"plan_id", "synthetic"}, {"items", QJsonArray{}}};
        require(record.write(QJsonDocument(history).toJson(QJsonDocument::Compact)) > 0, "Write synthetic history record");
      }
      auto service = std::make_shared<ImageBatchService>(scope(root), std::filesystem::path((root + "/endpoint-service").toStdWString()));
      auto sharedGroups = std::shared_ptr<ImageTaskGroups>(&groups, [](ImageTaskGroups *) {});
      const auto endpointName = "formaview-version-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
      ImageEndpoint endpoint(service, endpointName, nullptr, sharedGroups);
      const auto requestOverPipe = [&](const QJsonObject &request) {
        QLocalSocket socket;
        socket.connectToServer(endpointName);
        require(socket.waitForConnected(5000), "Connect isolated endpoint");
        socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
        socket.flush();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!socket.canReadLine() && std::chrono::steady_clock::now() < deadline) {
          QCoreApplication::processEvents();
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(socket.canReadLine(), "Endpoint response timed out");
        return QJsonDocument::fromJson(socket.readLine()).object();
      };
      QJsonArray expected;
      for (const auto &row : checked(service->request(query("list_jobs")))["items"].toArray()) expected.append(row);
      for (const auto &row : checked(groups.request(query("list_jobs")))["items"].toArray()) expected.append(row);
      for (const auto offset : {0, 2, 3, static_cast<int>(expected.size()) - 1, static_cast<int>(expected.size())}) {
        auto listing = query("list_jobs"); listing["offset"] = offset; listing["limit"] = 2;
        const auto actual = checked(requestOverPipe(listing));
        const auto pageItems = actual["items"].toArray();
        const auto count = std::min<int>(2, static_cast<int>(expected.size()) - offset);
        require(actual["total"].toInt() == expected.size() && pageItems.size() == count, "Merged history page size");
        for (int i = 0; i < count; ++i)
          require(pageItems[i].toObject()["id"] == expected[offset+i].toObject()["id"], "Merged history page order");
        const auto next = offset + count;
        require(next < expected.size() ? actual["next_offset"].toInt(-1) == next : actual["next_offset"].isNull(),
                "Merged history next offset");
      }
      for (const auto offset : {-1.0, 0.5, static_cast<double>(expected.size()) + 1.0}) {
        auto listing = query("list_jobs"); listing["offset"] = offset;
        require(requestOverPipe(listing)["error"].toObject()["code"] == "invalid_argument",
                "Merged history must reject invalid offsets");
      }
      auto invalidListVersion = query("list_jobs"); invalidListVersion["version"] = 2;
      require(requestOverPipe(invalidListVersion)["error"].toObject()["code"] == "invalid_argument",
              "Merged history must reject invalid protocol versions");
      auto invalidPipe = query("get_job", id); invalidPipe["version"] = 2;
      require(requestOverPipe(invalidPipe)["error"].toObject()["code"] == "invalid_argument",
              "Endpoint must reject unsupported versions for grouped jobs");
      auto invalidPipeKind = query("cancel", id); invalidPipeKind["kind"] = "typo";
      require(requestOverPipe(invalidPipeKind)["error"].toObject()["code"] == "invalid_argument",
              "Endpoint must reject invalid grouped cancellation kinds");
      auto extraPipeCancel = query("cancel", id); extraPipeCancel["kind"] = "job"; extraPipeCancel["unexpected"] = true;
      require(requestOverPipe(extraPipeCancel)["error"].toObject()["code"] == "invalid_argument",
              "Endpoint must reject unknown grouped cancellation fields");
      const auto directPlanId = checked(groups.request(query("get_job", id)))["plan_id"].toString();
      require(requestOverPipe(query("pause", directPlanId))["error"].toObject()["code"] == "invalid_argument",
              "Endpoint must reject a plan ID for grouped job pause");
      require(requestOverPipe(query("resume", directPlanId))["error"].toObject()["code"] == "invalid_argument",
              "Endpoint must reject a plan ID for grouped job resume");
      require(requestOverPipe(query("retry_plan", directPlanId))["error"].toObject()["code"] == "invalid_argument",
              "Endpoint must reject a plan ID for grouped job retry");
      auto wrongPipeSubmit = query("submit"); wrongPipeSubmit["plan_id"] = id;
      wrongPipeSubmit["idempotency_key"] = "wrong-type"; wrongPipeSubmit["approval_digest"] = "invalid";
      require(requestOverPipe(wrongPipeSubmit)["error"].toObject()["code"] == "invalid_argument",
              "Endpoint must reject a job ID for grouped plan submission");
      require(requestOverPipe(query("get_job", directPlanId))["error"].toObject()["code"] == "invalid_argument",
              "Endpoint must reject a plan ID for grouped job lookup");
      require(requestOverPipe(query("get_plan", id))["error"].toObject()["code"] == "invalid_argument",
              "Endpoint must reject a job ID for grouped plan lookup");
      auto mismatchedPipe = query("cancel", directPlanId); mismatchedPipe["kind"] = "job";
      require(requestOverPipe(mismatchedPipe)["error"].toObject()["code"] == "invalid_argument",
              "Endpoint must reject a plan ID used as a grouped job ID");
      require(requestOverPipe(query("get_job", id))["id"].toString() == id,
              "Endpoint must continue accepting valid grouped requests");
    }
    completed = wait(groups, "get_job", id);
    require(completed["state"] == "partially_succeeded", "Direct failures do not prevent remaining outputs");
    auto wrongRetry = query("retry_plan", completed["plan_id"].toString());
    const auto wrongRetryResult = groups.request(wrongRetry);
    auto wrongSubmit = query("submit"); wrongSubmit["plan_id"] = id;
    wrongSubmit["idempotency_key"] = "wrong-type"; wrongSubmit["approval_digest"] = "invalid";
    const auto wrongSubmitResult = groups.request(wrongSubmit);
    if (!wrongRetryResult.contains("error"))
      std::cerr << "plan ID created retry=" << wrongRetryResult["plan_id"].toString().toStdString() << '\n';
    if (wrongSubmitResult["error"].toObject()["code"] != "invalid_argument")
      std::cerr << "job ID submit error=" << wrongSubmitResult["error"].toObject()["code"].toString().toStdString() << '\n';
    require(wrongRetryResult["error"].toObject()["code"] == "invalid_argument",
            "Retry must reject a grouped plan ID");
    require(wrongSubmitResult["error"].toObject()["code"] == "invalid_argument",
            "Submit must reject a grouped job ID");
    const auto result = completed["items"].toArray();
    require(result.size() == 10, "Every direct item is retained");
    for (int i = 0; i < 10; ++i) {
      const auto expected = i == 1 || i == 2 || i == 4 ? "failed" : i == 3 ? "skipped" : "succeeded";
      require(result[i].toObject()["status"] == expected, "Direct item outcome differs from expected");
    }
    require(!result[4].toObject()["preflight"].toObject()["losses"].toArray().isEmpty(), "Losses are retained for review");
    require(!QFileInfo::exists(root + "/direct-5.tga"), "Unacknowledged losses never write output");
    const auto timing = completed["timings"].toObject();
    require(timing.contains("first_output_ms") && timing["first_output_ms"].toDouble() < timing["last_check_ms"].toDouble(), "First write precedes final item preflight");
    QFile report(root + "/direct-result.json"); require(report.open(QIODevice::WriteOnly), "Save direct evidence"); report.write(QJsonDocument(completed).toJson());
    QFile source(root + "/source.tga"); require(source.open(QIODevice::ReadOnly) && source.readAll() == sourceBytes, "Cross-item source protection remains intact");
    auto duplicates = requests(root, 2, "duplicate-direct-");
    auto duplicate = duplicates[1].toObject();
    duplicate["output"] = duplicates[0].toObject()["output"];
    duplicate["conflict"] = "overwrite"; duplicates[1] = duplicate;
    q["items"] = duplicates;
    const auto duplicateId = checked(groups.request(q))["job_id"].toString();
    const auto duplicateResult = wait(groups, "get_job", duplicateId)["items"].toArray();
    require(duplicateResult[0].toObject()["status"] == "succeeded" && duplicateResult[1].toObject()["status"] == "failed", "Duplicate direct output cannot overwrite an earlier item");
    q["items"] = requests(root, 32, "cancel-");
    cancelledId = checked(groups.request(q))["job_id"].toString();
    checked(groups.request(query("pause", cancelledId)));
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    require(checked(groups.request(query("get_job", cancelledId)))["pause_requested"].toBool(), "Direct pause is recorded");
    checked(groups.request(query("resume", cancelledId)));
    auto cancel = query("cancel", cancelledId); cancel["kind"] = "job";
    checked(groups.request(cancel));
    const auto cancelled = wait(groups, "get_job", cancelledId);
    require(cancelled["state"] == "cancelled", "Direct cancellation settles");
    for (const auto &v : cancelled["items"].toArray()) {
      const auto status = v.toObject()["status"].toString();
      require(status != "pending" && status != "running" && status != "ready", "Cancellation leaves no nonterminal item");
    }
    ImageBatchService publicService(scope(root), std::filesystem::path((root + "/public-service").toStdWString()));
    require(publicService.request(q).contains("error"), "Public MCP service refuses desktop direct operation");
  }
  {
    ImageTaskGroups recovered(scope(root), std::filesystem::path((root + "/direct-groups").toStdWString()));
    const auto after = checked(recovered.request(query("get_job", completed["id"].toString())));
    require(after["items"] == completed["items"], "Direct success, failure and loss evidence survives restart");
    const auto cancelled = checked(recovered.request(query("get_job", cancelledId)));
    require(cancelled["state"] == "cancelled", "Direct cancellation remains cancelled after restart");
  }
  std::cout << "PASS direct per-item checks/exports, partial failures, losses, protection, pause/cancel, recovery and MCP boundary" << std::endl;
}
}
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    if (argc == 3 && app.arguments()[1] == "--endpoint-boundary") { endpointBoundary(app.arguments()[2]); return 0; }
    if (argc == 3 && app.arguments()[1] == "--unrelated-journal") {
      unrelatedJournal(app.arguments()[2]);
      return 0;
    }
    if (argc == 3 && app.arguments()[1] == "--request-file") {
      QFile file(app.arguments()[2]); require(file.open(QIODevice::ReadOnly), "Open real regression requests");
      const auto config = QJsonDocument::fromJson(file.readAll()).object();
      AccessScope policy;
      for (auto v : config["input_roots"].toArray()) policy.input_roots.emplace_back(v.toString().toStdWString());
      for (auto v : config["output_roots"].toArray()) policy.output_roots.emplace_back(v.toString().toStdWString());
      const auto journal = config["journal"].toString();
      require(!QFileInfo::exists(journal) && QDir().mkpath(journal), "New regression journal");
      ImageTaskGroups groups(policy, std::filesystem::path(journal.toStdWString()));
      const auto plan = start(groups, config["items"].toArray());
      require(plan["state"] == "ready", "Real batch preflight");
      const auto submitted = checked(groups.request(submission(plan)));
      const auto job = wait(groups, "get_job", submitted["job_id"].toString());
      QFile report(config["report"].toString()); require(report.open(QIODevice::WriteOnly), "Open real regression report");
      report.write(QJsonDocument(job).toJson()); report.close();
      require(job["items"].toArray().size() == config["items"].toArray().size(), "All real items retained");
      require(job["state"] == "succeeded", "All real conversions completed");
      for (auto v : job["items"].toArray()) require(v.toObject()["status"] == "succeeded", "No unfinished real items");
      std::cout << "PASS real batch " << job["total"].toInt() << std::endl; return 0;
    }
    if (argc == 3 && app.arguments()[1] == "--cancelled-recovery") { cancelledRecovery(app.arguments()[2]); return 0; }
    if (argc == 3 && app.arguments()[1] == "--failed-recovery") { failedRecovery(app.arguments()[2]); return 0; }
    if (argc == 3 && app.arguments()[1] == "--direct") { directExport(app.arguments()[2]); return 0; }
    if (argc == 3 && app.arguments()[1] == "--crash-producer") { crashProducer(app.arguments()[2]); return 0; }
    if (argc == 3 && app.arguments()[1] == "--long-journal") {
      const auto root = app.arguments()[2];
      require(!QFileInfo::exists(root) && QDir().mkpath(root), "Create new focused long-journal root");
      fixture(root + "/source.tga"); longJournal(root); return 0;
    }
    require(argc == 2, "Expected external test root");
    QDir().mkpath(QString::fromLocal8Bit(argv[1]));
    QTemporaryDir temp(QString::fromLocal8Bit(argv[1]) + "/groups-XXXXXX");
    require(temp.isValid(), "Create temporary fixtures"); temp.setAutoRemove(false);
    endpointBoundary(temp.path() + "/endpoint-boundary");
    unrelatedJournal(temp.path() + "/unrelated-journal");
    std::cout << "ROOT " << temp.path().toStdString() << std::endl; run(temp.path());
    failedRecovery(temp.path() + "/failed-recovery");
    cancelledRecovery(temp.path() + "/cancelled-recovery");
    killedRecovery(temp.path() + "/crash-recovery"); return 0;
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
