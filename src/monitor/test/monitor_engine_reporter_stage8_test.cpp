#include "engine.h"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <variant>

#include <unistd.h>

using namespace TLSSMON;
using namespace std::chrono_literals;

namespace {

class TestPipe final {
public:
    TestPipe() {
        int descriptors[2]{-1, -1};
        assert(::pipe(descriptors) == 0);
        _read_fd = descriptors[0];
        _write_fd = descriptors[1];
    }

    ~TestPipe() {
        assert(::close(_read_fd) == 0);
        assert(::close(_write_fd) == 0);
    }

    TestPipe(const TestPipe&) = delete;
    TestPipe& operator=(const TestPipe&) = delete;

    int read_fd() const noexcept {
        return _read_fd;
    }

    void signal() const {
        const char byte = '*';
        assert(::write(_write_fd, &byte, sizeof(byte)) ==
               static_cast<ssize_t>(sizeof(byte)));
    }

    void drain_one() const {
        char byte{};
        assert(::read(_read_fd, &byte, sizeof(byte)) ==
               static_cast<ssize_t>(sizeof(byte)));
    }

private:
    int _read_fd{-1};
    int _write_fd{-1};
};

class Rendezvous final {
public:
    explicit Rendezvous(std::size_t target)
        : _target(target) {
        assert(_target > 0);
    }

    bool arrive_and_wait_for(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(_mutex);
        ++_arrived;
        _condition.notify_all();

        return _condition.wait_for(
            lock,
            timeout,
            [&] {
                return _arrived >= _target;
            });
    }

private:
    const std::size_t _target;
    std::mutex _mutex;
    std::condition_variable _condition;
    std::size_t _arrived{0};
};

bool wait_for_phase(
    Engine& engine,
    EnginePhase expected,
    std::chrono::milliseconds timeout) {

    const auto deadline =
        std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        if (engine.get_phase() == expected) {
            return true;
        }

        std::this_thread::yield();
    }

    return engine.get_phase() == expected;
}

const MonData::NumericValue& numeric_value(
    const MonData::StoredRecord& record) {

    return std::get<MonData::NumericValue>(
        record._data._value);
}

const std::string& string_value(
    const MonData::StoredRecord& record) {

    return std::get<std::string>(record._data._value);
}

/*
 * 阶段 8 案例 1：同步 Timer 回调调用 report_count()。
 *
 * 验证普通 Timer 在 Engine 的 reactor 线程中上报计数，Publisher 同步运行在
 * 同一线程；记录经过 Reporter/Store 后可以在 STOPPED 状态继续查询。
 */
void test_timer_callback_reports_count() {
    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    const MonData::MonitorKey key{80, 1, 1, 1};
    std::atomic<std::size_t> publish_count{0};
    std::promise<std::thread::id> publisher_thread_promise;
    std::future<std::thread::id> publisher_thread =
        publisher_thread_promise.get_future();

    assert(engine.set_publisher(
        [&](MonData::StoredRecord record) {
            assert(record._data._key == key);
            publish_count.fetch_add(1, std::memory_order_relaxed);
            publisher_thread_promise.set_value(
                std::this_thread::get_id());
        }));

    std::promise<std::thread::id> callback_thread_promise;
    std::future<std::thread::id> callback_thread =
        callback_thread_promise.get_future();
    std::promise<MonData::UpdateResult> report_result_promise;
    std::future<MonData::UpdateResult> report_result =
        report_result_promise.get_future();

    const auto timer = engine.set_timer(
        MonCallback{
            "stage8-timer-report-count",
            [&]() -> int {
                callback_thread_promise.set_value(
                    std::this_thread::get_id());
                report_result_promise.set_value(
                    engine.report_count(key, 7, "timer-count"));
                engine.stop();
                return 0;
            },
            false},
        TimerFlags::ONCE,
        10ms);

    assert(timer.has_value());

    std::promise<ENGINESTATE> run_promise;
    std::future<ENGINESTATE> run_result = run_promise.get_future();
    std::thread runner([&] {
        run_promise.set_value(engine.run());
    });

    const bool completed =
        run_result.wait_for(2s) == std::future_status::ready;
    if (!completed) {
        engine.stop();
    }
    runner.join();

    assert(completed);
    assert(run_result.get() == ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::STOPPED);

    const auto result = report_result.get();
    assert(result._status == MonData::UpdateStatus::INSERTED);
    assert(publish_count.load(std::memory_order_relaxed) == 1);
    assert(callback_thread.get() == publisher_thread.get());

    const auto stored = engine.find_data(key);
    assert(stored.has_value());
    assert(numeric_value(*stored)._value == 7);
    assert(numeric_value(*stored)._state == 0);
}

