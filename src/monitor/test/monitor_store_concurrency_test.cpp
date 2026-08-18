#include "monitor_store.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using namespace TLSSMON;

MonData::MonitorData make_numeric(MonData::MonitorKey key, std::uint32_t value,
                                  std::uint32_t state = 2,
                                  std::string description = "concurrency") {
    return MonData::MonitorData{key, std::move(description),
        MonData::NumericValue{value, state}};
}

MonData::MonitorTimestamp timestamp_at(std::int64_t seconds) {
    return MonData::MonitorTimestamp{std::chrono::seconds{seconds}};
}

const MonData::NumericValue &
numeric_value(const MonData::StoredRecord &record) {
    return std::get<MonData::NumericValue>(record._data._value);
}

class StartGate final {
public:
    StartGate() = default;

    StartGate(const StartGate &) = delete;
    StartGate &operator=(const StartGate &) = delete;

    /**
     * 等待主线程打开启动门。
     */
    void wait() {
        std::unique_lock<std::mutex> lock(_mutex);

        /*
         * 使用带谓词的 wait()：
         *
         * 1. 防止虚假唤醒；
         * 2. 即使 open() 先执行，后进入的线程也不会丢失通知。
         */
        _condition.wait(lock, [this] { return _open; });
    }

    /**
     * 打开启动门并唤醒所有工作线程。
     */
    void open() {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _open = true;
        }

        /*
         * 修改共享状态时需要持锁；
         * notify_all() 可以在释放锁后执行。
         */
        _condition.notify_all();
    }

private:
    std::mutex _mutex;
    std::condition_variable _condition;
    bool _open{false};
};

/**
 * 验证并发测试使用的数据夹具。
 */
void test_numeric_fixture() {
    const MonData::MonitorKey key{1, 0, 10, 1};

    MonData::StoredRecord record{make_numeric(key, 42, 1, "fixture"),
        timestamp_at(100)};

    assert(record._data._key == key);
    assert(record._data._description == "fixture");

    assert(numeric_value(record)._value == 42);
    assert(numeric_value(record)._state == 1);

    assert(record._changed_at == timestamp_at(100));
}

/**
 * 验证 StartGate 可以同时释放全部工作线程。
 */
void test_start_gate_releases_all_workers() {
    constexpr std::size_t worker_count = 4;

    StartGate gate;

    /*
     * 这些计数器会被多个线程访问，
     * 因此必须使用 atomic。
     */
    std::atomic<std::size_t> ready_count{0};
    std::atomic<std::size_t> released_count{0};

    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    for (std::size_t index = 0; index < worker_count; ++index) {

        workers.emplace_back([&gate, &ready_count, &released_count] {
                             /*
                              * 表示当前线程已经启动，
                              * 即将进入 StartGate。
                              */
                             ready_count.fetch_add(1);

                             gate.wait();

                             /*
                              * 只有 gate.open() 执行后，
                              * 才能运行到这里。
                              */
                             released_count.fetch_add(1);
                             });
    }

    /*
     * 等待全部线程启动。
     *
     * 这里不使用 sleep()，避免测试依赖机器速度。
     */
    while (ready_count.load() != worker_count) {
        std::this_thread::yield();
    }

    /*
     * 启动门仍然关闭，没有线程可以通过。
     */
    assert(released_count.load() == 0);

    gate.open();

    /*
     * MonitorStore 或测试夹具析构前，
     * 必须回收全部工作线程。
     */
    for (std::thread &worker : workers) {
        worker.join();
    }

    assert(released_count.load() == worker_count);
}

/**
 * 验证多个线程同时写入不同 Key。
 *
 * 8 个线程分别写入 200 条记录：
 *
 * 8 * 200 = 1600 条记录。
 *
 * 所有 Key、数值和时间戳都不重复。
 */
