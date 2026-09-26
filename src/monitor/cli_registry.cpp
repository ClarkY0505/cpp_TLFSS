#include "cli_registry.h"
#include "cli_types.h"

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace TLSSMON {
namespace {
bool is_cli_space(char character) noexcept {
  return std::isspace(static_cast<unsigned char>(character)) != 0;
}

bool is_valid_command_name(std::string_view name) noexcept {
  if (name.empty()) {
    return false;
  }

  for (const char character : name) {
    if (is_cli_space(character)) {
      return false;
    }
  }
  return true;
}

CliDispatchResult line_too_long_result() {
  return {CliDispatchStatus::LINE_TOO_LONG, "error: line too long\n"};
}

CliDispatchResult too_many_tokens_result() {
  return {CliDispatchStatus::TOO_MANY_TOKENS, "error: too many tokens\n"};
}

CliDispatchResult empty_input_result() {
  return {CliDispatchStatus::EMPTY_INPUT, {}};
}

CliDispatchResult unknow_command_result(const std::string &command) {
  return {CliDispatchStatus::UNKNOWN_COMMAND,
          "unknown command: " + command + '\n'};
}

CliDispatchResult handler_error_result() {
  return {CliDispatchStatus::HANDLER_ERROR, "error: command failed\n"};
}

bool tokenize(std::string_view line, std::vector<std::string> &tokens) {
  std::size_t position = 0U;
  while (position < line.size()) {
    while (position < line.size() && is_cli_space(line[position])) {
      ++position;
    }

    if (position == line.size()) {
      break;
    }

    const std::size_t begin = position;
    /*
     * 找到当前 token 末尾。
     */
    while (position < line.size() && !is_cli_space(line[position])) {
      ++position;
    }

    if (tokens.size() == CLI_MAX_TOKENS) {
      return false;
    }

    tokens.emplace_back(line.substr(begin, position - begin));
  }

  return true;
}
} // namespace

CliRegisterStatus CliRegistry::register_command(std::string name,
                                                std::string help,
                                                CliHandler handler) {
  if (!is_valid_command_name(name)) {
    return CliRegisterStatus::INVALID_NAME;
  }

  if (!handler) {
    return CliRegisterStatus::INVALID_HANDLER;
  }

  // 旧式 Handler 不读取会话状态，统一适配到带 Context 的分派路径。
  CliSessionHandler adapted =
      [handler = std::move(handler)](CliSessionContext &,
                                     const CliArguments &arguments) {
        return handler(arguments);
      };

  return register_command(std::move(name), std::move(help), std::move(adapted));
}

CliRegisterStatus CliRegistry::register_command(std::string name,
                                                std::string help,
                                                CliSessionHandler handler) {
  if (!is_valid_command_name(name)) {
    return CliRegisterStatus::INVALID_NAME;
  }

  if (!handler) {
    return CliRegisterStatus::INVALID_HANDLER;
  }

  std::lock_guard<std::mutex> lock{_mutex};
  const auto insertion = _commands.emplace(
      std::move(name), Command{std::move(help), std::move(handler)});

  if (!insertion.second) {
    return CliRegisterStatus::DUPLICATE_COMMAND;
  }

  return CliRegisterStatus::SUCCESS;
}

CliDispatchResult CliRegistry::dispatch(std::string_view line) const {
  CliSessionContext context;
  return dispatch(line, context);
}

CliDispatchResult CliRegistry::dispatch(std::string_view line, CliSessionContext& context) const {
  if (line.size() > CLI_MAX_LINE_SIZE) {
    return line_too_long_result();
  }

  std::vector<std::string> tokens;
  tokens.reserve(CLI_MAX_TOKENS);

  if (!tokenize(line, tokens)) {
    return too_many_tokens_result();
  }

  if (tokens.empty()) {
    return empty_input_result();
  }

  CliSessionHandler handler;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    const auto command = _commands.find(tokens.front());
    if (command == _commands.end()) {
      return unknow_command_result(tokens.front());
    }
    handler = command->second._handler;
  }

  // Handler 在解锁后调用，允许命令内部查询或注册其他命令。
  CliArguments arguments;
  arguments.reserve(tokens.size() - 1U);
  for (std::size_t index = 1U; index < tokens.size(); ++index) {
    arguments.push_back(std::move(tokens[index]));
  }
  try {
    return {CliDispatchStatus::SUCCESS, handler(context, arguments)};
  } catch (...) {
    /*
     * CLI 是 Engine AIO 调用路径的一部分。
     * 用户 Handler 异常不能穿透到事件循环。
     */
    return handler_error_result();
  }
}

std::vector<CliCommandInfo> CliRegistry::commands() const {
  std::lock_guard<std::mutex> lock(_mutex);

  std::vector<CliCommandInfo> snapshot;
  snapshot.reserve(_commands.size());

  for (const auto &entry : _commands) {
    snapshot.push_back(CliCommandInfo{entry.first, entry.second._help});
  }

  return snapshot;
}

} // namespace TLSSMON