/*
 * 阶段 8 案例 2：异步 Timer worker 调用 report_error()。
 *
 * TimerManager 的异步模式要求 RECURRING | WORKER。callback_claimed 只允许
 * 首次激活上报并停止 Engine，避免 recurring Timer 在清理前重复处理。
 */
void test_async_timer_callback_reports_error() {
    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    const MonData::MonitorKey key{80, 2, 1, 1};
    std::atomic<bool> callback_claimed{false};
    std::atomic<std::size_t> publish_count{0};
    std::promise<std::thread::id> callback_thread_promise;
    std::future<std::thread::id> callback_thread =
        callback_thread_promise.get_future();
    std::promise<std::thread::id> publisher_thread_promise;
    std::future<std::thread::id> publisher_thread =
        publisher_thread_promise.get_future();
    std::promise<MonData::UpdateResult> report_result_promise;
    std::future<MonData::UpdateResult> report_result =
        report_result_promise.get_future();

    assert(engine.set_publisher(
        [&](MonData::StoredRecord record) {
            assert(record._data._key == key);
            publish_count.fetch_add(1, std::memory_order_relaxed);
            publisher_thread_promise.set_value(
                std::this_thread::get_id());
        }));

    const auto timer = engine.set_timer(
        MonCallback{
            "stage8-async-timer-report-error",
            [&]() -> int {
                if (callback_claimed.exchange(
                        true,
                        std::memory_order_acq_rel)) {
                    return 0;
                }

                callback_thread_promise.set_value(
                    std::this_thread::get_id());
                report_result_promise.set_value(
                    engine.report_error(key, 0, "timer-error"));
                engine.stop();
                return 0;
            },
            true},
        TimerFlags::RECURRING | TimerFlags::WORKER,
        10ms);

    assert(timer.has_value());

    std::promise<ENGINESTATE> run_promise;
    std::future<ENGINESTATE> run_result = run_promise.get_future();
    std::thread runner([&] {
        run_promise.set_value(engine.run());
    });

    const bool completed =
        run_result.wait_for(2s) == std::future_status::ready;
    if (!completed) {
        engine.stop();
    }
    runner.join();

    assert(completed);
    assert(run_result.get() == ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::STOPPED);

    const auto result = report_result.get();
    assert(result._status == MonData::UpdateStatus::INSERTED);
    assert(result._record.has_value());
    assert(numeric_value(*result._record)._value == 0);
    assert(numeric_value(*result._record)._state == 2);
    assert(publish_count.load(std::memory_order_relaxed) == 1);
    assert(callback_thread.get() == publisher_thread.get());
}

/*
 * 阶段 8 案例 3：异步 AIO 回调调用 report_string()。
 *
 * 同时验证 Publisher 与上报调用位于同一个 AIO worker，且完整字符串保存到
 * Store。callback_claimed 防止电平触发造成同一 fd 重复处理。
 */