void test_concurrent_writes_to_distinct_keys() {
    constexpr std::size_t thread_count = 8;
    constexpr std::size_t records_per_thread = 200;

    constexpr std::size_t expected_record_count =
        thread_count * records_per_thread;

    MonitorStore store;
    StartGate gate;

    /*
     * 已经启动并准备进入 StartGate 的线程数。
     */
    std::atomic<std::size_t> ready_count{0};

    /*
     * 成功插入的记录数。
     */
    std::atomic<std::size_t> inserted_count{0};

    /*
     * 非预期返回结果数量。
     */
    std::atomic<std::size_t> unexpected_count{0};

    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    for (std::size_t thread_index = 0; thread_index < thread_count;
         ++thread_index) {

        /*
         * thread_index 必须按值捕获。
         *
         * 其他测试对象在所有线程 join() 前都保持有效，
         * 因此按引用捕获。
         */
        workers.emplace_back([&, thread_index] {
                             ready_count.fetch_add(1);

                             /*
                              * 等待所有工作线程创建完成。
                              */
                             gate.wait();

                             for (std::size_t record_index = 0; record_index < records_per_thread;
                                  ++record_index) {

                             /*
                              * ordinal 的范围是：
                              *
                              * 0 ～ 1599
                              */
                             const std::size_t ordinal =
                             thread_index * records_per_thread + record_index;

                             /*
                              * 数值从 1 开始，避免首次零值被忽略。
                              */
                             const std::uint32_t sequence = static_cast<std::uint32_t>(ordinal + 1);

                             /*
                              * 每条记录使用不同 Key。
                              *
                              * mid：区分工作线程；
                              * fid：区分线程内部记录；
                              * eid：全局唯一序号。
                              */
                             const MonData::MonitorKey key{
                                 static_cast<std::uint32_t>(thread_index + 1),

                                     0,

                                     static_cast<std::uint32_t>(record_index + 1),

                                     sequence};

                             const MonData::UpdateResult result =
                                 store.update(make_numeric(key, sequence, 2, "distinct-key"), false,
                                              timestamp_at(sequence));

                             /*
                              * 不在线程内部直接 assert。
                              *
                              * 线程结束后由主线程统一验证，确保所有
                              * std::thread 都能正常 join()。
                              */
                             if (result._status == MonData::UpdateStatus::INSERTED &&
                                 result._record.has_value()) {

                                 inserted_count.fetch_add(1);
                             } else {
                                 unexpected_count.fetch_add(1);
                             }
                             }
        });
    }

    /*
     * 等待所有线程都已经启动。
     */
    while (ready_count.load() != thread_count) {
        std::this_thread::yield();
    }

    /*
     * 同时释放全部工作线程。
     */
    gate.open();

    for (std::thread &worker : workers) {
        worker.join();
    }

    /*
     * 所有线程已经结束，现在可以安全检查 Store。
     */
    assert(ready_count.load() == thread_count);

    assert(inserted_count.load() == expected_record_count);

    assert(unexpected_count.load() == 0);

    /*
     * 不允许丢失记录。
     */
    assert(store.size() == expected_record_count);

    const auto snapshot = store.query();

    assert(snapshot.size() == expected_record_count);

    /*
     * 逐条确认每个 Key 和记录内容均存在。
     */
    for (std::size_t thread_index = 0; thread_index < thread_count;
         ++thread_index) {

        for (std::size_t record_index = 0; record_index < records_per_thread;
             ++record_index) {

            const std::size_t ordinal =
                thread_index * records_per_thread + record_index;

            const std::uint32_t sequence = static_cast<std::uint32_t>(ordinal + 1);

            const MonData::MonitorKey key{
                static_cast<std::uint32_t>(thread_index + 1),

                    0,

                    static_cast<std::uint32_t>(record_index + 1),

                    sequence};

            const auto stored = store.find(key);

            assert(stored.has_value());

            assert(stored->_data._description == "distinct-key");

            assert(numeric_value(*stored)._value == sequence);

            assert(numeric_value(*stored)._state == 2);

            assert(stored->_changed_at == timestamp_at(sequence));
        }
    }
}

/**
 * 验证多个线程同时写入同一个 Key。
 *
 * 每次写入使用全局唯一的非零值，因此：
 *
 * 1. 第一次写入返回 INSERTED；
 * 2. 其余写入全部返回 UPDATED；
 * 3. 不应出现 UNCHANGED；
 * 4. Store 中始终只有一个 Key 节点。
 */
