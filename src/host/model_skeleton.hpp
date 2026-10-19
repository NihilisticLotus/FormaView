#pragma once
#include "model_document.hpp"
#include <QPersistentModelIndex>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>

class ModelSkeleton final : public QSortFilterProxyModel {
  Q_OBJECT
  Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY
                 filterChanged)
  Q_PROPERTY(bool weightedOnly READ weightedOnly WRITE setWeightedOnly NOTIFY
                 filterChanged)
public:
  enum Role {
    NodeKey = Qt::UserRole + 1,
    JointIndex,
    InfluencePoints,
    InPalette
  };
  explicit ModelSkeleton(QObject *parent = nullptr);
  QHash<int, QByteArray> roleNames() const override;
  Qt::ItemFlags flags(const QModelIndex &index) const override;
  void replace(const ModelDocument *, int mesh, int binding);
  QString searchText() const { return search_; }
  bool weightedOnly() const { return weightedOnly_; }
  void setSearchText(const QString &);
  void setWeightedOnly(bool);
  Q_INVOKABLE QModelIndex indexForNode(const QString &, int joint = -1) const;
  QVariantMap details(const QString &, int joint) const;
signals:
  void filterChanged();

protected:
  bool filterAcceptsRow(int, const QModelIndex &) const override;

private:
  QStandardItemModel source_;
  QHash<QString, QPersistentModelIndex> nodes_;
  QHash<int, QPersistentModelIndex> joints_;
  QHash<QString, QVariantMap> details_;
  QString search_;
  bool weightedOnly_{};
};
