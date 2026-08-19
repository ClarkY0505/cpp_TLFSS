#include "engine.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace TLSSMON;

namespace {

const MonData::NumericValue& numeric_value(
    const MonData::StoredRecord& record) {

    return std::get<MonData::NumericValue>(
        record._data._value);
}

/*
 * 阶段 9 案例 1：多个线程通过 Engine 上报不同 Key。
 *
 * 每次写入都是首次非零计数，因此全部应为 INSERTED，Publisher 调用次数和
 * Store 快照大小都必须等于总写入数。这同时验证 Engine 没有额外串行化或
 * 丢弃并发 Reporter 调用。
 */
void test_multiple_threads_report_different_keys() {
    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    constexpr std::size_t writer_count = 8;
    constexpr std::size_t writes_per_writer = 200;
    constexpr std::size_t expected =
        writer_count * writes_per_writer;

    std::atomic<std::size_t> inserted{0};
    std::atomic<std::size_t> publish_count{0};
    std::atomic<bool> start{false};

    assert(engine.set_publisher(
        [&](MonData::StoredRecord) {
            publish_count.fetch_add(1, std::memory_order_relaxed);
        }));

    std::vector<std::thread> writers;
    writers.reserve(writer_count);

    for (std::size_t writer = 0;
         writer < writer_count;
         ++writer) {

        writers.emplace_back([&, writer] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (std::size_t index = 0;
                 index < writes_per_writer;
                 ++index) {

                const auto result = engine.report_count(
                    {90,
                     1,
                     static_cast<std::uint32_t>(writer),
                     static_cast<std::uint32_t>(index)},
                    1,
                    "different-key");

                if (result._status ==
                    MonData::UpdateStatus::INSERTED) {
                    inserted.fetch_add(
                        1,
                        std::memory_order_relaxed);
                }
            }
        });
    }

    start.store(true, std::memory_order_release);

    for (std::thread& writer : writers) {
        writer.join();
    }

    assert(inserted.load(std::memory_order_relaxed) == expected);
    assert(publish_count.load(std::memory_order_relaxed) == expected);
    assert(engine.query_data().size() == expected);
}

/*
 * 阶段 9 案例 2：多个线程上报同一 Key 和相同值。
 *
 * Store 的同 Key 更新必须原子化：只能有一个 INSERTED，其余调用全部为
 * UNCHANGED；Publisher 只收到一次实际变化，Store 不产生重复节点。
 */
