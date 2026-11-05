#ifndef TLSS_OBSERVABILITY_STORAGE_MONITOR_RUNTIME_H
#define TLSS_OBSERVABILITY_STORAGE_MONITOR_RUNTIME_H

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include "observability/storage_metrics.h"
#include "observability/storage_monitor_schema.h"
#include "observability/system_resource_sampler.h"

namespace TLSS::OBSERVABILITY {
struct ThresholdConfig {
  std::uint32_t trigger;
  std::uint32_t recovery;
  std::chrono::milliseconds duration{10000};
};
// State is owned by the monitor thread; missing data resets only the pending duration.
class ThresholdAlarm {
 public:
  explicit ThresholdAlarm(ThresholdConfig config);
  bool feed(std::optional<std::uint32_t> value, std::chrono::steady_clock::time_point now);

 private:
  ThresholdConfig config_;
  bool active_ = false;
  std::optional<std::chrono::steady_clock::time_point> pending_since_;
};

struct StorageMonitorConfig {
  std::string collector_host = "127.0.0.1";
  std::uint16_t metrics_port = 9300;
  std::uint16_t alarms_port = 9400;
  std::uint64_t source_id = 1;
  std::filesystem::path outbox = "./data/monitor-outbox";
  std::uint64_t max_outbox_bytes = 64ULL * 1024 * 1024;
  std::chrono::milliseconds sample_interval{1000};
  std::chrono::milliseconds full_snapshot_interval{10000};
  std::chrono::milliseconds resource_stale_after{15000};
  ThresholdConfig disk_warning{8500, 8000};
  ThresholdConfig disk_critical{9500, 9000};
  ThresholdConfig inode_warning{9000, 8500};
  ThresholdConfig memory_warning{9000, 8500};
  // Optional durable destination, primarily for embedding and fault-injection tests.
  // It is called synchronously on the monitor thread and must honor durable().
  TLSSMON::AlarmPublisher alarm_sink;
};
StorageMonitorConfig monitor_config_from_environment();

struct StorageMonitorStatus {
  bool running = false;
  std::string error;
};
class StorageMonitorRuntime {
 public:
  StorageMonitorRuntime(StorageMonitorConfig config, std::shared_ptr<StorageMetrics> metrics,
                        std::shared_ptr<SystemResourceSampler> resources = {},
                        std::function<void()> refresh_metrics = {});
  ~StorageMonitorRuntime();
  StorageMonitorRuntime(const StorageMonitorRuntime&) = delete;
  StorageMonitorRuntime& operator=(const StorageMonitorRuntime&) = delete;
  bool start();
  // Call outside the monitor thread, after all business producers and sampler have stopped.
  // Final sampling happens on the monitor thread. Unacknowledged outbox files survive.
  void finish_and_stop();
  StorageMonitorStatus status() const;
  std::optional<TLSSMON::MonData::StoredRecord> find_data(Metric metric) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace TLSS::OBSERVABILITY
#endif
