#include "cli_commands.h"
#include "cli_registry.h"
#include "cli_server.h"
#include "engine.h"
#include "monitor_data.h"
#include "monitor_error.h"
#include "timer_types.h"
#include "udp_publisher.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

using namespace std::chrono_literals;

namespace {
constexpr std::uint16_t CLI_PORT = 9001U;
constexpr std::uint16_t UDP_DESTINATION_PORT = 9100U;

constexpr std::uint32_t MODULE_A_ID = 0x100U;
constexpr auto HEARTBEAT_INTERVAL = 1s;
constexpr TLSSMON::MonData::MonitorKey HEARTBEAT_KEY{MODULE_A_ID, 99U, 1U, 0U};
constexpr TLSSMON::MonData::MonitorKey ERROR_KEY{MODULE_A_ID, 0U, 1U, 1U};
bool register_demo_module(TLSSMON::Engine &engine) {
  const TLSSMON::ModuleRegisterStatus status = engine.register_module(
      {MODULE_A_ID,
       "module-a",
       "metadata service",
       {{TLSSMON::MonitorLevel::INFO, "heartbeat"},
        {TLSSMON::MonitorLevel::WARN, "metadata timeout"},
        {TLSSMON::MonitorLevel::EMERGENCY_STOP, "metadata unavailable"}}});
  if (status != TLSSMON::ModuleRegisterStatus::SUCCESS) {
    std::cerr << "[demo] failed to register module-a: status="
              << static_cast<int>(status) << '\n';

    return false;
  }

  return true;
}
std::shared_ptr<TLSSMON::Wire::UdpPublisher>
install_udp_publisher(TLSSMON::Engine &engine) {
  auto publisher = std::make_shared<TLSSMON::Wire::UdpPublisher>(
      TLSSMON::Wire::UdpPublisherConfig{
          TLSSMON::Wire::UdpEndpoint{"127.0.0.1", UDP_DESTINATION_PORT},
          TLSSMON::Wire::WireVersion::V2});

  if (!publisher->ready()) {
    std::cerr << "[demo] failed to initialize UDP publisher\n";

    return {};
  }

  const bool installed =
      engine.set_publisher([publisher](TLSSMON::MonData::StoredRecord record) {
        const TLSSMON::Wire::UdpPublishResult result = publisher->send(record);

        if (!result.success()) {
          std::cerr << "[udp] send failed: status="
                    << static_cast<int>(result._status)
                    << " system_error=" << result._system_error << '\n';
        }
      });

  if (!installed) {
    std::cerr << "[demo] Engine rejected UDP publisher\n";

    return {};
  }

  return publisher;
}

bool register_heartbeat_timer(TLSSMON::Engine &engine,
                              std::atomic<std::uint32_t> &heartbeat_count) {
  const std::optional<TLSSMON::TimerHandle> handle = engine.set_timer(
      TLSSMON::MonCallback{
          "m8-heartbeat",
          [&engine, &heartbeat_count]() -> int {
            const std::uint32_t value =
                heartbeat_count.fetch_add(1U, std::memory_order_relaxed) + 1U;

            /*
             * 不传 description。
             *
             * Reporter 应从 Engine Registry 补全：
             *
             * level = INFO
             * description = "heartbeat"
             */
            const TLSSMON::MonData::UpdateResult result =
                engine.report_count(HEARTBEAT_KEY, value);

            std::cout << "[module-a] heartbeat=" << value
                      << " update_status=" << static_cast<int>(result._status)
                      << '\n'
                      << std::flush;

            return 0;
          },
          false},
      TLSSMON::TimerFlags::RECURRING, HEARTBEAT_INTERVAL);

  if (!handle.has_value()) {
    std::cerr << "[demo] failed to register heartbeat timer\n";

    return false;
  }

  return true;
}

bool register_stop_command(TLSSMON::CliRegistry &registry,
                           TLSSMON::Engine &engine) {
  const TLSSMON::CliRegisterStatus status = registry.register_command(
      "stop", "stop monitor engine",
      [&engine](const TLSSMON::CliArguments &) -> std::string {
        std::cout << "[demo] stop requested from CLI\n" << std::flush;

        engine.stop();
        return "stopping\n";
      });

  if (status != TLSSMON::CliRegisterStatus::SUCCESS) {
    std::cerr << "[demo] failed to register stop command\n";

    return false;
  }

  return true;
}

bool register_demo_commands(TLSSMON::CliRegistry &registry,
                            TLSSMON::Engine &engine) {
  if (TLSSMON::register_help_command(registry) !=
      TLSSMON::CliRegisterStatus::SUCCESS) {
    return false;
  }

  /*
   * 必须使用 Engine 重载。
   *
   * 不能再创建或传入外部 MonitorModuleRegistry。
   */
  if (TLSSMON::register_modules_command(registry, engine) !=
      TLSSMON::CliRegisterStatus::SUCCESS) {
    return false;
  }

  if (TLSSMON::register_use_command(registry, engine) !=
      TLSSMON::CliRegisterStatus::SUCCESS) {
    return false;
  }

  if (TLSSMON::register_db_dump_command(registry, engine) !=
      TLSSMON::CliRegisterStatus::SUCCESS) {
    return false;
  }

  return register_stop_command(registry, engine);
}

} // namespace
int main() {
  TLSSMON::Engine engine{TLSSMON::MonConfig{"monitor-m8-demo", CLI_PORT, 1U}};

  TLSSMON::CliRegistry registry;

  if (engine.init() != TLSSMON::ENGINESTATE::SUCCESSFUL) {
    std::cerr << "[demo] Engine initialization failed\n";

    return 1;
  }

  /*
   * init() 成功后处于 READY，此时注册模块。
   */
  if (!register_demo_module(engine)) {
    return 1;
  }

  if (!register_demo_commands(registry, engine)) {
    std::cerr << "[demo] failed to register CLI commands\n";

    return 1;
  }

  TLSSMON::CliServer cli{engine, registry,
                         TLSSMON::CliServerConfig{engine.cli_port(),
                                                  TLSSMON::CLI_MAX_CLIENTS,
                                                  "storage"}};

  if (!cli.start()) {
    std::cerr << "[demo] failed to start CLI server\n";

    return 1;
  }

  /*
   * 安装 V2 Publisher。
   */
  const std::shared_ptr<TLSSMON::Wire::UdpPublisher> udp_publisher =
      install_udp_publisher(engine);

  if (!udp_publisher) {
    cli.close();
    return 1;
  }

  /*
   * 首次错误零值。
   *
   * report_error() 使用 force=true，因此必须插入；
   * Registry 把 level 规范化为 WARN，并补全 description。
   */
  const TLSSMON::MonData::UpdateResult initial_error =
      engine.report_error(ERROR_KEY, 0U);

  if (initial_error._status != TLSSMON::MonData::UpdateStatus::INSERTED) {
    std::cerr << "[demo] failed to report initial error: status="
              << static_cast<int>(initial_error._status) << '\n';

    cli.close();
    return 1;
  }

  std::atomic<std::uint32_t> heartbeat_count{0U};

  if (!register_heartbeat_timer(engine, heartbeat_count)) {
    cli.close();
    return 1;
  }

  std::cout << "[demo] M8 module registered\n"
            << "[demo] CLI listening on 127.0.0.1:" << cli.bound_port() << '\n'
            << "[demo] connect with: nc 127.0.0.1 " << cli.bound_port() << '\n'
            << "[demo] commands: "
               "help, modules, use, db_dump, stop\n"
            << "[demo] module: module-a\n"
            << "[demo] UDP V2 destination: 127.0.0.1:" << UDP_DESTINATION_PORT
            << '\n'
            << std::flush;

  const TLSSMON::ENGINESTATE run_result = engine.run();

  cli.close();

  if (run_result != TLSSMON::ENGINESTATE::SUCCESSFUL) {
    std::cerr << "[demo] Engine stopped with error: "
              << static_cast<int>(run_result) << '\n';

    return 1;
  }

  std::cout << "[demo] Engine stopped successfully\n"
            << "[demo] final records=" << engine.query_data().size() << '\n'
            << std::flush;

  return 0;
}
