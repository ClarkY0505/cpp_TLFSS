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

const MonData::NumericValue& numeric_value(
    const MonData::StoredRecord& record) {
    return std::get<MonData::NumericValue>(record._data._value);
}

void test_first_nonzero_count_is_inserted_and_published() {
    MonitorStore store;
    MonitorReporter reporter(store);

    std::vector<MonData::StoredRecord> published;
    reporter.set_publisher([&](MonData::StoredRecord record) {
        published.push_back(std::move(record));
    });

    const MonData::MonitorKey key{1, 2, 3, 4};
    const MonData::MonitorTimestamp timestamp = timestamp_at(100);

    const MonData::UpdateResult result =
        reporter.report_count(key, 5, "first", timestamp);

    assert(result._status == MonData::UpdateStatus::INSERTED);
    assert(result.changed());
    assert(result._record.has_value());
    assert(result._record->_data._key == key);
    assert(result._record->_data._description == "first");
    assert(numeric_value(*result._record)._value == 5);
    assert(numeric_value(*result._record)._state == 0);
    assert(result._record->_changed_at == timestamp);

    assert(published.size() == 1);
    assert(published[0]._data._key == key);
    assert(published[0]._data._description == "first");
    assert(numeric_value(published[0])._value == 5);
    assert(numeric_value(published[0])._state == 0);
    assert(published[0]._changed_at == timestamp);

    const auto stored = store.find(key);
    assert(stored.has_value());
    assert(stored->_data._description == "first");
    assert(numeric_value(*stored)._value == 5);
    assert(numeric_value(*stored)._state == 0);
    assert(stored->_changed_at == timestamp);
    assert(store.size() == 1);
}

void test_same_count_is_unchanged_and_not_published_again() {
    MonitorStore store;
    MonitorReporter reporter(store);

    std::vector<MonData::StoredRecord> published;
    reporter.set_publisher([&](MonData::StoredRecord record) {
        published.push_back(std::move(record));
    });

    const MonData::MonitorKey key{2, 3, 4, 5};
    const MonData::MonitorTimestamp first_timestamp = timestamp_at(200);
    const MonData::MonitorTimestamp duplicate_timestamp = timestamp_at(300);

    const MonData::UpdateResult inserted =
        reporter.report_count(key, 7, "original", first_timestamp);
    assert(inserted._status == MonData::UpdateStatus::INSERTED);
    assert(published.size() == 1);

    const MonData::UpdateResult unchanged = reporter.report_count(
        key, 7, "duplicate-metadata", duplicate_timestamp);

    assert(unchanged._status == MonData::UpdateStatus::UNCHANGED);
    assert(!unchanged.changed());
    assert(unchanged._record.has_value());
    assert(unchanged._record->_data._description == "original");
    assert(numeric_value(*unchanged._record)._value == 7);
    assert(numeric_value(*unchanged._record)._state == 0);
    assert(unchanged._record->_changed_at == first_timestamp);

    assert(published.size() == 1);

    const auto stored = store.find(key);
    assert(stored.has_value());
    assert(stored->_data._description == "original");
    assert(numeric_value(*stored)._value == 7);
    assert(numeric_value(*stored)._state == 0);
    assert(stored->_changed_at == first_timestamp);
}

