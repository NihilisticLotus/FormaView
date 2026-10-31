#pragma once
#include "formaview/model_export.hpp"
#include <QByteArray>
#include <QJsonArray>
#include <memory>

namespace formaview::scene_export {
using Loader = std::function<QByteArray(const std::filesystem::path &)>;
class Document {
public:
  static Document read(const std::filesystem::path &, const QByteArray &,
                       Loader, skeletal::Limits, std::stop_token,
                       bool inventory_only = false, const QJsonArray *selected_clips = nullptr);
  Document(Document &&) noexcept;
  ~Document();
  QJsonObject inspect() const;
  QByteArray encode(const QJsonObject &options, QJsonObject &result);

private:
  struct Impl;
  explicit Document(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
};
bool handles(const QString &suffix);
QJsonObject execute(const QJsonObject &, const AccessScope &, skeletal::Limits,
                    std::stop_token, std::span<const InputSnapshot>,
                    const std::function<void(const QJsonObject &)> &,
                    std::span<const std::filesystem::path>,
                    const std::filesystem::path &);
} // namespace formaview::scene_export
