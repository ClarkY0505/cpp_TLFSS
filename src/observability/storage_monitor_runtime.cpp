#include "observability/storage_monitor_runtime.h"
#include "monitor/reliable_alarm_publisher.h"
#include "monitor/udp_publisher.h"

#include <sys/eventfd.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <condition_variable>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace TLSS::OBSERVABILITY {
using Clock = std::chrono::steady_clock;
namespace {
thread_local const void* current_runtime = nullptr;
}
ThresholdAlarm::ThresholdAlarm(ThresholdConfig config)
    : config_(config) {
  if (config.recovery >= config.trigger || config.duration.count() < 0)
    throw std::invalid_argument("invalid alarm threshold or hold duration");
}
bool ThresholdAlarm::feed(std::optional<std::uint32_t> value, Clock::time_point now) {
  if (!value) {
    pending_since_.reset();
    return active_;
  }
  const bool transition = active_ ? *value <= config_.recovery : *value >= config_.trigger;
  if (!transition) {
    pending_since_.reset();
    return active_;
  }
  if (!pending_since_)
    pending_since_ = now;
  if (now - *pending_since_ >= config_.duration) {
    active_ = !active_;
    pending_since_.reset();
  }
  return active_;
}
namespace {
std::uint64_t env_number(const char* key, std::uint64_t fallback, std::uint64_t minimum,
                         std::uint64_t maximum) {
  const char* value = std::getenv(key);
  if (!value)
    return fallback;
  const std::string text(value);
  std::uint64_t number = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || number < minimum ||
      number > maximum)
    throw std::invalid_argument(std::string("invalid ") + key);
  return number;
}
}  // namespace
StorageMonitorConfig monitor_config_from_environment() {
  StorageMonitorConfig config;
  if (const char* host = std::getenv("TLSS_MONITOR_HOST"))
    config.collector_host = host;
  if (const char* outbox = std::getenv("TLSS_MONITOR_OUTBOX"))
    config.outbox = outbox;
  config.metrics_port =
      static_cast<std::uint16_t>(env_number("TLSS_MONITOR_METRICS_PORT", 9300, 1, 65535));
  config.alarms_port =
      static_cast<std::uint16_t>(env_number("TLSS_MONITOR_ALARMS_PORT", 9400, 1, 65535));
  config.source_id = env_number("TLSS_MONITOR_SOURCE_ID", 1, 1, UINT64_MAX);
  config.sample_interval =
      std::chrono::milliseconds(env_number("TLSS_MONITOR_INTERVAL_MS", 1000, 1, 3600000));
  config.full_snapshot_interval =
      std::chrono::milliseconds(env_number("TLSS_MONITOR_SNAPSHOT_MS", 10000, 1, 3600000));
  return config;
}

struct StorageMonitorRuntime::Impl {
  StorageMonitorConfig config;
  std::shared_ptr<StorageMetrics> metrics;
  std::shared_ptr<SystemResourceSampler> resources;
  std::function<void()> refresh;
  std::unique_ptr<TLSSMON::Engine> engine;
  std::shared_ptr<TLSSMON::Wire::UdpPublisher> udp;
  std::shared_ptr<TLSSMON::ReliableAlarmPublisher> alarm;
  std::vector<ThresholdAlarm> thresholds;
  std::array<bool, static_cast<std::size_t>(ResourceAlarm::count)> observed{};
  std::thread thread;
  int control_fd = -1;
  std::mutex lifecycle;
  mutable std::mutex error_mutex;
  std::string error;
  std::mutex started_mutex;
  std::condition_variable started_cv;
  bool started = false;
  bool exited = false;
  bool attempted = false;
  std::atomic<bool> running{false};
  std::atomic<bool> stopping{false};
  Clock::time_point began{};
  Clock::time_point last_snapshot{};
  std::uint64_t previous_db_failures = 0;
  std::uint64_t previous_metadata_failures = 0;