void test_different_count_is_updated_and_published_again() {
    MonitorStore store;
    MonitorReporter reporter(store);

    std::vector<MonData::StoredRecord> published;
    reporter.set_publisher([&](MonData::StoredRecord record) {
        published.push_back(std::move(record));
    });

    const MonData::MonitorKey key{3, 4, 5, 6};
    const MonData::MonitorTimestamp first_timestamp = timestamp_at(400);
    const MonData::MonitorTimestamp changed_timestamp = timestamp_at(500);

    const MonData::UpdateResult inserted =
        reporter.report_count(key, 8, "original", first_timestamp);
    assert(inserted._status == MonData::UpdateStatus::INSERTED);

    const MonData::UpdateResult updated =
        reporter.report_count(key, 9, "changed", changed_timestamp);

    assert(updated._status == MonData::UpdateStatus::UPDATED);
    assert(updated.changed());
    assert(updated._record.has_value());
    assert(updated._record->_data._description == "changed");
    assert(numeric_value(*updated._record)._value == 9);
    assert(numeric_value(*updated._record)._state == 0);
    assert(updated._record->_changed_at == changed_timestamp);

    assert(published.size() == 2);
    assert(published[1]._data._description == "changed");
    assert(numeric_value(published[1])._value == 9);
    assert(numeric_value(published[1])._state == 0);
    assert(published[1]._changed_at == changed_timestamp);

    const auto stored = store.find(key);
    assert(stored.has_value());
    assert(stored->_data._description == "changed");
    assert(numeric_value(*stored)._value == 9);
    assert(numeric_value(*stored)._state == 0);
    assert(stored->_changed_at == changed_timestamp);
    assert(store.size() == 1);
}

void test_initial_zero_count_is_ignored_and_not_published() {
    MonitorStore store;
    MonitorReporter reporter(store);

    std::vector<MonData::StoredRecord> published;
    reporter.set_publisher([&](MonData::StoredRecord record) {
        published.push_back(std::move(record));
    });

    const MonData::MonitorKey key{4, 5, 6, 7};

    const MonData::UpdateResult result =
        reporter.report_count(key, 0, "initial-zero", timestamp_at(600));

    assert(result._status ==
           MonData::UpdateStatus::IGNORED_INITIAL_ZERO);
    assert(!result.changed());
    assert(!result._record.has_value());
    assert(published.empty());
    assert(!store.find(key).has_value());
    assert(store.size() == 0);
}

void test_existing_count_can_return_to_zero_and_is_published() {
    MonitorStore store;
    MonitorReporter reporter(store);

    std::vector<MonData::StoredRecord> published;
    reporter.set_publisher([&](MonData::StoredRecord record) {
        published.push_back(std::move(record));
    });

    const MonData::MonitorKey key{5, 6, 7, 8};
    const MonData::MonitorTimestamp first_timestamp = timestamp_at(700);
    const MonData::MonitorTimestamp zero_timestamp = timestamp_at(800);

    const MonData::UpdateResult inserted =
        reporter.report_count(key, 11, "nonzero", first_timestamp);
    assert(inserted._status == MonData::UpdateStatus::INSERTED);

    const MonData::UpdateResult restored_zero =
        reporter.report_count(key, 0, "restored-zero", zero_timestamp);

    assert(restored_zero._status == MonData::UpdateStatus::UPDATED);
    assert(restored_zero.changed());
    assert(restored_zero._record.has_value());
    assert(restored_zero._record->_data._description == "restored-zero");
    assert(numeric_value(*restored_zero._record)._value == 0);
    assert(numeric_value(*restored_zero._record)._state == 0);
    assert(restored_zero._record->_changed_at == zero_timestamp);

    assert(published.size() == 2);
    assert(published[1]._data._description == "restored-zero");
    assert(numeric_value(published[1])._value == 0);
    assert(numeric_value(published[1])._state == 0);
    assert(published[1]._changed_at == zero_timestamp);

    const auto stored = store.find(key);
    assert(stored.has_value());
    assert(stored->_data._description == "restored-zero");
    assert(numeric_value(*stored)._value == 0);
    assert(numeric_value(*stored)._state == 0);
    assert(stored->_changed_at == zero_timestamp);
    assert(store.size() == 1);
}

} // namespace

int main() {
    test_first_nonzero_count_is_inserted_and_published();
    test_same_count_is_unchanged_and_not_published_again();
    test_different_count_is_updated_and_published_again();
    test_initial_zero_count_is_ignored_and_not_published();
    test_existing_count_can_return_to_zero_and_is_published();
    return 0;
}
