#include "aio_manager.h"
#include "callback_registry.h"
#include "engine_type.h"
#include "wake_pipe.h"

#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <sys/select.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace TLSSMON;

namespace {

class TestPipe {
public:
    TestPipe()
    {
        int fds[2]{-1, -1};
        assert(::pipe(fds) == 0);
        _read_fd = fds[0];
        _write_fd = fds[1];
    }

    ~TestPipe()
    {
        assert(::close(_read_fd) == 0);
        assert(::close(_write_fd) == 0);
    }

    TestPipe(const TestPipe&) = delete;
    TestPipe& operator=(const TestPipe&) = delete;

    int read_fd() const noexcept
    {
        return _read_fd;
    }

    void signal() const
    {
        constexpr char value = '*';
        assert(::write(_write_fd, &value, sizeof(value)) == sizeof(value));
    }

    void drain_one() const
    {
        char value{};
        assert(::read(_read_fd, &value, sizeof(value)) == sizeof(value));
    }

private:
    int _read_fd{-1};
    int _write_fd{-1};
};

bool readable_now(int fd)
{
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);

    timeval timeout{};
    const int result = ::select(
        fd + 1,
        &read_fds,
        nullptr,
        nullptr,
        &timeout);

    return result == 1 && FD_ISSET(fd, &read_fds);
}

std::string printed_stats(const CallbackRegistry& registry)
{
    std::ostringstream output;
    std::streambuf* original = std::clog.rdbuf(output.rdbuf());

    registry.print_stats();

    std::clog.rdbuf(original);
    return output.str();
}

void test_add_rejects_invalid_fd()
{
    CallbackRegistry registry;
    WakeupPipe wakeup;
    assert(wakeup.init() == PIPESTATUS::SUCCESSFUL);

    AioManager manager(registry, wakeup);

    assert(!manager.add(
        -1,
        MonCallback{"negative-fd", [] { return 0; }, false}));

    assert(!manager.add(
        FD_SETSIZE,
        MonCallback{"oversized-fd", [] { return 0; }, false}));
}

void test_add_rejects_uninitialized_wakeup_pipe()
{
    CallbackRegistry registry;
    WakeupPipe wakeup;
    AioManager manager(registry, wakeup);

    assert(!manager.add(
        0,
        MonCallback{"not-ready", [] { return 0; }, false}));
}

void test_add_returns_handle_and_wakes_select()
{
    CallbackRegistry registry;
    WakeupPipe wakeup;
    assert(wakeup.init() == PIPESTATUS::SUCCESSFUL);

    AioManager manager(registry, wakeup);
    auto handle = manager.add(
        wakeup.read_fd(),
        MonCallback{"wakeup", [] { return 0; }, false});

    assert(handle);
    assert(handle->_id != 0);
    assert(readable_now(wakeup.read_fd()));
    assert(wakeup.drain() == PIPESTATUS::SUCCESSFUL);
}

void test_add_returns_unique_handles()
{
    CallbackRegistry registry;
    WakeupPipe wakeup;
    assert(wakeup.init() == PIPESTATUS::SUCCESSFUL);

    AioManager manager(registry, wakeup);

    auto first = manager.add(
        wakeup.read_fd(),
        MonCallback{"first", [] { return 0; }, false});

    auto second = manager.add(
        wakeup.read_fd(),
        MonCallback{"second", [] { return 0; }, false});

    assert(first);
    assert(second);
    assert(first->_id != second->_id);
}

void test_cleanup_unregisters_owned_callbacks()
{
    CallbackRegistry registry;
    WakeupPipe wakeup;
    assert(wakeup.init() == PIPESTATUS::SUCCESSFUL);

    {
        AioManager manager(registry, wakeup);
        auto handle = manager.add(
            wakeup.read_fd(),
            MonCallback{"owned-by-aio", [] { return 0; }, false});

        assert(handle);
        assert(
            printed_stats(registry).find("owned-by-aio")
            != std::string::npos);
    }

    assert(
        printed_stats(registry).find("owned-by-aio")
        == std::string::npos);
}

