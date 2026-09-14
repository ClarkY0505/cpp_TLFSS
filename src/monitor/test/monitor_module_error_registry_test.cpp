#include "monitor_module_registry.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <type_traits>
#include <utility>

namespace {

using namespace TLSSMON;

/* 旧 M7 三字段初始化仍然合法，省略错误表时默认为空。 */
void test_legacy_three_field_initialization() {
  const MonitorModuleInfo module{
      0x100U,
      "module-a",
      "metadata service"
  };

  assert(module._mid == 0x100U);
  assert(module._name == "module-a");
  assert(module._description == "metadata service");
  assert(module._errors.empty());
}

/* vector 下标就是 EID，每个位置保存对应的级别和描述。 */
void test_module_contains_dense_error_table() {
  const MonitorModuleInfo module{
      0x100U,
      "module-a",
      "metadata service",
      {
          {MonitorLevel::INFO, "heartbeat"},
          {MonitorLevel::WARN, "metadata timeout"},
          {MonitorLevel::EMERGENCY_STOP, "metadata unavailable"}
      }
  };

  assert(module._errors.size() == 3U);
  assert(module._errors[0U] ==
         (MonitorErrorInfo{MonitorLevel::INFO, "heartbeat"}));
  assert(module._errors[1U] ==
         (MonitorErrorInfo{MonitorLevel::WARN, "metadata timeout"}));
  assert(module._errors[2U] ==
         (MonitorErrorInfo{MonitorLevel::EMERGENCY_STOP,
                           "metadata unavailable"}));
}

/* 空错误表是合法状态，兼容尚未声明 M8 元数据的模块。 */
void test_empty_error_table_can_be_registered() {
  MonitorModuleRegistry registry;

  assert(registry.register_module(
             {0x200U, "module-b", "replication service", {}}) ==
         ModuleRegisterStatus::SUCCESS);

  const auto module = registry.find_by_id(0x200U);
  assert(module.has_value());
  assert(module->_errors.empty());
}

/* Registry 必须拥有错误表副本，不依赖调用者模块的生命周期。 */
void test_registry_owns_error_table_copy() {
  MonitorModuleRegistry registry;
  MonitorModuleInfo source{
      0x300U,
      "module-c",
      "storage service",
      {
          {MonitorLevel::INFO, "healthy"},
          {MonitorLevel::SOFT_STOP, "read only"}
      }
  };

  assert(registry.register_module(source) ==
         ModuleRegisterStatus::SUCCESS);

  source._errors[0U]._level = MonitorLevel::EMERGENCY_STOP;
  source._errors[0U]._description = "modified outside registry";
  source._errors.push_back(
      {MonitorLevel::WARN, "new external entry"});

  const auto stored = registry.find_by_id(0x300U);
  assert(stored.has_value());
  assert(stored->_errors.size() == 2U);
  assert(stored->_errors[0U] ==
         (MonitorErrorInfo{MonitorLevel::INFO, "healthy"}));
  assert(stored->_errors[1U] ==
         (MonitorErrorInfo{MonitorLevel::SOFT_STOP, "read only"}));
}

/* 查询结果也是快照，修改快照不能反向改变 Registry。 */
void test_registry_query_returns_independent_error_table() {
  MonitorModuleRegistry registry;

  assert(registry.register_module(
             {0x400U,
              "module-d",
              "network service",
              {{MonitorLevel::WARN, "link down"}}}) ==
         ModuleRegisterStatus::SUCCESS);

  auto snapshot = registry.find_by_id(0x400U);
  assert(snapshot.has_value());
  snapshot->_errors[0U]._description = "modified snapshot";

  const auto stored = registry.find_by_id(0x400U);
  assert(stored.has_value());
  assert(stored->_errors[0U]._description == "link down");
}

/* 模块相等比较必须包含整个错误表。 */
void test_module_equality_includes_error_table() {
  const MonitorModuleInfo first{
      0x500U,
      "module-e",
      "worker service",
      {{MonitorLevel::INFO, "heartbeat"}}
  };

  const MonitorModuleInfo same = first;

  MonitorModuleInfo different_level = first;
  different_level._errors[0U]._level = MonitorLevel::WARN;

  MonitorModuleInfo different_description = first;
  different_description._errors[0U]._description = "worker heartbeat";

  MonitorModuleInfo different_size = first;
  different_size._errors.push_back(
      {MonitorLevel::EMERGENCY_STOP, "worker unavailable"});

  assert(first == same);
  assert(!(first != same));
  assert(first != different_level);
  assert(first != different_description);
  assert(first != different_size);
}

/* 增加错误表后，MonitorModuleInfo 仍保持普通值类型属性。 */
void test_module_value_type_contract() {
  static_assert(std::is_copy_constructible_v<MonitorModuleInfo>);
  static_assert(std::is_copy_assignable_v<MonitorModuleInfo>);
  static_assert(std::is_move_constructible_v<MonitorModuleInfo>);
  static_assert(std::is_move_assignable_v<MonitorModuleInfo>);

  MonitorModuleInfo source{
      0x600U,
      "module-f",
      "cache service",
      {{MonitorLevel::WARN, "cache pressure"}}
  };

  MonitorModuleInfo moved = std::move(source);
  assert(moved._mid == 0x600U);
  assert(moved._errors.size() == 1U);
  assert(moved._errors[0U] ==
         (MonitorErrorInfo{MonitorLevel::WARN, "cache pressure"}));
}

} // namespace

int main() {
  test_legacy_three_field_initialization();
  test_module_contains_dense_error_table();
  test_empty_error_table_can_be_registered();
  test_registry_owns_error_table_copy();
  test_registry_query_returns_independent_error_table();
  test_module_equality_includes_error_table();
  test_module_value_type_contract();

  std::cout << "M8_STAGE2_MODULE_ERROR_TABLE=PASS\n";
  return 0;
}
