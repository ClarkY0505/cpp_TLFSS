#ifndef TLSS_OBSERVABILITY_STORAGE_METRICS_H
#define TLSS_OBSERVABILITY_STORAGE_METRICS_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace TLSS::OBSERVABILITY {
enum class Metric : std::size_t {
  service_state,
  connections,
  commands_rejected,
  login_success,
  login_invalid,
  auth_backend_failures,
  signup_success,
  signup_failed,
  auth_queue_timeouts,
  upload_started,
  upload_active,
  upload_committed,
  upload_failed,
  upload_cancelled,
  download_started,
  download_active,
  download_completed,
  download_failed,
  download_cancelled,
  metadata_queries_failed,
  metadata_writes_failed,
  cache_requests,
  cache_hits,
  cache_loads,
  object_published,
  object_publish_failed,
  object_open_failed,
  verification_failed,
  worker_queue,
  worker_waiting,
  worker_rejected,
  worker_timeouts,
  db_total,
  db_idle,
  db_borrowed,
  db_waiters,
  db_failures,
  disk_active,
  disk_rejected,
  resource_disk_total_mib,
  resource_disk_used_mib,
  resource_disk_available_mib,
  resource_disk_percent,
  resource_inode_percent,
  resource_readonly,
  resource_disk_valid,
  resource_memory_total_mib,
  resource_memory_available_mib,
  resource_memory_percent,
  resource_rss_mib,
  resource_peak_rss_mib,
  resource_virtual_mib,
  resource_memory_valid,
  resource_sequence,
  heartbeat,
  uptime_seconds,
  udp_failures,
  alarm_failures,
  overflow,
  resource_inode_valid,
  count
};

class StorageMetrics {
 public:
  static constexpr std::size_t size = static_cast<std::size_t>(Metric::count);
  using Snapshot = std::array<std::uint64_t, size>;
  void add(Metric metric, std::uint64_t amount = 1) noexcept {
    auto& value = values_[static_cast<std::size_t>(metric)];
    auto current = value.load(std::memory_order_relaxed);
    for (;;) {
      const auto maximum = std::numeric_limits<std::uint64_t>::max();
      const auto next = amount > maximum - current ? maximum : current + amount;
      if (value.compare_exchange_weak(current, next, std::memory_order_relaxed))
        return;
    }
  }
  void set(Metric metric, std::uint64_t value) noexcept {
    values_[static_cast<std::size_t>(metric)].store(value, std::memory_order_relaxed);
  }
  void subtract(Metric metric, std::uint64_t amount = 1) noexcept {
    auto& value = values_[static_cast<std::size_t>(metric)];
    auto current = value.load(std::memory_order_relaxed);
    while (!value.compare_exchange_weak(current, amount > current ? 0 : current - amount,
                                        std::memory_order_relaxed)) {
    }
  }
  std::uint64_t get(Metric metric) const noexcept {
    return values_[static_cast<std::size_t>(metric)].load(std::memory_order_relaxed);
  }
  // Independent observations; the complete array is not one atomic transaction.
  Snapshot snapshot() const noexcept {
    Snapshot result{};
    for (std::size_t i = 0; i < size; ++i)
      result[i] = values_[i].load(std::memory_order_relaxed);
    return result;
  }

 private:
  std::array<std::atomic<std::uint64_t>, size> values_{};
};
}  // namespace TLSS::OBSERVABILITY
#endif