  Impl(StorageMonitorConfig value, std::shared_ptr<StorageMetrics> counts,
       std::shared_ptr<SystemResourceSampler> sampler, std::function<void()> callback)
      : config(std::move(value))
      , metrics(std::move(counts))
      , resources(std::move(sampler))
      , refresh(std::move(callback)) {
    engine = std::make_unique<TLSSMON::Engine>(TLSSMON::MonConfig{"storage-reporter", 0, 1});
  }
  ~Impl() {
    if (control_fd >= 0)
      ::close(control_fd);
  }
  void fail(std::string message) {
    std::lock_guard<std::mutex> lock(error_mutex);
    error = std::move(message);
  }
  void wake() {
    std::uint64_t one = 1;
    ssize_t result;
    do {
      result = ::write(control_fd, &one, sizeof one);
    } while (result < 0 && errno == EINTR);
    if (result < 0 && errno != EAGAIN) {
      fail("monitor control wakeup failed");
      if (engine)
        engine->stop();
    }
  }
  void send(TLSSMON::MonData::StoredRecord record) {
    if (!udp->send(record).success())
      metrics->add(Metric::udp_failures);
  }
  bool fresh(Clock::time_point last, Clock::time_point now) const {
    return last != Clock::time_point{} && now - last <= config.resource_stale_after;
  }
  ResourceSnapshot collect_resources(Clock::time_point now) {
    ResourceSnapshot snapshot;
    if (!resources)
      return snapshot;
    snapshot = resources->snapshot();
    const auto& disk = snapshot.disk;
    const auto& memory = snapshot.memory;
    metrics->set(Metric::resource_disk_valid, disk.valid && fresh(disk.last_success, now));
    metrics->set(Metric::resource_inode_valid,
                 disk.valid && disk.inode_valid && fresh(disk.last_success, now));
    metrics->set(Metric::resource_memory_valid, memory.valid && fresh(memory.last_success, now));
    metrics->set(Metric::resource_sequence, snapshot.sequence);
    constexpr std::uint64_t mib = 1024ULL * 1024;
    // Failed sources retain last-known values; validity is published independently.
    metrics->set(Metric::resource_disk_total_mib, disk.total_bytes / mib);
    metrics->set(Metric::resource_disk_used_mib, disk.used_bytes / mib);
    metrics->set(Metric::resource_disk_available_mib, disk.available_bytes / mib);
    metrics->set(Metric::resource_disk_percent, disk.used_percent_x100);
    metrics->set(Metric::resource_inode_percent, disk.inode_used_percent_x100);
    metrics->set(Metric::resource_readonly, disk.readonly);
    metrics->set(Metric::resource_memory_total_mib, memory.total_bytes / mib);
    metrics->set(Metric::resource_memory_available_mib, memory.available_bytes / mib);
    metrics->set(Metric::resource_memory_percent, memory.used_percent_x100);
    metrics->set(Metric::resource_rss_mib, memory.rss_bytes / mib);
    metrics->set(Metric::resource_peak_rss_mib, memory.peak_rss_bytes / mib);
    metrics->set(Metric::resource_virtual_mib, memory.virtual_bytes / mib);
    return snapshot;
  }
  void report_alarms(const ResourceSnapshot& sample, Clock::time_point now) {
    std::array<std::optional<std::uint32_t>, static_cast<std::size_t>(ResourceAlarm::count)>
        values{};
    if (resources) {
      if (sample.disk.valid && fresh(sample.disk.last_success, now)) {
        values[0] = values[1] = sample.disk.used_percent_x100;
        if (sample.disk.inode_valid)
          values[2] = sample.disk.inode_used_percent_x100;
        values[3] = sample.disk.readonly ? 1 : 0;
      }
      if (sample.memory.valid && fresh(sample.memory.last_success, now))
        values[4] = sample.memory.used_percent_x100;
      const bool stale =
          !fresh(sample.disk.last_success, now) || !fresh(sample.memory.last_success, now);
      if (sample.disk.valid && sample.memory.valid && !stale)
        values[5] = 0;
      else if (sample.disk.consecutive_failures >= 3 || sample.memory.consecutive_failures >= 3 ||
               (stale && now - began >= config.resource_stale_after))
        values[5] = 1;
    }
    values[6] = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(metrics->get(Metric::worker_queue), UINT32_MAX));
    const auto db_failures = metrics->get(Metric::db_failures);
    const auto metadata_failures = metrics->get(Metric::metadata_writes_failed);
    values[7] = db_failures > previous_db_failures ? 1 : 0;
    values[8] = metadata_failures > previous_metadata_failures ? 1 : 0;
    previous_db_failures = db_failures;
    previous_metadata_failures = metadata_failures;
    for (std::size_t i = 0; i < values.size(); ++i) {
      const bool active = thresholds[i].feed(values[i], now);
      // Unknown readings must not clear a Collector's previously persisted alarm.
      // Nor does a first high reading awaiting its hold duration establish normality.
      if (!observed[i]) {
        if (!values[i])
          continue;
        const auto recovery = i == 0   ? config.disk_warning.recovery
                              : i == 1 ? config.disk_critical.recovery
                              : i == 2 ? config.inode_warning.recovery
                              : i == 4 ? config.memory_warning.recovery
                              : i == 6 ? 128U
                                       : 0U;
        if (!active && *values[i] > recovery)
          continue;
        observed[i] = true;
      }
      // report_error preserves the previous Store value on durability failure.
      // Re-submit desired state every tick, so an unsuccessful write is retried.
      auto result = engine->report_error(alarm_key(static_cast<ResourceAlarm>(i)), active ? 1 : 0);
      if (result._status == TLSSMON::MonData::UpdateStatus::DURABILITY_FAILED)
        metrics->add(Metric::alarm_failures);
    }
  }
  void tick(bool final = false) {
    if (refresh) {
      try {
        refresh();
      } catch (const std::exception& exception) {
        fail(exception.what());
      } catch (...) {
        fail("business metrics snapshot failed");
      }
    }
    const auto now = Clock::now();
    const auto sample = collect_resources(now);
    metrics->add(Metric::heartbeat);
    metrics->set(Metric::uptime_seconds,
                 std::chrono::duration_cast<std::chrono::seconds>(now - began).count());
    auto snapshot = metrics->snapshot();
    bool overflow = false;
    for (const auto value : snapshot)
      overflow = overflow || value > UINT32_MAX;
    metrics->set(Metric::overflow, overflow ? 1 : 0);
    snapshot[static_cast<std::size_t>(Metric::overflow)] = overflow ? 1 : 0;
    for (const auto& def : metric_definitions()) {
      const auto value = snapshot[static_cast<std::size_t>(def.metric)];
      engine->update_data(
          {metric_key(def.metric), def.name,
           TLSSMON::MonData::NumericValue{
               static_cast<std::uint32_t>(std::min<std::uint64_t>(value, UINT32_MAX)), 0}},
          true);
    }
    if (final || last_snapshot == Clock::time_point{} ||
        now - last_snapshot >= config.full_snapshot_interval) {
      for (const auto& def : metric_definitions()) {
        auto record = engine->find_data(metric_key(def.metric));
        if (record)
          send(std::move(*record));
      }
      last_snapshot = now;
    }
    report_alarms(sample, now);
  }
  int control() {
    std::uint64_t value;
    while (::read(control_fd, &value, sizeof value) < 0 && errno == EINTR) {
    }
    try {
      tick(stopping.load());
    } catch (const std::exception& exception) {
      fail(exception.what());
    } catch (...) {
      fail("monitor sampling failed");
    }
    {
      std::lock_guard<std::mutex> lock(started_mutex);
      started = true;
    }
    started_cv.notify_all();
    if (stopping.load())
      engine->stop();
    return 0;
  }
  void cleanup() {
    // Engine callbacks capture this, but no callbacks run after the runner joins.
    alarm.reset();
    udp.reset();
    if (control_fd >= 0) {
      ::close(control_fd);
      control_fd = -1;
    }
  }
};

