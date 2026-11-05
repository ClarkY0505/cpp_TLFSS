#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <condition_variable>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include "monitor/reliable_alarm_collector.h"
#include "monitor/udp_receiver.h"
#include "observability/storage_monitor_runtime.h"

using namespace TLSS::OBSERVABILITY;
using namespace std::chrono_literals;
namespace {
void check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
std::uint32_t numeric(const TLSSMON::MonData::StoredRecord& record) {
  return std::get<TLSSMON::MonData::NumericValue>(record._data._value)._value;
}
void thresholds() {
  ThresholdAlarm alarm({8500, 8000, 100ms});
  const auto zero = std::chrono::steady_clock::time_point{};
  check(!alarm.feed(9000, zero), "threshold must hold");
  check(alarm.feed(9000, zero + 100ms), "threshold activation");
  check(alarm.feed(8200, zero + 200ms), "hysteresis band");
  check(alarm.feed(7900, zero + 300ms), "recovery must hold");
  check(alarm.feed(std::nullopt, zero + 500ms), "invalid data cannot recover");
  check(alarm.feed(7900, zero + 600ms), "invalid resets recovery duration");
  check(!alarm.feed(7900, zero + 700ms), "recovery");
}
void schema() {
  std::set<std::pair<std::uint32_t, std::uint32_t>> keys;
  for (const auto& def : metric_definitions()) {
    check(keys.emplace(def.module_id, def.event_id).second, "duplicate schema key");
    check(metric_key(def.metric)._eid == def.event_id, "schema lookup");
  }
  check(keys.size() == StorageMetrics::size, "incomplete schema");
}
void lifecycle() {
  TLSSMON::Wire::UdpReceiver receiver({"127.0.0.1", 0});
  check(receiver.ready(), "UDP receiver");
  auto metrics = std::make_shared<StorageMetrics>();
  metrics->set(Metric::connections, std::numeric_limits<std::uint64_t>::max());
  StorageMonitorConfig config;
  config.metrics_port = receiver.bound_port();
  config.sample_interval = 20ms;
  config.full_snapshot_interval = 50ms;
  std::atomic<int> attempts{0};
  std::thread::id report_thread;
  const auto caller = std::this_thread::get_id();
  config.alarm_sink = [&](TLSSMON::MonData::StoredRecord) {
    report_thread = std::this_thread::get_id();
    const int attempt = ++attempts;
    return TLSSMON::AlarmEnqueueResult{
        attempt == 1 ? TLSSMON::AlarmEnqueueStatus::IO_ERROR : TLSSMON::AlarmEnqueueStatus::SUCCESS,
        0};
  };
  std::atomic<int> refreshes{0};
  StorageMonitorRuntime runtime(config, metrics, {}, [&] {
    ++refreshes;
  });
  check(runtime.start(), "runtime start");
  check(!runtime.start(), "one-shot start");
  auto zero = runtime.find_data(Metric::upload_started);
  check(zero && numeric(*zero) == 0, "initial zero missing");
  check(numeric(*runtime.find_data(Metric::connections)) == UINT32_MAX, "wire saturation");
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  int repeated_zero = 0;
  while (std::chrono::steady_clock::now() < deadline && repeated_zero < 2) {
    pollfd fd{receiver.fd(), POLLIN, 0};
    ::poll(&fd, 1, 50);
    for (;;) {
      auto result = receiver.receive_one();
      if (result._status == TLSSMON::Wire::UdpReceiveStatus::WOULD_BLOCK)
        break;
      check(result.success(), "UDP decode");
      if (result._record->_data._key == metric_key(Metric::upload_started))
        ++repeated_zero;
    }
  }
  check(repeated_zero >= 2, "unchanged value not periodically retransmitted");
  metrics->set(Metric::service_state, 4);
  auto first_stop = std::async(std::launch::async, [&] {
    runtime.finish_and_stop();
  });
  auto second_stop = std::async(std::launch::async, [&] {
    runtime.finish_and_stop();
  });
  first_stop.get();
  second_stop.get();
  check(!runtime.status().running, "runtime stopped");
  check(numeric(*runtime.find_data(Metric::service_state)) == 4, "final sample");
  check(attempts >= 4, "durability failure not retried");
  check(report_thread != caller, "alarm persisted on main thread");
  check(refreshes >= 2, "refresh and final refresh");
  StorageMonitorRuntime unused(config, metrics);
  unused.finish_and_stop();
  check(!unused.start(), "stop-before-start must be terminal");
  config.sample_interval = 0ms;
  StorageMonitorRuntime invalid(config, metrics);
  check(!invalid.start() && !invalid.status().error.empty(), "invalid config");
}

struct Directory {
  std::filesystem::path path;
  Directory() {
    char pattern[] = "/tmp/tlss-monitor-runtime-XXXXXX";
    const char* value = ::mkdtemp(pattern);
    check(value != nullptr, "temporary directory");
    path = value;
  }
  ~Directory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};
void write_fixture(const std::filesystem::path& path, const std::string& content) {
  {
    std::ofstream stream(path.string() + ".tmp");
    stream << content;
    check(bool(stream), "write fixture");
  }
  std::filesystem::rename(path.string() + ".tmp", path);
}
template <class Predicate>
void eventually(Predicate predicate, const char* message) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!predicate()) {
    check(std::chrono::steady_clock::now() < deadline, message);
    std::this_thread::sleep_for(1ms);
  }
}
void resources() {
  Directory fixture;
  TLSSMON::Wire::UdpReceiver receiver({"127.0.0.1", 0});
  check(receiver.ready(), "resource receiver");
  auto sampler = std::make_shared<SystemResourceSampler>(
      SystemResourceSamplerConfig{fixture.path / "storage", 5ms, fixture.path / "proc"});
  check(sampler->start(), "resource sampler");
  auto metrics = std::make_shared<StorageMetrics>();
  StorageMonitorConfig config;
  config.metrics_port = receiver.bound_port();
  config.sample_interval = 10ms;
  config.full_snapshot_interval = 30ms;
  config.resource_stale_after = 50ms;
  metrics->set(Metric::worker_queue, 255);
  config.memory_warning = {9000, 8500, 0ms};
  std::mutex mutex;
  bool memory_active = false;
  bool memory_recovered = false;
  int memory_attempts = 0;
  int resource_states_before_valid = 0;
  std::atomic<bool> fixtures_ready{false};
  config.alarm_sink = [&](TLSSMON::MonData::StoredRecord record) {
    const auto key = record._data._key;
    std::lock_guard<std::mutex> lock(mutex);
    if (key._eid <= 6 && !fixtures_ready.load() && numeric(record) == 0)
      ++resource_states_before_valid;
    if (key == alarm_key(ResourceAlarm::memory_high)) {
      if (numeric(record) == 1) {
        ++memory_attempts;
        if (memory_attempts == 1)
          return TLSSMON::AlarmEnqueueResult{TLSSMON::AlarmEnqueueStatus::IO_ERROR, 0};
        memory_active = true;
      } else if (memory_active)
        memory_recovered = true;
    }
    return TLSSMON::AlarmEnqueueResult{TLSSMON::AlarmEnqueueStatus::SUCCESS, 0};
  };
  StorageMonitorRuntime runtime(config, metrics, sampler);
  check(runtime.start(), "resource runtime");
  {
    std::lock_guard<std::mutex> lock(mutex);
    check(resource_states_before_valid == 0, "unavailable samples published false recovery");
  }
  std::filesystem::create_directory(fixture.path / "storage");
  std::filesystem::create_directories(fixture.path / "proc/self");
  write_fixture(fixture.path / "proc/self/status",
                "VmRSS: 2048 kB\nVmHWM: 4096 kB\nVmSize: 8192 kB\n");
  fixtures_ready.store(true);
  write_fixture(fixture.path / "proc/meminfo", "MemTotal: 10000 kB\nMemAvailable: 500 kB\n");
  eventually(
      [&] {
        std::lock_guard<std::mutex> lock(mutex);
        return memory_active;
      },
      "memory alarm/retry");
  check(numeric(*runtime.find_data(Metric::resource_memory_percent)) == 9500,
        "memory percentage wire units");
  check(numeric(*runtime.find_data(Metric::resource_rss_mib)) == 2, "RSS MiB units");
  write_fixture(fixture.path / "proc/meminfo", "MemTotal: 10000 kB\nMemAvailable: 5000 kB\n");
  eventually(
      [&] {
        std::lock_guard<std::mutex> lock(mutex);
        return memory_recovered;
      },
      "memory recovery");
  sampler->finish_and_stop();
  eventually(
      [&] {
        return numeric(*runtime.find_data(Metric::resource_memory_valid)) == 0;
      },
      "stale data validity");
  check(numeric(*runtime.find_data(Metric::resource_rss_mib)) == 2,
        "stale data lost previous value");
  runtime.finish_and_stop();
  // procfs exposes no inode capacity on Linux; publish its validity separately.
  auto no_inodes =
      std::make_shared<SystemResourceSampler>(SystemResourceSamplerConfig{"/proc", 1h, "/proc"});
  check(no_inodes->start(), "procfs sampler");
  auto inode_metrics = std::make_shared<StorageMetrics>();
  config.alarm_sink = [](TLSSMON::MonData::StoredRecord) {
    return TLSSMON::AlarmEnqueueResult{TLSSMON::AlarmEnqueueStatus::SUCCESS, 0};
  };
  StorageMonitorRuntime inode_runtime(config, inode_metrics, no_inodes);
  check(inode_runtime.start(), "inode runtime");
  check(numeric(*inode_runtime.find_data(Metric::resource_inode_valid)) ==
            static_cast<std::uint32_t>(no_inodes->snapshot().disk.inode_valid),
        "inode validity wire flag");
  no_inodes->finish_and_stop();
  inode_runtime.finish_and_stop();
}
void transport() {
  Directory fixture;
  // Reserve an unused TCP port without listening: the first reporter must spool offline.
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  check(fd >= 0, "reserve socket");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  check(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0, "reserve bind");
  socklen_t length = sizeof address;
  check(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0, "reserve port");
  StorageMonitorConfig config;
  config.alarms_port = ntohs(address.sin_port);
  config.outbox = fixture.path / "outbox";
  config.sample_interval = 20ms;
  auto metrics = std::make_shared<StorageMetrics>();
  {
    StorageMonitorRuntime offline(config, metrics);
    check(offline.start(), "reporter requires collector online");
    offline.finish_and_stop();
  }
  std::size_t pending = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(config.outbox / "pending"))
    if (entry.path().extension() == ".alarm")
      ++pending;
  check(pending >= 3, "offline durable alarms missing");
  ::close(fd);
  TLSSMON::Engine collector({"test-collector", 0, 1});
  check(collector.init() == TLSSMON::ENGINESTATE::SUCCESSFUL, "collector init");
  check(register_storage_modules(collector), "collector schema");
  TLSSMON::ReliableAlarmCollector alarms(
      collector, {"127.0.0.1", config.alarms_port, fixture.path / "inbox", 8, 2s});
  check(alarms.ready(), "collector listen");
  {
    StorageMonitorRuntime restored(config, metrics);
    check(restored.start(), "restored reporter");
    eventually(
        [&] {
          return alarms.status()._accepted >= pending;
        },
        "pending alarm delivery after restart");
    eventually(
        [&] {
          for (const auto& entry :
               std::filesystem::recursive_directory_iterator(config.outbox / "pending"))
            if (entry.path().extension() == ".alarm")
              return false;
          return true;
        },
        "acknowledged alarms not removed");
    restored.finish_and_stop();
  }
  alarms.stop();
  collector.stop();
}

