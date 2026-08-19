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

const std::string &string_value(const MonData::StoredRecord &record) {

  return std::get<std::string>(record._data._value);
}

MonData::MonitorData make_string_data(MonData::MonitorKey key,
                                      std::string value,
                                      std::string description) {

  return MonData::MonitorData{std::move(key), std::move(description),
                              std::move(value)};
}

/*
 * 首次字符串必须保存并发布一次。
 */
void test_first_string_is_inserted_and_published() {
  MonitorStore store;
  MonitorReporter reporter(store);

  std::vector<MonData::StoredRecord> published;

  reporter.set_publisher([&](MonData::StoredRecord record) {
    published.push_back(std::move(record));
  });

  const MonData::MonitorKey key{1, 2, 3, 4};

  const MonData::MonitorTimestamp timestamp = timestamp_at(100);

  const MonData::UpdateResult result =
      reporter.report_string(key, "ready", "first-string", timestamp);

  assert(result._status == MonData::UpdateStatus::INSERTED);

  assert(result.changed());
  assert(result._record.has_value());

  assert(result._record->_data._key == key);

  assert(result._record->_data._description == "first-string");

  assert(string_value(*result._record) == "ready");

  assert(result._record->_changed_at == timestamp);

  /*
   * INSERTED 必须发布一次。
   */
  assert(published.size() == 1);

  assert(published[0]._data._key == key);

  assert(published[0]._data._description == "first-string");

  assert(string_value(published[0]) == "ready");

  assert(published[0]._changed_at == timestamp);

  /*
   * Store 中保存的记录必须相同。
   */
  const auto stored = store.find(key);

  assert(stored.has_value());

  assert(stored->_data._key == key);

  assert(stored->_data._description == "first-string");

  assert(string_value(*stored) == "ready");

  assert(stored->_changed_at == timestamp);

  assert(store.size() == 1);
}

/*
 * 相同字符串必须返回 UNCHANGED，
 * 不更新时间、描述和 Publisher 调用次数。
 */
void test_same_string_is_unchanged_and_not_published_again() {
  MonitorStore store;
  MonitorReporter reporter(store);

  std::vector<MonData::StoredRecord> published;

  reporter.set_publisher([&](MonData::StoredRecord record) {
    published.push_back(std::move(record));
  });

  const MonData::MonitorKey key{2, 3, 4, 5};

  const MonData::MonitorTimestamp first_timestamp = timestamp_at(200);

  const MonData::MonitorTimestamp duplicate_timestamp = timestamp_at(300);

  const MonData::UpdateResult inserted = reporter.report_string(
      key, "same-value", "original-description", first_timestamp);

  assert(inserted._status == MonData::UpdateStatus::INSERTED);

  assert(published.size() == 1);

  const MonData::UpdateResult unchanged = reporter.report_string(
      key, "same-value", "duplicate-description", duplicate_timestamp);

  assert(unchanged._status == MonData::UpdateStatus::UNCHANGED);

  assert(!unchanged.changed());
  assert(unchanged._record.has_value());

  /*
   * UNCHANGED 保留首次写入的数据。
   */
  assert(string_value(*unchanged._record) == "same-value");

  assert(unchanged._record->_data._description == "original-description");

  assert(unchanged._record->_changed_at == first_timestamp);

  /*
   * 相同字符串不能重复发布。
   */
  assert(published.size() == 1);

  const auto stored = store.find(key);

  assert(stored.has_value());

  assert(string_value(*stored) == "same-value");

  assert(stored->_data._description == "original-description");

  assert(stored->_changed_at == first_timestamp);
}

/*
 * 不同字符串必须更新并再次发布。
 */
