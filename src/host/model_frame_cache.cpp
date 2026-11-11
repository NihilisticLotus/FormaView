#include "model_session.hpp"
#include <QDataStream>
#include <QIODevice>

ModelRenderState ModelSession::renderState() const {
  ModelRenderState state;
  state.mode = mode_;
  state.mesh = mesh_;
  state.bone = bone_;
  state.highlightedNode = highlightedNode_;
  state.material = material_;
  state.materialOnly = materialOnly_;
  state.isolated = isolated_;
  state.bindings = bindingSelections_;
  state.uvs = uvSelections_;
  state.textures = textureLinks_;
  state.classes = classes_;
  state.levels = levels_;
  state.collisionMode = collisionMode_;
  state.clip = clipIndex_;
  state.time = time_;
  state.visible = visibleMeshes();
  return state;
}
QByteArray ModelSession::stateKey(const ModelRenderState &state) const {
  QByteArray bytes;
  QDataStream stream(&bytes, QIODevice::WriteOnly);
  stream << state.mode << state.mesh << state.bone << state.material
         << state.materialOnly << state.isolated << state.collisionMode << state.highlightedNode;
  // Geometry-cache documents differ per timeline sample.
  if (document_ && document_->geometryCache)
    stream << static_cast<quint64>(document_->cacheSample);
  for (std::size_t i = 0; i < state.bindings.size(); ++i) {
    stream << state.bindings[i] << state.uvs[i];
    const auto &item = state.classes[i];
    stream << item.group << item.level << item.collision << state.visible[i];
    stream << static_cast<quint64>(state.textures[i].size());
    for (const auto &[slot, link] : state.textures[i])
      stream << slot << link.uv << link.path
             << static_cast<quint64>(
                    reinterpret_cast<std::uintptr_t>(link.pixels.get()));
  }
  for (const auto &[group, level] : state.levels)
    stream << group << level;
  return bytes;
}
// Plain mode-0 pose payload: what the resident GPU geometry bakes into its vertices
// and draws. Selected bone/node, material, selected mesh without isolation,
// visibility, LOD levels and collision appearance only select resident draws.
QByteArray ModelSession::gpuStateKey(const ModelRenderState &state) const {
  if (state.mode != 0) return QByteArrayLiteral("F") + stateKey(state);
  QByteArray bytes;
  QDataStream stream(&bytes, QIODevice::WriteOnly);
  stream << QByteArrayLiteral("N") << state.mode;
  for (std::size_t i = 0; i < state.uvs.size(); ++i) {
    stream << state.uvs[i] << state.classes[i].collision;
    stream << static_cast<quint64>(state.textures[i].size());
    for (const auto &[slot, link] : state.textures[i])
      stream << slot << link.uv << link.path
             << static_cast<quint64>(reinterpret_cast<std::uintptr_t>(link.pixels.get()));
  }
  return bytes;
}
// Which resident draws a pose shows: visibility, isolation, the isolated mesh,
// and collision overlay versus opaque appearance.
QByteArray ModelSession::gpuViewKey(const ModelRenderState &state) const {
  QByteArray bytes;
  QDataStream stream(&bytes, QIODevice::WriteOnly);
  stream << state.mode << state.isolated << (state.isolated ? state.mesh : -1) << state.collisionMode;
  for (const bool shown : state.visible) stream << shown;
  return bytes;
}
void ModelSession::cacheFrame() {
  const auto bytes =
      [](const std::shared_ptr<const formaview::render::Frame> &frame) {
        if (!frame)
          return std::uint64_t{};
        auto size = static_cast<std::uint64_t>(
            (frame->vertices.capacity() + frame->guides.capacity()) *
                sizeof(formaview::render::Vertex) +
            frame->draws.capacity() * sizeof(formaview::render::DrawRange) +
            frame->rgba.capacity() * sizeof(float) +
            frame->deformation.capacity() * sizeof(formaview::render::DeformedVertex) +
            frame->streamSignature.capacity());
        // Charge shared static ABC data conservatively to every cached frame.
        if (const auto &base=frame->streamGeometry)
          size += (base->vertices.capacity()+base->guides.capacity())*sizeof(formaview::render::Vertex) +
                  base->sourceCorners.capacity()*sizeof(std::array<std::uint32_t,2>) +
                  base->draws.capacity()*sizeof(formaview::render::DrawRange) +
                  base->rgba.capacity()*sizeof(float);
        for (const auto &texture : frame->textures)
          size += texture->rgba.capacity() * sizeof(float);
        return size;
      };
  const auto size = bytes(frames_.model) + bytes(frames_.uv);
  constexpr std::uint64_t budget = 96ULL * 1024 * 1024;
  if (size > budget)
    return;
  while (!frameCache_.empty() &&
         (frameCache_.size() >= 4 || cachedBytes_ + size > budget)) {
    cachedBytes_ -= frameCache_.back().bytes;
    frameCache_.pop_back();
  }
  frameCache_.push_front(
      {stateKey(renderedState_), frames_, renderedState_, size});
  cachedBytes_ += size;
}
QVariantMap ModelSession::performanceInfoForTest() const {
  return {{"frame_cache_bytes",QVariant::fromValue<qulonglong>(cachedBytes_)},
          {"frame_cache_entries",static_cast<int>(frameCache_.size())},
          {"uv_memo_bytes",QVariant::fromValue<qulonglong>(abcSource_?abcSource_->uvMemoBytes():0)},
          {"active_reads",abcJob_?1:0}, {"frame_jobs",static_cast<int>(frameJobs_.size())},
          {"pending_frame",pendingFrame_}, {"preparing",preparing_}};
}
