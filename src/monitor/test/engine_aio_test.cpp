#include "../engine.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <memory>
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

bool wait_for_phase(
    Engine& engine,
    EnginePhase phase,
    std::chrono::milliseconds timeout)
{
    const auto deadline =
        std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        if (engine.get_phase() == phase) {
            return true;
        }

        std::this_thread::yield();
    }

    return engine.get_phase() == phase;
}

void test_engine_aio_requires_ready_or_running_phase()
{
    TestPipe input;
    Engine engine(MonConfig{"monitor", 9000, 1});

    assert(!engine.add_aio(
        input.read_fd(),
        MonCallback{"before-init", [] { return 0; }, false}));
    assert(!engine.remove_aio(AioHandle{999}));

    assert(engine.init() == ENGINESTATE::SUCCESSFUL);
    assert(!engine.add_aio(
        -1,
        MonCallback{"invalid-fd", [] { return 0; }, false}));

    const auto handle = engine.add_aio(
        input.read_fd(),
        MonCallback{"ready-registration", [] { return 0; }, false});

    assert(handle);
    assert(engine.remove_aio(*handle));
    assert(!engine.remove_aio(*handle));
}

void test_engine_dispatches_user_fd_and_removes_registration()
{
    using namespace std::chrono_literals;

    TestPipe input;
    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    std::atomic<int> call_count{0};
    std::promise<void> callback_called;
    std::future<void> callback_completed =
        callback_called.get_future();

    const auto handle = engine.add_aio(
        input.read_fd(),
        MonCallback{
            "engine-user-fd",
            [&] {
                input.drain_one();
                call_count.fetch_add(1, std::memory_order_relaxed);
                callback_called.set_value();
                return 0;
            },
            false});

    assert(handle);

    std::promise<ENGINESTATE> run_promise;
    std::future<ENGINESTATE> run_result =
        run_promise.get_future();

    std::thread runner([&] {
        run_promise.set_value(engine.run());
    });

    assert(wait_for_phase(engine, EnginePhase::RUNNING, 1s));

    input.signal();
    assert(
        callback_completed.wait_for(1s)
        == std::future_status::ready);
    assert(call_count.load(std::memory_order_relaxed) == 1);

    assert(engine.remove_aio(*handle));
    engine.stop();

    assert(
        run_result.wait_for(1s)
        == std::future_status::ready);
    runner.join();

    assert(run_result.get() == ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::STOPPED);
    assert(!engine.add_aio(
        input.read_fd(),
        MonCallback{"after-stop", [] { return 0; }, false}));
    assert(!engine.remove_aio(*handle));
}

void test_removed_engine_aio_is_not_dispatched()
{
    using namespace std::chrono_literals;

    TestPipe input;
    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    std::atomic<int> call_count{0};
    const auto handle = engine.add_aio(
        input.read_fd(),
        MonCallback{
            "removed-before-run",
            [&] {
                call_count.fetch_add(1, std::memory_order_relaxed);
                return 0;
            },
            false});

    assert(handle);
    assert(engine.remove_aio(*handle));

    std::promise<ENGINESTATE> run_promise;
    std::future<ENGINESTATE> run_result =
        run_promise.get_future();

    std::thread runner([&] {
        run_promise.set_value(engine.run());
    });

    assert(wait_for_phase(engine, EnginePhase::RUNNING, 1s));

    input.signal();
    std::this_thread::sleep_for(50ms);
    assert(call_count.load(std::memory_order_relaxed) == 0);

    engine.stop();
    assert(
        run_result.wait_for(1s)
        == std::future_status::ready);
    runner.join();

    assert(run_result.get() == ENGINESTATE::SUCCESSFUL);
    input.drain_one();
}

void test_concurrent_engine_add_remove_while_running()
{
    using namespace std::chrono_literals;

    constexpr int registration_count = 24;
    std::vector<std::unique_ptr<TestPipe>> inputs;
    inputs.reserve(registration_count);

    for (int index = 0; index < registration_count; ++index) {
        inputs.push_back(std::make_unique<TestPipe>());
    }

    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    std::promise<ENGINESTATE> run_promise;
    std::future<ENGINESTATE> run_result =
        run_promise.get_future();

    std::thread runner([&] {
        run_promise.set_value(engine.run());
    });

    assert(wait_for_phase(engine, EnginePhase::RUNNING, 1s));

    std::atomic<int> added{0};
    std::atomic<int> removed{0};
    std::vector<std::thread> workers;
    workers.reserve(registration_count);

    for (int index = 0; index < registration_count; ++index) {
        workers.emplace_back([&, index] {
            const auto handle = engine.add_aio(
                inputs[index]->read_fd(),
                MonCallback{"concurrent-engine-aio", [] { return 0; }, false});

            if (handle) {
                added.fetch_add(1, std::memory_order_relaxed);

                if (engine.remove_aio(*handle)) {
                    removed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    assert(added.load(std::memory_order_relaxed)
           == registration_count);
    assert(removed.load(std::memory_order_relaxed)
           == registration_count);

    engine.stop();
    assert(
        run_result.wait_for(1s)
        == std::future_status::ready);
    runner.join();

    assert(run_result.get() == ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::STOPPED);
}

} // namespace

int main()
{
    test_engine_aio_requires_ready_or_running_phase();
    test_engine_dispatches_user_fd_and_removes_registration();
    test_removed_engine_aio_is_not_dispatched();
    test_concurrent_engine_add_remove_while_running();
}
