#ifndef __CLI_PROMPT_H__
#define __CLI_PROMPT_H__

#include "cli_types.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace TLSSMON {

/** @brief 提示符中名称允许的最大字节数。 */
inline constexpr std::size_t CLI_PROMPT_NAME_MAX = 63U;
/**
 * @brief 校验提示符名称是否满足长度和安全字符约束。
 * @param name 待校验的提示符名称。
 * @return 提示符名称满足长度和字符约束时返回 true。
 */
bool is_valid_cli_prompt_name(std::string_view name) noexcept;
/**
 * @brief 根据当前 Session 生成提示符。
 *
 * 未选择模块：
 *
 *     [storage]>
 *
 * 选择 module-a：
 *
 *     [module-a]>
 *
 * 返回值包含最后一个空格。
 *
 * 如果当前需要显示的名称非法，返回空字符串。
 * @param default_name 未选择模块时显示的默认提示符名称。
 * @param context 当前客户端独立的会话状态。
 * @return 包含末尾空格的提示符；名称非法时返回空字符串。
 */
std::string make_cli_prompt(std::string_view default_name,
                            const CliSessionContext &context);

} // namespace TLSSMON

#endif // __CLI_PROMPT_H__
