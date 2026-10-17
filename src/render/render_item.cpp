#include "formaview/render_item.hpp"
#include <QFile>
#include <array>
#include <chrono>
#include <cstring>
#include <d3d11.h>
#include <dxgi1_4.h>
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
    pipeline.reset();
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
    observe = source->observe;
    if (!requested && source->readback) {
      readback = source->readback;
      requested = true;
    }
  }
  void initialize(QRhiCommandBuffer *) override {
    if (owner != rhi()) {
      pipeline.reset();
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
    if (!payload || payload->vertices.empty())
      return;
    const auto start = Clock::now();
    auto *updates = rhi()->nextResourceUpdateBatch();
    if (payload->revision != revision) {
      const auto bytes = payload->vertices.size() * sizeof(Vertex);
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
      const QSize size(payload->width, payload->height);
      if (!texture || texture->pixelSize() != size) {
        pipeline.reset();
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
      revision = payload->revision;
    }
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
           QRhiVertexInputAttribute(0, 2, QRhiVertexInputAttribute::Float,
                                    20)});
      pipeline->setVertexInputLayout(layout);
      pipeline->setShaderResourceBindings(bindings.get());
      pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
      pipeline->setSampleCount(renderTarget()->sampleCount());
      pipeline->setDepthTest(true);
      pipeline->setDepthWrite(true);
      if (!pipeline->create())
        qFatal("Pipeline failed");
    }
    std::array<float, 20> u{};
    const auto matrix = rhi()->clipSpaceCorrMatrix() * payload->matrix;
    std::memcpy(u.data(), matrix.constData(), 64);
    u[16] = payload->heat ? 1.F : 0.F;
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
    cb->draw(static_cast<quint32>(payload->vertices.size()));
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
               static_cast<std::uint64_t>(vertices->size()) +
                   static_cast<std::uint64_t>(payload->rgba.size() * 4) + 80,
               gpu_usage(rhi()), false});
    update();
  }

private:
  QRhi *owner{};
  std::shared_ptr<const Frame> payload;
  std::uint64_t revision{~std::uint64_t{}};
  std::unique_ptr<QRhiBuffer> vertices, uniform;
  std::unique_ptr<QRhiTexture> texture;
  std::unique_ptr<QRhiSampler> sampler;
  std::unique_ptr<QRhiShaderResourceBindings> bindings;
  std::unique_ptr<QRhiGraphicsPipeline> pipeline;
  std::function<void(Observation)> observe;
  std::function<void(QByteArray, QSize)> readback;
  QRhiReadbackResult result;
  bool requested{}, readback_started{};
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
