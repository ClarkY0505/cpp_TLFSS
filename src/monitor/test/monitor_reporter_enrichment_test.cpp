#include "engine.h"
#include "monitor_module_registry.h"
#include "monitor_reporter.h"
#include "monitor_store.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

using namespace TLSSMON;

constexpr std::uint32_t MODULE_ID = 0x100U;

MonData::MonitorTimestamp timestamp(std::int64_t seconds) {
  return MonData::MonitorTimestamp{std::chrono::seconds{seconds}};
}

void register_standard_module(MonitorModuleRegistry &modules) {
  assert(modules.register_module(
             {MODULE_ID,
              "module-a",
              "metadata service",
              {
                  {MonitorLevel::INFO, "heartbeat"},
                  {MonitorLevel::WARN, "metadata timeout"},
                  {MonitorLevel::EMERGENCY_STOP, "metadata unavailable"},
                  {MonitorLevel::SOFT_STOP, ""}
              }}) ==
         ModuleRegisterStatus::SUCCESS);
}

/* 阶段 5 的旧 M5 构造和 Registry 绑定构造必须同时保留。 */
void test_reporter_constructor_contract() {
  static_assert(
      std::is_constructible_v<MonitorReporter, MonitorStore &>);
  static_assert(
      std::is_constructible_v<MonitorReporter,
                              MonitorStore &,
                              const MonitorModuleRegistry &>);
  static_assert(!std::is_default_constructible_v<MonitorReporter>);
  static_assert(!std::is_copy_constructible_v<MonitorReporter>);
  static_assert(!std::is_copy_assignable_v<MonitorReporter>);
  static_assert(!std::is_move_constructible_v<MonitorReporter>);
  static_assert(!std::is_move_assignable_v<MonitorReporter>);
}

/* 普通计数上报必须采用 Registry 的权威 level 和非空描述。 */
void test_report_count_enriches_level_and_description() {
  MonitorStore store;
  MonitorModuleRegistry modules;
  register_standard_module(modules);
  MonitorReporter reporter{store, modules};

  const MonData::MonitorKey supplied{MODULE_ID, 99U, 7U, 1U};
  const MonData::MonitorKey normalized{
      MODULE_ID,
      static_cast<std::uint32_t>(MonitorLevel::WARN),
      7U,
      1U
  };
  const auto changed_at = timestamp(100);

  const MonData::UpdateResult result = reporter.report_count(
      supplied, 42U, "caller description", changed_at);

  assert(result._status == MonData::UpdateStatus::INSERTED);
  assert(result._record.has_value());
  assert(result._record->_data._key == normalized);
  assert(result._record->_data._description == "metadata timeout");
  assert(result._record->_changed_at == changed_at);

  const auto *numeric = std::get_if<MonData::NumericValue>(
      &result._record->_data._value);
  assert(numeric != nullptr);
  assert(numeric->_value == 42U);
  assert(numeric->_state == 0U);

  assert(!store.find(supplied).has_value());
  assert(store.find(normalized).has_value());
}

/* 错误上报同样补全元数据，并保持 force=true 和 fresh 状态。 */
void test_report_error_enriches_and_keeps_error_semantics() {
  MonitorStore store;
  MonitorModuleRegistry modules;
  register_standard_module(modules);
  MonitorReporter reporter{store, modules};

  const auto changed_at = timestamp(200);
  const MonData::UpdateResult result = reporter.report_error(
      {MODULE_ID, 0U, 8U, 2U}, 0U, "caller description", changed_at);

  assert(result._status == MonData::UpdateStatus::INSERTED);
  assert(result._record.has_value());
  assert(result._record->_data._key._level ==
         static_cast<std::uint32_t>(MonitorLevel::EMERGENCY_STOP));
  assert(result._record->_data._description == "metadata unavailable");
  assert(result._record->_changed_at == changed_at);

  const auto *numeric = std::get_if<MonData::NumericValue>(
      &result._record->_data._value);
  assert(numeric != nullptr);
  assert(numeric->_value == 0U);
  assert(numeric->_state == 2U);
}