StorageMonitorRuntime::StorageMonitorRuntime(StorageMonitorConfig config,
                                             std::shared_ptr<StorageMetrics> metrics,
                                             std::shared_ptr<SystemResourceSampler> resources,
                                             std::function<void()> refresh)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(metrics), std::move(resources),
                                   std::move(refresh))) {}
StorageMonitorRuntime::~StorageMonitorRuntime() {
  finish_and_stop();
}
bool StorageMonitorRuntime::start() {
  auto& self = *impl_;
  std::lock_guard<std::mutex> lifecycle(self.lifecycle);
  if (self.attempted)
    return false;
  self.attempted = true;
  try {
    if (!self.metrics || self.config.sample_interval.count() <= 0 ||
        self.config.full_snapshot_interval.count() <= 0 ||
        self.config.resource_stale_after.count() <= 0)
      throw std::invalid_argument("invalid monitor metrics or interval");
    self.thresholds.emplace_back(self.config.disk_warning);
    self.thresholds.emplace_back(self.config.disk_critical);
    self.thresholds.emplace_back(self.config.inode_warning);
    self.thresholds.emplace_back(ThresholdConfig{1, 0, std::chrono::milliseconds{0}});
    self.thresholds.emplace_back(self.config.memory_warning);
    self.thresholds.emplace_back(ThresholdConfig{1, 0, std::chrono::milliseconds{0}});
    self.thresholds.emplace_back(ThresholdConfig{205, 128, std::chrono::milliseconds{10000}});
    self.thresholds.emplace_back(ThresholdConfig{1, 0, std::chrono::milliseconds{10000}});
    self.thresholds.emplace_back(ThresholdConfig{1, 0, std::chrono::milliseconds{10000}});
    if (self.engine->init() != TLSSMON::ENGINESTATE::SUCCESSFUL ||
        !register_storage_modules(*self.engine))
      throw std::runtime_error("monitor engine initialization failed");
    self.udp = std::make_shared<TLSSMON::Wire::UdpPublisher>(TLSSMON::Wire::UdpPublisherConfig{
        {self.config.collector_host, self.config.metrics_port}, TLSSMON::Wire::WireVersion::V2});
    if (!self.udp->ready())
      throw std::runtime_error("monitor UDP initialization failed");
    self.engine->set_publisher([&self](TLSSMON::MonData::StoredRecord record) {
      self.send(std::move(record));
    });
    if (self.config.alarm_sink)
      self.engine->set_alarm_publisher(self.config.alarm_sink);
    else {
      self.alarm =
          std::make_shared<TLSSMON::ReliableAlarmPublisher>(TLSSMON::ReliableAlarmPublisherConfig{
              self.config.collector_host, self.config.alarms_port, self.config.source_id,
              self.config.outbox, self.config.max_outbox_bytes, std::chrono::milliseconds{2000},
              std::chrono::milliseconds{30000}});
      if (!self.alarm->ready())
        throw std::runtime_error("monitor outbox initialization failed");
      std::weak_ptr<TLSSMON::ReliableAlarmPublisher> weak = self.alarm;
      self.engine->set_alarm_publisher([weak](TLSSMON::MonData::StoredRecord record) {
        if (auto publisher = weak.lock())
          return publisher->enqueue(std::move(record));
        return TLSSMON::AlarmEnqueueResult{};
      });
    }
    self.control_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (self.control_fd < 0 ||
        !self.engine->add_aio(self.control_fd, TLSSMON::MonCallback{"monitor-control",
                                                                    [&self] {
                                                                      return self.control();
                                                                    },
                                                                    false}))
      throw std::runtime_error("monitor control initialization failed");
    if (!self.engine->set_timer(TLSSMON::MonCallback{"storage-sampling",
                                                     [&self] {
                                                       if (!self.stopping.load()) {
                                                         try {
                                                           self.tick();
                                                         } catch (const std::exception& exception) {
                                                           self.fail(exception.what());
                                                         } catch (...) {
                                                           self.fail("monitor sampling failed");
                                                         }
                                                       }
                                                       return 0;
                                                     },
                                                     false},
                                TLSSMON::TimerFlags::RECURRING, self.config.sample_interval))
      throw std::runtime_error("monitor timer initialization failed");
    self.began = Clock::now();
    self.last_snapshot = self.began;
    self.thread = std::thread([&self] {
      current_runtime = &self;
      self.running.store(true);
      const auto result = self.engine->run();
      if (result != TLSSMON::ENGINESTATE::SUCCESSFUL)
        self.fail("monitor event loop failed");
      self.running.store(false);
      current_runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(self.started_mutex);
        self.exited = true;
      }
      self.started_cv.notify_all();
    });
    self.wake();
    std::unique_lock<std::mutex> lock(self.started_mutex);
    self.started_cv.wait(lock, [&self] {
      return self.started || self.exited;
    });
    if (!self.started || self.exited) {
      lock.unlock();
      self.thread.join();
      self.cleanup();
      return false;
    }
    return true;
  } catch (const std::exception& exception) {
    self.fail(exception.what());
    if (self.engine)
      self.engine->stop();
    if (self.thread.joinable())
      self.thread.join();
    self.cleanup();
    return false;
  }
}
void StorageMonitorRuntime::finish_and_stop() {
  auto& self = *impl_;
  if (current_runtime == &self)
    throw std::logic_error("monitor cannot join itself");
  std::lock_guard<std::mutex> lock(self.lifecycle);
  self.attempted = true;
  if (!self.thread.joinable())
    return;
  if (self.thread.get_id() == std::this_thread::get_id())
    throw std::logic_error("monitor cannot join itself");
  self.stopping.store(true);
  if (self.engine->get_phase() == TLSSMON::EnginePhase::RUNNING)
    self.wake();
  self.thread.join();
  self.running.store(false);
  self.cleanup();
}
StorageMonitorStatus StorageMonitorRuntime::status() const {
  std::lock_guard<std::mutex> lock(impl_->error_mutex);
  return {impl_->running.load(), impl_->error};
}
std::optional<TLSSMON::MonData::StoredRecord> StorageMonitorRuntime::find_data(
    Metric metric) const {
  return impl_->engine ? impl_->engine->find_data(metric_key(metric)) : std::nullopt;
}
}  // namespace TLSS::OBSERVABILITY
