#include "monitor_error.h"
#include "monitor_module_registry.h"
#include "monitor_reporter.h"
#include "monitor_store.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace TLSSMON;

constexpr std::uint32_t MODULE_ID = 0x100U;
constexpr std::uint32_t FUNCTION_ID = 1U;

MonData::MonitorTimestamp make_timestamp(std::int64_t seconds) {
  return MonData::MonitorTimestamp{std::chrono::seconds{seconds}};
}

void register_test_module(MonitorModuleRegistry &modules) {
  const ModuleRegisterStatus status = modules.register_module(
      {MODULE_ID,
       "module-a",
       "stage8 test module",
       {{MonitorLevel::INFO, "heartbeat"},
        {MonitorLevel::WARN, "metadata timeout"},
        {MonitorLevel::SOFT_STOP, ""}}});

  assert(status == ModuleRegisterStatus::SUCCESS);
}

/*
 * Registry 规范化必须先于 Store 去重：不同的调用者 level 在补全后
 * 落到同一个 Key，相同值只保存和发布一次，UNCHANGED 保留原时间戳。
 */
void test_normalization_happens_before_numeric_deduplication() {
  MonitorStore store;
  MonitorModuleRegistry modules;
  register_test_module(modules);
  MonitorReporter reporter{store, modules};

  std::vector<MonData::StoredRecord> published;
  reporter.set_publisher([&](MonData::StoredRecord record) {
    published.push_back(std::move(record));
  });

  const MonData::MonitorKey first_supplied{
      MODULE_ID, 99U, FUNCTION_ID, 1U};
  const MonData::MonitorKey normalized{
      MODULE_ID,
      static_cast<std::uint32_t>(MonitorLevel::WARN),
      FUNCTION_ID,
      1U};
  const auto first_time = make_timestamp(100);

  const MonData::UpdateResult first = reporter.report_count(
      first_supplied, 42U, "caller description", first_time);

  assert(first._status == MonData::UpdateStatus::INSERTED);
  assert(first._record.has_value());
  assert(first._record->_data._key == normalized);
  assert(first._record->_data._description == "metadata timeout");
  assert(first._record->_changed_at == first_time);
  assert(!store.find(first_supplied).has_value());
  assert(store.find(normalized).has_value());
  assert(store.size() == 1U);

  assert(published.size() == 1U);
  assert(published[0]._data._key == normalized);
  assert(published[0]._data._description == "metadata timeout");
  assert(published[0]._changed_at == first_time);

  const MonData::MonitorKey second_supplied{
      MODULE_ID, 77U, FUNCTION_ID, 1U};
  const auto duplicate_time = make_timestamp(200);

  const MonData::UpdateResult duplicate = reporter.report_count(
      second_supplied, 42U, "another caller description", duplicate_time);

  assert(duplicate._status == MonData::UpdateStatus::UNCHANGED);
  assert(duplicate._record.has_value());
  assert(duplicate._record->_data._key == normalized);
  assert(duplicate._record->_changed_at == first_time);
  assert(duplicate._record->_changed_at != duplicate_time);
  assert(!store.find(second_supplied).has_value());
  assert(store.size() == 1U);
  assert(published.size() == 1U);
}

/*
 * Registry 描述为空时会保留调用者描述；即使后续调用者描述变化，
 * M4 数值去重仍只比较 NumericValue::_value，不覆盖描述和时间戳。
 */
