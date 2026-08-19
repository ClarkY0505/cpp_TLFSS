#include "engine.h"
#include "monitor_data.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <dirent.h>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include <unistd.h>

using namespace TLSSMON;
/*
 * 固定 Engine 数据接口签名。
 *
 * 默认参数不属于函数类型，因此 ExpectedUpdateData 中仍包含 bool，
 * ExpectedQueryData 中仍包含 MonitorFilter 参数。
 */
using ExpectedUpdateData =
MonData::UpdateResult (Engine::*)(MonData::MonitorData, bool);

using ExpectedFindData = std::optional<MonData::StoredRecord> (Engine::*)(
                                                                          const MonData::MonitorKey &) const;

using ExpectedQueryData = std::vector<MonData::StoredRecord> (Engine::*)(
                                                                         const MonData::MonitorFilter &) const;

static_assert(
              std::is_same_v<decltype(&Engine::update_data), ExpectedUpdateData>);

static_assert(std::is_same_v<decltype(&Engine::find_data), ExpectedFindData>);

static_assert(std::is_same_v<decltype(&Engine::query_data), ExpectedQueryData>);

/**
 * 构造一条数值监控数据。
 */
MonData::MonitorData
make_numeric(MonData::MonitorKey key, std::uint32_t value,
             std::uint32_t state = 2,
             std::string description = "engine-store-test") {
    return MonData::MonitorData{key, std::move(description),
        MonData::NumericValue{value, state}};
}

MonData::MonitorData
make_string(MonData::MonitorKey key, std::string value,
            std::string description = "engine-string-test") {
    return MonData::MonitorData{key, std::move(description), std::move(value)};
}

const MonData::NumericValue &
numeric_value(const MonData::StoredRecord &record) {
    const auto *value = std::get_if<MonData::NumericValue>(&record._data._value);

    assert(value != nullptr);
    return *value;
}

const std::string &string_value(const MonData::StoredRecord &record) {
    const auto *value = std::get_if<std::string>(&record._data._value);

    assert(value != nullptr);
    return *value;
}

class TestPipe final {
public:
    TestPipe() {
        int fds[2]{-1, -1};

        assert(::pipe(fds) == 0);

        _read_fd = fds[0];
        _write_fd = fds[1];
    }

    ~TestPipe() {
        assert(::close(_read_fd) == 0);
        assert(::close(_write_fd) == 0);
    }

    TestPipe(const TestPipe &) = delete;
    TestPipe &operator=(const TestPipe &) = delete;

    int read_fd() const noexcept {
        return _read_fd;
    }

    void signal() const {
        constexpr char value = '*';

        assert(::write(_write_fd, &value, sizeof(value)) ==
               static_cast<ssize_t>(sizeof(value)));
    }

    void drain_one() const {
        char value{};

        assert(::read(_read_fd, &value, sizeof(value)) ==
               static_cast<ssize_t>(sizeof(value)));
    }

private:
    int _read_fd{-1};
    int _write_fd{-1};
};

class StartGate final {
public:
    void wait() {
        std::unique_lock<std::mutex> lock(_mutex);

        _condition.wait(lock, [this] { return _opened; });
    }

    void open() {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _opened = true;
        }

        _condition.notify_all();
    }

private:
    std::mutex _mutex;
    std::condition_variable _condition;
    bool _opened{false};
};

bool wait_for_phase(Engine &engine, EnginePhase expected,
                    std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        if (engine.get_phase() == expected) {
            return true;
        }

        std::this_thread::yield();
    }

    return engine.get_phase() == expected;
}

bool wait_for_count(const std::atomic<std::size_t> &count,
                    std::size_t expected,
                    std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        if (count.load(std::memory_order_acquire) >= expected) {
            return true;
        }

        std::this_thread::yield();
    }

    return count.load(std::memory_order_acquire) >= expected;
}

/**
 * CREATED 状态尚未创建 MonContext。
 *
 * update_data() 必须拒绝写入，并通过 INVALID 明确表示
 * 本次请求不是一次有效的 Store 更新。
 */
