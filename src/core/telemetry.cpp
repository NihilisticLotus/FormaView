#include "formaview/telemetry.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

namespace formaview {
PerformanceRecorder::PerformanceRecorder(std::size_t samples_per_metric, std::size_t max_metrics)
    : samples_per_metric_(samples_per_metric), max_metrics_(max_metrics) {
    if (samples_per_metric == 0 || max_metrics == 0) throw std::invalid_argument("Telemetry must be bounded above zero");
}
bool PerformanceRecorder::record(MetricKey key, std::chrono::nanoseconds duration, std::chrono::nanoseconds long_threshold) {
    if (key.name.empty() || duration.count() < 0 || long_threshold.count() < 0) return false;
    const auto lock = std::lock_guard{mutex_};
    if (!streams_.contains(key) && streams_.size() >= max_metrics_) return false;
    auto& stream = streams_[std::move(key)];
    const auto ms = std::chrono::duration<double, std::milli>{duration}.count();
    if (stream.milliseconds.size() < samples_per_metric_) stream.milliseconds.push_back(ms);
    else stream.milliseconds[stream.next] = ms;
    stream.next = (stream.next + 1) % samples_per_metric_;
    ++stream.observations;
    if (duration > long_threshold) ++stream.long_events;
    stream.max_ms = std::max(stream.max_ms, ms);
    return true;
}
std::optional<DurationSummary> PerformanceRecorder::summary(const MetricKey& key) const {
    const auto lock = std::lock_guard{mutex_};
    const auto found = streams_.find(key);
    if (found == streams_.end()) return std::nullopt;
    const auto& stream = found->second;
    auto sorted = stream.milliseconds;
    std::sort(sorted.begin(), sorted.end());
    const auto quantile = [&](double p) {
        const auto index = static_cast<std::size_t>(std::ceil(p * static_cast<double>(sorted.size()))) - 1;
        return sorted[index];
    };
    const auto retained = static_cast<std::uint64_t>(sorted.size());
    return DurationSummary{stream.observations, retained, stream.observations - retained,
                           stream.long_events, quantile(0.95), quantile(0.99), stream.max_ms};
}
std::optional<ProcessMemory> process_memory() {
#ifdef _WIN32
    auto counters = PROCESS_MEMORY_COUNTERS_EX{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) return std::nullopt;
    return ProcessMemory{counters.WorkingSetSize, counters.PeakWorkingSetSize, counters.PrivateUsage};
#else
    return std::nullopt;
#endif
}
} // namespace formaview
