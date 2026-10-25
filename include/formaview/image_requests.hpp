#pragma once
#include "formaview/file_access.hpp"
#include "formaview/image_conversion.hpp"
#include <QJsonObject>

namespace formaview::imaging {
// Runtime catalog of the real image request service. Qt-backed formats are
// advertised only when the required image handlers are available.
[[nodiscard]] QJsonObject image_capabilities();
[[nodiscard]] std::vector<std::wstring> image_input_extensions();
// Versioned JSON boundary shared by local clients. inspect/preflight/convert
// are blocking worker operations; transport must not run these on a UI thread.
[[nodiscard]] AccessScope parse_image_scope(const QJsonObject &);
[[nodiscard]] ImageConversionOptions parse_image_options(const QJsonObject &);
[[nodiscard]] QJsonObject execute_image_request(
    const QJsonObject &, const AccessScope &, Limits = {}, std::stop_token = {},
    std::span<const InputSnapshot> protected_inputs = {},
    const std::function<void(const QJsonObject &)> &commit_observer = {},
    std::span<const std::filesystem::path> reserved_outputs = {},
    const std::filesystem::path &approved_output = {});
[[nodiscard]] QJsonObject image_error_json(const skeletal::Failure &);
} // namespace formaview::imaging
