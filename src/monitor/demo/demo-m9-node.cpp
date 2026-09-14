#include "alarm_publisher_types.h"
#include "cli_commands.h"
#include "cli_registry.h"
#include "cli_server.h"
#include "demo_modules.h"
#include "engine.h"
#include "monitor_data.h"
#include "monitor_policy.h"
#include "reliable_alarm_publisher.h"
#include "timer_types.h"
#include "udp_publisher.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

using namespace std::chrono_literals;

namespace {

constexpr std::uint16_t METRIC_UDP_PORT = 9300U;
constexpr std::uint16_t ALARM_TCP_PORT = 9400U;
constexpr const char *COLLECTOR_HOST = "127.0.0.1";

struct NodeOptions final {
  const TLSSMON::MonitorModuleInfo *_module{nullptr};
  std::uint64_t _source_id{0U};
  std::filesystem::path _outbox;
  std::uint16_t _cli_port{0U};
};

struct NodeState final {
  std::atomic<bool> _sampling{true};
  std::chrono::steady_clock::time_point _started{
      std::chrono::steady_clock::now()};

  std::uint64_t _tick{0U};
  std::uint32_t _network_error_total{0U};

  std::unique_ptr<TLSSMON::MonitorPolicy> _temperature_policy{
      TLSSMON::make_smooth_trim_policy(4U, 20U)};

  std::unique_ptr<TLSSMON::MonitorPolicy> _disk_full_policy{
      TLSSMON::make_cont_for_policy(80U, TLSSMON::PolicyTime{3000})};

  std::unique_ptr<TLSSMON::MonitorPolicy> _network_burst_policy{
      TLSSMON::make_times_per_period_policy(0U, 3U, TLSSMON::PolicyTime{3000})};

