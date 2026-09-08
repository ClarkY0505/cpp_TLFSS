#ifndef __CLI_TYPES_H__
#define __CLI_TYPES_H__

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace TLSSMON {
/*
 * 一条命令行允许保存的最大字节数。
 * 该长度不包含 TCP 行结束符 '\n' 或 "\r\n"。
 */
inline constexpr std::size_t CLI_MAX_LINE_SIZE = 255U;

/*
 * 一条命令最多允许的 token 数量。
 *
 * 该数量包含命令名，因此实际最多是：
 *
 *     1 个命令名 + 7 个参数
 */
inline constexpr std::size_t CLI_MAX_TOKENS = 8U;

/*
 * TCP CLI 同时允许存在的最大客户端数量。
 *
 * 这是并发连接上限，不是进程生命周期内累计连接上限。
 * 客户端断开后，对应位置必须能够重新使用。
 */
inline constexpr std::size_t CLI_MAX_CLIENTS = 8U;

/*
 * CliRegistry::register_command() 的执行结果。
 */
enum class CliRegisterStatus : std::uint8_t {
  /*
   * 命令注册成功。
   */
  SUCCESS,

  /*
   * 命令名为空，或者包含空白、回车、换行等非法字符。
   */
  INVALID_NAME,

  /*
   * 调用者传入了空的 std::function。
   */
  INVALID_HANDLER,

  /*
   * 注册表中已经存在完全相同的命令名。
   */
  DUPLICATE_COMMAND
};

enum class CliDispatchStatus : std::uint8_t {
  /*
   * 找到命令，并且 Handler 正常返回。
   */
  SUCCESS,

  /*
   * 输入为空，或者输入只包含空白字符。
   */
  EMPTY_INPUT,

  /*
   * 第一个 token 没有对应的已注册命令。
   */
  UNKNOWN_COMMAND,

  /*
   * token 数超过 CLI_MAX_TOKENS。
   */
  TOO_MANY_TOKENS,

  /*
   * 输入行长度超过 CLI_MAX_LINE_SIZE。
   */
  LINE_TOO_LONG,

  /*
   * Handler 抛出异常。
   *
   * 阶段 3 必须捕获异常并转换成这个状态，
   * 不能让异常穿透到 Engine AIO 事件循环。
   */
  HANDLER_ERROR

};

using CliArguments = std::vector<std::string>;

/*
 * 单个 TCP 客户端连接独立拥有的状态。
 *
 * 每个 CliServer::ClientSession 将来都应保存一个
 * CliSessionContext，不能由 CliRegistry 全局共享。
 */
struct CliSessionContext final {
  /*
   * 当前选择模块的 mid。
   *
   * std::nullopt 表示没有选择具体模块，也就是 all。
   * 后续 db_dump 使用该字段构造 MonitorFilter。
   */
  std::optional<std::uint32_t> _selected_mid;
  /*
   * 当前选择模块的稳定名称。
   *
   * 该字段用于生成类似 [module-a]> 的提示符。
   * 未选择模块时为空。
   */
  std::string _selected_module_name;

  /*
   * 清除当前模块选择。
   *
   * use all 命令将调用这个函数，使 Session
   * 回到默认的全模块视图。
   */
  void clear_module() {
    _selected_mid.reset();
    _selected_module_name.clear();
  }

  /*
   * 判断当前 Session 是否选择了具体模块。
   *
   * mid 是模块过滤的权威状态；名称仅用于显示。
   */
  bool has_selected_module() const noexcept {
    return _selected_mid.has_value();
  }
};

/*
 * 命令处理函数。
 *
 * Handler 同步执行并按值返回完整输出。
 * std::string 可以保存 UTF-8 和内嵌 NUL。
 *
 * 调用 Handler 时必须：
 * 1. 在注册表锁内复制 Handler；
 * 2. 释放注册表锁；
 * 3. 再调用复制出来的 Handler。
 */
using CliHandler = std::function<std::string(const CliArguments &)>;
using CliSessionHandler =
    std::function<std::string(CliSessionContext &, const CliArguments &)>;

struct CliDispatchResult final {
  CliDispatchStatus _status{CliDispatchStatus::HANDLER_ERROR};
  std::string _output;
};

struct CliCommandInfo final {
  std::string _name;
  std::string _help;
};
} // namespace TLSSMON

#endif // __CLI_TYPES_H__
