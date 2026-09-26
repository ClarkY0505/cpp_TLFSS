#include "common/memory/tlss_memcpy.h"

#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using CopyFunction = void* (*)(void*, const void*, std::size_t);
using Clock = std::chrono::steady_clock;

constexpr std::size_t k_alignment = 4096;
constexpr std::size_t k_working_set = 192ULL * 1024ULL * 1024ULL;
constexpr std::size_t k_target_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::size_t k_round_count = 5;
constexpr double k_gib = 1024.0 * 1024.0 * 1024.0;

class AlignedBuffer {
 public:
  explicit AlignedBuffer(std::size_t size) : size_(size) {
    void* pointer = nullptr;
    if (posix_memalign(&pointer, k_alignment, size) != 0) {
      throw std::bad_alloc{};
    }
    data_ = static_cast<std::uint8_t*>(pointer);
  }

  ~AlignedBuffer() { std::free(data_); }
  AlignedBuffer(const AlignedBuffer&) = delete;
  AlignedBuffer& operator=(const AlignedBuffer&) = delete;

  std::uint8_t* data() noexcept { return data_; }
  std::size_t size() const noexcept { return size_; }

 private:
  std::uint8_t* data_{nullptr};
  std::size_t size_{0};
};

// Compile this translation unit with -fno-builtin-memcpy. Verify the call in
// this wrapper with objdump before interpreting the glibc measurements.
__attribute__((noinline)) void* glibc_copy(void* dst, const void* src, std::size_t size) {
  return ::memcpy(dst, src, size);
}

__attribute__((noinline)) void* avx2_copy(void* dst, const void* src, std::size_t size) {
  return TLSS::MEMORY::memcpy(dst, src, size, TLSS::MEMORY::CopyBackend::Avx2,
                              TLSS::MEMORY::CopyHint::Default);
}

__attribute__((noinline)) void* rep_copy(void* dst, const void* src, std::size_t size) {
  return TLSS::MEMORY::memcpy(dst, src, size, TLSS::MEMORY::CopyBackend::RepMovsb,
                              TLSS::MEMORY::CopyHint::Default);
}

__attribute__((noinline)) void* direct_nt_copy(void* dst, const void* src, std::size_t size) {
  return TLSS::MEMORY::memcpy(dst, src, size, TLSS::MEMORY::CopyBackend::NonTemporal,
                              TLSS::MEMORY::CopyHint::Streaming);
}

__attribute__((noinline)) void* auto_default_copy(void* dst, const void* src,
                                                   std::size_t size) {
  return TLSS::MEMORY::memcpy(dst, src, size);
}

__attribute__((noinline)) void* auto_streaming_copy(void* dst, const void* src,
                                                     std::size_t size) {
  return TLSS::MEMORY::memcpy(dst, src, size, TLSS::MEMORY::CopyBackend::Auto,
                              TLSS::MEMORY::CopyHint::Streaming);
}

struct BenchmarkCase {
  const char* name;
  CopyFunction copy;
};

struct BenchmarkResult {
  double gib_per_second;
  double ns_per_call;
  std::size_t call_count;
};

constexpr std::array<BenchmarkCase, 6> k_single_cases{{
    {"glibc", glibc_copy},
    {"AVX2 API", avx2_copy},
    {"REP API", rep_copy},
    {"Direct NT API", direct_nt_copy},
    {"Auto Default", auto_default_copy},
    {"Auto Streaming", auto_streaming_copy},
}};

constexpr std::array<BenchmarkCase, 3> k_runtime_cases{{
    {"glibc", glibc_copy},
    {"Direct NT API", direct_nt_copy},
    {"Auto Streaming", auto_streaming_copy},
}};

constexpr std::array<std::size_t, 12> k_sizes{{
    1024, 2048, 3072, 4096, 8192, 16384, 32768, 65536,
    131072, 262144, 524288, 1048576,
}};

constexpr std::array<std::size_t, 5> k_runtime_sizes{{
    65536, 131072, 262144, 524288, 1048576,
}};

void initialize_source(std::uint8_t* src, std::size_t size) {
  for (std::size_t index = 0; index < size; ++index) {
    src[index] = static_cast<std::uint8_t>((index * 131U + 17U) & 0xffU);
  }
}