  TLSSMON::PolicyTime logical_time() const {
    return std::chrono::duration_cast<TLSSMON::PolicyTime>(
        std::chrono::steady_clock::now() - _started);
  }
};

bool parse_source_id(const char *text, std::uint64_t &result) {
  if (text == nullptr || *text == '\0') {
    return false;
  }

  errno = 0;
  char *end = nullptr;
  const unsigned long long parsed = std::strtoull(text, &end, 0);

  if (errno != 0 || end == text || *end != '\0' || parsed == 0U) {
    return false;
  }

  result = static_cast<std::uint64_t>(parsed);
  return true;
}

bool parse_port(const char *text, std::uint16_t &result) {
  if (text == nullptr || *text == '\0') {
    return false;
  }

  errno = 0;
  char *end = nullptr;
  const unsigned long parsed = std::strtoul(text, &end, 10);

  if (errno != 0 || end == text || *end != '\0' || parsed == 0U ||
      parsed > 65535U) {
    return false;
  }

  result = static_cast<std::uint16_t>(parsed);
  return true;
}

std::optional<NodeOptions> parse_options(int argc, char **argv) {
  if (argc != 5) {
    return std::nullopt;
  }

  NodeOptions options;

  const std::string module_name = argv[1];

  if (module_name == "disk") {
    options._module = &TLSSMON::DemoM9::disk_module();
  } else if (module_name == "network") {
    options._module = &TLSSMON::DemoM9::network_module();
  } else {
    return std::nullopt;
  }

  if (!parse_source_id(argv[2], options._source_id)) {
    return std::nullopt;
  }

  options._outbox = argv[3];

  if (options._outbox.empty()) {
    return std::nullopt;
  }

  if (!parse_port(argv[4], options._cli_port)) {
    return std::nullopt;
  }

  return options;
}

void print_usage(const char *program) {
  std::cerr << "usage: " << program
            << " <disk|network> <source-id> <outbox-dir> <cli-port>\n";
}

TLSSMON::MonData::MonitorKey make_key(std::uint32_t mid, std::uint32_t eid) {
  /*
   * level 先填 0。
   * Reporter 会根据模块错误表修正 level 和 description。
   */
  return TLSSMON::MonData::MonitorKey{mid, 0U,
                                      TLSSMON::DemoM9::SAMPLE_FUNCTION_ID, eid};
}

std::shared_ptr<TLSSMON::Wire::UdpPublisher>
install_udp_publisher(TLSSMON::Engine &engine) {
  auto publisher = std::make_shared<TLSSMON::Wire::UdpPublisher>(
      TLSSMON::Wire::UdpPublisherConfig{{COLLECTOR_HOST, METRIC_UDP_PORT},
                                        TLSSMON::Wire::WireVersion::V2});

  if (!publisher->ready()) {
    std::cerr << "[node] UDP publisher initialization failed"
              << " status=" << static_cast<int>(publisher->setup_status())
              << '\n';
    return {};
  }

  const std::weak_ptr<TLSSMON::Wire::UdpPublisher> weak_publisher{publisher};

  if (!engine.set_publisher(
          [weak_publisher](TLSSMON::MonData::StoredRecord record) {
            const auto locked = weak_publisher.lock();

            if (!locked) {
              return;
            }

            const auto result = locked->send(record);

            if (!result.success()) {
              const auto &key = record._data._key;

              /*
               * 不输出 description 或字符串 payload。
               */
              std::cerr << "[udp] send failed" << " mid=" << key._mid
                        << " fid=" << key._fid << " eid=" << key._eid
                        << " status=" << static_cast<int>(result._status)
                        << " errno=" << result._system_error << '\n';
            }
          })) {
    std::cerr << "[node] Engine rejected UDP publisher\n";
    return {};
  }

  return publisher;
}

std::shared_ptr<TLSSMON::ReliableAlarmPublisher>
install_alarm_publisher(TLSSMON::Engine &engine, const NodeOptions &options) {
  auto publisher = std::make_shared<TLSSMON::ReliableAlarmPublisher>(
      TLSSMON::ReliableAlarmPublisherConfig{COLLECTOR_HOST, ALARM_TCP_PORT,
                                            options._source_id, options._outbox,
                                            64U * 1024U * 1024U, 2s, 30s});

  if (!publisher->ready()) {
    std::cerr << "[node] reliable publisher initialization failed"
              << " status=" << static_cast<int>(publisher->setup_status())
              << " errno=" << publisher->setup_error() << '\n';
    return {};
  }

  const std::weak_ptr<TLSSMON::ReliableAlarmPublisher> weak_publisher{
      publisher};

  if (!engine.set_alarm_publisher([weak_publisher](
                                      TLSSMON::MonData::StoredRecord record)
                                      -> TLSSMON::AlarmEnqueueResult {
        const auto locked = weak_publisher.lock();

        if (!locked) {
          return {TLSSMON::AlarmEnqueueStatus::NOT_READY, 0};
        }

        const TLSSMON::MonData::MonitorKey key = record._data._key;

        TLSSMON::AlarmEnqueueResult result = locked->enqueue(std::move(record));

        if (!result.durable()) {
          /*
           * 不输出告警字符串 payload。
           */
          std::cerr << "[alarm] enqueue failed" << " mid=" << key._mid
                    << " fid=" << key._fid << " eid=" << key._eid
                    << " status=" << static_cast<int>(result._status)
                    << " errno=" << result._system_error << '\n';
        }

        return result;
      })) {
    std::cerr << "[node] Engine rejected reliable publisher\n";
    return {};
  }

  return publisher;
}

int sample_disk(TLSSMON::Engine &engine, NodeState &state) {
  static constexpr std::array<std::uint32_t, 10U> temperatures{
      45U, 46U, 47U, 120U, 48U, 49U, 50U, 51U, 52U, 53U};

  static constexpr std::array<std::uint32_t, 10U> used_percent{
      60U, 70U, 81U, 83U, 85U, 88U, 89U, 75U, 72U, 68U};

  const std::size_t index =
      static_cast<std::size_t>(state._tick % temperatures.size());

  const TLSSMON::PolicyTime now = state.logical_time();

  const std::uint32_t raw_temperature = temperatures[index];

  const TLSSMON::PolicyFeedResult smoothed =
      state._temperature_policy->feed(raw_temperature, now);

  if (smoothed._noteworthy) {
    const auto result =
        engine.report_count(make_key(TLSSMON::DemoM9::DISK_MODULE_ID,
                                     TLSSMON::DemoM9::DISK_TEMPERATURE),
                            smoothed._value);

    std::cout << "[disk] raw_temperature=" << raw_temperature
              << " smoothed=" << smoothed._value
              << " update=" << static_cast<int>(result._status) << '\n';
  }

  const std::uint32_t used = used_percent[index];

  (void)engine.report_count(make_key(TLSSMON::DemoM9::DISK_MODULE_ID,
                                     TLSSMON::DemoM9::DISK_USED_PERCENT),
                            used);

  const TLSSMON::PolicyFeedResult full =
      state._disk_full_policy->feed(used, now);

  if (full._noteworthy) {
    const auto result =
        engine.report_error(make_key(TLSSMON::DemoM9::DISK_MODULE_ID,
                                     TLSSMON::DemoM9::DISK_ALMOST_FULL),
                            used);

    std::cout << "[disk] sustained usage alarm" << " used=" << used
              << " update=" << static_cast<int>(result._status) << '\n';
  }

  ++state._tick;
  return 0;
}

int sample_network(TLSSMON::Engine &engine, NodeState &state) {
  static constexpr std::array<bool, 12U> errors{false, true, true, false,
                                                true,  true, true, false,
                                                false, true, true, true};

  const std::size_t index =
      static_cast<std::size_t>(state._tick % errors.size());

  const bool has_error = errors[index];
  const TLSSMON::PolicyTime now = state.logical_time();

  if (has_error) {
    ++state._network_error_total;

    const TLSSMON::PolicyFeedResult burst =
        state._network_burst_policy->feed(1U, now);

    if (burst._noteworthy) {
      const auto result =
          engine.report_error(make_key(TLSSMON::DemoM9::NETWORK_MODULE_ID,
                                       TLSSMON::DemoM9::NETWORK_RX_ERROR_BURST),
                              state._network_error_total);

      std::cout << "[network] receive error burst"
                << " total=" << state._network_error_total
                << " update=" << static_cast<int>(result._status) << '\n';
    }
  }

  (void)engine.report_count(make_key(TLSSMON::DemoM9::NETWORK_MODULE_ID,
                                     TLSSMON::DemoM9::NETWORK_RX_ERROR_COUNT),
                            state._network_error_total);

  const std::string link = state._tick % 8U == 7U ? "down" : "up";

  (void)engine.report_string(make_key(TLSSMON::DemoM9::NETWORK_MODULE_ID,
                                      TLSSMON::DemoM9::NETWORK_LINK_STATUS),
                             link);

  std::cout << "[network] error_total=" << state._network_error_total
            << " link=" << link << '\n';

  ++state._tick;
  return 0;
}

bool register_sampling_timer(TLSSMON::Engine &engine,
                             const NodeOptions &options,
                             const std::shared_ptr<NodeState> &state) {
  const bool disk = options._module->_mid == TLSSMON::DemoM9::DISK_MODULE_ID;

  const std::chrono::milliseconds interval = disk ? 1s : 700ms;

  const std::optional<TLSSMON::TimerHandle> timer = engine.set_timer(
      TLSSMON::MonCallback{disk ? "disk-sampler" : "network-sampler",
                           [&engine, state, disk]() -> int {
                             if (!state->_sampling.load(
                                     std::memory_order_acquire)) {
                               return 0;
                             }

                             return disk ? sample_disk(engine, *state)
                                         : sample_network(engine, *state);
                           },
                           false},
      TLSSMON::TimerFlags::RECURRING, interval);

  return timer.has_value();
}

bool register_commands(TLSSMON::CliRegistry &registry, TLSSMON::Engine &engine,
                       const std::shared_ptr<NodeState> &state,
                       const TLSSMON::ReliableAlarmPublisher *alarm_publisher) {
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

  if (TLSSMON::register_alarm_status_command(registry, alarm_publisher,
                                             nullptr) !=
      TLSSMON::CliRegisterStatus::SUCCESS) {
    return false;
  }

  return registry.register_command(
             "stop", "stop this monitor node",
             [&engine,
              state](const TLSSMON::CliArguments &arguments) -> std::string {
               if (!arguments.empty()) {
                 return "usage: stop\n";
               }

               /*
                * 先关闭业务采样门，再提交 Engine 停止请求。
                */
               state->_sampling.store(false, std::memory_order_release);
               engine.stop();
               return "stopping\n";
             }) == TLSSMON::CliRegisterStatus::SUCCESS;
}

} // namespace

