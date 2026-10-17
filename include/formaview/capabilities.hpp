#pragma once
#include "formaview/contracts.hpp"
#include <functional>
#include <stop_token>

namespace formaview {
struct Capability {
    std::string id, input_format, output_format, variant, implementation_version;
    std::vector<std::string> limitations;
};
class CapabilityRegistry final {
public:
    using Executor = std::function<TaskResult(const ConversionPlan&, std::stop_token)>;
    // Construct during service setup, then share read-only. No extension-based defaults.
    void add(Capability capability, Executor executor);
    [[nodiscard]] std::vector<Capability> list() const;
    [[nodiscard]] TaskResult execute(const std::string& id, const ConversionPlan& plan, std::stop_token stop = {}) const;
private:
    struct Entry { Capability capability; Executor executor; };
    std::map<std::string, Entry> entries_;
};
} // namespace formaview