void test_aio_callback_reports_string() {
    TestPipe input;
    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    const MonData::MonitorKey key{80, 3, 1, 1};
    std::atomic<bool> callback_claimed{false};
    std::atomic<std::size_t> publish_count{0};
    std::promise<std::thread::id> callback_thread_promise;
    std::future<std::thread::id> callback_thread =
        callback_thread_promise.get_future();
    std::promise<std::thread::id> publisher_thread_promise;
    std::future<std::thread::id> publisher_thread =
        publisher_thread_promise.get_future();
    std::promise<MonData::UpdateResult> report_result_promise;
    std::future<MonData::UpdateResult> report_result =
        report_result_promise.get_future();

    assert(engine.set_publisher(
        [&](MonData::StoredRecord record) {
            assert(record._data._key == key);
            publish_count.fetch_add(1, std::memory_order_relaxed);
            publisher_thread_promise.set_value(
                std::this_thread::get_id());
        }));

    const auto aio = engine.add_aio(
        input.read_fd(),
        MonCallback{
            "stage8-aio-report-string",
            [&]() -> int {
                if (callback_claimed.exchange(
                        true,
                        std::memory_order_acq_rel)) {
                    return 0;
                }

                input.drain_one();
                callback_thread_promise.set_value(
                    std::this_thread::get_id());
                report_result_promise.set_value(
                    engine.report_string(
                        key,
                        "aio-ready",
                        "aio-string"));
                engine.stop();
                return 0;
            },
            true});

    assert(aio.has_value());

    std::promise<ENGINESTATE> run_promise;
    std::future<ENGINESTATE> run_result = run_promise.get_future();
    std::thread runner([&] {
        run_promise.set_value(engine.run());
    });

    assert(wait_for_phase(engine, EnginePhase::RUNNING, 1s));
    input.signal();

    const bool completed =
        run_result.wait_for(2s) == std::future_status::ready;
    if (!completed) {
        engine.stop();
    }
    runner.join();

    assert(completed);
    assert(run_result.get() == ENGINESTATE::SUCCESSFUL);

    const auto result = report_result.get();
    assert(result._status == MonData::UpdateStatus::INSERTED);
    assert(publish_count.load(std::memory_order_relaxed) == 1);
    assert(callback_thread.get() == publisher_thread.get());

    const auto stored = engine.find_data(key);
    assert(stored.has_value());
    assert(string_value(*stored) == "aio-ready");
}

/*
 * 阶段 8 案例 4：两个异步 AIO 回调并发上报不同 Key。
 *
 * Rendezvous 要求两个 worker 都到达后才继续，证明它们确实并发存活。最后
 * 完成上报的 worker 负责 stop()，避免一个 worker 过早切换 STOPPING 使另一个
 * 尚未进入 report_count() 的调用被拒绝。
 */
void test_async_aio_callbacks_report_different_keys() {
    constexpr std::size_t callback_count = 2;
    std::array<TestPipe, callback_count> inputs;
    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    Rendezvous rendezvous(callback_count);
    std::array<std::atomic<bool>, callback_count> claimed{};
    std::atomic<std::size_t> rendezvous_success{0};
    std::atomic<std::size_t> completed{0};
    std::atomic<std::size_t> inserted{0};
    std::atomic<std::size_t> publish_count{0};

    assert(engine.set_publisher(
        [&](MonData::StoredRecord) {
            publish_count.fetch_add(1, std::memory_order_relaxed);
        }));

    for (std::size_t index = 0; index < callback_count; ++index) {
        const auto aio = engine.add_aio(
            inputs[index].read_fd(),
            MonCallback{
                "stage8-concurrent-different-key",
                [&, index]() -> int {
                    if (claimed[index].exchange(
                            true,
                            std::memory_order_acq_rel)) {
                        return 0;
                    }

                    inputs[index].drain_one();

                    if (rendezvous.arrive_and_wait_for(2s)) {
                        rendezvous_success.fetch_add(
                            1,
                            std::memory_order_relaxed);
                    }

                    const auto result = engine.report_count(
                        {80,
                         4,
                         static_cast<std::uint32_t>(index),
                         1},
                        1,
                        "different-key");

                    if (result._status ==
                        MonData::UpdateStatus::INSERTED) {
                        inserted.fetch_add(1, std::memory_order_relaxed);
                    }

                    if (completed.fetch_add(
                            1,
                            std::memory_order_acq_rel) + 1 ==
                        callback_count) {
                        engine.stop();
                    }

                    return 0;
                },
                true});

        assert(aio.has_value());
    }

    std::promise<ENGINESTATE> run_promise;
    std::future<ENGINESTATE> run_result = run_promise.get_future();
    std::thread runner([&] {
        run_promise.set_value(engine.run());
    });

    assert(wait_for_phase(engine, EnginePhase::RUNNING, 1s));

    for (const TestPipe& input : inputs) {
        input.signal();
    }

    const bool run_completed =
        run_result.wait_for(4s) == std::future_status::ready;
    if (!run_completed) {
        engine.stop();
    }
    runner.join();

    assert(run_completed);
    assert(run_result.get() == ENGINESTATE::SUCCESSFUL);
    assert(rendezvous_success.load(std::memory_order_relaxed) ==
           callback_count);
    assert(inserted.load(std::memory_order_relaxed) == callback_count);
    assert(publish_count.load(std::memory_order_relaxed) == callback_count);
    assert(engine.query_data().size() == callback_count);
}