void test_created_engine_rejects_update() {
    Engine engine(MonConfig{"monitor", 9000, 1});

    const MonData::MonitorKey key{1, 2, 3, 4};

    const MonData::UpdateResult result =
        engine.update_data(make_numeric(key, 42));

    assert(result._status == MonData::UpdateStatus::INVALID);

    assert(!result._record.has_value());

    /*
     * 被拒绝的写入不能改变 Engine 生命周期。
     */
    assert(engine.get_phase() == EnginePhase::CREATED);
}

/**
 * CREATED 状态没有可读取的 MonContext。
 *
 * find_data() 返回 nullopt；
 * query_data() 返回空快照。
 */
void test_created_engine_returns_empty_reads() {
    Engine engine(MonConfig{"monitor", 9000, 1});

    const Engine &const_engine = engine;
    const MonData::MonitorKey key{1, 2, 3, 4};

    const std::optional<MonData::StoredRecord> found =
        const_engine.find_data(key);

    assert(!found.has_value());

    /*
     * 不传参数，验证 query_data() 提供默认空过滤器。
     */
    const std::vector<MonData::StoredRecord> all_records =
        const_engine.query_data();

    assert(all_records.empty());

    MonData::MonitorFilter filter;
    filter.module_id = 1;

    const std::vector<MonData::StoredRecord> filtered_records =
        const_engine.query_data(filter);

    assert(filtered_records.empty());

    assert(engine.get_phase() == EnginePhase::CREATED);
}

void test_ready_engine_inserts_and_finds_numeric_data() {
    Engine engine(MonConfig{"monitor", 9000, 1});

    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    assert(engine.get_phase() == EnginePhase::READY);

    const MonData::MonitorKey key{10, 2, 30, 40};

    const auto before = std::chrono::system_clock::now();

    const MonData::UpdateResult result =
        engine.update_data(make_numeric(key, 42, 2, "ready-numeric"));

    const auto after = std::chrono::system_clock::now();

    assert(result._status == MonData::UpdateStatus::INSERTED);

    assert(result._record.has_value());
    assert(result.changed());

    assert(result._record->_data._key == key);
    assert(result._record->_data._description == "ready-numeric");

    assert(numeric_value(*result._record)._value == 42);

    assert(numeric_value(*result._record)._state == 2);

    /*
     * 时间戳由 Engine 生成，应位于调用前后时间范围内。
     */
    assert(result._record->_changed_at >= before);

    assert(result._record->_changed_at <= after);

    const auto found = engine.find_data(key);

    assert(found.has_value());
    assert(found->_data._key == key);
    assert(numeric_value(*found)._value == 42);
    assert(found->_changed_at == result._record->_changed_at);
}

void test_ready_engine_preserves_unchanged_numeric_record() {
    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    const MonData::MonitorKey key{11, 2, 30, 40};

    const auto first = engine.update_data(make_numeric(key, 7, 2, "original"));

    assert(first._status == MonData::UpdateStatus::INSERTED);

    assert(first._record.has_value());

    /*
     * value 相同，但 state 和 description 不同。
     * 数值去重只比较 NumericValue::_value。
     */
    const auto second =
        engine.update_data(make_numeric(key, 7, 1, "replacement"));

    assert(second._status == MonData::UpdateStatus::UNCHANGED);

    assert(second._record.has_value());
    assert(!second.changed());

    assert(second._record->_data._description == "original");

    assert(numeric_value(*second._record)._state == 2);

    assert(second._record->_changed_at == first._record->_changed_at);

    const auto stored = engine.find_data(key);

    assert(stored.has_value());
    assert(stored->_data._description == "original");
    assert(numeric_value(*stored)._value == 7);
    assert(numeric_value(*stored)._state == 2);
}

