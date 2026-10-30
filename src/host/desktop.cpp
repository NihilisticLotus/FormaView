#include "desktop.hpp"
#include "window_chrome.hpp"
#include <QQuickWindow>
#ifdef FORMAVIEW_MODEL_UI
#include "model_session.hpp"
#endif
#ifdef FORMAVIEW_IMAGE_UI
#include "image_jobs.hpp"
#include "image_session.hpp"
#endif
#include <QClipboard>
#include <QCollator>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QUrl>
#include <algorithm>
#include <filesystem>

namespace {
QString localPath(const QString &input) {
  if (input.trimmed().isEmpty())
    return {};
  const auto url = QUrl(input);
  const auto path = url.isLocalFile()
                        ? url.toLocalFile()
                        : QDir::fromNativeSeparators(input.trimmed());
  return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}
formaview::TaskResult failure(formaview::ErrorCode code, const QString &text) {
  auto result = formaview::TaskResult{};
  result.error = formaview::Error{code, text.toStdString(), {}, true};
  return result;
}
QString kind(const QString &name) {
  const auto suffix = QFileInfo(name).suffix().toLower();
  if (QStringList{"png", "jpg", "jpeg", "jpe", "jfif", "bmp", "dib", "gif",
                  "tif", "tiff", "tga", "webp", "ico", "dds", "hdr", "rgbe",
                  "exr"}
          .contains(suffix))
    return "image";
  if (QStringList{"fbx", "obj", "gltf", "glb", "psk", "pskx"}.contains(suffix))
    return "model";
  if (suffix == "psa")
    return "animation";
  // PMX/VRM convert to FBX but have no professional preview in this release.
  if (suffix == "pmx" || suffix == "vrm")
    return "convert";
  // ABC is a future-version format: listed only under "all types".
  return "other";
}
QString stateName(formaview::TaskState state) {
  using S = formaview::TaskState;
  switch (state) {
  case S::Queued:
    return "queued";
  case S::Running:
    return "running";
  case S::Cancelling:
    return "cancelling";
  case S::Succeeded:
    return "succeeded";
  case S::PartiallySucceeded:
    return "partial";
  case S::Failed:
    return "failed";
  case S::Cancelled:
    return "cancelled";
  case S::StaleDiscarded:
    return "stale";
  case S::Interrupted:
    return "interrupted";
  }
  return "unknown";
}
} // namespace
int FileList::rowCount(const QModelIndex &parent) const {
  return parent.isValid() ? 0 : static_cast<int>(entries_.size());
}
const FileEntry *FileList::at(int row) const {
  return row >= 0 && row < rowCount() ? &entries_[static_cast<size_t>(row)]
                                      : nullptr;
}
QVariant FileList::data(const QModelIndex &index, int role) const {
  const auto *e = at(index.row());
  if (!e)
    return {};
  switch (role) {
  case Path:
    return e->path;
  case Name:
    return e->name;
  case Kind:
    return e->kind;
  case Bytes:
    return e->bytes;
  case Modified:
    return e->modified;
  case Directory:
    return e->directory;
  case Selected:
    return selected_.contains(e->path);
  default:
    return {};
  }
}
QHash<int, QByteArray> FileList::roleNames() const {
  return {{Path, "filePath"},         {Name, "fileName"},
          {Kind, "fileKind"},         {Bytes, "fileBytes"},
          {Modified, "fileModified"}, {Directory, "isDirectory"},
          {Selected, "selected"}};
}
void FileList::replace(std::vector<FileEntry> entries) {
  beginResetModel();
  entries_ = std::move(entries);
  endResetModel();
}
int FileList::indexOf(const QString &path) const {
  const auto it = std::find_if(entries_.begin(), entries_.end(),
                               [&](const auto &e) { return e.path == path; });
  return it == entries_.end() ? -1 : static_cast<int>(it - entries_.begin());
}
void FileList::restoreSelection(const QSet<QString> &paths) {
  selected_ = paths;
  if (rowCount())
    emit dataChanged(index(0), index(rowCount() - 1), {Selected});
}
void FileList::select(int row, int modifiers) {
  const auto *entry = at(row);
  if (!entry)
    return;
  if (modifiers & Qt::ShiftModifier) {
    if (!(modifiers & Qt::ControlModifier))
      selected_.clear();
    for (auto i = std::max(0, std::min(anchor_, row));
         i <= std::max(anchor_, row) && i < rowCount(); ++i)
      selected_.insert(entries_[static_cast<size_t>(i)].path);
  } else if (modifiers & Qt::ControlModifier) {
    if (!selected_.remove(entry->path))
      selected_.insert(entry->path);
    anchor_ = row;
  } else {
    selected_ = {entry->path};
    anchor_ = row;
  }
  if (rowCount())
    emit dataChanged(index(0), index(rowCount() - 1), {Selected});
}
int Desktop::activeTasks() const {
  int count = active_;
#ifdef FORMAVIEW_IMAGE_UI
  if (imageJobs_) for (const auto &v : static_cast<ImageJobs *>(imageJobs_.get())->jobs()) {
    const auto state = v.toMap().value("state").toString();
    if (state == "queued" || state == "running") ++count;
  }
#endif
  return count;
}
void Desktop::styleToolWindow(QObject *window) {
  installWindowChrome(qobject_cast<QQuickWindow *>(window));
}
Desktop::Desktop(QObject *parent) : QObject(parent), files_(this) {
  recentWriter_.setMaxThreadCount(1);
#ifdef FORMAVIEW_MODEL_UI
  model_ = std::make_unique<ModelSession>();
#endif
#ifdef FORMAVIEW_IMAGE_UI
  image_ = std::make_unique<ImageSession>();
  imageJobs_ = std::make_unique<ImageJobs>();
  connect(static_cast<ImageJobs *>(imageJobs_.get()), &ImageJobs::changed, this, &Desktop::tasksChanged);
#endif
  auto settings = QSettings{};
  language_ = settings.value("language", "zh-CN").toString();
  preferences_ = settings.value("preferences").toMap();
  if (preferences_.contains("restoreLayout") &&
      !preferences_.value("restoreLayout").toBool()) {
    preferences_.remove("fileWidth");
    preferences_["filesVisible"] =
        preferences_.value("defaultFilesVisible", true);
  }
  recent_ = settings.value("recent").toStringList().mid(0, 20);
  pollTimer_.setInterval(30);
  pollTimer_.setObjectName("desktopJobPoll");
  connect(&pollTimer_, &QTimer::timeout, this, &Desktop::poll);
  pollTimer_.start();
}
Desktop::~Desktop() {
  pollTimer_.stop();
  scheduler_.shutdown();
  recentWriter_.waitForDone();
}
void Desktop::setLanguage(const QString &value) {
  if (value != language_ &&
      QStringList{"en", "zh-CN", "zh-TW"}.contains(value)) {
    language_ = value;
    QSettings{}.setValue("language", value);
    emit languageChanged();
  }
}
void Desktop::preference(const QString &key, const QVariant &value) {
  if (!QStringList{"fileWidth", "thumbnails", "filesVisible", "mcpInputRoot",
                   "mcpOutputRoot", "mcpPython", "mcpAdapter", "density",
                   "restoreLayout", "defaultFilesVisible", "inspectorMode"}
           .contains(key))
    return;
  if (key == "density" &&
      !QStringList{"standard", "compact"}.contains(value.toString()))
    return;
  if (key == "inspectorMode" &&
      !QStringList{"auto", "always", "hidden"}.contains(value.toString()))
    return;
  if (preferences_.value(key) == value)
    return;
  preferences_[key] = value;
  QSettings{}.setValue("preferences", preferences_);
  emit preferencesChanged();
}
void Desktop::saveView(double position) { scroll_ = std::max(0.0, position); }
void Desktop::saveDirectory() {
  if (directory_.isEmpty())
    return;
  views_[directory_] = {
      query_,  files_.at(current_) ? files_.at(current_)->path : QString{},
      type_,   sort_,
      scroll_, files_.selection()};
  viewOrder_.removeAll(directory_);
  viewOrder_.append(directory_);
  while (viewOrder_.size() > 50)
    views_.remove(viewOrder_.takeFirst());
  auto bytes = quint64{};
  for (auto it = views_.cbegin(); it != views_.cend(); ++it)
    for (const auto &path : it->selection)
      bytes += static_cast<quint64>(path.size() * 2 + 64);
  while (bytes > 32ULL * 1024 * 1024 && !viewOrder_.isEmpty()) {
    const auto oldest = viewOrder_.takeFirst();
    for (const auto &path : views_[oldest].selection)
      bytes -= static_cast<quint64>(path.size() * 2 + 64);
    views_.remove(oldest);
  }
}
void Desktop::start(
    QString operation, QString path, quint64 revision,
    std::function<formaview::TaskResult(std::stop_token, Result &)> work,
    int historyTarget, QString resourceId) {
  // Replaced requests are cancelled before another bounded submission.
  for (const auto &job : jobs_)
    if (job.operation == operation && !job.consumed)
      job.handle.cancel();
  auto output = std::make_shared<Result>();
  output->path = path;
  output->resourceId = std::move(resourceId);
  output->assetRequestRevision = assetRevision_;
  auto request = formaview::TaskRequest{};
  request.priority = operation == "scan" || operation == "filter"
                         ? formaview::Priority::Background
                         : formaview::Priority::Foreground;
  request.resources = {1, operation == "filter" ? 0U : 1U, 32ULL * 1024 * 1024,
                       0};
  request.work = [output, work = std::move(work)](std::stop_token stop) {
    return work(stop, *output);
  };
  auto submitted = scheduler_.submit(std::move(request));
  if (!submitted.task) {
    if (operation == "folders") {
      folderMenu_["busy"] = false;
      folderMenu_["error"] = QString::fromStdString(submitted.error->message);
      emit folderMenuChanged();
      return;
    }
    error_ = QString::fromStdString(submitted.error->message);
    errorTarget_ = path;
    if (operation == "scan")
      scanning_ = false;
    if (operation == "filter")
      filtering_ = false;
    if (operation == "resource")
      for (auto &value : resourceIssues_) {
        auto issue = value.toMap();
        if (issue["id"] == output->resourceId) {
          issue["state"] = "missing";
          issue["detail"] = error_;
          value = issue;
        }
      }
    emit changed();
    return;
  }
  jobs_.push_back({std::move(*submitted.task), std::move(output),
                   std::move(operation), std::move(path), revision, false,
                   historyTarget});
  poll();
}
void Desktop::navigate(const QString &path) { scan(localPath(path)); }
QVariantList Desktop::drives() const {
  QVariantList result;
  for (const auto &drive : QDir::drives()) {
    const auto path = QDir::fromNativeSeparators(drive.absoluteFilePath());
    if (path.isEmpty())
      continue;
    result.append(QVariantMap{{"name", path}, {"path", path}});
  }
  return result;
}
QVariantList Desktop::breadcrumbs(const QString &input) const {
  // Pure lexical splitting: never stat a path on the GUI thread (UNC included).
  auto path = QDir::fromNativeSeparators(input);
  auto result = QVariantList{};
  if (path.isEmpty())
    return result;
  auto root = QString{};
  if (path.startsWith("//")) {
    const auto server = path.indexOf('/', 2);
    const auto share = server < 0 ? -1 : path.indexOf('/', server + 1);
    root = share < 0 ? path : path.left(share);
  } else if (path.size() >= 2 && path[1] == ':') {
    root = path.left(2) + '/';
  } else if (path.startsWith('/'))
    root = "/";
  if (root.isEmpty())
    return result;
  result.append(QVariantMap{{"name", root.endsWith(":/") ? root.left(2) : root},
                            {"path", root}});
  auto current = root;
  for (const auto &part :
       path.mid(root.size()).split('/', Qt::SkipEmptyParts)) {
    if (!current.endsWith('/'))
      current += '/';
    current += part;
    result.append(QVariantMap{{"name", part}, {"path", current}});
  }
  return result;
}
void Desktop::closeFolderMenu() {
  ++folderRevision_;
  for (const auto &job : jobs_)
    if (job.operation == "folders" && !job.consumed)
      job.handle.cancel();
  folderMenu_.clear();
  emit folderMenuChanged();
}
void Desktop::listFolders(const QString &input) {
  const auto path = localPath(input);
  if (path.isEmpty()) {
    closeFolderMenu();
    return;
  }
  folderMenu_ = {{"path", path},
                 {"busy", true},
                 {"error", ""},
                 {"entries", QVariantList{}}};
  emit folderMenuChanged();
  start("folders", path, ++folderRevision_,
        [path](std::stop_token stop, Result &out) {
          auto ec = std::error_code{};
          auto iterator = std::filesystem::directory_iterator(
              std::filesystem::path(path.toStdWString()), ec);
          if (ec) {
            out.error = QString::fromStdString(ec.message());
            return failure(formaview::ErrorCode::Io, out.error);
          }
          auto visited = 0;
          auto bytes = quint64{};
          for (; iterator != std::filesystem::directory_iterator{};
               iterator.increment(ec)) {
            if (stop.stop_requested())
              return failure(formaview::ErrorCode::Cancelled, "Cancelled");
            if (ec)
              break;
            if (++visited > 100000 || out.entries.size() >= 4096 ||
                bytes > 4ULL * 1024 * 1024) {
              out.error = "Folder list exceeds its metadata budget";
              return failure(formaview::ErrorCode::ResourceLimit, out.error);
            }
            if (!iterator->is_directory(ec)) {
              if (ec)
                break;
              continue;
            }
            const auto name =
                QString::fromStdWString(iterator->path().filename().wstring());
            const auto child = QDir::fromNativeSeparators(
                QString::fromStdWString(iterator->path().wstring()));
            bytes += static_cast<quint64>((child.size() + name.size()) * 2 +
                                          sizeof(FileEntry));
            out.entries.push_back({child, name, "folder", 0, 0, true});
          }
          if (ec) {
            out.error = QString::fromStdString(ec.message());
            return failure(formaview::ErrorCode::Io, out.error);
          }
          auto collator = QCollator{};
          collator.setNumericMode(true);
          collator.setCaseSensitivity(Qt::CaseInsensitive);
          std::sort(out.entries.begin(), out.entries.end(),
                    [&collator](const auto &a, const auto &b) {
                      const auto order = collator.compare(a.name, b.name);
                      return order ? order < 0 : a.path < b.path;
                    });
          return formaview::TaskResult{};
        });
}
void Desktop::scan(const QString &path, int historyTarget) {
  if (path.isEmpty())
    return;
  closeFolderMenu();
  saveDirectory();
  scanning_ = true;
  filtering_ = false;
  error_.clear();
  requested_ = path;
  ++filterRevision_;
  const auto rev = ++directoryRevision_;
  emit changed();
  start(
      "scan", path, rev,
      [path](std::stop_token stop, Result &out) {
        auto ec = std::error_code{};
        const auto root = std::filesystem::path(path.toStdWString());
        if (!std::filesystem::is_directory(root, ec) || ec) {
          out.error = "Directory unavailable: " + path;
          return failure(formaview::ErrorCode::Io, out.error);
        }
        auto iterator = std::filesystem::directory_iterator(root, ec);
        if (ec) {
          out.error = QString::fromStdString(ec.message());
          return failure(formaview::ErrorCode::Io, out.error);
        }
        auto allocated = quint64{};
        for (; iterator != std::filesystem::directory_iterator{};
             iterator.increment(ec)) {
          if (stop.stop_requested())
            return failure(formaview::ErrorCode::Cancelled, "Cancelled");
          if (ec) {
            out.error = QString::fromStdString(ec.message());
            return failure(formaview::ErrorCode::Io, out.error);
          }
          if (out.entries.size() >= 100000) {
            out.error = "Directory exceeds 100000 entry budget";
            return failure(formaview::ErrorCode::ResourceLimit, out.error);
          }
          const auto info =
              QFileInfo(QString::fromStdWString(iterator->path().wstring()));
          allocated += sizeof(FileEntry) +
                       static_cast<quint64>((info.absoluteFilePath().size() +
                                             info.fileName().size() + 16) *
                                            2);
          if (allocated > 16ULL * 1024 * 1024) {
            out.error = "Directory metadata exceeds 16 MiB budget";
            return failure(formaview::ErrorCode::ResourceLimit, out.error);
          }
          out.entries.push_back(
              {QDir::fromNativeSeparators(info.absoluteFilePath()),
               info.fileName(), kind(info.fileName()), info.size(),
               info.lastModified().toMSecsSinceEpoch(), info.isDir()});
        }
        if (ec) {
          out.error = QString::fromStdString(ec.message());
          return failure(formaview::ErrorCode::Io, out.error);
        }
        return formaview::TaskResult{};
      },
      historyTarget);
}
void Desktop::history(int offset) {
  const auto at = historyAt_ + offset;
  if (at >= 0 && at < history_.size())
    scan(history_[at], at);
}
void Desktop::parentDirectory() {
  if (directory_.isEmpty())
    return;
  // Lexical navigation only. QDir::cdUp() checks existence synchronously.
  const auto path = std::filesystem::path(directory_.toStdWString());
  const auto parent = path.parent_path();
  const auto serverEnd = directory_.indexOf('/', 2);
  const auto uncRoot =
      directory_.startsWith("//") &&
      (serverEnd < 0 || directory_.indexOf('/', serverEnd + 1) < 0);
  if (!uncRoot && !parent.empty() && parent != path)
    navigate(QString::fromStdWString(parent.wstring()));
}
void Desktop::refresh() {
  if (!directory_.isEmpty())
    scan(directory_, historyAt_);
}
void Desktop::filter(const QString &text, int type, int order) {
  if (text != query_ || type != type_)
    scroll_ = 0;
  query_ = text.left(512);
  type_ = std::clamp(type, 0, 5);
  sort_ = std::clamp(order, 0, 3);
  emit changed();
  if (!source_)
    return;
  filtering_ = true;
  emit changed();
  const auto data = source_;
  const auto query = query_;
  const auto category = type_;
  const auto sort = sort_;
  const auto revision = ++filterRevision_;
  start("filter", directory_, revision,
        [data, query, category, sort](std::stop_token stop, Result &out) {
          const auto types =
              QStringList{"", "image", "model", "animation", "", ""};
          for (const auto &entry : *data) {
            if (stop.stop_requested())
              return failure(formaview::ErrorCode::Cancelled, "Cancelled");
            // Default (0) lists folders plus previewable files only;
            // 5 is the explicit "every file" escape hatch.
            const bool accepted =
                entry.directory || category == 5 ||
                (category == 0 ? entry.kind != QStringLiteral("other")
                               : category == 2 ? entry.kind == types[2] || entry.kind == QStringLiteral("convert")
                                               : entry.kind == types[category]);
            if (entry.name.contains(query, Qt::CaseInsensitive) && accepted)
              out.entries.push_back(entry);
          }
          auto collator = QCollator(QLocale(QLocale::English));
          collator.setNumericMode(true);
          collator.setCaseSensitivity(Qt::CaseInsensitive);
          std::sort(out.entries.begin(), out.entries.end(),
                    [&](const auto &a, const auto &b) {
                      if (a.directory != b.directory)
                        return a.directory;
                      if (sort == 2 && a.modified != b.modified)
                        return a.modified > b.modified;
                      if (sort == 3 && a.bytes != b.bytes)
                        return a.bytes > b.bytes;
                      const auto c = collator.compare(a.name, b.name);
                      if (c)
                        return sort == 1 ? c > 0 : c < 0;
                      return a.path < b.path;
                    });
          if (stop.stop_requested())
            return failure(formaview::ErrorCode::Cancelled, "Cancelled");
          return formaview::TaskResult{};
        });
}
void Desktop::select(int row, int modifiers) {
  const auto *entry = files_.at(row);
  if (!entry)
    return;
  current_ = row;
  files_.select(row, modifiers);
  emit changed();
  if (!(modifiers & (Qt::ControlModifier | Qt::ShiftModifier)) &&
      !entry->directory)
    openPath(entry->path);
}
void Desktop::activate(int row) {
  const auto *entry = files_.at(row);
  if (!entry)
    return;
  if (entry->directory)
    navigate(entry->path);
  else
    openPath(entry->path);
}
void Desktop::selectAll() {
  auto paths = QSet<QString>{};
  for (auto i = 0; i < files_.rowCount(); ++i)
    paths.insert(files_.at(i)->path);
  files_.restoreSelection(paths);
  emit changed();
}
void Desktop::openPath(const QString &input) {
  for (auto &job : jobs_)
    if (job.operation == "resource" && !job.consumed)
      job.handle.cancel();
  for (auto &value : resourceIssues_) {
    auto issue = value.toMap();
    if (issue["state"] == "checking") {
      issue["state"] = "missing";
      value = issue;
    }
  }
  const auto path = localPath(input);
  requested_ = path;
  error_.clear();
  emit changed();
  const auto navigationRevision = directoryRevision_;
  start("inspect", path, ++assetRevision_,
        [path, navigationRevision](std::stop_token stop, Result &out) {
          out.navigationRevision = navigationRevision;
          if (stop.stop_requested())
            return failure(formaview::ErrorCode::Cancelled, "Cancelled");
          const auto info = QFileInfo(path);
          if (info.isDir()) {
            out.isDirectory = true;
            return formaview::TaskResult{};
          }
          auto file = QFile(path);
          if (!file.open(QIODevice::ReadOnly)) {
            out.error = file.errorString();
            return failure(formaview::ErrorCode::Io, out.error);
          }
          const auto header = file.read(4096);
          if (file.error() != QFileDevice::NoError) {
            out.error = file.errorString();
            return failure(formaview::ErrorCode::Io, out.error);
          }
          if (stop.stop_requested())
            return failure(formaview::ErrorCode::Cancelled, "Cancelled");
          out.asset = {{"path", path},
                       {"name", info.fileName()},
                       {"bytes", info.size()},
                       {"modified", info.lastModified().toString(Qt::ISODate)},
                       {"kind", kind(info.fileName())},
                       {"suffix", info.suffix().toUpper()},
                       {"headerBytes", header.size()}};
          return formaview::TaskResult{};
        });
}
void Desktop::openUrls(const QList<QUrl> &urls) {
  if (!urls.isEmpty() && urls.first().isLocalFile())
    openPath(urls.first().toLocalFile());
}
QStringList Desktop::selectedImages() const {
  QStringList result;
  for (const auto &path : files_.selection())
    if (kind(path) == "image")
      result.push_back(path);
  result.sort(Qt::CaseInsensitive);
  if (result.isEmpty() && asset_.value("kind") == "image")
    result.push_back(asset_.value("path").toString());
  return result;
}
QStringList Desktop::selectedModels() const {
  QStringList result;
  for (const auto &path : files_.selection())
    if (const auto k = kind(path); k == "model" || k == "animation" || k == "convert") result.push_back(path);
  result.sort(Qt::CaseInsensitive);
  if (result.isEmpty() && (asset_.value("kind") == "model" || asset_.value("kind") == "animation" || asset_.value("kind") == "convert"))
    result.push_back(asset_.value("path").toString());
  return result;
}
void Desktop::cancel(const QString &id) {
  for (auto &job : jobs_)
    if (QString::fromStdString(job.handle.snapshot().id.value) == id)
      job.handle.cancel();
  poll();
}
void Desktop::retry(const QString &id) {
  for (const auto &job : jobs_)
    if (QString::fromStdString(job.handle.snapshot().id.value) == id &&
        job.consumed) {
      const auto operation = job.operation, path = job.path;
      const auto idCopy = job.result->resourceId;
      if (operation == "scan")
        navigate(path);
      else if (operation == "inspect")
        openPath(path);
      else if (operation == "resource")
        validateResource(idCopy, path);
      else
        filter(query_, type_, sort_);
      return;
    }
}
void Desktop::copy(const QString &text) {
  QGuiApplication::clipboard()->setText(text);
}
void Desktop::reveal(const QString &path) {
  QDesktopServices::openUrl(
      QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}
void Desktop::clearAsset() {
#ifdef FORMAVIEW_MODEL_UI
  static_cast<ModelSession *>(model_.get())->clear();
#endif
#ifdef FORMAVIEW_IMAGE_UI
  static_cast<ImageSession *>(image_.get())->clear();
#endif
  ++assetRevision_;
  for (auto &j : jobs_)
    if ((j.operation == "inspect" || j.operation == "resource") && !j.consumed)
      j.handle.cancel();
  asset_.clear();
  resourceIssues_.clear();
  ++sessionRevision_;
  emit changed();
}
void Desktop::dismissError() {
  error_.clear();
  emit changed();
}
QString Desktop::report() const {
  return QString::fromUtf8(
      QJsonDocument(
          QJsonObject{
              {"requested", requested()},
              {"error", error_},
              {"current", QJsonObject::fromVariantMap(asset_)},
              {"resources", QJsonArray::fromVariantList(resourceIssues_)},
              {"tasks", QJsonArray::fromVariantList(taskRows_)}})
          .toJson());
}
bool Desktop::reportMissingResource(quint64 session, const QString &id,
                                    const QString &expected) {
  if (session != sessionRevision_ || asset_.isEmpty() || id.isEmpty() ||
      resourceIssues_.size() >= 256)
    return false;
  for (const auto &value : resourceIssues_)
    if (value.toMap()["id"] == id)
      return false;
  resourceIssues_.append(QVariantMap{{"id", id},
                                     {"expected", expected},
                                     {"candidate", ""},
                                     {"resolved", ""},
                                     {"state", "missing"},
                                     {"detail", ""}});
  emit changed();
  return true;
}
void Desktop::validateResource(const QString &id, const QString &candidate) {
  for (auto &value : resourceIssues_) {
    auto issue = value.toMap();
    if (issue["state"] == "checking") {
      issue["state"] = "missing";
      value = issue;
    }
  }
  auto found = false;
  for (auto &value : resourceIssues_) {
    auto issue = value.toMap();
    if (issue["id"] == id) {
      issue["state"] = "checking";
      issue["candidate"] = localPath(candidate);
      issue["detail"] = "";
      value = issue;
      found = true;
    }
  }
  if (!found)
    return;
  emit changed();
  const auto path = localPath(candidate);
  start(
      "resource", path, ++resourceRevision_,
      [path](std::stop_token stop, Result &out) {
        if (stop.stop_requested())
          return failure(formaview::ErrorCode::Cancelled, "Cancelled");
        const auto info = QFileInfo(path);
        auto file = QFile(path);
        if (!info.isFile() || !file.open(QIODevice::ReadOnly)) {
          out.error = "Candidate is not a readable file";
          return failure(formaview::ErrorCode::Io, out.error);
        }
        file.read(4096);
        if (file.error() != QFileDevice::NoError) {
          out.error = file.errorString();
          return failure(formaview::ErrorCode::Io, out.error);
        }
        if (stop.stop_requested())
          return failure(formaview::ErrorCode::Cancelled, "Cancelled");
        out.asset = {{"bytes", info.size()},
                     {"modified", info.lastModified().toMSecsSinceEpoch()}};
        return formaview::TaskResult{};
      },
      -1, id);
}
void Desktop::applyResource(const QString &id) {
  for (auto &value : resourceIssues_) {
    auto issue = value.toMap();
    if (issue["id"] == id && issue["state"] == "checked") {
      // This is a session path override, not a decoded-format or compatibility
      // assertion.
      issue["resolved"] = issue["candidate"];
      issue["state"] = "mapped";
      value = issue;
      emit changed();
      return;
    }
  }
}
void Desktop::publish(Job &job) {
  const auto state = job.handle.snapshot().state;
  if (job.operation == "folders") {
    if (job.revision != folderRevision_)
      return;
    auto entries = QVariantList{};
    if (state == formaview::TaskState::Succeeded)
      for (const auto &entry : job.result->entries)
        entries.append(QVariantMap{{"name", entry.name}, {"path", entry.path}});
    folderMenu_["busy"] = false;
    folderMenu_["entries"] = entries;
    folderMenu_["error"] = job.result->error;
    emit folderMenuChanged();
    return;
  }
  const auto matching =
      (job.operation == "scan"     ? job.revision == directoryRevision_
       : job.operation == "filter" ? job.revision == filterRevision_
       : job.operation == "resource"
           ? (job.revision == resourceRevision_ &&
              job.result->assetRequestRevision == assetRevision_)
           : job.revision == assetRevision_);
  if (!matching)
    return;
  if (job.operation == "resource") {
    for (auto &value : resourceIssues_) {
      auto issue = value.toMap();
      if (issue["id"] == job.result->resourceId) {
        issue["state"] =
            state == formaview::TaskState::Succeeded ? "checked" : "missing";
        issue["detail"] = job.result->error;
        value = issue;
      }
    }
    emit changed();
    return;
  }
  if (job.operation == "scan")
    scanning_ = false;
  if (job.operation == "filter")
    filtering_ = false;
  if (state != formaview::TaskState::Succeeded) {
    if (state == formaview::TaskState::Failed) {
      errorTarget_ = job.path;
      error_ = job.result->error.isEmpty()
                   ? QString::fromStdString(
                         job.handle.snapshot().result->error->message)
                   : job.result->error;
    }
    emit changed();
    return;
  }
  if (job.operation == "scan") {
    const auto sameDirectory = directory_ == job.path;
    directory_ = job.path;
    source_ = std::make_shared<const std::vector<FileEntry>>(
        std::move(job.result->entries));
    recent_.removeAll(directory_);
    recent_.prepend(directory_);
    recent_ = recent_.mid(0, 20);
    // QSettings destruction flushes to disk and can block for >100ms.
    // Keep snapshot writes ordered, and drain them before Desktop is destroyed.
    recentWriter_.start([recent=recent_] { QSettings{}.setValue("recent",recent); });
    if (!sameDirectory) {
      const auto view = views_.value(directory_);
      query_ = view.query;
      type_ = view.type;
      sort_ = view.sort;
      scroll_ = view.scroll;
      files_.replace({});
      files_.restoreSelection(view.selection);
      current_ = -1;
    }
    if (job.historyTarget >= 0)
      historyAt_ = job.historyTarget;
    else {
      while (history_.size() > historyAt_ + 1)
        history_.removeLast();
      if (history_.isEmpty() || history_.last() != directory_)
        history_.append(directory_);
      if (history_.size() > 50)
        history_.removeFirst();
      historyAt_ = static_cast<int>(history_.size()) - 1;
    }
    // Submitting work here would invalidate jobs_ while poll iterates it.
    filtering_ = true;
    ++filterRevision_;
    const auto scanRevision = job.revision;
    QMetaObject::invokeMethod(
        this,
        [this, scanRevision] {
          if (scanRevision == directoryRevision_)
            filter(query_, type_, sort_);
        },
        Qt::QueuedConnection);
  } else if (job.operation == "filter") {
    const auto current = files_.at(current_) ? files_.at(current_)->path
                                             : views_.value(directory_).current;
    files_.replace(std::move(job.result->entries));
    current_ = files_.indexOf(current);
    emit viewRestored();
  } else if (job.result->isDirectory) {
    const auto path = job.path;
    const auto revision = assetRevision_;
    const auto navigation = directoryRevision_;
    QMetaObject::invokeMethod(
        this,
        [this, path, revision, navigation] {
          if (revision == assetRevision_ && navigation == directoryRevision_)
            navigate(path);
        },
        Qt::QueuedConnection);
  } else {
    asset_ = job.result->asset;
#ifdef FORMAVIEW_MODEL_UI
    // M07: standalone animation sources (PSA, animation-only FBX) preview in
    // the model workspace as skeleton documents.
    if (asset_["kind"] == "model" || asset_["kind"] == "animation")
      static_cast<ModelSession *>(model_.get())->open(job.path);
    else
      static_cast<ModelSession *>(model_.get())->clear();
#endif
#ifdef FORMAVIEW_IMAGE_UI
    if (asset_["kind"] == "image")
      static_cast<ImageSession *>(image_.get())->open(job.path);
    else
      static_cast<ImageSession *>(image_.get())->clear();
#endif
    resourceIssues_.clear();
    ++sessionRevision_;
    const auto parent =
        QDir::fromNativeSeparators(QFileInfo(job.path).absolutePath());
    if (parent == directory_) {
      const int row = files_.indexOf(job.path);
      if (row >= 0) {
        current_ = row;
        if (!files_.selection().contains(job.path))
          files_.select(row, 0);
      }
    }
    if (parent != directory_ &&
        job.result->navigationRevision == directoryRevision_) {
      const auto path = job.path;
      const auto revision = assetRevision_;
      const auto nav = directoryRevision_;
      QMetaObject::invokeMethod(
          this,
          [this, parent, path, revision, nav] {
            if (revision != assetRevision_ || nav != directoryRevision_)
              return;
            views_[parent].current = path;
            views_[parent].selection = {path};
            navigate(parent);
          },
          Qt::QueuedConnection);
    }
  }
  emit changed();
}
void Desktop::poll() {
  auto rows = QVariantList{};
  auto active = 0;
  for (auto &job : jobs_) {
    const auto snap = job.handle.snapshot();
    const auto terminal = formaview::is_terminal(snap.state);
    if (terminal && !job.consumed) {
      job.consumed = true;
      publish(job);
      std::vector<FileEntry>{}.swap(job.result->entries);
    }
    if (job.operation == "folders")
      continue;
    if (!terminal)
      ++active;
    const auto detail =
        snap.result && snap.result->error
            ? QString::fromStdString(snap.result->error->message)
            : QString{};
    rows.prepend(QVariantMap{{"id", QString::fromStdString(snap.id.value)},
                             {"operation", job.operation},
                             {"path", job.path},
                             {"state", stateName(snap.state)},
                             {"terminal", terminal},
                             {"detail", detail},
                             {"cancelRequested", snap.cancellation_requested}});
  }
  while (jobs_.size() > 64) {
    const auto it = std::find_if(jobs_.begin(), jobs_.end(),
                                 [](const auto &j) { return j.consumed; });
    if (it == jobs_.end())
      break;
    jobs_.erase(it);
  }
  if (rows != taskRows_ || active != active_) {
    taskRows_ = std::move(rows);
    active_ = active;
    emit tasksChanged();
  }
}
