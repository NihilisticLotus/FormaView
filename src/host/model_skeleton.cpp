#include "model_skeleton.hpp"
#include <QSet>
#include <cmath>

ModelSkeleton::ModelSkeleton(QObject *parent) : QSortFilterProxyModel(parent) {
  setSourceModel(&source_);
  setRecursiveFilteringEnabled(true);
}
QHash<int, QByteArray> ModelSkeleton::roleNames() const {
  return {{Qt::DisplayRole, "jointLabel"},
          {NodeKey, "nodeKey"},
          {JointIndex, "jointIndex"},
          {InfluencePoints, "influencePoints"},
          {InPalette, "inPalette"}};
}
Qt::ItemFlags ModelSkeleton::flags(const QModelIndex &index) const {
  return QSortFilterProxyModel::flags(index) & ~Qt::ItemIsEditable;
}
void ModelSkeleton::setSearchText(const QString &value) {
  if (search_ == value)
    return;
  beginFilterChange();
  search_ = value;
  endFilterChange(Direction::Rows);
  emit filterChanged();
}
void ModelSkeleton::setWeightedOnly(bool value) {
  if (weightedOnly_ == value)
    return;
  beginFilterChange();
  weightedOnly_ = value;
  endFilterChange(Direction::Rows);
  emit filterChanged();
}
bool ModelSkeleton::filterAcceptsRow(int row, const QModelIndex &parent) const {
  const auto index = source_.index(row, 0, parent);
  return (!weightedOnly_ || index.data(InfluencePoints).toULongLong() > 0) &&
         index.data().toString().contains(search_.trimmed(),
                                          Qt::CaseInsensitive);
}
QModelIndex ModelSkeleton::indexForNode(const QString &id, int joint) const {
  if (joint >= 0 && joints_.contains(joint))
    return mapFromSource(joints_.value(joint));
  return mapFromSource(nodes_.value(id));
}
QVariantMap ModelSkeleton::details(const QString &id, int joint) const {
  auto result = details_.value(id);
  if (result.isEmpty())
    return result;
  const auto index = joint >= 0 ? joints_.value(joint) : nodes_.value(id);
  result.insert("boneIndex", index.data(JointIndex));
  result.insert("inPalette", index.data(JointIndex).toInt() >= 0);
  result.insert("points", index.data(InfluencePoints));
  return result;
}
void ModelSkeleton::replace(const ModelDocument *document, int mesh,
                            int binding) {
  nodes_.clear();
  joints_.clear();
  details_.clear();
  source_.clear();
  if (!document || mesh < 0 ||
      mesh >= static_cast<int>(document->meshes.size()))
    return;
  const auto &skins = document->meshes[static_cast<std::size_t>(mesh)].skins;
  if (binding < 0 || binding >= static_cast<int>(skins.size()))
    return;
  const auto &skin = skins[static_cast<std::size_t>(binding)];
  QHash<QString, QList<int>> palette;
  QHash<QString, qulonglong> nodeCounts;
  std::vector<qulonglong> counts(static_cast<std::size_t>(skin.bones.size()));
  for (int i = 0; i < skin.palette.size(); ++i)
    palette[skin.palette[i].toMap().value("nodeId").toString()].append(i);
  for (const auto &point : skin.weights) {
    QSet<int> seen;
    for (const auto &weight : point)
      if (weight.bone >= 0 && weight.bone < skin.bones.size() &&
          std::isfinite(weight.value) && weight.value > 0)
        seen.insert(weight.bone);
    QSet<QString> seenNodes;
    for (int joint : seen) {
      ++counts[static_cast<std::size_t>(joint)];
      seenNodes.insert(skin.palette[joint].toMap().value("nodeId").toString());
    }
    for (const auto &id : seenNodes)
      ++nodeCounts[id];
  }
  QSet<QString> included;
  for (auto it = palette.cbegin(); it != palette.cend(); ++it) {
    auto id = it.key();
    QSet<QString> chain;
    while (!id.isEmpty() && !chain.contains(id)) {
      chain.insert(id);
      included.insert(id);
      id = document->nodeById.value(id).value("parent").toString();
    }
  }
  QHash<QString, QStandardItem *> items;
  std::map<QString, std::unique_ptr<QStandardItem>> owners;
  QHash<QString, int> siblings;
  for (const auto &value : document->nodes) {
    const auto node = value.toMap();
    const auto id = node.value("id").toString();
    if (included.contains(id))
      ++siblings[node.value("parent").toString() + QChar(0) +
                 node.value("name").toString()];
  }
  for (const auto &value : document->nodes) {
    const auto node = value.toMap();
    const auto id = node.value("id").toString();
    if (!included.contains(id))
      continue;
    auto label = node.value("name").toString();
    if (label.isEmpty())
      label = QStringLiteral("—");
    if (siblings.value(node.value("parent").toString() + QChar(0) +
                       node.value("name").toString()) > 1)
      label += " · " + id.section('/', -1);
    auto item = std::make_unique<QStandardItem>(label);
    item->setData(id, NodeKey);
    const auto indices = palette.value(id);
    item->setData(indices.size() == 1 ? indices.front() : -1, JointIndex);
    item->setData(!indices.isEmpty(), InPalette);
    const auto total = nodeCounts.value(id);
    item->setData(QVariant::fromValue(total), InfluencePoints);
    auto detail = node;
    detail.insert("label", label);
    const auto parent =
        document->nodeById.value(node.value("parent").toString());
    detail.insert("parentName", parent.value("name"));
    details_.insert(id, detail);
    items.insert(id, item.get());
    owners[id] = std::move(item);
  }
  for (const auto &value : document->nodes) {
    const auto node = value.toMap();
    const auto id = node.value("id").toString();
    if (!items.contains(id))
      continue;
    auto *target = items.value(node.value("parent").toString(),
                               source_.invisibleRootItem());
    if (target == items.value(id))
      target = source_.invisibleRootItem();
    target->appendRow(owners.at(id).release());
  }
  for (auto it = items.cbegin(); it != items.cend(); ++it) {
    auto *item = it.value();
    nodes_.insert(it.key(), source_.indexFromItem(item));
    const auto indices = palette.value(it.key());
    for (int i : indices) {
      auto *entry = item;
      if (indices.size() > 1) {
        auto owned = std::make_unique<QStandardItem>(item->text() + " · #" +
                                                     QString::number(i));
        owned->setData(it.key(), NodeKey);
        owned->setData(i, JointIndex);
        owned->setData(true, InPalette);
        owned->setData(QVariant::fromValue(counts[static_cast<std::size_t>(i)]),
                       InfluencePoints);
        entry = owned.get();
        item->appendRow(owned.release());
      }
      joints_.insert(i, source_.indexFromItem(entry));
    }
  }
}