/*
 * 阶段 8 案例 5：多个异步 AIO 回调上报同一 Key 和相同值。
 *
 * 两个 worker 在 Rendezvous 后同时调用 report_count()。Store 的同 Key 更新
 * 必须原子化：一个调用 INSERTED，另一个 UNCHANGED，Publisher 只能收到一次
 * 首次变化，Store 只能包含一个节点。
 */
void test_async_aio_callbacks_same_key_publish_once() {
    constexpr std::size_t callback_count = 2;
    std::array<TestPipe, callback_count> inputs;
    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    const MonData::MonitorKey key{80, 5, 1, 1};
    Rendezvous rendezvous(callback_count);
    std::array<std::atomic<bool>, callback_count> claimed{};
    std::atomic<std::size_t> rendezvous_success{0};
    std::atomic<std::size_t> completed{0};
    std::atomic<std::size_t> inserted{0};
    std::atomic<std::size_t> unchanged{0};
    std::atomic<std::size_t> publish_count{0};

    assert(engine.set_publisher(
        [&](MonData::StoredRecord record) {
            assert(record._data._key == key);
            publish_count.fetch_add(1, std::memory_order_relaxed);
        }));

    for (std::size_t index = 0; index < callback_count; ++index) {
        const auto aio = engine.add_aio(
            inputs[index].read_fd(),
            MonCallback{
                "stage8-concurrent-same-key",
                [&, index]() -> int {
                    if (claimed[index].exchange(
                            true,
                            std::memory_order_acq_rel)) {
                        return 0;
                    }

                    inputs[index].drain_one();

                    if (rendezvous.arrive_and_wait_for(2s)) {
                        rendezvous_success.fetch_add(
                            1,
                            std::memory_order_relaxed);
                    }

                    const auto result = engine.report_count(
                        key,
                        9,
                        "same-key");

                    if (result._status ==
                        MonData::UpdateStatus::INSERTED) {
                        inserted.fetch_add(1, std::memory_order_relaxed);
                    } else if (result._status ==
                               MonData::UpdateStatus::UNCHANGED) {
                        unchanged.fetch_add(1, std::memory_order_relaxed);
                    }

                    if (completed.fetch_add(
                            1,
                            std::memory_order_acq_rel) + 1 ==
                        callback_count) {
                        engine.stop();
                    }

                    return 0;
                },
                true});

        assert(aio.has_value());
    }

    std::promise<ENGINESTATE> run_promise;
    std::future<ENGINESTATE> run_result = run_promise.get_future();
    std::thread runner([&] {
        run_promise.set_value(engine.run());
    });

    assert(wait_for_phase(engine, EnginePhase::RUNNING, 1s));

    for (const TestPipe& input : inputs) {
        input.signal();
    }

    const bool run_completed =
        run_result.wait_for(4s) == std::future_status::ready;
    if (!run_completed) {
        engine.stop();
    }
    runner.join();

    assert(run_completed);
    assert(run_result.get() == ENGINESTATE::SUCCESSFUL);
    assert(rendezvous_success.load(std::memory_order_relaxed) ==
           callback_count);
    assert(inserted.load(std::memory_order_relaxed) == 1);
    assert(unchanged.load(std::memory_order_relaxed) == 1);
    assert(publish_count.load(std::memory_order_relaxed) == 1);
    assert(engine.query_data().size() == 1);
}

} // namespace

int main() {
    test_timer_callback_reports_count();
    test_async_timer_callback_reports_error();
    test_aio_callback_reports_string();
    test_async_aio_callbacks_report_different_keys();
    test_async_aio_callbacks_same_key_publish_once();
    return 0;
}
