#pragma once
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace formaview {
enum class CachePath { Cold, Warm, NotApplicable };
struct MetricKey {
    std::string name;
    CachePath cache{CachePath::NotApplicable};
    auto operator<=>(const MetricKey&) const = default;
};
struct DurationSummary {
    std::uint64_t observations{}, retained{}, evicted{}, long_events{};
    double p95_ms{}, p99_ms{}, max_ms{};
};
// Quantiles describe the bounded retained window, using nearest-rank selection.
// observations/long_events count the full stream; eviction is explicit.
class PerformanceRecorder final {
public:
    explicit PerformanceRecorder(std::size_t samples_per_metric = 4096, std::size_t max_metrics = 64);
    bool record(MetricKey key, std::chrono::nanoseconds duration,
                std::chrono::nanoseconds long_threshold = std::chrono::milliseconds{100});
    [[nodiscard]] std::optional<DurationSummary> summary(const MetricKey& key) const;
private:
    struct Stream {
        std::vector<double> milliseconds;
        std::size_t next{};
        std::uint64_t observations{}, long_events{};
        double max_ms{};
    };
    const std::size_t samples_per_metric_, max_metrics_;
    mutable std::mutex mutex_;
    std::map<MetricKey, Stream> streams_;
};
struct ProcessMemory {
    std::uint64_t working_set_bytes{}, peak_working_set_bytes{}, private_bytes{};
};
[[nodiscard]] std::optional<ProcessMemory> process_memory();
} // namespace formaview
