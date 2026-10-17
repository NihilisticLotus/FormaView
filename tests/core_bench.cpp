#include "formaview/scheduler.hpp"
#include "formaview/telemetry.hpp"
#include <chrono>
#include <iostream>

int main() {
    namespace fv = formaview;
    using namespace std::chrono_literals;
    auto recorder = fv::PerformanceRecorder{};
    const auto initial_memory = fv::process_memory();
    {
        auto scheduler = fv::Scheduler{};
        for (auto i = 0; i < 1001; ++i) {
            const auto start = std::chrono::steady_clock::now();
            auto result = scheduler.submit({1, fv::Priority::Foreground, {}, {}, [](auto) { return fv::TaskResult{}; }});
            if (!result.task || !result.task->wait_for(2s) || result.task->snapshot().state != fv::TaskState::Succeeded) return 1;
            const auto path = i == 0 ? fv::CachePath::Cold : fv::CachePath::Warm;
            recorder.record({"submit_to_terminal", path}, std::chrono::steady_clock::now() - start);
            recorder.record({"queue", path}, result.task->snapshot().queue_time);
        }
        scheduler.shutdown();
        if (scheduler.stats().running != 0 || scheduler.stats().reserved.memory_bytes != 0) return 2;
    }
    std::cout << "{\"boundary\":\"synthetic no-op task; first call cold, remaining calls warm; no disk cache or product workload\",\"metrics\":[";
    auto separator = "";
    for (const auto path : {fv::CachePath::Cold, fv::CachePath::Warm}) {
        for (const auto* name : {"submit_to_terminal", "queue"}) {
            const auto result = recorder.summary({name, path});
            if (!result) return 3;
            std::cout << separator << "{\"name\":\"" << name << "\",\"path\":\"" << (path == fv::CachePath::Cold ? "cold" : "warm")
                      << "\",\"n\":" << result->observations << ",\"p95_ms\":" << result->p95_ms << ",\"p99_ms\":" << result->p99_ms
                      << ",\"max_ms\":" << result->max_ms << ",\"over_100ms\":" << result->long_events << '}';
            separator = ",";
        }
    }
    std::cout << "]";
    if (const auto memory = fv::process_memory()) {
        std::cout << ",\"post_shutdown_working_set\":" << memory->working_set_bytes << ",\"process_peak_working_set\":" << memory->peak_working_set_bytes
                  << ",\"post_shutdown_private_bytes\":" << memory->private_bytes;
        if (initial_memory) std::cout << ",\"initial_working_set\":" << initial_memory->working_set_bytes;
    }
    std::cout << "}\n";
}
