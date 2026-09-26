#include "cli_prompt.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace TLSSMON {
namespace {

constexpr bool is_valid_prompt_character(unsigned char character) noexcept {
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
} // namespace

bool is_valid_cli_prompt_name(std::string_view name) noexcept {
  if (name.empty() || name.size() > CLI_PROMPT_NAME_MAX) {
    return false;
  }

  for (const char character : name) {
    const auto byte = static_cast<unsigned char>(character);

    if (!is_valid_prompt_character(byte)) {
      return false;
    }
  }

  return true;
}

std::string make_cli_prompt(std::string_view default_name,
                            const CliSessionContext &context) {
  // 当前连接的模块选择只影响自己的提示符，不修改服务端全局名称。
  const std::string_view prompt_name =
      context.has_selected_module()
          ? std::string_view{context._selected_module_name}
          : default_name;
  if (!is_valid_cli_prompt_name(prompt_name)) {
    return {};
  }

  std::string output;
  /*
   * 两个方括号、\"> \" 三个字节，共增加五个字节。
   */
  output.reserve(prompt_name.size() + 5U);
  output.push_back('[');
  output.append(prompt_name.data(), prompt_name.size());
  output += "]> ";

  return output;
}

} // namespace TLSSMON
