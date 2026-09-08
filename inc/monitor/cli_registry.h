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
/*
 * CLI 命令注册和分派中心。
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

  /*
   * 注册一个命令。
   *
   * 命令名区分大小写。
   * 重复注册不会覆盖已有命令。
   */
  CliRegisterStatus register_command(std::string name, std::string help,
                                     CliHandler handler);
  /*
   * 为了保留原有命令，重载这个函数
   * 主要目的：想要在同一个engine下来实现不同模块的监测
   * 而不是希望多个客户端来监测不同的模块
   *
   * */
  CliRegisterStatus register_command(std::string name, std::string help,
                                     CliSessionHandler handler);

  /*
   * 解析一条完整命令行，并同步执行匹配的 Handler。
   *
   * Handler 在 Registry 解锁后调用。
   */
  CliDispatchResult dispatch(std::string_view line) const;
  /*
   * 使用调用者提供的 Context 分派命令。
   *
   * 同一个客户端应该在多次调用之间重复使用同一个
   * CliSessionContext。
   */
  CliDispatchResult dispatch(std::string_view line,
                             CliSessionContext &context) const;

  /*
   * 返回已注册命令的独立快照。
   *
   * 由于内部使用 std::map，返回顺序稳定为命令名字典序。
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
