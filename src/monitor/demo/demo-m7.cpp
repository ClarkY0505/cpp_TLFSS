#include "cli_commands.h"
#include "cli_registry.h"
#include "cli_server.h"
#include "engine.h"
#include "monitor_data.h"
#include "monitor_module_registry.h"
#include "timer_types.h"
#include "udp_publisher.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {
constexpr std::uint16_t CLI_PORT = 9000U;
constexpr std::uint16_t UDP_DESTINATION_PORT = 9100U;

constexpr auto HEARTBEAT_INTERVAL = 1s;
constexpr auto SLOW_TIMER_INTERVAL = 1500ms;
constexpr auto SLOW_WORK_DURATION = 400ms;

/*
 * 两个受监控模块的 mid。
 */
constexpr std::uint32_t MODULE_A_ID = 0x100U;
constexpr std::uint32_t MODULE_B_ID = 0x200U;

/*
 * Timer 参数。
 */
constexpr auto MODULE_A_INTERVAL = 1s;
constexpr auto MODULE_B_INTERVAL = 1500ms;
constexpr auto MODULE_B_WORK_DURATION = 400ms;

/*
 * module-a：
 *
 * 模拟元数据和命名空间服务的 heartbeat。
 */
constexpr TLSSMON::MonData::MonitorKey MODULE_A_KEY{MODULE_A_ID, 1U, 1U, 1U};

/*
 * module-b：
 *
 * 模拟复制和数据块服务的异步 worker。
 */
constexpr TLSSMON::MonData::MonitorKey MODULE_B_KEY{MODULE_B_ID, 1U, 1U, 1U};

std::shared_ptr<TLSSMON::Wire::UdpPublisher>
install_udp_publisher(TLSSMON::Engine &engine) {
  auto publisher = std::make_shared<TLSSMON::Wire::UdpPublisher>(
      TLSSMON::Wire::UdpPublisherConfig{
          TLSSMON::Wire::UdpEndpoint{"127.0.0.1", UDP_DESTINATION_PORT},
          TLSSMON::Wire::WireVersion::V2});

  if (!publisher->ready()) {
    std::cerr << "[demo] failed to initialize UDP Publisher\n";
    return {};
  }

  if (!engine.set_publisher([publisher](TLSSMON::MonData::StoredRecord record) {
        const TLSSMON::Wire::UdpPublishResult result = publisher->send(record);
        if (!result.success()) {
          std::cerr << "[udp] send failed: status="
                    << static_cast<int>(result._status)
                    << " system_error=" << result._system_error << '\n';
        }
      })) {
    std::cerr << "[demo] Engine rejected UDP Publisher\n";
    return {};
  }

  return publisher;
}

bool register_module_a_timer(TLSSMON::Engine &engine,
                              std::atomic<std::uint32_t> &heartbeat_count) {
  const auto handle = engine.set_timer(
      TLSSMON::MonCallback{
          "module-a-heartbeat",
          [&engine, &heartbeat_count]() -> int {
            const std::uint32_t value =
                heartbeat_count.fetch_add(1U, std::memory_order_relaxed) + 1U;
            const TLSSMON::MonData::UpdateResult result =
                engine.report_count(MODULE_A_KEY, value, "module-a heartbeat");

            std::cout << "[module-a] value=" << value
                      << " update_status=" << static_cast<int>(result._status)
                      << '\n'
                      << std::flush;
            return 0;
          },
          false},
      TLSSMON::TimerFlags::RECURRING, MODULE_A_INTERVAL);

  if (!handle.has_value()) {
    std::cerr << "[demo] failed to register heartbeat timer\n";
    return false;
  }
  return true;
}

bool register_module_b_timer(TLSSMON::Engine &engine,
                                std::atomic<std::uint32_t> &worker_count) {
  const auto handle = engine.set_timer(
      TLSSMON::MonCallback{
          "slow",
          [&engine, &worker_count]() -> int {
            std::cout << "[module-b] start\n" << std::flush;
            std::this_thread::sleep_for(SLOW_WORK_DURATION);

            const std::uint32_t sequence =
                worker_count.fetch_add(1U, std::memory_order_relaxed) + 1U;
            const std::string value =
                "worker-cycle-" + std::to_string(sequence);
            const TLSSMON::MonData::UpdateResult result = engine.report_string(
                MODULE_B_KEY, value, "module-b asynchronous worker");

            std::cout << "[module-b] value=" << value
                      << " update_status=" << static_cast<int>(result._status)
                      << '\n'
                      << std::flush;
            return 0;
          },
          false},
      TLSSMON::TimerFlags::RECURRING | TLSSMON::TimerFlags::WORKER,
      SLOW_TIMER_INTERVAL);

  if (!handle.has_value()) {
    std::cerr << "[demo] failed to register slow worker timer\n";
    return false;
  }
  return true;
}

