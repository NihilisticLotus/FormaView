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
  replace(prepare(document,mesh,binding));
}
ModelSkeleton::Prepared ModelSkeleton::prepare(const ModelDocument *document,
                                               int mesh,int binding) {
  Prepared prepared;
  if (!document)
    return prepared;
  if (document->skeletonOnly) {
    // Animation documents (standalone PSA, animation-only FBX) expose the
    // bone hierarchy directly. Raw unpaired records keep their verbatim
    // order; the document warnings already mark the hierarchy unverified.
    QHash<QString, int> duplicates;
    for (const auto &name : document->boneNames)
      ++duplicates[name];
    QHash<QString, QStandardItem *> items;
    std::map<QString, std::unique_ptr<QStandardItem>> owners;
    for (int i = 0; i < static_cast<int>(document->boneNames.size()); ++i) {
      const auto id = QStringLiteral("bone/%1").arg(i);
      auto label = document->boneNames[static_cast<std::size_t>(i)];
      if (label.isEmpty())
        label = QStringLiteral("—");
      if (duplicates.value(label) > 1)
        label += QStringLiteral(" · #") + QString::number(i);
      auto item = std::make_unique<QStandardItem>(label);
      item->setData(id, NodeKey);
      item->setData(i, JointIndex);
      item->setData(false, InPalette);
      item->setData(QVariant::fromValue(qulonglong{0}), InfluencePoints);
      const auto parent = document->boneParents[static_cast<std::size_t>(i)];
      prepared.details.insert(
          id, QVariantMap{{"id", id},
                          {"name", document->boneNames[static_cast<std::size_t>(i)]},
                          {"boneIndex", i},
                          {"parentIndex", parent},
                          {"inPalette", false}});
      items.insert(id, item.get());
      owners[id] = std::move(item);
    }
    std::vector<std::unique_ptr<QStandardItem>> roots;
    for (int i = 0; i < static_cast<int>(document->boneNames.size()); ++i) {
      const auto id = QStringLiteral("bone/%1").arg(i);
      const auto parent = document->boneParents[static_cast<std::size_t>(i)];
      auto *target = parent >= 0 && parent < static_cast<int>(document->boneNames.size())
                         ? items.value(QStringLiteral("bone/%1").arg(parent))
                         : nullptr;
      if (target && target != items.value(id))
        target->appendRow(owners.at(id).release());
      else
        roots.push_back(std::move(owners.at(id)));
    }
    prepared.roots=std::move(roots);
    prepared.nodes=std::move(items);
    for (int i = 0; i < static_cast<int>(document->boneNames.size()); ++i)
      prepared.joints.insert(i,prepared.nodes.value(QStringLiteral("bone/%1").arg(i)));
    return prepared;
  }
  if (mesh < 0 || mesh >= static_cast<int>(document->meshes.size()))
    return prepared;
  const auto &skins = document->meshes[static_cast<std::size_t>(mesh)].skins;
  if (binding < 0 || binding >= static_cast<int>(skins.size()))
    return prepared;
  const auto &skin = skins[static_cast<std::size_t>(binding)];
  QHash<QString, QList<int>> palette;
  QHash<QString, qulonglong> nodeCounts;
  std::vector<qulonglong> counts(static_cast<std::size_t>(skin.bones.size()));
  QHash<QString, int> nodeSlots;
  QStringList nodeIds;
  std::vector<int> jointNodes(static_cast<std::size_t>(skin.bones.size()), -1);
  for (int i = 0; i < skin.palette.size(); ++i) {
    const auto id = skin.palette[i].toMap().value("nodeId").toString();
    palette[id].append(i);
    if (!nodeSlots.contains(id)) {
      nodeSlots.insert(id, static_cast<int>(nodeIds.size()));
      nodeIds.append(id);
    }
    if (i < skin.bones.size())
      jointNodes[static_cast<std::size_t>(i)] = nodeSlots.value(id);
  }
  std::vector<std::size_t> seenJoints(counts.size()),
      seenNodes(static_cast<std::size_t>(nodeIds.size()));
  std::vector<qulonglong> nodeTotals(static_cast<std::size_t>(nodeIds.size()));
  // Stamp arrays count each source point once without allocating two sets per
  // point, or repeatedly converting palette QVariantMaps in this GUI callback.
  std::size_t stamp = 0;
  for (const auto &point : skin.weights) {
    ++stamp;
    for (const auto &weight : point) {
      if (weight.bone >= 0 && weight.bone < skin.bones.size() &&
          std::isfinite(weight.value) && weight.value > 0) {
        const auto joint = static_cast<std::size_t>(weight.bone);
        if (seenJoints[joint] != stamp) {
          ++counts[joint];
          seenJoints[joint] = stamp;
        }
        const auto node = jointNodes[joint];
        if (node >= 0 && seenNodes[static_cast<std::size_t>(node)] != stamp) {
          ++nodeTotals[static_cast<std::size_t>(node)];
          seenNodes[static_cast<std::size_t>(node)] = stamp;
        }
      }
    }
  }
  for (int i = 0; i < nodeIds.size(); ++i)
    nodeCounts.insert(nodeIds[i], nodeTotals[static_cast<std::size_t>(i)]);
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
    prepared.details.insert(id, detail);
    items.insert(id, item.get());
    owners[id] = std::move(item);
  }
  std::vector<std::unique_ptr<QStandardItem>> roots;
  for (const auto &value : document->nodes) {
    const auto node = value.toMap();
    const auto id = node.value("id").toString();
    if (!items.contains(id))
      continue;
    auto *target = items.value(node.value("parent").toString());
    if (target == items.value(id))
      target = nullptr;
    if (target)
      target->appendRow(owners.at(id).release());
    else
      roots.push_back(std::move(owners.at(id)));
  }
  QHash<int, QStandardItem *> jointItems;
  for (auto it = items.cbegin(); it != items.cend(); ++it) {
    auto *item = it.value();
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
      jointItems.insert(i, entry);
    }
  }
  prepared.roots=std::move(roots);
  prepared.nodes=std::move(items);
  prepared.joints=std::move(jointItems);
  return prepared;
}
void ModelSkeleton::replace(Prepared prepared) {
  nodes_.clear();
  joints_.clear();
  details_=std::move(prepared.details);
  source_.clear();
  QList<QStandardItem *> rows;
  rows.reserve(static_cast<qsizetype>(prepared.roots.size()));
  for(auto &root:prepared.roots) rows.append(root.release());
  source_.invisibleRootItem()->appendRows(rows);
  for(auto it=prepared.nodes.cbegin();it!=prepared.nodes.cend();++it)
    nodes_.insert(it.key(),source_.indexFromItem(it.value()));
  for(auto it=prepared.joints.cbegin();it!=prepared.joints.cend();++it)
    joints_.insert(it.key(),source_.indexFromItem(it.value()));
}
