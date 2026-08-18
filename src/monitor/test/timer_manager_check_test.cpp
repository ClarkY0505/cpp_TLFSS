#include "../callback_registry.h"
#include "../engine_type.h"
#include "../timer_manager.h"
#include "../wake_pipe.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace TLSSMON;

namespace {

using namespace std::chrono_literals;

std::int64_t timeout_us(const timeval& timeout)
{
    return static_cast<std::int64_t>(timeout.tv_sec) * 1'000'000
        + static_cast<std::int64_t>(timeout.tv_usec);
}

void assert_normalized(const timeval& timeout)
{
    assert(timeout.tv_sec >= 0);
    assert(timeout.tv_usec >= 0);
    assert(timeout.tv_usec < 1'000'000);
}

std::string printed_stats(const CallbackRegistry& registry)
{
    std::ostringstream output;
    std::streambuf* const original = std::clog.rdbuf(output.rdbuf());

    registry.print_stats();

    std::clog.rdbuf(original);
    return output.str();
}

void test_check_returns_one_second_when_empty()
{
    CallbackRegistry registry;
    WakeupPipe wakeup;
    TimerManager manager(registry, wakeup);

    timeval timeout{-1, -1};
    manager.check(timeout);

    assert(timeout.tv_sec == 1);
    assert(timeout.tv_usec == 0);
}

void test_check_returns_time_until_future_timer()
{
    CallbackRegistry registry;
    WakeupPipe wakeup;
    assert(wakeup.init() == PIPESTATUS::SUCCESSFUL);

    TimerManager manager(registry, wakeup);
    int call_count = 0;

    assert(manager.add(
        MonCallback{"future", [&] { ++call_count; return 0; }, false},
        TimerFlags::ONCE,
        250ms));
    assert(wakeup.drain() == PIPESTATUS::SUCCESSFUL);

    timeval timeout{};
    manager.check(timeout);

    const std::int64_t remaining = timeout_us(timeout);
    assert(call_count == 0);
    assert_normalized(timeout);
    assert(remaining > 0);
    assert(remaining <= 250'000);
}

void test_once_timer_fires_once_and_unregisters()
{
    CallbackRegistry registry;
    WakeupPipe wakeup;
    assert(wakeup.init() == PIPESTATUS::SUCCESSFUL);

    TimerManager manager(registry, wakeup);
    int call_count = 0;

    assert(manager.add(
        MonCallback{"once-only", [&] { ++call_count; return 0; }, false},
        TimerFlags::ONCE,
        5ms));
    assert(wakeup.drain() == PIPESTATUS::SUCCESSFUL);
    assert(printed_stats(registry).find("once-only") != std::string::npos);

    std::this_thread::sleep_for(20ms);

    timeval timeout{};
    manager.check(timeout);

    assert(call_count == 1);
    assert(printed_stats(registry).find("once-only") == std::string::npos);
    assert(timeout.tv_sec == 1);
    assert(timeout.tv_usec == 0);

    manager.check(timeout);
    assert(call_count == 1);
}

void test_due_timers_fire_in_deadline_order()
{
    CallbackRegistry registry;
    WakeupPipe wakeup;
    assert(wakeup.init() == PIPESTATUS::SUCCESSFUL);

    TimerManager manager(registry, wakeup);
    std::vector<std::string> order;

    assert(manager.add(
        MonCallback{"later", [&] { order.push_back("later"); return 0; }, false},
        TimerFlags::ONCE,
        40ms));

    assert(manager.add(
        MonCallback{"earlier", [&] { order.push_back("earlier"); return 0; }, false},
        TimerFlags::ONCE,
        5ms));

    assert(wakeup.drain() == PIPESTATUS::SUCCESSFUL);
    std::this_thread::sleep_for(60ms);

    timeval timeout{};
    manager.check(timeout);

    assert(order.size() == 2);
    assert(order[0] == "earlier");
    assert(order[1] == "later");
    assert(timeout.tv_sec == 1);
    assert(timeout.tv_usec == 0);
}

void test_recurring_timer_is_reinserted()
{
    CallbackRegistry registry;
    WakeupPipe wakeup;
    assert(wakeup.init() == PIPESTATUS::SUCCESSFUL);

    TimerManager manager(registry, wakeup);
    int call_count = 0;

    assert(manager.add(
        MonCallback{"recurring", [&] { ++call_count; return 0; }, false},
        TimerFlags::RECURRING,
        30ms));
    assert(wakeup.drain() == PIPESTATUS::SUCCESSFUL);

    std::this_thread::sleep_for(45ms);

    timeval timeout{};
    manager.check(timeout);

    const int first_count = call_count;
    const std::int64_t remaining = timeout_us(timeout);

    assert(first_count >= 1);
    assert_normalized(timeout);
    assert(remaining > 0);
    assert(remaining <= 30'000);
    assert(printed_stats(registry).find("recurring") != std::string::npos);

    std::this_thread::sleep_for(
        std::chrono::microseconds{remaining} + 5ms);

    manager.check(timeout);
    assert(call_count > first_count);
}

void test_check_refreshes_now_after_callback()
{
    CallbackRegistry registry;
    WakeupPipe wakeup;
    assert(wakeup.init() == PIPESTATUS::SUCCESSFUL);

    TimerManager manager(registry, wakeup);
    int first_count = 0;
    int second_count = 0;

    assert(manager.add(
        MonCallback{
            "slow-first",
            [&] {
                ++first_count;
                std::this_thread::sleep_for(80ms);
                return 0;
            },
            false},
        TimerFlags::ONCE,
        1ms));

    assert(manager.add(
        MonCallback{"second", [&] { ++second_count; return 0; }, false},
        TimerFlags::ONCE,
        40ms));

    assert(wakeup.drain() == PIPESTATUS::SUCCESSFUL);
    std::this_thread::sleep_for(10ms);

    timeval timeout{};
    manager.check(timeout);

    assert(first_count == 1);
    assert(second_count == 1);
    assert(timeout.tv_sec == 1);
    assert(timeout.tv_usec == 0);
}

void test_callback_can_add_timer()
{
    CallbackRegistry registry;
    WakeupPipe wakeup;
    assert(wakeup.init() == PIPESTATUS::SUCCESSFUL);

    TimerManager manager(registry, wakeup);
    int parent_count = 0;
    int child_count = 0;
    std::optional<TimerHandle> child_handle;

    assert(manager.add(
        MonCallback{
            "parent",
            [&] {
                ++parent_count;
                child_handle = manager.add(
                    MonCallback{"child", [&] { ++child_count; return 0; }, false},
                    TimerFlags::ONCE,
                    5ms);
                return 0;
            },
            false},
        TimerFlags::ONCE,
        1ms));

    assert(wakeup.drain() == PIPESTATUS::SUCCESSFUL);
    std::this_thread::sleep_for(10ms);

    timeval timeout{};
    manager.check(timeout);

    assert(parent_count == 1);
    assert(child_handle);

    assert(wakeup.drain() == PIPESTATUS::SUCCESSFUL);
    std::this_thread::sleep_for(10ms);
    manager.check(timeout);

    assert(child_count == 1);
}

void test_add_can_run_while_check_executes_callback()
{
    CallbackRegistry registry;
    WakeupPipe wakeup;
    assert(wakeup.init() == PIPESTATUS::SUCCESSFUL);

    std::mutex callback_mutex;
    std::condition_variable callback_condition;
    bool callback_started = false;
    bool release_callback = false;

    TimerManager manager(registry, wakeup);
    int first_count = 0;
    int second_count = 0;

    assert(manager.add(
        MonCallback{
            "blocking-first",
            [&] {
                std::unique_lock<std::mutex> lock(callback_mutex);
                ++first_count;
                callback_started = true;
                callback_condition.notify_all();
                callback_condition.wait(lock, [&] { return release_callback; });
                return 0;
            },
            false},
        TimerFlags::ONCE,
        1ms));

    assert(wakeup.drain() == PIPESTATUS::SUCCESSFUL);
    std::this_thread::sleep_for(10ms);

    timeval reactor_timeout{};
    std::thread reactor([&] { manager.check(reactor_timeout); });

    {
        std::unique_lock<std::mutex> lock(callback_mutex);
        assert(callback_condition.wait_for(
            lock,
            1s,
            [&] { return callback_started; }));
    }

    auto add_future = std::async(std::launch::async, [&] {
        return manager.add(
            MonCallback{
                "concurrent-second",
                [&] { ++second_count; return 0; },
                false},
            TimerFlags::ONCE,
            1ms);
    });

    assert(add_future.wait_for(1s) == std::future_status::ready);
    const auto second_handle = add_future.get();
    assert(second_handle);

    std::this_thread::sleep_for(10ms);

    {
        std::lock_guard<std::mutex> lock(callback_mutex);
        release_callback = true;
    }
    callback_condition.notify_all();

    reactor.join();

    assert(first_count == 1);
    assert(second_count == 1);
    assert(wakeup.drain() == PIPESTATUS::SUCCESSFUL);
}

void test_recurring_worker_timer_stays_alive()
{
    CallbackRegistry registry;
    WakeupPipe wakeup;
    assert(wakeup.init() == PIPESTATUS::SUCCESSFUL);

    std::mutex worker_mutex;
    std::condition_variable worker_condition;
    int worker_count = 0;

    TimerManager manager(registry, wakeup);

    assert(manager.add(
        MonCallback{
            "recurring-worker",
            [&] {
                {
                    std::lock_guard<std::mutex> lock(worker_mutex);
                    ++worker_count;
                }
                worker_condition.notify_all();
                return 0;
            },
            false},
        TimerFlags::RECURRING | TimerFlags::WORKER,
        20ms));

    assert(wakeup.drain() == PIPESTATUS::SUCCESSFUL);
    std::this_thread::sleep_for(30ms);

    timeval timeout{};
    manager.check(timeout);

    {
        std::unique_lock<std::mutex> lock(worker_mutex);
        assert(worker_condition.wait_for(
            lock,
            1s,
            [&] { return worker_count > 0; }));
    }

    assert(printed_stats(registry).find("recurring-worker") != std::string::npos);
}

void test_once_worker_is_rejected()
{
    CallbackRegistry registry;
    WakeupPipe wakeup;
    assert(wakeup.init() == PIPESTATUS::SUCCESSFUL);

    TimerManager manager(registry, wakeup);

    assert(!manager.add(
        MonCallback{"once-worker", [] { return 0; }, false},
        TimerFlags::ONCE | TimerFlags::WORKER,
        10ms));
}

} // namespace

int main()
{
    test_check_returns_one_second_when_empty();
    test_check_returns_time_until_future_timer();
    test_once_timer_fires_once_and_unregisters();
    test_due_timers_fire_in_deadline_order();
    test_recurring_timer_is_reinserted();
    test_check_refreshes_now_after_callback();
    test_callback_can_add_timer();
    test_add_can_run_while_check_executes_callback();
    test_recurring_worker_timer_stays_alive();
    test_once_worker_is_rejected();
    return 0;
}
