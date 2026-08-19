#include "monitor_reporter.h"
#include "monitor_store.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>
using namespace TLSSMON;

namespace {

MonData::MonitorTimestamp timestamp_at(std::int64_t seconds) {
    return MonData::MonitorTimestamp{std::chrono::seconds{seconds}};
}

const MonData::NumericValue &
numeric_value(const MonData::StoredRecord &record) {

    return std::get<MonData::NumericValue>(record._data._value);
}

/*
 * 首次错误零值也必须插入。
 *
 * 这同时证明 report_error() 内部使用 force=true。
 */
void test_initial_zero_error_is_inserted_and_published() {
    MonitorStore store;
    MonitorReporter reporter(store);

    std::vector<MonData::StoredRecord> published;

    reporter.set_publisher([&](MonData::StoredRecord record) {
                           published.push_back(std::move(record));
                           });

    const MonData::MonitorKey key{
        1,  // mid
            7,  // level
            23, // fid
            45  // eid
    };

    const MonData::MonitorTimestamp timestamp = timestamp_at(100);

    const MonData::UpdateResult result =
        reporter.report_error(key, 0, "first-error", timestamp);

    assert(result._status == MonData::UpdateStatus::INSERTED);

    assert(result.changed());
    assert(result._record.has_value());

    assert(result._record->_data._key == key);
    assert(result._record->_data._description == "first-error");

    assert(numeric_value(*result._record)._value == 0);
    assert(numeric_value(*result._record)._state == 2);

    assert(result._record->_changed_at == timestamp);

    /*
     * INSERTED 必须发布一次。
     */
    assert(published.size() == 1);

    assert(published[0]._data._key == key);
    assert(published[0]._data._description == "first-error");

    assert(numeric_value(published[0])._value == 0);
    assert(numeric_value(published[0])._state == 2);

    assert(published[0]._changed_at == timestamp);

    /*
     * Store 中也必须保存完全一致的记录。
     */
    const auto stored = store.find(key);

    assert(stored.has_value());
    assert(stored->_data._key == key);
    assert(stored->_data._description == "first-error");

    assert(numeric_value(*stored)._value == 0);
    assert(numeric_value(*stored)._state == 2);

    assert(stored->_changed_at == timestamp);
    assert(store.size() == 1);
}

/*
 * force=true 只能绕过首次零值门禁，
 * 不能绕过相同数值的去重规则。
 */
void test_same_error_is_unchanged_and_not_published_again() {
    MonitorStore store;
    MonitorReporter reporter(store);

    std::vector<MonData::StoredRecord> published;

    reporter.set_publisher([&](MonData::StoredRecord record) {
                           published.push_back(std::move(record));
                           });

    const MonData::MonitorKey key{2, 8, 24, 46};

    const MonData::MonitorTimestamp first_timestamp = timestamp_at(200);

    const MonData::MonitorTimestamp duplicate_timestamp = timestamp_at(300);

    const MonData::UpdateResult inserted =
        reporter.report_error(key, 0, "original-error", first_timestamp);

    assert(inserted._status == MonData::UpdateStatus::INSERTED);

    assert(published.size() == 1);

    /*
     * 数值仍然是零，但修改 description 和 timestamp。
     *
     * 因为数值相同，所以必须是 UNCHANGED。
     */
    const MonData::UpdateResult unchanged =
        reporter.report_error(key, 0, "duplicate-error", duplicate_timestamp);

    assert(unchanged._status == MonData::UpdateStatus::UNCHANGED);

    assert(!unchanged.changed());
    assert(unchanged._record.has_value());

    /*
     * UNCHANGED 保留首次记录的元数据。
     */
    assert(unchanged._record->_data._description == "original-error");

    assert(numeric_value(*unchanged._record)._value == 0);
    assert(numeric_value(*unchanged._record)._state == 2);

    assert(unchanged._record->_changed_at == first_timestamp);

    /*
     * 相同值不能再次发布。
     */
    assert(published.size() == 1);

    const auto stored = store.find(key);

    assert(stored.has_value());

    assert(stored->_data._description == "original-error");

    assert(numeric_value(*stored)._value == 0);
    assert(numeric_value(*stored)._state == 2);

    assert(stored->_changed_at == first_timestamp);
}

/*
 * 已存在的错误数值发生变化时，
 * 必须更新 Store 并再次发布。
 */
void test_changed_error_is_updated_and_published_again() {
    MonitorStore store;
    MonitorReporter reporter(store);

    std::vector<MonData::StoredRecord> published;

    reporter.set_publisher([&](MonData::StoredRecord record) {
                           published.push_back(std::move(record));
                           });

    const MonData::MonitorKey key{3, 9, 25, 47};

    const MonData::MonitorTimestamp first_timestamp = timestamp_at(400);

    const MonData::MonitorTimestamp changed_timestamp = timestamp_at(500);

    const MonData::UpdateResult inserted =
        reporter.report_error(key, 5, "first-value", first_timestamp);

    assert(inserted._status == MonData::UpdateStatus::INSERTED);

    const MonData::UpdateResult updated =
        reporter.report_error(key, 9, "changed-value", changed_timestamp);

    assert(updated._status == MonData::UpdateStatus::UPDATED);

    assert(updated.changed());
    assert(updated._record.has_value());

    assert(updated._record->_data._description == "changed-value");

    assert(numeric_value(*updated._record)._value == 9);
    assert(numeric_value(*updated._record)._state == 2);

    assert(updated._record->_changed_at == changed_timestamp);

    /*
     * 首次插入发布一次，变化后再发布一次。
     */
    assert(published.size() == 2);

    assert(published[1]._data._description == "changed-value");

    assert(numeric_value(published[1])._value == 9);
    assert(numeric_value(published[1])._state == 2);

    assert(published[1]._changed_at == changed_timestamp);

    const auto stored = store.find(key);

    assert(stored.has_value());

    assert(stored->_data._description == "changed-value");

    assert(numeric_value(*stored)._value == 9);
    assert(numeric_value(*stored)._state == 2);

    assert(stored->_changed_at == changed_timestamp);
    assert(store.size() == 1);
}

/*
 * 已存在的非零错误恢复为零时，
 * 零值属于实际变化，必须更新和发布。
 */
void test_existing_error_can_return_to_zero() {
    MonitorStore store;
    MonitorReporter reporter(store);

    std::vector<MonData::StoredRecord> published;

    reporter.set_publisher([&](MonData::StoredRecord record) {
                           published.push_back(std::move(record));
                           });

    const MonData::MonitorKey key{4, 10, 26, 48};

    const MonData::MonitorTimestamp first_timestamp = timestamp_at(600);

    const MonData::MonitorTimestamp zero_timestamp = timestamp_at(700);

    const MonData::UpdateResult inserted =
        reporter.report_error(key, 12, "nonzero-error", first_timestamp);

    assert(inserted._status == MonData::UpdateStatus::INSERTED);

    const MonData::UpdateResult restored_zero =
        reporter.report_error(key, 0, "restored-zero", zero_timestamp);

    assert(restored_zero._status == MonData::UpdateStatus::UPDATED);

    assert(restored_zero.changed());
    assert(restored_zero._record.has_value());

    assert(restored_zero._record->_data._description == "restored-zero");

    assert(numeric_value(*restored_zero._record)._value == 0);
    assert(numeric_value(*restored_zero._record)._state == 2);

    assert(restored_zero._record->_changed_at == zero_timestamp);

    assert(published.size() == 2);

    assert(numeric_value(published[1])._value == 0);
    assert(numeric_value(published[1])._state == 2);

    assert(published[1]._changed_at == zero_timestamp);

    const auto stored = store.find(key);

    assert(stored.has_value());
    assert(numeric_value(*stored)._value == 0);
    assert(numeric_value(*stored)._state == 2);

    assert(stored->_changed_at == zero_timestamp);
}

/*
 * 阶段 4 不实现 M8 的错误码映射和元数据补全。
 *
 * 调用方传入的完整 Key 和 description 必须原样保存。
 */
void test_error_does_not_apply_m8_metadata_mapping() {
    MonitorStore store;
    MonitorReporter reporter(store);

    std::vector<MonData::StoredRecord> published;

    reporter.set_publisher([&](MonData::StoredRecord record) {
                           published.push_back(std::move(record));
                           });

    const MonData::MonitorKey caller_key{
        100, // mid
            37,  // 调用方明确指定的 level
            200, // fid
            999  // eid
    };

    const MonData::MonitorTimestamp timestamp = timestamp_at(800);

    const MonData::UpdateResult result =
        reporter.report_error(caller_key, 17, "caller-description", timestamp);

    assert(result._status == MonData::UpdateStatus::INSERTED);

    assert(result._record.has_value());

    /*
     * Key 的四个字段必须原样保留。
     */
    assert(result._record->_data._key == caller_key);

    assert(result._record->_data._key._mid == 100);
    assert(result._record->_data._key._level == 37);
    assert(result._record->_data._key._fid == 200);
    assert(result._record->_data._key._eid == 999);

    /*
     * description 不能根据 eid 自动替换。
     */
    assert(result._record->_data._description == "caller-description");

    assert(numeric_value(*result._record)._value == 17);
    assert(numeric_value(*result._record)._state == 2);

    assert(published.size() == 1);

    assert(published[0]._data._key == caller_key);

    assert(published[0]._data._description == "caller-description");
}

/*
 * M4 数值去重只比较 NumericValue::_value。
 *
 * state 从 0 变为 2 本身不能触发更新。
 */
void test_state_change_alone_does_not_bypass_numeric_dedup() {
    MonitorStore store;
    MonitorReporter reporter(store);

    std::vector<MonData::StoredRecord> published;

    reporter.set_publisher([&](MonData::StoredRecord record) {
                           published.push_back(std::move(record));
                           });

    const MonData::MonitorKey key{5, 11, 27, 49};

    const MonData::MonitorTimestamp count_timestamp = timestamp_at(900);

    const MonData::MonitorTimestamp error_timestamp = timestamp_at(1000);

    /*
     * report_count() 以 state=0 保存数值 20。
     */
    const MonData::UpdateResult count_inserted =
        reporter.report_count(key, 20, "count-value", count_timestamp);

    assert(count_inserted._status == MonData::UpdateStatus::INSERTED);

    assert(count_inserted._record.has_value());
    assert(numeric_value(*count_inserted._record)._value == 20);
    assert(numeric_value(*count_inserted._record)._state == 0);

    assert(published.size() == 1);

    /*
     * report_error() 构造 state=2，但数值仍然是 20。
     *
     * M4 只比较数值主体，因此结果必须是 UNCHANGED。
     */
    const MonData::UpdateResult error_unchanged =
        reporter.report_error(key, 20, "error-value", error_timestamp);

    assert(error_unchanged._status == MonData::UpdateStatus::UNCHANGED);

    assert(!error_unchanged.changed());
    assert(error_unchanged._record.has_value());

    /*
     * UNCHANGED 保留原来的 count 记录。
     */
    assert(error_unchanged._record->_data._description == "count-value");

    assert(numeric_value(*error_unchanged._record)._value == 20);
    assert(numeric_value(*error_unchanged._record)._state == 0);

    assert(error_unchanged._record->_changed_at == count_timestamp);

    /*
     * state 和 description 的变化不能触发发布。
     */
    assert(published.size() == 1);

    const auto stored = store.find(key);

    assert(stored.has_value());

    assert(stored->_data._description == "count-value");
    assert(numeric_value(*stored)._value == 20);
    assert(numeric_value(*stored)._state == 0);

    assert(stored->_changed_at == count_timestamp);
}

} // namespace

int main() {
    test_initial_zero_error_is_inserted_and_published();
    test_same_error_is_unchanged_and_not_published_again();
    test_changed_error_is_updated_and_published_again();
    test_existing_error_can_return_to_zero();
    test_error_does_not_apply_m8_metadata_mapping();
    test_state_change_alone_does_not_bypass_numeric_dedup();

    return 0;
}
