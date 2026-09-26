#ifndef __CLI_REGISTRY_H__
#define __CLI_REGISTRY_H__

#include "cli_types.h"

#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace TLSSMON {
/**
 * @brief CLI 命令注册和分派中心。
 *
 * Registry 拥有命令名、帮助文本和 Handler。
 * Registry 可以被多个线程同时注册、查询和调用。
 */
class CliRegistry final {
public:
  CliRegistry() = default;
  ~CliRegistry() = default;

  CliRegistry(const CliRegistry &) = delete;
  CliRegistry &operator=(const CliRegistry &) = delete;

  CliRegistry(CliRegistry &&) = delete;
  CliRegistry &operator=(CliRegistry &&) = delete;

  /**
   * @brief 注册一个命令。
   *
   * 命令名区分大小写。
   * 重复注册不会覆盖已有命令。
   * @param name 要注册的命令名，区分大小写且不能包含空白。
   * @param help 命令帮助文本。
   * @param handler 命令处理函数。
   * @return 命令注册状态，包含名称、Handler 或重复命令错误。
   */
  CliRegisterStatus register_command(std::string name, std::string help,
                                     CliHandler handler);
  /**
   * @brief 注册可访问客户端会话状态的命令。
   * @note Handler 在分派时收到对应连接的 CliSessionContext。
   * @param name 要注册的命令名，区分大小写且不能包含空白。
   * @param help 命令帮助文本。
   * @param handler 命令处理函数。
   * @return 命令注册状态，包含名称、Handler 或重复命令错误。
   */
  CliRegisterStatus register_command(std::string name, std::string help,
                                     CliSessionHandler handler);

  /**
   * @brief 解析一条完整命令行，并同步执行匹配的 Handler。
   *
   * Handler 在 Registry 解锁后调用。
   * @param line 一条完整的命令行。
   * @return 分派状态以及 Handler 产生的输出文本。
   */
  CliDispatchResult dispatch(std::string_view line) const;
  /**
   * @brief 使用调用者提供的 Context 分派命令。
   *
   * 同一个客户端应该在多次调用之间重复使用同一个
   * CliSessionContext。
   * @param line 一条完整的命令行。
   * @param context 当前客户端独立的会话状态。
   * @return 分派状态以及 Handler 产生的输出文本。
   */
  CliDispatchResult dispatch(std::string_view line,
                             CliSessionContext &context) const;

  /**
   * @brief 返回已注册命令的独立快照。
   *
   * 由于内部使用 std::map，返回顺序稳定为命令名字典序。
   * @return 按命令名字典序排列的独立快照。
   */
  std::vector<CliCommandInfo> commands() const;

private:
  struct Command final {
    std::string _help;
    CliSessionHandler _handler;
  };

  mutable std::mutex _mutex;
  std::map<std::string, Command> _commands;
};
} // namespace TLSSMON
#endif // __CLI_REGISTRY_H__
