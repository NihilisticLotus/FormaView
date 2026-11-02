#include "formaview/render_item.hpp"
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickWindow>
#include <QThread>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>

using namespace formaview::render;
namespace {
void quad(Frame &f, float z, QVector3D colour, float alpha, int mode,
          float u = .25F, float v = .25F) {
  for (const auto &p : {QPointF(-1, -1), QPointF(1, -1), QPointF(-1, 1),
                        QPointF(-1, 1), QPointF(1, -1), QPointF(1, 1)})
    f.vertices.push_back({static_cast<float>(p.x()), static_cast<float>(p.y()),
                          z, u, v, 0, .4F, .7F, 1, colour.x(), colour.y(),
                          colour.z(), alpha, static_cast<float>(mode), .5F});
}
struct Result {
  std::array<float, 4> rgba;
  std::uint64_t owned{};
  int gridPixels{}, axisPixels{};
};
void rejectedFrame(const std::shared_ptr<Frame> &frame) {
  QQuickWindow window;
  window.setFlags(window.flags() | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
  window.resize(128, 128);
  auto item = std::make_unique<Item>(window.contentItem());
  item->setSize({128, 128});
  struct State {
    std::atomic<int> failures{};
    std::atomic<bool> readbackFailed{}, recovered{};
  };
  const auto state = std::make_shared<State>();
  item->frameFailure = [state](QString reason, std::shared_ptr<const Frame>) {
    QMetaObject::invokeMethod(QCoreApplication::instance(), [state, reason] {
      if (!reason.isEmpty()) ++state->failures;
    }, Qt::QueuedConnection);
  };
  item->readback = [state](QByteArray bytes, QSize size) {
    state->readbackFailed = bytes.isEmpty() && size.isEmpty();
  };
  const auto wait = [&](const auto &ready) {
    QElapsedTimer timer; timer.start();
    while (!ready() && timer.elapsed() < 10000) {
      QCoreApplication::processEvents(); QThread::msleep(1);
    }
    if (!ready()) throw std::runtime_error("Frame rejection/recovery timed out");
  };
  item->setFrame(frame); window.show();
  wait([&] { return state->failures == 1 && state->readbackFailed; });
  auto valid = std::make_shared<Frame>();
  valid->rgba = {1, 1, 1, 1}; valid->revision = frame->revision + 1;
  quad(*valid, 0, {1, 0, 0}, 1, 0);
  item->frameSubmitted = [state, valid](auto submitted) { state->recovered = submitted == valid; };
  item->setFrame(valid);
  wait([&] { return state->recovered.load(); });
  if (state->failures != 1) throw std::runtime_error("Repeated frame failure notification");
  item.reset(); window.hide(); window.releaseResources();
}
Result render(const std::shared_ptr<Frame> &frame, bool grid = false,
              bool axes = false) {
  auto window = std::make_unique<QQuickWindow>();
  window->setFlags(window->flags() | Qt::WindowDoesNotAcceptFocus |
                   Qt::WindowTransparentForInput);
  window->setTitle("FormaView isolated material readback");
  window->resize(256, 256);
  window->setPersistentGraphics(false);
  window->setPersistentSceneGraph(false);
  auto item = std::make_unique<Item>(window->contentItem());
  item->setSize({256, 256});
  item->setFixedColorBufferWidth(128);
  item->setFixedColorBufferHeight(128);
  item->showGrid = grid;
  item->showAxes = axes;
  struct ReadbackState {
    Result result{};
    std::atomic<bool> done = false;
    std::atomic<std::uint64_t> owned = 0;
  };
  const auto state = std::make_shared<ReadbackState>();
  item->observe = [state](Observation value) {
    state->owned.store(value.owned_gpu_bytes);
  };
  item->readback = [state](const QByteArray &bytes, QSize size) {
    if (size.isValid() && !size.isEmpty() &&
        bytes.size() == size.width() * size.height() * 16) {
      const auto at =
          (size.height() / 2 * size.width() + size.width() / 2) * 16;
      std::memcpy(state->result.rgba.data(), bytes.constData() + at, 16);
      for (qsizetype pixel = 0; pixel < bytes.size(); pixel += 16) {
        std::array<float, 4> value;
        std::memcpy(value.data(), bytes.constData() + pixel, 16);
        if (std::abs(value[0] - .25F) < 1e-4F &&
            std::abs(value[1] - .5F) < 1e-4F &&
            std::abs(value[2] - .75F) < 1e-4F)
          ++state->result.gridPixels;
        if (std::abs(value[0] - .75F) < 1e-4F &&
            std::abs(value[1] - .5F) < 1e-4F &&
            std::abs(value[2] - .25F) < 1e-4F)
          ++state->result.axisPixels;
      }
    } else
      state->result.rgba.fill(-999);
    state->done.store(true);
  };
  item->setFrame(frame);
  window->show();
  QElapsedTimer deadline;
  deadline.start();
  while (!state->done.load() && deadline.elapsed() < 10000) {
    QCoreApplication::processEvents();
    QThread::msleep(1);
  }
  if (!state->done.load())
    throw std::runtime_error("GPU readback timeout");
  const Result result{state->result.rgba, state->owned.load(),
                      state->result.gridPixels, state->result.axisPixels};
  item.reset();
  window->hide();
  window->releaseResources();
  window.reset();
  QCoreApplication::processEvents();
  return result;
}
struct ResidencyResult {
  std::vector<Observation> stages;
  std::array<float, 4> rgba{};
  Observation expired, cleared;
};
QJsonObject idleRendering(const std::shared_ptr<Frame> &frame, bool empty) {
  QQuickWindow window;
  window.setFlags(window.flags() | Qt::WindowDoesNotAcceptFocus |
                  Qt::WindowTransparentForInput);
  window.resize(256, 256);
  auto item = std::make_unique<Item>(window.contentItem());
  item->setSize({256, 256});
  const auto frames = std::make_shared<std::atomic<int>>(0);
  const auto callbacks = std::make_shared<std::atomic<int>>(0);
  const auto failure = std::make_shared<std::atomic<bool>>(false);
  item->observe = [frames](Observation value) {
    if (!value.released)
      ++*frames;
  };
  if (empty)
    item->readback = [callbacks, failure](QByteArray bytes, QSize size) {
      ++*callbacks;
      failure->store(bytes.isEmpty() && size.isEmpty());
    };
  item->setFrame(empty ? std::make_shared<Frame>() : frame);
  window.show();
  const auto settle = [](int duration) {
    QElapsedTimer time;
    time.start();
    while (time.elapsed() < duration) {
      QCoreApplication::processEvents();
      QThread::msleep(1);
    }
  };
  settle(300);
  const auto before = frames->load();
  settle(500);
  const auto after = frames->load();
  if (before < 1 || after > before + 1 ||
      (empty && (callbacks->load() != 1 || !failure->load())))
    throw std::runtime_error("Static renderer did not settle or empty readback "
                             "failed to report once");
  item.reset();
  window.hide();
  window.releaseResources();
  return {
      {"case", empty ? "empty readback failure and idle" : "static model idle"},
      {"before_frames", before},
      {"after_frames", after},
      {"idle_ms", 500},
      {"failure_callbacks", callbacks->load()}};
}
ResidencyResult residency(std::vector<std::shared_ptr<Frame>> frames) {
  QQuickWindow window;
  window.setFlags(window.flags() | Qt::WindowDoesNotAcceptFocus |
                  Qt::WindowTransparentForInput);
  window.setPersistentGraphics(false);
  window.setPersistentSceneGraph(false);
  window.resize(256, 256);
  auto item = std::make_unique<Item>(window.contentItem());
  item->setSize({256, 256});
  item->setFixedColorBufferWidth(128);
  item->setFixedColorBufferHeight(128);
  struct Shared {
    std::mutex mutex;
    ResidencyResult result;
    std::atomic<int> stage{-1};
    std::atomic<bool> read{};
  };
  const auto state = std::make_shared<Shared>();
  state->result.stages.resize(frames.size());
  const auto wait = [&](const std::function<bool()> &ready) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 10000) {
      QCoreApplication::processEvents();
      QThread::msleep(1);
    }
    if (!ready())
      throw std::runtime_error("GPU residency transition timeout");
  };
  window.show();
  for (int stage = 0; stage < static_cast<int>(frames.size()); ++stage) {
    item->observe = [state, stage](Observation value) {
      std::lock_guard guard(state->mutex);
      auto &entry = state->result.stages.at(static_cast<std::size_t>(stage));
      const auto hit = entry.resident_hit || value.resident_hit;
      const auto bytes = entry.uploaded_bytes + value.uploaded_bytes;
      const auto residents =
          std::max(entry.resident_frames, value.resident_frames);
      const auto owned = std::max(entry.owned_gpu_bytes, value.owned_gpu_bytes);
      entry = value;
      entry.resident_hit = hit;
      entry.uploaded_bytes = bytes;
      entry.resident_frames = residents;
      entry.owned_gpu_bytes = owned;
      state->stage.store(stage);
    };
    if (stage + 1 == static_cast<int>(frames.size()))
      item->readback = [state](const QByteArray &bytes, QSize size) {
        const auto at =
            (size.height() / 2 * size.width() + size.width() / 2) * 16;
        if (!size.isValid() || size.isEmpty() ||
            bytes.size() != size.width() * size.height() * 16)
          return;
        std::memcpy(state->result.rgba.data(), bytes.constData() + at, 16);
        state->read.store(true);
      };
    item->setFrame(frames.at(static_cast<std::size_t>(stage)));
    wait([&] { return state->stage.load() >= stage; });
  }
  wait([&] { return state->read.load(); });
  frames.clear();
  item->observe = [state](Observation value) {
    std::lock_guard guard(state->mutex);
    state->result.expired = value;
    state->stage.store(1000);
  };
  item->update();
  wait([&] { return state->stage.load() == 1000; });
  item->observe = [state](Observation value) {
    if (value.released)
      return;
    std::lock_guard guard(state->mutex);
    state->result.cleared = value;
    state->stage.store(1001);
  };
  item->setFrame(nullptr);
  wait([&] { return state->stage.load() == 1001; });
  item.reset();
  window.hide();
  window.releaseResources();
  std::lock_guard guard(state->mutex);
  return state->result;
}
} // namespace
int main(int argc, char **argv) {
  QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
  QGuiApplication app(argc, argv);
  if (app.arguments().size() != 2)
    return 64;
  QJsonArray records;
  int assertions = 0;
  const auto check = [&](const QString &name, std::shared_ptr<Frame> frame,
                         std::array<float, 4> expected) {
    const auto actual = render(frame);
    for (std::size_t i = 0; i < 4; ++i) {
      ++assertions;
      if (!std::isfinite(actual.rgba[i]) ||
          std::abs(actual.rgba[i] - expected[i]) > 3e-4F)
        throw std::runtime_error(
            (name + QString(" channel %1 actual %2 expected %3")
                        .arg(i)
                        .arg(actual.rgba[i], 0, 'g', 9)
                        .arg(expected[i], 0, 'g', 9))
                .toStdString());
    }
    records.append(QJsonObject{
        {"case", name},
        {"rgba", QJsonArray{actual.rgba[0], actual.rgba[1], actual.rgba[2],
                            actual.rgba[3]}},
        {"shader_resource_bytes", static_cast<qint64>(actual.owned)}});
    if (!frame->textures.empty()) {
      ++assertions;
      if (actual.owned < frame->vertices.size() * sizeof(Vertex) + 16 + 64 +
                             renderer_uniform_bytes)
        throw std::runtime_error(
            "Extra model texture omitted from owned GPU resource accounting");
    }
  };
  const auto base = [] {
    auto f = std::make_shared<Frame>();
    f->revision = 1;
    f->rgba = {1, 1, 1, 1};
    f->lit = true;
    return f;
  };
  constexpr float background = 24.F / 255;
  try {
    for (int kind = 0; kind < 4; ++kind) {
      auto invalid = std::make_shared<Frame>();
      invalid->rgba = {1, 1, 1, 1};
      quad(*invalid, 0, {1, 0, 0}, 1, 0);
      if (kind == 0) invalid->rgba.pop_back();
      if (kind == 1) invalid->guides.resize(1);
      if (kind == 2) invalid->textures.push_back(std::make_shared<Texture>());
      if (kind == 3) invalid->backgroundVertices = 6;
      rejectedFrame(invalid);
      ++assertions;
    }
    records.append(QJsonObject{{"case", "invalid upload/guide/texture/background rejection and recovery"}, {"cases", 4}});
    auto f = base();
    quad(*f, 0, {1, 0, 0}, .25F, 0);
    check("OPAQUE ignores coverage", f, {1, 0, 0, 1});
    f = base();
    quad(*f, 0, {1, 0, 0}, .25F, 1);
    check("MASK discards below cutoff", f,
          {background, background, background, 1});
    f = base();
    quad(*f, 0, {1, 0, 0}, .75F, 1);
    check("MASK survives with opaque coverage", f, {1, 0, 0, 1});
    f = base();
    quad(*f, 0, {1, 0, 0}, .5F, 2);
    f->draws = {{0, 6, -1, true, {}}};
    check("BLEND over opaque clear", f,
          {.5F + .5F * background, .5F * background, .5F * background, 1});
    f = base();
    quad(*f, -.5F, {1, 0, 0}, .5F, 2);
    quad(*f, .5F, {0, 0, 1}, 1, 0);
    f->draws = {{0, 6, -1, true, {0, 0, -.5F}}, {6, 6, -1, false, {0, 0, .5F}}};
    check("Opaque before transparent depth", f, {.5F, 0, .5F, 1});
    f = base();
    quad(*f, -.5F, {1, 0, 0}, .5F, 2);
    quad(*f, .5F, {0, 1, 0}, .5F, 2);
    f->draws = {{0, 6, -1, true, {0, 0, -.5F}}, {6, 6, -1, true, {0, 0, .5F}}};
    check("Transparent ranges sorted far to near", f,
          {.5F + .25F * background, .25F + .25F * background, .25F * background,
           1});
    f = base();
    f->rgba = {.5F, .5F, .5F, 1};
    f->linearColour = true;
    quad(*f, 0, {.5F, .5F, .5F}, 1, 0);
    check("sRGB texture times linear factor", f,
          {.3607802F, .3607802F, .3607802F, 1});
    for (int wrap = 0; wrap < 3; ++wrap) {
      f = base();
      quad(*f, 0, {1, 1, 1}, 1, 0, 1.25F, .25F);
      auto texture = std::make_shared<Texture>();
      texture->width = texture->height = 2;
      texture->rgba = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1};
      texture->wrapU = wrap;
      f->textures = {texture};
      f->draws = {{0, 6, 0, false, {}}};
      check(QString("Texture wrap %1").arg(wrap), f,
            wrap == 0 ? std::array<float, 4>{1, 0, 0, 1}
                      : std::array<float, 4>{0, 1, 0, 1});
    }
    f = base();
    f->uv = true;
    f->lit = false;
    quad(*f, 0, {.25F, .5F, .75F}, 1, -1);
    check("UV guide uses explicit display tint", f, {.25F, .5F, .75F, 1});
    f = base();
    f->uv = true;
    f->lit = false;
    f->lines = true;
    quad(*f, .5F, {1, 1, 1}, 1, 0);
    auto uvTexture = std::make_shared<Texture>();
    uvTexture->width = uvTexture->height = 2;
    uvTexture->rgba = {1, 0, 0, .2F, 0, 1, 0, .2F, 0, 0, 1, .2F, 1, 1, 1, .2F};
    f->textures = {uvTexture};
    f->backgroundTexture = 0;
    f->backgroundVertices = 6;
    check("UV RGB background uses triangle pipeline without lighting", f,
          {1, 0, 0, 1});
    {
      auto guideFrame = base();
      quad(*guideFrame, .5F, {1, 0, 0}, 1, 0);
      for (const auto p : {QVector3D(-1, 0, -.5F), QVector3D(1, 0, -.5F),
                           QVector3D(0, -1, -.5F), QVector3D(0, 1, -.5F)}) {
        const bool gridLine = guideFrame->guides.size() < 2;
        guideFrame->guides.push_back({p.x(), p.y(), p.z(), 0, 0, 0, 0, 1, 0,
                                      gridLine ? .25F : .75F, .5F,
                                      gridLine ? .75F : .25F, 1, -2, .5F});
      }
      guideFrame->gridVertices = 2;
      const auto hidden = render(guideFrame),
                 grid = render(guideFrame, true, false),
                 axes = render(guideFrame, false, true);
      ++assertions;
      if (hidden.gridPixels || hidden.axisPixels || !grid.gridPixels ||
          grid.axisPixels || axes.gridPixels || !axes.axisPixels)
        throw std::runtime_error(
            "Ground grid and origin axes must switch independently without "
            "changing shaded source geometry");
    }
    {
      auto selected = base();
      quad(*selected, -.5F, {1, 0, 0}, 1, 0);
      selected->guides = {
          {-1, 0, .5F, 0, 0, 0, 0, 1, 0, .75F, .5F, .25F, 1, -2, .5F},
          {1, 0, .5F, 0, 0, 0, 0, 1, 0, .75F, .5F, .25F, 1, -2, .5F}};
      selected->selectionVertices = 2;
      const auto result = render(selected, false, false);
      ++assertions;
      if (!result.axisPixels)
        throw std::runtime_error("Selected skeleton segment remains visible "
                                 "with both reference guides disabled");
    }
    const auto cached = [&](QVector3D colour) {
      auto frame = base();
      frame->cacheable = true;
      quad(*frame, 0, colour, 1, 0);
      return frame;
    };
    {
      f->cacheable = true;
      const auto result = residency({f, cached({0, 0, 1}), f});
      ++assertions;
      if (!result.stages[2].resident_hit || result.stages[2].uploaded_bytes ||
          result.rgba != std::array<float, 4>{1, 0, 0, 1})
        throw std::runtime_error(
            "Resident UV background must restore texture and triangle/line "
            "pipelines after a 3D frame");
    }
    {
      auto red = cached({1, 0, 0}), blue = cached({0, 0, 1});
      const auto result = residency({red, blue, red});
      records.append(QJsonObject{
          {"case", "resident A/B/A and clear"},
          {"resident_hit", result.stages[2].resident_hit},
          {"uploaded_bytes", static_cast<qint64>(result.stages[2].uploaded_bytes)},
          {"resident_frames", static_cast<int>(result.stages[2].resident_frames)},
          {"rgba", QJsonArray{result.rgba[0], result.rgba[1], result.rgba[2], result.rgba[3]}},
          {"clear_owned_bytes", static_cast<qint64>(result.cleared.owned_gpu_bytes)},
          {"clear_resident_frames", static_cast<int>(result.cleared.resident_frames)}});
      ++assertions;
      if (!result.stages[2].resident_hit || result.stages[2].uploaded_bytes ||
          result.stages[2].resident_frames != 2 ||
          result.rgba != std::array<float, 4>{1, 0, 0, 1} ||
          result.cleared.owned_gpu_bytes != renderer_uniform_bytes ||
          result.cleared.resident_frames)
        throw std::runtime_error(
            (QString("Resident A/B/A must reuse GPU objects and display exact A "
                     "despite equal source revision: hit=%1 uploaded=%2 "
                     "resident=%3 rgba=[%4,%5,%6,%7] clearOwned=%8 "
                     "clearResident=%9")
                 .arg(result.stages[2].resident_hit)
                 .arg(result.stages[2].uploaded_bytes)
                 .arg(result.stages[2].resident_frames)
                 .arg(result.rgba[0], 0, 'g', 9)
                 .arg(result.rgba[1], 0, 'g', 9)
                 .arg(result.rgba[2], 0, 'g', 9)
                 .arg(result.rgba[3], 0, 'g', 9)
                 .arg(result.cleared.owned_gpu_bytes)
                 .arg(result.cleared.resident_frames))
                .toStdString());
    }
    {
      std::vector<std::shared_ptr<Frame>> sequence;
      for (const auto colour :
           {QVector3D(1, 0, 0), QVector3D(0, 1, 0), QVector3D(0, 0, 1),
            QVector3D(1, 1, 0), QVector3D(0, 1, 1)})
        sequence.push_back(cached(colour));
      const auto result = residency(std::move(sequence));
      ++assertions;
      if (result.stages[4].resident_frames != 4 ||
          result.stages[4].owned_gpu_bytes >
              256ULL * 1024 * 1024 + renderer_uniform_bytes ||
          result.expired.resident_frames != 1 ||
          result.cleared.owned_gpu_bytes != renderer_uniform_bytes)
        throw std::runtime_error(
            (QString("GPU LRU count/budget and expired-source/clear release "
                     "failed: count=%1 owned=%2 expired=%3 clear=%4")
                 .arg(result.stages[4].resident_frames)
                 .arg(result.stages[4].owned_gpu_bytes)
                 .arg(result.expired.resident_frames)
                 .arg(result.cleared.owned_gpu_bytes))
                .toStdString());
    }
    records.append(idleRendering(cached({1, 0, 0}), false));
    ++assertions;
    records.append(idleRendering({}, true));
    ++assertions;
  } catch (const std::exception &e) {
    QFile output(app.arguments()[1]);
    if (output.open(QIODevice::WriteOnly))
      output.write(QJsonDocument(QJsonObject{{"qt", qVersion()},
          {"backend", "D3D11"}, {"cases", records},
          {"assertions", assertions}, {"failure", QString::fromUtf8(e.what())}}).toJson());
    std::cerr << "FAILED after " << assertions << " assertions: " << e.what()
              << '\n';
    return 1;
  }
  QFile output(app.arguments()[1]);
  if (!output.open(QIODevice::WriteOnly))
    return 2;
  output.write(QJsonDocument(QJsonObject{{"qt", qVersion()},
                                         {"backend", "D3D11"},
                                         {"cases", records},
                                         {"assertions", assertions}})
                   .toJson());
  std::cout << "PASSED " << assertions << " actual GPU material assertions\n";
}
