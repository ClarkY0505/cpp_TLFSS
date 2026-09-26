#ifndef __MONITOR_ERROR_H__
#define __MONITOR_ERROR_H__

#include <cstdint>
#include <string>
#include <string_view>

namespace TLSSMON {

/**
 * @brief 监控错误/事件的严重级别。
 *
 * 数值必须和旧 M8 协议保持一致，不能随意调整顺序：
 *
 * 0：普通信息
 * 1：警告
 * 2：软停止
 * 3：紧急停止
 */
enum class MonitorLevel : std::uint32_t {
  INFO = 0U,
  WARN = 1U,
  SOFT_STOP = 2U,
  EMERGENCY_STOP = 3U
};

/**
 * @brief 一个 EID 对应的静态元数据。
 *
 * _level：
 *   该事件的权威严重级别。
 *
 * _description：
 *   面向用户的可读描述。
 *
 * MonitorErrorInfo 使用值语义：
 * 注册表以后保存它的副本，不依赖调用者字符串的生命周期。
 */
struct MonitorErrorInfo final {
  MonitorLevel _level{MonitorLevel::INFO};
  std::string _description;
};

/**
 * @brief 检查一个原始整数是否能转换为合法 MonitorLevel。
 *
 * 该函数是 constexpr，可以用于 static_assert 和运行时校验。
 * @param level 原始严重级别数值。
 * @return 级别属于已定义枚举值时返回 true。
 */
constexpr bool is_valid_monitor_level(std::uint32_t level) noexcept {
  switch (level) {
  case static_cast<std::uint32_t>(MonitorLevel::INFO):
  case static_cast<std::uint32_t>(MonitorLevel::WARN):
  case static_cast<std::uint32_t>(MonitorLevel::SOFT_STOP):
  case static_cast<std::uint32_t>(MonitorLevel::EMERGENCY_STOP):
    return true;

  default:
    return false;
  }
}

/**
 * @brief 将级别转换为 CLI 使用的短名称。
 *
 * 未知级别不能导致异常，统一降级为 "?"。
 *
 * 返回值引用字符串字面量，生命周期覆盖整个进程。
 * @param level 原始严重级别数值。
 * @return 级别的 CLI 短名称；未知级别返回问号。
 */
std::string_view monitor_level_name(std::uint32_t level) noexcept;

/**
 * @brief MonitorErrorInfo 的相等关系包含级别和描述。
 * @param lhs 左操作数。
 * @param rhs 右操作数。
 * @return 比较条件成立时返回 true，否则返回 false。
 */
inline bool operator==(const MonitorErrorInfo &lhs,
                       const MonitorErrorInfo &rhs) {
  return lhs._level == rhs._level && lhs._description == rhs._description;
}

/**
 * @brief 判断错误元数据的级别或描述是否不同。
 * @param lhs 左操作数。
 * @param rhs 右操作数。
 * @return 比较条件成立时返回 true，否则返回 false。
 */
inline bool operator!=(const MonitorErrorInfo &lhs,
                       const MonitorErrorInfo &rhs) {
  return !(lhs == rhs);
}

} // namespace TLSSMON
#endif // __MONITOR_ERROR_H__
