#include "formaview/render_item.hpp"
#include <QFile>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <list>
#include <QDebug>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <wrl/client.h>

namespace formaview::render {
namespace {
using Clock = std::chrono::steady_clock;
// Params block: matrix, options (80 bytes, preview) + display, skinOptions, skinMode.
constexpr quint32 uniformBytes = renderer_uniform_bytes;
double elapsed(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}
QShader shader(const char *name) {
  QFile file(QString::fromLatin1(name));
  if (!file.open(QIODevice::ReadOnly)) {
    if(QString::fromLatin1(name).contains("/skin"))return {};
    qFatal("Shader unavailable");
  }
  auto s = QShader::fromSerialized(file.readAll());
  if (!s.isValid() && !QString::fromLatin1(name).contains("/skin"))
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
    retiredBuffers.clear();
    extraTextures.clear();
    pipeline.reset();
    blendedPipeline.reset();
    uvBackgroundPipeline.reset();
    guideBindings.reset();
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
    data = payload && payload->geometry ? payload->geometry : payload;
    skinFailure = source->skinFailure;
    frameFailure = source->frameFailure;
    if (failedFrame.lock() != data || (data && failedRevision != data->revision))
      frameFailed = false;
    frameSubmitted = source->frameSubmitted;
    viewMatrix = source->useViewMatrix
                     ? source->viewMatrix
                     : (payload ? payload->matrix : QMatrix4x4{});
    showGrid = source->showGrid;
    showAxes = source->showAxes;
    requestedWireframe = source->wireframe;
    skinProbe=source->skinProbe;
    observe = source->observe;
    if (!requested && source->readback) {
      readback = source->readback;
      requested = true;
    }
  }
  void initialize(QRhiCommandBuffer *) override {
    if (owner != rhi()) {
      retiredBuffers.clear();
      if (!readbackCompleted) {
        readback_started = false;
        readbackPasses = 0;
      }
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
      guideBindings.reset();
    bindings.reset();
      texture.reset();
      sampler.reset();
      vertices.reset();
      uniform.reset();
      skinSource.reset(); skinRanges.reset(); morphRanges.reset(); influenceTexture.reset(); paletteTexture.reset(); morphTexture.reset();
      skinBytes=0; poseRevision=~std::uint64_t{}; guidePoseRevision=~std::uint64_t{};
      owner = rhi();
      revision = ~std::uint64_t{};
    }
    if (!uniform) {
      uniform.reset(
          rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, uniformBytes));
      if (!uniform->create()) {
        uniform.reset();
        rejectFrame(nullptr, nullptr, "Uniform allocation failed");
      }
    }
  }
  void render(QRhiCommandBuffer *cb) override {
    if (!frameFailed && owner != rhi()) initialize(nullptr);
    if (frameFailed) {
      cb->beginPass(renderTarget(), QColor(24, 24, 24), {1, 0}); cb->endPass();
      return;
    }
    // The previous frame has been submitted: buffers replaced during it can go now.
    retiredBuffers.clear();
    if (!data || data->vertices.empty()) {
      residentCache.clear();
      guidePipeline.reset();
      selectionPipeline.reset();
      guideVertices.reset();
      uploadedFrame.reset();
      activeBytes = 0;
      pipeline.reset();
      blendedPipeline.reset();
      uvBackgroundPipeline.reset();
      guideBindings.reset();
    bindings.reset();
      texture.reset();
      sampler.reset();
      vertices.reset();
      extraTextures.clear();
      skinSource.reset(); skinRanges.reset(); morphRanges.reset(); influenceTexture.reset(); paletteTexture.reset(); morphTexture.reset(); skinBytes=0;
      revision = ~std::uint64_t{};
      cb->beginPass(renderTarget(), QColor(28, 28, 30), {1, 0});
      cb->endPass();
      if (observe)
        observe({0, 0, 0, uniform ? std::uint64_t{uniformBytes} : 0ULL, gpu_usage(rhi()), false});
      if (readback && !readbackCompleted) {
        readbackCompleted = true;
        readback({}, {});
      }
      return;
    }
    if (skinSource != data->skin) {
      residentCache.clear();
      pipeline.reset(); blendedPipeline.reset(); guidePipeline.reset(); selectionPipeline.reset(); uvBackgroundPipeline.reset();
      guideBindings.reset();
    bindings.reset(); guideBindings.reset(); extraTextures.clear(); texture.reset(); vertices.reset(); guideVertices.reset();
      skinRanges.reset(); morphRanges.reset(); influenceTexture.reset(); paletteTexture.reset(); morphTexture.reset();
      skinSource=data->skin; uploadedFrame.reset(); activeBytes=0; revision=~std::uint64_t{};
      poseRevision=~std::uint64_t{}; guidePoseRevision=~std::uint64_t{}; skinFailed=false;
    }
    const auto start = Clock::now();
    std::uint64_t uploadedBytes = 0;
    bool residentHit = false;
    auto *updates = rhi()->nextResourceUpdateBatch();
    if (skinSource && !prepareSkin(updates, uploadedBytes)) {
      updates->release();
      cb->beginPass(renderTarget(), QColor(24,24,24), {1,0}); cb->endPass();
      return;
    }
    if (uploadedFrame.lock() != data || data->revision != revision) {
      {
        std::erase_if(residentCache,
                      [](const auto &entry) { return entry.source.expired(); });
        const auto found = std::find_if(
            residentCache.begin(), residentCache.end(), [&](const auto &entry) {
              return entry.source.lock() == data &&
                     entry.revision == data->revision;
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
    const bool wire = requestedWireframe && !data->lines && !data->uv;
    if (lineMode != data->lines || wireMode != wire || pipelineProbe!=skinProbe) {
      guidePipeline.reset();selectionPipeline.reset();pipelineProbe=skinProbe;
      pipeline.reset();
      blendedPipeline.reset();
      uvBackgroundPipeline.reset();
      lineMode = data->lines;
      wireMode = wire;
    }
    if (uploadedFrame.lock() != data || data->revision != revision) {
      const auto bytes = data->vertices.size() * sizeof(Vertex);
      uploadedBytes += bytes + data->rgba.size() * sizeof(float);
      if (bytes > vertex_upload_budget || data->width <= 0 ||
          data->height <= 0 ||
          data->rgba.size() !=
              static_cast<std::size_t>(data->width) *
                  static_cast<std::size_t>(data->height) * 4 ||
          data->rgba.size() > 128ULL * 1024 * 1024) {
        rejectFrame(cb, updates, "Render upload budget/shape rejected"); return;
      }
      if (!vertices || vertices->size() != bytes) {
        vertices.reset(rhi()->newBuffer(skinSource?QRhiBuffer::Immutable:QRhiBuffer::Dynamic,
                                        QRhiBuffer::VertexBuffer,
                                        static_cast<quint32>(bytes)));
        if (!vertices->create()) {
          if(skinSource){rejectSkin("Vertex allocation failed");updates->release();return;}
          rejectFrame(cb, updates, "Vertex allocation failed"); return;
          }
      }
      if(skinSource)updates->uploadStaticBuffer(vertices.get(),data->vertices.data());
      else updates->updateDynamicBuffer(vertices.get(), 0,static_cast<quint32>(bytes),data->vertices.data());
      if (data->guides.empty()) {
        guidePipeline.reset();
        selectionPipeline.reset();
        guideVertices.reset();
      } else {
        const auto guideBytes = data->guides.size() * sizeof(Vertex);
        if (guideBytes > 1024 * 1024 ||
            data->gridVertices +
                    static_cast<std::uint64_t>(data->selectionVertices) >
                data->guides.size() ||
            data->selectionVertices % 2 || data->gridVertices % 2 ||
            data->guides.size() % 2) {
          rejectFrame(cb, updates, "Invalid model guide geometry"); return;
        }
        if (!guideVertices || guideVertices->size() != guideBytes) {
          if (guideVertices) retiredBuffers.push_back(std::move(guideVertices));
          guideVertices.reset(
              rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer,
                               static_cast<quint32>(guideBytes)));
          if (!guideVertices->create()) {
            if(skinSource){rejectSkin("Guide allocation failed");updates->release();return;}
            rejectFrame(cb, updates, "Guide allocation failed"); return;
            }
        }
        updates->updateDynamicBuffer(guideVertices.get(), 0,
                                     static_cast<quint32>(guideBytes),
                                     data->guides.data());
        uploadedBytes += guideBytes;
      }
      const QSize size(data->width, data->height);
      if (!texture || texture->pixelSize() != size) {
        guidePipeline.reset();
        selectionPipeline.reset();
        pipeline.reset();
        blendedPipeline.reset();
        uvBackgroundPipeline.reset();
        guideBindings.reset();
    bindings.reset();
        texture.reset(rhi()->newTexture(QRhiTexture::RGBA32F, size));
        if (!texture->create()) {
          if(skinSource){rejectSkin("Float texture unavailable");updates->release();return;}
          rejectFrame(cb, updates, "Float texture unavailable"); return;
          }
        if (!sampler) {
          sampler.reset(rhi()->newSampler(
              QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
              QRhiSampler::Repeat, QRhiSampler::Repeat));
          if (!sampler->create()) {
            if(skinSource){rejectSkin("Sampler failed");updates->release();return;}
            rejectFrame(cb, updates, "Sampler failed"); return;
            }
        }
        bindings.reset(rhi()->newShaderResourceBindings());
        configureBindings(bindings.get(), texture.get(), sampler.get());
        if (!bindings->create()) {
          if(skinSource){rejectSkin("Bindings failed");updates->release();return;}
          rejectFrame(cb, updates, "Bindings failed"); return;
          }
      }
      QRhiTextureSubresourceUploadDescription upload(
          data->rgba.data(),
          static_cast<quint32>(data->rgba.size() * sizeof(float)));
      updates->uploadTexture(
          texture.get(),
          QRhiTextureUploadDescription({QRhiTextureUploadEntry(0, 0, upload)}));
      const bool reuseTextures = extraTextures.size() == data->textures.size() &&
          std::equal(extraTextures.begin(), extraTextures.end(), data->textures.begin(),
                     [](const auto &gpu, const auto &cpu) { return gpu->source == cpu; });
      if (!reuseTextures) {
        extraTextures.clear();
        for (const auto &src : data->textures) {
          if (!src || src->width <= 0 || src->height <= 0 ||
              src->rgba.size() != static_cast<std::size_t>(src->width) *
                                      static_cast<std::size_t>(src->height) * 4) {
            rejectFrame(cb, updates, "Model texture shape rejected"); return;
          }
          uploadedBytes += src->rgba.size() * sizeof(float);
          auto extra = std::make_unique<ExtraTexture>();
          extra->source = src;
          extra->texture.reset(rhi()->newTexture(
              QRhiTexture::RGBA32F, {src->width, src->height}, 1,
              src->mipmaps
                  ? QRhiTexture::MipMapped | QRhiTexture::UsedWithGenerateMips
                  : QRhiTexture::Flags{}));
          if (!extra->texture->create()) {
            if(skinSource){rejectSkin("Model texture allocation failed");updates->release();return;}
            rejectFrame(cb, updates, "Model texture allocation failed"); return;
            }
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
          if (!extra->sampler->create()) {
            if(skinSource){rejectSkin("Model sampler allocation failed");updates->release();return;}
            rejectFrame(cb, updates, "Model sampler allocation failed"); return;
            }
          extra->bindings.reset(rhi()->newShaderResourceBindings());
          configureBindings(extra->bindings.get(), extra->texture.get(), extra->sampler.get());
          if (!extra->bindings->create()) {
            if(skinSource){rejectSkin("Model texture bindings failed");updates->release();return;}
            rejectFrame(cb, updates, "Model texture bindings failed"); return;
            }
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
      }
      revision = data->revision;
      uploadedFrame = data;
      activeBytes = bytes + data->rgba.size() * sizeof(float) +
                    data->guides.size() * sizeof(Vertex);
      for (const auto &src : data->textures) {
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
    if (skinSource && payload->selectionVertices && payload->revision!=guidePoseRevision) {
      const auto bytes=payload->guides.size()*sizeof(Vertex);
      if (!guideVertices || guideVertices->size()!=bytes) {
        // The static block above may already have queued an update on this buffer in the same batch.
        if (guideVertices) retiredBuffers.push_back(std::move(guideVertices));
        guideVertices.reset(rhi()->newBuffer(QRhiBuffer::Dynamic,QRhiBuffer::VertexBuffer,static_cast<quint32>(bytes)));
        if(!guideVertices->create()){rejectSkin("GPU animated guides allocation failed");updates->release();return;}
      }
      updates->updateDynamicBuffer(guideVertices.get(),0,static_cast<quint32>(bytes),payload->guides.data());uploadedBytes+=bytes;
      guidePoseRevision=payload->revision;
    }
    if (guideVertices && !guideBindings) {
      guideBindings.reset(rhi()->newShaderResourceBindings());
      using B=QRhiShaderResourceBinding;
      guideBindings->setBindings({B::uniformBuffer(0,B::VertexStage|B::FragmentStage,uniform.get()),B::sampledTexture(1,B::FragmentStage,texture.get(),sampler.get())});
      if(!guideBindings->create()){if(skinSource){rejectSkin("Guide bindings failed");updates->release();return;}rejectFrame(cb, updates, "Guide bindings failed");return;}
    }
    trimResidents();
    if (!pipeline) {
      pipeline.reset(rhi()->newGraphicsPipeline());
      pipeline->setShaderStages(
          {{QRhiShaderStage::Vertex, shader(skinSource ? ":/shaders/skin.vert.qsb" : ":/shaders/preview.vert.qsb")},
           {QRhiShaderStage::Fragment, shader(skinProbe ? ":/shaders/skin_probe.frag.qsb" : ":/shaders/preview.frag.qsb")}});
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
      guideLayout=layout;
      if (skinSource) {
        layout.setBindings({QRhiVertexInputBinding(sizeof(Vertex)), QRhiVertexInputBinding(8), QRhiVertexInputBinding(8)});
        layout.setAttributes({
          {0,0,QRhiVertexInputAttribute::Float3,0}, {0,1,QRhiVertexInputAttribute::Float2,12},
          {0,2,QRhiVertexInputAttribute::Float,20}, {0,3,QRhiVertexInputAttribute::Float3,24},
          {0,4,QRhiVertexInputAttribute::Float3,36}, {0,5,QRhiVertexInputAttribute::Float3,48},
          {1,6,QRhiVertexInputAttribute::UInt2,0}, {2,7,QRhiVertexInputAttribute::UInt2,0}});
      }
      pipeline->setVertexInputLayout(layout);
      pipeline->setShaderResourceBindings(bindings.get());
      pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
      pipeline->setSampleCount(renderTarget()->sampleCount());
      pipeline->setDepthTest(true);
      pipeline->setDepthWrite(true);
      pipeline->setTopology(skinProbe ? QRhiGraphicsPipeline::Points : lineMode ? QRhiGraphicsPipeline::Lines
                                     : QRhiGraphicsPipeline::Triangles);
      pipeline->setPolygonMode(wireMode ? QRhiGraphicsPipeline::Line : QRhiGraphicsPipeline::Fill);
      if (!pipeline->create()) {
        if(skinSource){rejectSkin("Pipeline failed");updates->release();return;}
        rejectFrame(cb, updates, "Pipeline failed"); return;
        }
    }
    if (data->backgroundVertices && !uvBackgroundPipeline) {
      uvBackgroundPipeline.reset(rhi()->newGraphicsPipeline());
      uvBackgroundPipeline->setShaderStages(
          {{QRhiShaderStage::Vertex, shader(skinSource ? ":/shaders/skin.vert.qsb" : ":/shaders/preview.vert.qsb")},
           {QRhiShaderStage::Fragment, shader(skinProbe ? ":/shaders/skin_probe.frag.qsb" : ":/shaders/preview.frag.qsb")}});
      uvBackgroundPipeline->setVertexInputLayout(pipeline->vertexInputLayout());
      uvBackgroundPipeline->setShaderResourceBindings(bindings.get());
      uvBackgroundPipeline->setRenderPassDescriptor(
          renderTarget()->renderPassDescriptor());
      uvBackgroundPipeline->setSampleCount(renderTarget()->sampleCount());
      uvBackgroundPipeline->setDepthTest(true);
      uvBackgroundPipeline->setDepthWrite(true);
      uvBackgroundPipeline->setTopology(QRhiGraphicsPipeline::Triangles);
      if (!uvBackgroundPipeline->create()) {
        if(skinSource){rejectSkin("UV background pipeline failed");updates->release();return;}
        rejectFrame(cb, updates, "UV background pipeline failed"); return;
        }
    }
    if (guideVertices && !guidePipeline) {
      guidePipeline.reset(rhi()->newGraphicsPipeline());
      guidePipeline->setShaderStages(
          {{QRhiShaderStage::Vertex, shader(":/shaders/preview.vert.qsb")},
           {QRhiShaderStage::Fragment, shader(skinProbe ? ":/shaders/skin_probe.frag.qsb" : ":/shaders/preview.frag.qsb")}});
      guidePipeline->setVertexInputLayout(guideLayout);
      guidePipeline->setShaderResourceBindings(guideBindings.get());
      guidePipeline->setRenderPassDescriptor(
          renderTarget()->renderPassDescriptor());
      guidePipeline->setSampleCount(renderTarget()->sampleCount());
      guidePipeline->setDepthTest(true);
      guidePipeline->setDepthWrite(false);
      guidePipeline->setTopology(QRhiGraphicsPipeline::Lines);
      if (!guidePipeline->create()) {
        if(skinSource){rejectSkin("Guide pipeline failed");updates->release();return;}
        rejectFrame(cb, updates, "Guide pipeline failed"); return;
        }
    }
    if (guideVertices && payload->selectionVertices && !selectionPipeline) {
      selectionPipeline.reset(rhi()->newGraphicsPipeline());
      selectionPipeline->setShaderStages(
          {{QRhiShaderStage::Vertex, shader(":/shaders/preview.vert.qsb")},
           {QRhiShaderStage::Fragment, shader(skinProbe ? ":/shaders/skin_probe.frag.qsb" : ":/shaders/preview.frag.qsb")}});
      selectionPipeline->setVertexInputLayout(guideLayout);
      selectionPipeline->setShaderResourceBindings(guideBindings.get());
      selectionPipeline->setRenderPassDescriptor(
          renderTarget()->renderPassDescriptor());
      selectionPipeline->setSampleCount(renderTarget()->sampleCount());
      selectionPipeline->setDepthTest(false);
      selectionPipeline->setDepthWrite(false);
      selectionPipeline->setTopology(QRhiGraphicsPipeline::Lines);
      if (!selectionPipeline->create()) {
        if(skinSource){rejectSkin("Skeleton selection pipeline failed");updates->release();return;}
        rejectFrame(cb, updates, "Skeleton selection pipeline failed"); return;
        }
    }
    if (!blendedPipeline &&
        std::any_of(data->draws.begin(), data->draws.end(),
                    [](const auto &draw) { return draw.blend; })) {
      blendedPipeline.reset(rhi()->newGraphicsPipeline());
      blendedPipeline->setShaderStages(
          {{QRhiShaderStage::Vertex, shader(skinSource ? ":/shaders/skin.vert.qsb" : ":/shaders/preview.vert.qsb")},
           {QRhiShaderStage::Fragment, shader(skinProbe ? ":/shaders/skin_probe.frag.qsb" : ":/shaders/preview.frag.qsb")}});
      blendedPipeline->setVertexInputLayout(pipeline->vertexInputLayout());
      blendedPipeline->setShaderResourceBindings(bindings.get());
      blendedPipeline->setRenderPassDescriptor(
          renderTarget()->renderPassDescriptor());
      blendedPipeline->setSampleCount(renderTarget()->sampleCount());
      blendedPipeline->setDepthTest(true);
      blendedPipeline->setDepthWrite(false);
      blendedPipeline->setPolygonMode(wireMode ? QRhiGraphicsPipeline::Line : QRhiGraphicsPipeline::Fill);
      QRhiGraphicsPipeline::TargetBlend blend;
      blend.enable = true;
      blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
      blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
      blendedPipeline->setTargetBlends({blend});
      if (!blendedPipeline->create()) {
        if(skinSource){rejectSkin("Transparent pipeline failed");updates->release();return;}
        rejectFrame(cb, updates, "Transparent pipeline failed"); return;
        }
    }
    std::array<float, uniformBytes / 4> u{};
    const auto matrix = rhi()->clipSpaceCorrMatrix() * viewMatrix;
    std::memcpy(u.data(), matrix.constData(), 64);
    u[16] = data->heat ? 1.F : 0.F;
    u[17] = data->lit ? 1.F : 0.F;
    u[18] = data->linearColour ? 1.F : 0.F;
    u[19] = data->uv ? 1.F : 0.F;
    if(skinSource) {
      std::memcpy(u.data()+20,skinSource->display.constData(),64);
      u[36]=static_cast<float>(influenceWidth); u[37]=static_cast<float>(paletteWidth);
      u[38]=static_cast<float>(skinProbe); u[39]=static_cast<float>(data->vertices.size());
      u[40]=skinSource->boneRotationNormals ? 1.F : 0.F;
      u[41]=skinSource->morphStride?static_cast<float>(skinSource->morphPaletteBase):0.F;
      u[42]=static_cast<float>(skinSource->morphStride);
      u[43]=static_cast<float>(morphWidth);
    }
    updates->updateDynamicBuffer(uniform.get(), 0, skinSource?uniformBytes:80, u.data());
    const auto prepare = elapsed(start);
    const auto record = Clock::now();
    if (data->backgroundVertices &&
        (!data->uv || !data->lines || data->backgroundVertices != 6 ||
         data->vertices.size() < 6 || data->backgroundTexture < 0 ||
         static_cast<std::size_t>(data->backgroundTexture) >= extraTextures.size())) {
      rejectFrame(cb, updates, "Invalid UV background range"); return;
    }
    cb->beginPass(renderTarget(), QColor(24, 24, 24, skinProbe?0:255), {1, 0}, updates);
    cb->setGraphicsPipeline(pipeline.get());
    cb->setViewport({0, 0,
                     static_cast<float>(renderTarget()->pixelSize().width()),
                     static_cast<float>(renderTarget()->pixelSize().height())});
    cb->setShaderResources();

    const QRhiCommandBuffer::VertexInput meshInputs[]={{vertices.get(),0},{skinRanges.get(),0},{morphRanges.get(),0}};
    cb->setVertexInput(0, skinSource?3:1, meshInputs);
    if (data->backgroundVertices) {
      cb->setGraphicsPipeline(uvBackgroundPipeline.get());
      cb->setShaderResources(
          extraTextures
              .at(static_cast<std::size_t>(data->backgroundTexture))
              ->bindings.get());
      cb->draw(data->backgroundVertices);
      cb->setGraphicsPipeline(pipeline.get());
      cb->setShaderResources(bindings.get());
    }
    const auto guideData=payload->geometry?payload:data;
    const auto drawGuides = [&] {
      if (!guideVertices ||
          (!showGrid && !showAxes && !guideData->selectionVertices) ||
          data->uv)
        return;
      cb->setGraphicsPipeline(guidePipeline.get());
      cb->setShaderResources(guideBindings.get());
      const QRhiCommandBuffer::VertexInput guideBinding(guideVertices.get(), 0);
      cb->setVertexInput(0, 1, &guideBinding);
      if (showGrid && guideData->gridVertices)
        cb->draw(guideData->gridVertices);
      const auto axes = static_cast<quint32>(guideData->guides.size()) -
                        guideData->gridVertices - guideData->selectionVertices;
      if (showAxes && axes)
        cb->draw(axes, 1, guideData->gridVertices);
      if (guideData->selectionVertices) {
        cb->setGraphicsPipeline(selectionPipeline.get());
        cb->setShaderResources(guideBindings.get());
        cb->draw(guideData->selectionVertices, 1,
                 static_cast<quint32>(guideData->guides.size()) -
                     guideData->selectionVertices);
      }
      cb->setGraphicsPipeline(pipeline.get());
      cb->setShaderResources(bindings.get());
      cb->setVertexInput(0, skinSource?2:1, meshInputs);
    };
    if (draws().empty() && !payload->explicitDraws) {
      cb->draw(static_cast<quint32>(data->vertices.size()) -
                   data->backgroundVertices,
               1, data->backgroundVertices);
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
      for (const auto &draw : draws())
        if (!draw.blend)
          drawRange(draw);
      drawGuides();
      if (blendedPipeline) {
        std::vector<const DrawRange *> transparent;
        for (const auto &draw : draws())
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
    if (frameSubmitted) frameSubmitted(payload);
    if (readback && !readback_started) {
      readback_started = true;
      result.completed = [this] {
        if (readbackCompleted)
          return;
        readbackCompleted = true;
        readback(result.data, result.pixelSize);
      };
      auto *batch = rhi()->nextResourceUpdateBatch();
      batch->readBackTexture(QRhiReadbackDescription(colorTexture()), &result);
      cb->resourceUpdate(batch);
    }
    if (observe)
      observe({prepare, elapsed(record), cb->lastCompletedGpuTime(),
               residentBytes() + skinBytes + uniformBytes, gpu_usage(rhi()), false, payload->revision,
               uploadedBytes,
               static_cast<std::uint32_t>(residentCache.size() + 1),
               residentHit, payload->clip, payload->seconds, bool(skinSource)});
    if (readback && !readbackCompleted) {
      if (++readbackPasses < 120)
        update();
      else {
        readbackCompleted = true;
        readback({}, {});
      }
    }
  }

private:
  struct ExtraTexture {
    std::shared_ptr<const Texture> source;
    std::unique_ptr<QRhiTexture> texture;
    std::unique_ptr<QRhiShaderResourceBindings> bindings, guideBindings;
    std::unique_ptr<QRhiSampler> sampler;
  };
  struct Resident {
    std::weak_ptr<const Frame> source;
    std::uint64_t bytes{}, revision{};
    bool lines{}, wire{};
    std::unique_ptr<QRhiBuffer> vertices, guides;
    std::unique_ptr<QRhiTexture> texture;
    std::unique_ptr<QRhiSampler> sampler;
    std::unique_ptr<QRhiShaderResourceBindings> bindings, guideBindings;
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
    saved.wire = wireMode;
    saved.vertices = std::move(vertices);
    saved.guides = std::move(guideVertices);
    saved.texture = std::move(texture);
    saved.sampler = std::move(sampler);
    saved.bindings = std::move(bindings);
    saved.guideBindings=std::move(guideBindings);
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
    guideBindings=std::move(saved.guideBindings);
    sampler = std::move(saved.sampler);
    texture = std::move(saved.texture);
    vertices = std::move(saved.vertices);
    guideVertices = std::move(saved.guides);
    uploadedFrame = saved.source;
    activeBytes = saved.bytes;
    revision = saved.revision;
    lineMode = saved.lines;
    wireMode = saved.wire;
  }
  std::vector<std::unique_ptr<ExtraTexture>> extraTextures;
  QRhi *owner{};
  std::shared_ptr<const Frame> payload, data;
  std::shared_ptr<const SkinData> skinSource;
  std::unique_ptr<QRhiBuffer> skinRanges, morphRanges;
  std::unique_ptr<QRhiTexture> influenceTexture, paletteTexture, morphTexture;
  int morphWidth{};
  std::unique_ptr<QRhiSampler> skinSampler;
  QRhiVertexInputLayout guideLayout;
  std::uint64_t guidePoseRevision{~std::uint64_t{}};
  std::uint64_t poseRevision{~std::uint64_t{}}, skinBytes{};
  int influenceWidth{}, paletteWidth{};
  bool skinFailed{};
  int skinProbe{},pipelineProbe{};
  std::function<void(QString, std::shared_ptr<const Frame>)> skinFailure;
  std::function<void(QString, std::shared_ptr<const Frame>)> frameFailure;
  std::weak_ptr<const Frame> failedFrame;
  std::uint64_t failedRevision{};
  bool frameFailed{};
  std::function<void(std::shared_ptr<const Frame>)> frameSubmitted;
  const std::vector<DrawRange> &draws() const { return payload->geometry?payload->draws:data->draws; }
  void configureBindings(QRhiShaderResourceBindings *srb, QRhiTexture *tex, QRhiSampler *sample) {
    using B=QRhiShaderResourceBinding;
    if (skinSource) srb->setBindings({
      B::uniformBuffer(0,B::VertexStage|B::FragmentStage,uniform.get()), B::sampledTexture(1,B::FragmentStage,tex,sample),
      B::sampledTexture(3,B::VertexStage,influenceTexture.get(),skinSampler.get()),
      B::sampledTexture(4,B::VertexStage,paletteTexture.get(),skinSampler.get()),
      B::sampledTexture(5,B::VertexStage,morphTexture.get(),skinSampler.get())});
    else srb->setBindings({B::uniformBuffer(0,B::VertexStage|B::FragmentStage,uniform.get()), B::sampledTexture(1,B::FragmentStage,tex,sample)});
  }
  void rejectFrame(QRhiCommandBuffer *cb, QRhiResourceUpdateBatch *updates,
                   const QString &reason) {
    if (updates) updates->release();
    if (!frameFailed) {
      qWarning().noquote() << "Render frame rejected:" << reason;
      if (frameFailure) frameFailure(reason, payload);
    }
    frameFailed = true;
    failedFrame = data;
    failedRevision = data ? data->revision : 0;
    // A partially created resource must never be reused by the next frame.
    // initialize() retires all resources before a new payload is rendered.
    owner = nullptr;
    if (readback && !readbackCompleted) { readbackCompleted = true; readback({}, {}); }
    if (cb) { cb->beginPass(renderTarget(), QColor(24, 24, 24), {1, 0}); cb->endPass(); }
  }
  bool rejectSkin(QString reason) {
    if (!skinFailed) { qWarning().noquote()<<"GPU skinning CPU fallback:"<<reason; if(skinFailure)skinFailure(reason,payload); }
    skinFailed=true;
    if(readback && !readbackCompleted){readbackCompleted=true;readback({},{});}
    return false;
  }
  bool prepareSkin(QRhiResourceUpdateBatch *batch, std::uint64_t &uploaded) {
    if (skinFailed) return rejectSkin("GPU skinning remains unavailable");
    if(qEnvironmentVariableIsSet("FORMAVIEW_GPU_FORCE_FALLBACK"))return rejectSkin("Diagnostic forced GPU capability fallback");
    const auto limit=rhi()->resourceLimit(QRhi::TextureSizeMax);
    if (!rhi()->isFeatureSupported(QRhi::TexelFetch) || !rhi()->isFeatureSupported(QRhi::IntAttributes) ||
        !rhi()->isTextureFormatSupported(QRhiTexture::RGBA32F) || rhi()->resourceLimit(QRhi::MaxVertexInputs)<7 || limit<4)
      return rejectSkin("Required float vertex textures or integer attributes unavailable");
    // Palette = cluster matrices (16 floats each, up to morphPaletteBase*4)
    // plus morph weight rows (groups x stride floats) when morphs animate.
    const auto clusterFloats = static_cast<std::size_t>(skinSource->morphPaletteBase) * 4;
    if (skinSource->ranges.size()!=data->vertices.size() || skinSource->influences.empty() || payload->palette.empty() || payload->palette.size()<clusterFloats ||
        (skinSource->morphStride ? (payload->palette.size()-clusterFloats)%skinSource->morphStride : payload->palette.size()%16))
      return rejectSkin("Invalid GPU skin geometry/palette shape");
    const auto shape=[&](std::size_t floats) {
      const auto texels=(floats+3)/4;
      const auto width=std::min<std::size_t>(std::min(limit,1024),texels);
      const auto height=(texels+width-1)/width;
      return height>static_cast<std::size_t>(limit)?QSize{}:QSize(static_cast<int>(width),static_cast<int>(height));
    };
    const auto influenceSize=shape(skinSource->influences.size()), paletteSize=shape(payload->palette.size());
    const auto morphSize=skinSource->morphStride?shape(skinSource->morph.size()):QSize(1,1);
    if (influenceSize.isEmpty() || paletteSize.isEmpty() || skinSource->influences.size()>32ULL*1024*1024 || payload->palette.size()>32ULL*1024*1024 || skinSource->morph.size()>128ULL*1024*1024)
      return rejectSkin("GPU skin texture budget exceeded");
    if (!skinRanges) {
      if(!shader(":/shaders/skin.vert.qsb").isValid())return rejectSkin("GPU skin shader unavailable");
      skinRanges.reset(rhi()->newBuffer(QRhiBuffer::Immutable,QRhiBuffer::VertexBuffer,static_cast<quint32>(skinSource->ranges.size()*8)));
      morphRanges.reset(rhi()->newBuffer(QRhiBuffer::Immutable,QRhiBuffer::VertexBuffer,static_cast<quint32>(skinSource->morphRanges.size()*8)));
      influenceTexture.reset(rhi()->newTexture(QRhiTexture::RGBA32F,influenceSize));
      paletteTexture.reset(rhi()->newTexture(QRhiTexture::RGBA32F,paletteSize));
      morphTexture.reset(rhi()->newTexture(QRhiTexture::RGBA32F,skinSource->morphStride?morphSize:QSize(1,1)));
      skinSampler.reset(rhi()->newSampler(QRhiSampler::Nearest,QRhiSampler::Nearest,QRhiSampler::None,QRhiSampler::ClampToEdge,QRhiSampler::ClampToEdge));
      if (!skinRanges->create() || !morphRanges->create() || !influenceTexture->create() || !paletteTexture->create() || !morphTexture->create() || !skinSampler->create())
        return rejectSkin("GPU skin resource allocation failed");
      batch->uploadStaticBuffer(skinRanges.get(),skinSource->ranges.data());
      if (!skinSource->morphRanges.empty())
        batch->uploadStaticBuffer(morphRanges.get(),skinSource->morphRanges.data());
      std::vector<float> padded(static_cast<std::size_t>(influenceSize.width())*influenceSize.height()*4);
      std::copy(skinSource->influences.begin(),skinSource->influences.end(),padded.begin());
      batch->uploadTexture(influenceTexture.get(),QRhiTextureUploadDescription({QRhiTextureUploadEntry(0,0,QRhiTextureSubresourceUploadDescription(padded.data(),static_cast<quint32>(padded.size()*4)))}));
      if (skinSource->morphStride) {
        std::vector<float> morphPadded(static_cast<std::size_t>(morphSize.width())*morphSize.height()*4);
        std::copy(skinSource->morph.begin(),skinSource->morph.end(),morphPadded.begin());
        batch->uploadTexture(morphTexture.get(),QRhiTextureUploadDescription({QRhiTextureUploadEntry(0,0,QRhiTextureSubresourceUploadDescription(morphPadded.data(),static_cast<quint32>(morphPadded.size()*4)))}));
        uploaded+=morphPadded.size()*4;
      }
      uploaded+=skinSource->ranges.size()*8+padded.size()*4;
      influenceWidth=influenceSize.width(); paletteWidth=paletteSize.width(); morphWidth=skinSource->morphStride?morphSize.width():0;
      skinBytes=skinSource->ranges.size()*8+padded.size()*4+static_cast<std::size_t>(paletteSize.width())*paletteSize.height()*16;
      qInfo()<<"GPU skinning active:"<<rhi()->driverInfo()<<"vertices"<<data->vertices.size()<<"clusters"<<payload->palette.size()/16<<"influence bytes"<<padded.size()*4;
    }
    if (paletteTexture->pixelSize()!=paletteSize) return rejectSkin("GPU pose palette changed geometry domain");
    if (poseRevision!=payload->revision) {
      std::vector<float> padded(static_cast<std::size_t>(paletteSize.width())*paletteSize.height()*4);
      std::copy(payload->palette.begin(),payload->palette.end(),padded.begin());
      batch->uploadTexture(paletteTexture.get(),QRhiTextureUploadDescription({QRhiTextureUploadEntry(0,0,QRhiTextureSubresourceUploadDescription(padded.data(),static_cast<quint32>(padded.size()*4)))}));
      uploaded+=padded.size()*4+uniformBytes; poseRevision=payload->revision;
    }
    return true;
  }
  QMatrix4x4 viewMatrix;
  std::uint64_t revision{~std::uint64_t{}};
  std::unique_ptr<QRhiBuffer> vertices, uniform, guideVertices;
  std::vector<std::unique_ptr<QRhiBuffer>> retiredBuffers;
  std::unique_ptr<QRhiTexture> texture;
  std::unique_ptr<QRhiSampler> sampler;
  std::unique_ptr<QRhiShaderResourceBindings> bindings, guideBindings;
  std::unique_ptr<QRhiGraphicsPipeline> pipeline;
  std::unique_ptr<QRhiGraphicsPipeline> blendedPipeline;
  std::unique_ptr<QRhiGraphicsPipeline> uvBackgroundPipeline;
  std::unique_ptr<QRhiGraphicsPipeline> guidePipeline, selectionPipeline;
  bool showGrid{}, showAxes{}, requestedWireframe{}, wireMode{};
  std::function<void(Observation)> observe;
  std::function<void(QByteArray, QSize)> readback;
  QRhiReadbackResult result;
  bool requested{}, readback_started{}, readbackCompleted{};
  unsigned readbackPasses{};
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
