#ifndef __CLI_PROMPT_H__
#define __CLI_PROMPT_H__

#include "cli_types.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace TLSSMON {

inline constexpr std::size_t CLI_PROMPT_NAME_MAX = 63U;
bool is_valid_cli_prompt_name(std::string_view name) noexcept;
/*
 * 根据当前 Session 生成提示符。
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
 */
std::string make_cli_prompt(std::string_view default_name,
                            const CliSessionContext &context);

} // namespace TLSSMON

#endif // __CLI_PROMPT_H__
