#include "monitor/cli_commands.h"
#include "monitor/cli_registry.h"
#include "monitor/cli_server.h"
#include "monitor/engine.h"
#include "monitor/monitor_collector.h"
#include "monitor/reliable_alarm_collector.h"
#include "monitor/udp_receiver.h"
#include "observability/storage_monitor_schema.h"

#include <pthread.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace {

struct Options {
  std::string bind_address{"127.0.0.1"};
  std::uint16_t metrics_port{9300};
  std::uint16_t alarms_port{9400};
  std::uint16_t cli_port{9310};
  std::filesystem::path inbox{"./data/monitor-inbox"};
  std::chrono::milliseconds heartbeat_timeout{5000};
  bool help{false};
};

void print_usage(const char* executable) {
  std::cout << "usage: " << executable << " [options]\n"
            << "  --bind ADDRESS                 metric/alarm bind address (127.0.0.1)\n"
            << "  --metrics-port PORT            UDP metrics port (9300)\n"
            << "  --alarms-port PORT             reliable alarm TCP port (9400)\n"
            << "  --cli-port PORT                local CLI TCP port (9310)\n"
            << "  --inbox DIRECTORY              persistent alarm inbox (./data/monitor-inbox)\n"
            << "  --heartbeat-timeout-ms MS      unreachable suspicion interval (5000)\n"
            << "  --help                         show this help\n"
            << "Ports may be 0 to request an available port. CLI binds 127.0.0.1.\n";
}

bool parse_number(std::string_view value, std::uint64_t minimum, std::uint64_t maximum,
                  std::uint64_t& result) {
  if (value.empty() || value.front() < '0' || value.front() > '9') {
    return false;
  }
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
         result >= minimum && result <= maximum;
}

bool parse_options(int argc, char** argv, Options& options) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view flag{argv[index]};
    if (flag == "--help") {
      options.help = true;
      continue;
    }
    if (flag != "--bind" && flag != "--metrics-port" && flag != "--alarms-port" &&
        flag != "--cli-port" && flag != "--inbox" && flag != "--heartbeat-timeout-ms") {
      std::cerr << "unknown option: " << flag << '\n';
      return false;
    }
    if (++index >= argc) {
      std::cerr << "missing value for " << flag << '\n';
      return false;
    }
    const std::string_view value{argv[index]};
    if (value.empty()) {
      std::cerr << "empty value for " << flag << '\n';
      return false;
    }
    if (flag == "--bind") {
      options.bind_address = value;
    } else if (flag == "--inbox") {
      options.inbox = value;
    } else {
      std::uint64_t number = 0;
      const bool timeout = flag == "--heartbeat-timeout-ms";
      const std::uint64_t maximum =
          timeout ? static_cast<std::uint64_t>(
                        std::numeric_limits<std::chrono::milliseconds::rep>::max())
                  : std::numeric_limits<std::uint16_t>::max();
      if (!parse_number(value, timeout ? 1U : 0U, maximum, number)) {
        std::cerr << "invalid value for " << flag << ": " << value << '\n';
        return false;
      }
      if (timeout) {
        options.heartbeat_timeout = std::chrono::milliseconds{number};
      } else if (flag == "--metrics-port") {
        options.metrics_port = static_cast<std::uint16_t>(number);
      } else if (flag == "--alarms-port") {
        options.alarms_port = static_cast<std::uint16_t>(number);
      } else {
        options.cli_port = static_cast<std::uint16_t>(number);
      }
    }
  }
  return true;
}

// Block before Engine or the reliable collector can create worker threads.
class SignalSource final {
 public:
  SignalSource() {
    sigset_t mask;
    ::sigemptyset(&mask);
    ::sigaddset(&mask, SIGINT);
    ::sigaddset(&mask, SIGTERM);
    if (::pthread_sigmask(SIG_BLOCK, &mask, &previous_mask_) == 0) {
      masked_ = true;
      fd_ = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    }
  }
  ~SignalSource() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    if (masked_) {
      (void)::pthread_sigmask(SIG_SETMASK, &previous_mask_, nullptr);
    }
  }
  SignalSource(const SignalSource&) = delete;
  SignalSource& operator=(const SignalSource&) = delete;
  int fd() const noexcept {
    return fd_;
  }

 private:
  int fd_{-1};
  bool masked_{false};
  sigset_t previous_mask_{};
};

