#pragma once
#include "formaview/scheduler.hpp"
#include <QAbstractListModel>
#include <QImage>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <memory>

struct FileEntry {
  QString path, name, kind;
  qint64 bytes{}, modified{};
  bool directory{};
};
class FileList final : public QAbstractListModel {
  Q_OBJECT
public:
  enum Role {
    Path = Qt::UserRole + 1,
    Name,
    Kind,
    Bytes,
    Modified,
    Directory,
    Selected
  };
  explicit FileList(QObject *parent = nullptr) : QAbstractListModel(parent) {}
  int rowCount(const QModelIndex &parent = {}) const override;
  QVariant data(const QModelIndex &, int) const override;
  QHash<int, QByteArray> roleNames() const override;
  void replace(std::vector<FileEntry> entries);
  void select(int row, int modifiers);
  void restoreSelection(const QSet<QString> &paths);
  const FileEntry *at(int row) const;
  int indexOf(const QString &path) const;
  const QSet<QString> &selection() const { return selected_; }

private:
  std::vector<FileEntry> entries_;
  QSet<QString> selected_;
  int anchor_{};
};

class Desktop final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QAbstractItemModel *files READ files CONSTANT)
  Q_PROPERTY(QString directory READ directory NOTIFY changed)
  Q_PROPERTY(QString query READ query NOTIFY changed)
  Q_PROPERTY(int typeFilter READ typeFilter NOTIFY changed)
  Q_PROPERTY(int sortOrder READ sortOrder NOTIFY changed)
  Q_PROPERTY(int currentIndex READ currentIndex NOTIFY changed)
  Q_PROPERTY(int count READ count NOTIFY changed)
  Q_PROPERTY(int selectedCount READ selectedCount NOTIFY changed)
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(bool canBack READ canBack NOTIFY changed)
  Q_PROPERTY(bool canForward READ canForward NOTIFY changed)
  Q_PROPERTY(QString error READ error NOTIFY changed)
  Q_PROPERTY(QString requested READ requested NOTIFY changed)
  Q_PROPERTY(QVariantMap asset READ asset NOTIFY changed)
  Q_PROPERTY(qulonglong sessionRevision READ sessionRevision NOTIFY changed)
  Q_PROPERTY(QVariantList tasks READ tasks NOTIFY tasksChanged)
  Q_PROPERTY(int activeTasks READ activeTasks NOTIFY tasksChanged)
  Q_PROPERTY(
      QString language READ language WRITE setLanguage NOTIFY languageChanged)
  Q_PROPERTY(QVariantMap preferences READ preferences NOTIFY preferencesChanged)
  Q_PROPERTY(QStringList recent READ recent NOTIFY changed)
  Q_PROPERTY(QVariantList resourceIssues READ resourceIssues NOTIFY changed)
  Q_PROPERTY(double scrollPosition READ scrollPosition NOTIFY viewRestored)
public:
  explicit Desktop(QObject *parent = nullptr);
  ~Desktop() override;
  QAbstractItemModel *files() { return &files_; }
  QString directory() const { return directory_; }
  QString query() const { return query_; }
  int typeFilter() const { return type_; }
  int sortOrder() const { return sort_; }
  int currentIndex() const { return current_; }
  int count() const { return files_.rowCount(); }
  int selectedCount() const {
    return static_cast<int>(files_.selection().size());
  }
  bool busy() const { return scanning_ || filtering_; }
  bool canBack() const { return historyAt_ > 0; }
  bool canForward() const { return historyAt_ + 1 < history_.size(); }
  QString error() const { return error_; }
  QString requested() const {
    return error_.isEmpty() ? requested_ : errorTarget_;
  }
  QVariantMap asset() const { return asset_; }
  qulonglong sessionRevision() const { return sessionRevision_; }
  QVariantList tasks() const { return taskRows_; }
  int activeTasks() const { return active_; }
  QString language() const { return language_; }
  void setLanguage(const QString &);
  QVariantMap preferences() const { return preferences_; }
  QStringList recent() const { return recent_; }
  QVariantList resourceIssues() const { return resourceIssues_; }
  // Adapter entry: only reports belonging to the currently displayed session
  // are accepted.
  bool reportMissingResource(quint64 session, const QString &id,
                             const QString &expected);
  Q_INVOKABLE void validateResource(const QString &id,
                                    const QString &candidate);
  Q_INVOKABLE void applyResource(const QString &id);
  double scrollPosition() const { return scroll_; }
  Q_INVOKABLE void navigate(const QString &path);
  Q_INVOKABLE void history(int offset);
  Q_INVOKABLE void parentDirectory();
  Q_INVOKABLE void refresh();
  Q_INVOKABLE void filter(const QString &text, int type, int order);
  Q_INVOKABLE void select(int row, int modifiers = 0);
  Q_INVOKABLE void selectAll();
  Q_INVOKABLE void activate(int row);
  Q_INVOKABLE void openPath(const QString &path);
  Q_INVOKABLE void openUrls(const QList<QUrl> &urls);
  Q_INVOKABLE void cancel(const QString &id);
  Q_INVOKABLE void retry(const QString &id);
  Q_INVOKABLE void copy(const QString &text);
  Q_INVOKABLE void reveal(const QString &path);
  Q_INVOKABLE void saveView(double position);
  Q_INVOKABLE void preference(const QString &key, const QVariant &value);
  Q_INVOKABLE void clearAsset();
  Q_INVOKABLE void dismissError();
  Q_INVOKABLE QString report() const;
signals:
  void changed();
  void tasksChanged();
  void languageChanged();
  void preferencesChanged();
  void viewRestored();

private:
  struct Result {
    std::vector<FileEntry> entries;
    QString path, error;
    QString resourceId;
    QVariantMap asset;
    bool isDirectory{};
    quint64 navigationRevision{};
    quint64 assetRequestRevision{};
  };
  struct Job {
    formaview::TaskHandle handle;
    std::shared_ptr<Result> result;
    QString operation, path;
    quint64 revision{};
    bool consumed{};
    int historyTarget{-1};
  };
  struct View {
    QString query, current;
    int type{}, sort{};
    double scroll{};
    QSet<QString> selection;
  };
  void scan(const QString &, int historyTarget = -1);
  void
  start(QString operation, QString path, quint64 revision,
        std::function<formaview::TaskResult(std::stop_token, Result &)> work,
        int historyTarget = -1, QString resourceId = {});
  void poll();
  void saveDirectory();
  void publish(Job &);
  FileList files_;
  formaview::Scheduler scheduler_;
  std::vector<Job> jobs_;
  std::shared_ptr<const std::vector<FileEntry>> source_;
  QTimer pollTimer_;
  QString directory_, query_, error_, errorTarget_, requested_,
      language_{"zh-CN"};
  QVariantMap asset_, preferences_;
  QVariantList taskRows_, resourceIssues_;
  QMap<QString, View> views_;
  QStringList viewOrder_, history_, recent_;
  int historyAt_{-1}, type_{}, sort_{}, current_{-1}, active_{};
  quint64 directoryRevision_{}, filterRevision_{}, assetRevision_{},
      sessionRevision_{}, resourceRevision_{};
  double scroll_{};
  bool scanning_{}, filtering_{};
};