void test_different_string_is_updated_and_published_again() {
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
      reporter.report_string(key, "ready", "first-state", first_timestamp);

  assert(inserted._status == MonData::UpdateStatus::INSERTED);

  const MonData::UpdateResult updated = reporter.report_string(
      key, "running", "changed-state", changed_timestamp);

  assert(updated._status == MonData::UpdateStatus::UPDATED);

  assert(updated.changed());
  assert(updated._record.has_value());

  assert(string_value(*updated._record) == "running");

  assert(updated._record->_data._description == "changed-state");

  assert(updated._record->_changed_at == changed_timestamp);

  /*
   * 首次插入和实际变化各发布一次。
   */
  assert(published.size() == 2);

  assert(string_value(published[1]) == "running");

  assert(published[1]._data._description == "changed-state");

  assert(published[1]._changed_at == changed_timestamp);

  const auto stored = store.find(key);

  assert(stored.has_value());

  assert(string_value(*stored) == "running");

  assert(stored->_data._description == "changed-state");

  assert(stored->_changed_at == changed_timestamp);

  assert(store.size() == 1);
}

/*
 * 两个字符串即使前 64 字节完全相同，
 * 后续内容不同也必须识别为变化。
 */
void test_strings_with_same_64_byte_prefix_are_different() {
  MonitorStore store;
  MonitorReporter reporter(store);

  std::vector<MonData::StoredRecord> published;

  reporter.set_publisher([&](MonData::StoredRecord record) {
    published.push_back(std::move(record));
  });

  const MonData::MonitorKey key{4, 5, 6, 7};

  const std::string prefix(64, 'A');

  const std::string first_value = prefix + "-first";

  const std::string second_value = prefix + "-second";

  assert(first_value.size() > 64);
  assert(second_value.size() > 64);

  assert(first_value.substr(0, 64) == second_value.substr(0, 64));

  assert(first_value != second_value);

  const MonData::UpdateResult inserted = reporter.report_string(
      key, first_value, "first-prefix-value", timestamp_at(600));

  assert(inserted._status == MonData::UpdateStatus::INSERTED);

  assert(inserted._record.has_value());

  assert(string_value(*inserted._record) == first_value);

  const MonData::UpdateResult updated = reporter.report_string(
      key, second_value, "second-prefix-value", timestamp_at(700));

  assert(updated._status == MonData::UpdateStatus::UPDATED);

  assert(updated.changed());
  assert(updated._record.has_value());

  /*
   * 必须保存完整的第二个字符串。
   */
  assert(string_value(*updated._record) == second_value);

  assert(string_value(*updated._record).size() == second_value.size());

  assert(published.size() == 2);

  assert(string_value(published[0]) == first_value);

  assert(string_value(published[1]) == second_value);

  const auto stored = store.find(key);

  assert(stored.has_value());

  assert(string_value(*stored) == second_value);
}

/*
 * 空字符串是合法字符串。
 *
 * 首次空字符串不能套用数值零值门禁。
 */
void test_empty_string_is_inserted_and_published() {
  MonitorStore store;
  MonitorReporter reporter(store);

  std::vector<MonData::StoredRecord> published;

  reporter.set_publisher([&](MonData::StoredRecord record) {
    published.push_back(std::move(record));
  });

  const MonData::MonitorKey key{5, 6, 7, 8};

  const MonData::MonitorTimestamp timestamp = timestamp_at(800);

  const MonData::UpdateResult result =
      reporter.report_string(key, "", "empty-string", timestamp);

  assert(result._status == MonData::UpdateStatus::INSERTED);

  assert(result.changed());
  assert(result._record.has_value());

  assert(string_value(*result._record).empty());

  assert(result._record->_data._description == "empty-string");

  assert(result._record->_changed_at == timestamp);

  assert(published.size() == 1);

  assert(string_value(published[0]).empty());

  const auto stored = store.find(key);

  assert(stored.has_value());

  assert(string_value(*stored).empty());

  assert(stored->_data._description == "empty-string");

  assert(stored->_changed_at == timestamp);

  assert(store.size() == 1);
}

/*
 * force 对字符串插入和去重没有实际影响。
 *
 * 使用统一 update() 显式传入不同的 force 值。
 */