class AioRegistration final {
 public:
  AioRegistration(TLSSMON::Engine& engine, int fd, TLSSMON::MonCallback callback)
      : engine_(engine)
      , handle_(engine.add_aio(fd, std::move(callback))) {}
  ~AioRegistration() {
    if (handle_) {
      (void)engine_.remove_aio(*handle_);
    }
  }
  AioRegistration(const AioRegistration&) = delete;
  AioRegistration& operator=(const AioRegistration&) = delete;
  bool ready() const noexcept {
    return handle_.has_value();
  }

 private:
  TLSSMON::Engine& engine_;
  std::optional<TLSSMON::AioHandle> handle_;
};

class ServiceHeartbeat final {
 public:
  void received() {
    std::lock_guard<std::mutex> lock(mutex_);
    last_received_ = std::chrono::steady_clock::now();
  }
  std::string status(std::chrono::milliseconds timeout) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!last_received_) {
      return "service=never_seen last_age_ms=unknown\n";
    }
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - *last_received_);
    return std::string{"service="} + (age >= timeout ? "suspected_unreachable" : "online") +
           " last_age_ms=" + std::to_string(age.count()) + '\n';
  }

 private:
  mutable std::mutex mutex_;
  std::optional<std::chrono::steady_clock::time_point> last_received_;
};

int drain_metrics(TLSSMON::Engine& engine, const TLSSMON::Wire::UdpReceiver& receiver,
                  ServiceHeartbeat& heartbeat) {
  // Bound work per activation so a busy publisher cannot starve CLI or signals.
  for (std::size_t count = 0; count < 128U; ++count) {
    auto result = receiver.receive_one();
    if (result._status == TLSSMON::Wire::UdpReceiveStatus::WOULD_BLOCK ||
        result._status == TLSSMON::Wire::UdpReceiveStatus::RECEIVE_ERROR ||
        result._status == TLSSMON::Wire::UdpReceiveStatus::SOCKET_ERROR) {
      return 0;
    }
    if (!result.success() || !result._record) {
      continue;
    }
    const auto& record = *result._record;
    const auto& definitions = TLSS::OBSERVABILITY::metric_definitions();
    const bool known_metric = std::any_of(
        definitions.begin(), definitions.end(),
        [&record](const TLSS::OBSERVABILITY::MetricDefinition& definition) {
          return record._data._key == TLSS::OBSERVABILITY::metric_key(definition.metric);
        });
    if (!known_metric ||
        !std::holds_alternative<TLSSMON::MonData::NumericValue>(record._data._value)) {
      continue;
    }
    const bool is_heartbeat = record._data._key == TLSS::OBSERVABILITY::metric_key(
                                                       TLSS::OBSERVABILITY::Metric::heartbeat);
    const auto updated = TLSSMON::ingest_decoded_record(engine, std::move(*result._record), true);
    if (is_heartbeat && updated._status != TLSSMON::MonData::UpdateStatus::INVALID &&
        updated._status != TLSSMON::MonData::UpdateStatus::DURABILITY_FAILED) {
      // Receipt time is local and independent of remote changed_at or value changes.
      heartbeat.received();
    }
  }
  return 0;
}

bool register_commands(TLSSMON::CliRegistry& registry, TLSSMON::Engine& engine,
                       const TLSSMON::ReliableAlarmCollector& alarms, ServiceHeartbeat& heartbeat,
                       std::chrono::milliseconds timeout) {
  using TLSSMON::CliRegisterStatus;
  if (TLSSMON::register_help_command(registry) != CliRegisterStatus::SUCCESS ||
      TLSSMON::register_modules_command(registry, engine) != CliRegisterStatus::SUCCESS ||
      TLSSMON::register_use_command(registry, engine) != CliRegisterStatus::SUCCESS ||
      TLSSMON::register_db_dump_command(registry, engine) != CliRegisterStatus::SUCCESS ||
      TLSSMON::register_alarm_status_command(registry, nullptr, &alarms) !=
          CliRegisterStatus::SUCCESS) {
    return false;
  }
  if (registry.register_command("service_status", "show storage service heartbeat state",
                                [&heartbeat, timeout](const TLSSMON::CliArguments& arguments) {
                                  return arguments.empty() ? heartbeat.status(timeout)
                                                           : std::string{"usage: service_status\n"};
                                }) != CliRegisterStatus::SUCCESS) {
    return false;
  }
  return registry.register_command(
             "stop", "stop collector",
             [&engine](const TLSSMON::CliArguments& arguments) -> std::string {
               if (!arguments.empty()) {
                 return "usage: stop\n";
               }
               engine.stop();
               return "stopping\n";
             }) == CliRegisterStatus::SUCCESS;
}

