#include "../engine.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <vector>

using namespace TLSSMON;

namespace {

using namespace std::chrono_literals;

using ExpectedSetTimerFunction =
    std::optional<TimerHandle> (Engine::*)(
                                           MonCallback,
                                           TimerFlags,
                                           std::chrono::milliseconds);

static_assert(
              std::is_same_v<
              decltype(&Engine::set_timer),
              ExpectedSetTimerFunction>);

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
struct RunOutcome {
    bool _completed_on_time{false};
    ENGINESTATE _state{ENGINESTATE::NOTREADY};
};
RunOutcome wait_for_run_to_finish(
                                  Engine& engine,
                                  std::future<ENGINESTATE>& result,
                                  std::thread& runner,
                                  std::chrono::milliseconds timeout)
{
    const bool completed_on_time =
        result.wait_for(timeout) == std::future_status::ready;

    /*
     * 当前阶段 7 尚未实现时，Timer 不会触发。
     * 测试超时后必须主动停止 Engine，避免测试线程永久阻塞。
     */
    if (!completed_on_time) {
        engine.stop();
    }

    runner.join();

    return RunOutcome{
        completed_on_time,
            result.get()
    };
}

void test_run_activates_timer_without_user_aio()
{
    std::atomic<int> fire_count{0};

    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    const auto handle = engine.set_timer(
                                         MonCallback{
                                         "engine-once-timer",
                                         [&] {
                                         ++fire_count;
                                         engine.stop();
                                         return 0;
                                         },
                                         false},
                                         TimerFlags::ONCE,
                                         20ms);

    assert(handle);

    std::promise<ENGINESTATE> run_promise;
    std::future<ENGINESTATE> run_result =
        run_promise.get_future();

    std::thread runner([&] {
                       run_promise.set_value(engine.run());
                       });

    const RunOutcome outcome = wait_for_run_to_finish(
                                                      engine,
                                                      run_result,
                                                      runner,
                                                      1s);

    assert(outcome._completed_on_time);
    assert(outcome._state == ENGINESTATE::SUCCESSFUL);
    assert(fire_count.load() == 1);
    assert(engine.get_phase() == EnginePhase::STOPPED);
}

void test_run_checks_due_timer_before_processing_aio()
{
    TestPipe input;

    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    std::vector<int> activation_order;

    const auto timer_handle = engine.set_timer(
                                               MonCallback{
                                               "ordered-timer",
                                               [&] {
                                               activation_order.push_back(1);
                                               return 0;
                                               },
                                               false},
                                               TimerFlags::ONCE,
                                               10ms);

    assert(timer_handle);

    const auto aio_handle = engine.add_aio(
                                           input.read_fd(),
                                           MonCallback{
                                           "ordered-aio",
                                           [&] {
                                           input.drain_one();
                                           activation_order.push_back(2);
                                           engine.stop();
                                           return 0;
                                           },
                                           false});

    assert(aio_handle);

    /*
     * 在启动 run() 前让 Timer 确定到期，同时让用户 fd 就绪。
     * run() 第一轮必须先激活 Timer，再处理 AIO。
     */
    std::this_thread::sleep_for(30ms);
    input.signal();

    std::promise<ENGINESTATE> run_promise;
    std::future<ENGINESTATE> run_result =
        run_promise.get_future();

    std::thread runner([&] {
                       run_promise.set_value(engine.run());
                       });

    const RunOutcome outcome = wait_for_run_to_finish(
                                                      engine,
                                                      run_result,
                                                      runner,
                                                      1s);

    assert(outcome._completed_on_time);
    assert(outcome._state == ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::STOPPED);

    const std::vector<int> expected_order{1, 2};
    assert(activation_order == expected_order);
}

void test_running_engine_recalculates_timeout_for_new_earlier_timer()
{
    std::atomic<int> early_fire_count{0};
    std::atomic<int> late_fire_count{0};

    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    std::promise<ENGINESTATE> run_promise;
    std::future<ENGINESTATE> run_result =
        run_promise.get_future();

    std::thread runner([&] {
                       run_promise.set_value(engine.run());
                       });

    assert(wait_for_phase(
                          engine,
                          EnginePhase::RUNNING,
                          1s));

    const auto late_handle = engine.set_timer(
                                              MonCallback{
                                              "late-timer",
                                              [&] {
                                              ++late_fire_count;
                                              return 0;
                                              },
                                              false},
                                              TimerFlags::ONCE,
                                              2s);

    assert(late_handle);

    /*
     * 给 Reactor 足够时间进入以 late-timer 为 timeout 的 select()。
     */
    std::this_thread::sleep_for(50ms);

    const auto early_handle = engine.set_timer(
                                               MonCallback{
                                               "early-timer",
                                               [&] {
                                               ++early_fire_count;
                                               engine.stop();
                                               return 0;
                                               },
                                               false},
                                               TimerFlags::ONCE,
                                               20ms);

    assert(early_handle);

    const RunOutcome outcome = wait_for_run_to_finish(
                                                      engine,
                                                      run_result,
                                                      runner,
                                                      1s);

    assert(outcome._completed_on_time);
    assert(outcome._state == ENGINESTATE::SUCCESSFUL);
    assert(early_fire_count.load() == 1);
    assert(late_fire_count.load() == 0);
    assert(engine.get_phase() == EnginePhase::STOPPED);
}

void test_run_reactivates_recurring_timer()
{
    constexpr int expected_fire_count = 3;
    std::atomic<int> fire_count{0};

    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    const auto handle = engine.set_timer(
                                         MonCallback{
                                         "engine-recurring-timer",
                                         [&] {
                                         const int current = ++fire_count;

                                         if (current == expected_fire_count) {
                                         engine.stop();
                                         }

                                         return 0;
                                         },
                                         false},
                                         TimerFlags::RECURRING,
                                         20ms);

    assert(handle);

    std::promise<ENGINESTATE> run_promise;
    std::future<ENGINESTATE> run_result =
        run_promise.get_future();

    std::thread runner([&] {
                       run_promise.set_value(engine.run());
                       });

    const RunOutcome outcome = wait_for_run_to_finish(
                                                      engine,
                                                      run_result,
                                                      runner,
                                                      1s);

    assert(outcome._completed_on_time);
    assert(outcome._state == ENGINESTATE::SUCCESSFUL);
    assert(fire_count.load() == expected_fire_count);
    assert(engine.get_phase() == EnginePhase::STOPPED);
}


void test_set_timer_requires_ready_or_running_phase()
{
    Engine engine(MonConfig{"monitor", 9000, 1});

    assert(!engine.set_timer(
                             MonCallback{"before-init", [] { return 0; }, false},
                             TimerFlags::ONCE,
                             1s));

    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    const auto handle = engine.set_timer(
                                         MonCallback{"ready-timer", [] { return 0; }, false},
                                         TimerFlags::ONCE,
                                         1s);

    assert(handle);
    assert(handle->_id == 1);
}

void test_set_timer_forwards_validation_and_preserves_ids()
{
    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    assert(!engine.set_timer(
                             MonCallback{"empty-callback", {}, false},
                             TimerFlags::ONCE,
                             100ms));

    assert(!engine.set_timer(
                             MonCallback{"zero-delay", [] { return 0; }, false},
                             TimerFlags::ONCE,
                             0ms));

    assert(!engine.set_timer(
                             MonCallback{"negative-delay", [] { return 0; }, false},
                             TimerFlags::ONCE,
                             -1ms));

    const auto unknown_flags =
        static_cast<TimerFlags>(1U << 7);

    assert(!engine.set_timer(
                             MonCallback{"unknown-flags", [] { return 0; }, false},
                             unknown_flags,
                             100ms));

    assert(!engine.set_timer(
                             MonCallback{"once-worker", [] { return 0; }, false},
                             TimerFlags::ONCE | TimerFlags::WORKER,
                             100ms));

    const auto first = engine.set_timer(
                                        MonCallback{"first-valid", [] { return 0; }, false},
                                        TimerFlags::ONCE,
                                        1s);

    const auto second = engine.set_timer(
                                         MonCallback{"recurring-worker", [] { return 0; }, false},
                                         TimerFlags::RECURRING | TimerFlags::WORKER,
                                         1s);

    assert(first);
    assert(second);
    assert(first->_id == 1);
    assert(second->_id == 2);
}

void test_set_timer_accepts_running_and_rejects_stopping_and_stopped()
{
    TestPipe input;
    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    std::mutex callback_mutex;
    std::condition_variable callback_condition;
    bool callback_started = false;
    bool release_callback = false;

    assert(engine.add_aio(
                          input.read_fd(),
                          MonCallback{
                          "blocking-aio",
                          [&] {
                          input.drain_one();

                          std::unique_lock<std::mutex> lock(callback_mutex);
                          callback_started = true;
                          callback_condition.notify_all();
                          callback_condition.wait(
                                                  lock,
                                                  [&] { return release_callback; });
                          return 0;
                          },
                          false}));

    std::promise<ENGINESTATE> run_promise;
    std::future<ENGINESTATE> run_result =
        run_promise.get_future();

    std::thread runner([&] {
                       run_promise.set_value(engine.run());
                       });

    assert(wait_for_phase(engine, EnginePhase::RUNNING, 1s));

    const auto running_handle = engine.set_timer(
                                                 MonCallback{"running-timer", [] { return 0; }, false},
                                                 TimerFlags::ONCE,
                                                 5s);
    assert(running_handle);

    input.signal();

    {
        std::unique_lock<std::mutex> lock(callback_mutex);
        assert(callback_condition.wait_for(
                                           lock,
                                           1s,
                                           [&] { return callback_started; }));
    }

    engine.stop();
    assert(engine.get_phase() == EnginePhase::STOPPING);

    assert(!engine.set_timer(
                             MonCallback{"stopping-timer", [] { return 0; }, false},
                             TimerFlags::ONCE,
                             1s));

    {
        std::lock_guard<std::mutex> lock(callback_mutex);
        release_callback = true;
    }
    callback_condition.notify_all();

    assert(run_result.wait_for(1s) == std::future_status::ready);
    runner.join();

    assert(run_result.get() == ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::STOPPED);

    assert(!engine.set_timer(
                             MonCallback{"stopped-timer", [] { return 0; }, false},
                             TimerFlags::ONCE,
                             1s));
}

void test_concurrent_set_timer_while_running_returns_unique_handles()
{
    constexpr int registration_count = 24;

    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    std::promise<ENGINESTATE> run_promise;
    std::future<ENGINESTATE> run_result =
        run_promise.get_future();

    std::thread runner([&] {
                       run_promise.set_value(engine.run());
                       });

    assert(wait_for_phase(engine, EnginePhase::RUNNING, 1s));

    std::mutex handles_mutex;
    std::vector<std::uint64_t> handles;
    handles.reserve(registration_count);

    std::vector<std::thread> workers;
    workers.reserve(registration_count);

    for (int index = 0; index < registration_count; ++index) {
        workers.emplace_back([&, index] {
                             const auto handle = engine.set_timer(
                                                                  MonCallback{
                                                                  "concurrent-timer-" + std::to_string(index),
                                                                  [] { return 0; },
                                                                  false},
                                                                  TimerFlags::ONCE,
                                                                  5s);

                             assert(handle);

                             std::lock_guard<std::mutex> lock(handles_mutex);
                             handles.push_back(handle->_id);
                             });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    assert(handles.size() == registration_count);

    const std::set<std::uint64_t> unique_handles(
                                                 handles.begin(),
                                                 handles.end());

    assert(unique_handles.size() == registration_count);

    engine.stop();

    assert(run_result.wait_for(1s) == std::future_status::ready);
    runner.join();

    assert(run_result.get() == ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::STOPPED);
}

} // namespace

int main()
{
    test_set_timer_requires_ready_or_running_phase();
    test_set_timer_forwards_validation_and_preserves_ids();
    test_set_timer_accepts_running_and_rejects_stopping_and_stopped();
    test_concurrent_set_timer_while_running_returns_unique_handles();

    test_run_activates_timer_without_user_aio();
    test_run_checks_due_timer_before_processing_aio();
    test_running_engine_recalculates_timeout_for_new_earlier_timer();
    test_run_reactivates_recurring_timer();

    return 0;
}
