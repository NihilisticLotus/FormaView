#include "formaview/scheduler.hpp"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace formaview {
namespace detail {
using Clock = std::chrono::steady_clock;
struct GenerationState { std::mutex mutex; std::uint64_t revision{}; };
struct TaskRecord {
    TaskRequest request;
    TaskSnapshot snapshot;
    std::stop_source stop;
    Clock::time_point submitted{Clock::now()}, started{};
};
struct SchedulerState {
    mutable std::mutex mutex;
    std::condition_variable changed;
    SchedulerLimits limits;
    std::deque<std::shared_ptr<TaskRecord>> queue;
    std::vector<std::shared_ptr<TaskRecord>> active;
    Resources used{0, 0, 0, 0}, background_used{0, 0, 0, 0};
    std::size_t background_active{};
    std::uint64_t next_id{};
    bool closing{};
};
} // namespace detail
namespace {
bool within(const Resources& value, const Resources& limit) {
    return value.cpu_slots <= limit.cpu_slots && value.io_slots <= limit.io_slots
        && value.memory_bytes <= limit.memory_bytes && value.gpu_bytes <= limit.gpu_bytes;
}
Resources minus(const Resources& a, const Resources& b) {
    return {a.cpu_slots - b.cpu_slots, a.io_slots - b.io_slots, a.memory_bytes - b.memory_bytes, a.gpu_bytes - b.gpu_bytes};
}
Resources plus(const Resources& a, const Resources& b) {
    return {a.cpu_slots + b.cpu_slots, a.io_slots + b.io_slots, a.memory_bytes + b.memory_bytes, a.gpu_bytes + b.gpu_bytes};
}
TaskResult error_result(ErrorCode code, std::string message) {
    return {api_version, {}, Error{code, std::move(message), {}, false}};
}
TaskState result_state(const TaskResult& result) {
    auto successes = std::size_t{};
    auto failures = std::size_t{result.error ? 1U : 0U};
    auto cancelled = false;
    for (const auto& item : result.items) {
        successes += item.status == ItemStatus::Succeeded ? 1U : 0U;
        failures += item.status == ItemStatus::Failed ? 1U : 0U;
        cancelled = cancelled || item.status == ItemStatus::Cancelled;
    }
    if (cancelled) return TaskState::Cancelled;
    if (failures != 0) return successes != 0 ? TaskState::PartiallySucceeded : TaskState::Failed;
    return TaskState::Succeeded;
}
bool fits(const detail::SchedulerState& state, const detail::TaskRecord& task) {
    if (!within(task.request.resources, minus(state.limits.capacity, state.used))) return false;
    if (task.request.priority == Priority::Foreground) return true;
    const auto background_limit = minus(state.limits.capacity, state.limits.foreground_reserve);
    return state.background_active < state.limits.workers - state.limits.foreground_worker_reserve
        && within(task.request.resources, minus(background_limit, state.background_used));
}
auto next_task(detail::SchedulerState& state) {
    for (const auto priority : {Priority::Foreground, Priority::Background}) {
        const auto found = std::find_if(state.queue.begin(), state.queue.end(), [&](const auto& task) {
            return task->request.priority == priority && fits(state, *task);
        });
        if (found != state.queue.end()) return found;
    }
    return state.queue.end();
}
void run_worker(const std::shared_ptr<detail::SchedulerState>& state) {
    while (true) {
        auto task = std::shared_ptr<detail::TaskRecord>{};
        auto work = std::function<TaskResult(std::stop_token)>{};
        {
            auto lock = std::unique_lock{state->mutex};
            state->changed.wait(lock, [&] { return state->closing || next_task(*state) != state->queue.end(); });
            if (state->closing) return;
            const auto next = next_task(*state);
            task = *next;
            state->queue.erase(next);
            state->active.push_back(task);
            state->used = plus(state->used, task->request.resources);
            if (task->request.priority == Priority::Background) {
                ++state->background_active;
                state->background_used = plus(state->background_used, task->request.resources);
            }
            task->started = detail::Clock::now();
            task->snapshot.queue_time = task->started - task->submitted;
            task->snapshot.state = TaskState::Running;
            work = std::move(task->request.work);
        }
        auto result = TaskResult{};
        try {
            result = task->request.generation.current()
                ? work(task->stop.get_token()) : error_result(ErrorCode::StaleResult, "Request was replaced before execution");
            if (result.version != api_version) result = error_result(ErrorCode::InvalidArgument, "Unsupported result version");
        } catch (const std::exception& error) {
            result = error_result(ErrorCode::Internal, error.what());
        } catch (...) {
            result = error_result(ErrorCode::Internal, "Unknown task exception");
        }
        work = {}; // Release work captures before reporting a terminal state.
        const auto current = task->request.generation.current();
        {
            const auto lock = std::lock_guard{state->mutex};
            task->snapshot.run_time = detail::Clock::now() - task->started;
            if (!current) {
                task->snapshot.state = TaskState::StaleDiscarded;
                result = error_result(ErrorCode::StaleResult, "Request was replaced; result discarded");
            } else if (task->snapshot.cancellation_requested) {
                task->snapshot.state = TaskState::Cancelled;
                // Keep any committed per-item results. Cancellation cannot roll them back.
                result.error = Error{ErrorCode::Cancelled, "Work returned after cancellation request", {}, false};
            } else {
                task->snapshot.state = result_state(result);
            }
            task->snapshot.result = std::move(result);
            state->used = minus(state->used, task->request.resources);
            if (task->request.priority == Priority::Background) {
                --state->background_active;
                state->background_used = minus(state->background_used, task->request.resources);
            }
            std::erase(state->active, task);
        }
        state->changed.notify_all();
    }
}
} // namespace

