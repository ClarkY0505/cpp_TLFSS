#include "../engine.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <dirent.h>
#include <future>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

using namespace TLSSMON;

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

class ClogCapture {
public:
    ClogCapture()
        : _previous(std::clog.rdbuf(_buffer.rdbuf()))
    {
    }

    ~ClogCapture()
    {
        std::clog.rdbuf(_previous);
    }

    ClogCapture(const ClogCapture&) = delete;
    ClogCapture& operator=(const ClogCapture&) = delete;

    std::string str() const
    {
        return _buffer.str();
    }

private:
    std::ostringstream _buffer;
    std::streambuf* _previous;
};

static std::size_t count_open_fds()
{
    DIR* directory = ::opendir("/proc/self/fd");
    assert(directory != nullptr);

    std::size_t count = 0;
    while (dirent* entry = ::readdir(directory)) {
        const std::string_view name{entry->d_name};
        if (name != "." && name != "..") {
            ++count;
        }
    }

    assert(::closedir(directory) == 0);
    return count;
}

static bool wait_for_phase(Engine& engine, EnginePhase target, std::chrono::milliseconds timeout)
{
    const auto deadline =
        std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        if (engine.get_phase() == target) {
            return true;
        }

        std::this_thread::yield();
    }

    return engine.get_phase() == target;
}

static void test_run_requires_ready_engine()
{
    Engine engine(MonConfig{"monitor", 9000, 1});

    assert(engine.run() == ENGINESTATE::NOTREADY);
    assert(engine.get_phase() == EnginePhase::CREATED);
}

static void test_stop_before_run_is_noop()
{
    Engine engine(MonConfig{"monitor", 9000, 1});

    assert(engine.init() == ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::READY);

    engine.stop();

    assert(engine.get_phase() == EnginePhase::READY);
}

static void test_run_blocks_until_stop()
{
    using namespace std::chrono_literals;

    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    std::promise<ENGINESTATE> promise;
    std::future<ENGINESTATE> result = promise.get_future();

    std::thread runner([&engine, &promise] {
                       promise.set_value(engine.run());
                       });

    assert(wait_for_phase(
                          engine,
                          EnginePhase::RUNNING,
                          1s));

    // 进入 RUNNING 后，run() 必须仍阻塞在 select()。
    assert(result.wait_for(50ms)
           == std::future_status::timeout);

    engine.stop();

    // stop() 必须通过 WakeupPipe 及时打断 select()。
    assert(result.wait_for(1s)
           == std::future_status::ready);

    runner.join();

    assert(result.get() == ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::STOPPED);
}

static void test_second_run_is_rejected_while_running()
{
    using namespace std::chrono_literals;

    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    std::promise<ENGINESTATE> promise;
    auto first_result = promise.get_future();

    std::thread runner([&engine, &promise] {
                       promise.set_value(engine.run());
                       });

    assert(wait_for_phase(
                          engine,
                          EnginePhase::RUNNING,
                          1s));

    assert(engine.run() == ENGINESTATE::ALREADYRUNNING);
    assert(engine.get_phase() == EnginePhase::RUNNING);

    engine.stop();

    assert(first_result.wait_for(1s)
           == std::future_status::ready);

    runner.join();

    assert(first_result.get() == ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::STOPPED);
}

static void test_stop_is_idempotent()
{
    using namespace std::chrono_literals;

    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    std::promise<ENGINESTATE> promise;
    auto result = promise.get_future();

    std::thread runner([&engine, &promise] {
                       promise.set_value(engine.run());
                       });

    assert(wait_for_phase(
                          engine,
                          EnginePhase::RUNNING,
                          1s));

    engine.stop();
    engine.stop();

    assert(result.wait_for(1s)
           == std::future_status::ready);

    runner.join();

    assert(result.get() == ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::STOPPED);

    // STOPPED 状态再次调用也应当是空操作。
    engine.stop();
    assert(engine.get_phase() == EnginePhase::STOPPED);
}

static void test_stopped_engine_cannot_restart()
{
    using namespace std::chrono_literals;

    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    std::promise<ENGINESTATE> promise;
    auto result = promise.get_future();

    std::thread runner([&engine, &promise] {
                       promise.set_value(engine.run());
                       });

    assert(wait_for_phase(
                          engine,
                          EnginePhase::RUNNING,
                          1s));

    engine.stop();

    assert(result.wait_for(1s)
           == std::future_status::ready);

    runner.join();
    assert(result.get() == ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::STOPPED);

    assert(engine.run() == ENGINESTATE::NOTREADY);
    assert(engine.get_phase() == EnginePhase::STOPPED);
}