void test_ready_engine_updates_changed_numeric_data() {
    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    const MonData::MonitorKey key{12, 2, 30, 40};

    const auto first =
        engine.update_data(make_numeric(key, 7, 2, "before-update"));

    assert(first._status == MonData::UpdateStatus::INSERTED);

    const auto before_update = std::chrono::system_clock::now();

    const auto second =
        engine.update_data(make_numeric(key, 8, 1, "after-update"));

    const auto after_update = std::chrono::system_clock::now();

    assert(second._status == MonData::UpdateStatus::UPDATED);

    assert(second._record.has_value());
    assert(second.changed());

    assert(numeric_value(*second._record)._value == 8);

    assert(numeric_value(*second._record)._state == 1);

    assert(second._record->_data._description == "after-update");

    assert(second._record->_changed_at >= before_update);

    assert(second._record->_changed_at <= after_update);

    const auto stored = engine.find_data(key);

    assert(stored.has_value());
    assert(numeric_value(*stored)._value == 8);
}

void test_ready_engine_applies_initial_zero_gate() {
    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    const MonData::MonitorKey key{13, 2, 30, 40};

    const auto ignored = engine.update_data(make_numeric(key, 0), false);

    assert(ignored._status == MonData::UpdateStatus::IGNORED_INITIAL_ZERO);

    assert(!ignored._record.has_value());
    assert(!engine.find_data(key).has_value());

    const auto forced = engine.update_data(make_numeric(key, 0), true);

    assert(forced._status == MonData::UpdateStatus::INSERTED);

    assert(forced._record.has_value());

    const auto stored = engine.find_data(key);

    assert(stored.has_value());
    assert(numeric_value(*stored)._value == 0);
}

void test_ready_engine_accepts_strings() {
    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    const MonData::MonitorKey empty_key{14, 2, 30, 40};
    const MonData::MonitorKey text_key{14, 2, 31, 40};

    const auto empty_result =
        engine.update_data(make_string(empty_key, "", "empty-string"));

    assert(empty_result._status == MonData::UpdateStatus::INSERTED);

    assert(empty_result._record.has_value());
    assert(string_value(*empty_result._record).empty());

    const auto text_result =
        engine.update_data(make_string(text_key, "sensor-ok", "normal-string"));

    assert(text_result._status == MonData::UpdateStatus::INSERTED);

    assert(text_result._record.has_value());

    assert(string_value(*text_result._record) == "sensor-ok");

    const auto empty_stored = engine.find_data(empty_key);

    const auto text_stored = engine.find_data(text_key);

    assert(empty_stored.has_value());
    assert(text_stored.has_value());
    assert(string_value(*empty_stored).empty());
    assert(string_value(*text_stored) == "sensor-ok");
}
void test_ready_engine_forwards_query_filter() {
    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    const MonData::MonitorKey first_key{20, 1, 100, 1000};

    const MonData::MonitorKey expected_key{20, 2, 200, 2000};

    const MonData::MonitorKey third_key{21, 2, 200, 2000};

    assert(engine.update_data(make_numeric(first_key, 1)).changed());

    assert(engine.update_data(make_numeric(expected_key, 2)).changed());

    assert(engine.update_data(make_numeric(third_key, 3)).changed());

    /*
     * 默认空过滤器返回全部记录。
     */
    const auto all_records = engine.query_data();

    assert(all_records.size() == 3);

    MonData::MonitorFilter filter;
    filter.module_id = 20;
    filter.level = 2;
    filter.function_id = 200;
    filter.event_id = 2000;

    const auto filtered = engine.query_data(filter);

    assert(filtered.size() == 1);
    assert(filtered[0]._data._key == expected_key);
    assert(numeric_value(filtered[0])._value == 2);
}