/* 字符串上报补全元数据，但空字符串和完整字符串去重规则保持不变。 */
void test_report_string_enriches_without_changing_deduplication() {
  MonitorStore store;
  MonitorModuleRegistry modules;
  register_standard_module(modules);
  MonitorReporter reporter{store, modules};

  const MonData::MonitorKey supplied{MODULE_ID, 77U, 9U, 0U};
  const auto first_time = timestamp(300);
  const auto duplicate_time = timestamp(301);
  const auto changed_time = timestamp(302);

  const MonData::UpdateResult first = reporter.report_string(
      supplied, "", "caller description", first_time);
  assert(first._status == MonData::UpdateStatus::INSERTED);
  assert(first._record.has_value());
  assert(first._record->_data._key._level ==
         static_cast<std::uint32_t>(MonitorLevel::INFO));
  assert(first._record->_data._description == "heartbeat");
  assert(std::get<std::string>(first._record->_data._value).empty());

  const MonData::UpdateResult duplicate = reporter.report_string(
      supplied, "", "different caller description", duplicate_time);
  assert(duplicate._status == MonData::UpdateStatus::UNCHANGED);
  assert(duplicate._record.has_value());
  assert(duplicate._record->_changed_at == first_time);

  const MonData::UpdateResult changed = reporter.report_string(
      supplied, "running", "caller description", changed_time);
  assert(changed._status == MonData::UpdateStatus::UPDATED);
  assert(changed._record.has_value());
  assert(std::get<std::string>(changed._record->_data._value) == "running");
  assert(changed._record->_changed_at == changed_time);
}

/* Registry 描述为空时只覆盖 level，调用者描述必须保留。 */
void test_empty_registry_description_preserves_caller_description() {
  MonitorStore store;
  MonitorModuleRegistry modules;
  register_standard_module(modules);
  MonitorReporter reporter{store, modules};

  const MonData::UpdateResult result = reporter.report_count(
      {MODULE_ID, 99U, 10U, 3U},
      1U,
      "caller fallback",
      timestamp(400));

  assert(result._status == MonData::UpdateStatus::INSERTED);
  assert(result._record.has_value());
  assert(result._record->_data._key._level ==
         static_cast<std::uint32_t>(MonitorLevel::SOFT_STOP));
  assert(result._record->_data._description == "caller fallback");
}

/* 未知 MID 或越界 EID 不拒绝上报，并保留调用者元数据。 */
void test_unknown_metadata_preserves_supplied_fields() {
  MonitorStore store;
  MonitorModuleRegistry modules;
  register_standard_module(modules);
  MonitorReporter reporter{store, modules};

  const MonData::UpdateResult unknown_mid = reporter.report_count(
      {0x999U, 91U, 1U, 0U},
      1U,
      "unknown module",
      timestamp(500));
  assert(unknown_mid._status == MonData::UpdateStatus::INSERTED);
  assert(unknown_mid._record.has_value());
  assert(unknown_mid._record->_data._key._level == 91U);
  assert(unknown_mid._record->_data._description == "unknown module");

  const MonData::UpdateResult unknown_eid = reporter.report_count(
      {MODULE_ID, 92U, 1U, 99U},
      2U,
      "unknown event",
      timestamp(501));
  assert(unknown_eid._status == MonData::UpdateStatus::INSERTED);
  assert(unknown_eid._record.has_value());
  assert(unknown_eid._record->_data._key._level == 92U);
  assert(unknown_eid._record->_data._description == "unknown event");
}

