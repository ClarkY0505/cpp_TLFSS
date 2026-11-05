#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <thread>

namespace TLSS::OBSERVABILITY {

struct SystemResourceSamplerConfig {
  std::filesystem::path storage_root;
  std::chrono::milliseconds interval{5000};
  std::filesystem::path proc_root{"/proc"};
};

struct DiskSnapshot {
  bool valid = false;
  bool inode_valid = false;
  bool readonly = false;
  uint64_t total_bytes = 0;
  uint64_t used_bytes = 0;
  uint64_t available_bytes = 0;
  uint32_t used_percent_x100 = 0;
  uint32_t inode_used_percent_x100 = 0;
  int error = 0;
  uint64_t consecutive_failures = 0;
  std::chrono::steady_clock::time_point last_attempt{};
  std::chrono::steady_clock::time_point last_success{};
};

struct MemorySnapshot {
  bool valid = false;
  uint64_t total_bytes = 0;
  uint64_t available_bytes = 0;
  uint64_t rss_bytes = 0;
  uint64_t peak_rss_bytes = 0;
  uint64_t virtual_bytes = 0;
  uint32_t used_percent_x100 = 0;
  int error = 0;
  uint64_t consecutive_failures = 0;
  std::chrono::steady_clock::time_point last_attempt{};
  std::chrono::steady_clock::time_point last_success{};
};

struct ResourceSnapshot {
  DiskSnapshot disk;
  MemorySnapshot memory;
  uint64_t sequence = 0;
};

class SystemResourceSampler {
 public:
  explicit SystemResourceSampler(SystemResourceSamplerConfig config);
  ~SystemResourceSampler();

  // One-shot lifecycle. A successful start publishes an initial sample before
  // returning, even if either source is unavailable. Nonpositive intervals
  // are rejected. Stopping before start permanently closes the lifecycle.
  bool start();
  // Wake the sampler, publish its final collection, and join. Idempotent.
  void finish_and_stop();
  // Failed sources retain their last good values with valid=false and fresh
  // error metadata. Percentages use 10000 for 100% of the resource total.
  ResourceSnapshot snapshot() const;

 private:
  void collect_and_publish();
  void run();

  const SystemResourceSamplerConfig config_;
  mutable std::mutex snapshot_mutex_;
  ResourceSnapshot snapshot_;
  std::mutex lifecycle_mutex_;
  std::mutex wait_mutex_;
  std::condition_variable wakeup_;
  bool started_ = false;
  bool stopped_ = false;
  bool stop_requested_ = false;
  std::thread worker_;
};

}  // namespace TLSS::OBSERVABILITY