void test_timer_callback_updates_engine_store() {
    using namespace std::chrono_literals;

    Engine engine(MonConfig{"monitor", 9000, 1});

    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    assert(engine.get_phase() == EnginePhase::READY);

    const MonData::MonitorKey key{30, 3, 300, 3000};

    /*
     * Timer 回调通过 promise 把 update_data() 的结果
     * 传回测试线程。
     */
    std::promise<MonData::UpdateResult> update_promise;

    std::future<MonData::UpdateResult> update_future =
        update_promise.get_future();

    std::atomic<std::size_t> callback_count{0};

    const std::optional<TimerHandle> timer_handle = engine.set_timer(
                                                                     MonCallback{"monitor-store-timer",
                                                                     [&]() -> int {
                                                                     callback_count.fetch_add(1, std::memory_order_relaxed);

                                                                     /*
                                                                      * 此时回调由 Engine::run() 触发，
                                                                      * Engine 应处于 RUNNING。
                                                                      */
                                                                     const MonData::UpdateResult result = engine.update_data(
                                                                                                                             make_numeric(key, 88, 2, "timer-record"));

                                                                     /*
                                                                      * 先完成写入，再请求停止。
                                                                      *
                                                                      * 如果先 stop()，Engine 会进入 STOPPING，
                                                                      * update_data() 将按状态规则拒绝写入。
                                                                      */
                                                                     update_promise.set_value(result);

                                                                     engine.stop();

                                                                     return 0;
                                                                     },
                                                                     false},
        TimerFlags::ONCE, 20ms);

    assert(timer_handle.has_value());
    assert(static_cast<bool>(*timer_handle));

    /*
     * Engine::run() 是阻塞接口，因此放到独立线程。
     */
    std::promise<ENGINESTATE> run_promise;

    std::future<ENGINESTATE> run_future = run_promise.get_future();

    std::thread runner([&]() { run_promise.set_value(engine.run()); });

    /*
     * Timer 必须在限定时间内触发。
     */
    const bool update_completed =
        update_future.wait_for(1s) == std::future_status::ready;

    if (!update_completed) {
        /*
         * 测试失败时先请求停止，避免 runner 永久阻塞。
         */
        engine.stop();
    }

    const bool run_completed =
        run_future.wait_for(1s) == std::future_status::ready;

    if (!run_completed) {
        engine.stop();
    }

    /*
     * 所有 Engine 相关断言前先回收 runner，
     * 避免测试退出时仍存在可连接线程。
     */
    runner.join();

    assert(update_completed);
    assert(run_completed);

    const MonData::UpdateResult update_result = update_future.get();

    const ENGINESTATE run_result = run_future.get();

    assert(update_result._status == MonData::UpdateStatus::INSERTED);

    assert(update_result._record.has_value());
    assert(update_result.changed());

    assert(update_result._record->_data._key == key);

    assert(update_result._record->_data._description == "timer-record");

    assert(numeric_value(*update_result._record)._value == 88);

    assert(numeric_value(*update_result._record)._state == 2);

    assert(callback_count.load(std::memory_order_relaxed) == 1);

    assert(run_result == ENGINESTATE::SUCCESSFUL);

    assert(engine.get_phase() == EnginePhase::STOPPED);

    /*
     * Engine 停止时没有清空 MonitorStore，
     * 因此 STOPPED 状态仍然能够读取 Timer 写入的数据。
     */
    const auto stored = engine.find_data(key);

    assert(stored.has_value());

    assert(stored->_data._description == "timer-record");

    assert(numeric_value(*stored)._value == 88);

    const auto snapshot = engine.query_data();

    assert(snapshot.size() == 1);
    assert(snapshot[0]._data._key == key);
}

constexpr std::size_t aio_callback_count = 8;

struct AioScenarioResult final {
    std::array<MonData::UpdateStatus, aio_callback_count> _statuses;
    std::vector<MonData::StoredRecord> _snapshot;
};

