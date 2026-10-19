#include "model_session.hpp"

QVariantMap ModelSession::sourceNode() const {
  if (!document_)
    return {};
  if (const auto it = document_->nodeById.constFind(selectedNode_);
      it != document_->nodeById.cend())
    return it.value();
  return {{"id", selectedNode_},
          {"name", document_->name},
          {"stableId", document_->hash}};
}
QVariantMap ModelSession::bindingInfo() const {
  if (!document_ || bindingIndex() < 0)
    return {};
  const auto &skin = document_->meshes.at(static_cast<std::size_t>(mesh_))
                         .skins.at(static_cast<std::size_t>(bindingIndex()));
  QVariantMap result{{"id", skin.id},
                     {"method", skin.method},
                     {"paletteSize", skin.bones.size()}};
  if (bone_ >= 0 && bone_ < skin.palette.size()) {
    const auto entry = skin.palette.at(bone_).toMap();
    for (auto it = entry.cbegin(); it != entry.cend(); ++it)
      result.insert(it.key(), it.value());
  }
  return result;
}
