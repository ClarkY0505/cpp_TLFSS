#ifndef __CLI_COMMANDS_H__
#define __CLI_COMMANDS_H__

#include "cli_registry.h"

namespace TLSSMON {

class MonitorModuleRegistry;
class Engine;
class ReliableAlarmPublisher;
class ReliableAlarmCollector;

/**
 * @brief 注册内置 help 命令。
 *
 * 成功时返回 SUCCESS。
 * 如果 help 已存在，返回 DUPLICATE_COMMAND。
 *
 * registry 的生命周期必须覆盖所有 help 调用。
 * @param registry 调用方持有的注册表；其生命周期须覆盖当前对象或注册的回调。
 * @return 内置命令的注册状态。
 */
CliRegisterStatus register_help_command(CliRegistry &registry);
/**
 * @brief 注册内置 db_dump 命令。
 *
 * Handler 持有 Engine 非拥有引用，因此 Engine 必须比
 * CliRegistry 和 Handler 活得更久。
 * @param registry 调用方持有的注册表；其生命周期须覆盖当前对象或注册的回调。
 * @param engine 用于注册事件或读写监控数据的 Engine；由调用方持有。
 * @return 内置命令的注册状态。
 */
CliRegisterStatus register_db_dump_command(CliRegistry &registry,
                                           Engine &engine);
/**
 * @brief 使用独立模块注册表注册 modules 命令；注册表须比 Handler 存活更久。
 * @param registry 调用方持有的注册表；其生命周期须覆盖当前对象或注册的回调。
 * @param modules 用于查询或补齐模块元数据的注册表；由调用方持有。
 * @return 内置命令的注册状态。
 */
CliRegisterStatus register_modules_command(CliRegistry &registry,
                                           MonitorModuleRegistry &modules);
/**
 * @brief 使用独立模块注册表注册 use 命令。
 * @param registry 调用方持有的注册表；其生命周期须覆盖当前对象或注册的回调。
 * @param modules 用于查询或补齐模块元数据的注册表；由调用方持有。
 * @return 内置命令的注册状态。
 */
CliRegisterStatus register_use_command(CliRegistry &registry,
                                       MonitorModuleRegistry &modules);
/**
 * @brief Handler 通过 Engine 查询模块，确保 CLI、Reporter 和
 * description 回填使用同一份模块注册表。
 *
 * Engine 生命周期必须长于 CliRegistry。
 * @param registry 调用方持有的注册表；其生命周期须覆盖当前对象或注册的回调。
 * @param engine 用于注册事件或读写监控数据的 Engine；由调用方持有。
 * @return 内置命令的注册状态。
 */
CliRegisterStatus register_modules_command(CliRegistry &registry,
                                           Engine &engine);
/**
 * @brief 通过 Engine 的模块注册表注册 use 命令。
 * @param registry 调用方持有的注册表；其生命周期须覆盖当前对象或注册的回调。
 * @param engine 用于注册事件或读写监控数据的 Engine；由调用方持有。
 * @return 内置命令的注册状态。
 */
CliRegisterStatus register_use_command(CliRegistry &registry, Engine &engine);

/**
 * @brief publisher 和 collector 都是非拥有指针。
 *
 * 非空对象必须比 CliRegistry 及其正在执行的 Handler 活得更久。
 * 两者都为空时，命令输出 alarm channel inactive。
 * @param registry 调用方持有的注册表；其生命周期须覆盖当前对象或注册的回调。
 * @param publisher 可选的可靠告警发送端指针；不转移所有权。
 * @param collector 可选的可靠告警接收端指针；不转移所有权。
 * @return 内置命令的注册状态。
 */
CliRegisterStatus
register_alarm_status_command(CliRegistry &registry,
                              const ReliableAlarmPublisher *publisher,
                              const ReliableAlarmCollector *collector);

} // namespace TLSSMON
#endif // __CLI_COMMANDS_H__
