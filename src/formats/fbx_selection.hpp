#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <optional>
#include <stop_token>

namespace formaview::scene_export {
// Rewrites only binary record offsets and animation membership. Property
// payloads (including compressed key/tangent arrays) remain byte-identical.
// Unsupported/ambiguous structure returns nullopt for the full SDK path.
std::optional<QByteArray> select_fbx_takes(const QByteArray &,
                                           const QJsonArray &catalog,
                                           const QJsonArray &selected,
                                           std::stop_token);
// Structural bound check for binary FBX before the SDK allocates from
// declared counts. Throws InvalidArgument; ASCII files are left to the SDK.
void validate_fbx_structure(const QByteArray &, std::uint64_t decoded_budget,
                            std::stop_token);
} // namespace formaview::scene_export