void test_multiple_threads_same_key_publish_once() {
    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    constexpr std::size_t writer_count = 16;
    const MonData::MonitorKey key{90, 2, 1, 1};

    std::atomic<bool> start{false};
    std::atomic<std::size_t> inserted{0};
    std::atomic<std::size_t> unchanged{0};
    std::atomic<std::size_t> publish_count{0};

    assert(engine.set_publisher(
        [&](MonData::StoredRecord record) {
            assert(record._data._key == key);
            publish_count.fetch_add(1, std::memory_order_relaxed);
        }));

    std::vector<std::thread> writers;
    writers.reserve(writer_count);

    for (std::size_t index = 0;
         index < writer_count;
         ++index) {

        writers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            const auto result = engine.report_count(
                key,
                42,
                "same-key");

            if (result._status ==
                MonData::UpdateStatus::INSERTED) {
                inserted.fetch_add(1, std::memory_order_relaxed);
            } else if (result._status ==
                       MonData::UpdateStatus::UNCHANGED) {
                unchanged.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    start.store(true, std::memory_order_release);

    for (std::thread& writer : writers) {
        writer.join();
    }

    assert(inserted.load(std::memory_order_relaxed) == 1);
    assert(unchanged.load(std::memory_order_relaxed) ==
           writer_count - 1);
    assert(publish_count.load(std::memory_order_relaxed) == 1);
    assert(engine.query_data().size() == 1);

    const auto stored = engine.find_data(key);
    assert(stored.has_value());
    assert(numeric_value(*stored)._value == 42);
}

/*
 * 阶段 9 案例 3：Publisher 只接收实际变化。
 *
 * 相同数值即使 description 或 state 改变也属于 UNCHANGED；只有首次插入和
 * 数值主体变化才能越过去重门。本案例固定 Publisher 看到的值序列为 1、2、3。
 */
void test_publisher_receives_only_value_changes() {
    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    const MonData::MonitorKey key{90, 3, 1, 1};
    std::mutex values_mutex;
    std::vector<std::uint32_t> published_values;

    assert(engine.set_publisher(
        [&](MonData::StoredRecord record) {
            std::lock_guard<std::mutex> lock(values_mutex);
            published_values.push_back(numeric_value(record)._value);
        }));

    const auto inserted = engine.report_count(key, 1, "first");
    const auto same_value =
        engine.report_count(key, 1, "description-changed");
    const auto updated = engine.report_count(key, 2, "second");
    const auto state_only = engine.report_error(key, 2, "fresh-state");
    const auto error_updated = engine.report_error(key, 3, "third");

    assert(inserted._status == MonData::UpdateStatus::INSERTED);
    assert(same_value._status == MonData::UpdateStatus::UNCHANGED);
    assert(updated._status == MonData::UpdateStatus::UPDATED);
    assert(state_only._status == MonData::UpdateStatus::UNCHANGED);
    assert(error_updated._status == MonData::UpdateStatus::UPDATED);

    const std::vector<std::uint32_t> expected{1, 2, 3};
    std::lock_guard<std::mutex> lock(values_mutex);
    assert(published_values == expected);
}

/*
 * 阶段 9 案例 4：Publisher 执行期间可以查询 Engine。
 *
 * Reporter 必须在 Store 解锁后调用 Publisher。回调内部同时执行 find_data()
 * 和 query_data()；若 Publisher 在 Store 锁内执行，本案例会发生自锁死锁。
 */
void test_publisher_can_query_engine() {
    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    const MonData::MonitorKey key{90, 4, 1, 1};
    std::atomic<bool> find_succeeded{false};
    std::atomic<bool> query_succeeded{false};

    assert(engine.set_publisher(
        [&](MonData::StoredRecord record) {
            const auto found = engine.find_data(record._data._key);
            const auto snapshot = engine.query_data();

            find_succeeded.store(
                found.has_value() &&
                    numeric_value(*found)._value == 7,
                std::memory_order_relaxed);
            query_succeeded.store(
                snapshot.size() == 1,
                std::memory_order_relaxed);
        }));

    const auto result = engine.report_count(key, 7, "query-engine");

    assert(result._status == MonData::UpdateStatus::INSERTED);
    assert(find_succeeded.load(std::memory_order_relaxed));
    assert(query_succeeded.load(std::memory_order_relaxed));
}

/*
 * 阶段 9 案例 5：Publisher 可以重新进入 Engine 上报其他 Key。
 *
 * 外层 Publisher 看到 outer_key 后同步调用 report_string(inner_key)。内层写入
 * 会再次进入 Publisher。原子门只限制外层派生一次，不阻止内层正常发布。
 */
void test_publisher_can_reenter_engine_reporter() {
    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    const MonData::MonitorKey outer_key{90, 5, 1, 1};
    const MonData::MonitorKey inner_key{90, 5, 1, 2};
    std::atomic<bool> nested_started{false};
    std::atomic<std::size_t> publish_count{0};
    std::atomic<bool> nested_inserted{false};

    assert(engine.set_publisher(
        [&](MonData::StoredRecord record) {
            publish_count.fetch_add(1, std::memory_order_relaxed);

            if (record._data._key == outer_key &&
                !nested_started.exchange(
                    true,
                    std::memory_order_acq_rel)) {

                const auto nested = engine.report_string(
                    inner_key,
                    "nested",
                    "publisher-reentry");

                nested_inserted.store(
                    nested._status ==
                        MonData::UpdateStatus::INSERTED,
                    std::memory_order_relaxed);
            }
        }));

    const auto outer = engine.report_count(
        outer_key,
        1,
        "outer");

    assert(outer._status == MonData::UpdateStatus::INSERTED);
    assert(nested_inserted.load(std::memory_order_relaxed));
    assert(publish_count.load(std::memory_order_relaxed) == 2);
    assert(engine.query_data().size() == 2);
    assert(engine.find_data(inner_key).has_value());
}

/*
 * 阶段 9 案例 6：注销 Publisher 后不再进入旧回调。
 *
 * 空 Publisher 只关闭后续发布，不停止 Store。注销后的变化仍返回 UPDATED 并
 * 保存新值，但旧 Publisher 的调用次数保持不变。
 */
void test_unregister_stops_future_old_publisher_calls() {
    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    const MonData::MonitorKey key{90, 6, 1, 1};
    std::atomic<std::size_t> old_publisher_count{0};

    assert(engine.set_publisher(
        [&](MonData::StoredRecord) {
            old_publisher_count.fetch_add(
                1,
                std::memory_order_relaxed);
        }));

    const auto inserted = engine.report_count(key, 1, "before-unregister");
    assert(inserted._status == MonData::UpdateStatus::INSERTED);
    assert(old_publisher_count.load(std::memory_order_relaxed) == 1);

    assert(engine.set_publisher({}));

    const auto updated = engine.report_count(key, 2, "after-unregister");
    assert(updated._status == MonData::UpdateStatus::UPDATED);
    assert(old_publisher_count.load(std::memory_order_relaxed) == 1);

    const auto stored = engine.find_data(key);
    assert(stored.has_value());
    assert(numeric_value(*stored)._value == 2);
}

/*
 * 阶段 9 案例 7：Engine 析构自动释放 Reporter 和 Publisher。
 *
 * Publisher 捕获 shared_ptr，外部只保留 weak_ptr。Engine 存活时闭包持有资源；
 * Engine 离开作用域后 MonContext/Reporter/Publisher 自动析构，weak_ptr 必须过期。
 */
void test_engine_destruction_releases_publisher() {
    std::weak_ptr<int> retained;

    {
        Engine engine(MonConfig{"monitor", 9000, 1});
        assert(engine.init() == ENGINESTATE::SUCCESSFUL);

        auto lifetime = std::make_shared<int>(42);
        retained = lifetime;

        assert(engine.set_publisher(
            [lifetime](MonData::StoredRecord) {
                assert(*lifetime == 42);
            }));

        lifetime.reset();
        assert(!retained.expired());

        const auto inserted = engine.report_count(
            {90, 7, 1, 1},
            1,
            "publisher-lifetime");
        assert(inserted._status == MonData::UpdateStatus::INSERTED);
    }

    assert(retained.expired());
}

/*
 * 阶段 9 案例 8：Publisher 注册与 Engine 上报并发。
 *
 * setter 高频替换两个 Publisher，多个 writer 同时写入唯一 Key。每次 INSERTED
 * 必须选择一个完整 Publisher 快照执行，A/B 调用次数之和必须等于写入数。
 * 该案例主要配合 TSan 验证 std::function 注册和复制没有数据竞争。
 */
void test_concurrent_publisher_registration_and_engine_reports() {
    Engine engine(MonConfig{"monitor", 9000, 1});
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);

    constexpr std::size_t writer_count = 4;
    constexpr std::size_t writes_per_writer = 250;
    constexpr std::size_t expected =
        writer_count * writes_per_writer;

    std::atomic<std::size_t> publisher_a_count{0};
    std::atomic<std::size_t> publisher_b_count{0};
    std::atomic<std::size_t> inserted{0};
    std::atomic<bool> start{false};

    Engine::MonitorPublisher publisher_a =
        [&](MonData::StoredRecord) {
            publisher_a_count.fetch_add(
                1,
                std::memory_order_relaxed);
        };

    Engine::MonitorPublisher publisher_b =
        [&](MonData::StoredRecord) {
            publisher_b_count.fetch_add(
                1,
                std::memory_order_relaxed);
        };

    assert(engine.set_publisher(publisher_a));

    std::thread setter([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (std::size_t index = 0; index < 5000; ++index) {
            const bool accepted = engine.set_publisher(
                (index % 2 == 0) ? publisher_a : publisher_b);
            assert(accepted);
        }
    });

    std::vector<std::thread> writers;
    writers.reserve(writer_count);

    for (std::size_t writer = 0;
         writer < writer_count;
         ++writer) {

        writers.emplace_back([&, writer] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (std::size_t index = 0;
                 index < writes_per_writer;
                 ++index) {

                const auto result = engine.report_count(
                    {90,
                     8,
                     static_cast<std::uint32_t>(writer),
                     static_cast<std::uint32_t>(index)},
                    1,
                    "registration-race");

                if (result._status ==
                    MonData::UpdateStatus::INSERTED) {
                    inserted.fetch_add(
                        1,
                        std::memory_order_relaxed);
                }
            }
        });
    }

    start.store(true, std::memory_order_release);

    setter.join();
    for (std::thread& writer : writers) {
        writer.join();
    }

    const std::size_t total_published =
        publisher_a_count.load(std::memory_order_relaxed) +
        publisher_b_count.load(std::memory_order_relaxed);

    assert(inserted.load(std::memory_order_relaxed) == expected);
    assert(total_published == expected);
    assert(engine.query_data().size() == expected);
}

} // namespace

int main() {
    test_multiple_threads_report_different_keys();
    test_multiple_threads_same_key_publish_once();
    test_publisher_receives_only_value_changes();
    test_publisher_can_query_engine();
    test_publisher_can_reenter_engine_reporter();
    test_unregister_stops_future_old_publisher_calls();
    test_engine_destruction_releases_publisher();
    test_concurrent_publisher_registration_and_engine_reports();
    return 0;
}
