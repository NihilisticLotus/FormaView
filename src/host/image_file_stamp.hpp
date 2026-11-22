#pragma once

#include "formaview/pro_image.hpp"
#include <QCryptographicHash>
#include <QFile>
#include <algorithm>
#include <io.h>
#include <windows.h>

namespace formaview::host {
// The caller holds an ImageReadLease throughout cache lookup and decoding.
inline QString imageFileStamp(const QString &path, std::stop_token stop,
    decltype(&GetFileInformationByHandleEx) query = GetFileInformationByHandleEx) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    throw skeletal::Failure(ErrorCode::Io, "Cannot open image for identification");
  const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(file.handle()));
  FILE_ID_INFO identity{};
  FILE_BASIC_INFO basic{};
  FILE_STANDARD_INFO standard{};
  if (query(handle, FileIdInfo, &identity, sizeof(identity)) &&
      query(handle, FileBasicInfo, &basic, sizeof(basic)) &&
      query(handle, FileStandardInfo, &standard, sizeof(standard)) &&
      basic.ChangeTime.QuadPart != 0 &&
      std::any_of(std::begin(identity.FileId.Identifier), std::end(identity.FileId.Identifier),
                  [](BYTE b) { return b != 0; })) {
    return QString::number(identity.VolumeSerialNumber) + ':' +
           QByteArray(reinterpret_cast<const char *>(identity.FileId.Identifier), 16).toHex() +
           ':' + QString::number(basic.ChangeTime.QuadPart) + ':' +
           QString::number(basic.LastWriteTime.QuadPart) + ':' +
           QString::number(standard.EndOfFile.QuadPart);
  }
  // Some file providers do not expose persistent IDs or a change time. Size
  // and modification time alone cannot distinguish an in-place replacement.
  QCryptographicHash digest(QCryptographicHash::Sha256);
  QByteArray chunk(256 * 1024, Qt::Uninitialized);
  for (;;) {
    if (stop.stop_requested())
      throw skeletal::Failure(ErrorCode::Cancelled, "Image identification cancelled");
    const auto n = file.read(chunk.data(), chunk.size());
    if (n < 0)
      throw skeletal::Failure(ErrorCode::Io, "Cannot read image for identification");
    if (n == 0)
      break;
    digest.addData(QByteArrayView(chunk.constData(), n));
  }
  return QStringLiteral("sha256:") + digest.result().toHex();
}
} // namespace formaview::host
