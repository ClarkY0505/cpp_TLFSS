#ifndef TLSS_OBSERVABILITY_STORAGE_MONITOR_SCHEMA_H
#define TLSS_OBSERVABILITY_STORAGE_MONITOR_SCHEMA_H

#include <vector>
#include "monitor/engine.h"
#include "observability/storage_metrics.h"

namespace TLSS::OBSERVABILITY {
struct MetricDefinition {
  Metric metric;
  std::uint32_t module_id;
  std::uint32_t event_id;
  const char* name;
};
enum class ResourceAlarm : std::uint32_t {
  disk_high,
  disk_critical,
  inode_high,
  readonly,
  memory_high,
  sampling_failure,
  worker_congestion,
  database_failures,
  metadata_failures,
  count
};
const std::vector<MetricDefinition>& metric_definitions();
const std::vector<TLSSMON::MonitorModuleInfo>& storage_modules();
bool register_storage_modules(TLSSMON::Engine& engine);
TLSSMON::MonData::MonitorKey metric_key(Metric metric);
TLSSMON::MonData::MonitorKey alarm_key(ResourceAlarm alarm);
}  // namespace TLSS::OBSERVABILITY
#endif
