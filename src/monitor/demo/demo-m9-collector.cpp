#include "cli_commands.h"
#include "cli_registry.h"
#include "cli_server.h"
#include "demo_modules.h"
#include "engine.h"
#include "monitor_collector.h"
#include "reliable_alarm_collector.h"
#include "udp_receiver.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

using namespace std::chrono_literals;

namespace {

constexpr std::uint16_t METRIC_UDP_PORT = 9300U;
constexpr std::uint16_t ALARM_TCP_PORT = 9400U;
constexpr std::uint16_t COLLECTOR_CLI_PORT = 9310U;

int drain_udp_receiver(
    TLSSMON::Engine &engine,
    const std::weak_ptr<TLSSMON::Wire::UdpReceiver> &weak_receiver) {
  const auto receiver = weak_receiver.lock();

  if (!receiver) {
    return 0;
  }

  for (;;) {
    TLSSMON::Wire::UdpReceiveResult received = receiver->receive_one();

    if (received._status == TLSSMON::Wire::UdpReceiveStatus::WOULD_BLOCK) {
      return 0;
    }

    if (!received.success() || !received._record.has_value()) {
      /*
       * 一个错误数据报已经被消费。
       * 继续 drain，不能影响后面的合法数据报。
       */
      std::cerr << "[collector/udp] receive failed"
                << " receive_status=" << static_cast<int>(received._status)
                << " wire_status=" << static_cast<int>(received._wire_status)
                << " errno=" << received._system_error << '\n';

      if (received._status == TLSSMON::Wire::UdpReceiveStatus::RECEIVE_ERROR ||
          received._status == TLSSMON::Wire::UdpReceiveStatus::SOCKET_ERROR) {
        return 0;
      }

      continue;
    }

    const TLSSMON::MonData::MonitorKey key = received._record->_data._key;

    /*
     * force=true：
     * Collector 应保存生产端已经决定发布的初始零值。
     *
     * V2 changed_at 会由 ingest_decoded_record() 保留。
     */
    const TLSSMON::MonData::UpdateResult updated =
        TLSSMON::ingest_decoded_record(engine, std::move(*received._record),
                                       true);

    /*
     * 只记录 Key 和结果，不输出字符串 payload。
     */
    std::cout << "[collector/udp]" << " mid=" << key._mid << " fid=" << key._fid
              << " eid=" << key._eid
              << " update=" << static_cast<int>(updated._status) << '\n';
  }
}

bool register_modules(TLSSMON::Engine &engine) {
  if (engine.register_module(TLSSMON::DemoM9::disk_module()) !=
      TLSSMON::ModuleRegisterStatus::SUCCESS) {
    return false;
  }

  return engine.register_module(TLSSMON::DemoM9::network_module()) ==
         TLSSMON::ModuleRegisterStatus::SUCCESS;
}

bool register_commands(TLSSMON::CliRegistry &registry, TLSSMON::Engine &engine,
                       const TLSSMON::ReliableAlarmCollector *collector) {
  if (TLSSMON::register_help_command(registry) !=
      TLSSMON::CliRegisterStatus::SUCCESS) {
    return false;
  }

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

  if (TLSSMON::register_alarm_status_command(registry, nullptr, collector) !=
      TLSSMON::CliRegisterStatus::SUCCESS) {
    return false;
  }

  return registry.register_command(
             "stop", "stop collector",
             [&engine](const TLSSMON::CliArguments &arguments) -> std::string {
               if (!arguments.empty()) {
                 return "usage: stop\n";
               }

               engine.stop();
               return "stopping\n";
             }) == TLSSMON::CliRegisterStatus::SUCCESS;
}

} // namespace

int main(int argc, char **argv) {
  if (argc > 2) {
    std::cerr << "usage: " << argv[0] << " [inbox-dir]\n";
    return 1;
  }

  const std::filesystem::path inbox =
      argc == 2 ? std::filesystem::path{argv[1]}
                : std::filesystem::path{"./m9-data/collector-inbox"};

  TLSSMON::Engine engine{
      TLSSMON::MonConfig{"monitor-m9-collector", COLLECTOR_CLI_PORT, 10U}};

  if (engine.init() != TLSSMON::ENGINESTATE::SUCCESSFUL) {
    std::cerr << "[collector] Engine initialization failed\n";
    return 1;
  }

  if (!register_modules(engine)) {
    std::cerr << "[collector] module registration failed\n";
    return 1;
  }

  auto udp_receiver = std::make_shared<TLSSMON::Wire::UdpReceiver>(
      TLSSMON::Wire::UdpReceiverConfig{"127.0.0.1", METRIC_UDP_PORT});

  if (!udp_receiver->ready()) {
    std::cerr << "[collector] UDP receiver initialization failed"
              << " status=" << static_cast<int>(udp_receiver->setup_status())
              << '\n';
    return 1;
  }

  const std::weak_ptr<TLSSMON::Wire::UdpReceiver> weak_receiver{udp_receiver};

  const auto udp_handle = engine.add_aio(
      udp_receiver->fd(),
      TLSSMON::MonCallback{"m9-udp-collector",
                           [&engine, weak_receiver]() -> int {
                             return drain_udp_receiver(engine, weak_receiver);
                           },
                           false});

  if (!udp_handle.has_value()) {
    std::cerr << "[collector] failed to register UDP AIO\n";
    return 1;
  }

  auto alarm_collector = std::make_unique<TLSSMON::ReliableAlarmCollector>(
      engine, TLSSMON::ReliableAlarmCollectorConfig{"127.0.0.1", ALARM_TCP_PORT,
                                                    inbox, 128U, 30s});

  if (!alarm_collector->ready()) {
    std::cerr << "[collector] reliable collector initialization failed"
              << " status=" << static_cast<int>(alarm_collector->setup_status())
              << " errno=" << alarm_collector->setup_error() << '\n';
    return 1;
  }

  TLSSMON::CliRegistry registry;

  if (!register_commands(registry, engine, alarm_collector.get())) {
    std::cerr << "[collector] CLI command registration failed\n";
    return 1;
  }

  TLSSMON::CliServer cli{engine, registry,
                         TLSSMON::CliServerConfig{COLLECTOR_CLI_PORT,
                                                  TLSSMON::CLI_MAX_CLIENTS,
                                                  "collector"}};

  if (!cli.start()) {
    std::cerr << "[collector] CLI startup failed\n";
    return 1;
  }

  std::cout << "[collector] UDP metrics=127.0.0.1:"
            << udp_receiver->bound_port() << '\n'
            << "[collector] TCP alarms=127.0.0.1:"
            << alarm_collector->bound_port() << '\n'
            << "[collector] CLI=127.0.0.1:" << cli.bound_port() << '\n'
            << "[collector] inbox=" << inbox << '\n'
            << "[collector] commands: help modules use "
               "db_dump alarm_status stop\n"
            << std::flush;

  const TLSSMON::ENGINESTATE run_result = engine.run();

  /*
   * 关闭顺序：
   *
   * 1. Engine 已停止并等待完 AIO/Timer worker；
   * 2. 释放 UDP Receiver；
   * 3. stop/join 可靠 Collector；
   * 4. 关闭 CLI；
   * 5. main 返回后析构 Engine。
   */
  udp_receiver.reset();

  alarm_collector->stop();
  alarm_collector.reset();

  cli.close();

  if (run_result != TLSSMON::ENGINESTATE::SUCCESSFUL) {
    std::cerr << "[collector] Engine stopped with status="
              << static_cast<int>(run_result) << '\n';
    return 1;
  }

  std::cout << "[collector] stopped successfully\n";
  return 0;
}
