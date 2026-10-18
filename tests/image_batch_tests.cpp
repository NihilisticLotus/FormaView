#include "formaview/image_batch.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QUuid>
#include <chrono>
#include <io.h>
#include <iostream>
#include <sstream>
#include <thread>
#include <windows.h>

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
}
} // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
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
