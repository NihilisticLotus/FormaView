#include "formaview/render_item.hpp"
#include <QFile>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <list>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <wrl/client.h>

namespace formaview::render {
namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}
QShader shader(const char *name) {
  QFile file(QString::fromLatin1(name));
  if (!file.open(QIODevice::ReadOnly))
    qFatal("Shader unavailable");
  auto s = QShader::fromSerialized(file.readAll());
  if (!s.isValid())
    qFatal("Invalid shader");
  return s;
}
std::uint64_t gpu_usage(QRhi *rhi) {
  if (rhi->backend() != QRhi::D3D11)
    return 0;
  const auto *handles =
      static_cast<const QRhiD3D11NativeHandles *>(rhi->nativeHandles());
  Microsoft::WRL::ComPtr<IDXGIDevice> device;
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
  if (FAILED(static_cast<ID3D11Device *>(handles->dev)
                 ->QueryInterface(IID_PPV_ARGS(&device))) ||
      FAILED(device->GetAdapter(&adapter)) || FAILED(adapter.As(&adapter3)))
    return 0;
  DXGI_QUERY_VIDEO_MEMORY_INFO info{};
  if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                                            &info)))
    return 0;
  return info.CurrentUsage;
}
class Renderer final : public QQuickRhiItemRenderer {
public:
  ~Renderer() override {
    residentCache.clear();
    guidePipeline.reset();
    selectionPipeline.reset();
    guideVertices.reset();
    extraTextures.clear();
    pipeline.reset();
    blendedPipeline.reset();
    uvBackgroundPipeline.reset();
    bindings.reset();
    texture.reset();
    sampler.reset();
    vertices.reset();
    uniform.reset();
    if (observe)
      observe({0, 0, 0, 0, owner ? gpu_usage(owner) : 0, true});
  }
  void synchronize(QQuickRhiItem *item) override {
    const auto *source = static_cast<Item *>(item);
    payload = source->frame;
    viewMatrix = source->useViewMatrix
                     ? source->viewMatrix
                     : (payload ? payload->matrix : QMatrix4x4{});
    showGrid = source->showGrid;
    showAxes = source->showAxes;
    observe = source->observe;
    if (!requested && source->readback) {
      readback = source->readback;
      requested = true;
    }
  }
  void initialize(QRhiCommandBuffer *) override {
    if (owner != rhi()) {
      residentCache.clear();
      guidePipeline.reset();
      selectionPipeline.reset();
      guideVertices.reset();
      uploadedFrame.reset();
      activeBytes = 0;
      extraTextures.clear();
      pipeline.reset();
      blendedPipeline.reset();
      uvBackgroundPipeline.reset();
      bindings.reset();
      texture.reset();
      sampler.reset();
      vertices.reset();
      uniform.reset();
      owner = rhi();
      revision = ~std::uint64_t{};
    }
    if (!uniform) {
      uniform.reset(
          rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 80));
      if (!uniform->create())
        qFatal("Uniform allocation failed");
    }
  }
  void render(QRhiCommandBuffer *cb) override {
    if (!payload || payload->vertices.empty()) {
      residentCache.clear();
      guidePipeline.reset();
      selectionPipeline.reset();
      guideVertices.reset();
      uploadedFrame.reset();
      activeBytes = 0;
      pipeline.reset();
      blendedPipeline.reset();
      uvBackgroundPipeline.reset();
      bindings.reset();
      texture.reset();
      sampler.reset();
      vertices.reset();
      extraTextures.clear();
      revision = ~std::uint64_t{};
      cb->beginPass(renderTarget(), QColor(28, 28, 30), {1, 0});
      cb->endPass();
      if (observe)
        observe({0, 0, 0, uniform ? 80ULL : 0ULL, gpu_usage(rhi()), false});
      return;
    }
    const auto start = Clock::now();
    std::uint64_t uploadedBytes = 0;
    bool residentHit = false;
    auto *updates = rhi()->nextResourceUpdateBatch();
    if (uploadedFrame.lock() != payload || payload->revision != revision) {
      if (!payload->cacheable)
        residentCache.clear();
      else {
        std::erase_if(residentCache,
                      [](const auto &entry) { return entry.source.expired(); });
        const auto found = std::find_if(
            residentCache.begin(), residentCache.end(), [&](const auto &entry) {
              return entry.source.lock() == payload &&
                     entry.revision == payload->revision;
            });
        Resident restored;
        if (found != residentCache.end()) {
          restored = std::move(*found);
          residentCache.erase(found);
          residentHit = true;
        }
        saveActive();
        if (residentHit)
          restore(std::move(restored));
      }
    }
    if (lineMode != payload->lines) {
      pipeline.reset();
      blendedPipeline.reset();
      uvBackgroundPipeline.reset();
      lineMode = payload->lines;
    }
    if (uploadedFrame.lock() != payload || payload->revision != revision) {
      const auto bytes = payload->vertices.size() * sizeof(Vertex);
      uploadedBytes = bytes + payload->rgba.size() * sizeof(float);
      if (bytes > 256ULL * 1024 * 1024 || payload->width <= 0 ||
          payload->height <= 0 ||
          payload->rgba.size() !=
              static_cast<std::size_t>(payload->width) *
                  static_cast<std::size_t>(payload->height) * 4 ||
          payload->rgba.size() > 128ULL * 1024 * 1024)
        qFatal("Render upload budget/shape rejected");
      if (!vertices || vertices->size() != bytes) {
        vertices.reset(rhi()->newBuffer(QRhiBuffer::Dynamic,
                                        QRhiBuffer::VertexBuffer,
                                        static_cast<quint32>(bytes)));
        if (!vertices->create())
          qFatal("Vertex allocation failed");
      }
      updates->updateDynamicBuffer(vertices.get(), 0,
                                   static_cast<quint32>(bytes),
                                   payload->vertices.data());
      if (payload->guides.empty()) {
        guidePipeline.reset();
        selectionPipeline.reset();
        guideVertices.reset();
      } else {
        const auto guideBytes = payload->guides.size() * sizeof(Vertex);
        if (guideBytes > 1024 * 1024 ||
            payload->gridVertices +
                    static_cast<std::uint64_t>(payload->selectionVertices) >
                payload->guides.size() ||
            payload->selectionVertices % 2 || payload->gridVertices % 2 ||
            payload->guides.size() % 2)
          qFatal("Invalid model guide geometry");
        if (!guideVertices || guideVertices->size() != guideBytes) {
          guideVertices.reset(
              rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer,
                               static_cast<quint32>(guideBytes)));
          if (!guideVertices->create())
            qFatal("Guide allocation failed");
        }
        updates->updateDynamicBuffer(guideVertices.get(), 0,
                                     static_cast<quint32>(guideBytes),
                                     payload->guides.data());
        uploadedBytes += guideBytes;
      }
      const QSize size(payload->width, payload->height);
      if (!texture || texture->pixelSize() != size) {
        guidePipeline.reset();
        selectionPipeline.reset();
        pipeline.reset();
        blendedPipeline.reset();
        uvBackgroundPipeline.reset();
        bindings.reset();
        texture.reset(rhi()->newTexture(QRhiTexture::RGBA32F, size));
        if (!texture->create())
          qFatal("Float texture unavailable");
        if (!sampler) {
          sampler.reset(rhi()->newSampler(
              QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
              QRhiSampler::Repeat, QRhiSampler::Repeat));
          if (!sampler->create())
            qFatal("Sampler failed");
        }
        bindings.reset(rhi()->newShaderResourceBindings());
        bindings->setBindings({QRhiShaderResourceBinding::uniformBuffer(
                                   0,
                                   QRhiShaderResourceBinding::VertexStage |
                                       QRhiShaderResourceBinding::FragmentStage,
                                   uniform.get()),
                               QRhiShaderResourceBinding::sampledTexture(
                                   1, QRhiShaderResourceBinding::FragmentStage,
                                   texture.get(), sampler.get())});
        if (!bindings->create())
          qFatal("Bindings failed");
      }
      QRhiTextureSubresourceUploadDescription upload(
          payload->rgba.data(),
          static_cast<quint32>(payload->rgba.size() * sizeof(float)));
      updates->uploadTexture(
          texture.get(),
          QRhiTextureUploadDescription({QRhiTextureUploadEntry(0, 0, upload)}));
      extraTextures.clear();
      for (const auto &src : payload->textures) {
        uploadedBytes += src->rgba.size() * sizeof(float);
        if (!src || src->width <= 0 || src->height <= 0 ||
            src->rgba.size() != static_cast<std::size_t>(src->width) *
                                    static_cast<std::size_t>(src->height) * 4)
          qFatal("Model texture shape rejected");
        auto extra = std::make_unique<ExtraTexture>();
        extra->texture.reset(rhi()->newTexture(
            QRhiTexture::RGBA32F, {src->width, src->height}, 1,
            src->mipmaps
                ? QRhiTexture::MipMapped | QRhiTexture::UsedWithGenerateMips
                : QRhiTexture::Flags{}));
        if (!extra->texture->create())
          qFatal("Model texture allocation failed");
        const auto wrap = [](int mode) {
          return mode == 1   ? QRhiSampler::ClampToEdge
                 : mode == 2 ? QRhiSampler::Mirror
                             : QRhiSampler::Repeat;
        };
        extra->sampler.reset(rhi()->newSampler(
            src->linearMag ? QRhiSampler::Linear : QRhiSampler::Nearest,
            src->linearMin ? QRhiSampler::Linear : QRhiSampler::Nearest,
            src->mipmaps
                ? (src->linearMip ? QRhiSampler::Linear : QRhiSampler::Nearest)
                : QRhiSampler::None,
            wrap(src->wrapU), wrap(src->wrapV)));
        if (!extra->sampler->create())
          qFatal("Model sampler allocation failed");
        extra->bindings.reset(rhi()->newShaderResourceBindings());
        extra->bindings->setBindings(
            {QRhiShaderResourceBinding::uniformBuffer(
                 0,
                 QRhiShaderResourceBinding::VertexStage |
                     QRhiShaderResourceBinding::FragmentStage,
                 uniform.get()),
             QRhiShaderResourceBinding::sampledTexture(
                 1, QRhiShaderResourceBinding::FragmentStage,
                 extra->texture.get(), extra->sampler.get())});
        if (!extra->bindings->create())
          qFatal("Model texture bindings failed");
        QRhiTextureSubresourceUploadDescription pixels(
            src->rgba.data(),
            static_cast<quint32>(src->rgba.size() * sizeof(float)));
        updates->uploadTexture(extra->texture.get(),
                               QRhiTextureUploadDescription(
                                   {QRhiTextureUploadEntry(0, 0, pixels)}));
        if (src->mipmaps)
          updates->generateMips(extra->texture.get());
        extraTextures.push_back(std::move(extra));
      }
      revision = payload->revision;
      uploadedFrame = payload;
      activeBytes = bytes + payload->rgba.size() * sizeof(float) +
                    payload->guides.size() * sizeof(Vertex);
      for (const auto &src : payload->textures) {
        auto w = src->width, h = src->height;
        do {
          activeBytes += static_cast<std::uint64_t>(w) * h * 16;
          if (!src->mipmaps || (w == 1 && h == 1))
            break;
          w = std::max(w / 2, 1);
          h = std::max(h / 2, 1);
        } while (true);
      }
    }
    trimResidents();
    if (!pipeline) {
      pipeline.reset(rhi()->newGraphicsPipeline());
      pipeline->setShaderStages(
          {{QRhiShaderStage::Vertex, shader(":/shaders/preview.vert.qsb")},
           {QRhiShaderStage::Fragment, shader(":/shaders/preview.frag.qsb")}});
      QRhiVertexInputLayout layout;
      layout.setBindings({QRhiVertexInputBinding(sizeof(Vertex))});
      layout.setAttributes(
          {QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, 0),
           QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2, 12),
           QRhiVertexInputAttribute(0, 2, QRhiVertexInputAttribute::Float, 20),
           QRhiVertexInputAttribute(0, 3, QRhiVertexInputAttribute::Float3, 24),
           QRhiVertexInputAttribute(0, 4, QRhiVertexInputAttribute::Float3, 36),
           QRhiVertexInputAttribute(0, 5, QRhiVertexInputAttribute::Float3,
                                    48)});
      pipeline->setVertexInputLayout(layout);
      pipeline->setShaderResourceBindings(bindings.get());
      pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
      pipeline->setSampleCount(renderTarget()->sampleCount());
      pipeline->setDepthTest(true);
      pipeline->setDepthWrite(true);
      pipeline->setTopology(lineMode ? QRhiGraphicsPipeline::Lines
                                     : QRhiGraphicsPipeline::Triangles);
      if (!pipeline->create())
        qFatal("Pipeline failed");
    }
    if (payload->backgroundVertices && !uvBackgroundPipeline) {
      uvBackgroundPipeline.reset(rhi()->newGraphicsPipeline());
      uvBackgroundPipeline->setShaderStages(
          {{QRhiShaderStage::Vertex, shader(":/shaders/preview.vert.qsb")},
           {QRhiShaderStage::Fragment, shader(":/shaders/preview.frag.qsb")}});
      uvBackgroundPipeline->setVertexInputLayout(pipeline->vertexInputLayout());
      uvBackgroundPipeline->setShaderResourceBindings(bindings.get());
      uvBackgroundPipeline->setRenderPassDescriptor(
          renderTarget()->renderPassDescriptor());
      uvBackgroundPipeline->setSampleCount(renderTarget()->sampleCount());
      uvBackgroundPipeline->setDepthTest(true);
      uvBackgroundPipeline->setDepthWrite(true);
      uvBackgroundPipeline->setTopology(QRhiGraphicsPipeline::Triangles);
      if (!uvBackgroundPipeline->create())
        qFatal("UV background pipeline failed");
    }
    if (guideVertices && !guidePipeline) {
      guidePipeline.reset(rhi()->newGraphicsPipeline());
      guidePipeline->setShaderStages(
          {{QRhiShaderStage::Vertex, shader(":/shaders/preview.vert.qsb")},
           {QRhiShaderStage::Fragment, shader(":/shaders/preview.frag.qsb")}});
      guidePipeline->setVertexInputLayout(pipeline->vertexInputLayout());
      guidePipeline->setShaderResourceBindings(bindings.get());
      guidePipeline->setRenderPassDescriptor(
          renderTarget()->renderPassDescriptor());
      guidePipeline->setSampleCount(renderTarget()->sampleCount());
      guidePipeline->setDepthTest(true);
      guidePipeline->setDepthWrite(false);
      guidePipeline->setTopology(QRhiGraphicsPipeline::Lines);
      if (!guidePipeline->create())
        qFatal("Guide pipeline failed");
    }
    if (guideVertices && payload->selectionVertices && !selectionPipeline) {
      selectionPipeline.reset(rhi()->newGraphicsPipeline());
      selectionPipeline->setShaderStages(
          {{QRhiShaderStage::Vertex, shader(":/shaders/preview.vert.qsb")},
           {QRhiShaderStage::Fragment, shader(":/shaders/preview.frag.qsb")}});
      selectionPipeline->setVertexInputLayout(pipeline->vertexInputLayout());
      selectionPipeline->setShaderResourceBindings(bindings.get());
      selectionPipeline->setRenderPassDescriptor(
          renderTarget()->renderPassDescriptor());
      selectionPipeline->setSampleCount(renderTarget()->sampleCount());
      selectionPipeline->setDepthTest(false);
      selectionPipeline->setDepthWrite(false);
      selectionPipeline->setTopology(QRhiGraphicsPipeline::Lines);
      if (!selectionPipeline->create())
        qFatal("Skeleton selection pipeline failed");
    }
    if (!blendedPipeline &&
        std::any_of(payload->draws.begin(), payload->draws.end(),
                    [](const auto &draw) { return draw.blend; })) {
      blendedPipeline.reset(rhi()->newGraphicsPipeline());
      blendedPipeline->setShaderStages(
          {{QRhiShaderStage::Vertex, shader(":/shaders/preview.vert.qsb")},
           {QRhiShaderStage::Fragment, shader(":/shaders/preview.frag.qsb")}});
      blendedPipeline->setVertexInputLayout(pipeline->vertexInputLayout());
      blendedPipeline->setShaderResourceBindings(bindings.get());
      blendedPipeline->setRenderPassDescriptor(
          renderTarget()->renderPassDescriptor());
      blendedPipeline->setSampleCount(renderTarget()->sampleCount());
      blendedPipeline->setDepthTest(true);
      blendedPipeline->setDepthWrite(false);
      QRhiGraphicsPipeline::TargetBlend blend;
      blend.enable = true;
      blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
      blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
      blendedPipeline->setTargetBlends({blend});
      if (!blendedPipeline->create())
        qFatal("Transparent pipeline failed");
    }
    std::array<float, 20> u{};
    const auto matrix = rhi()->clipSpaceCorrMatrix() * viewMatrix;
    std::memcpy(u.data(), matrix.constData(), 64);
    u[16] = payload->heat ? 1.F : 0.F;
    u[17] = payload->lit ? 1.F : 0.F;
    u[18] = payload->linearColour ? 1.F : 0.F;
    u[19] = payload->uv ? 1.F : 0.F;
    updates->updateDynamicBuffer(uniform.get(), 0, 80, u.data());
    const auto prepare = elapsed(start);
    const auto record = Clock::now();
    cb->beginPass(renderTarget(), QColor(24, 24, 24), {1, 0}, updates);
    cb->setGraphicsPipeline(pipeline.get());
    cb->setViewport({0, 0,
                     static_cast<float>(renderTarget()->pixelSize().width()),
                     static_cast<float>(renderTarget()->pixelSize().height())});
    cb->setShaderResources();
    const QRhiCommandBuffer::VertexInput binding(vertices.get(), 0);
    cb->setVertexInput(0, 1, &binding);
    if (payload->backgroundVertices) {
      if (!payload->uv || !payload->lines || payload->backgroundVertices != 6 ||
          payload->vertices.size() < 6 || payload->backgroundTexture < 0 ||
          static_cast<std::size_t>(payload->backgroundTexture) >=
              extraTextures.size())
        qFatal("Invalid UV background range");
      cb->setGraphicsPipeline(uvBackgroundPipeline.get());
      cb->setShaderResources(
          extraTextures
              .at(static_cast<std::size_t>(payload->backgroundTexture))
              ->bindings.get());
      cb->draw(payload->backgroundVertices);
      cb->setGraphicsPipeline(pipeline.get());
      cb->setShaderResources(bindings.get());
    }
    const auto drawGuides = [&] {
      if (!guideVertices ||
          (!showGrid && !showAxes && !payload->selectionVertices) ||
          payload->uv)
        return;
      cb->setGraphicsPipeline(guidePipeline.get());
      cb->setShaderResources(bindings.get());
      const QRhiCommandBuffer::VertexInput guideBinding(guideVertices.get(), 0);
      cb->setVertexInput(0, 1, &guideBinding);
      if (showGrid && payload->gridVertices)
        cb->draw(payload->gridVertices);
      const auto axes = static_cast<quint32>(payload->guides.size()) -
                        payload->gridVertices - payload->selectionVertices;
      if (showAxes && axes)
        cb->draw(axes, 1, payload->gridVertices);
      if (payload->selectionVertices) {
        cb->setGraphicsPipeline(selectionPipeline.get());
        cb->setShaderResources(bindings.get());
        cb->draw(payload->selectionVertices, 1,
                 static_cast<quint32>(payload->guides.size()) -
                     payload->selectionVertices);
      }
      cb->setGraphicsPipeline(pipeline.get());
      cb->setShaderResources(bindings.get());
      cb->setVertexInput(0, 1, &binding);
    };
    if (payload->draws.empty()) {
      cb->draw(static_cast<quint32>(payload->vertices.size()) -
                   payload->backgroundVertices,
               1, payload->backgroundVertices);
      drawGuides();
    } else {
      const auto drawRange = [&](const DrawRange &draw) {
        cb->setShaderResources(
            draw.texture >= 0
                ? extraTextures.at(static_cast<std::size_t>(draw.texture))
                      ->bindings.get()
                : bindings.get());
        cb->draw(draw.count, 1, draw.first);
      };
      for (const auto &draw : payload->draws)
        if (!draw.blend)
          drawRange(draw);
      drawGuides();
      if (blendedPipeline) {
        std::vector<const DrawRange *> transparent;
        for (const auto &draw : payload->draws)
          if (draw.blend)
            transparent.push_back(&draw);
        std::stable_sort(transparent.begin(), transparent.end(),
                         [&](const auto *a, const auto *b) {
                           return viewMatrix.map(a->center).z() >
                                  viewMatrix.map(b->center).z();
                         });
        cb->setGraphicsPipeline(blendedPipeline.get());
        for (const auto *draw : transparent)
          drawRange(*draw);
      }
    }
    cb->endPass();
    if (readback && !readback_started) {
      readback_started = true;
      result.completed = [this] { readback(result.data, result.pixelSize); };
      auto *batch = rhi()->nextResourceUpdateBatch();
      batch->readBackTexture(QRhiReadbackDescription(colorTexture()), &result);
      cb->resourceUpdate(batch);
    }
    if (observe)
      observe({prepare, elapsed(record), cb->lastCompletedGpuTime(),
               residentBytes() + 80, gpu_usage(rhi()), false, payload->revision,
               uploadedBytes,
               static_cast<std::uint32_t>(residentCache.size() + 1),
               residentHit});
    update();
  }

