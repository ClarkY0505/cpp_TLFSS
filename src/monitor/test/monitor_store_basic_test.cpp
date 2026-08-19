#include "monitor_store.h"

#include <cassert>
#include <cmath>
#include <ctime>
#include <type_traits>

using namespace TLSSMON;
static MonData::MonitorData make_numeric(
      MonData::MonitorKey key,
      std::uint32_t value,
      std::uint32_t state = 2)
  {
      return MonData::MonitorData{
          key,
          "numeric",
          MonData::NumericValue{value, state}
      };
  }


static void test_new_store_is_empty()
{
    MonitorStore store;

    assert(store.size() == 0);
}

static void test_missing_key_is_not_found()
{
    MonitorStore store;

    const auto record = store.find(MonData::MonitorKey{1,0,2,3});

    assert(!record.has_value());
}

static void test_store_is_not_copyable_or_movable()
{
    static_assert(
                  !std::is_copy_constructible_v<MonitorStore>);

    static_assert(
                  !std::is_copy_assignable_v<MonitorStore>);

    static_assert(
                  !std::is_move_constructible_v<MonitorStore>);

    static_assert(
                  !std::is_move_assignable_v<MonitorStore>);
}

static void test_first_record_is_inserted()
  {
      MonitorStore store;

      const MonData::MonitorKey key{1, 0, 2, 3};
      const MonData::MonitorTimestamp timestamp{
          std::chrono::seconds{100}
      };

      const MonData::UpdateResult result =
          store.update(
              make_numeric(key, 42),
              false,
              timestamp);

      assert(result._status == MonData::UpdateStatus::INSERTED);
      assert(result.changed());
      assert(result._record.has_value());

      assert(store.size() == 1);

      const auto stored = store.find(key);

      assert(stored.has_value());
      assert(stored->_data._key == key);
      assert(stored->_data._description == "numeric");
      assert(stored->_changed_at == timestamp);

      assert(
          std::get<MonData::NumericValue>(stored->_data._value)._value
          == 42);
  }

static void test_different_fids_are_distinct_records()
{
    MonitorStore store;

    const MonData::MonitorKey first_key{1, 0, 10, 3};
    const MonData::MonitorKey second_key{1, 0, 11, 3};
    const MonData::MonitorTimestamp timestamp{
        std::chrono::seconds{100}
    };

    const MonData::UpdateResult first =
        store.update(make_numeric(first_key, 42), false, timestamp);

    const MonData::UpdateResult second =
        store.update(make_numeric(second_key, 43), false, timestamp);

    assert(first_key._fid == 10);
    assert(second_key._fid == 11);
    assert(first._status == MonData::UpdateStatus::INSERTED);
    assert(second._status == MonData::UpdateStatus::INSERTED);
    assert(store.size() == 2);
    assert(store.find(first_key).has_value());
    assert(store.find(second_key).has_value());
}


int main()
{
    test_new_store_is_empty();
    test_missing_key_is_not_found();
    test_store_is_not_copyable_or_movable();
    test_first_record_is_inserted();
    test_different_fids_are_distinct_records();

    return 0;
}
