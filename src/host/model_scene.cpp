#include "model_scene.hpp"
#include <QHash>
#include <algorithm>

void ModelScene::replace(const ModelDocument *document) {
  replace(prepare(document));
}
ModelScene::Prepared ModelScene::prepare(const ModelDocument *document) {
  Prepared prepared;
  if (!document)
    return prepared;
  auto root = std::make_unique<QStandardItem>(document->name);
  auto *asset = root.get();
  asset->setData(document->hash + ":root", NodeId);
  asset->setData(document->hash + ":root", StableId);
  asset->setData(-1, MeshIndex);
  asset->setData(false, Bone);
  asset->setData(true, SourceActive);
  QHash<QString, QStandardItem *> nodes;
  std::map<QString, std::unique_ptr<QStandardItem>> owners;
  for (const auto &value : document->nodes) {
    const auto node = value.toMap();
    const auto id = node.value("id").toString();
    const bool camera = std::any_of(document->cameras.begin(), document->cameras.end(),
        [&](const auto &item) { return id == QString("node/%1").arg(item.node); });
    auto item = std::make_unique<QStandardItem>(node.value("name").toString() +
                                                (camera ? " [camera]" : "") +
                                                " [" + id + "]");
    item->setData(id, NodeId);
    item->setData(node.value("stableId", id), StableId);
    item->setData(-1, MeshIndex);
    item->setData(node.value("bone", false), Bone);
    item->setData(node.value("active", true), SourceActive);
    nodes[id] = item.get();
    owners[id] = std::move(item);
  }
  for (const auto &value : document->nodes) {
    const auto node = value.toMap();
    const auto id = node.value("id").toString(),
               parent = node.value("parent").toString();
    auto *target = nodes.value(parent, asset);
    if (target == nodes.value(id))
      target = asset;
    target->appendRow(owners.at(id).release());
  }
  for (std::size_t i = 0; i < document->meshes.size(); ++i) {
    const auto &mesh = document->meshes[i];
    auto item = std::make_unique<QStandardItem>(mesh.name);
    auto *ptr = item.get();
    item->setData(mesh.nodeId, NodeId);
    item->setData(mesh.id, StableId);
    item->setData(static_cast<int>(i), MeshIndex);
    item->setData(false, Bone);
    item->setData(true, SourceActive);
    nodes.value(mesh.nodeId, asset)->appendRow(item.release());
    prepared.meshes[static_cast<int>(i)] = ptr;
  }
  // Populate off-model, then publish one complete subtree. Publishing the root
  // first made views and proxies process every descendant insertion separately.
  prepared.root=std::move(root);
  return prepared;
}
void ModelScene::replace(Prepared prepared) {
  clear();
  meshes_=std::move(prepared.meshes);
  if(prepared.root) invisibleRootItem()->appendRow(prepared.root.release());
}
QModelIndex ModelScene::meshIndex(int mesh) const {
  const auto it = meshes_.find(mesh);
  return it == meshes_.end() ? QModelIndex{} : indexFromItem(it->second);
}
