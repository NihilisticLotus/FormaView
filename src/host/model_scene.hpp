#pragma once
#include "model_document.hpp"
#include <QStandardItemModel>

class ModelScene final : public QStandardItemModel {
public:
  enum Role {
    NodeId = Qt::UserRole + 1,
    StableId,
    MeshIndex,
    Bone,
    SourceActive
  };
  explicit ModelScene(QObject *parent = nullptr) : QStandardItemModel(parent) {}
  QHash<int, QByteArray> roleNames() const override {
    return {{Qt::DisplayRole, "sourceLabel"},
            {NodeId, "nodeId"},
            {StableId, "stableId"},
            {MeshIndex, "meshIndex"},
            {Bone, "bone"},
            {SourceActive, "sourceActive"}};
  }
  void replace(const ModelDocument *);
  QModelIndex meshIndex(int mesh) const;
  Qt::ItemFlags flags(const QModelIndex &index) const override {
    return QStandardItemModel::flags(index) & ~Qt::ItemIsEditable;
  }

private:
  std::map<int, QStandardItem *> meshes_;
};
