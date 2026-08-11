#include "../callback_registry.h"
#include "../engine_type.h"
#include "../timer_manager.h"
#include "../wake_pipe.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <sys/select.h>
#include <thread>

using namespace TLSSMON;

namespace {

void test_add_wakes_blocked_select()
{
    CallbackRegistry registry;
    WakeupPipe wakeup;
    assert(wakeup.init() == PIPESTATUS::SUCCESSFUL);

    TimerManager manager(registry, wakeup);

    std::promise<void> select_starting;
    std::future<void> select_started = select_starting.get_future();

    std::atomic<int> select_result{-2};
    std::atomic<bool> wakeup_readable{false};

    std::thread waiter([&] {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(wakeup.read_fd(), &read_fds);

        timeval timeout{};
        timeout.tv_sec = 2;

        // If add() writes before select() enters the kernel, the byte remains
        // in the pipe and select() still observes the descriptor as readable.
        select_starting.set_value();

        const int result = ::select(
            wakeup.read_fd() + 1,
            &read_fds,
            nullptr,
            nullptr,
            &timeout);

        select_result.store(result);
        if (result > 0) {
            wakeup_readable.store(
                FD_ISSET(wakeup.read_fd(), &read_fds));
        }
    });

    select_started.wait();

    const auto handle = manager.add(
        MonCallback{
            "timer-wakeup",
            [] { return 0; },
            false},
        TimerFlags::ONCE,
        std::chrono::seconds{5});

    waiter.join();

    assert(handle);
    assert(handle->_id == 1);
    assert(select_result.load() == 1);
    assert(wakeup_readable.load());
    assert(wakeup.drain() == PIPESTATUS::SUCCESSFUL);
}

} // namespace

int main()
{
    test_add_wakes_blocked_select();
    return 0;
}