private:
  struct ExtraTexture {
    std::unique_ptr<QRhiTexture> texture;
    std::unique_ptr<QRhiShaderResourceBindings> bindings;
    std::unique_ptr<QRhiSampler> sampler;
  };
  struct Resident {
    std::weak_ptr<const Frame> source;
    std::uint64_t bytes{}, revision{};
    bool lines{};
    std::unique_ptr<QRhiBuffer> vertices, guides;
    std::unique_ptr<QRhiTexture> texture;
    std::unique_ptr<QRhiSampler> sampler;
    std::unique_ptr<QRhiShaderResourceBindings> bindings;
    std::vector<std::unique_ptr<ExtraTexture>> extraTextures;
    std::unique_ptr<QRhiGraphicsPipeline> pipeline, blended, background, guide,
        selection;
  };
  static constexpr std::uint64_t residentBudget = 256ULL * 1024 * 1024;
  std::list<Resident> residentCache;
  std::weak_ptr<const Frame> uploadedFrame;
  std::uint64_t activeBytes{};
  std::uint64_t residentBytes() const {
    auto totalBytes = activeBytes;
    for (const auto &entry : residentCache)
      totalBytes += entry.bytes;
    return totalBytes;
  }
  void trimResidents() {
    std::erase_if(residentCache,
                  [](const auto &entry) { return entry.source.expired(); });
    while (!residentCache.empty() &&
           (residentCache.size() >= 4 || residentBytes() > residentBudget))
      residentCache.pop_back();
  }
  void saveActive() {
    const auto source = uploadedFrame.lock();
    if (!source || !source->cacheable || !vertices ||
        activeBytes > residentBudget)
      return;
    Resident saved;
    saved.source = source;
    saved.bytes = activeBytes;
    saved.revision = revision;
    saved.lines = lineMode;
    saved.vertices = std::move(vertices);
    saved.guides = std::move(guideVertices);
    saved.texture = std::move(texture);
    saved.sampler = std::move(sampler);
    saved.bindings = std::move(bindings);
    saved.extraTextures = std::move(extraTextures);
    saved.pipeline = std::move(pipeline);
    saved.blended = std::move(blendedPipeline);
    saved.background = std::move(uvBackgroundPipeline);
    saved.guide = std::move(guidePipeline);
    saved.selection = std::move(selectionPipeline);
    residentCache.push_front(std::move(saved));
    uploadedFrame.reset();
    activeBytes = 0;
    revision = ~std::uint64_t{};
  }
  void restore(Resident saved) {
    pipeline = std::move(saved.pipeline);
    guidePipeline = std::move(saved.guide);
    selectionPipeline = std::move(saved.selection);
    blendedPipeline = std::move(saved.blended);
    uvBackgroundPipeline = std::move(saved.background);
    extraTextures = std::move(saved.extraTextures);
    bindings = std::move(saved.bindings);
    sampler = std::move(saved.sampler);
    texture = std::move(saved.texture);
    vertices = std::move(saved.vertices);
    guideVertices = std::move(saved.guides);
    uploadedFrame = saved.source;
    activeBytes = saved.bytes;
    revision = saved.revision;
    lineMode = saved.lines;
  }
  std::vector<std::unique_ptr<ExtraTexture>> extraTextures;
  QRhi *owner{};
  std::shared_ptr<const Frame> payload;
  QMatrix4x4 viewMatrix;
  std::uint64_t revision{~std::uint64_t{}};
  std::unique_ptr<QRhiBuffer> vertices, uniform, guideVertices;
  std::unique_ptr<QRhiTexture> texture;
  std::unique_ptr<QRhiSampler> sampler;
  std::unique_ptr<QRhiShaderResourceBindings> bindings;
  std::unique_ptr<QRhiGraphicsPipeline> pipeline;
  std::unique_ptr<QRhiGraphicsPipeline> blendedPipeline;
  std::unique_ptr<QRhiGraphicsPipeline> uvBackgroundPipeline;
  std::unique_ptr<QRhiGraphicsPipeline> guidePipeline, selectionPipeline;
  bool showGrid{}, showAxes{};
  std::function<void(Observation)> observe;
  std::function<void(QByteArray, QSize)> readback;
  QRhiReadbackResult result;
  bool requested{}, readback_started{};
  bool lineMode{};
};
} // namespace
Item::Item(QQuickItem *parent) : QQuickRhiItem(parent) {
  setAlphaBlending(false);
  setColorBufferFormat(TextureFormat::RGBA32F);
}
void Item::setFrame(std::shared_ptr<const Frame> value) {
  frame = std::move(value);
  update();
}
QQuickRhiItemRenderer *Item::createRenderer() { return new Renderer; }
} // namespace formaview::render
