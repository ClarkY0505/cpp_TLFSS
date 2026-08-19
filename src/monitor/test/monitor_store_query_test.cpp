#include "monitor_store.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <variant>

using namespace TLSSMON;

/**
 * 构造数值型监控数据。
 *
 * 所有测试值都使用非零数值，避免受到阶段 3
 * “首次零值门禁”的影响。
 */
static MonData::MonitorData make_numeric(MonData::MonitorKey key, std::uint32_t value)
{
    return MonData::MonitorData{key, "query-fixture", MonData::NumericValue{value,2}};
}

/**
 * 构造固定时间点，方便后续验证快照内容。
 */
static MonData::MonitorTimestamp timestamp_at(std::int64_t seconds)
{
    return MonData::MonitorTimestamp{std::chrono::seconds{seconds}};
}

static const MonData::NumericValue& numeric_value(const MonData::StoredRecord& record)
{
    return std::get<MonData::NumericValue>(record._data._value);
}

/**
 * 向 Store 中插入五条测试记录。
 *
 * 插入顺序故意不同于 MonitorKey 的排序顺序，
 * 用于验证 query() 返回的是 map 顺序，而不是插入顺序。
 */
static void populate_store(MonitorStore& store)
{
    struct FixtureEntry {
        MonData::MonitorKey key;
        std::uint32_t value;
        std::int64_t seconds;
    };

    const FixtureEntry entries[] = {
        // 插入顺序 1
        {{2, 1, 20, 2}, 50, 500},

        // 插入顺序 2
        {{1, 1, 20, 3}, 30, 300},

        // 插入顺序 3
        {{1, 0, 30, 2}, 20, 200},

        // 插入顺序 4
        {{1, 0, 10, 3}, 10, 100},

        // 插入顺序 5
        {{2, 0, 10, 1}, 40, 400}
    };

    for (const FixtureEntry& entry : entries) {
        const MonData::UpdateResult result =
            store.update(
                         make_numeric(
                                      entry.key,
                                      entry.value),
                         false,
                         timestamp_at(entry.seconds));

        // 测试夹具本身必须成功插入。
        assert(result._status == MonData::UpdateStatus::INSERTED);
    }

    assert(store.size() == 5);
}

static void test_query_on_empty_store_returns_empty()
{
    MonitorStore store;

    // 使用 query() 的默认空过滤器。
    const auto records = store.query();

    assert(records.empty());
}

static void test_empty_filter_returns_all_records()
{
    MonitorStore store;

    populate_store(store);

    // 未传入过滤器，等价于四个字段均为 nullopt。
    const auto records = store.query();

    assert(records.size() == 5);
}

static void test_query_preserves_key_order()
{
    MonitorStore store;

    // populate_store() 的插入顺序是乱序的。
    populate_store(store);

    const auto records = store.query();

    // query() 必须按照：
    //
    // mid → level → fid → eid
    //
    // 返回记录。
    const MonData::MonitorKey expected_keys[] = {
        {1, 0, 10, 3},
        {1, 0, 30, 2},
        {1, 1, 20, 3},
        {2, 0, 10, 1},
        {2, 1, 20, 2}
    };

    const std::uint32_t expected_values[] = {
        10,
        20,
        30,
        40,
        50
    };

    assert(records.size() == 5);

    for (std::size_t index = 0;
         index < records.size();
         ++index) {

        assert(
               records[index]._data._key
               == expected_keys[index]);

        assert(
               numeric_value(records[index])._value
               == expected_values[index]);
    }
}

static void assert_query_keys(
                              const std::vector<MonData::StoredRecord>& records,
                              const MonData::MonitorKey* expected_keys,
                              std::size_t expected_count)
{
    assert(records.size() == expected_count);

    for (std::size_t index = 0;
         index < expected_count;
         ++index) {

        assert(
               records[index]._data._key
               == expected_keys[index]);
    }
}

// 1. module_id 独立过滤
static void test_query_filters_by_module_id()
{
    MonitorStore store;
    populate_store(store);

    MonData::MonitorFilter filter;
    filter.module_id = 1;

    const auto records = store.query(filter);

    const MonData::MonitorKey expected_keys[] = {
        {1, 0, 10, 3},
        {1, 0, 30, 2},
        {1, 1, 20, 3}
    };

    assert_query_keys(records, expected_keys, 3);
}

// 2. level 独立过滤
static void test_query_filters_by_level()
{
    MonitorStore store;
    populate_store(store);

    MonData::MonitorFilter filter;
    filter.level = 0;

    const auto records = store.query(filter);

    const MonData::MonitorKey expected_keys[] = {
        {1, 0, 10, 3},
        {1, 0, 30, 2},
        {2, 0, 10, 1}
    };

    assert_query_keys(records, expected_keys, 3);
}