double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  const std::size_t middle = values.size() / 2;
  return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2.0;
}

void check_result(CopyFunction copy, std::uint8_t* dst, const std::uint8_t* src,
                  std::size_t size) {
  if (copy(dst, src, size) != dst || std::memcmp(dst, src, size) != 0) {
    throw std::runtime_error("copy result mismatch");
  }
}

BenchmarkResult run_streaming_once(CopyFunction copy, std::uint8_t* dst,
                                   const std::uint8_t* src, std::size_t copy_size,
                                   std::size_t working_set_size) {
  if (copy_size == 0 || copy_size > working_set_size) {
    throw std::invalid_argument("invalid streaming copy size");
  }

  const std::size_t copies_per_pass = working_set_size / copy_size;
  const std::size_t usable_size = copies_per_pass * copy_size;
  const std::size_t passes = (k_target_bytes + usable_size - 1) / usable_size;
  const std::size_t call_count = passes * copies_per_pass;
  const std::size_t total_bytes = passes * usable_size;

  for (std::size_t index = 0; index < copies_per_pass; ++index) {
    const std::size_t offset = index * copy_size;
    if (copy(dst + offset, src + offset, copy_size) != dst + offset) {
      throw std::runtime_error("copy returned the wrong destination");
    }
  }

  std::atomic_signal_fence(std::memory_order_seq_cst);
  const auto begin = Clock::now();
  for (std::size_t pass = 0; pass < passes; ++pass) {
    for (std::size_t index = 0; index < copies_per_pass; ++index) {
      const std::size_t offset = index * copy_size;
      copy(dst + offset, src + offset, copy_size);
    }
  }
  const auto end = Clock::now();
  std::atomic_signal_fence(std::memory_order_seq_cst);

  if (std::memcmp(dst, src, usable_size) != 0) {
    throw std::runtime_error("streaming copy mismatch");
  }

  const double seconds = std::chrono::duration<double>(end - begin).count();
  return {(static_cast<double>(total_bytes) / k_gib) / seconds,
          seconds * 1.0e9 / static_cast<double>(call_count), call_count};
}

BenchmarkResult run_hot_once(CopyFunction copy, std::uint8_t* dst, const std::uint8_t* src,
                             std::size_t copy_size) {
  if (copy_size == 0) {
    throw std::invalid_argument("invalid hot copy size");
  }
  const std::size_t call_count = std::max<std::size_t>(1, k_target_bytes / copy_size);
  for (std::size_t index = 0; index < 1024; ++index) {
    copy(dst, src, copy_size);
  }

  std::atomic_signal_fence(std::memory_order_seq_cst);
  const auto begin = Clock::now();
  for (std::size_t index = 0; index < call_count; ++index) {
    copy(dst, src, copy_size);
  }
  const auto end = Clock::now();
  std::atomic_signal_fence(std::memory_order_seq_cst);

  if (std::memcmp(dst, src, copy_size) != 0) {
    throw std::runtime_error("hot copy mismatch");
  }

  const double seconds = std::chrono::duration<double>(end - begin).count();
  return {(static_cast<double>(call_count) * copy_size / k_gib) / seconds,
          seconds * 1.0e9 / static_cast<double>(call_count), call_count};
}