LatestRequest::LatestRequest() : state_(std::make_shared<detail::GenerationState>()) {}
GenerationTicket LatestRequest::issue() {
    const auto lock = std::lock_guard{state_->mutex};
    if (state_->revision == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Generation exhausted");
    auto ticket = GenerationTicket{};
    ticket.state_ = state_;
    ticket.revision_ = ++state_->revision;
    return ticket;
}
bool GenerationTicket::current() const {
    if (!state_) return true;
    const auto lock = std::lock_guard{state_->mutex};
    return state_->revision == revision_;
}
bool GenerationTicket::publish(const std::function<void()>& callback) const {
    if (!state_) { callback(); return true; }
    const auto lock = std::lock_guard{state_->mutex};
    if (state_->revision != revision_) return false;
    callback();
    return true;
}
TaskSnapshot TaskHandle::snapshot() const {
    if (!state_ || !record_) throw std::logic_error("Empty task handle");
    const auto lock = std::lock_guard{state_->mutex};
    return record_->snapshot;
}
bool TaskHandle::wait_for(std::chrono::milliseconds timeout) const {
    if (!state_ || !record_) throw std::logic_error("Empty task handle");
    auto lock = std::unique_lock{state_->mutex};
    return state_->changed.wait_for(lock, timeout, [&] { return is_terminal(record_->snapshot.state); });
}
bool TaskHandle::cancel() const {
    if (!state_ || !record_) return false;
    auto discarded_work = std::function<TaskResult(std::stop_token)>{};
    {
        const auto lock = std::lock_guard{state_->mutex};
        if (is_terminal(record_->snapshot.state)) return false;
        record_->snapshot.cancellation_requested = true;
        const auto queued = std::find(state_->queue.begin(), state_->queue.end(), record_);
        if (queued != state_->queue.end()) {
            state_->queue.erase(queued);
            discarded_work = std::move(record_->request.work);
            record_->snapshot.state = TaskState::Cancelled;
            record_->snapshot.queue_time = detail::Clock::now() - record_->submitted;
            record_->snapshot.result = error_result(ErrorCode::Cancelled, "Cancelled before execution");
        } else {
            record_->snapshot.state = TaskState::Cancelling;
        }
    }
    discarded_work = {};
    record_->stop.request_stop(); // Stop callbacks may call other control methods; no scheduler lock held.
    state_->changed.notify_all();
    return true;
}
struct Scheduler::Impl {
    std::shared_ptr<detail::SchedulerState> state{std::make_shared<detail::SchedulerState>()};
    std::vector<std::thread> workers;
    std::mutex shutdown_mutex;
};
Scheduler::Scheduler(SchedulerLimits limits) : impl_(std::make_unique<Impl>()) {
    if (limits.workers == 0 || limits.queue_capacity == 0 || limits.capacity.cpu_slots == 0
        || limits.foreground_worker_reserve >= limits.workers
        || limits.foreground_queue_reserve > limits.queue_capacity
        || !within(limits.foreground_reserve, limits.capacity)) {
        throw std::invalid_argument("Invalid scheduler limits");
    }
    impl_->state->limits = limits;
    try {
        for (auto index = std::uint32_t{}; index < limits.workers; ++index) {
            impl_->workers.emplace_back(run_worker, impl_->state);
        }
    } catch (...) {
        shutdown();
        throw;
    }
}
Scheduler::~Scheduler() { shutdown(); }
SubmitResult Scheduler::submit(TaskRequest request) {
    const auto state = impl_->state;
    auto task = std::make_shared<detail::TaskRecord>();
    auto rejection = std::optional<Error>{};
    {
        const auto lock = std::lock_guard{state->mutex};
        if (state->closing) rejection = Error{ErrorCode::ShuttingDown, "Scheduler is closing", {}, false};
        else if (request.version != api_version || !request.work || request.resources.cpu_slots == 0
                 || (request.priority != Priority::Foreground && request.priority != Priority::Background)) {
            rejection = Error{ErrorCode::InvalidArgument, "Invalid task version, priority, work or CPU reservation", {}, false};
        } else if (!within(request.resources, state->limits.capacity)
            || (request.priority == Priority::Background
                && !within(request.resources, minus(state->limits.capacity, state->limits.foreground_reserve)))) {
            rejection = Error{ErrorCode::ResourceLimit, "Task exceeds reservable resources", {}, false};
        } else {
            const auto background_queued = static_cast<std::size_t>(std::count_if(state->queue.begin(), state->queue.end(), [](const auto& queued) {
                return queued->request.priority == Priority::Background;
            }));
            if (state->queue.size() >= state->limits.queue_capacity
                || (request.priority == Priority::Background
                    && background_queued >= state->limits.queue_capacity - state->limits.foreground_queue_reserve)) {
                rejection = Error{ErrorCode::QueueFull, "Bounded queue is full", {}, true};
            } else if (state->next_id == std::numeric_limits<std::uint64_t>::max()) {
                rejection = Error{ErrorCode::ResourceLimit, "Session task identifiers exhausted", {}, false};
            } else {
                task->request = std::move(request);
                task->snapshot.id = JobId{"session-job-" + std::to_string(++state->next_id)};
                state->queue.push_back(task);
            }
        }
    }
    if (rejection) return {std::nullopt, std::move(rejection)};
    auto handle = TaskHandle{};
    handle.state_ = state;
    handle.record_ = std::move(task);
    state->changed.notify_all();
    return {std::move(handle), std::nullopt};
}
SchedulerStats Scheduler::stats() const {
    const auto lock = std::lock_guard{impl_->state->mutex};
    return {impl_->state->queue.size(), impl_->state->active.size(), impl_->state->used};
}
void Scheduler::shutdown() {
    // request_stop invokes callbacks synchronously. A callback may re-enter
    // shutdown on this same control thread; the outer call remains responsible
    // for cancellation and joining all workers.
    static thread_local std::vector<const Impl*> shutdown_stack;
    if (std::find(shutdown_stack.begin(), shutdown_stack.end(), impl_.get()) != shutdown_stack.end()) return;
    const auto guard = std::lock_guard{impl_->shutdown_mutex};
    shutdown_stack.push_back(impl_.get());
    struct PopShutdown { std::vector<const Impl*>& stack; ~PopShutdown() { stack.pop_back(); } } pop{shutdown_stack};
    const auto state = impl_->state;
    auto stop_sources = std::vector<std::stop_source>{};
    auto discarded_work = std::vector<std::function<TaskResult(std::stop_token)>>{};
    {
        const auto lock = std::lock_guard{state->mutex};
        state->closing = true;
        for (auto& task : state->queue) {
            task->snapshot.cancellation_requested = true;
            task->snapshot.state = TaskState::Cancelled;
            task->snapshot.queue_time = detail::Clock::now() - task->submitted;
            task->snapshot.result = error_result(ErrorCode::Cancelled, "Scheduler closed before execution");
            discarded_work.push_back(std::move(task->request.work));
        }
        state->queue.clear();
        for (auto& task : state->active) {
            task->snapshot.cancellation_requested = true;
            task->snapshot.state = TaskState::Cancelling;
            stop_sources.push_back(task->stop);
        }
    }
    discarded_work.clear();
    for (auto& source : stop_sources) source.request_stop();
    state->changed.notify_all();
    for (auto& worker : impl_->workers) if (worker.joinable()) worker.join();
}
} // namespace formaview
