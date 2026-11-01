#include "formaview/capabilities.hpp"
#include "formaview/scheduler.hpp"
#include "formaview/telemetry.hpp"
#include <atomic>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>

using namespace std::chrono_literals;
namespace fv = formaview;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool opened{};
    void open() { const auto lock = std::lock_guard{mutex}; opened = true; changed.notify_all(); }
    void wait(std::stop_token stop = {}) {
        auto callback = std::stop_callback{stop, [&] { changed.notify_all(); }};
        auto lock = std::unique_lock{mutex};
        if (!changed.wait_for(lock, 3s, [&] { return opened || stop.stop_requested(); })) throw std::runtime_error("Test gate timed out");
    }
};
fv::SchedulerLimits limits(std::uint32_t workers = 2) {
    return {workers, 4, 1, workers > 1 ? 1U : 0U, {workers, 2, 100, 20}, {workers > 1 ? 1U : 0U, 0, 0, 0}};
}
fv::TaskHandle submit(fv::Scheduler& scheduler, std::function<fv::TaskResult(std::stop_token)> work,
                      fv::Priority priority = fv::Priority::Background, fv::Resources resources = {},
                      fv::GenerationTicket generation = {}) {
    auto result = scheduler.submit({fv::api_version, priority, resources, std::move(generation), std::move(work)});
    check(result.task.has_value(), "Task unexpectedly rejected");
    return *result.task;
}
fv::TaskSnapshot done(const fv::TaskHandle& task) {
    check(task.wait_for(4s), "Task did not reach terminal state");
    auto result = task.snapshot();
    check(result.result.has_value(), "Terminal task has no result");
    return result;
}
void ready(std::future<void>& future) { check(future.wait_for(2s) == std::future_status::ready, "Work did not start"); future.get(); }
void identity() {
    const auto source = fv::SourceIdentity{std::string(64, 'A'), "synthetic", "1.0", "joint/persistent:12"};
    const auto expected = fv::ObjectId{"fv-source-v1:64:" + std::string(64, 'a') + "9:synthetic3:1.019:joint/persistent:12"};
    check(source.stable_id() == expected, "Stable source identifier changed encoding");
    auto changed = source;
    changed.object_key = "joint/persistent:13";
    check(source.stable_id() != changed.stable_id(), "Duplicate names cannot distinguish source objects");
    changed = source; changed.sha256 = std::string(64, 'a');
    check(source.stable_id() == changed.stable_id(), "Digest case changed source identity");
    check(fv::asset_id_from_sha256(source.sha256) == fv::asset_id_from_sha256(changed.sha256), "Asset digest case changed identity");
    changed = source; changed.adapter_version = "2.0";
    check(source.stable_id() != changed.stable_id(), "Adapter version absent from identity");
    auto rejected = false;
    try { (void)fv::SourceIdentity{"bad", "x", "1", "key"}.stable_id(); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "Invalid fingerprint accepted");
    auto geometry = fv::Geometry{};
    geometry.point_influences = {{{fv::ObjectId{"j0"}, 0.8}, {fv::ObjectId{"j1"}, 0.5}, {fv::ObjectId{"j2"}, -0.1},
                                  {fv::ObjectId{"j3"}, 0.0}, {fv::ObjectId{"j4"}, 0.2}, {fv::ObjectId{"j5"}, 0.3}}};
    geometry.render_mapping = {{0, 0, 0, 0}, {0, 1, 2, 3}};
    check(geometry.point_influences.front().size() == 6 && geometry.point_influences[0][2].source_weight == -0.1,
          "Source influence contract lost complete, raw weights");
    check(geometry.render_mapping[0].source_point == geometry.render_mapping[1].source_point,
          "Split vertices cannot map back to one source point");
}
void capabilities() {
    auto registry = fv::CapabilityRegistry{};
    check(registry.list().empty(), "Empty implementation claims format support");
    const auto result = registry.execute("fbx.read", {});
    check(result.error && result.error->code == fv::ErrorCode::Unsupported, "Absent parser executed successfully");
    auto rejected = false;
    try { registry.add({"fake", "psa", "fbx", "", "1", {}}, {}); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected && registry.list().empty(), "Metadata without executor advertised as executable");
    registry.add({"test.only", "synthetic", "synthetic", "", "1", {}}, [](const auto&, auto) { return fv::TaskResult{}; });
    auto plan = fv::ConversionPlan{}; plan.version = 99;
    check(registry.execute("test.only", plan).error->code == fv::ErrorCode::InvalidArgument, "Unknown plan version accepted");
}
void bounds() {
    auto scheduler = fv::Scheduler{limits()};
    auto gate = std::make_shared<Gate>();
    auto started = std::promise<void>{}; auto future = started.get_future();
    auto first = submit(scheduler, [&, gate](auto stop) { started.set_value(); gate->wait(stop); return fv::TaskResult{}; });
    ready(future);
    auto queued = std::vector<fv::TaskHandle>{};
    for (auto i = 0; i < 3; ++i) queued.push_back(submit(scheduler, [](auto) { return fv::TaskResult{}; }));
    auto rejected = scheduler.submit({1, fv::Priority::Background, {}, {}, [](auto) { return fv::TaskResult{}; }});
    check(!rejected.task && rejected.error->code == fv::ErrorCode::QueueFull, "Background queue did not preserve foreground slot");
    auto foreground_gate = std::make_shared<Gate>();
    auto fg_started = std::promise<void>{}; auto fg_future = fg_started.get_future();
    auto fg = submit(scheduler, [&, foreground_gate](auto stop) { fg_started.set_value(); foreground_gate->wait(stop); return fv::TaskResult{}; }, fv::Priority::Foreground);
    ready(fg_future);
    auto fg_queued = submit(scheduler, [](auto) { return fv::TaskResult{}; }, fv::Priority::Foreground);
    rejected = scheduler.submit({1, fv::Priority::Foreground, {}, {}, [](auto) { return fv::TaskResult{}; }});
    check(!rejected.task && rejected.error->code == fv::ErrorCode::QueueFull, "Total queue exceeded capacity");
    check(scheduler.stats().queued == 4 && scheduler.stats().running == 2, "Queue/concurrency counters exceed limits");
    gate->open(); foreground_gate->open();
    check(done(first).state == fv::TaskState::Succeeded && done(fg).state == fv::TaskState::Succeeded, "Blocked work failed");
    for (const auto& task : queued) check(done(task).state == fv::TaskState::Succeeded, "Queued work failed");
    check(done(fg_queued).state == fv::TaskState::Succeeded, "Foreground queued work failed");
}
void budgets() {
    auto config = limits(3);
    config.foreground_reserve = {1, 1, 20, 5};
    auto scheduler = fv::Scheduler{config};
    auto gate = std::make_shared<Gate>();
    auto started = std::promise<void>{}; auto future = started.get_future();
    auto first = submit(scheduler, [&, gate](auto stop) { started.set_value(); gate->wait(stop); return fv::TaskResult{}; }, fv::Priority::Background, {1, 1, 60, 10});
    ready(future);
    auto second = submit(scheduler, [](auto) { return fv::TaskResult{}; }, fv::Priority::Background, {1, 1, 60, 10});
    check(!second.wait_for(20ms), "I/O or memory budget allowed competing background work");
    auto fg = submit(scheduler, [](auto) { return fv::TaskResult{}; }, fv::Priority::Foreground, {1, 1, 30, 5});
    check(done(fg).state == fv::TaskState::Succeeded, "Reserved resources did not admit foreground work");
    for (const auto resource : {fv::Resources{4, 0, 0, 0}, fv::Resources{1, 3, 0, 0}, fv::Resources{1, 0, 101, 0}, fv::Resources{1, 0, 0, 21}}) {
        const auto rejected = scheduler.submit({1, fv::Priority::Foreground, resource, {}, [](auto) { return fv::TaskResult{}; }});
        check(rejected.error && rejected.error->code == fv::ErrorCode::ResourceLimit, "Impossible reservation accepted");
    }
    gate->open(); (void)done(first); (void)done(second);
    check(scheduler.stats().reserved.memory_bytes == 0 && scheduler.stats().reserved.io_slots == 0, "Reservations leaked after completion");
    // Isolate each budget so one exhausted resource cannot mask another broken limit.
    for (const auto resource : {fv::Resources{2, 0, 0, 0}, fv::Resources{1, 2, 0, 0}, fv::Resources{1, 0, 60, 0}, fv::Resources{1, 0, 0, 10}}) {
        auto isolated_config = limits(4);
        isolated_config.capacity = {4, 4, 100, 20};
        isolated_config.foreground_reserve = {1, 1, 20, 5};
        auto isolated = fv::Scheduler{isolated_config};
        auto held_gate = std::make_shared<Gate>();
        auto held_started = std::promise<void>{}; auto held_future = held_started.get_future();
        auto held = submit(isolated, [&, held_gate](auto stop) { held_started.set_value(); held_gate->wait(stop); return fv::TaskResult{}; }, fv::Priority::Background, resource);
        ready(held_future);
        auto waiting = submit(isolated, [](auto) { return fv::TaskResult{}; }, fv::Priority::Background, resource);
        check(!waiting.wait_for(20ms), "Independent CPU/I/O/memory/GPU reservation limit failed");
        held_gate->open(); (void)done(held); (void)done(waiting);
    }
}
void cancellation() {
    auto scheduler = fv::Scheduler{limits(1)};
    auto gate = std::make_shared<Gate>();
    auto started = std::promise<void>{}; auto future = started.get_future();
    auto observed_stop = std::atomic<bool>{};
    auto task = submit(scheduler, [&, gate](auto stop) {
        started.set_value(); gate->wait(); observed_stop = stop.stop_requested();
        auto result = fv::TaskResult{};
        auto item = fv::ItemResult{}; item.id = fv::ItemId{"committed-before-cancel"}; result.items.push_back(item);
        return result;
    });
    ready(future);
    auto invoked = std::atomic<bool>{};
    auto queued = submit(scheduler, [&](auto) { invoked = true; return fv::TaskResult{}; });
    check(queued.cancel(), "Queued cancellation not accepted");
    check(done(queued).state == fv::TaskState::Cancelled && !invoked, "Cancelled queued function ran");
    check(task.cancel(), "Running cancellation not accepted");
    check(task.snapshot().state == fv::TaskState::Cancelling && !task.snapshot().result, "Intent incorrectly reported as terminal cancellation");
    check(!task.wait_for(20ms), "Noninterruptible section falsely completed on cancel");
    gate->open();
    const auto terminal = done(task);
    check(terminal.state == fv::TaskState::Cancelled && observed_stop, "Work did not observe cancellation token");
    check(terminal.result->items.size() == 1, "Cancellation erased committed item results");
    check(!task.cancel(), "Cancellation changed terminal task");
    auto cooperative_started = std::promise<void>{}; auto cooperative_future = cooperative_started.get_future();
    auto never_opened = std::make_shared<Gate>();
    auto cooperative = submit(scheduler, [&, never_opened](auto stop) { cooperative_started.set_value(); never_opened->wait(stop); return fv::TaskResult{}; });
    ready(cooperative_future); cooperative.cancel();
    check(done(cooperative).state == fv::TaskState::Cancelled, "Cooperative cancellation did not stop work");
}
void replacement() {
    auto scheduler = fv::Scheduler{limits()};
    auto latest = fv::LatestRequest{};
    auto gate = std::make_shared<Gate>();
    auto started = std::promise<void>{}; auto future = started.get_future();
    const auto old_ticket = latest.issue();
    auto old = submit(scheduler, [&, gate](auto) { started.set_value(); gate->wait(); return fv::TaskResult{}; }, fv::Priority::Background, {}, old_ticket);
    ready(future);
    for (auto i = 0; i < 100; ++i) (void)latest.issue();
    const auto new_ticket = latest.issue();
    auto replacement_task = submit(scheduler, [](auto) { return fv::TaskResult{}; }, fv::Priority::Foreground, {}, new_ticket);
    check(done(replacement_task).state == fv::TaskState::Succeeded, "Newest request did not complete");
    gate->open();
    check(done(old).state == fv::TaskState::StaleDiscarded, "Late request was not discarded");
    auto shown = 0;
    check(new_ticket.publish([&] { shown = 2; }), "Current result refused publication");
    check(!old_ticket.publish([&] { shown = 1; }) && shown == 2, "Stale result replaced current presentation");
    (void)latest.issue();
    check(!new_ticket.publish([&] { shown = 3; }) && shown == 2, "Result completed before replacement escaped publication guard");
    auto ran = false;
    auto already_stale = submit(scheduler, [&](auto) { ran = true; return fv::TaskResult{}; }, fv::Priority::Foreground, {}, old_ticket);
    check(done(already_stale).state == fv::TaskState::StaleDiscarded && !ran, "Already stale request executed");
}
void priority() {
    auto scheduler = fv::Scheduler{limits()};
    auto gate = std::make_shared<Gate>();
    auto started = std::promise<void>{}; auto future = started.get_future();
    auto background = submit(scheduler, [&, gate](auto stop) { started.set_value(); gate->wait(stop); return fv::TaskResult{}; });
    ready(future);
    auto bg = std::vector<fv::TaskHandle>{};
    for (auto i = 0; i < 3; ++i) bg.push_back(submit(scheduler, [](auto) { return fv::TaskResult{}; }));
    for (auto i = 0; i < 100; ++i) {
        auto foreground = submit(scheduler, [](auto) { return fv::TaskResult{}; }, fv::Priority::Foreground);
        check(done(foreground).state == fv::TaskState::Succeeded, "Background saturation starved foreground");
        check(!background.wait_for(0ms), "Background gate unexpectedly ended");
    }
    gate->open(); (void)done(background);
    for (const auto& task : bg) (void)done(task);
    // With one worker, foreground queued behind an active step must precede queued background.
    auto serial = fv::Scheduler{limits(1)};
    auto serial_gate = std::make_shared<Gate>();
    auto active_started = std::promise<void>{}; auto active_future = active_started.get_future();
    auto active = submit(serial, [&, serial_gate](auto stop) { active_started.set_value(); serial_gate->wait(stop); return fv::TaskResult{}; });
    ready(active_future);
    auto order = std::vector<int>{};
    auto low = submit(serial, [&](auto) { order.push_back(2); return fv::TaskResult{}; });
    auto high = submit(serial, [&](auto) { order.push_back(1); return fv::TaskResult{}; }, fv::Priority::Foreground);
    serial_gate->open(); (void)done(active); (void)done(high); (void)done(low);
    check(order == std::vector<int>({1, 2}), "Queued foreground did not precede background");
}
void errors() {
    auto scheduler = fv::Scheduler{limits()};
    auto throwing = submit(scheduler, [](auto) -> fv::TaskResult { throw std::runtime_error("decoder exploded"); });
    const auto failed = done(throwing);
    check(failed.state == fv::TaskState::Failed && failed.result->error->message == "decoder exploded", "Exception detail did not propagate");
    auto structured = submit(scheduler, [](auto) {
        return fv::TaskResult{1, {}, fv::Error{fv::ErrorCode::InputChanged, "snapshot mismatch", {{"item", "42"}}, true}};
    });
    check(done(structured).result->error->code == fv::ErrorCode::InputChanged, "Structured error code lost");
    auto mixed = submit(scheduler, [](auto) {
        auto result = fv::TaskResult{};
        auto good = fv::ItemResult{}; good.id = fv::ItemId{"good"};
        auto bad = fv::ItemResult{}; bad.id = fv::ItemId{"bad"}; bad.status = fv::ItemStatus::Failed;
        bad.error = fv::Error{fv::ErrorCode::Io, "write failed", {}, true};
        result.items = {good, bad}; return result;
    });
    check(done(mixed).state == fv::TaskState::PartiallySucceeded, "Partial result reported complete success");
    auto invalid = scheduler.submit({99, fv::Priority::Foreground, {}, {}, [](auto) { return fv::TaskResult{}; }});
    check(invalid.error->code == fv::ErrorCode::InvalidArgument, "Unknown task version accepted");
    auto invalidPriority = scheduler.submit({1, static_cast<fv::Priority>(99), {}, {}, [](auto) { return fv::TaskResult{}; }});
    if (invalidPriority.task)
        check(!invalidPriority.task->wait_for(50ms) && invalidPriority.task->snapshot().state == fv::TaskState::Queued,
              "Accepted unknown priority did not remain queued");
    check(!invalidPriority.task && invalidPriority.error && invalidPriority.error->code == fv::ErrorCode::InvalidArgument,
          "Unknown priority accepted and left queued");
    auto healthy = submit(scheduler, [](auto) { return fv::TaskResult{}; });
    check(done(healthy).state == fv::TaskState::Succeeded, "Worker did not survive thrown exception");
}
void shutdown() {
    auto scheduler = fv::Scheduler{limits(1)};
    auto marker = std::make_shared<int>(42); auto weak = std::weak_ptr<int>{marker};
    auto gate = std::make_shared<Gate>();
    auto started = std::promise<void>{}; auto future = started.get_future();
    auto active = submit(scheduler, [&, marker, gate](auto stop) { started.set_value(); gate->wait(stop); return fv::TaskResult{}; });
    ready(future);
    auto queued_ran = false;
    auto queued = submit(scheduler, [&, marker](auto) { queued_ran = true; return fv::TaskResult{}; });
    marker.reset();
    scheduler.shutdown();
    check(done(active).state == fv::TaskState::Cancelled && done(queued).state == fv::TaskState::Cancelled && !queued_ran, "Shutdown did not cancel and join");
    check(weak.expired(), "Shutdown retained work captures");
    check(scheduler.stats().running == 0 && scheduler.stats().queued == 0 && scheduler.stats().reserved.cpu_slots == 0, "Shutdown leaked reservations");
    const auto rejected = scheduler.submit({1, fv::Priority::Foreground, {}, {}, [](auto) { return fv::TaskResult{}; }});
    check(rejected.error->code == fv::ErrorCode::ShuttingDown, "Closed scheduler accepted work");
    scheduler.shutdown();
}
void stress() {
    auto config = limits(4); config.queue_capacity = 32; config.foreground_queue_reserve = 4;
    auto scheduler = fv::Scheduler{config};
    auto executed = std::atomic<int>{}; auto accepted = std::atomic<int>{}; auto terminals = std::atomic<int>{};
    auto producers = std::vector<std::jthread>{};
    for (auto producer = 0; producer < 4; ++producer) {
        producers.emplace_back([&] {
            for (auto i = 0; i < 200; ++i) {
                auto result = scheduler.submit({1, i % 3 == 0 ? fv::Priority::Foreground : fv::Priority::Background, {}, {},
                    [&](auto) { ++executed; return fv::TaskResult{}; }});
                if (result.task) {
                    ++accepted;
                    if (i % 5 == 0) result.task->cancel();
                    if (result.task->wait_for(4s) && fv::is_terminal(result.task->snapshot().state)) ++terminals;
                }
            }
        });
    }
    producers.clear(); scheduler.shutdown();
    check(accepted > 0 && accepted == terminals && executed <= accepted, "Concurrent submission/cancellation lost task terminal states");
}
void telemetry() {
    auto recorder = fv::PerformanceRecorder{100, 2};
    for (auto i = 1; i <= 100; ++i) check(recorder.record({"duration", fv::CachePath::Cold}, std::chrono::milliseconds{i}, 90ms), "Metric refused");
    const auto summary = recorder.summary({"duration", fv::CachePath::Cold});
    check(summary && summary->p95_ms == 95 && summary->p99_ms == 99 && summary->long_events == 10, "Tail quantiles or long event counts wrong");
    check(recorder.record({"duration", fv::CachePath::Warm}, 1ms), "Warm stream refused");
    check(recorder.summary({"duration", fv::CachePath::Warm})->p99_ms == 1, "Cold and warm observations mixed");
    check(!recorder.record({"overflow", fv::CachePath::Warm}, 1ms), "Metric cardinality unbounded");
    for (auto i = 0; i < 100; ++i) recorder.record({"duration", fv::CachePath::Cold}, 2ms);
    const auto window = recorder.summary({"duration", fv::CachePath::Cold});
    check(window->observations == 200 && window->retained == 100 && window->evicted == 100 && window->p99_ms == 2,
          "Telemetry window did not evict old samples");
    check(!recorder.record({"duration", fv::CachePath::Cold}, -1ns), "Negative duration accepted");
#ifdef _WIN32
    const auto memory = fv::process_memory();
    check(memory && memory->working_set_bytes > 0 && memory->peak_working_set_bytes >= memory->working_set_bytes, "Process memory sampling unavailable");
#endif
}
} // namespace
int main(int argc, char* argv[]) {
    const auto cases = std::map<std::string_view, void(*)()>{{"identity", identity}, {"capabilities", capabilities}, {"bounds", bounds},
        {"budgets", budgets}, {"cancellation", cancellation}, {"replacement", replacement}, {"priority", priority}, {"errors", errors},
        {"shutdown", shutdown}, {"stress", stress}, {"telemetry", telemetry}};
    if (argc != 2 || !cases.contains(argv[1])) return 2;
    try { cases.at(argv[1])(); std::cout << "PASS " << argv[1] << '\n'; return 0; }
    catch (const std::exception& error) { std::cerr << "FAIL " << argv[1] << ": " << error.what() << '\n'; return 1; }
}
