#include "observability/system_resource_sampler.h"

#include <sys/statvfs.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace TLSS::OBSERVABILITY;
using namespace std::chrono_literals;

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

struct Fixture {
  std::filesystem::path root;
  std::filesystem::path storage;
  std::filesystem::path proc;

  Fixture() {
    char pattern[] = "/tmp/tlss-resource-XXXXXX";
    const char* directory = ::mkdtemp(pattern);
    if (!directory) {
      throw std::runtime_error("mkdtemp failed");
    }
    root = directory;
    storage = root / "storage";
    proc = root / "proc";
    std::filesystem::create_directory(storage);
    std::filesystem::create_directories(proc / "self");
    write(proc / "meminfo", "MemTotal: 10000 kB\nMemFree: 123 kB\nMemAvailable: 2500 kB\n");
    write(proc / "self/status", "Name:\tfixture\nVmSize: 800 kB\nVmHWM: 300 kB\nVmRSS: 200 kB\n");
  }

  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }

  static void write(const std::filesystem::path& path, const std::string& content) {
    std::ofstream output(path);
    output << content;
    if (!output) {
      throw std::runtime_error("fixture write failed");
    }
  }

  SystemResourceSamplerConfig config() const {
    return {storage, 1h, proc};
  }
};

void initial_collection_has_disk_and_memory() {
  Fixture fixture;
  SystemResourceSampler sampler(fixture.config());
  require(sampler.snapshot().sequence == 0, "pre-start snapshot must be empty");
  require(sampler.start(), "first start must succeed");
  const auto sample = sampler.snapshot();
  require(sample.sequence == 1, "start must publish initial collection");
  require(sample.disk.valid && sample.memory.valid, "both sources must be valid");
  struct statvfs disk {};
  require(::statvfs(fixture.storage.c_str(), &disk) == 0, "statvfs fixture failed");
  require(sample.disk.total_bytes == disk.f_blocks * disk.f_frsize, "disk total mismatch");
  require(sample.disk.used_bytes == (disk.f_blocks - disk.f_bfree) * disk.f_frsize,
          "disk used mismatch");
  require(sample.disk.available_bytes <= sample.disk.total_bytes, "disk available exceeds total");
  require(sample.disk.readonly == bool(disk.f_flag & ST_RDONLY), "readonly flag mismatch");
  require(sample.disk.inode_valid == (disk.f_files > 0), "inode validity mismatch");
  require(sample.disk.used_percent_x100 <= 10000, "disk percentage out of range");
  require(sample.memory.total_bytes == 10000 * 1024, "memory total mismatch");
  require(sample.memory.available_bytes == 2500 * 1024, "memory available mismatch");
  require(sample.memory.rss_bytes == 200 * 1024, "RSS mismatch");
  require(sample.memory.peak_rss_bytes == 300 * 1024, "peak RSS mismatch");
  require(sample.memory.virtual_bytes == 800 * 1024, "virtual memory mismatch");
  require(sample.memory.used_percent_x100 == 7500, "memory percentage mismatch");
  require(sample.disk.last_success == sample.disk.last_attempt, "disk success timestamp mismatch");
  require(sample.memory.last_success == sample.memory.last_attempt, "memory timestamp mismatch");
  require(!sampler.start(), "second start must be rejected");
  sampler.finish_and_stop();
  require(sampler.snapshot().sequence == 2, "stop must publish final collection");
}

void disk_failure_does_not_create_storage_or_invalidate_memory() {
  Fixture fixture;
  std::filesystem::remove(fixture.storage);
  SystemResourceSampler sampler(fixture.config());
  require(sampler.start(), "sampling unavailable sources must still start");
  const auto sample = sampler.snapshot();
  require(!sample.disk.valid && sample.disk.error == ENOENT, "missing storage must report ENOENT");
  require(sample.disk.consecutive_failures == 1, "disk failure count mismatch");
  require(sample.memory.valid, "disk failure must not invalidate memory");
  require(!std::filesystem::exists(fixture.storage), "sampler must not create storage");
}

