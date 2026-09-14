#ifndef __CLI_COMMANDS_H__
#define __CLI_COMMANDS_H__

#include "cli_registry.h"

namespace TLSSMON {

class MonitorModuleRegistry;
class Engine;
class ReliableAlarmPublisher;
class ReliableAlarmCollector;

/*
 * 注册内置 help 命令。
 *
 * 成功时返回 SUCCESS。
 * 如果 help 已存在，返回 DUPLICATE_COMMAND。
 *
 * registry 的生命周期必须覆盖所有 help 调用。
 */
CliRegisterStatus register_help_command(CliRegistry &registry);
/*
 * 注册内置 db_dump 命令。
 *
 * Handler 持有 Engine 非拥有引用，因此 Engine 必须比
 * CliRegistry 和 Handler 活得更久。
 */
CliRegisterStatus register_db_dump_command(CliRegistry &registry,
                                           Engine &engine);
/*
 * 用于单独测试使用
 * 或者外部单独调用
 * */
CliRegisterStatus register_modules_command(CliRegistry &registry,
                                           MonitorModuleRegistry &modules);
CliRegisterStatus register_use_command(CliRegistry &registry,
                                       MonitorModuleRegistry &modules);
/*
 * Handler 通过 Engine 查询模块，确保 CLI、Reporter 和
 * description 回填使用同一份模块注册表。
 *
 * Engine 生命周期必须长于 CliRegistry。
 */
CliRegisterStatus register_modules_command(CliRegistry &registry,
                                           Engine &engine);
CliRegisterStatus register_use_command(CliRegistry &registry, Engine &engine);

/*
 * publisher 和 collector 都是非拥有指针。
 *
 * 非空对象必须比 CliRegistry 及其正在执行的 Handler 活得更久。
 * 两者都为空时，命令输出 alarm channel inactive。
 */
CliRegisterStatus
register_alarm_status_command(CliRegistry &registry,
                              const ReliableAlarmPublisher *publisher,
                              const ReliableAlarmCollector *collector);

} // namespace TLSSMON
#endif // __CLI_COMMANDS_H__
