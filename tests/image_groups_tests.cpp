#include "formaview/image_groups.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QUuid>
#include <chrono>
#include <iostream>
#include <set>
#include <thread>
using namespace formaview;
using namespace formaview::imaging;
namespace {
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
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
  for (;;) {
    const auto r = checked(g.request(query(op, id))); const auto s = r["state"].toString();
    if (s != "planning" && s != "running" && s != "queued") return r;
    require(std::chrono::steady_clock::now() < deadline, "Group wait timed out");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}
QJsonObject start(ImageTaskGroups &g, const QJsonArray &items) {
  auto q = query("create_plan"); q["items"] = items;
  return wait(g, "get_plan", checked(g.request(q))["plan_id"].toString());
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
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(8);
  while (!output.contains("READY ") && child.state() != QProcess::NotRunning && std::chrono::steady_clock::now() < deadline) {
    child.waitForReadyRead(100); output += child.readAllStandardOutput();
  }
  if (!output.contains("READY ")) {
    child.kill(); child.waitForFinished(10000);
    throw std::runtime_error((output + child.readAllStandardError()).toStdString());
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
void run(const QString &root) {
  fixture(root + "/source.tga"); QDir().mkpath(root + "/groups");
  {
    ImageTaskGroups groups(scope(root), std::filesystem::path((root + "/groups").toStdWString()));
    for (const int count : {64, 65, 129}) {
      auto plan = start(groups, requests(root, count, QString("count-%1-").arg(count)));
      require(plan["state"] == "ready", "Every batch must preflight");
      require(plan["total"] == count && plan["batch_count"] == (count + 63) / 64, "Group membership/batch count");
      const auto q = submission(plan); const auto r = checked(groups.request(q));
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
    require(groups.request(q).contains("error"), "4097 images rejected atomically");
    q["items"] = requests(root, 4096, "boundary-"); r = checked(groups.request(q));
    q = query("cancel", r["plan_id"].toString()); q["kind"] = "plan"; checked(groups.request(q));
    require(wait(groups, "get_plan", r["plan_id"].toString())["state"] == "cancelled", "Cancel spans all pending child batches");
    std::cout << "PASS 4096 boundary, 4097 rejection, group cancellation" << std::endl;
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
    }
    std::cout << "PASS group journal recovery" << std::endl;
  }
}
}
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    if (argc == 3 && app.arguments()[1] == "--crash-producer") { crashProducer(app.arguments()[2]); return 0; }
    require(argc == 2, "Expected external test root");
    QDir().mkpath(QString::fromLocal8Bit(argv[1]));
    QTemporaryDir temp(QString::fromLocal8Bit(argv[1]) + "/groups-XXXXXX");
    require(temp.isValid(), "Create temporary fixtures"); temp.setAutoRemove(false);
    std::cout << "ROOT " << temp.path().toStdString() << std::endl; run(temp.path());
    killedRecovery(temp.path() + "/crash-recovery"); return 0;
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