void test_concurrent_writes_to_same_key() {
    constexpr std::size_t thread_count = 8;
    constexpr std::size_t writes_per_thread = 500;

    constexpr std::size_t total_writes = thread_count * writes_per_thread;

    /*
     * 所有线程共享同一个 Key。
     */
    const MonData::MonitorKey key{100, 1, 200, 300};

    MonitorStore store;
    StartGate gate;

    std::atomic<std::size_t> ready_count{0};
    std::atomic<std::size_t> inserted_count{0};
    std::atomic<std::size_t> updated_count{0};
    std::atomic<std::size_t> unchanged_count{0};
    std::atomic<std::size_t> unexpected_count{0};
    std::atomic<std::size_t> missing_record_count{0};

    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    for (std::size_t thread_index = 0; thread_index < thread_count;
         ++thread_index) {

        workers.emplace_back([&, thread_index] {
                             ready_count.fetch_add(1);

                             /*
                              * 等待所有工作线程启动。
                              */
                             gate.wait();

                             for (std::size_t write_index = 0; write_index < writes_per_thread;
                                  ++write_index) {

                             /*
                              * 为每次写入生成全局唯一序号。
                              *
                              * 范围：
                              *
                              * 1 ～ 4000
                              */
                             const std::size_t ordinal =
                             thread_index * writes_per_thread + write_index;

                             const std::uint32_t value = static_cast<std::uint32_t>(ordinal + 1);

                             const MonData::UpdateResult result =
                                 store.update(make_numeric(key, value, 2, "same-key"), false,
                                              timestamp_at(value));

                             /*
                              * INSERTED、UPDATED 和 UNCHANGED
                              * 都应返回记录副本。
                              */
                             if (!result._record.has_value()) {
                                 missing_record_count.fetch_add(1);
                             }

                             switch (result._status) {
                             case MonData::UpdateStatus::INSERTED:
                                 inserted_count.fetch_add(1);
                                 break;

                             case MonData::UpdateStatus::UPDATED:
                                 updated_count.fetch_add(1);
                                 break;

                             case MonData::UpdateStatus::UNCHANGED:
                                 unchanged_count.fetch_add(1);
                                 break;

                             default:
                                 /*
                                  * 包括：
                                  *
                                  * IGNORED_INITIAL_ZERO
                                  * INVALID
                                  */
                                 unexpected_count.fetch_add(1);
                                 break;
                             }
                             }
        });
    }

    /*
     * 等待所有线程就绪。
     */
    while (ready_count.load() != thread_count) {
        std::this_thread::yield();
    }

    /*
     * 同时释放所有线程。
     */
    gate.open();

    for (std::thread &worker : workers) {
        worker.join();
    }

    /*
     * 线程全部结束后统一检查结果。
     */
    assert(ready_count.load() == thread_count);

    /*
     * 对同一个不存在的 Key，并发首次写入时，
     * 只能有一个线程完成首次插入。
     */
    assert(inserted_count.load() == 1);

    /*
     * 每次写入的数值都不同，因此首次插入之后，
     * 其余写入全部应当更新。
     */
    assert(updated_count.load() == total_writes - 1);

    assert(unchanged_count.load() == 0);
    assert(unexpected_count.load() == 0);
    assert(missing_record_count.load() == 0);

    /*
     * 即使发生了 4,000 次并发写入，
     * 同一个 Key 也只能产生一个 map 节点。
     */
    assert(store.size() == 1);

    const auto snapshot = store.query();

    assert(snapshot.size() == 1);
    assert(snapshot[0]._data._key == key);

    const auto stored = store.find(key);

    assert(stored.has_value());
    assert(stored->_data._description == "same-key");
    assert(numeric_value(*stored)._state == 2);

    /*
     * 最后完成写入的是哪个线程由调度决定，
     * 所以不能断言具体最终值。
     */
    const std::uint32_t final_value = numeric_value(*stored)._value;

    assert(final_value >= 1);
    assert(final_value <= total_writes);

    /*
     * value 和 timestamp 必须来自同一次整体更新。
     *
     * 如果 update() 的替换过程不是互斥的，
     * 可能出现数值来自线程 A、时间戳来自线程 B。
     */
    assert(stored->_changed_at == timestamp_at(final_value));
}

/**
 * 检查查询与写入并发时获得的快照。
 */
