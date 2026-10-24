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
QVariantList ModelSession::lodLevels() const {
  const auto groups = lodGroups();
  if (lodGroup_ < 0 || lodGroup_ >= groups.size())
    return {};
  const auto &group = groups[lodGroup_];
  std::map<int, std::pair<qulonglong, qulonglong>> counts;
  for (std::size_t i = 0; i < classes_.size(); ++i)
    if (classes_[i].group == group && !classes_[i].collision) {
      auto &count = counts[classes_[i].level];
      count.first += document_->meshes[i].points.size();
      count.second += document_->meshes[i].triangles.size() / 3;
    }
  QVariantList result;
  for (const auto &[level, count] : counts) {
    const auto current = renderedState_.levels.find(group);
    const auto request = levels_.find(group);
    result.append(QVariantMap{
        {"level", level},
        {"points", count.first},
        {"triangles", count.second},
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
  return {{"group", item.group},
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
        return item.group == group && item.level == level && !item.collision;
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
