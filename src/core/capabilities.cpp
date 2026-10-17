#include "formaview/capabilities.hpp"
#include <stdexcept>

namespace formaview {
void CapabilityRegistry::add(Capability capability, Executor executor) {
    if (capability.id.empty() || capability.implementation_version.empty() || !executor) {
        throw std::invalid_argument("Executable capability requires identity, version and executor");
    }
    const auto key = capability.id;
    if (!entries_.emplace(key, Entry{std::move(capability), std::move(executor)}).second) {
        throw std::invalid_argument("Duplicate capability identity");
    }
}
std::vector<Capability> CapabilityRegistry::list() const {
    auto result = std::vector<Capability>{};
    for (const auto& [id, entry] : entries_) {
        (void)id;
        result.push_back(entry.capability);
    }
    return result;
}
TaskResult CapabilityRegistry::execute(const std::string& id, const ConversionPlan& plan, std::stop_token stop) const {
    const auto entry = entries_.find(id);
    if (entry == entries_.end()) {
        return {api_version, {}, Error{ErrorCode::Unsupported, "No executable capability registered", {{"capability", id}}, false}};
    }
    if (plan.version != api_version) {
        return {api_version, {}, Error{ErrorCode::InvalidArgument, "Unsupported plan version", {}, false}};
    }
    return entry->second.executor(plan, stop);
}
} // namespace formaview