bool query_write_snapshot_is_valid(
                                   const std::vector<MonData::StoredRecord> &snapshot,
                                   std::size_t maximum_size) {
    /*
     * writer 最多只会插入 maximum_size 条记录。
     */
    if (snapshot.size() > maximum_size) {
        return false;
    }

    /*
     * query() 必须保持 MonitorKey 的 map 顺序。
     */
    const bool sorted = std::is_sorted(
                                       snapshot.begin(), snapshot.end(),
                                       [](const MonData::StoredRecord &lhs, const MonData::StoredRecord &rhs) {
                                       return lhs._data._key < rhs._data._key;
                                       });

    if (!sorted) {
        return false;
    }

    /*
     * 相邻记录不能出现相同 Key。
     *
     * 快照已经排序，所以重复 Key 一定相邻。
     */
    const auto duplicate = std::adjacent_find(
                                              snapshot.begin(), snapshot.end(),
                                              [](const MonData::StoredRecord &lhs, const MonData::StoredRecord &rhs) {
                                              return lhs._data._key == rhs._data._key;
                                              });

    if (duplicate != snapshot.end()) {
        return false;
    }

    /*
     * 检查每条记录是否是完整副本。
     */
    for (const MonData::StoredRecord &record : snapshot) {
        const MonData::NumericValue *const numeric =
            std::get_if<MonData::NumericValue>(&record._data._value);

        if (numeric == nullptr) {
            return false;
        }

        /*
         * writer 约定：
         *
         * description = "query-write"
         * value       = key._eid
         * state       = 2
         * timestamp   = value 对应的秒数
         *
         * 如果 query() 与 update() 之间没有正确互斥，
         * 可能读取到字段组合不一致的记录。
         */
        if (record._data._description != "query-write" ||
            numeric->_value != record._data._key._eid || numeric->_state != 2 ||
            record._changed_at != timestamp_at(numeric->_value)) {

            return false;
        }
    }

    return true;
}

/**
 * 验证 query() 与不同 Key 写入并发执行。
 */