void test_force_does_not_change_string_behavior() {
  MonitorStore store;
  MonitorReporter reporter(store);

  std::vector<MonData::StoredRecord> published;

  reporter.set_publisher([&](MonData::StoredRecord record) {
    published.push_back(std::move(record));
  });

  const MonData::MonitorKey false_key{6, 7, 8, 9};
  const MonData::MonitorKey true_key{6, 7, 8, 10};

  /*
   * force=false 的首次字符串正常插入。
   */
  MonData::MonitorData false_data =
      make_string_data(false_key, "force-value", "force-false");

  const MonData::UpdateResult false_inserted =
      reporter.update(std::move(false_data), false, timestamp_at(900));

  assert(false_inserted._status == MonData::UpdateStatus::INSERTED);

  /*
   * force=true 的首次字符串行为相同。
   */
  MonData::MonitorData true_data =
      make_string_data(true_key, "force-value", "force-true");

  const MonData::UpdateResult true_inserted =
      reporter.update(std::move(true_data), true, timestamp_at(1000));

  assert(true_inserted._status == MonData::UpdateStatus::INSERTED);

  assert(published.size() == 2);

  /*
   * 对 false_key 使用 force=true 重复提交相同字符串。
   * force 不能绕过去重。
   */
  MonData::MonitorData false_duplicate =
      make_string_data(false_key, "force-value", "false-key-duplicate");

  const MonData::UpdateResult false_unchanged =
      reporter.update(std::move(false_duplicate), true, timestamp_at(1100));

  assert(false_unchanged._status == MonData::UpdateStatus::UNCHANGED);

  /*
   * 对 true_key 使用 force=false 重复提交相同字符串。
   * 结果仍然必须相同。
   */
  MonData::MonitorData true_duplicate =
      make_string_data(true_key, "force-value", "true-key-duplicate");

  const MonData::UpdateResult true_unchanged =
      reporter.update(std::move(true_duplicate), false, timestamp_at(1200));

  assert(true_unchanged._status == MonData::UpdateStatus::UNCHANGED);

  assert(published.size() == 2);

  assert(store.size() == 2);
}

/*
 * 字符串不保留旧版固定 64 字节限制。
 *
 * Store、UpdateResult 和 Publisher 都必须收到完整字符串。
 */
void test_long_string_is_not_truncated() {
  MonitorStore store;
  MonitorReporter reporter(store);

  std::vector<MonData::StoredRecord> published;

  reporter.set_publisher([&](MonData::StoredRecord record) {
    published.push_back(std::move(record));
  });

  const MonData::MonitorKey key{7, 8, 9, 10};

  std::string long_value;
  long_value.reserve(256);

  for (std::size_t index = 0; index < 256; ++index) {
    const char character = static_cast<char>('A' + (index % 26));

    long_value.push_back(character);
  }

  assert(long_value.size() == 256);

  const MonData::MonitorTimestamp timestamp = timestamp_at(1300);

  const MonData::UpdateResult result =
      reporter.report_string(key, long_value, "long-string", timestamp);

  assert(result._status == MonData::UpdateStatus::INSERTED);

  assert(result._record.has_value());

  /*
   * UpdateResult 必须包含完整字符串。
   */
  assert(string_value(*result._record) == long_value);

  assert(string_value(*result._record).size() == 256);

  /*
   * Publisher 必须收到完整字符串。
   */
  assert(published.size() == 1);

  assert(string_value(published[0]) == long_value);

  assert(string_value(published[0]).size() == 256);

  /*
   * Store 中也必须保存完整字符串。
   */
  const auto stored = store.find(key);

  assert(stored.has_value());

  assert(string_value(*stored) == long_value);

  assert(string_value(*stored).size() == 256);

  assert(stored->_changed_at == timestamp);
}

} // namespace

int main() {
  test_first_string_is_inserted_and_published();
  test_same_string_is_unchanged_and_not_published_again();
  test_different_string_is_updated_and_published_again();
  test_strings_with_same_64_byte_prefix_are_different();
  test_empty_string_is_inserted_and_published();
  test_force_does_not_change_string_behavior();
  test_long_string_is_not_truncated();

  return 0;
}