void test_description_does_not_participate_in_numeric_deduplication() {
  MonitorStore store;
  MonitorModuleRegistry modules;
  register_test_module(modules);
  MonitorReporter reporter{store, modules};

  std::vector<MonData::StoredRecord> published;
  reporter.set_publisher([&](MonData::StoredRecord record) {
    published.push_back(std::move(record));
  });

  const MonData::MonitorKey first_supplied{
      MODULE_ID, 91U, FUNCTION_ID, 2U};
  const MonData::MonitorKey normalized{
      MODULE_ID,
      static_cast<std::uint32_t>(MonitorLevel::SOFT_STOP),
      FUNCTION_ID,
      2U};
  const auto first_time = make_timestamp(300);

  const MonData::UpdateResult first = reporter.report_count(
      first_supplied, 7U, "first description", first_time);

  assert(first._status == MonData::UpdateStatus::INSERTED);
  assert(first._record.has_value());
  assert(first._record->_data._key == normalized);
  assert(first._record->_data._description == "first description");
  assert(first._record->_changed_at == first_time);
  assert(published.size() == 1U);

  const MonData::MonitorKey second_supplied{
      MODULE_ID, 92U, FUNCTION_ID, 2U};
  const auto duplicate_time = make_timestamp(301);

  const MonData::UpdateResult duplicate = reporter.report_count(
      second_supplied, 7U, "second description", duplicate_time);

  assert(duplicate._status == MonData::UpdateStatus::UNCHANGED);
  assert(duplicate._record.has_value());
  assert(duplicate._record->_data._key == normalized);
  assert(duplicate._record->_data._description == "first description");
  assert(duplicate._record->_changed_at == first_time);
  assert(store.size() == 1U);
  assert(published.size() == 1U);
}

/*
 * 元数据补全不能绕过普通计数的首次零值门禁：不创建 Key，
 * 不返回记录，也不调用 Publisher。
 */
void test_count_initial_zero_is_still_ignored() {
  MonitorStore store;
  MonitorModuleRegistry modules;
  register_test_module(modules);
  MonitorReporter reporter{store, modules};

  std::vector<MonData::StoredRecord> published;
  reporter.set_publisher([&](MonData::StoredRecord record) {
    published.push_back(std::move(record));
  });

  const MonData::UpdateResult result = reporter.report_count(
      {MODULE_ID, 99U, FUNCTION_ID, 1U},
      0U,
      "caller description",
      make_timestamp(400));

  assert(result._status == MonData::UpdateStatus::IGNORED_INITIAL_ZERO);
  assert(!result._record.has_value());
  assert(store.size() == 0U);
  assert(published.empty());
}

/*
 * report_error() 仍使用 force=true 和 state=2，因此首次错误零值
 * 必须以规范化后的 Key 插入，并向 Publisher 发布一次。
 */
void test_error_initial_zero_is_inserted_and_published() {
  MonitorStore store;
  MonitorModuleRegistry modules;
  register_test_module(modules);
  MonitorReporter reporter{store, modules};

  std::vector<MonData::StoredRecord> published;
  reporter.set_publisher([&](MonData::StoredRecord record) {
    published.push_back(std::move(record));
  });

  const MonData::MonitorKey supplied{
      MODULE_ID, 99U, FUNCTION_ID, 1U};
  const MonData::MonitorKey normalized{
      MODULE_ID,
      static_cast<std::uint32_t>(MonitorLevel::WARN),
      FUNCTION_ID,
      1U};
  const auto changed_at = make_timestamp(500);

  const MonData::UpdateResult result = reporter.report_error(
      supplied, 0U, "caller description", changed_at);

  assert(result._status == MonData::UpdateStatus::INSERTED);
  assert(result._record.has_value());
  assert(result._record->_data._key == normalized);
  assert(result._record->_data._description == "metadata timeout");
  assert(result._record->_changed_at == changed_at);

  const auto *numeric = std::get_if<MonData::NumericValue>(
      &result._record->_data._value);
  assert(numeric != nullptr);
  assert(numeric->_value == 0U);
  assert(numeric->_state == 2U);

  assert(published.size() == 1U);
  assert(published[0]._data._key == normalized);
  assert(published[0]._data._description == "metadata timeout");

  const auto *published_numeric = std::get_if<MonData::NumericValue>(
      &published[0]._data._value);
  assert(published_numeric != nullptr);
  assert(published_numeric->_value == 0U);
  assert(published_numeric->_state == 2U);

  assert(!store.find(supplied).has_value());
  assert(store.find(normalized).has_value());
  assert(store.size() == 1U);
}

} // namespace

int main() {
  test_normalization_happens_before_numeric_deduplication();
  test_description_does_not_participate_in_numeric_deduplication();
  test_count_initial_zero_is_still_ignored();
  test_error_initial_zero_is_inserted_and_published();

  std::cout << "M8_STAGE8_DEDUP_ORDER=PASS\n";
  return 0;
}
