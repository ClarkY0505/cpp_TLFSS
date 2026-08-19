#include "engine.h"
#include "monitor_data.h"
#include "monitor_reporter.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

using namespace TLSSMON;
using namespace std::chrono_literals;

namespace {

MonData::MonitorData make_numeric(
    MonData::MonitorKey key,
    std::uint32_t value,
    std::uint32_t state = 2,
    std::string description = {}) {

    return MonData::MonitorData{
        std::move(key),
        std::move(description),
        MonData::NumericValue{value, state}};
}

const MonData::NumericValue& numeric_value(
    const MonData::StoredRecord& record) {

    return std::get<MonData::NumericValue>(
        record._data._value);
}

const std::string& string_value(
    const MonData::StoredRecord& record) {

    return std::get<std::string>(
        record._data._value);
}

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

/*
 * 用于稳定构造 STOPPING 状态的测试管道。
 *
 * Engine 不拥有用户注册的 fd，因此测试负责创建、写入、读取和关闭管道。
 * TestPipe 必须在 Engine 之前构造，使 Engine 在析构时先移除 AIO 注册，随后
 * TestPipe 才关闭 fd。
 */
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

        assert(::write(
                   _write_fd,
                   &byte,
                   sizeof(byte)) ==
               static_cast<ssize_t>(sizeof(byte)));
    }

    void drain_one() const {
        char byte{};

        assert(::read(
                   _read_fd,
                   &byte,
                   sizeof(byte)) ==
               static_cast<ssize_t>(sizeof(byte)));
    }

private:
    int _read_fd{-1};
    int _write_fd{-1};
};

/*
 * 可重复等待的单向启动门。
 *
 * 异步 AIO 回调进入后在 wait() 中阻塞；测试线程调用 open() 后放行。这样
 * run() 会停在等待异步 worker 退出的位置，使 Engine 稳定保持 STOPPING。
 */