static void test_run_closes_wakeup_pipe_before_return()
{
    using namespace std::chrono_literals;

    const std::size_t baseline = count_open_fds();

    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);
    assert(count_open_fds() == baseline + 2);

    std::promise<ENGINESTATE> promise;
    std::future<ENGINESTATE> result = promise.get_future();

    std::thread runner([&] {
        promise.set_value(engine.run());
    });

    assert(wait_for_phase(
        engine,
        EnginePhase::RUNNING,
        1s));

    engine.stop();

    assert(result.wait_for(1s) == std::future_status::ready);
    runner.join();

    assert(result.get() == ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::STOPPED);

    /*
     * Engine 仍然存活，因此这里验证 WakeupPipe 是由 run()
     * 的退出流程关闭，而不是等到 Engine 析构时才关闭。
     */
    assert(count_open_fds() == baseline);
}

static void test_run_joins_active_async_worker_before_return()
{
    using namespace std::chrono_literals;

    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    std::mutex callback_mutex;
    std::condition_variable callback_condition;
    bool callback_started = false;
    bool release_callback = false;
    std::atomic<bool> callback_finished{false};

    const auto timer = engine.set_timer(
        MonCallback{
            "blocking-worker",
            [&] {
                {
                    std::unique_lock<std::mutex> lock(callback_mutex);
                    callback_started = true;
                    callback_condition.notify_all();
                    callback_condition.wait(
                        lock,
                        [&] { return release_callback; });
                }

                callback_finished.store(true, std::memory_order_release);
                return 0;
            },
            false},
        TimerFlags::RECURRING | TimerFlags::WORKER,
        10ms);

    assert(timer);

    std::promise<ENGINESTATE> promise;
    std::future<ENGINESTATE> result = promise.get_future();

    std::thread runner([&] {
        promise.set_value(engine.run());
    });

    {
        std::unique_lock<std::mutex> lock(callback_mutex);
        assert(callback_condition.wait_for(
            lock,
            1s,
            [&] { return callback_started; }));
    }

    engine.stop();

    /*
     * Reactor 已经收到停止请求，但 run() 必须等待当前 Worker
     * 回调结束并 join，不能提前返回。
     */
    assert(result.wait_for(50ms) == std::future_status::timeout);
    assert(!callback_finished.load(std::memory_order_acquire));

    {
        std::lock_guard<std::mutex> lock(callback_mutex);
        release_callback = true;
    }
    callback_condition.notify_all();

    assert(result.wait_for(1s) == std::future_status::ready);
    runner.join();

    assert(callback_finished.load(std::memory_order_acquire));
    assert(result.get() == ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::STOPPED);
}

static void test_run_prints_timer_and_aio_stats_before_cleanup()
{
    using namespace std::chrono_literals;

    TestPipe input;
    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    const auto timer = engine.set_timer(
        MonCallback{
            "pending-lifecycle-timer",
            [] { return 0; },
            false},
        TimerFlags::ONCE,
        5s);
    assert(timer);

    const auto aio = engine.add_aio(
        input.read_fd(),
        MonCallback{
            "lifecycle-aio",
            [&] {
                input.drain_one();
                engine.stop();
                return 0;
            },
            false});
    assert(aio);

    std::promise<ENGINESTATE> promise;
    std::future<ENGINESTATE> result = promise.get_future();
    std::string stats;

    {
        ClogCapture capture;

        std::thread runner([&] {
            promise.set_value(engine.run());
        });

        assert(wait_for_phase(
            engine,
            EnginePhase::RUNNING,
            1s));

        input.signal();

        assert(result.wait_for(1s) == std::future_status::ready);
        runner.join();

        assert(result.get() == ENGINESTATE::SUCCESSFUL);
        assert(engine.get_phase() == EnginePhase::STOPPED);
        stats = capture.str();
    }

    /*
     * 未到期 Timer 和已经触发的用户 AIO 都必须在 cleanup
     * 注销回调之前出现在 Registry 统计中。
     */
    assert(stats.find("pending-lifecycle-timer") != std::string::npos);
    assert(stats.find("callbacks=lifecycle-aio count=1")
           != std::string::npos);
}

int main(){
    test_run_requires_ready_engine();
    test_stop_before_run_is_noop();
    test_run_blocks_until_stop();
    test_second_run_is_rejected_while_running();
    test_stop_is_idempotent();
    test_stopped_engine_cannot_restart();
    test_run_closes_wakeup_pipe_before_return();
    test_run_joins_active_async_worker_before_return();
    test_run_prints_timer_and_aio_stats_before_cleanup();

}
