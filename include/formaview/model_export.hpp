#pragma once
#include "formaview/file_access.hpp"
#include "formaview/skeletal.hpp"
#include <QJsonObject>

namespace formaview::model_export {
[[nodiscard]] bool handles(const QJsonObject &);
void validate_options(const QJsonObject &);
// Includes an automatic same-basename PSA config candidate even when absent,
// so batch publication cannot overwrite another item's auxiliary input.
[[nodiscard]] std::vector<std::filesystem::path>
auxiliary_inputs(const QJsonObject &);
[[nodiscard]] QJsonObject capabilities();
[[nodiscard]] QJsonObject
execute(const QJsonObject &, const AccessScope &, skeletal::Limits = {},
        std::stop_token = {},
        std::span<const InputSnapshot> protected_inputs = {},
        const std::function<void(const QJsonObject &)> &commit_observer = {},
        std::span<const std::filesystem::path> reserved_outputs = {},
        const std::filesystem::path &approved_output = {},
        // Export curves/SDK workspace is separate from the input decode limit.
        std::uint64_t export_working_bytes = 4ULL * 1024 * 1024 * 1024);
} // namespace formaview::model_export