void malformed_memory_is_invalid() {
  const std::vector<std::string> bad_inputs = {
      "MemTotal: 10000 kB\n",  // Missing MemAvailable.
      "MemTotal: 0 kB\nMemAvailable: 0 kB\n",
      "MemTotal: 100 kB\nMemAvailable: 101 kB\n",
      "MemTotal: -1 kB\nMemAvailable: 0 kB\n",
      "MemTotal: 18446744073709551616 kB\nMemAvailable: 0 kB\n",
      "MemTotal: 18446744073709551615 kB\nMemAvailable: 0 kB\n",
      "MemTotal: 10000 MB\nMemAvailable: 100 kB\n",
      "MemTotal: 10000x kB\nMemAvailable: 100 kB\n",
      "MemTotal: 10000 kB extra\nMemAvailable: 100 kB\n",
      "MemTotal: 10000 kB\nMemTotal: 10000 kB\nMemAvailable: 100 kB\n"};
  for (const auto& input : bad_inputs) {
    Fixture fixture;
    Fixture::write(fixture.proc / "meminfo", input);
    SystemResourceSampler sampler(fixture.config());
    require(sampler.start(), "malformed fixture start failed");
    const auto sample = sampler.snapshot();
    require(sample.disk.valid, "malformed memory must not invalidate disk");
    require(!sample.memory.valid && sample.memory.error != 0, "malformed memory must report error");
    require(sample.memory.consecutive_failures == 1, "memory failure count mismatch");
    require(sample.memory.total_bytes == 0, "partial memory must not be published");
    require(sample.memory.last_success == std::chrono::steady_clock::time_point{},
            "failure must not record success");
  }
}

void invalid_process_status_and_missing_proc_are_invalid() {
  for (const auto& status :
       {"VmRSS: 1 kB\nVmSize: 3 kB\n", "VmRSS: bogus kB\nVmHWM: 2 kB\nVmSize: 3 kB\n",
        "VmRSS: 18446744073709551615 kB\nVmHWM: 2 kB\nVmSize: 3 kB\n"}) {
    Fixture fixture;
    Fixture::write(fixture.proc / "self/status", status);
    SystemResourceSampler sampler(fixture.config());
    require(sampler.start(), "status fixture start failed");
    require(!sampler.snapshot().memory.valid && sampler.snapshot().memory.error != 0,
            "missing or malformed process field must fail");
  }
  Fixture fixture;
  std::filesystem::remove_all(fixture.proc);
  SystemResourceSampler sampler(fixture.config());
  require(sampler.start(), "missing proc start failed");
  require(!sampler.snapshot().memory.valid && sampler.snapshot().memory.error == ENOENT,
          "missing proc must report ENOENT");
}

void failure_retains_last_good_values() {
  Fixture fixture;
  SystemResourceSampler sampler(fixture.config());
  require(sampler.start(), "fixture start failed");
  const auto good = sampler.snapshot();
  std::filesystem::remove(fixture.storage);
  std::filesystem::remove(fixture.proc / "meminfo");
  sampler.finish_and_stop();
  const auto bad = sampler.snapshot();
  require(bad.sequence == 2, "final failure must be published");
  require(!bad.disk.valid && !bad.memory.valid, "final errors must invalidate sources");
  require(!bad.disk.inode_valid, "failed disk must invalidate inode data");
  require(bad.disk.total_bytes == good.disk.total_bytes, "last good disk total lost");
  require(bad.disk.used_bytes == good.disk.used_bytes, "last good disk used lost");
  require(bad.memory.total_bytes == good.memory.total_bytes, "last good memory total lost");
  require(bad.memory.rss_bytes == good.memory.rss_bytes, "last good RSS lost");
  require(bad.disk.last_success == good.disk.last_success, "disk last success overwritten");
  require(bad.memory.last_success == good.memory.last_success, "memory last success overwritten");
  require(bad.disk.last_attempt >= good.disk.last_attempt, "disk attempt timestamp regressed");
  require(bad.disk.consecutive_failures == 1 && bad.memory.consecutive_failures == 1,
          "final failure counts mismatch");
}

void stop_is_immediate_idempotent_and_terminal() {
  Fixture fixture;
  SystemResourceSampler sampler(fixture.config());
  require(sampler.start(), "fixture start failed");
  const auto start = std::chrono::steady_clock::now();
  sampler.finish_and_stop();
  require(std::chrono::steady_clock::now() - start < 2s, "stop waited for long sampling interval");
  const auto sequence = sampler.snapshot().sequence;
  sampler.finish_and_stop();
  require(sampler.snapshot().sequence == sequence, "repeated stop collected again");
  require(!sampler.start(), "restart after stop must fail");
  SystemResourceSampler stopped(fixture.config());
  stopped.finish_and_stop();
  require(!stopped.start(), "stop-before-start must prevent restart");
  require(stopped.snapshot().sequence == 0, "stop-before-start must not collect");
}