class StartGate final {
public:
    void wait() {
        std::unique_lock<std::mutex> lock(_mutex);

        _condition.wait(
            lock,
            [&] {
                return _opened;
            });
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

/*
 * 案例 1：锁定 Engine 的 M5 Reporter 公共接口。
 *
 * 测试目的：
 * 使用编译期断言固定 Publisher 别名、成员函数参数和返回类型，防止后续实现
 * 在不知情的情况下破坏调用方接口。
 *
 * 默认参数不属于成员函数指针类型，因此额外使用 decltype() 验证三个
 * report_*() 接口都允许省略 description。
 */
using ExpectedEnginePublisher = MonitorReporter::Publisher;

static_assert(
    std::is_same_v<
        Engine::MonitorPublisher,
        ExpectedEnginePublisher>);

using ExpectedSetPublisher =
    bool (Engine::*)(Engine::MonitorPublisher);

static_assert(
    std::is_same_v<
        decltype(&Engine::set_publisher),
        ExpectedSetPublisher>);

using ExpectedNumericReport =
    MonData::UpdateResult (Engine::*)(
        MonData::MonitorKey,
        std::uint32_t,
        std::string);

static_assert(
    std::is_same_v<
        decltype(&Engine::report_count),
        ExpectedNumericReport>);

static_assert(
    std::is_same_v<
        decltype(&Engine::report_error),
        ExpectedNumericReport>);

using ExpectedStringReport =
    MonData::UpdateResult (Engine::*)(
        MonData::MonitorKey,
        std::string,
        std::string);

static_assert(
    std::is_same_v<
        decltype(&Engine::report_string),
        ExpectedStringReport>);

using DefaultCountResult = decltype(
    std::declval<Engine&>().report_count(
        std::declval<MonData::MonitorKey>(),
        std::declval<std::uint32_t>()));

using DefaultErrorResult = decltype(
    std::declval<Engine&>().report_error(
        std::declval<MonData::MonitorKey>(),
        std::declval<std::uint32_t>()));

using DefaultStringResult = decltype(
    std::declval<Engine&>().report_string(
        std::declval<MonData::MonitorKey>(),
        std::declval<std::string>()));

static_assert(
    std::is_same_v<
        DefaultCountResult,
        MonData::UpdateResult>);

static_assert(
    std::is_same_v<
        DefaultErrorResult,
        MonData::UpdateResult>);

static_assert(
    std::is_same_v<
        DefaultStringResult,
        MonData::UpdateResult>);

/*
 * 案例 2：CREATED 状态拒绝 Publisher 注册和全部写入入口。
 *
 * 执行过程：
 * 1. 构造 Engine，但不调用 init()；
 * 2. 尝试注册 Publisher；
 * 3. 分别调用 report_count、report_error、report_string 和 update_data；
 * 4. 检查所有请求都被拒绝，Publisher 没有执行，生命周期保持 CREATED。
 *
 * 通过标准：
 * set_publisher() 返回 false，四个写入结果均为 INVALID 且不携带记录。
 */
void test_created_engine_rejects_publisher_and_reports() {
    Engine engine(MonConfig{"monitor", 9000, 1});

    std::atomic<std::size_t> publish_count{0};

    const bool publisher_set = engine.set_publisher(
        [&](MonData::StoredRecord) {
            publish_count.fetch_add(
                1,
                std::memory_order_relaxed);
        });

    assert(!publisher_set);

    const auto count_result =
        engine.report_count({1, 1, 1, 1}, 10);

    const auto error_result =
        engine.report_error({1, 1, 1, 2}, 0);

    const auto string_result =
        engine.report_string({1, 1, 1, 3}, "value");

    const auto update_result = engine.update_data(
        make_numeric({1, 1, 1, 4}, 20));

    assert(count_result._status ==
           MonData::UpdateStatus::INVALID);
    assert(error_result._status ==
           MonData::UpdateStatus::INVALID);
    assert(string_result._status ==
           MonData::UpdateStatus::INVALID);
    assert(update_result._status ==
           MonData::UpdateStatus::INVALID);

    assert(!count_result._record.has_value());
    assert(!error_result._record.has_value());
    assert(!string_result._record.has_value());
    assert(!update_result._record.has_value());

    assert(publish_count.load(
               std::memory_order_relaxed) == 0);

    assert(engine.get_phase() == EnginePhase::CREATED);
}

/*
 * 案例 3：READY 状态可以注册 Publisher 并上报普通计数。
 *
 * 执行过程：
 * 1. init() 使 Engine 进入 READY；
 * 2. 注册一个保存 StoredRecord 副本的 Publisher；
 * 3. 省略 description 调用 report_count()；
 * 4. 同时检查 UpdateResult、Publisher 副本和 Store 查询结果。
 *
 * 通过标准：
 * 返回 INSERTED，state 固定为 0，默认 description 为空，时间戳位于调用前后
 * 的 system_clock 区间内，Publisher 和 Store 中都存在相同记录。
 */
void test_ready_engine_registers_publisher_and_reports_count() {
    Engine engine(MonConfig{"monitor", 9000, 1});

    assert(engine.init() == ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::READY);

    std::vector<MonData::StoredRecord> published;

    assert(engine.set_publisher(
        [&](MonData::StoredRecord record) {
            published.push_back(std::move(record));
        }));

    const MonData::MonitorKey key{2, 3, 4, 5};
    const auto before = std::chrono::system_clock::now();

    const auto result = engine.report_count(key, 10);

    const auto after = std::chrono::system_clock::now();

    assert(result._status == MonData::UpdateStatus::INSERTED);
    assert(result._record.has_value());
    assert(result._record->_data._key == key);
    assert(result._record->_data._description.empty());
    assert(numeric_value(*result._record)._value == 10);
    assert(numeric_value(*result._record)._state == 0);
    assert(result._record->_changed_at >= before);
    assert(result._record->_changed_at <= after);

    assert(published.size() == 1);
    assert(published[0]._data._key == key);
    assert(numeric_value(published[0])._value == 10);

    const auto stored = engine.find_data(key);
    assert(stored.has_value());
    assert(numeric_value(*stored)._value == 10);
}

/*
 * 案例 4：READY 状态正确转发错误和字符串上报。
 *
 * 测试目的：
 * 确认 Engine 不复制 Reporter 的业务规则，只负责生命周期检查和转发。首次
 * 错误零值必须由 Reporter 使用 force=true 插入；空字符串必须作为合法值保存。
 *
 * 通过标准：
 * 错误零值和空字符串都返回 INSERTED，错误 state 为 2，调用方 description
 * 原样保存，Publisher 收到两条记录。
 */
void test_ready_engine_forwards_error_and_string_reports() {
    Engine engine(MonConfig{"monitor", 9000, 1});

    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    std::vector<MonData::StoredRecord> published;

    assert(engine.set_publisher(
        [&](MonData::StoredRecord record) {
            published.push_back(std::move(record));
        }));

    const auto error_result = engine.report_error(
        {3, 7, 23, 45},
        0,
        "zero-error");

    assert(error_result._status ==
           MonData::UpdateStatus::INSERTED);
    assert(error_result._record.has_value());
    assert(numeric_value(*error_result._record)._value == 0);
    assert(numeric_value(*error_result._record)._state == 2);
    assert(error_result._record->_data._description ==
           "zero-error");

    const auto string_result = engine.report_string(
        {3, 8, 24, 46},
        "",
        "empty-string");

    assert(string_result._status ==
           MonData::UpdateStatus::INSERTED);
    assert(string_result._record.has_value());
    assert(string_value(*string_result._record).empty());
    assert(string_result._record->_data._description ==
           "empty-string");

    assert(published.size() == 2);
    assert(numeric_value(published[0])._state == 2);
    assert(string_value(published[1]).empty());
}

/*
 * 案例 5：旧 update_data() 接口必须经过 Reporter 发布门。
 *
 * 执行过程：
 * 1. 通过 update_data() 首次插入数值；
 * 2. 提交相同数值但不同 state 和 description；
 * 3. 再提交一个不同数值；
 * 4. 对照 Store 状态和 Publisher 次数。
 *
 * 通过标准：
 * 状态依次为 INSERTED、UNCHANGED、UPDATED，Publisher 只处理首次插入和实际
 * 数值变化。如果 update_data() 仍直接调用 Store，本案例的发布次数会失败。
 */
void test_update_data_uses_reporter_publish_gate() {
    Engine engine(MonConfig{"monitor", 9000, 1});

    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    std::vector<MonData::StoredRecord> published;

    assert(engine.set_publisher(
        [&](MonData::StoredRecord record) {
            published.push_back(std::move(record));
        }));

    const MonData::MonitorKey key{4, 4, 4, 4};

    const auto inserted = engine.update_data(
        make_numeric(key, 10, 2, "inserted"));

    assert(inserted._status ==
           MonData::UpdateStatus::INSERTED);
    assert(published.size() == 1);

    const auto unchanged = engine.update_data(
        make_numeric(key, 10, 0, "duplicate"));

    assert(unchanged._status ==
           MonData::UpdateStatus::UNCHANGED);
    assert(published.size() == 1);

    const auto updated = engine.update_data(
        make_numeric(key, 20, 1, "updated"));

    assert(updated._status ==
           MonData::UpdateStatus::UPDATED);
    assert(published.size() == 2);
    assert(numeric_value(published[0])._value == 10);
    assert(numeric_value(published[1])._value == 20);
}

/*
 * 案例 6：READY 状态可以通过空 Publisher 注销发布端。
 *
 * 测试目的：
 * 验证 Engine::set_publisher({}) 会转发给 Reporter，并且注销后只停止发布，
 * 不停止 Store 更新。
 *
 * 通过标准：
 * 首次插入后发布次数为 1；注销后的数值变化返回 UPDATED，但发布次数保持 1，
 * Store 保存最新值。
 */
void test_ready_engine_can_unregister_publisher() {
    Engine engine(MonConfig{"monitor", 9000, 1});

    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    std::atomic<std::size_t> publish_count{0};

    assert(engine.set_publisher(
        [&](MonData::StoredRecord) {
            publish_count.fetch_add(
                1,
                std::memory_order_relaxed);
        }));

    const MonData::MonitorKey key{5, 5, 5, 5};

    const auto inserted =
        engine.report_count(key, 1, "first");

    assert(inserted._status ==
           MonData::UpdateStatus::INSERTED);
    assert(publish_count.load(
               std::memory_order_relaxed) == 1);

    assert(engine.set_publisher({}));

    const auto updated =
        engine.report_count(key, 2, "after-unregister");

    assert(updated._status ==
           MonData::UpdateStatus::UPDATED);
    assert(publish_count.load(
               std::memory_order_relaxed) == 1);

    const auto stored = engine.find_data(key);
    assert(stored.has_value());
    assert(numeric_value(*stored)._value == 2);
}

/*
 * 案例 7：RUNNING 状态允许注册 Publisher 和上报。
 *
 * 执行过程：
 * 1. 初始化 Engine 并在 runner 线程执行 run()；
 * 2. 等待 phase 进入 RUNNING；
 * 3. 注册 Publisher 并上报字符串；
 * 4. stop()、等待 runner 退出，再从 STOPPED Engine 读取记录。
 *
 * 通过标准：
 * RUNNING 时 set_publisher() 返回 true，上报返回 INSERTED 并发布一次；停止后
 * 最后记录仍然可读取。
 */
void test_running_engine_accepts_publisher_and_reports() {
    Engine engine(MonConfig{"monitor", 9000, 1});

    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    std::promise<ENGINESTATE> run_promise;
    std::future<ENGINESTATE> run_future =
        run_promise.get_future();

    std::thread runner([&] {
        run_promise.set_value(engine.run());
    });

    assert(wait_for_phase(
        engine,
        EnginePhase::RUNNING,
        1s));

    std::atomic<std::size_t> publish_count{0};

    assert(engine.set_publisher(
        [&](MonData::StoredRecord) {
            publish_count.fetch_add(
                1,
                std::memory_order_relaxed);
        }));

    const MonData::MonitorKey key{6, 6, 6, 6};

    const auto result = engine.report_string(
        key,
        "running",
        "running-report");

    assert(result._status ==
           MonData::UpdateStatus::INSERTED);
    assert(publish_count.load(
               std::memory_order_relaxed) == 1);

    engine.stop();

    const bool completed =
        run_future.wait_for(2s) ==
        std::future_status::ready;

    if (!completed) {
        engine.stop();
    }

    runner.join();

    assert(completed);
    assert(run_future.get() ==
           ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::STOPPED);

    const auto stored = engine.find_data(key);
    assert(stored.has_value());
    assert(string_value(*stored) == "running");
}

/*
 * 案例 8：STOPPING 状态拒绝 Publisher 注册和全部写入入口。
 *
 * 执行过程：
 * 1. READY 时注册 Publisher 并保存一条基准记录；
 * 2. 注册一个异步 AIO 回调，让回调进入 StartGate 后阻塞；
 * 3. run() 后触发 AIO，再调用 stop()；run() 会等待该 worker，Engine 因此
 *    稳定停留在 STOPPING；
 * 4. 在 STOPPING 中尝试替换 Publisher 和调用四个写入入口；
 * 5. 检查请求全部拒绝、发布次数不变、基准记录仍可读取；
 * 6. 打开 StartGate，回收 runner。
 *
 * 通过标准：
 * set_publisher() 返回 false，四个写入均返回 INVALID，Store 未被清空或覆盖。
 */
void test_stopping_engine_rejects_publisher_and_reports() {
    TestPipe input;
    Engine engine(MonConfig{"monitor", 9000, 1});

    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    std::atomic<std::size_t> publish_count{0};

    assert(engine.set_publisher(
        [&](MonData::StoredRecord) {
            publish_count.fetch_add(
                1,
                std::memory_order_relaxed);
        }));

    const MonData::MonitorKey retained_key{7, 7, 7, 7};

    const auto inserted = engine.report_count(
        retained_key,
        10,
        "retained");

    assert(inserted._status ==
           MonData::UpdateStatus::INSERTED);
    assert(publish_count.load(
               std::memory_order_relaxed) == 1);

    StartGate callback_gate;
    std::promise<void> callback_entered_promise;
    std::future<void> callback_entered =
        callback_entered_promise.get_future();
    std::atomic<bool> callback_claimed{false};

    const auto handle = engine.add_aio(
        input.read_fd(),
        MonCallback{
            "hold-stopping",
            [&]() -> int {
                /*
                 * select() 是电平触发的。只允许第一个激活进入阻塞读取和启动门，
                 * 避免同一 fd 被重复调度后再次读取空管道。
                 */
                if (callback_claimed.exchange(
                        true,
                        std::memory_order_acq_rel)) {
                    return 0;
                }

                input.drain_one();
                callback_entered_promise.set_value();
                callback_gate.wait();
                return 0;
            },
            true});

    assert(handle.has_value());

    std::promise<ENGINESTATE> run_promise;
    std::future<ENGINESTATE> run_future =
        run_promise.get_future();

    std::thread runner([&] {
        run_promise.set_value(engine.run());
    });

    assert(wait_for_phase(
        engine,
        EnginePhase::RUNNING,
        1s));

    input.signal();

    const bool callback_started =
        callback_entered.wait_for(1s) ==
        std::future_status::ready;

    if (!callback_started) {
        callback_gate.open();
        engine.stop();
        runner.join();
        assert(callback_started);
    }

    engine.stop();

    assert(engine.get_phase() == EnginePhase::STOPPING);

    assert(!engine.set_publisher(
        [](MonData::StoredRecord) {}));

    assert(engine.report_count(
               {7, 7, 7, 8},
               1)._status ==
           MonData::UpdateStatus::INVALID);

    assert(engine.report_error(
               {7, 7, 7, 9},
               1)._status ==
           MonData::UpdateStatus::INVALID);

    assert(engine.report_string(
               {7, 7, 7, 10},
               "rejected")._status ==
           MonData::UpdateStatus::INVALID);

    assert(engine.update_data(
               make_numeric(
                   {7, 7, 7, 11},
                   1))._status ==
           MonData::UpdateStatus::INVALID);

    assert(publish_count.load(
               std::memory_order_relaxed) == 1);

    const auto retained = engine.find_data(retained_key);
    assert(retained.has_value());
    assert(numeric_value(*retained)._value == 10);

    callback_gate.open();

    const bool completed =
        run_future.wait_for(2s) ==
        std::future_status::ready;

    if (!completed) {
        engine.stop();
    }

    runner.join();

    assert(completed);
    assert(run_future.get() ==
           ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::STOPPED);
}

/*
 * 案例 9：STOPPED 状态拒绝上报，但保留停止前最后记录。
 *
 * 执行过程：
 * 1. READY 时注册 Publisher 并保存字符串；
 * 2. 启动后立即 stop()，等待 runner 完整退出；
 * 3. 在 STOPPED 尝试设置 Publisher 和调用三个 report_* 接口；
 * 4. 查询停止前保存的记录和完整快照。
 *
 * 通过标准：
 * STOPPED 中所有写操作被拒绝、Publisher 次数不增加，但 find_data() 和
 * query_data() 仍返回停止前的数据。
 */
void test_stopped_engine_rejects_reports_and_preserves_data() {
    Engine engine(MonConfig{"monitor", 9000, 1});

    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    std::atomic<std::size_t> publish_count{0};

    assert(engine.set_publisher(
        [&](MonData::StoredRecord) {
            publish_count.fetch_add(
                1,
                std::memory_order_relaxed);
        }));

    const MonData::MonitorKey key{8, 8, 8, 8};

    const auto inserted = engine.report_string(
        key,
        "final-state",
        "before-stop");

    assert(inserted._status ==
           MonData::UpdateStatus::INSERTED);
    assert(publish_count.load(
               std::memory_order_relaxed) == 1);

    std::promise<ENGINESTATE> run_promise;
    std::future<ENGINESTATE> run_future =
        run_promise.get_future();

    std::thread runner([&] {
        run_promise.set_value(engine.run());
    });

    assert(wait_for_phase(
        engine,
        EnginePhase::RUNNING,
        1s));

    engine.stop();

    const bool completed =
        run_future.wait_for(2s) ==
        std::future_status::ready;

    if (!completed) {
        engine.stop();
    }

    runner.join();

    assert(completed);
    assert(run_future.get() ==
           ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() == EnginePhase::STOPPED);

    assert(!engine.set_publisher(
        [](MonData::StoredRecord) {}));

    assert(engine.report_count(
               {8, 8, 8, 9},
               1)._status ==
           MonData::UpdateStatus::INVALID);

    assert(engine.report_error(
               {8, 8, 8, 10},
               1)._status ==
           MonData::UpdateStatus::INVALID);

    assert(engine.report_string(
               {8, 8, 8, 11},
               "rejected")._status ==
           MonData::UpdateStatus::INVALID);

    assert(publish_count.load(
               std::memory_order_relaxed) == 1);

    const auto retained = engine.find_data(key);
    assert(retained.has_value());
    assert(string_value(*retained) == "final-state");

    const auto snapshot = engine.query_data();
    assert(snapshot.size() == 1);
    assert(snapshot[0]._data._key == key);
}

/*
 * 案例 10：查询结果在 Engine 析构后仍然有效。
 *
 * 测试目的：
 * 验证 find_data() 和 query_data() 返回值语义副本，不包含指向 MonContext、
 * MonitorReporter 或 MonitorStore 内部对象的悬空引用。
 *
 * 执行过程：
 * 1. 在局部作用域内构造并初始化 Engine；
 * 2. 通过 report_string() 保存记录；
 * 3. 把 find_data() 和 query_data() 结果复制到作用域外；
 * 4. Engine 析构后继续读取两个快照。
 *
 * 通过标准：
 * Engine 析构后，两个快照仍保留完整 Key、description 和字符串值。
 */
void test_reporter_snapshots_survive_engine_destruction() {
    const MonData::MonitorKey key{9, 9, 9, 9};

    std::optional<MonData::StoredRecord> retained_record;
    std::vector<MonData::StoredRecord> retained_snapshot;

    {
        Engine engine(MonConfig{"monitor", 9000, 1});

        assert(engine.init() == ENGINESTATE::SUCCESSFUL);

        const auto inserted = engine.report_string(
            key,
            "retained-value",
            "retained-description");

        assert(inserted._status ==
               MonData::UpdateStatus::INSERTED);

        retained_record = engine.find_data(key);
        retained_snapshot = engine.query_data();

        assert(retained_record.has_value());
        assert(retained_snapshot.size() == 1);
    }

    assert(retained_record.has_value());
    assert(retained_record->_data._key == key);
    assert(retained_record->_data._description ==
           "retained-description");
    assert(string_value(*retained_record) ==
           "retained-value");

    assert(retained_snapshot.size() == 1);
    assert(retained_snapshot[0]._data._key == key);
    assert(string_value(retained_snapshot[0]) ==
           "retained-value");
}

/*
 * 案例 11：stop() 与已经接受的 AIO 上报并发时，允许在途上报完成。
 *
 * 测试目的：
 * 固定阶段 7 步骤 17 的停止并发语义。RUNNING 时已经进入 Reporter 的调用
 * 可以在 STOPPING 期间完成；STOPPING/STOPPED 中进入的新调用必须被拒绝。
 *
 * 执行过程：
 * 1. 注册一个会在 StartGate 上阻塞的 Publisher；
 * 2. 由异步 AIO 回调在 RUNNING 状态调用 report_count()；
 * 3. 等待 Publisher 进入后调用 stop()，使 Engine 稳定处于 STOPPING；
 * 4. 验证 STOPPING 中的新上报和 Publisher 替换均被拒绝；
 * 5. 验证异步 Publisher 未完成时 run() 不会提前返回；
 * 6. 放行 Publisher，等待 AIO worker 和 runner 退出；
 * 7. 验证在途上报已经保存，STOPPED 后的新上报仍被拒绝。
 *
 * 通过标准：
 * 在途上报返回 INSERTED，Publisher 只执行一次；STOPPING/STOPPED 中的新上报
 * 返回 INVALID；runner.join() 后 Engine 为 STOPPED，最后记录仍然可读取。
 */
void test_inflight_aio_report_finishes_during_stopping() {
    /*
     * Engine 不拥有用户注册的 fd。TestPipe 先构造、后析构，保证 Engine
     * 清理 AIO 注册期间管道仍然有效。
     */
    TestPipe input;
    Engine engine(MonConfig{"monitor", 9000, 1});

    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    const MonData::MonitorKey accepted_key{11, 1, 1, 1};
    const MonData::MonitorKey rejected_key{11, 1, 1, 2};

    std::atomic<std::size_t> publish_count{0};
    std::promise<void> publisher_entered_promise;
    std::future<void> publisher_entered =
        publisher_entered_promise.get_future();
    StartGate publisher_gate;

    assert(engine.set_publisher(
        [&](MonData::StoredRecord record) {
            assert(record._data._key == accepted_key);

            publish_count.fetch_add(
                1,
                std::memory_order_relaxed);

            /*
             * set_value() 证明 Store 更新已经完成且 Publisher 已经进入。
             * 随后的等待用于人为延长这次已接受调用的执行时间。
             */
            publisher_entered_promise.set_value();
            publisher_gate.wait();
        }));

    std::promise<MonData::UpdateResult> report_result_promise;
    std::future<MonData::UpdateResult> report_result =
        report_result_promise.get_future();
    std::atomic<bool> callback_claimed{false};

    const auto aio_handle = engine.add_aio(
        input.read_fd(),
        MonCallback{
            "inflight-reporter-stop",
            [&]() -> int {
                /*
                 * select() 是电平触发的。只让第一次激活处理管道并上报，
                 * 避免同一 fd 重复激活后再次读取空管道。
                 */
                if (callback_claimed.exchange(
                        true,
                        std::memory_order_acq_rel)) {
                    return 0;
                }

                input.drain_one();

                MonData::UpdateResult result =
                    engine.report_count(
                        accepted_key,
                        42,
                        "accepted-before-stop");

                /* report_count() 只有在 Publisher 被放行后才会返回。 */
                report_result_promise.set_value(
                    std::move(result));
                return 0;
            },
            true});

    assert(aio_handle.has_value());

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

    input.signal();

    /* Publisher 已进入，说明本次调用已在 RUNNING 阶段被接受。 */
    assert(
        publisher_entered.wait_for(1s) ==
        std::future_status::ready);

    assert(
        publish_count.load(
            std::memory_order_relaxed) == 1);

    /* Publisher 仍被阻塞，所以 report_count() 尚未返回。 */
    assert(
        report_result.wait_for(0ms) ==
        std::future_status::timeout);

    engine.stop();

    assert(engine.get_phase() ==
           EnginePhase::STOPPING);

    const MonData::UpdateResult rejected =
        engine.report_count(
            rejected_key,
            99,
            "rejected-after-stop");

    assert(rejected._status ==
           MonData::UpdateStatus::INVALID);
    assert(!rejected._record.has_value());

    assert(!engine.set_publisher(
        [](MonData::StoredRecord) {}));

    /*
     * run() 清理时会等待异步 AIO worker。Publisher 尚未放行，因此
     * Engine 必须继续停留在 STOPPING，run() 不能提前返回。
     */
    assert(
        run_result.wait_for(0ms) ==
        std::future_status::timeout);
    assert(engine.get_phase() ==
           EnginePhase::STOPPING);

    publisher_gate.open();

    assert(
        report_result.wait_for(2s) ==
        std::future_status::ready);

    const MonData::UpdateResult accepted =
        report_result.get();

    assert(accepted._status ==
           MonData::UpdateStatus::INSERTED);
    assert(accepted._record.has_value());
    assert(numeric_value(*accepted._record)._value == 42);

    assert(
        run_result.wait_for(2s) ==
        std::future_status::ready);

    runner.join();

    assert(run_result.get() ==
           ENGINESTATE::SUCCESSFUL);
    assert(engine.get_phase() ==
           EnginePhase::STOPPED);
    assert(
        publish_count.load(
            std::memory_order_relaxed) == 1);

    const auto stored = engine.find_data(accepted_key);

    assert(stored.has_value());
    assert(numeric_value(*stored)._value == 42);
    assert(stored->_data._description ==
           "accepted-before-stop");

    /* STOPPING 中被拒绝的 Key 不能产生记录。 */
    assert(!engine.find_data(rejected_key).has_value());

    const MonData::UpdateResult stopped_result =
        engine.report_error(
            {11, 1, 1, 3},
            1,
            "rejected-after-join");

    assert(stopped_result._status ==
           MonData::UpdateStatus::INVALID);
    assert(!stopped_result._record.has_value());
    assert(
        publish_count.load(
            std::memory_order_relaxed) == 1);
}

} // namespace

int main() {
    test_created_engine_rejects_publisher_and_reports();

    test_ready_engine_registers_publisher_and_reports_count();
    test_ready_engine_forwards_error_and_string_reports();
    test_update_data_uses_reporter_publish_gate();
    test_ready_engine_can_unregister_publisher();

    test_running_engine_accepts_publisher_and_reports();

    test_stopping_engine_rejects_publisher_and_reports();
    test_stopped_engine_rejects_reports_and_preserves_data();

    test_reporter_snapshots_survive_engine_destruction();

    test_inflight_aio_report_finishes_during_stopping();

    return 0;
}