void configuration() {
  const char* name = "TLSS_MONITOR_INTERVAL_MS";
  const char* previous = ::getenv(name);
  const std::optional<std::string> original =
      previous ? std::optional<std::string>(previous) : std::nullopt;
  struct Restore {
    const char* name;
    std::optional<std::string> value;
    ~Restore() {
      if (value)
        ::setenv(name, value->c_str(), 1);
      else
        ::unsetenv(name);
    }
  } restore{name, original};
  for (const char* value : {"0", "-1", "12x", "3600001", "18446744073709551616"}) {
    ::setenv(name, value, 1);
    bool rejected = false;
    try {
      (void)monitor_config_from_environment();
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected, "invalid environment interval accepted");
  }
  ::setenv(name, "123", 1);
  check(monitor_config_from_environment().sample_interval == 123ms, "valid environment interval");
}
void self_stop() {
  auto metrics = std::make_shared<StorageMetrics>();
  StorageMonitorConfig config;
  StorageMonitorRuntime* pointer = nullptr;
  std::atomic<bool> rejected{false};
  config.alarm_sink = [&](TLSSMON::MonData::StoredRecord) {
    try {
      pointer->finish_and_stop();
    } catch (const std::logic_error&) {
      rejected = true;
    }
    return TLSSMON::AlarmEnqueueResult{TLSSMON::AlarmEnqueueStatus::SUCCESS, 0};
  };
  StorageMonitorRuntime runtime(config, metrics);
  pointer = &runtime;
  check(runtime.start(), "self-stop runtime");
  check(rejected, "self-join must reject without lifecycle lock deadlock");
  runtime.finish_and_stop();
}

}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string scenario = argc == 2 ? argv[1] : "all";
    if (scenario == "thresholds" || scenario == "all")
      thresholds();
    if (scenario == "schema" || scenario == "all")
      schema();
    if (scenario == "lifecycle" || scenario == "all")
      lifecycle();
    if (scenario == "resources" || scenario == "all")
      resources();
    if (scenario == "transport" || scenario == "all")
      transport();
    if (scenario == "configuration" || scenario == "all")
      configuration();
    if (scenario == "self_stop" || scenario == "all")
      self_stop();
    std::cout << "PASS " << scenario << '\n';
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
