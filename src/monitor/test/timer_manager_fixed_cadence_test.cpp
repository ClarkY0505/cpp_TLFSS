#include "callback_registry.h"
#include "engine_type.h"
#include "timer_manager.h"
#include "wake_pipe.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>

using namespace TLSSMON;

namespace {

using namespace std::chrono_literals;

std::int64_t timeout_us(const timeval& timeout)
{
    return static_cast<std::int64_t>(timeout.tv_sec) * 1'000'000
        + static_cast<std::int64_t>(timeout.tv_usec);
}

void test_recurring_timer_catches_up_from_previous_deadline()
{
    CallbackRegistry registry;
    WakeupPipe wakeup;
    assert(wakeup.init() == PIPESTATUS::SUCCESSFUL);

    TimerManager manager(registry, wakeup);
    int call_count = 0;

    assert(manager.add(
        MonCallback{"fixed-cadence", [&] { ++call_count; return 0; }, false},
        TimerFlags::RECURRING,
        25ms));
    assert(wakeup.drain() == PIPESTATUS::SUCCESSFUL);

    /*
     * More than two periods have elapsed. With next += interval, one check()
     * catches up each missed deadline. With next = now + interval, it would
     * fire only once and schedule the next deadline from the current time.
     */
    std::this_thread::sleep_for(65ms);

    timeval timeout{};
    manager.check(timeout);

    const std::int64_t remaining = timeout_us(timeout);
    assert(call_count >= 2);
    assert(timeout.tv_sec >= 0);
    assert(timeout.tv_usec >= 0);
    assert(timeout.tv_usec < 1'000'000);
    assert(remaining > 0);
    assert(remaining <= 25'000);
}

} // namespace

int main()
{
    test_recurring_timer_catches_up_from_previous_deadline();
    return 0;
}
