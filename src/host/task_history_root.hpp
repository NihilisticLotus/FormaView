#pragma once

#include <QDir>
#include <QSettings>
#include <QString>

namespace formaview {
inline QString taskHistoryRoot(const QString &localData, const QString &programDir,
                               const QString &installedLayout, const QString &overrideRoot) {
  if (!overrideRoot.isEmpty()) return overrideRoot;
  const QSettings installation(QDir(programDir).filePath("installation.ini"),
                               QSettings::IniFormat);
  const auto installed = installation.value("Install/AppId").toString() ==
                         QStringLiteral("{618397E7-C70C-4D14-B601-D6FB2F562BA1}");
  if (!installed) return QDir(localData).filePath("development/image-journal");
  if (installedLayout == QStringLiteral("legacy"))
    return QDir(localData).filePath("image-journal");
  return QDir(localData).filePath("installed/image-journal");
}
} // namespace formaview