// 3. function_id 独立过滤
//  这里使用的是之前统一后的 fid 对应字段：
static void test_query_filters_by_function_id()
{
    MonitorStore store;
    populate_store(store);

    MonData::MonitorFilter filter;
    filter.function_id = 10;

    const auto records = store.query(filter);

    const MonData::MonitorKey expected_keys[] = {
        {1, 0, 10, 3},
        {2, 0, 10, 1}
    };

    assert_query_keys(records, expected_keys, 2);
}

// 4. event_id 独立过滤
static void test_query_filters_by_event_id()
{
    MonitorStore store;
    populate_store(store);

    MonData::MonitorFilter filter;
    filter.event_id = 3;

    const auto records = store.query(filter);

    const MonData::MonitorKey expected_keys[] = {
        {1, 0, 10, 3},
        {1, 1, 20, 3}
    };

    assert_query_keys(records, expected_keys, 2);
}

// 5.测试多字段 AND 组合
static void test_query_combines_filters_with_and()
{
    MonitorStore store;
    populate_store(store);

    MonData::MonitorFilter filter;

    // 单独 module_id == 1 时有三条记录。
    filter.module_id = 1;

    // 单独 level == 0 时也有三条记录。
    filter.level = 0;

    /*
     * 两个条件组合后，记录必须同时满足：
     *
     * _mid == 1 && _level == 0
     */
    const auto records = store.query(filter);

    const MonData::MonitorKey expected_keys[] = {
        {1, 0, 10, 3},
        {1, 0, 30, 2}
    };

    assert_query_keys(records, expected_keys, 2);
}

// 测试无匹配结果
static void test_query_returns_empty_when_no_record_matches()
{
    MonitorStore store;
    populate_store(store);

    MonData::MonitorFilter filter;

    // 测试夹具中不存在 module_id == 999 的记录。
    filter.module_id = 999;

    const auto records = store.query(filter);

    /*
     * 无匹配不是错误，也不应返回占位记录，
     * 而是返回一个有效的空快照。
     */
    assert(records.empty());
}

/**
 * 验证 query() 返回独立快照。
 *
 * 1. 修改快照不影响 Store；
 * 2. Store 销毁后快照仍然有效。
 */
static void test_query_returns_independent_snapshot()
{
    /*
     * snapshot 定义在 Store 作用域之外，
     * 用于验证 Store 销毁后数据仍然有效。
     */
    std::vector<MonData::StoredRecord> snapshot;

    {
        MonitorStore store;
        populate_store(store);

        /*
         * query() 返回 vector<StoredRecord>，
         * 每条 StoredRecord 都应当是 Store 内部记录的副本。
         */
        snapshot = store.query();

        assert(snapshot.size() == 5);

        /*
         * map 排序后的第一条记录是：
         *
         * key         = {1, 0, 10, 3}
         * description = "query-fixture"
         * value       = 10
         * timestamp   = 100 秒
         *
         * 这里只修改快照。
         */
        snapshot[0]._data._description = "snapshot-only";

        std::get<MonData::NumericValue>(
                                        snapshot[0]._data._value
                                       )._value = 999;

        snapshot[0]._changed_at = timestamp_at(999);

        /*
         * 再次查询 Store。
         *
         * 如果第一次 query() 返回的是独立副本，
         * Store 内部记录就仍然保持原始内容。
         *
         * 第二次 query() 也验证第一次查询结束后
         * mutex 已经释放，可以再次进入 query()。
         */
        const auto current_records = store.query();

        assert(current_records.size() == 5);

        assert(
               current_records[0]._data._description
               == "query-fixture");

        assert(
               numeric_value(current_records[0])._value
               == 10);

        assert(
               current_records[0]._changed_at
               == timestamp_at(100));
    }

    /*
     * 执行到这里时，store 已经析构。
     *
     * snapshot 仍然必须拥有完整、有效的数据，
     * 并保留之前对快照自身所做的修改。
     */
    assert(snapshot.size() == 5);

    assert(
           snapshot[0]._data._description
           == "snapshot-only");

    assert(
           numeric_value(snapshot[0])._value
           == 999);

    assert(
           snapshot[0]._changed_at
           == timestamp_at(999));
}


int main()
{
    test_query_on_empty_store_returns_empty();
    test_empty_filter_returns_all_records();
    test_query_preserves_key_order();

    test_query_filters_by_module_id();
    test_query_filters_by_level();
    test_query_filters_by_function_id();
    test_query_filters_by_event_id();

    test_query_combines_filters_with_and();
    test_query_returns_empty_when_no_record_matches();

    test_query_returns_independent_snapshot();
    return 0;
}

