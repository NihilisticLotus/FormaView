#include "formaview/contracts.hpp"
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace formaview {
AssetId asset_id_from_sha256(std::string_view sha256) {
    if (sha256.size() != 64 || !std::all_of(sha256.begin(), sha256.end(), [](unsigned char c) {
            return std::isxdigit(c) != 0;
        })) {
        throw std::invalid_argument("Asset identity requires a complete SHA256 digest");
    }
    auto digest = std::string{sha256};
    std::transform(digest.begin(), digest.end(), digest.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return AssetId{std::move(digest)};
}
ObjectId SourceIdentity::stable_id() const {
    const auto digest = asset_id_from_sha256(sha256).value;
    if (adapter.empty() || adapter_version.empty() || object_key.empty()) {
        throw std::invalid_argument("Source identity requires adapter version and persistent object key");
    }
    auto value = std::string{"fv-source-v1:"};
    for (const auto& field : {digest, adapter, adapter_version, object_key}) {
        value += std::to_string(field.size()) + ":" + field;
    }
    return ObjectId{std::move(value)};
}
bool is_terminal(TaskState state) noexcept {
    return state != TaskState::Queued && state != TaskState::Running && state != TaskState::Cancelling;
}
} // namespace formaview