AioScenarioResult run_async_aio_update_scenario(bool use_same_key) {
    using namespace std::chrono_literals;

    /*
     * inputs 比 engine 先构造，所以函数退出时 Engine 会先析构并移除
     * AIO 注册，随后才关闭测试 pipe。
     */
    std::vector<std::unique_ptr<TestPipe>> inputs;
    inputs.reserve(aio_callback_count);

    for (std::size_t index = 0; index < aio_callback_count; ++index) {
        inputs.push_back(std::make_unique<TestPipe>());
    }

    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    std::array<MonData::UpdateStatus, aio_callback_count> statuses;
    statuses.fill(MonData::UpdateStatus::INVALID);

    StartGate gate;
    std::atomic<std::size_t> ready_count{0};
    std::atomic<std::size_t> finished_count{0};
    std::array<std::atomic<bool>, aio_callback_count> callback_started;

    for (auto &started : callback_started) {
        started.store(false, std::memory_order_relaxed);
    }

    for (std::size_t index = 0; index < aio_callback_count; ++index) {
        const auto handle = engine.add_aio(
            inputs[index]->read_fd(),
            MonCallback{
                "monitor-store-async-aio",
                [&, index]() -> int {
                    /*
                     * select() 是电平触发的。异步 worker 消费 pipe 字节前，
                     * reactor 可能再次激活同一个 AIO。每个注册项只处理
                     * 第一次激活，避免重复进入后再次阻塞读取同一个 pipe。
                     */
                    if (callback_started[index].exchange(
                            true, std::memory_order_acq_rel)) {
                        return 0;
                    }

                    inputs[index]->drain_one();

                    ready_count.fetch_add(1, std::memory_order_release);

                    /*
                     * 全部异步 AIO worker 到齐后，由测试线程统一放行。
                     */
                    gate.wait();

                    const std::uint32_t sequence =
                        static_cast<std::uint32_t>(index + 1);

                    const MonData::MonitorKey key =
                        use_same_key
                            ? MonData::MonitorKey{41, 4, 1, 1}
                            : MonData::MonitorKey{40, 4, sequence, sequence};

                    const std::uint32_t value = use_same_key ? 99 : sequence;

                    const MonData::UpdateResult result = engine.update_data(
                        make_numeric(key, value, 2,
                                     use_same_key ? "aio-same-key"
                                                  : "aio-distinct-key"));

                    /*
                     * 每个 worker 只写自己的数组位置。主线程只在所有
                     * worker 结束后读取该数组。
                     */
                    statuses[index] = result._status;

                    finished_count.fetch_add(1, std::memory_order_release);

                    return 0;
                },
                true});

        assert(handle.has_value());
    }

    std::promise<ENGINESTATE> run_promise;
    std::future<ENGINESTATE> run_future = run_promise.get_future();

    std::thread runner([&] { run_promise.set_value(engine.run()); });

    assert(wait_for_phase(engine, EnginePhase::RUNNING, 1s));

    for (const auto &input : inputs) {
        input->signal();
    }

    const bool all_ready = wait_for_count(ready_count, aio_callback_count, 2s);

    /*
     * 即使等待失败也要打开启动门，避免已经到达的 worker 永久阻塞。
     */
    gate.open();

    const bool all_finished =
        wait_for_count(finished_count, aio_callback_count, 2s);

    engine.stop();

    const bool run_completed =
        run_future.wait_for(2s) == std::future_status::ready;

    if (!run_completed) {
        engine.stop();
    }

    runner.join();

    assert(all_ready);
    assert(all_finished);
    assert(run_completed);
    assert(run_future.get() == ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::STOPPED);

    return AioScenarioResult{statuses, engine.query_data()};
}

void test_async_aio_callbacks_update_distinct_keys() {
    const AioScenarioResult result =
        run_async_aio_update_scenario(false);

    for (const MonData::UpdateStatus status : result._statuses) {
        assert(status == MonData::UpdateStatus::INSERTED);
    }

    assert(result._snapshot.size() == aio_callback_count);

    /*
     * Store 快照按照 mid -> level -> fid -> eid 排序。
     */
    for (std::size_t index = 0; index < aio_callback_count; ++index) {
        const std::uint32_t sequence =
            static_cast<std::uint32_t>(index + 1);

        const MonData::MonitorKey expected_key{40, 4, sequence, sequence};

        assert(result._snapshot[index]._data._key == expected_key);
        assert(numeric_value(result._snapshot[index])._value == sequence);
    }
}

