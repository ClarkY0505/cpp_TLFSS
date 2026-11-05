#include "observability/system_resource_sampler.h"

#include <sys/statvfs.h>
#include <array>
#include <cerrno>
#include <charconv>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace TLSS::OBSERVABILITY {
namespace {

uint64_t increment(uint64_t value) {
  return value == std::numeric_limits<uint64_t>::max() ? value : value + 1;
}

uint32_t used_percentage(uint64_t used, uint64_t total) {
  return total == 0 ? 0
                    : static_cast<uint32_t>(static_cast<long double>(used) * 10000 /
                                            static_cast<long double>(total));
}

bool multiply(uint64_t count, uint64_t unit, uint64_t& result) {
  if (unit != 0 && count > std::numeric_limits<uint64_t>::max() / unit) {
    return false;
  }
  result = count * unit;
  return true;
}

void collect_disk(const std::filesystem::path& root, DiskSnapshot& sample) {
  sample.last_attempt = std::chrono::steady_clock::now();
  struct statvfs status {};
  int error = 0;
  uint64_t total = 0;
  uint64_t used = 0;
  uint64_t available = 0;
  if (::statvfs(root.c_str(), &status) != 0) {
    error = errno;
  } else if (status.f_frsize == 0 || status.f_bfree > status.f_blocks ||
             status.f_bavail > status.f_blocks) {
    error = EINVAL;
  } else if (!multiply(status.f_blocks, status.f_frsize, total) ||
             !multiply(status.f_blocks - status.f_bfree, status.f_frsize, used) ||
             !multiply(status.f_bavail, status.f_frsize, available)) {
    error = EOVERFLOW;
  }

  sample.valid = error == 0;
  sample.error = error;
  if (error != 0) {
    sample.inode_valid = false;
    sample.consecutive_failures = increment(sample.consecutive_failures);
    return;
  }
  sample.total_bytes = total;
  sample.used_bytes = used;
  sample.available_bytes = available;
  sample.used_percent_x100 = used_percentage(used, total);
  sample.readonly = (status.f_flag & ST_RDONLY) != 0;
  sample.inode_valid = status.f_files > 0 && status.f_ffree <= status.f_files;
  if (sample.inode_valid) {
    sample.inode_used_percent_x100 =
        used_percentage(status.f_files - status.f_ffree, status.f_files);
  }
  sample.consecutive_failures = 0;
  sample.last_success = sample.last_attempt;
}

struct ProcField {
  const char* name;
  uint64_t* value;
  bool seen = false;
};

// Linux reports these fields in kB, meaning 1024-byte units. Require the
// complete value and unit so malformed or overflowed input never becomes zero.
template <std::size_t Count>
int read_proc_fields(const std::filesystem::path& path, std::array<ProcField, Count> fields) {
  errno = 0;
  std::ifstream input(path);
  if (!input) {
    return errno != 0 ? errno : EIO;
  }
  std::string line;
  while (std::getline(input, line)) {
    std::istringstream tokens(line);
    std::string name;
    tokens >> name;
    for (auto& field : fields) {
      if (name != field.name) {
        continue;
      }
      std::string number;
      std::string unit;
      std::string extra;
      if (field.seen || !(tokens >> number >> unit) || unit != "kB" || tokens >> extra) {
        return EINVAL;
      }
      uint64_t kilobytes = 0;
      const auto parsed = std::from_chars(number.data(), number.data() + number.size(), kilobytes);
      if (parsed.ec == std::errc::result_out_of_range) {
        return EOVERFLOW;
      }
      if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size()) {
        return EINVAL;
      }
      if (!multiply(kilobytes, 1024, *field.value)) {
        return EOVERFLOW;
      }
      field.seen = true;
      break;
    }
  }
  if (input.bad()) {
    return EIO;
  }
  for (const auto& field : fields) {
    if (!field.seen) {
      return EINVAL;
    }
  }
  return 0;
}

void collect_memory(const std::filesystem::path& proc_root, MemorySnapshot& sample) {
  sample.last_attempt = std::chrono::steady_clock::now();
  MemorySnapshot fresh;
  int error = read_proc_fields(
      proc_root / "meminfo", std::array<ProcField, 2>{{{"MemTotal:", &fresh.total_bytes},
                                                       {"MemAvailable:", &fresh.available_bytes}}});
  if (error == 0 && (fresh.total_bytes == 0 || fresh.available_bytes > fresh.total_bytes)) {
    error = EINVAL;
  }
  if (error == 0) {
    error = read_proc_fields(proc_root / "self" / "status",
                             std::array<ProcField, 3>{{{"VmRSS:", &fresh.rss_bytes},
                                                       {"VmHWM:", &fresh.peak_rss_bytes},
                                                       {"VmSize:", &fresh.virtual_bytes}}});
  }
  sample.valid = error == 0;
  sample.error = error;
  if (error != 0) {
    sample.consecutive_failures = increment(sample.consecutive_failures);
    return;
  }
  sample.total_bytes = fresh.total_bytes;
  sample.available_bytes = fresh.available_bytes;
  sample.rss_bytes = fresh.rss_bytes;
  sample.peak_rss_bytes = fresh.peak_rss_bytes;
  sample.virtual_bytes = fresh.virtual_bytes;
  sample.used_percent_x100 =
      used_percentage(fresh.total_bytes - fresh.available_bytes, fresh.total_bytes);
  sample.consecutive_failures = 0;
  sample.last_success = sample.last_attempt;
}

}  // namespace

SystemResourceSampler::SystemResourceSampler(SystemResourceSamplerConfig config)
    : config_(std::move(config)) {}

SystemResourceSampler::~SystemResourceSampler() {
  finish_and_stop();
}

bool SystemResourceSampler::start() {
  std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
  if (started_ || stopped_) {
    return false;
  }
  if (config_.interval.count() <= 0) {
    stopped_ = true;
    return false;
  }
  started_ = true;
  collect_and_publish();
  try {
    worker_ = std::thread(&SystemResourceSampler::run, this);
  } catch (const std::system_error&) {
    stopped_ = true;
    return false;
  }
  return true;
}

void SystemResourceSampler::finish_and_stop() {
  // Serialize join and start. The worker uses only wait/snapshot mutexes,
  // allowing stop callers to join safely while readers continue to run.
  std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
  if (stopped_) {
    return;
  }
  stopped_ = true;
  {
    std::lock_guard<std::mutex> waiting(wait_mutex_);
    stop_requested_ = true;
  }
  wakeup_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

ResourceSnapshot SystemResourceSampler::snapshot() const {
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  return snapshot_;
}

void SystemResourceSampler::collect_and_publish() {
  auto next = snapshot();
  collect_disk(config_.storage_root, next.disk);
  collect_memory(config_.proc_root, next.memory);
  next.sequence = increment(next.sequence);
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  snapshot_ = next;
}

void SystemResourceSampler::run() {
  std::unique_lock<std::mutex> waiting(wait_mutex_);
  while (!wakeup_.wait_for(waiting, config_.interval, [this] {
    return stop_requested_;
  })) {
    waiting.unlock();
    collect_and_publish();
    waiting.lock();
  }
  waiting.unlock();
  collect_and_publish();
}

}  // namespace TLSS::OBSERVABILITY
