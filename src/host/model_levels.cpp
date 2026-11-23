#include "model_session.hpp"
#include <algorithm>
#include <set>

QStringList ModelSession::lodGroups() const {
  std::set<QString> groups;
  for (const auto &item : classes_)
    if (!item.group.isEmpty())
      groups.insert(item.group);
  QStringList result;
  for (const auto &group : groups)
    result.append(group);
  return result;
}
QVariantMap ModelSession::lodSummary() const {
  const auto &state = renderedState_;
  if (!document_ || !inspectable() || state.visible.size() != document_->meshes.size() ||
      state.classes.size() != state.visible.size()) return {};
  qulonglong surface = 0, collision = 0;
  for (std::size_t i = 0; i < state.visible.size(); ++i) {
    if (!state.visible[i] || (state.isolated && static_cast<int>(i) != state.mesh)) continue;
    auto &triangles = state.classes[i].collision ? collision : surface;
    triangles += document_->meshes[i].triangles.size() / 3;
  }
  return {{"surface", surface}, {"collision", collision}};
}
QVariantList ModelSession::lodLevels() const {
  const auto groups = lodGroups();
  if (lodGroup_ < 0 || lodGroup_ >= groups.size())
    return {};
  const auto &group = groups[lodGroup_];
  struct LevelCounts {
    qulonglong points{}, triangles{}, collisionTriangles{};
    int meshes{}, collisionMeshes{};
  };
  std::map<int, LevelCounts> counts;
  for (std::size_t i = 0; i < classes_.size(); ++i)
    if (classes_[i].group == group) {
      auto &count = counts[classes_[i].level];
      if (classes_[i].collision) {
        ++count.collisionMeshes;
        count.collisionTriangles += document_->meshes[i].triangles.size() / 3;
        continue;
      }
      ++count.meshes;
      count.points += document_->meshes[i].points.size();
      count.triangles += document_->meshes[i].triangles.size() / 3;
    }
  QVariantList result;
  for (const auto &[level, count] : counts) {
    const auto current = renderedState_.levels.find(group);
    const auto request = levels_.find(group);
    result.append(QVariantMap{
        {"level", level},
        {"points", count.points},
        {"triangles", count.triangles},
        {"meshes", count.meshes},
        {"collisionMeshes", count.collisionMeshes},
        {"collisionTriangles", count.collisionTriangles},
        {"current",
         current != renderedState_.levels.end() && current->second == level},
        {"requested", request != levels_.end() && request->second == level}});
  }
  return result;
}
QVariantMap ModelSession::classification() const {
  if (!document_ || classes_.empty())
    return {};
  const auto &item = classes_.at(static_cast<std::size_t>(mesh_));
  const auto &source = document_->meshes.at(static_cast<std::size_t>(mesh_));
  const auto level = levels_.find(item.group);
  const bool hiddenByLevel = !item.group.isEmpty() &&
      (level == levels_.end() || level->second != item.level);
  const bool hiddenByCollision = item.collision ? collisionMode_ == 0 : collisionMode_ == 2;
  return {{"hiddenByLevel", hiddenByLevel}, {"hiddenByCollision", hiddenByCollision},
          {"group", item.group},
          {"level", item.level},
          {"collision", item.collision},
          {"evidence", source.classificationEvidence},
          {"candidate", source.classificationCandidate},
          {"corrected", item.group != source.classification.group ||
                            item.level != source.classification.level ||
                            item.collision != source.classification.collision}};
}
std::vector<bool> ModelSession::visibleMeshes() const {
  std::vector<bool> visible;
  for (const auto &item : classes_) {
    const auto level = levels_.find(item.group);
    const bool atLevel = item.group.isEmpty() || (level != levels_.end() &&
                                                  level->second == item.level);
    visible.push_back(item.collision ? collisionMode_ != 0 && atLevel
                                     : collisionMode_ != 2 && atLevel);
  }
  return visible;
}
void ModelSession::chooseVisibleMesh() {
  const auto visible = visibleMeshes();
  if (visible.empty() || visible.at(static_cast<std::size_t>(mesh_)))
    return;
  const auto selected = std::find(visible.begin(), visible.end(), true);
  if (selected == visible.end())
    return;
  mesh_ = static_cast<int>(selected - visible.begin());
  selectedNode_ = document_->meshes.at(static_cast<std::size_t>(mesh_)).nodeId;
  bone_ = material_ = 0;
  updateSkeleton();
}
void ModelSession::revealSelectedMesh() {
  if (!selectedMesh() || !inspectable() || static_cast<std::size_t>(mesh_) >= classes_.size()) return;
  const auto &selected = classes_[mesh_];
  if (!selected.group.isEmpty()) levels_[selected.group] = selected.level;
  if ((selected.collision && collisionMode_ == 0) || (!selected.collision && collisionMode_ == 2))
    collisionMode_ = 1;
  clearSelection();
  rebuild();
  emit changed();
}
void ModelSession::showAvailableMeshes() {
  if (!document_ || classes_.empty()) return;
  isolated_ = false;
  collisionMode_ = 1;
  levels_.clear();
  for (const auto &item : classes_) {
    if (item.group.isEmpty()) continue;
    auto [it, inserted] = levels_.try_emplace(item.group, item.level);
    if (!inserted) it->second = std::min(it->second, item.level);
  }
  chooseVisibleMesh();
  clearSelection();
  rebuild();
  emit changed();
}
void ModelSession::selectLodGroup(int index) {
  if (index < 0 || index >= lodGroups().size())
    return;
  lodGroup_ = index;
  emit changed();
}
void ModelSession::requestLod(int level) {
  const auto groups = lodGroups();
  if (lodGroup_ < 0 || lodGroup_ >= groups.size())
    return;
  const auto &group = groups[lodGroup_];
  if (std::none_of(classes_.begin(), classes_.end(), [&](const auto &item) {
        return item.group == group && item.level == level;
      }))
    return;
  levels_[group] = level;
  globalFrameFailure_ = false;
  chooseVisibleMesh();
  clearSelection();
  rebuild();
  emit changed();
}
void ModelSession::setCollisionMode(int value) {
  if (value < 0 || value > 2 || value == collisionMode_)
    return;
  collisionMode_ = value;
  chooseVisibleMesh();
  clearSelection();
  rebuild();
  emit changed();
}
void ModelSession::classifyMesh(const QString &group, int level,
                                bool collision) {
  if (!selectedMesh() ||
      static_cast<std::size_t>(mesh_) >= classes_.size() ||
      level < 0 || level > 999 || group.size() > 120)
    return;
  auto &item = classes_.at(static_cast<std::size_t>(mesh_));
  item = {group.trimmed(), level, collision};
  globalFrameFailure_ = false;
  if (!item.group.isEmpty())
    levels_.try_emplace(item.group, level);
  lodGroup_ = std::max(0, static_cast<int>(lodGroups().indexOf(item.group)));
  chooseVisibleMesh();
  clearSelection();
  rebuild();
  emit changed();
}
void ModelSession::restoreClassification() {
  const auto *mesh = selectedMesh();
  if (!mesh)
    return;
  const auto &source = mesh->classification;
  classifyMesh(source.group, source.level, source.collision);
}