void test_query_while_writing_distinct_keys() {
    constexpr std::size_t writer_count = 4;
    constexpr std::size_t reader_count = 4;
    constexpr std::size_t records_per_writer = 400;
    constexpr std::size_t minimum_queries_per_reader = 200;

    constexpr std::size_t total_thread_count = writer_count + reader_count;

    constexpr std::size_t expected_record_count =
        writer_count * records_per_writer;

    MonitorStore store;
    StartGate gate;

    std::atomic<std::size_t> ready_count{0};
    std::atomic<std::size_t> writers_finished{0};
    std::atomic<std::size_t> inserted_count{0};
    std::atomic<std::size_t> unexpected_write_count{0};
    std::atomic<std::size_t> snapshot_failure_count{0};
    std::atomic<std::size_t> query_count{0};

    std::vector<std::thread> workers;
    workers.reserve(total_thread_count);

    /*
     * 创建 writer。
     */
    for (std::size_t writer_index = 0; writer_index < writer_count;
         ++writer_index) {

        workers.emplace_back([&, writer_index] {
                             ready_count.fetch_add(1);
                             gate.wait();

                             for (std::size_t record_index = 0; record_index < records_per_writer;
                                  ++record_index) {

                             const std::size_t ordinal =
                             writer_index * records_per_writer + record_index;

                             /*
                              * sequence 范围是 1～1600，
                              * 避免触发首次零值门禁。
                              */
                             const std::uint32_t sequence = static_cast<std::uint32_t>(ordinal + 1);

                             const MonData::MonitorKey key{
                             static_cast<std::uint32_t>(200 + writer_index),

                             2,

                             static_cast<std::uint32_t>(record_index + 1),

                             sequence};

                             const MonData::UpdateResult result =
                                 store.update(make_numeric(key, sequence, 2, "query-write"), false,
                                              timestamp_at(sequence));

                             /*
                              * 所有 Key 都是唯一的，
                              * 因此每次写入都必须是 INSERTED。
                              */
                             if (result._status == MonData::UpdateStatus::INSERTED &&
                                 result._record.has_value()) {

                                 inserted_count.fetch_add(1);
                             } else {
                                 unexpected_write_count.fetch_add(1);
                             }

                             /*
                              * 增加 writer 和 reader 交错运行的机会，
                              * 但测试正确性不依赖具体调度时间。
                              */
                             if ((record_index + 1) % 16 == 0) {
                                 std::this_thread::yield();
                             }
                             }

                             writers_finished.fetch_add(1);
        });
    }

    /*
     * 创建 reader。
     */
    for (std::size_t reader_index = 0; reader_index < reader_count;
         ++reader_index) {

        workers.emplace_back([&] {
                             ready_count.fetch_add(1);
                             gate.wait();

                             std::size_t local_query_count = 0;

                             /*
                              * reader 至少查询 200 次。
                              *
                              * 如果 writer 尚未全部完成，即使已经达到
                              * 200 次，也继续查询。
                              */
                             do {
                             const auto snapshot = store.query();

                             if (!query_write_snapshot_is_valid(snapshot, expected_record_count)) {

                             snapshot_failure_count.fetch_add(1);
                             }

                             ++local_query_count;

                             if (local_query_count % 16 == 0) {
                                 std::this_thread::yield();
                             }

                             } while (writers_finished.load() != writer_count ||
                                      local_query_count < minimum_queries_per_reader);

                             query_count.fetch_add(local_query_count);
        });
    }

    /*
     * 等待所有 writer 和 reader 就绪。
     */
    while (ready_count.load() != total_thread_count) {

        std::this_thread::yield();
    }

    gate.open();

    for (std::thread &worker : workers) {
        worker.join();
    }

    /*
     * 所有线程结束后统一断言。
     */
    assert(ready_count.load() == total_thread_count);

    assert(writers_finished.load() == writer_count);

    assert(inserted_count.load() == expected_record_count);

    assert(unexpected_write_count.load() == 0);

    assert(snapshot_failure_count.load() == 0);

    /*
     * 每个 reader 至少执行 200 次查询。
     */
    assert(query_count.load() >= reader_count * minimum_queries_per_reader);

    /*
     * writer 使用的 Key 全部不同，
     * 所以最终记录数必须为 1600。
     */
    assert(store.size() == expected_record_count);

    /*
     * 所有 writer 完成后的最终快照也必须有效。
     */
    const auto final_snapshot = store.query();

    assert(final_snapshot.size() == expected_record_count);

    assert(query_write_snapshot_is_valid(final_snapshot, expected_record_count));
}

/**
 * 验证 clear() 与 query()/update() 并发执行。
 */