int main(int argc, char **argv) {
  const std::optional<NodeOptions> parsed = parse_options(argc, argv);

  if (!parsed.has_value()) {
    print_usage(argv[0]);
    return 1;
  }

  const NodeOptions &options = *parsed;

  const std::uint8_t node_id =
      options._module->_mid == TLSSMON::DemoM9::DISK_MODULE_ID
          ? std::uint8_t{1U}
          : std::uint8_t{2U};

  TLSSMON::Engine engine{TLSSMON::MonConfig{
      "monitor-m9-" + options._module->_name, options._cli_port, node_id}};

  if (engine.init() != TLSSMON::ENGINESTATE::SUCCESSFUL) {
    std::cerr << "[node] Engine initialization failed\n";
    return 1;
  }

  if (engine.register_module(*options._module) !=
      TLSSMON::ModuleRegisterStatus::SUCCESS) {
    std::cerr << "[node] module registration failed\n";
    return 1;
  }

  auto udp_publisher = install_udp_publisher(engine);

  if (!udp_publisher) {
    return 1;
  }

  auto alarm_publisher = install_alarm_publisher(engine, options);

  if (!alarm_publisher) {
    return 1;
  }

  auto state = std::make_shared<NodeState>();

  if (!state->_temperature_policy || !state->_disk_full_policy ||
      !state->_network_burst_policy) {
    std::cerr << "[node] policy initialization failed\n";
    return 1;
  }

  TLSSMON::CliRegistry registry;

  if (!register_commands(registry, engine, state, alarm_publisher.get())) {
    std::cerr << "[node] CLI command registration failed\n";
    return 1;
  }

  TLSSMON::CliServer cli{engine, registry,
                         TLSSMON::CliServerConfig{options._cli_port,
                                                  TLSSMON::CLI_MAX_CLIENTS,
                                                  options._module->_name}};

  if (!cli.start()) {
    std::cerr << "[node] CLI startup failed\n";
    return 1;
  }

  if (!register_sampling_timer(engine, options, state)) {
    std::cerr << "[node] timer registration failed\n";
    cli.close();
    return 1;
  }

  std::cout << "[node] module=" << options._module->_name << '\n'
            << "[node] source_id=" << options._source_id << '\n'
            << "[node] UDP metrics -> " << COLLECTOR_HOST << ':'
            << METRIC_UDP_PORT << '\n'
            << "[node] TCP alarms -> " << COLLECTOR_HOST << ':'
            << ALARM_TCP_PORT << '\n'
            << "[node] outbox=" << options._outbox << '\n'
            << "[node] CLI=127.0.0.1:" << cli.bound_port() << '\n'
            << "[node] commands: help modules use db_dump "
               "alarm_status stop\n"
            << std::flush;

  const TLSSMON::ENGINESTATE run_result = engine.run();

  /*
   * run() 返回表示 Timer/AIO 及 worker 已经停止。
   */
  state->_sampling.store(false, std::memory_order_release);

  /*
   * Engine 中的 Publisher 回调只捕获 weak_ptr，所以这里可以严格
   * 停止可靠 Publisher，不会被 Reporter 中的回调延长生命周期。
   */
  alarm_publisher.reset();
  udp_publisher.reset();

  /*
   * Engine 已经停止，不会再执行 alarm_status Handler。
   * 因而可以先析构 Publisher，再关闭 CLI。
   */
  cli.close();

  if (run_result != TLSSMON::ENGINESTATE::SUCCESSFUL) {
    std::cerr << "[node] Engine stopped with status="
              << static_cast<int>(run_result) << '\n';
    return 1;
  }

  std::cout << "[node] stopped successfully\n";
  return 0;
}
