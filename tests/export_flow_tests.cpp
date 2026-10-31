#include "image_jobs.hpp"
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
#include <cstring>
#include <future>
#include <iostream>
#include <memory>

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
void fixture(const QString &path, int clips) {
  std::unique_ptr<FbxManager, FbxDestroy> manager(FbxManager::Create());
  manager->SetIOSettings(FbxIOSettings::Create(manager.get(), IOSROOT));
  std::unique_ptr<FbxScene, FbxDestroy> scene(FbxScene::Create(manager.get(), "Export flow"));
  auto *node = FbxNode::Create(scene.get(), "Root"); scene->GetRootNode()->AddChild(node);
  for (int i = 0; i < clips; ++i) {
    auto *stack = FbxAnimStack::Create(scene.get(), ("Clip_" + std::to_string(i)).c_str());
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
}

class ExportFlowTests {
public:
  static void run(const QString &root) {
    require(!QFileInfo::exists(root) && QDir().mkpath(root + "/out"), "Use a new external test directory");
    qputenv("FORMAVIEW_IMAGE_STATE_ROOT", (root + "/state").toUtf8());
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
    if (actorFixtures.isEmpty()) { std::cout << "SKIP optional synthetic ActorX fixture regression\n"; return; }
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
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    require(argc == 2, "External evidence root required");
    ExportFlowTests::run(app.arguments()[1]); return 0;
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
