#ifndef __MONITOR_MODULE_H__
#define __MONITOR_MODULE_H__

#include "monitor_error.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace TLSSMON {

/*
 * 模块名称最大长度。
 *
 * 按字节计算，不按 Unicode 字符数量计算。
 * 当前阶段只允许安全 ASCII，因此字节数就是字符数。
 */
inline constexpr std::size_t MONITOR_MODULE_NAME_MAX = 63U;

/*
 * 注册模块的结果。
 */
enum class ModuleRegisterStatus : std::uint8_t {
  /*
   * 注册成功。
   */
  SUCCESS,
  /*
   * 模块名称为空、过长、包含非法字符，
   * 或者使用了保留名称 "all"。
   */
  INVALID_NAME,
  /*
   * 已经存在相同 mid 的模块。
   */
  DUPLICATE_ID,
  /*
   * 已经存在相同名称的模块。
   */
  DUPLICATE_NAME,
  /*
   * 模块错误表中至少存在一个非法 MonitorLevel。
   */
  INVALID_ERROR_LEVEL,
  /*
   * 当前 Engine 生命周期阶段不允许注册模块。
   */
  INVALID_PHASE

};

/*
 * 一个受监控模块的公共信息。
 *
 * _mid：
 *   与 MonitorKey::_mid 对应。
 *
 * _name：
 *   CLI 使用的稳定名称，例如 module-a。
 *
 * _description：
 *   面向用户的说明文本，不参与唯一性判断。
 * _errors：
 *   当前模块的错误/事件元数据表。
 *
 *   vector 下标就是 eid：
 *
 *   _errors[0] 对应 eid=0
 *   _errors[1] 对应 eid=1
 *   _errors[2] 对应 eid=2
 *
 *   因此 EID 必须从 0 开始连续排列。
 */
struct MonitorModuleInfo final {
  std::uint32_t _mid{0U};
  std::string _name;
  std::string _description;
  std::vector<MonitorErrorInfo> _errors;

  MonitorModuleInfo() = default;

  /*
   * 第四个参数默认为空错误表，使旧 M7 的三字段初始化在启用
   * -Wmissing-field-initializers 和 -Werror 时仍能无警告编译。
   */
  MonitorModuleInfo(std::uint32_t mid, std::string name,
                    std::string description,
                    std::vector<MonitorErrorInfo> errors = {})
      : _mid(mid), _name(std::move(name)), _description(std::move(description)),
        _errors(std::move(errors)) {}
};

/*
 * 判断字符是否属于模块名称允许使用的安全 ASCII 集合。
 *
 * 合法字符：
 *
 * A-Z
 * a-z
 * 0-9
 * -
 * _
 * .
 */
constexpr bool
is_valid_module_name_character(unsigned char character) noexcept {
  const bool lowercase = character >= static_cast<unsigned char>('a') &&
                         character <= static_cast<unsigned char>('z');

  const bool uppercase = character >= static_cast<unsigned char>('A') &&
                         character <= static_cast<unsigned char>('Z');

  const bool digit = character >= static_cast<unsigned char>('0') &&
                     character <= static_cast<unsigned char>('9');

  return lowercase || uppercase || digit ||
         character == static_cast<unsigned char>('-') ||
         character == static_cast<unsigned char>('_') ||
         character == static_cast<unsigned char>('.');
}

/*
 * 校验模块名称。
 *
 * 规则：
 *
 * 1. 名称不能为空。
 * 2. 长度不能超过 63 字节。
 * 3. 只允许安全 ASCII 字符。
 * 4. "all" 是 use all 使用的保留名称。
 *
 * 这里显式转换为 unsigned char，避免 char 为有符号类型时，
 * 高位字节参与比较产生平台相关行为。
 */
inline bool is_valid_module_name(std::string_view name) noexcept {
  if (name.empty() || name.size() > MONITOR_MODULE_NAME_MAX) {
    return false;
  }

  if (name == "all") {
    return false;
  }

  for (const char character : name) {
    const auto byte = static_cast<unsigned char>(character);

    if (!is_valid_module_name_character(byte)) {
      return false;
    }
  }

  return true;
}

/*
 * MonitorModuleInfo 使用完整值语义。
 *
 * description 虽然不参与注册唯一性判断，
 * 但它是 MonitorModuleInfo 的组成部分，因此 operator==
 * 必须比较它。
 */
inline bool operator==(const MonitorModuleInfo &lhs,
                       const MonitorModuleInfo &rhs) {
  return lhs._mid == rhs._mid && lhs._name == rhs._name &&
         lhs._description == rhs._description && lhs._errors == rhs._errors;
}

inline bool operator!=(const MonitorModuleInfo &lhs,
                       const MonitorModuleInfo &rhs) {
  return !(lhs == rhs);
}

} // namespace TLSSMON

#endif // TLSSMON_MONITOR_MODULE_H