void test_process_activates_readable_callback()
{
    CallbackRegistry registry;
    WakeupPipe wakeup;
    TestPipe input;
    assert(wakeup.init() == PIPESTATUS::SUCCESSFUL);

    AioManager manager(registry, wakeup);
    int call_count = 0;

    const auto handle = manager.add(
        input.read_fd(),
        MonCallback{
            "readable",
            [&] {
                input.drain_one();
                ++call_count;
                return 0;
            },
            false});

    assert(handle);
    assert(wakeup.drain() == PIPESTATUS::SUCCESSFUL);

    input.signal();
    timeval timeout{0, 100000};

    assert(manager.process(&timeout) == 0);
    assert(call_count == 1);
}

void test_process_ignores_unreadable_callback()
{
    CallbackRegistry registry;
    WakeupPipe wakeup;
    TestPipe input;
    assert(wakeup.init() == PIPESTATUS::SUCCESSFUL);

    AioManager manager(registry, wakeup);
    int call_count = 0;

    assert(manager.add(
        input.read_fd(),
        MonCallback{
            "unreadable",
            [&] {
                ++call_count;
                return 0;
            },
            false}));

    assert(wakeup.drain() == PIPESTATUS::SUCCESSFUL);

    timeval timeout{};
    assert(manager.process(&timeout) == 0);
    assert(call_count == 0);
}

void test_same_fd_activates_each_registered_callback()
{
    CallbackRegistry registry;
    WakeupPipe wakeup;
    TestPipe input;
    assert(wakeup.init() == PIPESTATUS::SUCCESSFUL);

    AioManager manager(registry, wakeup);
    int first_count = 0;
    int second_count = 0;

    assert(manager.add(
        input.read_fd(),
        MonCallback{"first-reader", [&] { ++first_count; return 0; }, false}));
    assert(manager.add(
        input.read_fd(),
        MonCallback{"second-reader", [&] { ++second_count; return 0; }, false}));

    assert(wakeup.drain() == PIPESTATUS::SUCCESSFUL);
    input.signal();

    timeval timeout{0, 100000};
    assert(manager.process(&timeout) == 0);

    assert(first_count == 1);
    assert(second_count == 1);
    input.drain_one();
}

void test_remove_is_lazy_and_prevents_future_activation()
{
    CallbackRegistry registry;
    WakeupPipe wakeup;
    TestPipe input;
    assert(wakeup.init() == PIPESTATUS::SUCCESSFUL);

    AioManager manager(registry, wakeup);
    int call_count = 0;

    const auto handle = manager.add(
        input.read_fd(),
        MonCallback{"removed-reader", [&] { ++call_count; return 0; }, false});

    assert(handle);
    assert(wakeup.drain() == PIPESTATUS::SUCCESSFUL);
    assert(manager.remove(*handle));
    assert(!manager.remove(*handle));
    assert(wakeup.drain() == PIPESTATUS::SUCCESSFUL);

    input.signal();
    timeval timeout{};
    assert(manager.process(&timeout) == 0);

    assert(call_count == 0);
    assert(
        printed_stats(registry).find("removed-reader")
        == std::string::npos);
    input.drain_one();
}

void test_callback_can_remove_itself()
{
    CallbackRegistry registry;
    WakeupPipe wakeup;
    TestPipe input;
    assert(wakeup.init() == PIPESTATUS::SUCCESSFUL);

    AioManager manager(registry, wakeup);
    std::optional<AioHandle> handle;
    int call_count = 0;

    handle = manager.add(
        input.read_fd(),
        MonCallback{
            "self-removing",
            [&] {
                ++call_count;
                input.drain_one();
                assert(handle);
                assert(manager.remove(*handle));
                return 0;
            },
            false});

    assert(handle);
    assert(wakeup.drain() == PIPESTATUS::SUCCESSFUL);
    input.signal();

    timeval timeout{0, 100000};
    assert(manager.process(&timeout) == 0);
    assert(call_count == 1);

    assert(wakeup.drain() == PIPESTATUS::SUCCESSFUL);
    timeout = timeval{};
    assert(manager.process(&timeout) == 0);
    assert(
        printed_stats(registry).find("self-removing")
        == std::string::npos);
}

