#pragma once
#include "formaview/contracts.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <stop_token>

namespace formaview {
namespace detail { struct GenerationState; struct SchedulerState; struct TaskRecord; }

class GenerationTicket final {
public:
    GenerationTicket() = default;
    [[nodiscard]] bool current() const;
    // Serialises publication with replacement. Callback must be short and must not
    // call issue(), current() or publish() on this same generation owner.
    [[nodiscard]] bool publish(const std::function<void()>& callback) const;
private:
    friend class LatestRequest;
    std::shared_ptr<detail::GenerationState> state_;
    std::uint64_t revision_{};
};
class LatestRequest final {
public:
    LatestRequest();
    [[nodiscard]] GenerationTicket issue();
private:
    std::shared_ptr<detail::GenerationState> state_;
};

enum class Priority { Foreground, Background };
struct Resources {
    std::uint32_t cpu_slots{1}, io_slots{};
    std::uint64_t memory_bytes{}, gpu_bytes{};
};
struct SchedulerLimits {
    std::uint32_t workers{4};
    std::size_t queue_capacity{64}, foreground_queue_reserve{8};
    std::uint32_t foreground_worker_reserve{1};
    Resources capacity{4, 2, 512ULL * 1024 * 1024, 0};
    Resources foreground_reserve{1, 1, 64ULL * 1024 * 1024, 0};
};
struct TaskRequest {
    std::uint32_t version{api_version};
    Priority priority{Priority::Background};
    Resources resources;
    GenerationTicket generation;
    std::function<TaskResult(std::stop_token)> work;
};
struct TaskSnapshot {
    JobId id;
    TaskState state{TaskState::Queued};
    bool cancellation_requested{};
    std::optional<TaskResult> result; // Present only at actual completion.
    std::chrono::nanoseconds queue_time{}, run_time{};
};
class TaskHandle final {
public:
    [[nodiscard]] TaskSnapshot snapshot() const;
    [[nodiscard]] bool wait_for(std::chrono::milliseconds timeout) const;
    // True means the cancellation intent was accepted, not that work has stopped.
    bool cancel() const;
private:
    friend class Scheduler;
    std::shared_ptr<detail::SchedulerState> state_;
    std::shared_ptr<detail::TaskRecord> record_;
};
struct SubmitResult { std::optional<TaskHandle> task; std::optional<Error> error; };
struct SchedulerStats {
    std::size_t queued{}, running{};
    Resources reserved{0, 0, 0, 0};
};
class Scheduler final {
public:
    explicit Scheduler(SchedulerLimits limits = {});
    ~Scheduler();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    [[nodiscard]] SubmitResult submit(TaskRequest request);
    [[nodiscard]] SchedulerStats stats() const;
    // Control-thread operation: cancel queued work, request running cancellation,
    // join workers and release captures. Never call from a scheduled work function.
    void shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace formaview
