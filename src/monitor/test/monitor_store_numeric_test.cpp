#include "monitor_store.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>

using namespace TLSSMON;

static MonData::MonitorData make_numeric(MonData::MonitorKey key, std::uint32_t value,
                                         std::uint32_t state = 2, std::string description = "numeric")
{
    return MonData::MonitorData{key, std::move(description), MonData::NumericValue{value, state}};
}

static MonData::MonitorTimestamp timestamp_at(std::int64_t seconds)
{
    return MonData::MonitorTimestamp{std::chrono::seconds{seconds}};
}

static const MonData::NumericValue& numeric_value(const MonData::StoredRecord& record)
{
    return std::get<MonData::NumericValue>(record._data._value);
}

static void test_initial_zero_without_force_is_ignored()
{
    MonitorStore store;

    const MonData::MonitorKey key{1, 0, 10, 1};

    const MonData::UpdateResult result =
        store.update(
                     make_numeric(key, 0),
                     false,
                     timestamp_at(100));

    assert(
           result._status
           == MonData::UpdateStatus::IGNORED_INITIAL_ZERO);

    assert(!result.changed());
    assert(!result._record.has_value());
    assert(store.size() == 0);
    assert(!store.find(key).has_value());
}

static void test_force_inserts_initial_zero()
{
    MonitorStore store;

    const MonData::MonitorKey key{1, 0, 10, 2};
    const auto timestamp = timestamp_at(100);

    const MonData::UpdateResult result =
        store.update(
                     make_numeric(key, 0),
                     true,
                     timestamp);

    assert(
           result._status
           == MonData::UpdateStatus::INSERTED);

    assert(result.changed());
    assert(result._record.has_value());
    assert(store.size() == 1);

    const auto stored = store.find(key);

    assert(stored.has_value());
    assert(numeric_value(*stored)._value == 0);
    assert(stored->_changed_at == timestamp);
}

static void test_changed_numeric_value_updates_record()
{
    MonitorStore store;

    const MonData::MonitorKey key{1, 0, 10, 3};
    const auto first_time = timestamp_at(100);
    const auto second_time = timestamp_at(200);

    const auto first =
        store.update(
                     make_numeric(key, 42, 2, "first"),
                     false,
                     first_time);

    const auto second =
        store.update(
                     make_numeric(key, 43, 1, "second"),
                     false,
                     second_time);

    assert(
           first._status
           == MonData::UpdateStatus::INSERTED);

    assert(
           second._status
           == MonData::UpdateStatus::UPDATED);

    assert(second.changed());
    assert(second._record.has_value());
    assert(store.size() == 1);

    const auto stored = store.find(key);

    assert(stored.has_value());
    assert(numeric_value(*stored)._value == 43);
    assert(numeric_value(*stored)._state == 1);
    assert(stored->_data._description == "second");
    assert(stored->_changed_at == second_time);
}

static void test_same_numeric_value_preserves_original_record()
{
    MonitorStore store;

    const MonData::MonitorKey key{1, 0, 10, 4};
    const auto first_time = timestamp_at(100);
    const auto second_time = timestamp_at(200);

    store.update(
                 make_numeric(key, 42, 2, "original"),
                 false,
                 first_time);

    const auto result =
        store.update(
                     make_numeric(key, 42, 0, "changed-metadata"),
                     false,
                     second_time);

    assert(
           result._status
           == MonData::UpdateStatus::UNCHANGED);

    assert(!result.changed());
    assert(result._record.has_value());
    assert(store.size() == 1);

    const auto stored = store.find(key);

    assert(stored.has_value());
    assert(numeric_value(*stored)._value == 42);

    // state、description 和时间戳都必须保留旧值。
    assert(numeric_value(*stored)._state == 2);
    assert(stored->_data._description == "original");
    assert(stored->_changed_at == first_time);
}

static void test_existing_nonzero_can_change_to_zero()
{
    MonitorStore store;

    const MonData::MonitorKey key{1, 0, 10, 5};
    const auto first_time = timestamp_at(100);
    const auto second_time = timestamp_at(200);

    store.update(
                 make_numeric(key, 9),
                 false,
                 first_time);

    const auto result =
        store.update(
                     make_numeric(key, 0),
                     false,
                     second_time);

    assert(
           result._status
           == MonData::UpdateStatus::UPDATED);

    assert(result.changed());
    assert(store.size() == 1);

    const auto stored = store.find(key);

    assert(stored.has_value());
    assert(numeric_value(*stored)._value == 0);
    assert(stored->_changed_at == second_time);
}

static void test_force_does_not_update_same_existing_value()
{
    MonitorStore store;

    const MonData::MonitorKey key{1, 0, 10, 6};
    const auto first_time = timestamp_at(100);
    const auto second_time = timestamp_at(200);

    store.update(
                 make_numeric(key, 7, 2, "original"),
                 false,
                 first_time);

    const auto result =
        store.update(
                     make_numeric(key, 7, 1, "forced"),
                     true,
                     second_time);

    assert(
           result._status
           == MonData::UpdateStatus::UNCHANGED);

    const auto stored = store.find(key);

    assert(stored.has_value());
    assert(numeric_value(*stored)._state == 2);
    assert(stored->_data._description == "original");
    assert(stored->_changed_at == first_time);
}


int main(){

    test_initial_zero_without_force_is_ignored();
    test_force_inserts_initial_zero();
    test_changed_numeric_value_updates_record();
    test_same_numeric_value_preserves_original_record();
    test_existing_nonzero_can_change_to_zero();
    test_force_does_not_update_same_existing_value();
}