void periodic_sampling_recovers_after_failure() {
  Fixture fixture;
  auto config = fixture.config();
  config.interval = 5ms;
  Fixture::write(fixture.proc / "meminfo", "invalid\n");
  SystemResourceSampler sampler(config);
  require(sampler.start(), "fixture start failed");
  require(!sampler.snapshot().memory.valid, "bad memory fixture must fail initially");
  auto deadline = std::chrono::steady_clock::now() + 2s;
  while (sampler.snapshot().memory.consecutive_failures < 3 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  require(sampler.snapshot().memory.consecutive_failures >= 3, "periodic failures must accumulate");
  Fixture::write(fixture.proc / "meminfo", "MemTotal: 100 kB\nMemAvailable: 20 kB\n");
  deadline = std::chrono::steady_clock::now() + 2s;
  while (!sampler.snapshot().memory.valid && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const auto recovered = sampler.snapshot();
  require(recovered.memory.valid && recovered.memory.consecutive_failures == 0,
          "valid collection must reset failures");
  require(recovered.memory.error == 0 && recovered.memory.total_bytes == 100 * 1024,
          "recovery must publish current memory");
}

void default_proc_root_collects_real_process_memory() {
  Fixture fixture;
  SystemResourceSampler sampler({fixture.storage, 1h});
  require(sampler.start(), "real proc sampler start failed");
  const auto sample = sampler.snapshot();
  require(sample.memory.valid && sample.memory.total_bytes > 0,
          "default proc root must collect host memory");
  require(sample.memory.rss_bytes > 0 && sample.memory.virtual_bytes > 0,
          "default proc root must collect current process memory");
}

void readers_and_concurrent_stoppers_are_safe() {
  Fixture fixture;
  auto config = fixture.config();
  config.interval = 1ms;
  SystemResourceSampler sampler(config);
  require(sampler.start(), "fixture start failed");
  std::atomic<bool> stop_readers{false};
  std::atomic<bool> inconsistent{false};
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([&] {
      uint64_t sequence = 0;
      while (!stop_readers.load()) {
        const auto sample = sampler.snapshot();
        if (sample.sequence < sequence || !sample.memory.valid ||
            sample.memory.total_bytes != 10000 * 1024 || sample.memory.used_percent_x100 != 7500) {
          inconsistent.store(true);
        }
        sequence = sample.sequence;
      }
    });
  }
  std::this_thread::sleep_for(20ms);
  std::thread first([&] {
    sampler.finish_and_stop();
  });
  std::thread second([&] {
    sampler.finish_and_stop();
  });
  first.join();
  second.join();
  stop_readers.store(true);
  for (auto& reader : readers) {
    reader.join();
  }
  require(!inconsistent.load(), "concurrent reader saw inconsistent snapshot");
  require(sampler.snapshot().sequence >= 2, "concurrent stop must publish final snapshot");
}

}  // namespace

int main() {
  const std::vector<std::pair<const char*, std::function<void()>>> tests = {
      {"initial_collection_has_disk_and_memory", initial_collection_has_disk_and_memory},
      {"disk_failure_does_not_create_storage_or_invalidate_memory",
       disk_failure_does_not_create_storage_or_invalidate_memory},
      {"malformed_memory_is_invalid", malformed_memory_is_invalid},
      {"invalid_process_status_and_missing_proc_are_invalid",
       invalid_process_status_and_missing_proc_are_invalid},
      {"failure_retains_last_good_values", failure_retains_last_good_values},
      {"stop_is_immediate_idempotent_and_terminal", stop_is_immediate_idempotent_and_terminal},
      {"periodic_sampling_recovers_after_failure", periodic_sampling_recovers_after_failure},
      {"default_proc_root_collects_real_process_memory",
       default_proc_root_collects_real_process_memory},
      {"readers_and_concurrent_stoppers_are_safe", readers_and_concurrent_stoppers_are_safe}};
  unsigned failures = 0;
  for (const auto& test : tests) {
    try {
      test.second();
      std::cout << "PASS " << test.first << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << test.first << ": " << error.what() << '\n';
    }
  }
  std::cout << tests.size() - failures << '/' << tests.size() << " tests passed\n";
  return failures == 0 ? 0 : 1;
}