template <std::size_t N, std::size_t M>
void run_suite(std::string_view mode, const std::array<BenchmarkCase, N>& cases,
               const std::array<std::size_t, M>& sizes, bool hot) {
  AlignedBuffer streaming_src(hot ? 1 : k_working_set);
  AlignedBuffer streaming_dst(hot ? 1 : k_working_set);
  if (!hot) {
    initialize_source(streaming_src.data(), k_working_set);
  }

  for (std::size_t copy_size : sizes) {
    AlignedBuffer hot_src(hot ? copy_size : 1);
    AlignedBuffer hot_dst(hot ? copy_size : 1);
    if (hot) {
      initialize_source(hot_src.data(), copy_size);
    }
    auto* src = hot ? hot_src.data() : streaming_src.data();
    auto* dst = hot ? hot_dst.data() : streaming_dst.data();
    const std::size_t size = hot ? copy_size : k_working_set;
    std::array<std::vector<double>, N> bandwidth_samples;
    std::array<std::vector<double>, N> latency_samples;

    for (std::size_t round = 0; round < k_round_count; ++round) {
      for (std::size_t position = 0; position < N; ++position) {
        const std::size_t case_index = (round + position) % N;
        const BenchmarkCase& test_case = cases[case_index];
        std::memset(dst, 0, size);
        const BenchmarkResult result = hot
                                           ? run_hot_once(test_case.copy, dst, src, copy_size)
                                           : run_streaming_once(test_case.copy, dst, src, copy_size,
                                                                k_working_set);
        bandwidth_samples[case_index].push_back(result.gib_per_second);
        latency_samples[case_index].push_back(result.ns_per_call);
        std::cout << "sample," << mode << ',' << copy_size << ',' << test_case.name << ','
                  << round + 1 << ',' << std::fixed << std::setprecision(6)
                  << result.gib_per_second << ',' << result.ns_per_call << ',' << result.call_count
                  << '\n';
      }
    }

    for (std::size_t case_index = 0; case_index < N; ++case_index) {
      std::cout << "median," << mode << ',' << copy_size << ',' << cases[case_index].name
                << ",5," << std::fixed << std::setprecision(6)
                << median(bandwidth_samples[case_index]) << ','
                << median(latency_samples[case_index]) << ",0\n";
    }
    std::cout << std::flush;
  }
}

class AffinityGuard {
 public:
  explicit AffinityGuard(int cpu_id) {
    if (cpu_id < 0 || cpu_id >= CPU_SETSIZE ||
        pthread_getaffinity_np(pthread_self(), sizeof(original_), &original_) != 0 ||
        !CPU_ISSET(cpu_id, &original_)) {
      throw std::runtime_error("caller CPU is unavailable");
    }

    cpu_set_t target;
    CPU_ZERO(&target);
    CPU_SET(cpu_id, &target);
    if (pthread_setaffinity_np(pthread_self(), sizeof(target), &target) != 0) {
      throw std::runtime_error("failed to pin caller CPU");
    }
  }

  ~AffinityGuard() {
    pthread_setaffinity_np(pthread_self(), sizeof(original_), &original_);
  }

 private:
  cpu_set_t original_{};
};

void initialize_runtime_before_pinning() {
  AlignedBuffer src(256 * 1024);
  AlignedBuffer dst(256 * 1024);
  initialize_source(src.data(), src.size());
  check_result(auto_streaming_copy, dst.data(), src.data(), src.size());
}

void print_metadata(std::string_view mode) {
  cpu_set_t available;
  if (pthread_getaffinity_np(pthread_self(), sizeof(available), &available) != 0) {
    throw std::runtime_error("failed to read CPU affinity");
  }
  std::cout << "# mode=" << mode;
  if (mode == "hot") {
    std::cout << " working_set_bytes=copy_size";
  } else {
    std::cout << " working_set_bytes=" << k_working_set;
  }
  std::cout << " target_bytes_per_sample=" << k_target_bytes
            << " rounds=" << k_round_count << " affinity=";
  bool first = true;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (CPU_ISSET(cpu, &available)) {
      std::cout << (first ? "" : ":") << cpu;
      first = false;
    }
  }
  std::cout << "\n# kind,mode,size_bytes,backend,round,GiB_per_s,ns_per_call,calls\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2 || argc > 3) {
      throw std::invalid_argument("usage: memcpy_glibc_benchmark <streaming|hot|runtime> [caller_cpu]");
    }
    const std::string_view mode = argv[1];
    if (mode == "streaming" && argc == 2) {
      print_metadata(mode);
      run_suite(mode, k_single_cases, k_sizes, false);
    } else if (mode == "hot" && argc == 2) {
      print_metadata(mode);
      run_suite(mode, k_single_cases, k_sizes, true);
    } else if (mode == "runtime") {
      const int caller_cpu = argc == 3 ? std::stoi(argv[2]) : 8;
      initialize_runtime_before_pinning();
      AffinityGuard pin(caller_cpu);
      print_metadata(mode);
      run_suite(mode, k_runtime_cases, k_runtime_sizes, false);
    } else {
      throw std::invalid_argument("invalid benchmark mode or argument count");
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "benchmark failed: " << error.what() << '\n';
    return 1;
  }
}
