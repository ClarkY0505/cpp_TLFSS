#ifndef __CLI_COMMANDS_H__
#define __CLI_COMMANDS_H__

#include "cli_registry.h"

namespace TLSSMON {

class MonitorModuleRegistry;
class Engine;

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
CliRegisterStatus register_modules_command(CliRegistry &registry,
                                           MonitorModuleRegistry &modules);
CliRegisterStatus register_use_command(CliRegistry &registry,
                                       MonitorModuleRegistry &modules);

} // namespace TLSSMON
#endif // __CLI_COMMANDS_H__