void test_clear_while_querying_and_writing() {
    constexpr std::size_t writer_count = 4;
    constexpr std::size_t reader_count = 2;
    constexpr std::size_t clearer_count = 1;

    constexpr std::size_t records_per_writer = 400;
    constexpr std::size_t minimum_queries_per_reader = 200;
    constexpr std::size_t minimum_clear_count = 200;

    constexpr std::size_t total_thread_count =
        writer_count + reader_count + clearer_count;

    constexpr std::size_t maximum_record_count =
        writer_count * records_per_writer;

    MonitorStore store;
    StartGate gate;

    std::atomic<std::size_t> ready_count{0};
    std::atomic<std::size_t> writers_finished{0};

    std::atomic<bool> clearer_started{false};
    std::atomic<bool> clearer_finished{false};

    std::atomic<std::size_t> inserted_count{0};
    std::atomic<std::size_t> unexpected_write_count{0};

    std::atomic<std::size_t> query_count{0};
    std::atomic<std::size_t> clear_count{0};
    std::atomic<std::size_t> snapshot_failure_count{0};

    std::vector<std::thread> workers;
    workers.reserve(total_thread_count);

    /*
     * 创建 writer。
     */
    for (std::size_t writer_index = 0; writer_index < writer_count;
         ++writer_index) {

        workers.emplace_back([&, writer_index] {
                             ready_count.fetch_add(1);
                             gate.wait();

                             /*
                              * 确保 clear 线程已经进入并发阶段。
                              */
                             while (!clearer_started.load()) {
                             std::this_thread::yield();
                             }

                             for (std::size_t record_index = 0; record_index < records_per_writer;
                                  ++record_index) {

                             const std::size_t ordinal =
                             writer_index * records_per_writer + record_index;

                             const std::uint32_t sequence = static_cast<std::uint32_t>(ordinal + 1);

                             const MonData::MonitorKey key{
                             static_cast<std::uint32_t>(300 + writer_index),

                                 3,

                                 static_cast<std::uint32_t>(record_index + 1),

                                 sequence};

                             /*
                              * 继续使用 query_write_snapshot_is_valid()
                              * 规定的记录格式。
                              */
                             const MonData::UpdateResult result =
                                 store.update(make_numeric(key, sequence, 2, "query-write"), false,
                                              timestamp_at(sequence));

                             /*
                              * 所有 Key 只写一次。
                              *
                              * clear() 可能删除之前的记录，但不会令
                              * 当前唯一 Key 变成 UPDATED。
                              */
                             if (result._status == MonData::UpdateStatus::INSERTED &&
                                 result._record.has_value()) {

                                 inserted_count.fetch_add(1);
                             } else {
                                 unexpected_write_count.fetch_add(1);
                             }

                             if ((record_index + 1) % 16 == 0) {
                                 std::this_thread::yield();
                             }
                             }

                             writers_finished.fetch_add(1);
        });
    }

    /*
     * 创建 reader。
     */
    for (std::size_t reader_index = 0; reader_index < reader_count;
         ++reader_index) {

        workers.emplace_back([&] {
                             ready_count.fetch_add(1);
                             gate.wait();

                             while (!clearer_started.load()) {
                             std::this_thread::yield();
                             }

                             std::size_t local_query_count = 0;

                             /*
                              * writer 或 clearer 尚未结束时持续查询。
                              *
                              * 即使它们很快结束，每个 reader 也至少
                              * 查询 minimum_queries_per_reader 次。
                              */
                             do {
                             const auto snapshot = store.query();

                             if (!query_write_snapshot_is_valid(snapshot, maximum_record_count)) {

                                 snapshot_failure_count.fetch_add(1);
                             }

                             ++local_query_count;

                             if (local_query_count % 16 == 0) {
                                 std::this_thread::yield();
                             }

                             } while (writers_finished.load() != writer_count ||
                                      !clearer_finished.load() ||
                                      local_query_count < minimum_queries_per_reader);

                             query_count.fetch_add(local_query_count);
        });
    }

    /*
     * 创建 clearer。
     */
    workers.emplace_back([&] {
                         ready_count.fetch_add(1);
                         gate.wait();

                         clearer_started.store(true);

                         /*
                          * 至少清空 200 次。
                          *
                          * writer 尚未全部结束时，即使已经达到
                          * 200 次，也继续执行 clear()。
                          */
                         do {
                         store.clear();
                         clear_count.fetch_add(1);

                         std::this_thread::yield();

                         } while (writers_finished.load() != writer_count ||
                                  clear_count.load() < minimum_clear_count);

                         clearer_finished.store(true);
    });

    /*
     * 等待所有线程准备完成。
     */
    while (ready_count.load() != total_thread_count) {

        std::this_thread::yield();
    }

    gate.open();

    for (std::thread &worker : workers) {
        worker.join();
    }

    /*
     * 所有线程结束后统一检查。
     */
    assert(ready_count.load() == total_thread_count);

    assert(writers_finished.load() == writer_count);

    assert(clearer_started.load());
    assert(clearer_finished.load());

    /*
     * 每个唯一 Key 的 update() 都正常完成。
     */
    assert(inserted_count.load() == maximum_record_count);

    assert(unexpected_write_count.load() == 0);

    assert(clear_count.load() >= minimum_clear_count);

    assert(query_count.load() >= reader_count * minimum_queries_per_reader);

    assert(snapshot_failure_count.load() == 0);

    /*
     * clear() 的执行顺序不确定，因此最终大小也不确定。
     */
    const auto final_snapshot = store.query();

    assert(final_snapshot.size() <= maximum_record_count);

    /*
     * 所有线程已经结束，此时 size() 和 query()
     * 之间不会再发生修改。
     */
    assert(store.size() == final_snapshot.size());

    assert(query_write_snapshot_is_valid(final_snapshot, maximum_record_count));

    /*
     * 最后进行一次没有并发干扰的确定性清空。
     */
    store.clear();

    assert(store.size() == 0);
    assert(store.query().empty());
}

/**
 * 验证保留快照的内容没有发生变化。
 */
