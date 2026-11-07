#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <optional>
#include <stop_token>
#include <array>
#include <span>

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
struct BindMatrices {
  QByteArray marker, name;
  std::array<double, 16> transform{}, link{};
};
// Only accepts our SDK-generated binary output. Temporary unique names match
// records without relying on object IDs, ordering or user-provided names.
QByteArray restore_fbx_bind_matrices(const QByteArray &, std::span<const BindMatrices>,
                                    std::uint64_t output_budget, std::stop_token);
} // namespace formaview::scene_export
