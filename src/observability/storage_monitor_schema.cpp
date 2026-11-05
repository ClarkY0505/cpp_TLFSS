#include "observability/storage_monitor_schema.h"
#include <stdexcept>

namespace TLSS::OBSERVABILITY {
const std::vector<MetricDefinition>& metric_definitions() {
  static const std::vector<MetricDefinition> definitions{
      {Metric::service_state, 0x1000U, 0U, "service_state"},
      {Metric::connections, 0x1000U, 1U, "connections"},
      {Metric::commands_rejected, 0x1000U, 2U, "commands_rejected"},
      {Metric::login_success, 0x1001U, 0U, "login_success"},
      {Metric::login_invalid, 0x1001U, 1U, "login_invalid"},
      {Metric::auth_backend_failures, 0x1001U, 2U, "auth_backend_failures"},
      {Metric::signup_success, 0x1001U, 3U, "signup_success"},
      {Metric::signup_failed, 0x1001U, 4U, "signup_failed"},
      {Metric::auth_queue_timeouts, 0x1001U, 5U, "auth_queue_timeouts"},
      {Metric::upload_started, 0x1002U, 0U, "upload_started"},
      {Metric::upload_active, 0x1002U, 1U, "upload_active"},
      {Metric::upload_committed, 0x1002U, 2U, "upload_committed"},
      {Metric::upload_failed, 0x1002U, 3U, "upload_failed"},
      {Metric::upload_cancelled, 0x1002U, 4U, "upload_cancelled"},
      {Metric::download_started, 0x1003U, 0U, "download_started"},
      {Metric::download_active, 0x1003U, 1U, "download_active"},
      {Metric::download_completed, 0x1003U, 2U, "download_completed"},
      {Metric::download_failed, 0x1003U, 3U, "download_failed"},
      {Metric::download_cancelled, 0x1003U, 4U, "download_cancelled"},
      {Metric::metadata_queries_failed, 0x1004U, 0U, "metadata_queries_failed"},
      {Metric::metadata_writes_failed, 0x1004U, 1U, "metadata_writes_failed"},
      {Metric::cache_requests, 0x1004U, 2U, "cache_requests"},
      {Metric::cache_hits, 0x1004U, 3U, "cache_hits"},
      {Metric::cache_loads, 0x1004U, 4U, "cache_loads"},
      {Metric::object_published, 0x1005U, 0U, "object_published"},
      {Metric::object_publish_failed, 0x1005U, 1U, "object_publish_failed"},
      {Metric::object_open_failed, 0x1005U, 2U, "object_open_failed"},
      {Metric::verification_failed, 0x1005U, 3U, "verification_failed"},
      {Metric::worker_queue, 0x1006U, 0U, "worker_queue"},
      {Metric::worker_waiting, 0x1006U, 1U, "worker_waiting"},
      {Metric::worker_rejected, 0x1006U, 2U, "worker_rejected"},
      {Metric::worker_timeouts, 0x1006U, 3U, "worker_timeouts"},
      {Metric::db_total, 0x1007U, 0U, "db_total"},
      {Metric::db_idle, 0x1007U, 1U, "db_idle"},
      {Metric::db_borrowed, 0x1007U, 2U, "db_borrowed"},
      {Metric::db_waiters, 0x1007U, 3U, "db_waiters"},
      {Metric::db_failures, 0x1007U, 4U, "db_failures"},
      {Metric::disk_active, 0x1008U, 0U, "disk_active"},
      {Metric::disk_rejected, 0x1008U, 1U, "disk_rejected"},
      {Metric::resource_disk_total_mib, 0x1009U, 0U, "resource_disk_total_mib"},
      {Metric::resource_disk_used_mib, 0x1009U, 1U, "resource_disk_used_mib"},
      {Metric::resource_disk_available_mib, 0x1009U, 2U, "resource_disk_available_mib"},
      {Metric::resource_disk_percent, 0x1009U, 3U, "resource_disk_percent"},
      {Metric::resource_inode_percent, 0x1009U, 4U, "resource_inode_percent"},
      {Metric::resource_readonly, 0x1009U, 5U, "resource_readonly"},
      {Metric::resource_disk_valid, 0x1009U, 6U, "resource_disk_valid"},
      {Metric::resource_memory_total_mib, 0x1009U, 7U, "resource_memory_total_mib"},
      {Metric::resource_memory_available_mib, 0x1009U, 8U, "resource_memory_available_mib"},
      {Metric::resource_memory_percent, 0x1009U, 9U, "resource_memory_percent"},
      {Metric::resource_rss_mib, 0x1009U, 10U, "resource_rss_mib"},
      {Metric::resource_peak_rss_mib, 0x1009U, 11U, "resource_peak_rss_mib"},
      {Metric::resource_virtual_mib, 0x1009U, 12U, "resource_virtual_mib"},
      {Metric::resource_memory_valid, 0x1009U, 13U, "resource_memory_valid"},
      {Metric::resource_sequence, 0x1009U, 14U, "resource_sequence"},
      {Metric::heartbeat, 0x100aU, 0U, "heartbeat"},
      {Metric::uptime_seconds, 0x100aU, 1U, "uptime_seconds"},
      {Metric::udp_failures, 0x100aU, 2U, "udp_failures"},
      {Metric::alarm_failures, 0x100aU, 3U, "alarm_failures"},
      {Metric::overflow, 0x100aU, 4U, "overflow"},
      {Metric::resource_inode_valid, 0x1009U, 15U, "resource_inode_valid"},
  };
  return definitions;
}
const std::vector<TLSSMON::MonitorModuleInfo>& storage_modules() {
  static const auto modules = [] {
    std::vector<TLSSMON::MonitorModuleInfo> result;
    result.emplace_back(0x1000U, "service", "storage service",
                        std::vector<TLSSMON::MonitorErrorInfo>{
                            {TLSSMON::MonitorLevel::INFO, "service_state"},
                            {TLSSMON::MonitorLevel::INFO, "connections"},
                            {TLSSMON::MonitorLevel::INFO, "commands_rejected"},
                        });
    result.emplace_back(0x1001U, "auth", "storage auth",
                        std::vector<TLSSMON::MonitorErrorInfo>{
                            {TLSSMON::MonitorLevel::INFO, "login_success"},
                            {TLSSMON::MonitorLevel::INFO, "login_invalid"},
                            {TLSSMON::MonitorLevel::INFO, "auth_backend_failures"},
                            {TLSSMON::MonitorLevel::INFO, "signup_success"},
                            {TLSSMON::MonitorLevel::INFO, "signup_failed"},
                            {TLSSMON::MonitorLevel::INFO, "auth_queue_timeouts"},
                        });
    result.emplace_back(0x1002U, "upload", "storage upload",
                        std::vector<TLSSMON::MonitorErrorInfo>{
                            {TLSSMON::MonitorLevel::INFO, "upload_started"},
                            {TLSSMON::MonitorLevel::INFO, "upload_active"},
                            {TLSSMON::MonitorLevel::INFO, "upload_committed"},
                            {TLSSMON::MonitorLevel::INFO, "upload_failed"},
                            {TLSSMON::MonitorLevel::INFO, "upload_cancelled"},
                        });
    result.emplace_back(0x1003U, "download", "storage download",
                        std::vector<TLSSMON::MonitorErrorInfo>{
                            {TLSSMON::MonitorLevel::INFO, "download_started"},
                            {TLSSMON::MonitorLevel::INFO, "download_active"},
                            {TLSSMON::MonitorLevel::INFO, "download_completed"},
                            {TLSSMON::MonitorLevel::INFO, "download_failed"},
                            {TLSSMON::MonitorLevel::INFO, "download_cancelled"},
                        });
    result.emplace_back(0x1004U, "metadata", "storage metadata",
                        std::vector<TLSSMON::MonitorErrorInfo>{
                            {TLSSMON::MonitorLevel::INFO, "metadata_queries_failed"},
                            {TLSSMON::MonitorLevel::INFO, "metadata_writes_failed"},
                            {TLSSMON::MonitorLevel::INFO, "cache_requests"},
                            {TLSSMON::MonitorLevel::INFO, "cache_hits"},
                            {TLSSMON::MonitorLevel::INFO, "cache_loads"},
                        });
    result.emplace_back(0x1005U, "object_storage", "storage object_storage",
                        std::vector<TLSSMON::MonitorErrorInfo>{
                            {TLSSMON::MonitorLevel::INFO, "object_published"},
                            {TLSSMON::MonitorLevel::INFO, "object_publish_failed"},
                            {TLSSMON::MonitorLevel::INFO, "object_open_failed"},
                            {TLSSMON::MonitorLevel::INFO, "verification_failed"},
                        });
    result.emplace_back(0x1006U, "worker_pool", "storage worker_pool",
                        std::vector<TLSSMON::MonitorErrorInfo>{
                            {TLSSMON::MonitorLevel::INFO, "worker_queue"},
                            {TLSSMON::MonitorLevel::INFO, "worker_waiting"},
                            {TLSSMON::MonitorLevel::INFO, "worker_rejected"},
                            {TLSSMON::MonitorLevel::INFO, "worker_timeouts"},
                        });
    result.emplace_back(0x1007U, "db_pool", "storage db_pool",
                        std::vector<TLSSMON::MonitorErrorInfo>{
                            {TLSSMON::MonitorLevel::INFO, "db_total"},
                            {TLSSMON::MonitorLevel::INFO, "db_idle"},
                            {TLSSMON::MonitorLevel::INFO, "db_borrowed"},
                            {TLSSMON::MonitorLevel::INFO, "db_waiters"},
                            {TLSSMON::MonitorLevel::INFO, "db_failures"},
                        });
    result.emplace_back(0x1008U, "disk_executor", "storage disk_executor",
                        std::vector<TLSSMON::MonitorErrorInfo>{
                            {TLSSMON::MonitorLevel::INFO, "disk_active"},
                            {TLSSMON::MonitorLevel::INFO, "disk_rejected"},
                        });
    result.emplace_back(0x1009U, "resources", "storage resources",
                        std::vector<TLSSMON::MonitorErrorInfo>{
                            {TLSSMON::MonitorLevel::INFO, "resource_disk_total_mib"},
                            {TLSSMON::MonitorLevel::INFO, "resource_disk_used_mib"},
                            {TLSSMON::MonitorLevel::INFO, "resource_disk_available_mib"},
                            {TLSSMON::MonitorLevel::INFO, "resource_disk_percent"},
                            {TLSSMON::MonitorLevel::INFO, "resource_inode_percent"},
                            {TLSSMON::MonitorLevel::INFO, "resource_readonly"},
                            {TLSSMON::MonitorLevel::INFO, "resource_disk_valid"},
                            {TLSSMON::MonitorLevel::INFO, "resource_memory_total_mib"},
                            {TLSSMON::MonitorLevel::INFO, "resource_memory_available_mib"},
                            {TLSSMON::MonitorLevel::INFO, "resource_memory_percent"},
                            {TLSSMON::MonitorLevel::INFO, "resource_rss_mib"},
                            {TLSSMON::MonitorLevel::INFO, "resource_peak_rss_mib"},
                            {TLSSMON::MonitorLevel::INFO, "resource_virtual_mib"},
                            {TLSSMON::MonitorLevel::INFO, "resource_memory_valid"},
                            {TLSSMON::MonitorLevel::INFO, "resource_sequence"},
                            {TLSSMON::MonitorLevel::INFO, "resource_inode_valid"},
                        });
    result.emplace_back(0x100aU, "monitor_runtime", "storage monitor_runtime",
                        std::vector<TLSSMON::MonitorErrorInfo>{
                            {TLSSMON::MonitorLevel::INFO, "heartbeat"},
                            {TLSSMON::MonitorLevel::INFO, "uptime_seconds"},
                            {TLSSMON::MonitorLevel::INFO, "udp_failures"},
                            {TLSSMON::MonitorLevel::INFO, "alarm_failures"},
                            {TLSSMON::MonitorLevel::INFO, "overflow"},
                        });
    result.emplace_back(0x1100U, "storage_alarms", "storage resource conditions",
                        std::vector<TLSSMON::MonitorErrorInfo>{
                            {TLSSMON::MonitorLevel::WARN, "disk capacity high"},
                            {TLSSMON::MonitorLevel::EMERGENCY_STOP, "disk capacity critical"},
                            {TLSSMON::MonitorLevel::WARN, "inode capacity high"},
                            {TLSSMON::MonitorLevel::EMERGENCY_STOP, "filesystem read only"},
                            {TLSSMON::MonitorLevel::WARN, "host memory high"},
                            {TLSSMON::MonitorLevel::WARN, "resource sampling unavailable"},
                            {TLSSMON::MonitorLevel::WARN, "business queue congestion"},
                            {TLSSMON::MonitorLevel::WARN, "database borrow failures"},
                            {TLSSMON::MonitorLevel::WARN, "metadata write failures"}});
    return result;
  }();
  return modules;
}
bool register_storage_modules(TLSSMON::Engine& engine) {
  for (const auto& module : storage_modules())
    if (engine.register_module(module) != TLSSMON::ModuleRegisterStatus::SUCCESS)
      return false;
  return true;
}
TLSSMON::MonData::MonitorKey metric_key(Metric metric) {
  const auto index = static_cast<std::size_t>(metric);
  if (index >= metric_definitions().size())
    throw std::invalid_argument("invalid metric");
  const auto& def = metric_definitions()[index];
  return {def.module_id, 0, 1, def.event_id};
}
TLSSMON::MonData::MonitorKey alarm_key(ResourceAlarm alarm) {
  const auto index = static_cast<std::uint32_t>(alarm);
  const auto& module = storage_modules().back();
  if (index >= module._errors.size())
    throw std::invalid_argument("invalid alarm");
  return {module._mid, static_cast<std::uint32_t>(module._errors[index]._level), 1, index};
}
}  // namespace TLSS::OBSERVABILITY
