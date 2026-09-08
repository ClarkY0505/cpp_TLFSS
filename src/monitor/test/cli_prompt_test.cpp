#include "cli_prompt.h"

#include <cassert>
#include <iostream>
#include <string>

namespace {

using namespace TLSSMON;

/* 服务名和模块名都只接受非空、有限长度的安全 ASCII。 */
void test_prompt_name_validation() {
  assert(is_valid_cli_prompt_name("monitor"));
  assert(is_valid_cli_prompt_name("storage-1"));
  assert(!is_valid_cli_prompt_name(""));
  assert(!is_valid_cli_prompt_name("bad name"));
  assert(!is_valid_cli_prompt_name(std::string(CLI_PROMPT_NAME_MAX + 1U, 'x')));
}

/* 未选择模块时使用服务名；选择模块后使用当前 Session 的模块名。 */
void test_prompt_tracks_session_context() {
  CliSessionContext context;
  assert(make_cli_prompt("storage", context) == "[storage]> ");

  context._selected_mid = 10U;
  context._selected_module_name = "module-a";
  assert(make_cli_prompt("storage", context) == "[module-a]> ");

  context.clear_module();
  assert(make_cli_prompt("storage", context) == "[storage]> ");
}

} // namespace

int main() {
  test_prompt_name_validation();
  test_prompt_tracks_session_context();

  std::cout << "M7_CLI_PROMPT=PASS\n";
  return 0;
}
