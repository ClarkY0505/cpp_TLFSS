#include "cli_commands.h"
#include "cli_registry.h"
#include "cli_types.h"
#include "engine.h"
#include "monitor_data.h"
#include "monitor_module_registry.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace TLSSMON {
namespace {

constexpr const char *HELP_COMMAND_NAME = "help";
constexpr const char *HELP_COMMAND_DESCRIPTION = "list available commands";
constexpr const char *DB_DUMP_COMMAND_NAME = "db_dump";
constexpr const char *DB_DUMP_COMMAND_DESCRIPTION = "dump monitor records";
constexpr const char *MODULES_COMMAND_NAME = "modules";
constexpr const char *MODULES_COMMAND_DESCRIPTION = "list registered modules";
constexpr const char *MODULES_COMMAND_USAGE = "usage: modules\n";
constexpr const char *USE_COMMAND_NAME = "use";
constexpr const char *USE_COMMAND_DESCRIPTION = "select active module";
constexpr const char *USE_COMMAND_USAGE = "usage: use <module|all>\n";
constexpr std::int64_t NANOSECONDS_PER_SECOND = INT64_C(1'000'000'000);

static_assert(std::is_same_v<MonData::MonitorTimestamp::duration,
                             std::chrono::nanoseconds>,
              "MonitorTimestamp must use nanosecond precision");

static_assert(std::is_signed_v<MonData::MonitorTimestamp::duration::rep>,
              "MonitorTimestamp representation must be signed");

std::string format_modules(const std::vector<MonitorModuleInfo> &modules) {
  if (modules.empty()) {
    return "0 modules\n";
  }

  std::string output;

  for (const MonitorModuleInfo &module : modules) {
    output += module._name;
    output += " mid=";
    output += std::to_string(module._mid);
    output.push_back('\n');
  }

  return output;
}

std::string format_help(const std::vector<CliCommandInfo> &commands) {
  std::size_t maximum_name_length = 0U;

  for (const CliCommandInfo &command : commands) {
    maximum_name_length = std::max(maximum_name_length, command._name.size());
  }

  std::string output;

  for (const CliCommandInfo &command : commands) {
    output += command._name;
    if (!command._help.empty()) {
      const std::size_t padding =
          maximum_name_length - command._name.size() + 4U;
      output.append(padding, ' ');
      output += command._help;
    }

    output.push_back('\n');
  }
  return output;
}

std::string escape_cli_text(const std::string &input) {
  constexpr char hex_digits[] = "0123456789ABCDEF";

  std::string output;
  output.reserve(input.size());

  for (const char character : input) {
    const auto byte = static_cast<unsigned char>(character);

    switch (byte) {
    case '"':
      output += "\\\"";
      break;

    case '\\':
      output += "\\\\";
      break;

    case '\n':
      output += "\\n";
      break;

    case '\r':
      output += "\\r";
      break;

    case '\t':
      output += "\\t";
      break;

    default:
      if (byte < 0x20U || byte == 0x7FU) {
        output += "\\x";
        output.push_back(hex_digits[(byte >> 4U) & 0x0FU]);
        output.push_back(hex_digits[byte & 0x0FU]);
      } else {
        output.push_back(character);
      }

      break;
    }
  }

  return output;
}

std::string format_timestamp(MonData::MonitorTimestamp timestamp) {
  const std::int64_t total_nanoseconds =
      static_cast<std::int64_t>(timestamp.time_since_epoch().count());
  std::int64_t seconds = total_nanoseconds / NANOSECONDS_PER_SECOND;
  std::int64_t nanoseconds = total_nanoseconds % NANOSECONDS_PER_SECOND;

  if (nanoseconds < 0) {
    --seconds;
    nanoseconds += NANOSECONDS_PER_SECOND;
  }

  std::string fraction = std::to_string(nanoseconds);
  fraction.insert(fraction.begin(), 9U - fraction.size(), '0');

  return std::to_string(seconds) + '.' + fraction;
}

void append_record(std::string &output, const MonData::StoredRecord &record) {
  const MonData::MonitorData &data = record._data;
  const MonData::MonitorKey &key = data._key;

  output += "mid=";
  output += std::to_string(key._mid);

  output += " lvl=";
  output += std::to_string(key._level);

  output += " fid=";
  output += std::to_string(key._fid);

  output += " eid=";
  output += std::to_string(key._eid);

  if (const auto *numeric = std::get_if<MonData::NumericValue>(&data._value)) {
    output += " num=";
    output += std::to_string(numeric->_value);

    output += " state=";
    output += std::to_string(numeric->_state);
  } else {
    const std::string &value = std::get<std::string>(data._value);

    output += " str=\"";
    output += escape_cli_text(value);
    output += '"';
  }

  output += " desc=\"";
  output += escape_cli_text(data._description);
  output += '"';

  output += " changed_at=";
  output += format_timestamp(record._changed_at);
  output.push_back('\n');
}

std::string format_db_dump(const std::vector<MonData::StoredRecord> &records) {
  std::string output;
  for (const MonData::StoredRecord &record : records) {
    append_record(output, record);
  }

  output += std::to_string(records.size());
  output += " entries\n";

  return output;
}

} // namespace

CliRegisterStatus register_help_command(CliRegistry &registry) {
  /*
   * CliRegistry 按值保存 Handler。
   *
   * lambda 只保存 registry 的非拥有引用，因此要求 Registry
   * 在 help 命令可以被分派期间保持存活。
   */
  return registry.register_command(
      HELP_COMMAND_NAME, HELP_COMMAND_DESCRIPTION,
      [&registry](const CliArguments &) -> std::string {
        const std::vector<CliCommandInfo> snapshot = registry.commands();
        return format_help(snapshot);
      });
}

CliRegisterStatus register_db_dump_command(CliRegistry &registry,
                                           Engine &engine) {
  return registry.register_command(
      DB_DUMP_COMMAND_NAME, DB_DUMP_COMMAND_DESCRIPTION,
      [&engine](CliSessionContext &context,
                const CliArguments &) -> std::string {
        MonData::MonitorFilter filter;
        if (context._selected_mid.has_value()) {
          filter.module_id = context._selected_mid;
        }

        const std::vector<MonData::StoredRecord> snapshot =
            engine.query_data(filter);
        return format_db_dump(snapshot);
      });
}

CliRegisterStatus register_modules_command(CliRegistry &registry,
                                           MonitorModuleRegistry &modules) {
  return registry.register_command(
      MODULES_COMMAND_NAME, MODULES_COMMAND_DESCRIPTION,
      [&modules](const CliArguments &arguments) -> std::string {
        if (!arguments.empty()) {
          return MODULES_COMMAND_USAGE;
        }
        const std::vector<MonitorModuleInfo> snapshot = modules.modules();

        return format_modules(snapshot);
      });
}

CliRegisterStatus register_use_command(CliRegistry &registry,
                                       MonitorModuleRegistry &modules) {
  /*
   * use 需要修改当前连接的 CliSessionContext，
   * 因此这里必须注册 CliSessionHandler，而不是旧 CliHandler。
   */
  return registry.register_command(
      USE_COMMAND_NAME, USE_COMMAND_DESCRIPTION,
      [&modules](CliSessionContext &context,
                 const CliArguments &arguments) -> std::string {
        /*
         * 不带参数时只查询当前选择，不修改 Context。
         */
        if (arguments.empty()) {
          if (!context.has_selected_module()) {
            return "current module: all\n";
          }

          return "current module: " + context._selected_module_name + '\n';
        }

        /*
         * use 只接受零个或一个参数。
         *
         * 参数过多时必须保留原来的模块选择。
         */
        if (arguments.size() != 1U) {
          return USE_COMMAND_USAGE;
        }

        const std::string &requested_name = arguments[0];

        /*
         * all 是保留名称，用于清除当前选择。
         */
        if (requested_name == "all") {
          context.clear_module();
          return "using module: all\n";
        }

        /*
         * ModuleRegistry 返回 MonitorModuleInfo 副本。
         *
         * find_by_name() 返回时 Registry 锁已经释放，
         * 后续修改 Session 不会持有 ModuleRegistry 锁。
         */
        const std::optional<MonitorModuleInfo> module =
            modules.find_by_name(requested_name);

        /*
         * 未知模块不能改变当前选择。
         */
        if (!module.has_value()) {
          return "unknown module: " + requested_name + '\n';
        }

        /*
         * 先复制名称，再设置不会抛异常的整数 mid。
         *
         * 这样字符串复制失败时，不会先改变过滤使用的 mid。
         */
        context._selected_module_name = module->_name;
        context._selected_mid = module->_mid;

        return "using module: " + module->_name + '\n';
      });
}

} // namespace TLSSMON