void test_process_dispatches_async_callback()
{
    using namespace std::chrono_literals;

    CallbackRegistry registry;
    WakeupPipe wakeup;
    TestPipe input;
    assert(wakeup.init() == PIPESTATUS::SUCCESSFUL);

    AioManager manager(registry, wakeup);
    std::promise<void> called;
    std::future<void> completed = called.get_future();

    assert(manager.add(
        input.read_fd(),
        MonCallback{
            "async-reader",
            [&] {
                input.drain_one();
                called.set_value();
                return 0;
            },
            true}));

    assert(wakeup.drain() == PIPESTATUS::SUCCESSFUL);
    input.signal();

    timeval timeout{0, 100000};
    assert(manager.process(&timeout) == 0);
    assert(completed.wait_for(1s) == std::future_status::ready);

    registry.stop_workers();
}

void test_concurrent_add_is_deferred_until_next_process_pass()
{
    using namespace std::chrono_literals;

    CallbackRegistry registry;
    WakeupPipe wakeup;
    assert(wakeup.init() == PIPESTATUS::SUCCESSFUL);

    AioManager manager(registry, wakeup);

    assert(manager.add(
        wakeup.read_fd(),
        MonCallback{
            "control-wakeup",
            [&] {
                return static_cast<int>(wakeup.drain());
            },
            false}));

    // add() writes one byte; remove it before starting the blocked process().
    assert(wakeup.drain() == PIPESTATUS::SUCCESSFUL);

    std::promise<void> process_started;
    std::future<void> started = process_started.get_future();
    std::promise<int> process_finished;
    std::future<int> result = process_finished.get_future();

    std::thread reactor([&] {
        process_started.set_value();
        process_finished.set_value(manager.process(nullptr));
    });

    assert(started.wait_for(1s) == std::future_status::ready);
    assert(result.wait_for(100ms) == std::future_status::timeout);

    int late_callback_count = 0;
    assert(manager.add(
        wakeup.read_fd(),
        MonCallback{
            "late-same-fd",
            [&] {
                ++late_callback_count;
                return 0;
            },
            false}));

    assert(result.wait_for(1s) == std::future_status::ready);
    reactor.join();

    assert(result.get() == 0);

    // The late entry was not part of the fd_set snapshot for the first pass.
    assert(late_callback_count == 0);

    // It becomes eligible after the next process() rebuilds its snapshot.
    assert(wakeup.wakeup() == PIPESTATUS::SUCCESSFUL);
    timeval timeout{0, 100000};
    assert(manager.process(&timeout) == 0);
    assert(late_callback_count == 1);
}

void test_concurrent_add_returns_unique_handles()
{
    CallbackRegistry registry;
    WakeupPipe wakeup;
    assert(wakeup.init() == PIPESTATUS::SUCCESSFUL);

    AioManager manager(registry, wakeup);
    constexpr int thread_count = 4;
    constexpr int registrations_per_thread = 32;

    std::mutex handles_mutex;
    std::vector<std::uint64_t> handles;
    handles.reserve(thread_count * registrations_per_thread);

    std::vector<std::thread> workers;
    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
        workers.emplace_back([&] {
            for (int registration = 0;
                 registration < registrations_per_thread;
                 ++registration) {
                const auto handle = manager.add(
                    wakeup.read_fd(),
                    MonCallback{"concurrent", [] { return 0; }, false});

                assert(handle);

                std::lock_guard<std::mutex> lock(handles_mutex);
                handles.push_back(handle->_id);
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    assert(handles.size()
           == static_cast<std::size_t>(
               thread_count * registrations_per_thread));

    const std::set<std::uint64_t> unique_handles(
        handles.begin(),
        handles.end());

    assert(unique_handles.size() == handles.size());
    assert(wakeup.drain() == PIPESTATUS::SUCCESSFUL);
}

} // namespace

int main()
{
    test_add_rejects_invalid_fd();
    test_add_rejects_uninitialized_wakeup_pipe();
    test_add_returns_handle_and_wakes_select();
    test_add_returns_unique_handles();
    test_cleanup_unregisters_owned_callbacks();
    test_process_activates_readable_callback();
    test_process_ignores_unreadable_callback();
    test_same_fd_activates_each_registered_callback();
    test_remove_is_lazy_and_prevents_future_activation();
    test_callback_can_remove_itself();
    test_process_dispatches_async_callback();
    test_concurrent_add_is_deferred_until_next_process_pass();
    test_concurrent_add_returns_unique_handles();
}
