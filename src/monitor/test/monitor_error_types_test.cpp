#include "monitor_error.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace {

using namespace TLSSMON;

/* 固定严重级别的底层类型和协议数值，防止以后调整枚举顺序。 */
void test_monitor_level_numeric_contract() {
  static_assert(
      std::is_same_v<std::underlying_type_t<MonitorLevel>, std::uint32_t>);

  static_assert(static_cast<std::uint32_t>(MonitorLevel::INFO) == 0U);
  static_assert(static_cast<std::uint32_t>(MonitorLevel::WARN) == 1U);
  static_assert(static_cast<std::uint32_t>(MonitorLevel::SOFT_STOP) == 2U);
  static_assert(
      static_cast<std::uint32_t>(MonitorLevel::EMERGENCY_STOP) == 3U);
}

/* 四个已定义级别合法，所有边界外的原始整数都必须拒绝。 */
void test_monitor_level_validation() {
  static_assert(is_valid_monitor_level(0U));
  static_assert(is_valid_monitor_level(1U));
  static_assert(is_valid_monitor_level(2U));
  static_assert(is_valid_monitor_level(3U));

  static_assert(!is_valid_monitor_level(4U));
  static_assert(!is_valid_monitor_level(99U));
  static_assert(!is_valid_monitor_level(
      std::numeric_limits<std::uint32_t>::max()));
}

/* CLI 使用的四个短名称必须与旧 M8 保持一致。 */
void test_monitor_level_names() {
  assert(monitor_level_name(0U) == "info");
  assert(monitor_level_name(1U) == "warn");
  assert(monitor_level_name(2U) == "sstop");
  assert(monitor_level_name(3U) == "estop");
}

/* 未知级别必须安全降级为问号，不能越界或抛出异常。 */
void test_unknown_monitor_level_name() {
  assert(monitor_level_name(4U) == "?");
  assert(monitor_level_name(99U) == "?");
  assert(monitor_level_name(
             std::numeric_limits<std::uint32_t>::max()) == "?");
}

/* 默认错误项使用 INFO，并具有空描述。 */
void test_default_error_info() {
  const MonitorErrorInfo error;

  assert(error._level == MonitorLevel::INFO);
  assert(error._description.empty());
}

/* 相等关系必须同时比较严重级别和描述。 */
void test_error_info_equality() {
  const MonitorErrorInfo first{MonitorLevel::WARN, "frame drop"};
  const MonitorErrorInfo same{MonitorLevel::WARN, "frame drop"};
  const MonitorErrorInfo different_level{
      MonitorLevel::EMERGENCY_STOP, "frame drop"};
  const MonitorErrorInfo different_description{
      MonitorLevel::WARN, "sensor unavailable"};

  assert(first == same);
  assert(!(first != same));
  assert(first != different_level);
  assert(first != different_description);
}

/* 错误项必须具有独立字符串所有权，并支持复制和移动。 */
void test_error_info_value_semantics() {
  static_assert(std::is_copy_constructible_v<MonitorErrorInfo>);
  static_assert(std::is_copy_assignable_v<MonitorErrorInfo>);
  static_assert(std::is_move_constructible_v<MonitorErrorInfo>);
  static_assert(std::is_move_assignable_v<MonitorErrorInfo>);

  const MonitorErrorInfo original{
      MonitorLevel::WARN, "metadata timeout"};

  MonitorErrorInfo copied = original;
  copied._description = "modified copy";

  assert(original._description == "metadata timeout");
  assert(copied._description == "modified copy");

  MonitorErrorInfo moved = std::move(copied);
  assert(moved._level == MonitorLevel::WARN);
  assert(moved._description == "modified copy");
}

} // namespace

int main() {
  test_monitor_level_numeric_contract();
  test_monitor_level_validation();
  test_monitor_level_names();
  test_unknown_monitor_level_name();
  test_default_error_info();
  test_error_info_equality();
  test_error_info_value_semantics();

  std::cout << "M8_STAGE1_MONITOR_ERROR_TYPES=PASS\n";
  return 0;
}