/* 不绑定 Registry 的旧 Reporter 必须保持完整 M5 行为。 */
void test_legacy_reporter_does_not_enrich() {
  MonitorStore store;
  MonitorReporter reporter{store};

  const MonData::MonitorKey supplied{MODULE_ID, 88U, 2U, 1U};
  const MonData::UpdateResult result = reporter.report_count(
      supplied, 3U, "legacy description", timestamp(600));

  assert(result._status == MonData::UpdateStatus::INSERTED);
  assert(result._record.has_value());
  assert(result._record->_data._key == supplied);
  assert(result._record->_data._description == "legacy description");
}

/* 统一 update() 即使绑定 Registry，也必须保存完整原始数据。 */
void test_reporter_update_does_not_enrich() {
  MonitorStore store;
  MonitorModuleRegistry modules;
  register_standard_module(modules);
  MonitorReporter reporter{store, modules};

  const MonData::MonitorKey supplied{MODULE_ID, 66U, 3U, 1U};
  MonData::MonitorData data{
      supplied,
      "wire description",
      MonData::NumericValue{4U, 1U}
  };

  const MonData::UpdateResult result = reporter.update(
      std::move(data), false, timestamp(700));

  assert(result._status == MonData::UpdateStatus::INSERTED);
  assert(result._record.has_value());
  assert(result._record->_data._key == supplied);
  assert(result._record->_data._description == "wire description");

  const auto *numeric = std::get_if<MonData::NumericValue>(
      &result._record->_data._value);
  assert(numeric != nullptr);
  assert(numeric->_state == 1U);
}

/* Engine 的 report_* 必须使用 Engine 按值持有的同一注册表。 */
void test_engine_reporter_observes_modules_registered_after_init() {
  Engine engine{MonConfig{"m8-enrichment", 0U, 1U}};
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);

  assert(engine.register_module(
             {MODULE_ID,
              "module-a",
              "metadata service",
              {{MonitorLevel::WARN, "engine heartbeat"}}}) ==
         ModuleRegisterStatus::SUCCESS);

  const MonData::UpdateResult result = engine.report_count(
      {MODULE_ID, 99U, 4U, 0U}, 5U, "caller description");

  assert(result._status == MonData::UpdateStatus::INSERTED);
  assert(result._record.has_value());
  assert(result._record->_data._key._level ==
         static_cast<std::uint32_t>(MonitorLevel::WARN));
  assert(result._record->_data._description == "engine heartbeat");
}

/* Engine::update_data() 必须绕过 enrich()，保留 Collector 原始记录。 */
void test_engine_update_data_preserves_raw_metadata() {
  Engine engine{MonConfig{"m8-raw-update", 0U, 1U}};
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);

  assert(engine.register_module(
             {MODULE_ID,
              "module-a",
              "metadata service",
              {{MonitorLevel::WARN, "registry description"}}}) ==
         ModuleRegisterStatus::SUCCESS);

  const MonData::MonitorKey supplied{MODULE_ID, 55U, 5U, 0U};
  MonData::MonitorData data{
      supplied,
      "datagram description",
      MonData::NumericValue{6U, 0U}
  };

  const MonData::UpdateResult result = engine.update_data(
      std::move(data));

  assert(result._status == MonData::UpdateStatus::INSERTED);
  assert(result._record.has_value());
  assert(result._record->_data._key == supplied);
  assert(result._record->_data._description == "datagram description");
}

} // namespace

int main() {
  test_reporter_constructor_contract();
  test_report_count_enriches_level_and_description();
  test_report_error_enriches_and_keeps_error_semantics();
  test_report_string_enriches_without_changing_deduplication();
  test_empty_registry_description_preserves_caller_description();
  test_unknown_metadata_preserves_supplied_fields();
  test_legacy_reporter_does_not_enrich();
  test_reporter_update_does_not_enrich();
  test_engine_reporter_observes_modules_registered_after_init();
  test_engine_update_data_preserves_raw_metadata();

  std::cout << "M8_STAGE6_7_REPORTER_ENRICHMENT=PASS\n";
  return 0;
}
