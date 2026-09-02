#include "monitor_store.h"

#include <algorithm>
#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>
#include <utility>
#include <variant>

using namespace TLSSMON;

static MonData::MonitorData make_string(MonData::MonitorKey key, std::string value, std::string des = "string"){
    return MonData::MonitorData{key, std::move(des),std::move(value)};
}

static MonData::MonitorTimestamp timestamp_at(std::int64_t seconds){
    return MonData::MonitorTimestamp{std::chrono::seconds{seconds}};
}

/**
 * 从 StoredRecord 的 variant 中取得字符串。
 *
 * 仅在已经确认记录保存的是字符串时调用；
 * 类型不匹配时 std::get 会抛出 std::bad_variant_access。
 */
static const std::string& string_value(const MonData::StoredRecord& record)
{
    return std::get<std::string>(record._data._value);
}

static void test_first_string_is_inserted()
{
    MonitorStore store;

    const MonData::MonitorKey key{1, 0, 10, 1};
    const auto timestamp = timestamp_at(100);

    const MonData::UpdateResult result =
        store.update(
                     make_string(
                                 key,
                                 "sensor_ok",
                                 "first"),
                     false,
                     timestamp);

    // 首次字符串必须正常插入。
    assert(
           result._status
           == MonData::UpdateStatus::INSERTED);

    // INSERTED 表示 Store 内容发生了变化。
    assert(result.changed());

    // 返回结果中应当包含插入后的记录副本。
    assert(result._record.has_value());

    // Store 中只存在当前这一条记录。
    assert(store.size() == 1);

    const auto stored = store.find(key);

    // 可以通过完整主键找到记录。
    assert(stored.has_value());

    // 字符串内容必须完整保存。
    assert(string_value(*stored) == "sensor_ok");

    // 描述必须保存。
    assert(stored->_data._description == "first");

    // 首次插入时间必须等于调用方传入的时间。
    assert(stored->_changed_at == timestamp);
}

static void test_empty_string_is_inserted()
{
    MonitorStore store;

    const MonData::MonitorKey key{1, 0, 10, 2};
    const auto timestamp = timestamp_at(200);

    const MonData::UpdateResult result =
        store.update(
                     make_string(
                                 key,
                                 "",
                                 "empty"),
                     false,
                     timestamp);

    // 空字符串不能被当成数值零忽略。
    assert(
           result._status
           == MonData::UpdateStatus::INSERTED);

    assert(result.changed());
    assert(result._record.has_value());
    assert(store.size() == 1);

    const auto stored = store.find(key);

    assert(stored.has_value());

    // 确认保存的是合法空字符串。
    assert(string_value(*stored).empty());

    // 描述和时间戳也必须完整保存。
    assert(stored->_data._description == "empty");
    assert(stored->_changed_at == timestamp);
}

static void test_same_string_preserves_original_record()
{
    MonitorStore store;

    const MonData::MonitorKey key{1, 0, 10, 3};
    const auto first_time = timestamp_at(100);
    const auto second_time = timestamp_at(200);

    // 首次插入字符串。
    const MonData::UpdateResult first =
        store.update(
                     make_string(
                                 key,
                                 "sensor_ok",
                                 "original"),
                     false,
                     first_time);

    // 再次提交相同字符串，但修改描述和时间。
    const MonData::UpdateResult second =
        store.update(
                     make_string(
                                 key,
                                 "sensor_ok",
                                 "changed-description"),
                     false,
                     second_time);

    assert(
           first._status
           == MonData::UpdateStatus::INSERTED);

    // 字符串内容相同，第二次写入不得更新。
    assert(
           second._status
           == MonData::UpdateStatus::UNCHANGED);

    assert(!second.changed());
    assert(second._record.has_value());

    // 返回的记录副本必须是第一次保存的记录。
    assert(
           string_value(*second._record)
           == "sensor_ok");

    assert(
           second._record->_data._description
           == "original");

    assert(
           second._record->_changed_at
           == first_time);

    // 相同 Key 不得产生第二条记录。
    assert(store.size() == 1);

    const auto stored = store.find(key);

    assert(stored.has_value());

    // Store 内部同样必须保留第一次的数据。
    assert(string_value(*stored) == "sensor_ok");
    assert(stored->_data._description == "original");
    assert(stored->_changed_at == first_time);
}

static void test_changed_string_updates_record()
{
    MonitorStore store;

    const MonData::MonitorKey key{1, 0, 10, 4};
    const auto first_time = timestamp_at(100);
    const auto second_time = timestamp_at(200);

    // 首次插入原字符串。
    const MonData::UpdateResult first =
        store.update(
                     make_string(
                                 key,
                                 "sensor_ok",
                                 "original"),
                     false,
                     first_time);

    // 使用相同 Key 提交不同字符串。
    //
    // 两个字符串都有 "sensor_" 前缀，用于确保后续实现
    // 比较完整字符串，而不是只比较前几个字符。
    const MonData::UpdateResult second =
        store.update(
                     make_string(
                                 key,
                                 "sensor_bad",
                                 "changed"),
                     false,
                     second_time);

    assert(
           first._status
           == MonData::UpdateStatus::INSERTED);

    // 字符串内容发生变化，必须更新。
    assert(
           second._status
           == MonData::UpdateStatus::UPDATED);

    assert(second.changed());
    assert(second._record.has_value());

    // 返回记录必须包含新字符串。
    assert(
           string_value(*second._record)
           == "sensor_bad");

    // UPDATED 必须整体替换描述。
    assert(
           second._record->_data._description
           == "changed");

    // UPDATED 必须更新时间。
    assert(
           second._record->_changed_at
           == second_time);

    // 相同 Key 更新后仍然只有一条记录。
    assert(store.size() == 1);

    const auto stored = store.find(key);

    assert(stored.has_value());
    assert(string_value(*stored) == "sensor_bad");
    assert(stored->_data._description == "changed");
    assert(stored->_changed_at == second_time);
}

static void test_force_does_not_change_string_dedup()
  {
      MonitorStore store;

      const MonData::MonitorKey key{1, 0, 10, 5};
      const auto first_time = timestamp_at(100);
      const auto second_time = timestamp_at(200);

      // force=true 不会阻止首次字符串正常插入。
      const MonData::UpdateResult first =
          store.update(
              make_string(
                  key,
                  "sensor_ok",
                  "original"),
              true,
              first_time);

      // 已存在相同字符串时，即使 force=true，
      // 仍然应当按照字符串内容去重。
      const MonData::UpdateResult second =
          store.update(
              make_string(
                  key,
                  "sensor_ok",
                  "forced"),
              true,
              second_time);

      assert(
          first._status
          == MonData::UpdateStatus::INSERTED);

      assert(first.changed());

      // force 不能强制覆盖内容相同的字符串。
      assert(
          second._status
          == MonData::UpdateStatus::UNCHANGED);

      assert(!second.changed());
      assert(second._record.has_value());

      // 返回副本必须保留第一次记录。
      assert(
          string_value(*second._record)
          == "sensor_ok");

      assert(
          second._record->_data._description
          == "original");

      assert(
          second._record->_changed_at
          == first_time);

      assert(store.size() == 1);

      const auto stored = store.find(key);

      assert(stored.has_value());
      assert(string_value(*stored) == "sensor_ok");
      assert(stored->_data._description == "original");
      assert(stored->_changed_at == first_time);
  }


int main(){
    test_first_string_is_inserted();
    test_empty_string_is_inserted();
    test_same_string_preserves_original_record();
    test_changed_string_updates_record();
    test_force_does_not_change_string_dedup();
    return 0;
}