int run_collector(const Options& options) {
  SignalSource signals;
  if (signals.fd() < 0) {
    std::cerr << "collector: cannot initialize signal source\n";
    return 1;
  }
  TLSSMON::Engine engine{TLSSMON::MonConfig{"storage-monitor-collector", options.cli_port, 10U}};
  if (engine.init() != TLSSMON::ENGINESTATE::SUCCESSFUL ||
      !TLSS::OBSERVABILITY::register_storage_modules(engine)) {
    std::cerr << "collector: cannot initialize engine or storage schema\n";
    return 1;
  }
  AioRegistration signal_registration{
      engine, signals.fd(),
      TLSSMON::MonCallback{"collector-signals",
                           [&engine, &signals]() {
                             signalfd_siginfo signal{};
                             ssize_t received;
                             do {
                               received = ::read(signals.fd(), &signal, sizeof(signal));
                             } while (received < 0 && errno == EINTR);
                             if (received == static_cast<ssize_t>(sizeof(signal))) {
                               engine.stop();
                             }
                             return 0;
                           },
                           false}};
  if (!signal_registration.ready()) {
    std::cerr << "collector: cannot register signal source\n";
    return 1;
  }
  ServiceHeartbeat heartbeat;
  TLSSMON::Wire::UdpReceiver receiver{{options.bind_address, options.metrics_port}};
  if (!receiver.ready()) {
    std::cerr << "collector: cannot bind metric receiver\n";
    return 1;
  }
  AioRegistration metric_registration{engine, receiver.fd(),
                                      TLSSMON::MonCallback{"storage-metrics",
                                                           [&engine, &receiver, &heartbeat]() {
                                                             return drain_metrics(engine, receiver,
                                                                                  heartbeat);
                                                           },
                                                           false}};
  if (!metric_registration.ready()) {
    std::cerr << "collector: cannot register metric receiver\n";
    return 1;
  }
  TLSSMON::ReliableAlarmCollector alarms{
      engine,
      {options.bind_address, options.alarms_port, options.inbox, 128U, std::chrono::seconds{30}}};
  if (!alarms.ready()) {
    std::cerr << "collector: cannot initialize reliable alarm inbox/listener status="
              << static_cast<int>(alarms.setup_status()) << " errno=" << alarms.setup_error()
              << '\n';
    return 1;
  }
  TLSSMON::CliRegistry registry;
  if (!register_commands(registry, engine, alarms, heartbeat, options.heartbeat_timeout)) {
    std::cerr << "collector: cannot register CLI commands\n";
    return 1;
  }
  TLSSMON::CliServer cli{
      engine, registry, {options.cli_port, TLSSMON::CLI_MAX_CLIENTS, "collector"}};
  if (!cli.start()) {
    std::cerr << "collector: cannot start CLI listener\n";
    return 1;
  }
  std::cout << "READY metrics=" << receiver.bound_port() << " alarms=" << alarms.bound_port()
            << " cli=" << cli.bound_port() << '\n'
            << std::flush;
  const auto result = engine.run();
  alarms.stop();
  cli.close();
  if (result != TLSSMON::ENGINESTATE::SUCCESSFUL) {
    std::cerr << "collector: engine stopped with status=" << static_cast<int>(result) << '\n';
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Options options;
    if (!parse_options(argc, argv, options)) {
      return 1;
    }
    if (options.help) {
      print_usage(argv[0]);
      return 0;
    }
    return run_collector(options);
  } catch (const std::exception& error) {
    std::cerr << "collector: " << error.what() << '\n';
    return 1;
  }
}