bool register_demo_modules(TLSSMON::MonitorModuleRegistry &modules) {
  const TLSSMON::ModuleRegisterStatus module_a_status = modules.register_module(
      {MODULE_A_ID, "module-a", "metadata and namespace service"});
  if (module_a_status != TLSSMON::ModuleRegisterStatus::SUCCESS) {
    std::cerr << "[demo] failed to register module-a: status="
              << static_cast<int>(module_a_status) << '\n';

    return false;
  }

  const TLSSMON::ModuleRegisterStatus module_b_status = modules.register_module(
      {MODULE_B_ID, "module-b", "replication and chunk service"});

  if (module_b_status != TLSSMON::ModuleRegisterStatus::SUCCESS) {
    std::cerr << "[demo] failed to register module-b: status="
              << static_cast<int>(module_b_status) << '\n';

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

        /*
         * CliRegistry 已经释放内部互斥锁，Handler 可以安全重新进入 Engine。
         * stop() 只提交停止请求；run() 随后负责等待 worker 并清理资源。
         */
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
                            TLSSMON::MonitorModuleRegistry &modules,
                            TLSSMON::Engine &engine) {
  if (TLSSMON::register_help_command(registry) !=
      TLSSMON::CliRegisterStatus::SUCCESS) {
    std::cerr << "[demo] failed to register help command\n";
    return false;
  }

  if (TLSSMON::register_modules_command(registry, modules) !=
      TLSSMON::CliRegisterStatus::SUCCESS) {
    std::cerr << "[demo] failed to register modules command\n";
    return false;
  }

  if (TLSSMON::register_use_command(registry, modules) !=
      TLSSMON::CliRegisterStatus::SUCCESS) {
    std::cerr << "[demo] failed to register use command\n";
    return false;
  }

  if (TLSSMON::register_db_dump_command(registry, engine) !=
      TLSSMON::CliRegisterStatus::SUCCESS) {
    std::cerr << "[demo] failed to register db_dump command\n";
    return false;
  }

  if (!register_stop_command(registry, engine)) {
    return false;
  }

  return true;
}

} // namespace

int main() {

  TLSSMON::Engine engine{TLSSMON::MonConfig{"monitor-m7-demo", CLI_PORT, 1U}};

  TLSSMON::MonitorModuleRegistry modules;
  TLSSMON::CliRegistry registry;

  /*
   * 初始化 Engine。
   */
  const TLSSMON::ENGINESTATE init_result = engine.init();

  if (init_result != TLSSMON::ENGINESTATE::SUCCESSFUL) {
    std::cerr << "[demo] Engine initialization failed: "
              << static_cast<int>(init_result) << '\n';

    return 1;
  }

  /*
   * 注册两个受监控模块。
   */
  if (!register_demo_modules(modules)) {
    return 1;
  }

  /*
   * 注册 help、modules、use、db_dump 和 stop。
   */
  if (!register_demo_commands(registry, modules, engine)) {
    return 1;
  }

  /*
   * 一个 Engine 只创建一个 CLI 端口。
   *
   * 每个连接通过 use 命令维护自己的模块选择。
   */
  TLSSMON::CliServer cli{engine, registry,
                         TLSSMON::CliServerConfig{engine.cli_port(),
                                                  TLSSMON::CLI_MAX_CLIENTS,
                                                  "storage"}};

  if (!cli.start()) {
    std::cerr << "[demo] failed to start CLI Server\n";
    return 1;
  }

  std::cout << "[demo] CLI listening on 127.0.0.1:" << cli.bound_port() << '\n'
            << "[demo] connect with: nc 127.0.0.1 " << cli.bound_port() << '\n'
            << "[demo] commands: "
               "help, modules, use, db_dump, stop\n"
            << "[demo] modules: module-a, module-b\n"
            << "[demo] UDP V2 destination: 127.0.0.1:" << UDP_DESTINATION_PORT
            << '\n'
            << std::flush;

  /*
   * 安装 UDP V2 Publisher。
   *
   * Store 去重后，只有 INSERTED/UPDATED 记录会进入 Publisher。
   */
  const std::shared_ptr<TLSSMON::Wire::UdpPublisher> udp_publisher =
      install_udp_publisher(engine);

  if (!udp_publisher) {
    cli.close();
    return 1;
  }

  std::atomic<std::uint32_t> heartbeat_count{0U};
  std::atomic<std::uint32_t> worker_count{0U};

  /*
   * module-a 使用同步 Timer。
   * module-b 使用异步 WORKER Timer。
   */
  if (!register_module_a_timer(engine, heartbeat_count) ||
      !register_module_b_timer(engine, worker_count)) {
    cli.close();
    return 1;
  }

  /*
   * 阻塞运行 Engine。
   *
   * Demo 不自动停止，需要连接 CLI 后执行 stop。
   */
  const TLSSMON::ENGINESTATE run_result = engine.run();

  /*
   * run() 返回时 Engine 已清理 AIO 回调。
   * 此时关闭 CliServer 的 Listener 和残留 Session。
   */
  cli.close();

  if (run_result != TLSSMON::ENGINESTATE::SUCCESSFUL) {
    std::cerr << "[demo] Engine stopped with error: "
              << static_cast<int>(run_result) << '\n';

    return 1;
  }

  /*
   * Engine 停止后 Store 仍保留最后一次记录。
   */
  std::cout << "[demo] Engine stopped successfully\n"
            << "[demo] final records=" << engine.query_data().size() << '\n'
            << std::flush;

  return 0;
}