bool retained_snapshot_is_valid(
                                const std::vector<MonData::StoredRecord> &snapshot,
                                std::size_t expected_size) {
    if (snapshot.size() != expected_size) {
        return false;
    }

    for (std::size_t index = 0; index < expected_size; ++index) {

        const std::uint32_t sequence = static_cast<std::uint32_t>(index + 1);

        const MonData::MonitorKey expected_key{400, 4, sequence, sequence};

        const MonData::StoredRecord &record = snapshot[index];

        if (record._data._key != expected_key ||
            record._data._description != "retained-snapshot") {

            return false;
        }

        const MonData::NumericValue *const numeric =
            std::get_if<MonData::NumericValue>(&record._data._value);

        if (numeric == nullptr || numeric->_value != sequence ||
            numeric->_state != 2 || record._changed_at != timestamp_at(sequence)) {

            return false;
        }
    }

    return true;
}

/**
 * 验证快照在 Store 并发修改、清空和析构后仍然有效。
 */
void test_snapshot_survives_concurrent_mutation_and_store_destruction() {
    constexpr std::size_t baseline_record_count = 128;
    constexpr std::size_t writer_count = 4;
    constexpr std::size_t snapshot_reader_count = 2;
    constexpr std::size_t clearer_count = 1;

    constexpr std::size_t updates_per_writer = 300;
    constexpr std::size_t minimum_snapshot_reads = 200;
    constexpr std::size_t minimum_clear_count = 200;

    constexpr std::size_t total_update_count = writer_count * updates_per_writer;

    constexpr std::size_t total_thread_count =
        writer_count + snapshot_reader_count + clearer_count;

    /*
     * 定义在 Store 作用域之外。
     *
     * 用于验证 Store 析构后快照仍然有效。
     */
    std::vector<MonData::StoredRecord> retained_snapshot;

    {
        MonitorStore store;

        /*
         * 插入 128 条基准记录。
         */
        for (std::size_t index = 0; index < baseline_record_count; ++index) {

            const std::uint32_t sequence = static_cast<std::uint32_t>(index + 1);

            const MonData::MonitorKey key{400, 4, sequence, sequence};

            const MonData::UpdateResult result =
                store.update(make_numeric(key, sequence, 2, "retained-snapshot"),
                             false, timestamp_at(sequence));

            assert(result._status == MonData::UpdateStatus::INSERTED);
        }

        /*
         * 保存独立快照。
         */
        retained_snapshot = store.query();

        assert(
               retained_snapshot_is_valid(retained_snapshot, baseline_record_count));

        StartGate gate;

        std::atomic<std::size_t> ready_count{0};
        std::atomic<std::size_t> writers_finished{0};

        std::atomic<bool> clearer_started{false};
        std::atomic<bool> clearer_finished{false};

        std::atomic<std::size_t> accepted_update_count{0};

        std::atomic<std::size_t> unexpected_update_count{0};

        std::atomic<std::size_t> snapshot_read_count{0};

        std::atomic<std::size_t> snapshot_failure_count{0};

        std::atomic<std::size_t> clear_count{0};

        std::vector<std::thread> workers;
        workers.reserve(total_thread_count);

        /*
         * writer 只修改 Store，
         * 不修改 retained_snapshot。
         */
        for (std::size_t writer_index = 0; writer_index < writer_count;
             ++writer_index) {

            workers.emplace_back([&, writer_index] {
                                 ready_count.fetch_add(1);
                                 gate.wait();

                                 while (!clearer_started.load()) {
                                 std::this_thread::yield();
                                 }

                                 for (std::size_t update_index = 0; update_index < updates_per_writer;
                                      ++update_index) {

                                 const std::size_t ordinal =
                                 writer_index * updates_per_writer + update_index;

                                 /*
                                  * 重复修改基准快照中的 128 个 Key。
                                  */
                                 const std::uint32_t slot =
                                 static_cast<std::uint32_t>(ordinal % baseline_record_count + 1);

                                 /*
                                  * 每次更新使用唯一值，避免 UNCHANGED。
                                  */
                                 const std::uint32_t value =
                                     static_cast<std::uint32_t>(10001 + ordinal);

                                 const MonData::MonitorKey key{400, 4, slot, slot};

                                 const MonData::UpdateResult result =
                                     store.update(make_numeric(key, value, 2, "mutated-store"), false,
                                                  timestamp_at(value));

                                 /*
                                  * Key 可能被 clear() 删除：
                                  *
                                  * 存在时返回 UPDATED；
                                  * 被删除后返回 INSERTED。
                                  */
                                 const bool accepted =
                                     (result._status == MonData::UpdateStatus::INSERTED ||
                                      result._status == MonData::UpdateStatus::UPDATED) &&
                                     result._record.has_value();

                                 if (accepted) {
                                     accepted_update_count.fetch_add(1);
                                 } else {
                                     unexpected_update_count.fetch_add(1);
                                 }

                                 if ((update_index + 1) % 16 == 0) {
                                     std::this_thread::yield();
                                 }
                                 }

                                 writers_finished.fetch_add(1);
            });
        }

        /*
         * 快照 reader 不访问 Store，
         * 只反复检查 retained_snapshot。
         */
        for (std::size_t reader_index = 0; reader_index < snapshot_reader_count;
             ++reader_index) {

            workers.emplace_back([&] {
                                 ready_count.fetch_add(1);
                                 gate.wait();

                                 while (!clearer_started.load()) {
                                 std::this_thread::yield();
                                 }

                                 std::size_t local_read_count = 0;

                                 do {
                                 if (!retained_snapshot_is_valid(retained_snapshot,
                                                                 baseline_record_count)) {

                                 snapshot_failure_count.fetch_add(1);
                                 }

                                 ++local_read_count;

                                 if (local_read_count % 16 == 0) {
                                 std::this_thread::yield();
                                 }

                                 } while (writers_finished.load() != writer_count ||
                                          !clearer_finished.load() ||
                                          local_read_count < minimum_snapshot_reads);

                                 snapshot_read_count.fetch_add(local_read_count);
            });
        }

        /*
         * clearer 持续清空 Store。
         */
        workers.emplace_back([&] {
                             ready_count.fetch_add(1);
                             gate.wait();

                             clearer_started.store(true);

                             do {
                             store.clear();
                             clear_count.fetch_add(1);

                             std::this_thread::yield();

                             } while (writers_finished.load() != writer_count ||
                                      clear_count.load() < minimum_clear_count);

                             clearer_finished.store(true);
                             });

        while (ready_count.load() != total_thread_count) {

            std::this_thread::yield();
        }

        gate.open();

        /*
         * Store 销毁前必须结束全部工作线程。
         */
        for (std::thread &worker : workers) {
            worker.join();
        }

        assert(ready_count.load() == total_thread_count);

        assert(writers_finished.load() == writer_count);

        assert(clearer_started.load());
        assert(clearer_finished.load());

        assert(accepted_update_count.load() == total_update_count);

        assert(unexpected_update_count.load() == 0);

        assert(clear_count.load() >= minimum_clear_count);

        assert(snapshot_read_count.load() >=
               snapshot_reader_count * minimum_snapshot_reads);

        assert(snapshot_failure_count.load() == 0);

        /*
         * Store 已被大量修改和清空，
         * 旧快照仍保持最初内容。
         */
        assert(
               retained_snapshot_is_valid(retained_snapshot, baseline_record_count));

        /*
         * writer 只使用最初的 128 个 Key。
         */
        assert(store.size() <= baseline_record_count);

        /*
         * 清空 Store 不得影响旧快照。
         */
        store.clear();

        assert(store.size() == 0);
        assert(store.query().empty());

        assert(
               retained_snapshot_is_valid(retained_snapshot, baseline_record_count));
    }

    /*
     * 执行到这里，Store 已经析构。
     *
     * retained_snapshot 仍然必须可以完整访问。
     */
    assert(retained_snapshot_is_valid(retained_snapshot, baseline_record_count));
}

int main() {
    test_numeric_fixture();
    test_start_gate_releases_all_workers();

    test_concurrent_writes_to_distinct_keys();
    test_concurrent_writes_to_same_key();
    test_query_while_writing_distinct_keys();
    test_clear_while_querying_and_writing();

    test_snapshot_survives_concurrent_mutation_and_store_destruction();

    return 0;
}