void test_async_aio_callbacks_deduplicate_same_key() {
    const AioScenarioResult result =
        run_async_aio_update_scenario(true);

    const std::size_t inserted_count = static_cast<std::size_t>(
        std::count(result._statuses.begin(), result._statuses.end(),
                   MonData::UpdateStatus::INSERTED));

    const std::size_t unchanged_count = static_cast<std::size_t>(
        std::count(result._statuses.begin(), result._statuses.end(),
                   MonData::UpdateStatus::UNCHANGED));

    assert(inserted_count == 1);
    assert(unchanged_count == aio_callback_count - 1);
    assert(result._snapshot.size() == 1);

    const MonData::MonitorKey expected_key{41, 4, 1, 1};

    assert(result._snapshot[0]._data._key == expected_key);
    assert(numeric_value(result._snapshot[0])._value == 99);
}

void test_stopping_and_stopped_engine_preserve_store() {
    using namespace std::chrono_literals;

    /*
     * TestPipe 比 Engine 先构造，保证 Engine 先移除 AIO，随后才关闭
     * 用户持有的文件描述符。
     */
    TestPipe input;

    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    const MonData::MonitorKey key{50, 5, 500, 5000};

    const MonData::UpdateResult inserted = engine.update_data(
        make_numeric(key, 55, 2, "retained-after-stop"));

    assert(inserted._status == MonData::UpdateStatus::INSERTED);
    assert(inserted._record.has_value());

    const MonData::MonitorTimestamp original_timestamp =
        inserted._record->_changed_at;

    StartGate release_callback;
    std::promise<void> callback_entered_promise;
    std::future<void> callback_entered_future =
        callback_entered_promise.get_future();
    std::atomic<bool> callback_claimed{false};

    const auto aio_handle = engine.add_aio(
        input.read_fd(),
        MonCallback{
            "hold-engine-stopping",
            [&]() -> int {
                /*
                 * 电平触发可能重复激活，只处理第一次。
                 */
                if (callback_claimed.exchange(true,
                                              std::memory_order_acq_rel)) {
                    return 0;
                }

                input.drain_one();
                callback_entered_promise.set_value();

                /*
                 * run() 停止 worker 时会等待本回调返回，使 Engine 稳定
                 * 保持在 STOPPING，直到测试线程放行。
                 */
                release_callback.wait();

                return 0;
            },
            true});

    assert(aio_handle.has_value());

    std::promise<ENGINESTATE> run_promise;
    std::future<ENGINESTATE> run_future = run_promise.get_future();

    std::thread runner([&] { run_promise.set_value(engine.run()); });

    assert(wait_for_phase(engine, EnginePhase::RUNNING, 1s));

    input.signal();

    const bool callback_entered =
        callback_entered_future.wait_for(1s) == std::future_status::ready;

    if (!callback_entered) {
        release_callback.open();
        engine.stop();
        runner.join();

        assert(callback_entered);
    }

    engine.stop();

    /*
     * 异步回调仍被启动门阻塞，因此 run() 尚不能完成清理。
     */
    assert(engine.get_phase() == EnginePhase::STOPPING);

    const auto stopping_record = engine.find_data(key);

    assert(stopping_record.has_value());
    assert(numeric_value(*stopping_record)._value == 55);
    assert(stopping_record->_data._description == "retained-after-stop");
    assert(stopping_record->_changed_at == original_timestamp);

    const auto stopping_snapshot = engine.query_data();

    assert(stopping_snapshot.size() == 1);
    assert(stopping_snapshot[0]._data._key == key);

    const MonData::UpdateResult stopping_update = engine.update_data(
        make_numeric(key, 999, 1, "must-not-be-written"));

    assert(stopping_update._status == MonData::UpdateStatus::INVALID);
    assert(!stopping_update._record.has_value());

    const auto after_rejected_update = engine.find_data(key);

    assert(after_rejected_update.has_value());
    assert(numeric_value(*after_rejected_update)._value == 55);
    assert(after_rejected_update->_data._description ==
           "retained-after-stop");
    assert(after_rejected_update->_changed_at == original_timestamp);

    release_callback.open();

    const bool run_completed =
        run_future.wait_for(2s) == std::future_status::ready;

    if (!run_completed) {
        engine.stop();
    }

    runner.join();

    assert(run_completed);
    assert(run_future.get() == ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::STOPPED);

    const auto stopped_record = engine.find_data(key);

    assert(stopped_record.has_value());
    assert(numeric_value(*stopped_record)._value == 55);
    assert(stopped_record->_data._description == "retained-after-stop");
    assert(stopped_record->_changed_at == original_timestamp);

    const auto stopped_snapshot = engine.query_data();

    assert(stopped_snapshot.size() == 1);
    assert(stopped_snapshot[0]._data._key == key);

    const MonData::UpdateResult stopped_update =
        engine.update_data(make_numeric(key, 1000));

    assert(stopped_update._status == MonData::UpdateStatus::INVALID);
    assert(!stopped_update._record.has_value());

    const auto final_record = engine.find_data(key);

    assert(final_record.has_value());
    assert(numeric_value(*final_record)._value == 55);
}

void test_snapshots_survive_engine_destruction() {
    const MonData::MonitorKey key{60, 6, 600, 6000};

    std::optional<MonData::StoredRecord> retained_record;
    std::vector<MonData::StoredRecord> retained_snapshot;

    {
        Engine engine(MonConfig{"monitor", 9000, 1});

        assert(engine.init() == ENGINESTATE::SUCCESSFUL);

        const auto inserted = engine.update_data(
            make_string(key, "final-state", "retained-snapshot"));

        assert(inserted._status == MonData::UpdateStatus::INSERTED);

        retained_record = engine.find_data(key);
        retained_snapshot = engine.query_data();

        assert(retained_record.has_value());
        assert(retained_snapshot.size() == 1);
    }

    /*
     * Engine 和 MonContext 已经析构，值语义快照仍然有效。
     */
    assert(retained_record.has_value());
    assert(retained_record->_data._key == key);
    assert(retained_record->_data._description == "retained-snapshot");
    assert(string_value(*retained_record) == "final-state");

    assert(retained_snapshot.size() == 1);
    assert(retained_snapshot[0]._data._key == key);
    assert(string_value(retained_snapshot[0]) == "final-state");
}

std::size_t count_open_file_descriptors() {
    DIR *const directory = ::opendir("/proc/self/fd");

    assert(directory != nullptr);

    std::size_t count = 0;

    while (dirent *const entry = ::readdir(directory)) {
        const std::string_view name{entry->d_name};

        if (name != "." && name != "..") {
            ++count;
        }
    }

    assert(::closedir(directory) == 0);

    return count;
}

void test_engine_destruction_releases_resources() {
    const std::size_t descriptors_before = count_open_file_descriptors();

    /*
     * 重复构造和析构，放大 pipe 或 AIO 文件描述符泄漏。
     */
    for (std::size_t iteration = 0; iteration < 32; ++iteration) {
        Engine engine(MonConfig{"monitor", 9000, 1});

        assert(engine.init() == ENGINESTATE::SUCCESSFUL);

        const std::uint32_t sequence =
            static_cast<std::uint32_t>(iteration + 1);

        const MonData::MonitorKey key{70, 7, sequence, sequence};

        const auto result =
            engine.update_data(make_numeric(key, sequence));

        assert(result._status == MonData::UpdateStatus::INSERTED);
    }

    const std::size_t descriptors_after = count_open_file_descriptors();

    assert(descriptors_after == descriptors_before);
}

int main() {
    test_created_engine_rejects_update();
    test_created_engine_returns_empty_reads();

    test_ready_engine_inserts_and_finds_numeric_data();
    test_ready_engine_preserves_unchanged_numeric_record();
    test_ready_engine_updates_changed_numeric_data();
    test_ready_engine_applies_initial_zero_gate();
    test_ready_engine_accepts_strings();
    test_ready_engine_forwards_query_filter();

    test_timer_callback_updates_engine_store();

    test_async_aio_callbacks_update_distinct_keys();
    test_async_aio_callbacks_deduplicate_same_key();

    test_stopping_and_stopped_engine_preserve_store();
    test_snapshots_survive_engine_destruction();
    test_engine_destruction_releases_resources();

    return 0;
}
