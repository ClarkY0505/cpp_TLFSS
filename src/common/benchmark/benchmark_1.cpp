#include <immintrin.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include "common/memory/internal/copy_executor.h"
#include "common/memory/internal/copy_capability_policy.h"
#include "common/memory/internal/copy_partition.h"
#include "common/memory/internal/copy_policy.h"
#include "common/memory/internal/cpu_affinity.h"
#include "common/memory/internal/cpu_capabilities.h"
#include "common/memory/internal/cpu_core_type.h"
#include "common/memory/internal/cpu_topology.h"
#include "common/memory/internal/direct_nt_copy.h"
#include "common/memory/internal/memory_runtime.h"
#include "common/memory/internal/parallel_copy_pool.h"
#include "common/memory/internal/parallel_nt_copy.h"
#include "common/memory/internal/worker_selection.h"
#include "common/memory/tlss_memcpy.h"

volatile std::uint8_t benchmark_sink = 0;
namespace {

#ifdef TLSS_MEMORY_TESTING
std::atomic<bool> g_shutdown_order_probe_enabled{false};

class ShutdownOrderProbe {
 public:
  ~ShutdownOrderProbe() {
    if (!g_shutdown_order_probe_enabled.load(std::memory_order_acquire)) {
      return;
    }

    const bool started =
        TLSS::MEMORY::INTERNAL::memory_runtime_destruction_started_for_test();
    std::fprintf(stderr, "shutdown probe: runtime destruction started=%d\n",
                 started ? 1 : 0);
    if (started) {
      std::abort();
    }
  }
};

ShutdownOrderProbe g_shutdown_order_probe;
#endif

int get_perf_fd(const char* environment_name) {
  const char* value = std::getenv(environment_name);

  if (value == nullptr) {
    return -1;
  }

  return std::stoi(value);
}

void write_perf_command(int fd, const char* command) {
  const std::size_t command_size = std::strlen(command);

  std::size_t written_size = 0;

  while (written_size < command_size) {
    const ssize_t result = ::write(fd, command + written_size, command_size - written_size);

    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }

      throw std::runtime_error("failed to write perf control command");
    }

    written_size += static_cast<std::size_t>(result);
  }
}

void wait_perf_ack(int fd) {
  char buffer[16] = {};
  std::size_t offset = 0;

  while (true) {
    char character = 0;

    const ssize_t result = ::read(fd, &character, 1);

    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }

      throw std::runtime_error("failed to read perf acknowledgement");
    }

    if (result == 0) {
      throw std::runtime_error("perf acknowledgement pipe closed");
    }

    //
    // perf（Linux 性能分析工具）当前会在
    // "ack\n" 后面额外写入一个 NUL（空字符）。
    //
    // 第一次调用读到 '\n' 后，
    // '\0' 会留在 FIFO（先进先出管道）里，
    // 因此下一次读取时需要直接忽略。
    //
    if (character == '\0' || character == '\r') {
      continue;
    }

    if (character == '\n') {
      break;
    }

    if (offset >= sizeof(buffer) - 1) {
      throw std::runtime_error("perf acknowledgement too long");
    }

    buffer[offset++] = character;
  }

  buffer[offset] = '\0';

  if (std::strcmp(buffer, "ack") != 0) {
    std::cerr << "unexpected perf acknowledgement: [" << buffer << "]\n";

    throw std::runtime_error("invalid perf acknowledgement");
  }
}

void perf_enable() {
  const int control_fd = get_perf_fd("TLSS_PERF_CONTROL_FD");

  const int ack_fd = get_perf_fd("TLSS_PERF_ACK_FD");

  //
  // 普通运行 benchmark（基准测试）时，
  // 没有设置环境变量，就什么都不做。
  //
  if (control_fd < 0 || ack_fd < 0) {
    return;
  }

  write_perf_command(control_fd, "enable\n");

  //
  // 等待 perf（性能分析工具）
  // 确认 counter（计数器）已经真正启用。
  //
  wait_perf_ack(ack_fd);
}

void perf_disable() {
  const int control_fd = get_perf_fd("TLSS_PERF_CONTROL_FD");

  const int ack_fd = get_perf_fd("TLSS_PERF_ACK_FD");

  if (control_fd < 0 || ack_fd < 0) {
    return;
  }

  write_perf_command(control_fd, "disable\n");

  //
  // 等待 counter（计数器）真正关闭。
  //
  wait_perf_ack(ack_fd);
}

}  // namespace

// ==================================================
// std::memcpy wrapper
//
// noinline：
// 尽量保证 libc memcpy 也经历真实函数调用。
// ==================================================

__attribute__((noinline)) void* libc_memcpy(void* dst, const void* src, std::size_t n) {
  return std::memcpy(dst, src, n);
}

__attribute__((noinline)) void* tlss_avx2_memcpy(void* dst, const void* src, std::size_t n) {
  return TLSS::MEMORY::memcpy(dst, src, n, TLSS::MEMORY::CopyBackend::Avx2,
                              TLSS::MEMORY::CopyHint::Default);
}

__attribute__((noinline)) void* tlss_rep_memcpy(void* dst, const void* src, std::size_t n) {
  return TLSS::MEMORY::memcpy(dst, src, n, TLSS::MEMORY::CopyBackend::RepMovsb,
                              TLSS::MEMORY::CopyHint::Default);
}

__attribute__((noinline)) void* tlss_auto_memcpy(void* dst, const void* src, std::size_t n) {
  return TLSS::MEMORY::memcpy(dst, src, n);
}

__attribute__((noinline)) void* tlss_auto_streaming_memcpy(void* dst, const void* src,
                                                           std::size_t n) {
  return TLSS::MEMORY::memcpy(dst, src, n, TLSS::MEMORY::CopyBackend::Auto,
                              TLSS::MEMORY::CopyHint::Streaming);
}

__attribute__((noinline)) void* tlss_direct_nt_memcpy(void* dst, const void* src,
                                                      std::size_t n) {
  return TLSS::MEMORY::memcpy(dst, src, n, TLSS::MEMORY::CopyBackend::NonTemporal,
                              TLSS::MEMORY::CopyHint::Default);
}

extern "C" void* tlss_avx2_memcpy_4(void* dst, const void* src, std::size_t size);
extern "C" void* tlss_avx2_memcpy_5(void* dst, const void* src, std::size_t size);
extern "C" void* tlss_avx2_nt_memcpy(void* dst, const void* src, std::size_t size);
extern "C" void* tlss_avx2_nt_memcpy_prefetch(void* dst, const void* src, std::size_t size);
extern "C" void* tlss_avx2_nt_memcpy_2stream(void* dst, const void* src, std::size_t size);

__attribute__((noinline)) void* tlss_avx2_memcpy_v4(void* dst, const void* src, std::size_t size) {
  return tlss_avx2_memcpy_4(dst, src, size);
}

__attribute__((noinline)) void* tlss_avx2_memcpy_v5(void* dst, const void* src, std::size_t size) {
  return tlss_avx2_memcpy_5(dst, src, size);
}

__attribute__((noinline)) void* tlss_nt_memcpy(void* dst, const void* src, std::size_t size) {
  return tlss_avx2_nt_memcpy(dst, src, size);
}
__attribute__((noinline)) void* tlss_nt_prefetch_memcpy(void* dst, const void* src,
                                                        std::size_t size) {
  return tlss_avx2_nt_memcpy_prefetch(dst, src, size);
}

__attribute__((noinline)) void* tlss_nt_2stream_memcpy(void* dst, const void* src,
                                                       std::size_t size) {
  return tlss_avx2_nt_memcpy_2stream(dst, src, size);
}

std::uint64_t mix64(std::uint64_t value) noexcept {
  value += 0x9e3779b97f4a7c15ULL;

  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;

  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;

  return value ^ (value >> 31);
}

// ==================================================
// Copy Function
// ==================================================

using CopyFunction = void* (*)(void*, const void*, std::size_t);

using Clock = std::chrono::steady_clock;

// ==================================================
// aligned buffer
// ==================================================

class AlignedBuffer {
 public:
  explicit AlignedBuffer(std::size_t size, std::size_t alignment = 64) {
    void* ptr = nullptr;

    if (::posix_memalign(&ptr, alignment, size) != 0) {
      throw std::bad_alloc{};
    }

    data_ = static_cast<std::uint8_t*>(ptr);
  }

  ~AlignedBuffer() {
    std::free(data_);
  }

  AlignedBuffer(const AlignedBuffer&) = delete;
  AlignedBuffer& operator=(const AlignedBuffer&) = delete;

  std::uint8_t* data() {
    return data_;
  }

  const std::uint8_t* data() const {
    return data_;
  }

 private:
  std::uint8_t* data_{nullptr};
};

// ==================================================
// 根据字符串选择 memcpy 实现
// ==================================================

CopyFunction select_copy_function(std::string_view type) {
  if (type == "libc") {
    return libc_memcpy;
  }
  if (type == "avx2") {
    return tlss_avx2_memcpy_v5;
  }

  if (type == "avx2-api") {
    return tlss_avx2_memcpy;
  }

  if (type == "rep") {
    return tlss_rep_memcpy;
  }

  if (type == "auto") {
    return tlss_auto_memcpy;
  }

  if (type == "nt") {
    return tlss_nt_memcpy;
  }

  if (type == "direct-nt") {
    return tlss_direct_nt_memcpy;
  }

  if (type == "ntpf") {
    return tlss_nt_prefetch_memcpy;
  }

  if (type == "nt2") {
    return tlss_nt_2stream_memcpy;
  }

  if (type == "auto-stream") {
    return tlss_auto_streaming_memcpy;
  }
  return nullptr;
}

// ==================================================
// backend 显示名称
//
// 集中维护，避免新增 backend 时在多个 benchmark
// 分支里漏掉名称，导致 nullptr 被写入 std::cout。
// ==================================================

const char* copy_backend_display_name(std::string_view type) {
  if (type == "libc") {
    return "libc";
  }
  if (type == "avx2") {
    return "TLSS V6";
  }
  if (type == "avx2-api") {
    return "TLSS AVX2 API";
  }
  if (type == "rep") {
    return "TLSS REP";
  }
  if (type == "auto") {
    return "TLSS AUTO";
  }
  if (type == "nt") {
    return "TLSS NT";
  }
  if (type == "direct-nt") {
    return "TLSS DIRECT NT";
  }

  if (type == "ntpf") {
    return "TLSS NT+PF";
  }

  if (type == "nt2") {
    return "TLSS NT-2STREAM";
  }

  if (type == "auto-stream") {
    return "TLSS AUTO STREAMING";
  }
  return "UNKNOWN";
}

// ==================================================
// 当前 NT v1 是实验 kernel。
//
// 约束：
//   1. dst 必须 32-byte aligned；
//   2. size 必须是 256B 的整数倍；
//   3. src 可以不对齐，因为 load 使用 vmovdqu。
//
// 后续 NT kernel 支持 alignment peel / tail 后，
// 可以移除这些 benchmark 侧限制。
// ==================================================

bool validate_nt_experimental_request(std::size_t size, std::size_t dst_offset,
                                      const char* size_name = "size") {
  if ((dst_offset & 31U) != 0) {
    std::cerr << "NT experimental kernel requires dst 32-byte aligned\n";
    return false;
  }

  if ((size & 255U) != 0) {
    std::cerr << "NT experimental kernel requires " << size_name
              << " to be a multiple of 256 bytes\n";
    return false;
  }

  return true;
}

// ==================================================
// 普通 benchmark iteration
//
// 完整 benchmark 矩阵使用 256 MiB 左右工作量
// ==================================================

std::size_t get_iterations(std::size_t size) {
  constexpr std::size_t target_bytes = 256ULL * 1024ULL * 1024ULL;

  if (size == 0) {
    return 1;
  }

  std::size_t iterations = target_bytes / size;

  iterations = std::max<std::size_t>(iterations, 20);

  iterations = std::min<std::size_t>(iterations, 2'000'000);

  return iterations;
}

// ==================================================
// perf 模式 iteration
//
// 4 KiB：
// 4 GiB / 4 KiB = 1,048,576
//
// 正好与你之前的测试保持一致。
// ==================================================

std::size_t get_perf_iterations(std::size_t size) {
  constexpr std::size_t target_bytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;

  if (size == 0) {
    return 1;
  }

  std::size_t iterations = target_bytes / size;

  iterations = std::max<std::size_t>(iterations, 100);

  iterations = std::min<std::size_t>(iterations, 2'000'000);

  return iterations;
}

// ==================================================
// 单个 memcpy 实现 correctness test
// ==================================================

bool correctness_test_backend(const char* name, TLSS::MEMORY::CopyBackend backend) {
  constexpr std::size_t buffer_size = 8192;
  constexpr std::size_t kPageSize = 4096;
  constexpr std::uint8_t guard_value = 0xA5;
  AlignedBuffer src_buffer(buffer_size + kPageSize, kPageSize);
  AlignedBuffer dst_buffer(buffer_size + kPageSize, kPageSize);

  auto* src = src_buffer.data();
  auto* dst = dst_buffer.data();

  for (std::size_t i = 0; i < buffer_size + 64; ++i) {
    src[i] = static_cast<std::uint8_t>((i * 17 + 13) & 0xFF);
  }

  for (std::size_t offset = 0; offset < 32; ++offset) {
    for (std::size_t size = 0; size <= 4096; ++size) {
      std::memset(dst, guard_value, buffer_size + 64);

      void* result = TLSS::MEMORY::memcpy(dst + offset, src + offset, size, backend,
                                          TLSS::MEMORY::CopyHint::Default);
      if (result != dst + offset) {
        std::cerr << name << " return value FAILED" << " offset=" << offset << " size=" << size
                  << '\n';

        return false;
      }
      if (std::memcmp(dst + offset, src + offset, size) != 0) {
        std::cerr << name << " correctness FAILED" << " offset=" << offset << " size=" << size
                  << '\n';

        return false;
      }

      for (std::size_t i = 0; i < offset; ++i) {
        if (dst[i] != guard_value) {
          std::cerr << name << " write-before-dst FAILED" << " offset=" << offset
                    << " size=" << size << " corrupted_index=" << i << '\n';

          return false;
        }
      }

      const std::size_t end = offset + size;

      for (std::size_t i = end; i < buffer_size + 64; ++i) {
        if (dst[i] != guard_value) {
          std::cerr << name << " write-after-dst FAILED" << " offset=" << offset << " size=" << size
                    << " corrupted_index=" << i << '\n';

          return false;
        }
      }
    }
  }

  std::cout << std::left << std::setw(12) << name << " : PASS\n";

  return true;
}

// ==================================================
// V6 independent src/dst offset correctness
//
// 直接调用当前 V6 kernel：tlss_avx2_memcpy_v5
//
// 覆盖：
//   size       = 0 .. 4096
//   src_offset = 0 .. 31
//   dst_offset = 0 .. 31
//
// 总 case 数约 419 万。
// ==================================================

bool correctness_test_v6_independent_offsets() {
  constexpr std::size_t kPageSize = 4096;
  constexpr std::size_t max_size = 4096;
  constexpr std::size_t max_offset = 31;
  constexpr std::size_t guard_size = 64;
  constexpr std::uint8_t guard_value = 0xA5;

  //
  // 空间一定要覆盖：
  //
  // offset + max_size + guard
  //
  constexpr std::size_t buffer_size = max_size + max_offset + guard_size;

  AlignedBuffer src_buffer(buffer_size + kPageSize, kPageSize);

  AlignedBuffer dst_buffer(buffer_size + kPageSize, kPageSize);

  auto* src_base = src_buffer.data();
  auto* dst_base = dst_buffer.data();

  //
  // source 初始化。
  //
  for (std::size_t i = 0; i < buffer_size; ++i) {
    src_base[i] = static_cast<std::uint8_t>((i * 17 + 13) & 0xFF);
  }

  for (std::size_t src_offset = 0; src_offset <= max_offset; ++src_offset) {
    for (std::size_t dst_offset = 0; dst_offset <= max_offset; ++dst_offset) {
      for (std::size_t size = 0; size <= max_size; ++size) {
        std::memset(dst_base, guard_value, buffer_size);

        auto* src = src_base + src_offset;
        auto* dst = dst_base + dst_offset;

        //
        // 注意：
        // tlss_avx2_memcpy_v5 当前实际就是 V6。
        //
        void* result = tlss_avx2_memcpy_v5(dst, src, size);

        //
        // 1. memcpy 返回原始 dst
        //
        if (result != dst) {
          std::cerr << "V6 return value FAILED" << " src_offset=" << src_offset
                    << " dst_offset=" << dst_offset << " size=" << size << '\n';

          return false;
        }

        //
        // 2. 内容正确
        //
        if (std::memcmp(dst, src, size) != 0) {
          std::cerr << "V6 correctness FAILED" << " src_offset=" << src_offset
                    << " dst_offset=" << dst_offset << " size=" << size << '\n';

          return false;
        }

        //
        // 3. dst 前面不能被写
        //
        for (std::size_t i = 0; i < dst_offset; ++i) {
          if (dst_base[i] != guard_value) {
            std::cerr << "V6 write-before-dst FAILED" << " src_offset=" << src_offset
                      << " dst_offset=" << dst_offset << " size=" << size
                      << " corrupted_index=" << i << '\n';

            return false;
          }
        }

        //
        // 4. dst 后面不能被写
        //
        const std::size_t end = dst_offset + size;

        for (std::size_t i = end; i < buffer_size; ++i) {
          if (dst_base[i] != guard_value) {
            std::cerr << "V6 write-after-dst FAILED" << " src_offset=" << src_offset
                      << " dst_offset=" << dst_offset << " size=" << size
                      << " corrupted_index=" << i << '\n';

            return false;
          }
        }
      }
    }
  }

  std::cout << std::left << std::setw(12) << "V6 DIRECT" << " : PASS\n";

  return true;
}

// ==================================================
// NT v1 direct correctness
//
// 当前只验证 NT 实验 kernel 已承诺支持的输入域：
//   dst 32B aligned
//   size % 256 == 0
//
// src 故意覆盖若干非对齐 offset，验证 vmovdqu load。
// ==================================================

bool correctness_test_nt_direct() {
  constexpr std::size_t kPageSize = 4096;
  constexpr std::size_t kMaxSize = 8192;
  constexpr std::size_t kGuardSize = 64;
  constexpr std::uint8_t kGuardValue = 0xA5;

  AlignedBuffer src_buffer(kMaxSize + kPageSize, kPageSize);
  AlignedBuffer dst_buffer(kMaxSize + kGuardSize + kPageSize, kPageSize);

  auto* src_base = src_buffer.data();
  auto* dst = dst_buffer.data();

  for (std::size_t i = 0; i < kMaxSize + 64; ++i) {
    src_base[i] = static_cast<std::uint8_t>((i * 17 + 13) & 0xFF);
  }

  const std::size_t sizes[] = {256, 512, 1024, 2048, 4096, 8192};
  const std::size_t src_offsets[] = {0, 1, 15, 31};

  for (std::size_t src_offset : src_offsets) {
    const auto* src = src_base + src_offset;

    for (std::size_t size : sizes) {
      std::memset(dst, kGuardValue, kMaxSize + kGuardSize);

      void* result = tlss_avx2_nt_memcpy(dst, src, size);

      if (result != dst) {
        std::cerr << "NT return value FAILED" << " src_offset=" << src_offset << " size=" << size
                  << '\n';
        return false;
      }

      if (std::memcmp(dst, src, size) != 0) {
        std::cerr << "NT correctness FAILED" << " src_offset=" << src_offset << " size=" << size
                  << '\n';
        return false;
      }

      for (std::size_t i = size; i < kMaxSize + kGuardSize; ++i) {
        if (dst[i] != kGuardValue) {
          std::cerr << "NT write-after-dst FAILED" << " src_offset=" << src_offset
                    << " size=" << size << " corrupted_index=" << i << '\n';
          return false;
        }
      }
    }
  }

  std::cout << std::left << std::setw(12) << "NT DIRECT" << " : PASS\n";
  return true;
}

// ==================================================
// 测试我们的手写版本
// ==================================================

bool correctness_test() {
  if (!correctness_test_backend("AVX2", TLSS::MEMORY::CopyBackend::Avx2)) {
    return false;
  }

  if (!correctness_test_backend("REP MOVSB", TLSS::MEMORY::CopyBackend::RepMovsb)) {
    return false;
  }

  if (!correctness_test_backend("AUTO", TLSS::MEMORY::CopyBackend::Auto)) {
    return false;
  }

  //
  // 当前 V6 的真正独立 correctness。
  //
  if (!correctness_test_v6_independent_offsets()) {
    return false;
  }

  if (!correctness_test_nt_direct()) {
    return false;
  }

  return true;
}

// ==================================================
// benchmark 一个函数
// ==================================================

void benchmark_memcpy(const char* name, CopyFunction copy_function, std::size_t size,
                      std::size_t offset, std::size_t iterations) {
  // 多留 64B 用于 offset
  AlignedBuffer src_buffer(size + 64);
  AlignedBuffer dst_buffer(size + 64);

  auto* src = src_buffer.data() + offset;
  auto* dst = dst_buffer.data() + offset;

  std::cout << "src = " << static_cast<void*>(src) << " dst = " << static_cast<void*>(dst) << '\n';

  std::cout << "src % 4096 = " << (reinterpret_cast<std::uintptr_t>(src) & 4095)
            << " dst % 4096 = " << (reinterpret_cast<std::uintptr_t>(dst) & 4095) << '\n';

  std::cout << "src % 64 = " << (reinterpret_cast<std::uintptr_t>(src) & 63)
            << " dst % 64 = " << (reinterpret_cast<std::uintptr_t>(dst) & 63) << '\n';

  // ==================================================
  // 初始化 source
  // ==================================================

  for (std::size_t i = 0; i < size; ++i) {
    src[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xFF);
  }

  std::memset(dst, 0, size);

  // ==================================================
  // Warm up
  // ==================================================

  for (int i = 0; i < 100; ++i) {
    copy_function(dst, src, size);
  }

  // ==================================================
  // Benchmark
  // ==================================================

  const auto begin = Clock::now();

  for (std::size_t i = 0; i < iterations; ++i) {
    copy_function(dst, src, size);
  }

  const auto end = Clock::now();

  // ==================================================
  // 防止结果完全没有外部用途
  // ==================================================

  if (size != 0) {
    benchmark_sink = static_cast<std::uint8_t>(benchmark_sink ^ dst[size - 1]);
  }

  // ==================================================
  // 时间
  // ==================================================

  const double seconds = std::chrono::duration<double>(end - begin).count();

  // ==================================================
  // Latency
  // ==================================================

  const double ns_per_call = seconds * 1'000'000'000.0 / static_cast<double>(iterations);

  // ==================================================
  // Bandwidth
  // ==================================================

  const double total_bytes = static_cast<double>(size) * static_cast<double>(iterations);

  const double gib = total_bytes / (1024.0 * 1024.0 * 1024.0);

  const double gib_per_second = seconds > 0.0 ? gib / seconds : 0.0;

  // ==================================================
  // Output
  // ==================================================

  std::cout << std::left << std::setw(14) << name

            << "size=" << std::right << std::setw(9) << size << " B"

            << "  offset=" << std::setw(2) << offset

            << "  iterations=" << std::setw(10) << iterations

            << "  " << std::setw(9) << std::fixed << std::setprecision(2) << ns_per_call
            << " ns/call"

            << "  " << std::setw(9) << std::fixed << std::setprecision(2) << gib_per_second
            << " GiB/s"

            << '\n';
}

void benchmark_memcpy(const char* name, CopyFunction copy_function, std::size_t size,
                      std::size_t src_offset, std::size_t dst_offset, std::size_t iterations) {
  constexpr std::size_t kPageSize = 4096;

  AlignedBuffer src_buffer(size + kPageSize, kPageSize);
  AlignedBuffer dst_buffer(size + kPageSize, kPageSize);

  auto* src = src_buffer.data() + src_offset;
  auto* dst = dst_buffer.data() + dst_offset;

  std::cout << "src = " << static_cast<void*>(src) << " dst = " << static_cast<void*>(dst) << '\n';

  std::cout << "src % 4096 = " << (reinterpret_cast<std::uintptr_t>(src) & 4095)
            << " dst % 4096 = " << (reinterpret_cast<std::uintptr_t>(dst) & 4095) << '\n';

  std::cout << "src % 64 = " << (reinterpret_cast<std::uintptr_t>(src) & 63)
            << " dst % 64 = " << (reinterpret_cast<std::uintptr_t>(dst) & 63) << '\n';

  std::cout << "page offset delta = "
            << ((reinterpret_cast<std::uintptr_t>(dst) - reinterpret_cast<std::uintptr_t>(src)) &
                4095)
            << '\n';
  // ==================================================
  // 初始化 source
  // ==================================================

  for (std::size_t i = 0; i < size; ++i) {
    src[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xFF);
  }

  std::memset(dst, 0, size);

  // ==================================================
  // Warm up
  // ==================================================

  for (int i = 0; i < 100; ++i) {
    copy_function(dst, src, size);
  }

  // ==================================================
  // Benchmark
  // ==================================================

  const auto begin = Clock::now();

  for (std::size_t i = 0; i < iterations; ++i) {
    copy_function(dst, src, size);
  }

  const auto end = Clock::now();

  // ==================================================
  // 防止结果完全没有外部用途
  // ==================================================

  if (size != 0) {
    benchmark_sink = static_cast<std::uint8_t>(benchmark_sink ^ dst[size - 1]);
  }

  // ==================================================
  // 时间
  // ==================================================
  const double seconds = std::chrono::duration<double>(end - begin).count();
  // ==================================================
  // Latency
  // ==================================================
  const double ns_per_call = seconds * 1'000'000'000.0 / static_cast<double>(iterations);
  // ==================================================
  // Bandwidth
  // ==================================================
  const double total_bytes = static_cast<double>(size) * static_cast<double>(iterations);
  const double gib = total_bytes / (1024.0 * 1024.0 * 1024.0);
  const double gib_per_second = seconds > 0.0 ? gib / seconds : 0.0;

  // ==================================================
  // Output
  // ==================================================

  std::cout << std::left << std::setw(14) << name

            << "size=" << std::right << std::setw(9) << size << " B" << "  src_off=" << std::setw(4)
            << src_offset << "  dst_off=" << std::setw(4) << dst_offset
            << "  iterations=" << std::setw(10) << iterations << "  " << std::setw(9) << std::fixed
            << std::setprecision(2) << ns_per_call << " ns/call" << "  " << std::setw(9)
            << std::fixed << std::setprecision(2) << gib_per_second << " GiB/s" << '\n';
}

// ==================================================
// perf 单项测试模式
//
// ./benchmark libc 4096 0
// ./benchmark avx2 4096 0
// ./benchmark rep 4096 0
// ==================================================
int run_single_benchmark(std::string_view type, std::size_t size, std::size_t offset) {
  CopyFunction copy_function = select_copy_function(type);

  if (copy_function == nullptr) {
    std::cerr << "Unknown memcpy type: " << type << '\n';

    return 1;
  }

  if (offset >= 64) {
    std::cerr << "offset must be < 64\n";

    return 1;
  }

  if (type == "nt" && !validate_nt_experimental_request(size, offset)) {
    return 1;
  }

  const std::size_t iterations = get_perf_iterations(size);
  const char* display_name = copy_backend_display_name(type);

  benchmark_memcpy(display_name, copy_function, size, offset, iterations);

  return 0;
}

int run_single_benchmark(std::string_view type, std::size_t size, std::size_t src_offset,
                         std::size_t dst_offset) {
  CopyFunction copy_function = select_copy_function(type);

  if (copy_function == nullptr) {
    std::cerr << "Unknown memcpy type: " << type << '\n';
    return 1;
  }

  constexpr std::size_t kPageSize = 4096;

  if (src_offset >= kPageSize || dst_offset >= kPageSize) {
    std::cerr << "src_offset and dst_offset " << "must be < 4096\n";

    return 1;
  }

  if (type == "nt" && !validate_nt_experimental_request(size, dst_offset)) {
    return 1;
  }

  const std::size_t iterations = get_perf_iterations(size);
  const char* display_name = copy_backend_display_name(type);

  benchmark_memcpy(display_name, copy_function, size, src_offset, dst_offset, iterations);

  return 0;
}
// ==================================================
// 完整 benchmark 矩阵
// ==================================================

int run_full_benchmark() {
  std::cout << "Checking correctness...\n";

  if (!correctness_test()) {
    return 1;
  }

  std::cout << "Correctness: ALL PASS\n\n";
  /* const std::size_t sizes[] = { */
  /* 1,        2,         3,         4,         7,          8,          15,   16,       17, */
  /* 31,       32,        33,        63,        64,         65,         127,  128,      129, */
  /* 255,      256,       257,       511,       512,        513,        1024, 2 * 1024, 4 * 1024, */
  /* 8 * 1024, 16 * 1024, 32 * 1024, 64 * 1024, 256 * 1024, 1024 * 1024}; */
  /* const std::size_t sizes[] = {2048, 2304, 2560, 2816, 3072, 3328, 3584, 3840, 4096}; */
  /* const std::size_t sizes[] = {2816, 2880, 2944, 3008, 3072, 3136, 3200, 3264, 3328}; */
  /* const std::size_t sizes[] = {3328, 3392, 3456, 3520, 3584, 3648, 3712, */
  /* 3776, 3840, 3904, 3968, 4032, 4096}; */
  const std::size_t sizes[] = {256,  512,  768,  1024, 1536, 2048, 2560, 3072,
                               3584, 4096, 4608, 5120, 6144, 8192, 16384};

  /* const std::size_t offsets[] = {0, 1, 7, 15, 31}; */
  const std::size_t offsets[] = {0};

  for (std::size_t offset : offsets) {
    std::cout << "========================================\n"
              << "OFFSET = " << offset << '\n'
              << "========================================\n";

    for (std::size_t size : sizes) {
      const std::size_t iterations = get_iterations(size);
      /* benchmark_memcpy("std::memcpy", libc_memcpy, size, offset, iterations); */

      benchmark_memcpy("TLSS AVX2", tlss_avx2_memcpy, size, offset, iterations);

      /* benchmark_memcpy("TLSS REP", tlss_rep_memcpy, size, offset, iterations); */

      /* benchmark_memcpy("TLSS AUTO", tlss_auto_memcpy, size, offset, iterations); */
      std::cout << '\n';
    }
  }

  std::cout << "benchmark sink = " << static_cast<unsigned>(benchmark_sink) << '\n';

  return 0;
}
int run_full_sweep_benchmark() {
  std::cout << "Checking correctness...\n";

  if (!correctness_test()) {
    return 1;
  }

  std::cout << "Correctness: ALL PASS\n\n";

  constexpr std::size_t size = 4096;
  constexpr std::size_t src_offset = 0;
  const std::size_t dst_offsets[] = {0,   64,  128, 192, 256, 320, 384, 448, 512,
                                     576, 640, 704, 768, 832, 896, 960, 1024};

  const std::size_t iterations = get_iterations(size);

  std::cout << "========================================\n"
            << "PAGE OFFSET SWEEP\n"
            << "size       = " << size << '\n'
            << "src_offset = " << src_offset << '\n'
            << "========================================\n";

  /* for (std::size_t dst_offset = 0; dst_offset < 4096; dst_offset += 64) { */
  /*   benchmark_memcpy("TLSS AVX2", tlss_avx2_memcpy, size, src_offset, dst_offset, iterations); */
  /*   std::cout << '\n'; */
  /* } */

  for (std::size_t dst_offset : dst_offsets) {
    benchmark_memcpy("TLSS AVX2", tlss_avx2_memcpy, size, src_offset, dst_offset, iterations);
    std::cout << '\n';
  }

  std::cout << "benchmark sink = " << static_cast<unsigned>(benchmark_sink) << '\n';

  return 0;
}

int run_full_v5_v4_benchmark() {
  std::cout << "Checking correctness...\n";

  if (!correctness_test()) {
    return 1;
  }

  std::cout << "Correctness: ALL PASS\n\n";

  constexpr std::size_t size = 4096;
  constexpr std::size_t src_offset = 0;

  const std::size_t iterations = get_iterations(size);

  std::cout << "========================================\n"
            << "V4 / V5 PAGE OFFSET SWEEP\n"
            << "size       = " << size << '\n'
            << "src_offset = " << src_offset << '\n'
            << "========================================\n";

  for (std::size_t dst_offset = 0; dst_offset <= 1024; dst_offset += 64) {
    std::cout << "\n----------------------------------------\n"
              << "dst_offset = " << dst_offset << '\n'
              << "----------------------------------------\n";

    benchmark_memcpy("V3", tlss_avx2_memcpy, size, src_offset, dst_offset, iterations);
    benchmark_memcpy("V4", tlss_avx2_memcpy_v4, size, src_offset, dst_offset, iterations);
    benchmark_memcpy("V5", tlss_avx2_memcpy_v5, size, src_offset, dst_offset, iterations);

    std::cout << '\n';
  }

  std::cout << "benchmark sink = " << static_cast<unsigned>(benchmark_sink) << '\n';

  return 0;
}

double benchmark_once_ns(CopyFunction copy_function, void* dst, const void* src, std::size_t size,
                         std::size_t iterations) {
  // warm up
  for (int i = 0; i < 100; ++i) {
    copy_function(dst, src, size);
  }

  const auto begin = Clock::now();

  for (std::size_t i = 0; i < iterations; ++i) {
    copy_function(dst, src, size);
  }

  const auto end = Clock::now();

  if (size != 0) {
    auto* dst_bytes = static_cast<std::uint8_t*>(dst);

    benchmark_sink = static_cast<std::uint8_t>(benchmark_sink ^ dst_bytes[size - 1]);
  }

  const double seconds = std::chrono::duration<double>(end - begin).count();

  return seconds * 1'000'000'000.0 / static_cast<double>(iterations);
}

double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());

  return values[values.size() / 2];
}

struct BackendResult {
  double libc_ns{};
  double rep_ns{};
  double v4_ns{};
  double v5_ns{};
};

struct UpperBoundResult {
  double libc_ns{};
  double rep_ns{};
  double v6_ns{};
};

int run_full_once_ns_benchmark() {
  std::cout << "Checking correctness...\n";

  if (!correctness_test()) {
    return 1;
  }

  std::cout << "Correctness: ALL PASS\n\n";

  constexpr std::size_t kPageSize = 4096;

  constexpr std::size_t size = 4096;
  constexpr std::size_t src_offset = 0;

  constexpr std::size_t rounds = 21;

  /* const std::size_t dst_offsets[] = {384, 448}; */
  /* const std::size_t dst_offsets[] = {384, 400, 416, 432, 448}; */
  /* const std::size_t dst_offsets[] = {896, 960, 1024}; */
  const std::size_t dst_offsets[] = {768, 832, 896};

  /* const std::size_t sizes[] = {16,   32,   64,    128,   256,   512,    1024,  2048, */
  /* 4096, 8192, 16384, 32768, 65536, 131072, 262144}; */
  const std::size_t iterations = get_iterations(size);

  std::cout << "========================================\n"
            << "V4 / V5 MEDIAN TEST\n"
            << "size       = " << size << '\n'
            << "rounds     = " << rounds << '\n'
            << "iterations = " << iterations << '\n'
            << "========================================\n";

  for (std::size_t dst_offset : dst_offsets) {
    //
    // 每一个 delta 只申请一次 buffer
    //
    AlignedBuffer src_buffer(size + kPageSize, kPageSize);

    AlignedBuffer dst_buffer(size + kPageSize, kPageSize);

    auto* src = src_buffer.data() + src_offset;

    auto* dst = dst_buffer.data() + dst_offset;

    //
    // 初始化
    //
    for (std::size_t i = 0; i < size; ++i) {
      src[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xFF);
    }

    std::memset(dst, 0, size);

    std::vector<double> v4_results;
    std::vector<double> v5_results;

    v4_results.reserve(rounds);
    v5_results.reserve(rounds);

    std::cout << "\n----------------------------------------\n"
              << "dst_offset = " << dst_offset << '\n'
              << "src = " << static_cast<void*>(src) << " dst = " << static_cast<void*>(dst) << '\n'
              << "page offset delta = "
              << ((reinterpret_cast<std::uintptr_t>(dst) - reinterpret_cast<std::uintptr_t>(src)) &
                  4095)
              << '\n'
              << "----------------------------------------\n";

    //
    // 21轮
    //
    for (std::size_t round = 0; round < rounds; ++round) {
      double v4_ns = 0.0;
      double v5_ns = 0.0;

      //
      // 偶数轮：
      // V4 -> V5
      //
      if ((round & 1) == 0) {
        v4_ns = benchmark_once_ns(tlss_avx2_memcpy_v4, dst, src, size, iterations);

        v5_ns = benchmark_once_ns(tlss_avx2_memcpy_v5, dst, src, size, iterations);
      }

      //
      // 奇数轮：
      // V5 -> V4
      //
      else {
        v5_ns = benchmark_once_ns(tlss_avx2_memcpy_v5, dst, src, size, iterations);

        v4_ns = benchmark_once_ns(tlss_avx2_memcpy_v4, dst, src, size, iterations);
      }

      v4_results.push_back(v4_ns);
      v5_results.push_back(v5_ns);

      std::cout << "round=" << std::setw(2) << round << "  V4=" << std::fixed
                << std::setprecision(2) << v4_ns << " ns" << "  V5=" << v5_ns << " ns\n";
    }

    const double v4_median = median(v4_results);

    const double v5_median = median(v5_results);

    std::cout << "\nMEDIAN\n"
              << "V4 = " << std::fixed << std::setprecision(3) << v4_median << " ns/call\n"

              << "V5 = " << v5_median << " ns/call\n";

    if (v4_median < v5_median) {
      std::cout << "BEST = V4\n";
    } else if (v5_median < v4_median) {
      std::cout << "BEST = V5\n";
    } else {
      std::cout << "BEST = TIE\n";
    }
  }

  std::cout << "\nbenchmark sink = " << static_cast<unsigned>(benchmark_sink) << '\n';

  return 0;
}

BackendResult benchmark_size(std::size_t size, std::size_t iterations, std::size_t rounds) {
  constexpr std::size_t kPageSize = 4096;

  //
  // 多申请一页，保证空间足够。
  //
  AlignedBuffer src_buffer(size + kPageSize, kPageSize);

  AlignedBuffer dst_buffer(size + kPageSize, kPageSize);

  // src_offset = 0
  // dst_offset = 0
  //
  // 两者都 page aligned，
  // 因此也天然是 64B aligned。
  //
  auto* src = src_buffer.data();

  auto* dst = dst_buffer.data();

  //
  // 初始化源数据。
  //
  for (std::size_t i = 0; i < size; ++i) {
    src[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xFF);
  }

  std::memset(dst, 0, size);

  std::vector<double> libc_results;
  std::vector<double> rep_results;
  std::vector<double> v4_results;
  std::vector<double> v5_results;

  libc_results.reserve(rounds);
  rep_results.reserve(rounds);
  v4_results.reserve(rounds);
  v5_results.reserve(rounds);

  //
  // 21轮，轮换执行顺序。
  //
  for (std::size_t round = 0; round < rounds; ++round) {
    double libc_ns = 0.0;
    double rep_ns = 0.0;
    double v4_ns = 0.0;
    double v5_ns = 0.0;

    switch (round % 4) {
      case 0:

        libc_ns = benchmark_once_ns(libc_memcpy, dst, src, size, iterations);

        rep_ns = benchmark_once_ns(tlss_rep_memcpy, dst, src, size, iterations);

        v4_ns = benchmark_once_ns(tlss_avx2_memcpy_v4, dst, src, size, iterations);

        v5_ns = benchmark_once_ns(tlss_avx2_memcpy_v5, dst, src, size, iterations);

        break;

      case 1:

        rep_ns = benchmark_once_ns(tlss_rep_memcpy, dst, src, size, iterations);

        v4_ns = benchmark_once_ns(tlss_avx2_memcpy_v4, dst, src, size, iterations);

        v5_ns = benchmark_once_ns(tlss_avx2_memcpy_v5, dst, src, size, iterations);

        libc_ns = benchmark_once_ns(libc_memcpy, dst, src, size, iterations);

        break;

      case 2:

        v4_ns = benchmark_once_ns(tlss_avx2_memcpy_v4, dst, src, size, iterations);

        v5_ns = benchmark_once_ns(tlss_avx2_memcpy_v5, dst, src, size, iterations);

        libc_ns = benchmark_once_ns(libc_memcpy, dst, src, size, iterations);

        rep_ns = benchmark_once_ns(tlss_rep_memcpy, dst, src, size, iterations);

        break;

      case 3:

        v5_ns = benchmark_once_ns(tlss_avx2_memcpy_v5, dst, src, size, iterations);

        libc_ns = benchmark_once_ns(libc_memcpy, dst, src, size, iterations);

        rep_ns = benchmark_once_ns(tlss_rep_memcpy, dst, src, size, iterations);

        v4_ns = benchmark_once_ns(tlss_avx2_memcpy_v4, dst, src, size, iterations);

        break;
    }

    libc_results.push_back(libc_ns);
    rep_results.push_back(rep_ns);
    v4_results.push_back(v4_ns);
    v5_results.push_back(v5_ns);
  }

  return BackendResult{.libc_ns = median(libc_results),
                       .rep_ns = median(rep_results),
                       .v4_ns = median(v4_results),
                       .v5_ns = median(v5_results)};
}

UpperBoundResult benchmark_upper_bound(std::size_t size, std::size_t iterations,
                                       std::size_t rounds) {
  constexpr std::size_t kPageSize = 4096;

  //
  // 同一个 size 下所有 backend
  // 使用完全相同的 buffer 地址。
  //
  AlignedBuffer src_buffer(size + kPageSize, kPageSize);

  AlignedBuffer dst_buffer(size + kPageSize, kPageSize);

  auto* src = src_buffer.data();
  auto* dst = dst_buffer.data();

  //
  // page aligned:
  //
  // src_offset = 0
  // dst_offset = 0
  //
  for (std::size_t i = 0; i < size; ++i) {
    src[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xFF);
  }

  std::memset(dst, 0, size);

  std::vector<double> libc_results;
  std::vector<double> rep_results;
  std::vector<double> v6_results;

  libc_results.reserve(rounds);
  rep_results.reserve(rounds);
  v6_results.reserve(rounds);

  //
  // 三种 backend 轮换顺序，减少顺序偏差。
  //
  for (std::size_t round = 0; round < rounds; ++round) {
    double libc_ns = 0.0;
    double rep_ns = 0.0;
    double v6_ns = 0.0;

    switch (round % 3) {
      //
      // libc -> REP -> V6
      //
      case 0:

        libc_ns = benchmark_once_ns(libc_memcpy, dst, src, size, iterations);

        rep_ns = benchmark_once_ns(tlss_rep_memcpy, dst, src, size, iterations);

        v6_ns = benchmark_once_ns(tlss_avx2_memcpy_v5, dst, src, size, iterations);

        break;

      //
      // REP -> V6 -> libc
      //
      case 1:

        rep_ns = benchmark_once_ns(tlss_rep_memcpy, dst, src, size, iterations);

        v6_ns = benchmark_once_ns(tlss_avx2_memcpy_v5, dst, src, size, iterations);

        libc_ns = benchmark_once_ns(libc_memcpy, dst, src, size, iterations);

        break;

      //
      // V6 -> libc -> REP
      //
      case 2:

        v6_ns = benchmark_once_ns(tlss_avx2_memcpy_v5, dst, src, size, iterations);

        libc_ns = benchmark_once_ns(libc_memcpy, dst, src, size, iterations);

        rep_ns = benchmark_once_ns(tlss_rep_memcpy, dst, src, size, iterations);

        break;
    }

    libc_results.push_back(libc_ns);
    rep_results.push_back(rep_ns);
    v6_results.push_back(v6_ns);
  }

  return UpperBoundResult{
      .libc_ns = median(libc_results),
      .rep_ns = median(rep_results),
      .v6_ns = median(v6_results),
  };
}
const char* best_upper_backend(const UpperBoundResult& result) {
  double best = result.libc_ns;
  const char* name = "LIBC";

  if (result.rep_ns < best) {
    best = result.rep_ns;
    name = "REP";
  }

  if (result.v6_ns < best) {
    best = result.v6_ns;
    name = "V6";
  }

  return name;
}

int run_v6_upper_bound_benchmark() {
  std::cout << "Checking correctness...\n";

  if (!correctness_test()) {
    return 1;
  }

  std::cout << "Correctness: ALL PASS\n\n";

  constexpr std::size_t rounds = 21;

  const std::size_t sizes[] = {
      4096, 5120, 6144, 7168, 8192, 10240, 12288, 14336, 16384, 20480, 24576, 32768,
  };

  std::cout << "============================================================\n"
            << "PHASE 2 - V6 UPPER BOUND\n"
            << "src offset = 0\n"
            << "dst offset = 0\n"
            << "src/dst    = page aligned\n"
            << "rounds     = " << rounds << '\n'
            << "============================================================\n\n";

  std::cout << std::left << std::setw(12) << "SIZE"

            << std::right << std::setw(14) << "LIBC(ns)"

            << std::setw(14) << "REP(ns)"

            << std::setw(14) << "V6(ns)"

            << std::setw(16) << "V6-LIBC"

            << std::setw(16) << "V6-REP"

            << std::setw(10) << "BEST"

            << '\n';

  std::cout << std::string(96, '-') << '\n';

  for (const std::size_t size : sizes) {
    const std::size_t iterations = get_iterations(size);

    const UpperBoundResult result = benchmark_upper_bound(size, iterations, rounds);

    //
    // < 0:
    // V6 比目标 backend 快
    //
    // > 0:
    // V6 比目标 backend 慢
    //
    const double delta_libc = result.v6_ns - result.libc_ns;

    const double delta_rep = result.v6_ns - result.rep_ns;

    std::cout << std::left << std::setw(12) << size

              << std::right << std::fixed << std::setprecision(3)

              << std::setw(14) << result.libc_ns

              << std::setw(14) << result.rep_ns

              << std::setw(14) << result.v6_ns

              << std::setw(16) << delta_libc

              << std::setw(16) << delta_rep

              << std::setw(10) << best_upper_backend(result)

              << '\n';
  }

  std::cout << "\nbenchmark sink = " << static_cast<unsigned>(benchmark_sink) << '\n';

  return 0;
}

const char* best_backend(const BackendResult& result) {
  double best = result.libc_ns;

  const char* name = "LIBC";

  if (result.rep_ns < best) {
    best = result.rep_ns;
    name = "REP";
  }

  if (result.v4_ns < best) {
    best = result.v4_ns;
    name = "V4";
  }

  if (result.v5_ns < best) {
    best = result.v5_ns;
    name = "V5";
  }

  return name;
}

int run_full_map_benchmark() {
  std::cout << "Checking correctness...\n";

  if (!correctness_test()) {
    return 1;
  }

  std::cout << "Correctness: ALL PASS\n\n";

  constexpr std::size_t rounds = 21;

  /* const std::size_t sizes[] = {16,   32,   64,    128,   256,   512,    1024,  2048, */
  /* 4096, 8192, 16384, 32768, 65536, 131072, 262144}; */
  /* const std::size_t sizes[] = {16384, 18432, 20480, 22528, 24576, 28672, 32768}; */
  /* const std::size_t sizes[] = {8192, 10240, 12288, 14336, 16384}; */
  /* const std::size_t sizes[] = {256, 288, 320, 352, 384, 416, 448, 480, 512}; */
  /* const std::size_t sizes[] = {257, 273, 287,   289,   303,   319,   321,   383,   385,  447, */
  /* 449, 511, 16384, 18432, 20480, 22528, 24576, 28672, 32768}; */
  /* const std::size_t sizes[] = {352, 368, 383, 384, 385, 400, 416, */
  /* 432, 448, 464, 480, 496, 511, 512}; */
  /* const std::size_t sizes[] = {256, 288, 512, 1024, 2048, 4096, 8192, 16384, 32768}; */
  /* const std::size_t sizes[] = {256, 288, 512, 1024, 2048, 4096, 8192, 16384, 32768}; */
  /* const std::size_t sizes[] = {2048, 2304, 2560, 2816, 3072, 3328, 3584, 3840, 4096}; */
  /* const std::size_t sizes[] = {2048, 2112, 2176, 2240, 2304}; */
  /* const std::size_t sizes[] = {2304, 2368, 2432, 2496, 2560, 2624, 2688, 2752, 2816}; */
  /* const std::size_t sizes[] = {2560, 2624, 2688, 2752, 2816, 2880, 2944, 3008, */
  /* 3072, 3136, 3200, 3264, 3328, 3392, 3456, 3520}; */
  const std::size_t sizes[] = {4096, 5120, 6144, 7168, 8192, 10240, 12288, 14336, 16384};

  std::cout << "================================" << "================================\n"

            << "PHASE 2 - SINGLE CORE SIZE MAP\n"

            << "src offset = 0\n"
            << "dst offset = 0\n"
            << "src/dst    = page aligned\n"
            << "rounds     = " << rounds << '\n'

            << "================================" << "================================\n\n";

  std::cout << std::left << std::setw(12) << "SIZE"

            << std::right << std::setw(14) << "LIBC(ns)" << std::setw(14) << "REP(ns)"
            << std::setw(14) << "V4(ns)" << std::setw(14) << "V6(ns)" << std::setw(10) << "BEST"
            << '\n';

  std::cout << std::string(78, '-') << '\n';

  for (const std::size_t size : sizes) {
    const std::size_t iterations = get_iterations(size);

    const BackendResult result = benchmark_size(size, iterations, rounds);

    std::cout << std::left << std::setw(12) << size

              << std::right << std::fixed << std::setprecision(3)

              << std::setw(14) << result.libc_ns

              << std::setw(14) << result.rep_ns

              << std::setw(14) << result.v4_ns

              << std::setw(14) << result.v5_ns

              << std::setw(10) << best_backend(result)

              << '\n';
  }

  std::cout << "\nbenchmark sink = " << static_cast<unsigned>(benchmark_sink) << '\n';

  return 0;
}

struct AlignmentResult {
  std::size_t src_offset{};
  std::size_t dst_offset{};
  std::size_t delta{};

  BackendResult backend;
};

BackendResult benchmark_alignment(std::size_t size, std::size_t src_offset, std::size_t dst_offset,
                                  std::size_t iterations, std::size_t rounds) {
  constexpr std::size_t kPageSize = 4096;

  AlignedBuffer src_buffer(size + kPageSize, kPageSize);

  AlignedBuffer dst_buffer(size + kPageSize, kPageSize);

  auto* src = src_buffer.data() + src_offset;

  auto* dst = dst_buffer.data() + dst_offset;

  for (std::size_t i = 0; i < size; ++i) {
    src[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xFF);
  }

  std::memset(dst, 0, size);

  std::vector<double> libc_results;
  std::vector<double> rep_results;
  std::vector<double> v4_results;
  std::vector<double> v5_results;

  libc_results.reserve(rounds);
  rep_results.reserve(rounds);
  v4_results.reserve(rounds);
  v5_results.reserve(rounds);

  for (std::size_t round = 0; round < rounds; ++round) {
    double libc_ns = 0.0;
    double rep_ns = 0.0;
    double v4_ns = 0.0;
    double v5_ns = 0.0;

    switch (round % 4) {
      case 0:
        libc_ns = benchmark_once_ns(libc_memcpy, dst, src, size, iterations);

        rep_ns = benchmark_once_ns(tlss_rep_memcpy, dst, src, size, iterations);

        v4_ns = benchmark_once_ns(tlss_avx2_memcpy_v4, dst, src, size, iterations);

        v5_ns = benchmark_once_ns(tlss_avx2_memcpy_v5, dst, src, size, iterations);
        break;

      case 1:
        rep_ns = benchmark_once_ns(tlss_rep_memcpy, dst, src, size, iterations);

        v4_ns = benchmark_once_ns(tlss_avx2_memcpy_v4, dst, src, size, iterations);

        v5_ns = benchmark_once_ns(tlss_avx2_memcpy_v5, dst, src, size, iterations);

        libc_ns = benchmark_once_ns(libc_memcpy, dst, src, size, iterations);
        break;

      case 2:
        v4_ns = benchmark_once_ns(tlss_avx2_memcpy_v4, dst, src, size, iterations);

        v5_ns = benchmark_once_ns(tlss_avx2_memcpy_v5, dst, src, size, iterations);

        libc_ns = benchmark_once_ns(libc_memcpy, dst, src, size, iterations);

        rep_ns = benchmark_once_ns(tlss_rep_memcpy, dst, src, size, iterations);
        break;

      case 3:
        v5_ns = benchmark_once_ns(tlss_avx2_memcpy_v5, dst, src, size, iterations);

        libc_ns = benchmark_once_ns(libc_memcpy, dst, src, size, iterations);

        rep_ns = benchmark_once_ns(tlss_rep_memcpy, dst, src, size, iterations);

        v4_ns = benchmark_once_ns(tlss_avx2_memcpy_v4, dst, src, size, iterations);
        break;
    }

    libc_results.push_back(libc_ns);
    rep_results.push_back(rep_ns);
    v4_results.push_back(v4_ns);
    v5_results.push_back(v5_ns);
  }

  return BackendResult{.libc_ns = median(libc_results),
                       .rep_ns = median(rep_results),
                       .v4_ns = median(v4_results),
                       .v5_ns = median(v5_results)};
}

int run_full_alignement_benchmark() {
  std::cout << "Checking correctness...\n";

  if (!correctness_test()) {
    return 1;
  }

  std::cout << "Correctness: ALL PASS\n\n";

  constexpr std::size_t size = 4096;

  constexpr std::size_t rounds = 21;

  const std::size_t iterations = get_iterations(size);

  const std::size_t offsets[] = {0, 16, 32, 48};
  /* const std::size_t sizes[] = {256, 288, 512, 1024, 2048, 4096, 8192, 16384, 32768}; */
  /* const std::size_t offsets[] = {257, 273, 287,   289,   303,   319,   321,   383,   385,  447};
   */

  std::cout << "============================================================\n"
            << "PHASE 2 - ALIGNMENT MAP\n"
            << "size       = " << size << '\n'
            << "rounds     = " << rounds << '\n'
            << "iterations = " << iterations << '\n'
            << "============================================================\n\n";

  std::cout << std::left << std::setw(10) << "SRC_OFF" << std::setw(10) << "DST_OFF"
            << std::setw(10) << "DELTA"

            << std::right << std::setw(14) << "LIBC(ns)" << std::setw(14) << "REP(ns)"
            << std::setw(14) << "V4(ns)" << std::setw(14) << "V5(ns)" << std::setw(10) << "BEST"
            << '\n';

  std::cout << std::string(96, '-') << '\n';

  for (std::size_t src_offset : offsets) {
    for (std::size_t dst_offset : offsets) {
      const BackendResult result =
          benchmark_alignment(size, src_offset, dst_offset, iterations, rounds);

      const std::size_t delta = (dst_offset - src_offset) & 4095;

      std::cout << std::left << std::setw(10) << src_offset

                << std::setw(10) << dst_offset

                << std::setw(10) << delta

                << std::right << std::fixed << std::setprecision(3)

                << std::setw(14) << result.libc_ns

                << std::setw(14) << result.rep_ns

                << std::setw(14) << result.v4_ns

                << std::setw(14) << result.v5_ns

                << std::setw(10) << best_backend(result)

                << '\n';
    }
  }

  std::cout << "\nbenchmark sink = " << static_cast<unsigned>(benchmark_sink) << '\n';

  return 0;
}

struct PairedResult {
  double v4_ns{};
  double v6_ns{};
  double delta_ns{};  // V6 - V4
};

PairedResult benchmark_v4_v6_paired(std::size_t size, std::size_t iterations, std::size_t rounds) {
  constexpr std::size_t kPageSize = 4096;

  // 同一个 size 的 V4/V6 始终使用同一块 src/dst
  AlignedBuffer src_buffer(size + kPageSize, kPageSize);
  AlignedBuffer dst_buffer(size + kPageSize, kPageSize);

  auto* src = src_buffer.data();
  auto* dst = dst_buffer.data();

  // 初始化
  for (std::size_t i = 0; i < size; ++i) {
    src[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xFF);
  }

  std::memset(dst, 0, size);

  std::vector<double> v4_results;
  std::vector<double> v6_results;
  std::vector<double> delta_results;

  v4_results.reserve(rounds);
  v6_results.reserve(rounds);
  delta_results.reserve(rounds);

  for (std::size_t round = 0; round < rounds; ++round) {
    double v4_ns = 0.0;
    double v6_ns = 0.0;

    //
    // 偶数轮：
    // V4 -> V6
    //
    if ((round & 1) == 0) {
      v4_ns = benchmark_once_ns(tlss_avx2_memcpy_v4, dst, src, size, iterations);

      // 现在 tlss_avx2_memcpy_v5 实际就是你的 V6
      v6_ns = benchmark_once_ns(tlss_avx2_memcpy_v5, dst, src, size, iterations);
    }

    //
    // 奇数轮：
    // V6 -> V4
    //
    else {
      v6_ns = benchmark_once_ns(tlss_avx2_memcpy_v5, dst, src, size, iterations);

      v4_ns = benchmark_once_ns(tlss_avx2_memcpy_v4, dst, src, size, iterations);
    }

    v4_results.push_back(v4_ns);
    v6_results.push_back(v6_ns);

    //
    // 最关键：
    // 同一轮的差值
    //
    // > 0 : V4 更快
    // < 0 : V6 更快
    //
    delta_results.push_back(v6_ns - v4_ns);
  }

  return PairedResult{
      .v4_ns = median(v4_results),
      .v6_ns = median(v6_results),
      .delta_ns = median(delta_results),
  };
}

int run_v4_v6_crossover_benchmark() {
  std::cout << "Checking correctness...\n";

  if (!correctness_test()) {
    return 1;
  }

  std::cout << "Correctness: ALL PASS\n\n";

  constexpr std::size_t rounds = 21;

  /* const std::size_t sizes[] = { */
  /* 2048, 2112, 2176, 2240, 2304, 2368, 2432, 2496, 2560, 2624, 2688, 2752, 2816, */
  /* }; */
  const std::size_t sizes[] = {4096, 5120, 6144, 7168, 8192, 10240, 12288, 14336, 16384};

  std::cout << "============================================================\n"
            << "PHASE 2 - V4 / V6 PAIRED CROSSOVER\n"
            << "src offset = 0\n"
            << "dst offset = 0\n"
            << "src/dst    = page aligned\n"
            << "rounds     = " << rounds << '\n'
            << "============================================================\n\n";

  std::cout << std::left << std::setw(12) << "SIZE"

            << std::right << std::setw(14) << "V4(ns)" << std::setw(14) << "V6(ns)" << std::setw(18)
            << "DELTA(V6-V4)" << std::setw(12) << "RESULT" << '\n';

  std::cout << std::string(70, '-') << '\n';

  for (const std::size_t size : sizes) {
    const std::size_t iterations = get_iterations(size);

    const PairedResult result = benchmark_v4_v6_paired(size, iterations, rounds);

    const char* winner = "TIE";

    constexpr double kTieThresholdNs = 0.05;
    if (result.delta_ns > kTieThresholdNs) {
      winner = "V4";
    } else if (result.delta_ns < -kTieThresholdNs) {
      winner = "V6";
    } else {
      winner = "TIE";
    }
    std::cout << std::left << std::setw(12) << size

              << std::right << std::fixed << std::setprecision(3)

              << std::setw(14) << result.v4_ns << std::setw(14) << result.v6_ns << std::setw(18)
              << result.delta_ns << std::setw(12) << winner << '\n';
  }

  std::cout << "\nbenchmark sink = " << static_cast<unsigned>(benchmark_sink) << '\n';

  return 0;
}

struct StreamingResult {
  double libc_gib_s{};
  double rep_gib_s{};
  double v6_gib_s{};
};

double benchmark_streaming_once(CopyFunction copy_function, std::uint8_t* dst,
                                const std::uint8_t* src, std::size_t working_set_size,
                                std::size_t block_size) {
  const auto begin = Clock::now();

  for (std::size_t offset = 0; offset < working_set_size; offset += block_size) {
    copy_function(dst + offset, src + offset, block_size);
  }

  const auto end = Clock::now();

  benchmark_sink = static_cast<std::uint8_t>(benchmark_sink ^ dst[working_set_size - 1]);

  const double seconds = std::chrono::duration<double>(end - begin).count();

  //
  // 这里报告的是 logical copy bandwidth。
  //
  // 例如 memcpy 256 MiB：
  // logical bytes = 256 MiB
  //
  // 注意它不等于真实 DRAM 总线流量，
  // 因为真实流量还可能包含 write allocate 等。
  //
  const double gib = static_cast<double>(working_set_size) / (1024.0 * 1024.0 * 1024.0);

  return gib / seconds;
}

StreamingResult benchmark_streaming(std::size_t working_set_size, std::size_t block_size,
                                    std::size_t rounds) {
  constexpr std::size_t kAlignment = 4096;

  AlignedBuffer src_buffer(working_set_size, kAlignment);

  AlignedBuffer dst_buffer(working_set_size, kAlignment);

  auto* src = src_buffer.data();
  auto* dst = dst_buffer.data();

  //
  // first-touch：
  // 因为整个程序之后会 taskset -c 2，
  // 初始化也在同一个 CPU 上执行。
  //
  for (std::size_t i = 0; i < working_set_size; ++i) {
    src[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xFF);
  }

  //
  // 提前真正分配/触碰 dst 页面，
  // 避免 page fault 混进 benchmark。
  //
  std::memset(dst, 0, working_set_size);

  std::vector<double> libc_results;
  std::vector<double> rep_results;
  std::vector<double> v6_results;

  libc_results.reserve(rounds);
  rep_results.reserve(rounds);
  v6_results.reserve(rounds);

  for (std::size_t round = 0; round < rounds; ++round) {
    double libc_bw = 0.0;
    double rep_bw = 0.0;
    double v6_bw = 0.0;

    //
    // 每一轮轮换 backend 顺序，
    // 避免某一种总是先跑。
    //
    switch (round % 3) {
      case 0:

        libc_bw = benchmark_streaming_once(libc_memcpy, dst, src, working_set_size, block_size);

        rep_bw = benchmark_streaming_once(tlss_rep_memcpy, dst, src, working_set_size, block_size);

        v6_bw =
            benchmark_streaming_once(tlss_avx2_memcpy_v5, dst, src, working_set_size, block_size);

        break;

      case 1:

        rep_bw = benchmark_streaming_once(tlss_rep_memcpy, dst, src, working_set_size, block_size);

        v6_bw =
            benchmark_streaming_once(tlss_avx2_memcpy_v5, dst, src, working_set_size, block_size);

        libc_bw = benchmark_streaming_once(libc_memcpy, dst, src, working_set_size, block_size);

        break;

      case 2:

        v6_bw =
            benchmark_streaming_once(tlss_avx2_memcpy_v5, dst, src, working_set_size, block_size);

        libc_bw = benchmark_streaming_once(libc_memcpy, dst, src, working_set_size, block_size);

        rep_bw = benchmark_streaming_once(tlss_rep_memcpy, dst, src, working_set_size, block_size);

        break;
    }

    libc_results.push_back(libc_bw);
    rep_results.push_back(rep_bw);
    v6_results.push_back(v6_bw);
  }

  return StreamingResult{
      .libc_gib_s = median(libc_results),
      .rep_gib_s = median(rep_results),
      .v6_gib_s = median(v6_results),
  };
}

int run_phase3_streaming_benchmark() {
  std::cout << "Checking correctness...\n";

  if (!correctness_test()) {
    return 1;
  }

  std::cout << "Correctness: ALL PASS\n\n";

  constexpr std::size_t working_set_size = 256ULL * 1024ULL * 1024ULL;

  //
  // streaming 一轮本身已经很重，
  // 不需要再用 21 rounds。
  //
  constexpr std::size_t rounds = 7;

  const std::size_t block_sizes[] = {
      4096, 16 * 1024, 64 * 1024, 256 * 1024, 1024 * 1024,
  };

  std::cout << "============================================================\n"
            << "PHASE 3.1 - STREAMING BASELINE\n"
            << "working set = " << (working_set_size / (1024 * 1024)) << " MiB\n"
            << "rounds      = " << rounds << '\n'
            << "============================================================\n\n";

  std::cout << std::left << std::setw(14) << "BLOCK"

            << std::right << std::setw(16) << "LIBC(GiB/s)"

            << std::setw(16) << "REP(GiB/s)"

            << std::setw(16) << "V6(GiB/s)"

            << std::setw(12) << "BEST"

            << '\n';

  std::cout << std::string(74, '-') << '\n';

  for (const std::size_t block_size : block_sizes) {
    if (working_set_size % block_size != 0) {
      std::cerr << "working set must be divisible " << "by block size\n";

      return 1;
    }

    const StreamingResult result = benchmark_streaming(working_set_size, block_size, rounds);

    const char* winner = "LIBC";
    double best = result.libc_gib_s;

    if (result.rep_gib_s > best) {
      best = result.rep_gib_s;
      winner = "REP";
    }

    if (result.v6_gib_s > best) {
      winner = "V6";
    }

    std::cout << std::left << std::setw(14) << block_size

              << std::right << std::fixed << std::setprecision(2)

              << std::setw(16) << result.libc_gib_s

              << std::setw(16) << result.rep_gib_s

              << std::setw(16) << result.v6_gib_s

              << std::setw(12) << winner

              << '\n';
  }

  std::cout << "\nbenchmark sink = " << static_cast<unsigned>(benchmark_sink) << '\n';

  return 0;
}

int run_phase3_working_set_sweep() {
  std::cout << "Checking correctness...\n";

  if (!correctness_test()) {
    return 1;
  }

  std::cout << "Correctness: ALL PASS\n\n";

  constexpr std::size_t block_size = 64 * 1024;
  constexpr std::size_t rounds = 7;

  const std::size_t working_sets[] = {
      64 * 1024,        256 * 1024,       1024 * 1024,      4 * 1024 * 1024,   8 * 1024 * 1024,
      16 * 1024 * 1024, 32 * 1024 * 1024, 64 * 1024 * 1024, 128 * 1024 * 1024, 256 * 1024 * 1024,
  };

  std::cout << "============================================================\n"
            << "PHASE 3.2 - WORKING SET SWEEP\n"
            << "block size = " << (block_size / 1024) << " KiB\n"
            << "rounds     = " << rounds << '\n'
            << "============================================================\n\n";

  std::cout << std::left << std::setw(14) << "WORKING_SET"

            << std::right << std::setw(16) << "LIBC(GiB/s)" << std::setw(16) << "REP(GiB/s)"
            << std::setw(16) << "V6(GiB/s)" << std::setw(12) << "BEST" << '\n';

  std::cout << std::string(74, '-') << '\n';

  for (const std::size_t working_set_size : working_sets) {
    //
    // 当前 block_size = 64 KiB，
    // working set 必须至少能容纳一个 block。
    //
    if (working_set_size < block_size || working_set_size % block_size != 0) {
      std::cerr << "invalid working set: " << working_set_size << '\n';

      return 1;
    }

    const StreamingResult result = benchmark_streaming(working_set_size, block_size, rounds);

    const char* winner = "LIBC";
    double best = result.libc_gib_s;

    if (result.rep_gib_s > best) {
      best = result.rep_gib_s;
      winner = "REP";
    }

    if (result.v6_gib_s > best) {
      best = result.v6_gib_s;
      winner = "V6";
    }

    //
    // 输出 working set，统一用 KiB / MiB 更容易观察。
    //
    std::string working_set_text;

    if (working_set_size < 1024 * 1024) {
      working_set_text = std::to_string(working_set_size / 1024) + " KiB";
    } else {
      working_set_text = std::to_string(working_set_size / (1024 * 1024)) + " MiB";
    }

    std::cout << std::left << std::setw(14) << working_set_text

              << std::right << std::fixed << std::setprecision(2)

              << std::setw(16) << result.libc_gib_s

              << std::setw(16) << result.rep_gib_s

              << std::setw(16) << result.v6_gib_s

              << std::setw(12) << winner

              << '\n';
  }

  std::cout << "\nbenchmark sink = " << static_cast<unsigned>(benchmark_sink) << '\n';

  return 0;
}

std::size_t get_streaming_passes(std::size_t working_set_size) {
  constexpr std::size_t target_bytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;

  if (working_set_size == 0) {
    return 1;
  }

  return std::max<std::size_t>(1, target_bytes / working_set_size);
}

int run_single_streaming_benchmark(std::string_view type, std::size_t working_set_size,
                                   std::size_t block_size) {
  CopyFunction copy_function = select_copy_function(type);

  if (copy_function == nullptr) {
    std::cerr << "Unknown memcpy type: " << type << '\n';

    return 1;
  }

  if (working_set_size == 0 || block_size == 0) {
    std::cerr << "working_set_size and block_size " << "must be > 0\n";

    return 1;
  }

  if (working_set_size < block_size) {
    std::cerr << "working_set_size must be >= block_size\n";

    return 1;
  }

  if (working_set_size % block_size != 0) {
    std::cerr << "working_set_size must be divisible " << "by block_size\n";

    return 1;
  }

  // NT v1 每次调用都要求 size % 256 == 0。
  // buffer 基址是 4096B aligned；block_size 也是 256B 的整数倍时，
  // 每个 dst + offset 都会保持至少 32B alignment。
  if (type == "nt" && !validate_nt_experimental_request(block_size, 0, "block_size")) {
    return 1;
  }

  if (type == "nt2") {
    if ((block_size & 8191) != 0) {
      std::cerr << "NT 2-stream kernel requires " << "block_size multiple of 8192\n";
      return 1;
    }
  }

  constexpr std::size_t kAlignment = 4096;

  AlignedBuffer src_buffer(working_set_size, kAlignment);

  AlignedBuffer dst_buffer(working_set_size, kAlignment);

  auto* src = src_buffer.data();
  auto* dst = dst_buffer.data();

  //
  // first-touch
  //
  for (std::size_t i = 0; i < working_set_size; ++i) {
    src[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xFF);
  }

  std::memset(dst, 0, working_set_size);

  //
  // 先完整 warm-up 一遍。
  //
  for (std::size_t offset = 0; offset < working_set_size; offset += block_size) {
    copy_function(dst + offset, src + offset, block_size);
  }

  //
  // 正式测试。
  //
  const std::size_t passes = get_streaming_passes(working_set_size);
  const auto begin = Clock::now();

  for (std::size_t pass = 0; pass < passes; ++pass) {
    for (std::size_t offset = 0; offset < working_set_size; offset += block_size) {
      copy_function(dst + offset, src + offset, block_size);
    }
  }

  const auto end = Clock::now();

  benchmark_sink = static_cast<std::uint8_t>(benchmark_sink ^ dst[working_set_size - 1]);

  const double seconds = std::chrono::duration<double>(end - begin).count();
  const double total_bytes = static_cast<double>(working_set_size) * static_cast<double>(passes);
  const double gib = total_bytes / (1024.0 * 1024.0 * 1024.0);
  const double gib_per_second = gib / seconds;
  const char* display_name = copy_backend_display_name(type);

  std::cout << "passes       = " << passes << '\n' << "total copy   = " << gib << " GiB\n";
  std::cout << "Streaming benchmark\n"
            << "backend      = " << display_name << '\n'

            << "working set  = " << working_set_size << " B" << " ("
            << static_cast<double>(working_set_size) / (1024.0 * 1024.0) << " MiB)" << '\n'

            << "block size   = " << block_size << " B" << " ("
            << static_cast<double>(block_size) / 1024.0 << " KiB)" << '\n'

            << "elapsed      = " << std::fixed << std::setprecision(6) << seconds << " s\n"

            << "bandwidth    = " << std::setprecision(2) << gib_per_second << " GiB/s\n";

  return 0;
}

struct StreamingPairedResult {
  double rep_gib_s{};
  double v6_gib_s{};
  double delta_pct{};  // (V6 - REP) / REP * 100
};

StreamingPairedResult benchmark_streaming_paired(std::size_t working_set_size,
                                                 std::size_t block_size, int rounds = 21) {
  constexpr std::size_t kAlignment = 4096;

  AlignedBuffer src_buffer(working_set_size, kAlignment);

  AlignedBuffer dst_buffer(working_set_size, kAlignment);

  auto* src = src_buffer.data();
  auto* dst = dst_buffer.data();

  for (std::size_t i = 0; i < working_set_size; ++i) {
    src[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xff);
  }

  std::memset(dst, 0, working_set_size);

  std::vector<double> rep_results;
  std::vector<double> v6_results;
  std::vector<double> delta_results;

  rep_results.reserve(rounds);
  v6_results.reserve(rounds);
  delta_results.reserve(rounds);

  //
  // warm-up
  //
  benchmark_streaming_once(tlss_rep_memcpy, dst, src, working_set_size, block_size);

  benchmark_streaming_once(tlss_avx2_memcpy_v5, dst, src, working_set_size, block_size);

  for (int round = 0; round < rounds; ++round) {
    double rep{};
    double v6{};

    //
    // 交替执行顺序，抵消 order bias
    //
    if ((round & 1) == 0) {
      v6 = benchmark_streaming_once(tlss_avx2_memcpy_v5, dst, src, working_set_size, block_size);

      rep = benchmark_streaming_once(tlss_rep_memcpy, dst, src, working_set_size, block_size);

    } else {
      rep = benchmark_streaming_once(tlss_rep_memcpy, dst, src, working_set_size, block_size);

      v6 = benchmark_streaming_once(tlss_avx2_memcpy_v5, dst, src, working_set_size, block_size);
    }

    rep_results.push_back(rep);
    v6_results.push_back(v6);

    const double delta_pct = ((v6 - rep) / rep) * 100.0;

    delta_results.push_back(delta_pct);
  }

  benchmark_sink ^= dst[working_set_size - 1];

  return {median(rep_results), median(v6_results), median(delta_results)};
}

void run_streaming_paired_map() {
  constexpr std::size_t working_set_size = 128ULL * 1024ULL * 1024ULL;

  constexpr int rounds = 21;

  const std::vector<std::size_t> sizes{
      512,
      1024,
      2048,
      4096,
  };

  std::cout << "\nStreaming paired benchmark\n"
            << "Working set: 128 MiB\n"
            << "Rounds: " << rounds << "\n\n"

            << std::setw(10) << "SIZE" << std::setw(14) << "REP" << std::setw(14) << "V6"
            << std::setw(14) << "DELTA%" << std::setw(12) << "RESULT" << '\n';

  for (const auto size : sizes) {
    const auto result = benchmark_streaming_paired(working_set_size, size, rounds);

    const char* winner = nullptr;

    //
    // streaming 用 0.5% 作为工程 tie 区间
    //
    if (result.delta_pct > 0.5) {
      winner = "V6";
    } else if (result.delta_pct < -0.5) {
      winner = "REP";
    } else {
      winner = "TIE";
    }

    std::cout << std::setw(10) << size

              << std::setw(14) << std::fixed << std::setprecision(2) << result.rep_gib_s

              << std::setw(14) << result.v6_gib_s

              << std::setw(14) << result.delta_pct

              << std::setw(12) << winner

              << '\n';
  }
}

struct Nt2PairedResult {
  double libc_gib_s{};
  double nt2_gib_s{};
  double delta_pct{};
};

Nt2PairedResult benchmark_nt2_libc_paired(std::size_t working_set_size, std::size_t block_size,
                                          int rounds = 21) {
  constexpr std::size_t kAlignment = 4096;

  AlignedBuffer src_buffer(working_set_size, kAlignment);
  AlignedBuffer dst_buffer(working_set_size, kAlignment);

  auto* src = src_buffer.data();
  auto* dst = dst_buffer.data();

  for (std::size_t i = 0; i < working_set_size; ++i) {
    src[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xff);
  }

  std::memset(dst, 0, working_set_size);

  std::vector<double> libc_results;
  std::vector<double> nt2_results;
  std::vector<double> delta_results;

  libc_results.reserve(rounds);
  nt2_results.reserve(rounds);
  delta_results.reserve(rounds);

  // warm-up
  benchmark_streaming_once(libc_memcpy, dst, src, working_set_size, block_size);

  benchmark_streaming_once(tlss_nt_2stream_memcpy, dst, src, working_set_size, block_size);

  for (int round = 0; round < rounds; ++round) {
    double libc_bw{};
    double nt2_bw{};

    if ((round & 1) == 0) {
      nt2_bw =
          benchmark_streaming_once(tlss_nt_2stream_memcpy, dst, src, working_set_size, block_size);

      libc_bw = benchmark_streaming_once(libc_memcpy, dst, src, working_set_size, block_size);
    } else {
      libc_bw = benchmark_streaming_once(libc_memcpy, dst, src, working_set_size, block_size);

      nt2_bw =
          benchmark_streaming_once(tlss_nt_2stream_memcpy, dst, src, working_set_size, block_size);
    }

    libc_results.push_back(libc_bw);
    nt2_results.push_back(nt2_bw);

    delta_results.push_back(((nt2_bw - libc_bw) / libc_bw) * 100.0);
  }

  return {
      median(libc_results),
      median(nt2_results),
      median(delta_results),
  };
}

void run_nt2_libc_paired() {
  constexpr std::size_t working_set = 128ULL * 1024ULL * 1024ULL;

  constexpr std::size_t block = 32ULL * 1024ULL * 1024ULL;

  constexpr int rounds = 21;

  const auto r = benchmark_nt2_libc_paired(working_set, block, rounds);

  std::cout << "LIBC   = " << r.libc_gib_s << " GiB/s\n"
            << "NT2    = " << r.nt2_gib_s << " GiB/s\n"
            << "DELTA  = " << r.delta_pct << "%\n";
}

struct RepNt2PairedResult {
  double rep_gib_s{};
  double nt2_gib_s{};
  double delta_pct{};  // (NT2 - REP) / REP * 100
};

RepNt2PairedResult benchmark_rep_nt2_paired(std::size_t working_set_size, std::size_t block_size,
                                            int rounds = 21) {
  constexpr std::size_t kAlignment = 4096;

  AlignedBuffer src_buffer(working_set_size, kAlignment);
  AlignedBuffer dst_buffer(working_set_size, kAlignment);

  auto* src = src_buffer.data();
  auto* dst = dst_buffer.data();

  for (std::size_t i = 0; i < working_set_size; ++i) {
    src[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xff);
  }

  std::memset(dst, 0, working_set_size);

  std::vector<double> rep_results;
  std::vector<double> nt2_results;
  std::vector<double> delta_results;

  rep_results.reserve(rounds);
  nt2_results.reserve(rounds);
  delta_results.reserve(rounds);

  // warm-up
  benchmark_streaming_once(tlss_rep_memcpy, dst, src, working_set_size, block_size);

  benchmark_streaming_once(tlss_nt_2stream_memcpy, dst, src, working_set_size, block_size);

  for (int round = 0; round < rounds; ++round) {
    double rep_bw{};
    double nt2_bw{};

    // 偶数轮：NT2 -> REP
    if ((round & 1) == 0) {
      nt2_bw =
          benchmark_streaming_once(tlss_nt_2stream_memcpy, dst, src, working_set_size, block_size);

      rep_bw = benchmark_streaming_once(tlss_rep_memcpy, dst, src, working_set_size, block_size);
    }

    // 奇数轮：REP -> NT2
    else {
      rep_bw = benchmark_streaming_once(tlss_rep_memcpy, dst, src, working_set_size, block_size);

      nt2_bw =
          benchmark_streaming_once(tlss_nt_2stream_memcpy, dst, src, working_set_size, block_size);
    }

    rep_results.push_back(rep_bw);
    nt2_results.push_back(nt2_bw);

    delta_results.push_back(((nt2_bw - rep_bw) / rep_bw) * 100.0);
  }

  return {
      median(rep_results),
      median(nt2_results),
      median(delta_results),
  };
}

void run_rep_nt2_crossover_map() {
  constexpr std::size_t working_set_size = 128ULL * 1024ULL * 1024ULL;

  constexpr int rounds = 21;

  const std::size_t sizes[] = {
      8ULL * 1024,         16ULL * 1024,       32ULL * 1024,       64ULL * 1024,
      128ULL * 1024,       256ULL * 1024,      512ULL * 1024,      1ULL * 1024 * 1024,
      2ULL * 1024 * 1024,  4ULL * 1024 * 1024, 8ULL * 1024 * 1024, 16ULL * 1024 * 1024,
      32ULL * 1024 * 1024,
  };

  std::cout << "\nREP vs NT2 streaming crossover\n"
            << "Working set: 128 MiB\n"
            << "Rounds: " << rounds << "\n\n"

            << std::setw(12) << "SIZE" << std::setw(14) << "REP" << std::setw(14) << "NT2"
            << std::setw(14) << "DELTA%" << std::setw(12) << "RESULT" << '\n';

  for (const auto block_size : sizes) {
    if (working_set_size % block_size != 0) {
      continue;
    }

    // NT2 当前实验 kernel 要求 8 KiB multiple
    if (block_size % 8192 != 0) {
      continue;
    }

    const auto result = benchmark_rep_nt2_paired(working_set_size, block_size, rounds);

    const char* winner = "TIE";

    constexpr double kTieThresholdPct = 0.5;

    if (result.delta_pct > kTieThresholdPct) {
      winner = "NT2";
    } else if (result.delta_pct < -kTieThresholdPct) {
      winner = "REP";
    }

    std::string size_text;

    if (block_size < 1024ULL * 1024ULL) {
      size_text = std::to_string(block_size / 1024) + " KiB";
    } else {
      size_text = std::to_string(block_size / (1024ULL * 1024ULL)) + " MiB";
    }

    std::cout << std::setw(12) << size_text

              << std::setw(14) << std::fixed << std::setprecision(2) << result.rep_gib_s

              << std::setw(14) << result.nt2_gib_s

              << std::setw(14) << result.delta_pct

              << std::setw(12) << winner

              << '\n';
  }
}

struct HotV6Nt2PairedResult {
  double v6_ns{};
  double nt2_ns{};
  double delta_pct{};  // (NT2 - V6) / V6 * 100
};

HotV6Nt2PairedResult benchmark_hot_v6_nt2_paired(std::size_t size, std::size_t iterations,
                                                 int rounds = 21) {
  constexpr std::size_t kAlignment = 4096;

  AlignedBuffer src_buffer(size + kAlignment, kAlignment);
  AlignedBuffer dst_buffer(size + kAlignment, kAlignment);

  auto* src = src_buffer.data();
  auto* dst = dst_buffer.data();

  for (std::size_t i = 0; i < size; ++i) {
    src[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xff);
  }

  std::memset(dst, 0, size);

  std::vector<double> v6_results;
  std::vector<double> nt2_results;
  std::vector<double> delta_results;

  v6_results.reserve(rounds);
  nt2_results.reserve(rounds);
  delta_results.reserve(rounds);

  // warm-up
  benchmark_once_ns(tlss_avx2_memcpy_v5, dst, src, size, 100);

  benchmark_once_ns(tlss_nt_2stream_memcpy, dst, src, size, 100);

  for (int round = 0; round < rounds; ++round) {
    double v6_ns{};
    double nt2_ns{};

    // 偶数轮：V6 -> NT2
    if ((round & 1) == 0) {
      v6_ns = benchmark_once_ns(tlss_avx2_memcpy_v5, dst, src, size, iterations);

      nt2_ns = benchmark_once_ns(tlss_nt_2stream_memcpy, dst, src, size, iterations);
    }

    // 奇数轮：NT2 -> V6
    else {
      nt2_ns = benchmark_once_ns(tlss_nt_2stream_memcpy, dst, src, size, iterations);

      v6_ns = benchmark_once_ns(tlss_avx2_memcpy_v5, dst, src, size, iterations);
    }

    v6_results.push_back(v6_ns);
    nt2_results.push_back(nt2_ns);

    delta_results.push_back(((nt2_ns - v6_ns) / v6_ns) * 100.0);
  }

  return {
      median(v6_results),
      median(nt2_results),
      median(delta_results),
  };
}

void run_hot_v6_nt2_map() {
  constexpr int rounds = 21;

  const std::size_t sizes[] = {
      8ULL * 1024,
      16ULL * 1024,
      32ULL * 1024,
      64ULL * 1024,
  };

  std::cout << "\nV6 vs NT2 hot-cache benchmark\n"
            << "Rounds: " << rounds << "\n\n"

            << std::setw(12) << "SIZE" << std::setw(16) << "V6(ns)" << std::setw(16) << "NT2(ns)"
            << std::setw(14) << "DELTA%" << std::setw(12) << "RESULT" << '\n';

  for (const auto size : sizes) {
    // NT2 当前要求 size 是 8192 的整数倍
    if (size % 8192 != 0) {
      continue;
    }

    const std::size_t iterations = get_iterations(size);

    const auto result = benchmark_hot_v6_nt2_paired(size, iterations, rounds);

    const char* winner = "TIE";

    // hot benchmark 用 0.5% 作为工程 tie 区间
    if (result.delta_pct > 0.5) {
      // NT2 latency 更大
      winner = "V6";
    } else if (result.delta_pct < -0.5) {
      winner = "NT2";
    }

    std::string size_text = std::to_string(size / 1024) + " KiB";

    std::cout << std::setw(12) << size_text

              << std::setw(16) << std::fixed << std::setprecision(2) << result.v6_ns

              << std::setw(16) << result.nt2_ns

              << std::setw(14) << result.delta_pct

              << std::setw(12) << winner

              << '\n';
  }
}

struct AutoV6PairedResult {
  double v6_ns{};
  double auto_ns{};
  double delta_ns{};
  double delta_pct{};
};

AutoV6PairedResult benchmark_auto_v6_paired(std::size_t size, std::size_t iterations,
                                            int rounds = 21) {
  constexpr std::size_t k_alignment = 4096;

  AlignedBuffer src_buffer(size + k_alignment, k_alignment);

  AlignedBuffer dst_buffer(size + k_alignment, k_alignment);

  auto* src = src_buffer.data();
  auto* dst = dst_buffer.data();

  for (std::size_t i = 0; i < size; ++i) {
    src[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xff);
  }

  std::memset(dst, 0, size);

  std::vector<double> v6_results;
  std::vector<double> auto_results;
  std::vector<double> delta_results;
  std::vector<double> delta_pct_results;

  v6_results.reserve(rounds);
  auto_results.reserve(rounds);
  delta_results.reserve(rounds);
  delta_pct_results.reserve(rounds);

  //
  // warm-up（预热）
  //
  benchmark_once_ns(tlss_avx2_memcpy_v5, dst, src, size, 100);

  benchmark_once_ns(tlss_auto_memcpy, dst, src, size, 100);

  for (int round = 0; round < rounds; ++round) {
    double v6_ns{};
    double auto_ns{};

    //
    // 偶数轮：
    // V6 -> AUTO
    //
    if ((round & 1) == 0) {
      v6_ns = benchmark_once_ns(tlss_avx2_memcpy_v5, dst, src, size, iterations);

      auto_ns = benchmark_once_ns(tlss_auto_memcpy, dst, src, size, iterations);
    }

    //
    // 奇数轮：
    // AUTO -> V6
    //
    else {
      auto_ns = benchmark_once_ns(tlss_auto_memcpy, dst, src, size, iterations);

      v6_ns = benchmark_once_ns(tlss_avx2_memcpy_v5, dst, src, size, iterations);
    }

    v6_results.push_back(v6_ns);
    auto_results.push_back(auto_ns);

    const double delta_ns = auto_ns - v6_ns;

    const double delta_pct = (delta_ns / v6_ns) * 100.0;

    delta_results.push_back(delta_ns);
    delta_pct_results.push_back(delta_pct);
  }

  return {
      median(v6_results),
      median(auto_results),
      median(delta_results),
      median(delta_pct_results),
  };
}

void run_auto_v6_paired_map() {
  constexpr int k_rounds = 21;

  /* const std::size_t sizes[] = { */
  /* 1024, */
  /* 2048, */
  /* }; */
  const std::size_t sizes[] = {
      256, 512, 768, 1024, 1280, 1536, 1792, 2048,
  };

  std::cout << "\nAUTO vs V6 paired benchmark\n"
            << "Rounds: " << k_rounds << "\n\n"

            << std::setw(10) << "SIZE" << std::setw(14) << "V6(ns)" << std::setw(14) << "AUTO(ns)"
            << std::setw(14) << "DELTA(ns)" << std::setw(14) << "DELTA%" << '\n';

  for (const auto size : sizes) {
    const std::size_t iterations = get_iterations(size);

    const auto result = benchmark_auto_v6_paired(size, iterations, k_rounds);

    std::cout << std::setw(10) << size

              << std::setw(14) << std::fixed << std::setprecision(3) << result.v6_ns

              << std::setw(14) << result.auto_ns

              << std::setw(14) << result.delta_ns

              << std::setw(14) << result.delta_pct

              << '\n';
  }
}

struct alignas(64) ParallelWorkerResult {
  std::uint8_t sink{};
  std::uint64_t copied_bytes{};
};

bool pin_current_thread_to_cpu(int cpu_id) {
  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);
  CPU_SET(cpu_id, &cpu_set);

  const int result = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set), &cpu_set);
  if (result != 0) {
    std::cerr << "pthread_setaffinity_np failed" << " cpu=" << cpu_id << " error=" << result
              << '\n';

    return false;
  }

  return true;
}

// 这个本来是测试 所有线程都完成相同任务需要多久
// 由于偷懒想在原有基础上增加测试所有核心持续施压时能产生多少 aggregate bandwidth
// 但是逻辑错误了，现在废弃了，调用会出现段错误
void run_parallel_nt2_benchmark(std::size_t thread_count) {
  /* constexpr std::size_t k_thread_count = 1; */
  /* constexpr int k_cpu_ids[] = {0, 2, 4, 6, 8, 10, 12, 14}; */
  /* constexpr int k_cpu_ids[] = {16}; */

  constexpr int k_cpu_ids[] = {// P-core：每个物理核心只取一个逻辑线程
                               0, 2, 4, 6, 8, 10, 12, 14,

                               // E-core
                               16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27};

  if (thread_count == 0 || thread_count > std::size(k_cpu_ids)) {
    throw std::invalid_argument("thread_count must be in [1, 20]");
  }

  //
  // 每个线程自己的 working set（工作集）。
  //
  constexpr std::size_t k_working_set = 64ULL * 1024 * 1024;

  //
  // 每次调用 NT2 复制 32 MiB。
  //
  constexpr std::size_t k_block_size = 32ULL * 1024 * 1024;

  //
  // 每个线程总计：
  //
  // 64 MiB * 64 = 4 GiB
  //
  constexpr std::size_t k_passes = 64;

  constexpr std::size_t k_alignment = 4096;

  //
  // 两个不同的物理 P-core。
  //
  /* constexpr int k_cpu_ids[k_thread_count] = { */
  /* 0, */
  /* 2, */
  /* }; */

  std::atomic<std::size_t> ready_count{0};
  std::atomic<std::size_t> done_count{0};

  std::atomic<bool> start_flag{false};
  std::atomic<bool> stop_flag{false};

  /* ParallelWorkerResult results[k_thread_count]{}; */

  /* std::thread workers[k_thread_count]; */
  std::vector<std::thread> workers;
  std::vector<ParallelWorkerResult> results(thread_count);

  workers.reserve(thread_count);

  for (std::size_t thread_index = 0; thread_index < thread_count; ++thread_index) {
    workers.emplace_back([&, thread_index]() {
      const int cpu_id = k_cpu_ids[thread_index];

      if (!pin_current_thread_to_cpu(cpu_id)) {
        std::terminate();
      }

      //
      // 每个线程独立的 src / dst。
      //
      AlignedBuffer src_buffer(k_working_set, k_alignment);

      AlignedBuffer dst_buffer(k_working_set, k_alignment);

      auto* src = src_buffer.data();

      auto* dst = dst_buffer.data();

      //
      // 初始化内存。
      //
      // 非常重要：
      // 让 page fault（缺页）发生在正式计时之前。
      //
      for (std::size_t i = 0; i < k_working_set; ++i) {
        src[i] = static_cast<std::uint8_t>((i * 31 + thread_index * 17 + 7) & 0xff);
      }

      std::memset(dst, 0, k_working_set);

      //
      // 告诉主线程：
      // 当前 worker 已经准备完成。
      //
      ready_count.fetch_add(1, std::memory_order_release);

      //
      // 等待统一开始。
      //
      while (!start_flag.load(std::memory_order_acquire)) {
        _mm_pause();
      }

      //
      // 正式复制。
      //
      std::uint64_t copied_bytes = 0;

      while (!stop_flag.load(std::memory_order_relaxed)) {
        for (std::size_t offset = 0; offset < k_working_set; offset += k_block_size) {
          tlss_avx2_nt_memcpy_2stream(dst + offset, src + offset, k_block_size);

          copied_bytes += k_block_size;

          if (stop_flag.load(std::memory_order_relaxed)) {
            break;
          }
        }
      }

      results[thread_index].copied_bytes = copied_bytes;

      /* for (std::size_t pass = 0; pass < k_passes; ++pass) { */
      /*   for (std::size_t offset = 0; offset < k_working_set; offset += k_block_size) { */
      /*     tlss_avx2_nt_memcpy_2stream(dst + offset, src + offset, k_block_size); */
      /*   } */
      /* } */

      //
      // 保存一点结果，避免 benchmark 完全没有 observable result。
      //
      /* results[thread_index].sink = dst[k_working_set - 1]; */

      done_count.fetch_add(1, std::memory_order_release);
    });
  }

  //
  // 主线程等待两个 worker 都完成：
  //
  // - 绑核
  // - buffer 分配
  // - page fault
  // - 初始化
  //
  while (ready_count.load(std::memory_order_acquire) != thread_count) {
    _mm_pause();
  }

  //
  // 从这里才开始计时。
  //
  const auto begin = std::chrono::steady_clock::now();

  start_flag.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::seconds(1));
  stop_flag.store(true, std::memory_order_release);

  for (auto& worker : workers) {
    worker.join();
  }
  const auto end = std::chrono::steady_clock::now();
  //
  // 每个线程：
  //
  // 64 MiB * 64 = 4 GiB
  //
  // 两个线程：
  //
  // 8 GiB
  //
  std::uint64_t total_bytes = 0;
  for (const auto& result : results) {
    total_bytes += result.copied_bytes;
  }

  const double elapsed_seconds = std::chrono::duration<double>(end - begin).count();

  const double total_gib =
      static_cast<double>(total_bytes) / static_cast<double>(1024ULL * 1024 * 1024);

  const double bandwidth_gib_s = total_gib / elapsed_seconds;

  std::uint8_t sink = 0;

  for (const auto& result : results) {
    sink ^= result.sink;
  }

  std::cout << "CPUs          = ";

  for (std::size_t i = 0; i < thread_count; ++i) {
    if (i != 0) {
      std::cout << ',';
    }

    std::cout << k_cpu_ids[i];
  }

  std::cout << "\nParallel NT2 benchmark\n"
            << "threads       = " << thread_count << '\n'
            << "working set   = " << k_working_set / (1024 * 1024) << " MiB/thread\n"
            << "block size    = " << k_block_size / (1024 * 1024) << " MiB\n"
            << "copy/thread   = " << (k_working_set * k_passes) / (1024ULL * 1024 * 1024)
            << " GiB\n"
            << "total copy    = " << total_gib << " GiB\n"
            << "elapsed       = " << std::fixed << std::setprecision(6) << elapsed_seconds << " s\n"
            << "bandwidth     = " << std::setprecision(2) << bandwidth_gib_s << " GiB/s\n"

            << "sink          = " << static_cast<unsigned>(sink) << '\n';
}

struct alignas(64) TimedWorkerResult {
  std::uint64_t copied_bytes{};
  std::uint8_t sink{};
};

void run_parallel_nt2_timed_benchmark(std::size_t thread_count, const int* cpu_ids,
                                      std::size_t cpu_count) {
  constexpr std::size_t k_working_set = 64ULL * 1024 * 1024;

  constexpr std::size_t k_block_size = 32ULL * 1024 * 1024;

  constexpr std::size_t k_alignment = 4096;

  //
  // 用 2 秒而不是 1 秒。
  //
  // 原因：
  // 32 MiB/block，在慢一些的 E-core 上，
  // 一个 block 本身就需要几毫秒。
  //
  // 2 秒可以降低 stop tail（停止尾部）占总时间的比例。
  //
  constexpr auto k_measurement_time = std::chrono::seconds(2);

  /* constexpr int k_cpu_ids[] = {0,  2,  4,  6,  8,  10, 12, 14, 16, 17, */
  /* 18, 19, 20, 21, 22, 23, 24, 25, 26, 27}; */

  if (thread_count == 0 || thread_count > cpu_count) {
    throw std::invalid_argument("thread_count must be in [1, 20]");
  }

  std::atomic<std::size_t> ready_count{0};
  std::atomic<std::size_t> done_count{0};

  std::atomic<bool> start_flag{false};
  std::atomic<bool> stop_flag{false};

  std::vector<TimedWorkerResult> results(thread_count);

  std::vector<std::thread> workers;

  workers.reserve(thread_count);

  for (std::size_t thread_index = 0; thread_index < thread_count; ++thread_index) {
    workers.emplace_back([&, thread_index]() {
      const int cpu_id = cpu_ids[thread_index];

      if (!pin_current_thread_to_cpu(cpu_id)) {
        std::terminate();
      }

      AlignedBuffer src_buffer(k_working_set, k_alignment);

      AlignedBuffer dst_buffer(k_working_set, k_alignment);

      auto* src = src_buffer.data();
      auto* dst = dst_buffer.data();

      //
      // 在计时前完成 allocation + page fault。
      //
      for (std::size_t i = 0; i < k_working_set; ++i) {
        src[i] = static_cast<std::uint8_t>((i * 31 + thread_index * 17 + 7) & 0xff);
      }

      std::memset(dst, 0, k_working_set);

      ready_count.fetch_add(1, std::memory_order_release);

      //
      // 等待所有 worker 准备完成。
      //
      while (!start_flag.load(std::memory_order_acquire)) {
        _mm_pause();
      }

      std::uint64_t copied_bytes = 0;

      //
      // 不再规定“每线程复制 4 GiB”。
      //
      // 所有线程一直工作到主线程发出 stop。
      //
      while (!stop_flag.load(std::memory_order_relaxed)) {
        for (std::size_t offset = 0; offset < k_working_set; offset += k_block_size) {
          tlss_avx2_nt_memcpy_2stream(dst + offset, src + offset, k_block_size);

          copied_bytes += k_block_size;

          if (stop_flag.load(std::memory_order_relaxed)) {
            break;
          }
        }
      }

      results[thread_index].copied_bytes = copied_bytes;

      results[thread_index].sink = dst[k_working_set - 1];

      done_count.fetch_add(1, std::memory_order_release);
    });
  }

  //
  // 所有 worker 必须已经：
  //
  // 1. 创建
  // 2. 绑核
  // 3. 分配内存
  // 4. 完成 page fault
  // 5. 初始化 buffer
  //
  while (ready_count.load(std::memory_order_acquire) != thread_count) {
    _mm_pause();
  }

  const auto begin = std::chrono::steady_clock::now();

  //
  // 同时释放全部 worker。
  //
  start_flag.store(true, std::memory_order_release);

  std::this_thread::sleep_for(k_measurement_time);

  //
  // 通知全部 worker 停止。
  //
  stop_flag.store(true, std::memory_order_release);

  //
  // 线程可能正在执行一个 32 MiB block，
  // 所以等待当前 block 完成。
  //
  while (done_count.load(std::memory_order_acquire) != thread_count) {
    _mm_pause();
  }

  const auto end = std::chrono::steady_clock::now();

  for (auto& worker : workers) {
    worker.join();
  }

  std::uint64_t total_bytes = 0;
  std::uint8_t sink = 0;

  for (const auto& result : results) {
    total_bytes += result.copied_bytes;
    sink ^= result.sink;
  }

  const double elapsed_seconds = std::chrono::duration<double>(end - begin).count();

  const double total_gib =
      static_cast<double>(total_bytes) / static_cast<double>(1024ULL * 1024 * 1024);

  const double bandwidth_gib_s = total_gib / elapsed_seconds;

  std::cout << "CPUs          = ";

  for (std::size_t i = 0; i < thread_count; ++i) {
    if (i != 0) {
      std::cout << ',';
    }

    std::cout << cpu_ids[i];
  }

  std::cout << "\nParallel NT2 timed benchmark\n"

            << "threads       = " << thread_count << '\n'

            << "working set   = " << k_working_set / (1024 * 1024) << " MiB/thread\n"

            << "block size    = " << k_block_size / (1024 * 1024) << " MiB\n"

            << "elapsed       = " << std::fixed << std::setprecision(6) << elapsed_seconds << " s\n"

            << "total copy    = " << std::setprecision(2) << total_gib << " GiB\n"

            << "bandwidth     = " << bandwidth_gib_s << " GiB/s\n"

            << "sink          = " << static_cast<unsigned>(sink) << '\n';
}

void run_thread_creation_benchmark(std::size_t thread_count) {
  constexpr std::size_t k_iterations = 10000;

  const auto begin = std::chrono::steady_clock::now();

  for (std::size_t iteration = 0; iteration < k_iterations; ++iteration) {
    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    for (std::size_t thread_index = 0; thread_index < thread_count; ++thread_index) {
      workers.emplace_back([]() { asm volatile("" ::: "memory"); });
    }

    for (auto& worker : workers) {
      worker.join();
    }
  }

  const auto end = std::chrono::steady_clock::now();

  const double elapsed_seconds = std::chrono::duration<double>(end - begin).count();

  const double ns_per_batch = elapsed_seconds * 1e9 / static_cast<double>(k_iterations);

  const double us_per_batch = ns_per_batch / 1000.0;

  const double ns_per_thread = ns_per_batch / static_cast<double>(thread_count);

  std::cout << "Thread creation benchmark\n"
            << "threads/batch = " << thread_count << '\n'
            << "iterations    = " << k_iterations << '\n'
            << "elapsed       = " << std::fixed << std::setprecision(6) << elapsed_seconds << " s\n"
            << "ns/batch      = " << std::setprecision(2) << ns_per_batch << '\n'
            << "us/batch      = " << us_per_batch << '\n'
            << "ns/thread     = " << ns_per_thread << '\n';
}

class PersistentWorker {
 public:
  PersistentWorker() : worker_(&PersistentWorker::worker_loop, this) {}

  ~PersistentWorker() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }

    condition_.notify_one();

    if (worker_.joinable()) {
      worker_.join();
    }
  }

  PersistentWorker(const PersistentWorker&) = delete;

  PersistentWorker& operator=(const PersistentWorker&) = delete;

  void submit() {
    std::uint64_t generation = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);

      task_pending_ = true;
      task_done_ = false;
    }

    condition_.notify_one();
  }

  void wait() {
    std::unique_lock<std::mutex> lock(mutex_);

    done_condition_.wait(lock, [this]() { return task_done_; });
  }

 private:
  void worker_loop() {
    while (true) {
      std::unique_lock<std::mutex> lock(mutex_);

      condition_.wait(lock, [this]() { return task_pending_ || stopping_; });

      if (stopping_) {
        return;
      }

      task_pending_ = false;

      lock.unlock();

      //
      // 现在暂时执行“空任务”。
      //
      asm volatile("" ::: "memory");

      lock.lock();

      task_done_ = true;

      lock.unlock();

      done_condition_.notify_one();
    }
  }

 private:
  std::thread worker_;

  std::mutex mutex_;

  std::condition_variable condition_;
  std::condition_variable done_condition_;

  bool task_pending_{false};
  bool task_done_{false};
  bool stopping_{false};
};

void run_persistent_worker_benchmark() {
  constexpr std::size_t k_iterations = 10000;

  PersistentWorker worker;

  //
  // warmup（预热）
  //
  for (std::size_t i = 0; i < 100; ++i) {
    worker.submit();
    worker.wait();
  }

  const auto begin = std::chrono::steady_clock::now();

  for (std::size_t iteration = 0; iteration < k_iterations; ++iteration) {
    worker.submit();
    worker.wait();
  }

  const auto end = std::chrono::steady_clock::now();

  const double elapsed_seconds = std::chrono::duration<double>(end - begin).count();

  const double ns_per_dispatch = elapsed_seconds * 1e9 / static_cast<double>(k_iterations);

  std::cout << "Persistent worker benchmark\n"
            << "iterations      = " << k_iterations << '\n'
            << "ns/dispatch     = " << ns_per_dispatch << '\n'
            << "us/dispatch     = " << ns_per_dispatch / 1000.0 << '\n';
}

class AtomicSpinWorker {
 public:
  AtomicSpinWorker() : worker_(&AtomicSpinWorker::worker_loop, this) {}

  ~AtomicSpinWorker() {
    stopping_.store(true, std::memory_order_release);

    //
    // generation 改变，确保正在等待的 worker
    // 能观察到状态变化。
    //
    generation_.fetch_add(1, std::memory_order_release);

    if (worker_.joinable()) {
      worker_.join();
    }
  }

  AtomicSpinWorker(const AtomicSpinWorker&) = delete;

  AtomicSpinWorker& operator=(const AtomicSpinWorker&) = delete;

  void submit() {
    const std::uint64_t generation = generation_.fetch_add(1, std::memory_order_release) + 1;

    submitted_generation_ = generation;
  }

  void wait() const {
    const std::uint64_t target = submitted_generation_;

    while (completed_generation_.load(std::memory_order_acquire) < target) {
      _mm_pause();
    }
  }

 private:
  void worker_loop() {
    std::uint64_t observed_generation = 0;

    while (true) {
      std::uint64_t current_generation;

      do {
        if (stopping_.load(std::memory_order_acquire)) {
          return;
        }

        current_generation = generation_.load(std::memory_order_acquire);

        if (current_generation == observed_generation) {
          _mm_pause();
        }

      } while (current_generation == observed_generation);

      if (stopping_.load(std::memory_order_acquire)) {
        return;
      }

      //
      // 当前还是空任务。
      //
      asm volatile("" ::: "memory");

      observed_generation = current_generation;

      completed_generation_.store(observed_generation, std::memory_order_release);
    }
  }

 private:
  std::thread worker_;

  alignas(64) std::atomic<std::uint64_t> generation_{0};

  alignas(64) std::atomic<std::uint64_t> completed_generation_{0};

  //
  // submit/wait 都由主线程调用，
  // 暂时不需要 atomic。
  //
  std::uint64_t submitted_generation_{0};

  alignas(64) std::atomic<bool> stopping_{false};
};

void run_atomic_spin_worker_benchmark() {
  constexpr std::size_t k_iterations = 10000;

  AtomicSpinWorker worker;

  constexpr std::size_t k_warmup_iterations = 1000;

  for (std::size_t i = 0; i < k_warmup_iterations; ++i) {
    worker.submit();
    worker.wait();
  }

  const auto begin = std::chrono::steady_clock::now();

  for (std::size_t iteration = 0; iteration < k_iterations; ++iteration) {
    worker.submit();
    worker.wait();
  }

  const auto end = std::chrono::steady_clock::now();

  const double elapsed_seconds = std::chrono::duration<double>(end - begin).count();

  const double ns_per_dispatch = elapsed_seconds * 1e9 / static_cast<double>(k_iterations);

  std::cout << "Atomic spin worker benchmark\n"
            << "iterations      = " << k_iterations << '\n'
            << "ns/dispatch     = " << ns_per_dispatch << '\n'
            << "us/dispatch     = " << ns_per_dispatch / 1000.0 << '\n';
}

class HybridWorker {
 public:
  HybridWorker() : worker_(&HybridWorker::worker_loop, this) {}
  explicit HybridWorker(int cpu_id) : cpu_id_(cpu_id), worker_(&HybridWorker::worker_loop, this) {}

  ~HybridWorker() {
    {
      std::lock_guard<std::mutex> lock(mutex_);

      stopping_.store(true, std::memory_order_release);
    }

    condition_.notify_one();

    if (worker_.joinable()) {
      worker_.join();
    }
  }

  HybridWorker(const HybridWorker&) = delete;

  HybridWorker& operator=(const HybridWorker&) = delete;

  std::uint64_t sleep_count() const noexcept {
    return sleep_count_.load(std::memory_order_relaxed);
  }

  void submit() {
    std::uint64_t generation = 0;

    {
      std::lock_guard<std::mutex> lock(mutex_);

      generation = generation_.fetch_add(1, std::memory_order_release) + 1;

      submitted_generation_ = generation;
    }

    //
    // 如果 worker（工作线程）已经睡眠，
    // 把它唤醒。
    //
    condition_.notify_one();
  }

  void wait() const {
    const std::uint64_t target_generation = submitted_generation_;

    while (completed_generation_.load(std::memory_order_acquire) < target_generation) {
      _mm_pause();
    }
  }

 private:
  alignas(64) std::atomic<std::uint64_t> sleep_count_{0};
  int cpu_id_;

  void worker_loop() {
    if (!pin_current_thread_to_cpu(cpu_id_)) {
      std::terminate();
    }
    /* constexpr std::size_t k_spin_iterations = 4096; */
    constexpr std::size_t k_spin_iterations = 256;

    std::uint64_t observed_generation = 0;

    while (true) {
      //
      // Phase 1：
      // spin（自旋）一段时间。
      //
      bool task_found = false;

      for (std::size_t spin_index = 0; spin_index < k_spin_iterations; ++spin_index) {
        if (stopping_.load(std::memory_order_acquire)) {
          return;
        }

        const std::uint64_t current_generation = generation_.load(std::memory_order_acquire);

        if (current_generation != observed_generation) {
          task_found = true;
          break;
        }

        _mm_pause();
      }

      //
      // Phase 2：
      // 自旋期间没有任务，则进入 sleep（睡眠）。
      //
      if (!task_found) {
        std::unique_lock<std::mutex> lock(mutex_);

        sleep_count_.fetch_add(1, std::memory_order_relaxed);

        condition_.wait(lock, [this, observed_generation]() {
          return stopping_.load(std::memory_order_acquire) ||
                 generation_.load(std::memory_order_acquire) != observed_generation;
        });
      }

      if (stopping_.load(std::memory_order_acquire)) {
        return;
      }

      const std::uint64_t current_generation = generation_.load(std::memory_order_acquire);

      //
      // 当前仍然执行 empty task（空任务）。
      //
      asm volatile("" ::: "memory");

      observed_generation = current_generation;

      completed_generation_.store(observed_generation, std::memory_order_release);
    }
  }

 private:
  std::thread worker_;

  alignas(64) std::atomic<std::uint64_t> generation_{0};

  alignas(64) std::atomic<std::uint64_t> completed_generation_{0};

  std::uint64_t submitted_generation_{0};

  alignas(64) std::atomic<bool> stopping_{false};

  std::mutex mutex_;

  std::condition_variable condition_;
};

void run_hybrid_worker_hot_benchmark() {
  constexpr std::size_t k_iterations = 10000;

  constexpr std::size_t k_warmup_iterations = 1000;

  HybridWorker worker;

  for (std::size_t iteration = 0; iteration < k_warmup_iterations; ++iteration) {
    worker.submit();
    worker.wait();
  }

  const auto begin = std::chrono::steady_clock::now();

  for (std::size_t iteration = 0; iteration < k_iterations; ++iteration) {
    worker.submit();
    worker.wait();
  }

  const auto end = std::chrono::steady_clock::now();

  const double elapsed_seconds = std::chrono::duration<double>(end - begin).count();

  const double ns_per_dispatch = elapsed_seconds * 1e9 / static_cast<double>(k_iterations);

  std::cout << "Hybrid worker hot benchmark\n"
            << "iterations      = " << k_iterations << '\n'
            << "ns/dispatch     = " << ns_per_dispatch << '\n'
            << "us/dispatch     = " << ns_per_dispatch / 1000.0 << '\n';
}

void run_hybrid_worker_cold_benchmark() {
  constexpr std::size_t k_iterations = 1000;

  HybridWorker worker;

  //
  // 每轮故意等待足够久，
  // 让 worker（工作线程）退出 spin（自旋）
  // 并进入 condition_variable（条件变量）睡眠。
  //
  constexpr auto k_idle_time = std::chrono::milliseconds(1);

  double total_dispatch_ns = 0.0;

  for (std::size_t iteration = 0; iteration < k_iterations; ++iteration) {
    std::this_thread::sleep_for(k_idle_time);

    const auto begin = std::chrono::steady_clock::now();

    worker.submit();
    worker.wait();

    const auto end = std::chrono::steady_clock::now();

    const double dispatch_ns = std::chrono::duration<double>(end - begin).count() * 1e9;

    total_dispatch_ns += dispatch_ns;
  }

  const double ns_per_dispatch = total_dispatch_ns / static_cast<double>(k_iterations);

  std::cout << "Hybrid worker cold benchmark\n"
            << "iterations      = " << k_iterations << '\n'
            << "ns/dispatch     = " << ns_per_dispatch << '\n'
            << "us/dispatch     = " << ns_per_dispatch / 1000.0 << '\n';
}

void run_pause_loop_benchmark() {
  constexpr std::size_t k_spin_iterations = 4096;

  constexpr std::size_t k_rounds = 100000;

  const auto begin = std::chrono::steady_clock::now();

  for (std::size_t round = 0; round < k_rounds; ++round) {
    for (std::size_t spin_index = 0; spin_index < k_spin_iterations; ++spin_index) {
      _mm_pause();
    }
  }

  const auto end = std::chrono::steady_clock::now();

  const double elapsed_seconds = std::chrono::duration<double>(end - begin).count();

  const double ns_per_window = elapsed_seconds * 1e9 / static_cast<double>(k_rounds);

  const double ns_per_pause = ns_per_window / static_cast<double>(k_spin_iterations);

  std::cout << "Pause loop benchmark\n"
            << "spin iterations = " << k_spin_iterations << '\n'
            << "ns/window       = " << ns_per_window << '\n'
            << "us/window       = " << ns_per_window / 1000.0 << '\n'
            << "ns/pause        = " << ns_per_pause << '\n';
}

void busy_wait_for(std::chrono::nanoseconds duration) {
  const auto begin = std::chrono::steady_clock::now();

  while (std::chrono::steady_clock::now() - begin < duration) {
    _mm_pause();
  }
}

void run_hybrid_worker_gap_benchmark(std::chrono::microseconds gap) {
  constexpr std::size_t k_iterations = 1000;

  HybridWorker worker(2);

  double total_dispatch_ns = 0.0;

  for (std::size_t iteration = 0; iteration < k_iterations; ++iteration) {
    busy_wait_for(gap);

    const auto begin = std::chrono::steady_clock::now();

    worker.submit();
    worker.wait();

    const auto end = std::chrono::steady_clock::now();

    total_dispatch_ns += std::chrono::duration<double>(end - begin).count() * 1e9;
  }

  const double ns_per_dispatch = total_dispatch_ns / static_cast<double>(k_iterations);
  const std::uint64_t sleep_count = worker.sleep_count();

  std::cout << "sleep count     = " << sleep_count << '\n'

            << "sleep ratio     = "
            << static_cast<double>(sleep_count) / static_cast<double>(k_iterations) * 100.0
            << " %\n";

  std::cout << "Hybrid worker gap benchmark\n"
            << "gap            = " << gap.count() << " us\n"
            << "iterations     = " << k_iterations << '\n'
            << "ns/dispatch    = " << ns_per_dispatch << '\n'
            << "us/dispatch    = " << ns_per_dispatch / 1000.0 << '\n';
}

struct alignas(64) CopyTask {
  std::uint8_t* dst{};
  const std::uint8_t* src{};
  std::size_t size{};
};

class MultiWorkerPool {
 public:
  MultiWorkerPool(std::size_t worker_count, const int* cpu_ids)
      : worker_count_(worker_count), tasks_(worker_count_) {
    workers_.reserve(worker_count_);
    tasks_.resize(worker_count_);
    for (std::size_t worker_index = 0; worker_index < worker_count_; ++worker_index) {
      workers_.emplace_back([this, worker_index, cpu_id = cpu_ids[worker_index]]() {
        worker_loop(worker_index, cpu_id);
      });
    }
  }

  ~MultiWorkerPool() {
    {
      std::lock_guard<std::mutex> lock(mutex_);

      stopping_.store(true, std::memory_order_release);
    }

    condition_.notify_all();

    for (auto& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  MultiWorkerPool(const MultiWorkerPool&) = delete;

  MultiWorkerPool& operator=(const MultiWorkerPool&) = delete;

  void dispatch_copy(void* dst, const void* src, std::size_t size) {
    constexpr std::size_t k_nt_block_size = 8192;

    if (size % worker_count_ != 0) {
      throw std::invalid_argument("size must be divisible by worker_count");
    }

    const std::size_t chunk_size = size / worker_count_;

    if (chunk_size % k_nt_block_size != 0) {
      throw std::invalid_argument("chunk_size must be divisible by 8192");
    }

    auto* dst_bytes = static_cast<std::uint8_t*>(dst);

    const auto* src_bytes = static_cast<const std::uint8_t*>(src);

    {
      std::lock_guard<std::mutex> lock(mutex_);

      completed_count_.store(0, std::memory_order_relaxed);

      for (std::size_t worker_index = 0; worker_index < worker_count_; ++worker_index) {
        const std::size_t offset = worker_index * chunk_size;

        tasks_[worker_index].dst = dst_bytes + offset;

        tasks_[worker_index].src = src_bytes + offset;

        tasks_[worker_index].size = chunk_size;
      }

      generation_.fetch_add(1, std::memory_order_release);
    }

    condition_.notify_all();
  }

  void dispatch() {
    {
      std::lock_guard<std::mutex> lock(mutex_);

      completed_count_.store(0, std::memory_order_relaxed);

      generation_.fetch_add(1, std::memory_order_release);
    }

    condition_.notify_all();
  }

  void wait() const {
    while (completed_count_.load(std::memory_order_acquire) != worker_count_) {
      _mm_pause();
    }
  }

 private:
  void worker_loop(std::size_t worker_index, int cpu_id) {
    if (!pin_current_thread_to_cpu(cpu_id)) {
      std::terminate();
    }

    constexpr std::size_t k_spin_iterations = 256;

    std::uint64_t observed_generation = 0;

    while (true) {
      bool task_found = false;

      for (std::size_t spin_index = 0; spin_index < k_spin_iterations; ++spin_index) {
        if (stopping_.load(std::memory_order_acquire)) {
          return;
        }

        const std::uint64_t current_generation = generation_.load(std::memory_order_acquire);

        if (current_generation != observed_generation) {
          task_found = true;
          break;
        }

        _mm_pause();
      }

      if (!task_found) {
        std::unique_lock<std::mutex> lock(mutex_);

        condition_.wait(lock, [this, observed_generation]() {
          return stopping_.load(std::memory_order_acquire) ||
                 generation_.load(std::memory_order_acquire) != observed_generation;
        });
      }

      if (stopping_.load(std::memory_order_acquire)) {
        return;
      }

      const std::uint64_t current_generation = generation_.load(std::memory_order_acquire);

      const CopyTask& task = tasks_[worker_index];

      tlss_avx2_nt_memcpy_2stream(task.dst, task.src, task.size);

      observed_generation = current_generation;

      completed_count_.fetch_add(1, std::memory_order_release);
    }
  }

 private:
  std::size_t worker_count_;
  std::vector<CopyTask> tasks_;

  std::vector<std::thread> workers_;

  alignas(64) std::atomic<std::uint64_t> generation_{0};

  alignas(64) mutable std::atomic<std::size_t> completed_count_{0};

  alignas(64) std::atomic<bool> stopping_{false};

  std::mutex mutex_;

  std::condition_variable condition_;
};

void run_multi_worker_pool_benchmark(std::size_t worker_count) {
  constexpr int k_worker_cpu_ids[] = {0, 2, 4, 6, 8, 10, 12, 14};

  constexpr std::size_t k_iterations = 10000;

  constexpr std::size_t k_warmup_iterations = 1000;

  if (worker_count == 0 || worker_count > std::size(k_worker_cpu_ids)) {
    throw std::invalid_argument("worker_count must be in [1, 8]");
  }

  //
  // main thread（主线程）使用 E-core（能效核心），
  // 避免占用 worker（工作线程）的 P-core（性能核心）。
  //
  if (!pin_current_thread_to_cpu(16)) {
    throw std::runtime_error("failed to pin main thread");
  }

  MultiWorkerPool pool(worker_count, k_worker_cpu_ids);

  //
  // warmup（预热）
  //
  for (std::size_t iteration = 0; iteration < k_warmup_iterations; ++iteration) {
    pool.dispatch();
    pool.wait();
  }

  const auto begin = std::chrono::steady_clock::now();

  for (std::size_t iteration = 0; iteration < k_iterations; ++iteration) {
    pool.dispatch();
    pool.wait();
  }

  const auto end = std::chrono::steady_clock::now();

  const double elapsed_seconds = std::chrono::duration<double>(end - begin).count();

  const double ns_per_dispatch = elapsed_seconds * 1e9 / static_cast<double>(k_iterations);

  std::cout << "Multi-worker pool benchmark\n"
            << "workers        = " << worker_count << '\n'
            << "iterations     = " << k_iterations << '\n'
            << "ns/dispatch    = " << ns_per_dispatch << '\n'
            << "us/dispatch    = " << ns_per_dispatch / 1000.0 << '\n';
}

void run_pool_copy_benchmark(std::size_t worker_count) {
  constexpr int k_worker_cpu_ids[] = {0, 2, 4, 6, 8, 10, 12, 14};

  constexpr std::size_t k_size = 64ULL * 1024 * 1024;

  constexpr std::size_t k_alignment = 4096;

  constexpr std::size_t k_iterations = 64;

  if (worker_count == 0 || worker_count > std::size(k_worker_cpu_ids)) {
    throw std::invalid_argument("worker_count must be in [1, 8]");
  }

  //
  // main thread（主线程）放到 E-core（能效核心）。
  //
  if (!pin_current_thread_to_cpu(16)) {
    throw std::runtime_error("failed to pin main thread");
  }

  AlignedBuffer src_buffer(k_size, k_alignment);

  AlignedBuffer dst_buffer(k_size, k_alignment);

  auto* src = src_buffer.data();
  auto* dst = dst_buffer.data();

  for (std::size_t index = 0; index < k_size; ++index) {
    src[index] = static_cast<std::uint8_t>((index * 31 + 7) & 0xff);
  }

  std::memset(dst, 0, k_size);

  MultiWorkerPool pool(worker_count, k_worker_cpu_ids);

  //
  // warmup（预热）。
  //
  for (std::size_t iteration = 0; iteration < 4; ++iteration) {
    pool.dispatch_copy(dst, src, k_size);

    pool.wait();
  }

  const auto begin = std::chrono::steady_clock::now();

  for (std::size_t iteration = 0; iteration < k_iterations; ++iteration) {
    pool.dispatch_copy(dst, src, k_size);

    pool.wait();
  }

  const auto end = std::chrono::steady_clock::now();

  const double elapsed_seconds = std::chrono::duration<double>(end - begin).count();

  const std::uint64_t total_bytes = static_cast<std::uint64_t>(k_size) * k_iterations;

  const double total_gib =
      static_cast<double>(total_bytes) / static_cast<double>(1024ULL * 1024 * 1024);

  const double bandwidth_gib_s = total_gib / elapsed_seconds;

  std::cout << "Pool copy benchmark\n"
            << "workers        = " << worker_count << '\n'
            << "copy size      = 64 MiB\n"
            << "iterations     = " << k_iterations << '\n'
            << "elapsed        = " << elapsed_seconds << " s\n"
            << "bandwidth      = " << bandwidth_gib_s << " GiB/s\n"
            << "sink           = " << static_cast<unsigned>(dst[k_size - 1]) << '\n';

  const bool correctness = std::memcmp(dst, src, k_size) == 0;

  std::cout << "correctness    = " << (correctness ? "PASS" : "FAIL") << '\n';

  if (!correctness) {
    throw std::runtime_error("parallel copy correctness check failed");
  }
}

void run_pool_copy_benchmark(std::size_t worker_count, std::size_t size_kib) {
  constexpr int k_worker_cpu_ids[] = {0, 2, 4, 6, 8, 10, 12, 14};

  constexpr std::size_t k_alignment = 4096;

  constexpr std::size_t k_nt_block_size = 8192;

  //
  // 每一种配置都尽量复制约 4 GiB 总数据，
  // 从而让不同 copy size（复制大小）
  // 的 benchmark（基准测试）具有较接近的统计量。
  //
  /* constexpr std::size_t k_target_bytes = 64ULL * 1024 * 1024 * 1024; */
  constexpr std::size_t k_target_bytes = 2ULL * 1024 * 1024 * 1024;

  constexpr std::size_t k_warmup_iterations = 4;

  if (worker_count == 0 || worker_count > std::size(k_worker_cpu_ids)) {
    throw std::invalid_argument("worker_count must be in [1, 8]");
  }

  if (size_kib == 0) {
    throw std::invalid_argument("size_mib must be greater than 0");
  }

  const std::size_t copy_size = size_kib * 1024ULL;

  //
  // 当前 parallel copy（并行复制）采用等分策略。
  //
  // 因此 total size（总大小）
  // 必须可以整除 worker count（工作线程数量）。
  //
  if (copy_size % worker_count != 0) {
    throw std::invalid_argument("copy_size must be divisible by worker_count");
  }

  const std::size_t chunk_size = copy_size / worker_count;

  //
  // 当前 NT2 kernel（NT2 双流非临时写内核）
  // 要求每个 chunk（分块）
  // 是 8192 字节的整数倍。
  //
  if (chunk_size % k_nt_block_size != 0) {
    throw std::invalid_argument("chunk_size must be divisible by 8192");
  }

  //
  // 让不同 copy size（复制大小）
  // 的总搬运量尽可能保持约 4 GiB。
  //
  const std::size_t iterations = std::max<std::size_t>(1, k_target_bytes / copy_size);

  //
  // main thread（主线程）固定在 CPU16 的
  // E-core（能效核心）上，
  // 避免和 P-core worker（性能核心工作线程）
  // 竞争同一个物理核心。
  //
  if (!pin_current_thread_to_cpu(16)) {
    throw std::runtime_error("failed to pin main thread");
  }

  /* constexpr std::size_t k_working_set = 256ULL * 1024 * 1024; */
  constexpr std::size_t k_working_set = 2ULL * 1024 * 1024 * 1024;

  if (copy_size > k_working_set) {
    throw std::invalid_argument("copy_size must not exceed working_set");
  }

  if (k_working_set % copy_size != 0) {
    throw std::invalid_argument("working_set must be divisible by copy_size");
  }

  const std::size_t slot_count = k_working_set / copy_size;

  AlignedBuffer src_buffer(k_working_set, k_alignment);

  AlignedBuffer dst_buffer(k_working_set, k_alignment);

  auto* src = src_buffer.data();

  auto* dst = dst_buffer.data();

  //
  // 在 timing region（计时区域）
  // 之外初始化内存，
  // 避免 page fault（缺页异常）
  // 污染 benchmark（基准测试）。
  //
  for (std::size_t offset = 0; offset < k_working_set; offset += sizeof(std::uint64_t)) {
    const std::uint64_t value = mix64(static_cast<std::uint64_t>(offset));

    std::memcpy(src + offset, &value, sizeof(value));
  }

  std::memset(dst, 0, k_working_set);

  /* MultiWorkerPool pool(worker_count, k_worker_cpu_ids); */
  TLSS::MEMORY::INTERNAL::ParallelCopyPool pool(worker_count, k_worker_cpu_ids);

  //
  // warmup（预热）。
  //

  constexpr std::size_t k_warmup_size = 8ULL * 1024 * 1024;

  AlignedBuffer warmup_src_buffer(k_warmup_size, k_alignment);

  AlignedBuffer warmup_dst_buffer(k_warmup_size, k_alignment);

  auto* warmup_src = warmup_src_buffer.data();

  auto* warmup_dst = warmup_dst_buffer.data();

  for (std::size_t offset = 0; offset < k_warmup_size; offset += sizeof(std::uint64_t)) {
    const std::uint64_t value = mix64(static_cast<std::uint64_t>(offset));

    std::memcpy(warmup_src + offset, &value, sizeof(value));
  }

  std::memset(warmup_dst, 0, k_warmup_size);

  for (std::size_t iteration = 0; iteration < k_warmup_iterations; ++iteration) {
    /* pool.dispatch_copy(warmup_dst, warmup_src, k_warmup_size); */
    pool.wait();
  }

  perf_enable();
  const auto begin = std::chrono::steady_clock::now();

  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
    const std::size_t slot_index = iteration % slot_count;

    const std::size_t offset = slot_index * copy_size;

    /* pool.dispatch_copy(dst + offset, src + offset, copy_size); */

    pool.wait();
  }

  const auto end = std::chrono::steady_clock::now();
  perf_disable();

  //
  // correctness check（正确性检查）
  //
  // 不放进 timing region（计时区域）。
  //
  const bool correctness = std::memcmp(dst, src, k_working_set) == 0;

  if (!correctness) {
    throw std::runtime_error("parallel copy correctness check failed");
  }

  const double elapsed_seconds = std::chrono::duration<double>(end - begin).count();

  const std::uint64_t total_bytes =
      static_cast<std::uint64_t>(copy_size) * static_cast<std::uint64_t>(iterations);

  const double total_gib =
      static_cast<double>(total_bytes) / static_cast<double>(1024ULL * 1024 * 1024);

  const double bandwidth_gib_s = total_gib / elapsed_seconds;

  std::cout << "Pool copy benchmark\n"

            << "workers        = " << worker_count << '\n'

            << "copy size      = " << size_kib << " KiB\n"

            << "chunk size     = " << chunk_size / 1024ULL << " KiB/worker\n"

            << "iterations     = " << iterations << '\n'

            << "total copy     = " << total_gib << " GiB\n"

            << "elapsed        = " << elapsed_seconds << " s\n"

            << "bandwidth      = " << bandwidth_gib_s << " GiB/s\n"

            << "sink           = " << static_cast<unsigned>(dst[copy_size - 1]) << '\n'

            << "correctness    = PASS\n";
}

void run_memory_read_benchmark() {
  constexpr std::size_t k_working_set = 256ULL * 1024 * 1024;

  constexpr std::size_t k_total_read = 64ULL * 1024 * 1024 * 1024;

  constexpr std::size_t k_alignment = 4096;

  //
  // 固定在一个 P-core（性能核心）上，
  // 减少 scheduler migration（调度器核心迁移）的影响。
  //
  if (!pin_current_thread_to_cpu(0)) {
    throw std::runtime_error("failed to pin main thread");
  }

  AlignedBuffer buffer(k_working_set, k_alignment);

  auto* data = buffer.data();

  //
  // timing region（计时区域）之外初始化，
  // 提前完成 page fault（缺页异常）。
  //
  for (std::size_t index = 0; index < k_working_set; ++index) {
    data[index] = static_cast<std::uint8_t>((index * 31 + 7) & 0xff);
  }

  const std::size_t passes = k_total_read / k_working_set;

  __m256i accumulator = _mm256_setzero_si256();

  const auto begin = std::chrono::steady_clock::now();

  for (std::size_t pass = 0; pass < passes; ++pass) {
    //
    // 每次加载 32 B。
    //
    // 两次 load（加载）正好覆盖一个
    // 64 B cache line（缓存行）。
    //
    for (std::size_t offset = 0; offset < k_working_set; offset += 32) {
      const __m256i value = _mm256_load_si256(reinterpret_cast<const __m256i*>(data + offset));

      accumulator = _mm256_xor_si256(accumulator, value);
    }
  }

  const auto end = std::chrono::steady_clock::now();

  //
  // 防止 compiler optimizer（编译器优化器）
  // 删除整个读取循环。
  //
  alignas(32) std::uint64_t sink[4];

  _mm256_store_si256(reinterpret_cast<__m256i*>(sink), accumulator);

  const std::uint64_t checksum = sink[0] ^ sink[1] ^ sink[2] ^ sink[3];

  const double elapsed_seconds = std::chrono::duration<double>(end - begin).count();

  const double total_gib =
      static_cast<double>(k_total_read) / static_cast<double>(1024ULL * 1024 * 1024);

  const double bandwidth_gib_s = total_gib / elapsed_seconds;

  std::cout << "Memory read benchmark\n"
            << "working set    = " << k_working_set / (1024ULL * 1024) << " MiB\n"
            << "passes         = " << passes << '\n'
            << "total read     = " << total_gib << " GiB\n"
            << "elapsed        = " << elapsed_seconds << " s\n"
            << "bandwidth      = " << bandwidth_gib_s << " GiB/s\n"
            << "checksum       = " << checksum << '\n';
}

void run_direct_nt2_benchmark_kib(std::size_t size_kib) {
  constexpr std::size_t k_alignment = 4096;

  constexpr std::size_t k_nt_block_size = 8192;

  constexpr std::size_t k_working_set = 2ULL * 1024 * 1024 * 1024;

  constexpr std::size_t k_target_bytes = 2ULL * 1024 * 1024 * 1024;

  const std::size_t copy_size = size_kib * 1024ULL;

  if (copy_size == 0) {
    throw std::invalid_argument("copy_size must be greater than zero");
  }

  if (copy_size % k_nt_block_size != 0) {
    throw std::invalid_argument("copy_size must be divisible by 8192");
  }

  if (copy_size > k_working_set) {
    throw std::invalid_argument("copy_size exceeds working set");
  }

  if (k_working_set % copy_size != 0) {
    throw std::invalid_argument("working set must be divisible by copy size");
  }

  if (!pin_current_thread_to_cpu(0)) {
    throw std::runtime_error("failed to pin direct benchmark thread");
  }

  const std::size_t slot_count = k_working_set / copy_size;

  const std::size_t iterations = k_target_bytes / copy_size;

  AlignedBuffer src_buffer(k_working_set, k_alignment);

  AlignedBuffer dst_buffer(k_working_set, k_alignment);

  auto* src = src_buffer.data();

  auto* dst = dst_buffer.data();

  for (std::size_t offset = 0; offset < k_working_set; offset += sizeof(std::uint64_t)) {
    const std::uint64_t value = mix64(static_cast<std::uint64_t>(offset));

    std::memcpy(src + offset, &value, sizeof(value));
  }

  std::memset(dst, 0, k_working_set);

  //
  // 独立 warmup（预热）也可以继续沿用
  // 你刚才建立的方案。
  //

  const auto begin = std::chrono::steady_clock::now();

  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
    const std::size_t slot_index = iteration % slot_count;

    const std::size_t offset = slot_index * copy_size;

    tlss_avx2_nt_memcpy_2stream(dst + offset, src + offset, copy_size);
    /* std::memcpy(dst + offset, src + offset, copy_size); */
  }

  const auto end = std::chrono::steady_clock::now();

  const bool correctness = std::memcmp(dst, src, k_working_set) == 0;

  if (!correctness) {
    throw std::runtime_error("direct NT2 correctness failed");
  }

  const double elapsed_seconds = std::chrono::duration<double>(end - begin).count();

  const std::uint64_t total_bytes =
      static_cast<std::uint64_t>(copy_size) * static_cast<std::uint64_t>(iterations);

  const double total_gib =
      static_cast<double>(total_bytes) / static_cast<double>(1024ULL * 1024 * 1024);

  std::cout << "Direct NT2 benchmark\n"
            << "copy size      = " << size_kib << " KiB\n"
            << "iterations     = " << iterations << '\n'
            << "total copy     = " << total_gib << " GiB\n"
            << "elapsed        = " << elapsed_seconds << " s\n"
            << "bandwidth      = " << total_gib / elapsed_seconds << " GiB/s\n"
            << "correctness    = PASS\n";
}

void run_pool_copy_benchmark(std::size_t worker_count, std::size_t size_kib,
                             std::size_t cpu_offset) {
  constexpr int k_worker_cpu_ids[] = {0, 2, 4, 6, 8, 10, 12, 14};

  constexpr std::size_t k_worker_cpu_count = sizeof(k_worker_cpu_ids) / sizeof(k_worker_cpu_ids[0]);

  constexpr std::size_t k_alignment = 4096;

  constexpr std::size_t k_nt_block_size = 8192;

  //
  // 当前 benchmark（基准测试）
  // 每次正式计时总共复制 2 GiB。
  //
  constexpr std::size_t k_target_bytes = 2ULL * 1024 * 1024 * 1024;

  //
  // 正式 working set（工作集）也是 2 GiB。
  //
  // 当前设计意味着：
  //
  // 每个 slot（槽位）只访问一次，
  // 完整遍历整个 working set。
  //
  constexpr std::size_t k_working_set = 2ULL * 1024 * 1024 * 1024;

  constexpr std::size_t k_warmup_iterations = 4;

  constexpr std::size_t k_warmup_size = 8ULL * 1024 * 1024;

  //
  // ------------------------------------------------------------
  // Parameter validation（参数检查）
  // ------------------------------------------------------------
  //

  if (worker_count == 0) {
    throw std::invalid_argument("worker_count must be greater than zero");
  }

  if (cpu_offset >= k_worker_cpu_count) {
    throw std::invalid_argument("cpu_offset is out of range");
  }

  if (worker_count > k_worker_cpu_count - cpu_offset) {
    throw std::invalid_argument("cpu_offset + worker_count exceeds available CPU count");
  }

  if (size_kib == 0) {
    throw std::invalid_argument("size_kib must be greater than zero");
  }

  const std::size_t copy_size = size_kib * 1024ULL;

  if (copy_size > k_working_set) {
    throw std::invalid_argument("copy_size must not exceed working_set");
  }

  //
  // parallel copy（并行复制）
  // 当前采用 equal partition（等分策略）。
  //
  if (copy_size % worker_count != 0) {
    throw std::invalid_argument("copy_size must be divisible by worker_count");
  }

  const std::size_t chunk_size = copy_size / worker_count;

  //
  // 当前 NT2 kernel（NT2 双流非临时内核）
  // 要求每个 worker chunk（工作线程分块）
  // 至少 8192 B，并且是 8192 B 的整数倍。
  //
  if (chunk_size < k_nt_block_size) {
    throw std::invalid_argument("chunk_size must be at least 8192 bytes");
  }

  if (chunk_size % k_nt_block_size != 0) {
    throw std::invalid_argument("chunk_size must be divisible by 8192");
  }

  if (k_working_set % copy_size != 0) {
    throw std::invalid_argument("working_set must be divisible by copy_size");
  }

  //
  // 当前 benchmark 要求恰好完整遍历一次
  // working set（工作集）。
  //
  static_assert(k_target_bytes == k_working_set,
                "current benchmark expects one full working-set traversal");

  const std::size_t iterations = k_target_bytes / copy_size;

  const std::size_t slot_count = k_working_set / copy_size;

  //
  // ------------------------------------------------------------
  // Main thread affinity（主线程 CPU 亲和性）
  // ------------------------------------------------------------
  //
  // CPU16 是当前 i7-14700K 上用于 benchmark
  // 的 E-core（能效核心）。
  //
  // 避免 main thread（主线程）
  // 与 P-core worker（性能核心工作线程）
  // 竞争同一个物理核心。
  //
  if (!pin_current_thread_to_cpu(16)) {
    throw std::runtime_error("failed to pin main thread");
  }

  //
  // ------------------------------------------------------------
  // Formal working set（正式工作集）
  // ------------------------------------------------------------
  //

  AlignedBuffer src_buffer(k_working_set, k_alignment);

  AlignedBuffer dst_buffer(k_working_set, k_alignment);

  auto* src = src_buffer.data();

  auto* dst = dst_buffer.data();

  //
  // timing region（计时区域）之外完成
  // page allocation / page fault（页面分配/缺页）。
  //
  for (std::size_t offset = 0; offset < k_working_set; offset += sizeof(std::uint64_t)) {
    const std::uint64_t value = mix64(static_cast<std::uint64_t>(offset));

    std::memcpy(src + offset, &value, sizeof(value));
  }

  std::memset(dst, 0, k_working_set);

  //
  // ------------------------------------------------------------
  // Worker pool（工作线程池）
  // ------------------------------------------------------------
  //
  // cpu_offset = 0:
  //
  // 4 workers -> 0,2,4,6
  //
  // cpu_offset = 4:
  //
  // 4 workers -> 8,10,12,14
  //
  TLSS::MEMORY::INTERNAL::ParallelCopyPool pool(worker_count, k_worker_cpu_ids + cpu_offset);

  //
  // ------------------------------------------------------------
  // Warmup（预热）
  // ------------------------------------------------------------
  //
  // 使用独立 buffer（缓冲区），
  // 防止污染正式 2 GiB working set。
  //

  AlignedBuffer warmup_src_buffer(k_warmup_size, k_alignment);

  AlignedBuffer warmup_dst_buffer(k_warmup_size, k_alignment);

  auto* warmup_src = warmup_src_buffer.data();

  auto* warmup_dst = warmup_dst_buffer.data();

  for (std::size_t offset = 0; offset < k_warmup_size; offset += sizeof(std::uint64_t)) {
    const std::uint64_t value = mix64(static_cast<std::uint64_t>(offset));

    std::memcpy(warmup_src + offset, &value, sizeof(value));
  }

  std::memset(warmup_dst, 0, k_warmup_size);

  for (std::size_t iteration = 0; iteration < k_warmup_iterations; ++iteration) {
    /* pool.dispatch_copy(warmup_dst, warmup_src, k_warmup_size); */

    pool.wait();
  }

  //
  // ------------------------------------------------------------
  // Formal timing region（正式计时区域）
  // ------------------------------------------------------------
  //

  perf_enable();

  const auto begin = std::chrono::steady_clock::now();

  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
    const std::size_t slot_index = iteration % slot_count;

    const std::size_t offset = slot_index * copy_size;

    /* pool.dispatch_copy(dst + offset, src + offset, copy_size); */

    pool.wait();
  }

  const auto end = std::chrono::steady_clock::now();

  perf_disable();

  //
  // ------------------------------------------------------------
  // Correctness check（正确性检查）
  // ------------------------------------------------------------
  //

  const bool correctness = std::memcmp(dst, src, k_working_set) == 0;

  if (!correctness) {
    throw std::runtime_error("parallel copy correctness check failed");
  }

  //
  // ------------------------------------------------------------
  // Statistics（统计）
  // ------------------------------------------------------------
  //

  const double elapsed_seconds = std::chrono::duration<double>(end - begin).count();

  const std::uint64_t total_bytes =
      static_cast<std::uint64_t>(copy_size) * static_cast<std::uint64_t>(iterations);

  const double total_gib =
      static_cast<double>(total_bytes) / static_cast<double>(1024ULL * 1024 * 1024);

  const double bandwidth_gib_s = total_gib / elapsed_seconds;

  //
  // ------------------------------------------------------------
  // Output（输出）
  // ------------------------------------------------------------
  //

  std::cout << "Pool copy benchmark\n"

            << "workers        = " << worker_count << '\n'

            << "cpu offset     = " << cpu_offset << '\n'

            << "cpu ids        = ";

  for (std::size_t worker_index = 0; worker_index < worker_count; ++worker_index) {
    if (worker_index != 0) {
      std::cout << ',';
    }

    std::cout << k_worker_cpu_ids[cpu_offset + worker_index];
  }

  std::cout << '\n'

            << "copy size      = " << size_kib << " KiB\n"

            << "chunk size     = " << chunk_size / 1024ULL << " KiB/worker\n"

            << "iterations     = " << iterations << '\n'

            << "total copy     = " << total_gib << " GiB\n"

            << "elapsed        = " << elapsed_seconds << " s\n"

            << "bandwidth      = " << bandwidth_gib_s << " GiB/s\n"

            << "sink           = " << static_cast<unsigned>(dst[copy_size - 1]) << '\n'

            << "correctness    = PASS\n";
}

void run_pool_sync_benchmark(std::size_t worker_count, std::size_t cpu_offset) {
  constexpr int k_worker_cpu_ids[] = {0, 2, 4, 6, 8, 10, 12, 14};

  constexpr std::size_t k_warmup_iterations = 1000;
  constexpr std::size_t k_iterations = 1000000;

  if (!pin_current_thread_to_cpu(16)) {
    throw std::runtime_error("failed to pin main thread");
  }

  double elapsed_seconds = 0.0;

  {
    TLSS::MEMORY::INTERNAL::ParallelCopyPool pool(worker_count, k_worker_cpu_ids + cpu_offset);

    for (std::size_t iteration = 0; iteration < k_warmup_iterations; ++iteration) {
      /* pool.dispatch_empty_for_benchmark(); */
      pool.wait();
    }

    const auto begin = std::chrono::steady_clock::now();

    for (std::size_t iteration = 0; iteration < k_iterations; ++iteration) {
      /* pool.dispatch_empty_for_benchmark(); */
      pool.wait();
    }

    const auto end = std::chrono::steady_clock::now();

    elapsed_seconds = std::chrono::duration<double>(end - begin).count();

    std::cout << "before pool destruction\n" << std::flush;
  }

  std::cout << "after pool destruction\n" << std::flush;

  const double ns_per_operation =
      elapsed_seconds * 1'000'000'000.0 / static_cast<double>(k_iterations);

  std::cout << "Pool synchronization benchmark\n"
            << "workers        = " << worker_count << '\n'
            << "cpu offset     = " << cpu_offset << '\n'
            << "iterations     = " << k_iterations << '\n'
            << "elapsed        = " << elapsed_seconds << " s\n"
            << "ns/dispatch    = " << ns_per_operation << " ns\n";
}

void run_copy_partition_test() {
  using TLSS::MEMORY::INTERNAL::CopyPartition;
  using TLSS::MEMORY::INTERNAL::make_copy_partition;

  constexpr std::size_t k_alignment = 32;
  constexpr std::size_t k_nt_block_size = 8192;
  constexpr std::size_t k_worker_count = 8;

  constexpr std::size_t k_buffer_size = 512ULL * 1024;

  AlignedBuffer buffer(k_buffer_size + k_alignment, k_alignment);

  auto* base = buffer.data();

  constexpr std::size_t k_offsets[] = {0, 1, 13, 31};

  constexpr std::size_t k_sizes[] = {1, 31, 32, 65535, 65536, 65537, 128ULL * 1024, 153731};

  bool all_passed = true;

  for (std::size_t offset : k_offsets) {
    for (std::size_t size : k_sizes) {
      auto* dst = base + offset;

      const CopyPartition partition = make_copy_partition(dst, size, k_worker_count);

      const std::size_t total_size =
          partition.prefix_size + partition.body_size + partition.tail_size;

      bool passed = true;

      //
      // 1. size conservation（大小守恒）
      //
      if (total_size != size) {
        passed = false;
      }

      //
      // 2. 如果存在 parallel body（并行主体），
      // body 的 destination（目标地址）
      // 必须满足 32B alignment（32 字节对齐）。
      //
      if (partition.body_size != 0) {
        const auto body_address = reinterpret_cast<std::uintptr_t>(dst + partition.prefix_size);

        if ((body_address & (k_alignment - 1)) != 0) {
          passed = false;
        }
      }

      //
      // 3. body 必须由完整的
      // 8192B NT blocks（非临时块）组成。
      //
      if (partition.body_size != 0) {
        if (partition.body_size % k_nt_block_size != 0) {
          passed = false;
        }

        //
        // 4. 当前固定 worker pool（工作线程池）
        // 要求每个 worker 至少有一个 block。
        //
        const std::size_t block_count = partition.body_size / k_nt_block_size;

        if (block_count < k_worker_count) {
          passed = false;
        }
      }

      std::cout << "offset=" << offset

                << " size=" << size

                << " prefix=" << partition.prefix_size

                << " body=" << partition.body_size

                << " tail=" << partition.tail_size

                << " result=" << (passed ? "PASS" : "FAIL")

                << '\n';

      if (!passed) {
        all_passed = false;
      }
    }
  }

  if (!all_passed) {
    throw std::runtime_error("copy partition test failed");
  }

  std::cout << "Copy partition test: PASS\n";
}

void run_parallel_nt_correctness_test() {
  using TLSS::MEMORY::INTERNAL::parallel_nt_copy;
  using TLSS::MEMORY::INTERNAL::ParallelCopyPool;

  constexpr int k_worker_cpu_ids[] = {0, 2, 4, 6, 8, 10, 12, 14};

  constexpr std::size_t k_worker_count = 8;

  constexpr std::size_t k_alignment = 4096;

  //
  // guard region（保护区域）
  //
  // 用来检查 memcpy 有没有越界写。
  //
  constexpr std::size_t k_guard_size = 256;

  constexpr std::uint8_t k_guard_value = 0xa5;

  constexpr std::size_t k_max_copy_size = 256ULL * 1024;

  constexpr std::size_t k_buffer_size = k_guard_size + 32 + k_max_copy_size + k_guard_size;

  constexpr std::size_t k_dst_offsets[] = {0, 1, 13, 31};

  constexpr std::size_t k_src_offsets[] = {0, 1, 7, 31};

  constexpr std::size_t k_sizes[] = {1,
                                     31,
                                     32,

                                     65535,
                                     65536,
                                     65537,

                                     131071,
                                     131072,
                                     131073,

                                     153731,

                                     256ULL * 1024};

  //
  // benchmark main thread（基准测试主线程）
  // 仍然固定在 E-core 16。
  //
  if (!pin_current_thread_to_cpu(16)) {
    throw std::runtime_error("failed to pin main thread");
  }

  ParallelCopyPool pool(k_worker_count, k_worker_cpu_ids);

  AlignedBuffer src_buffer(k_buffer_size, k_alignment);

  AlignedBuffer dst_buffer(k_buffer_size, k_alignment);

  auto* src_base = src_buffer.data();

  auto* dst_base = dst_buffer.data();

  bool all_passed = true;

  std::size_t test_count = 0;

  for (std::size_t dst_offset : k_dst_offsets) {
    for (std::size_t src_offset : k_src_offsets) {
      for (std::size_t size : k_sizes) {
        ++test_count;

        //
        // ------------------------------------------------------
        // Reset buffers（重置缓冲区）
        // ------------------------------------------------------
        //

        std::memset(src_base, 0, k_buffer_size);

        std::memset(dst_base, k_guard_value, k_buffer_size);

        //
        // 让真正 memcpy 区域前面留下
        // k_guard_size 字节保护区域。
        //
        auto* src = src_base + k_guard_size + src_offset;

        auto* dst = dst_base + k_guard_size + dst_offset;

        //
        // ------------------------------------------------------
        // Source pattern（源数据模式）
        // ------------------------------------------------------
        //
        // 使用位置相关数据，
        // 避免周期过短的 pattern（模式）
        // 掩盖 offset/chunk bug（偏移/分块错误）。
        //
        for (std::size_t index = 0; index < size; ++index) {
          const std::uint64_t mixed = mix64(static_cast<std::uint64_t>(index) ^
                                            (static_cast<std::uint64_t>(src_offset) << 32));

          src[index] = static_cast<std::uint8_t>(mixed & 0xff);
        }

        //
        // ------------------------------------------------------
        // Execute real path（执行真实路径）
        // ------------------------------------------------------
        //

        /* void* const result = parallel_nt_copy(pool, dst, src, size); */

        bool passed = true;

        //
        // ------------------------------------------------------
        // 1. Return value（返回值）
        // ------------------------------------------------------
        //
        // memcpy 必须返回原始 dst。
        //
        /* if (result != dst) { */
        /* passed = false; */
        /* } */

        //
        // ------------------------------------------------------
        // 2. Payload correctness（有效数据正确性）
        // ------------------------------------------------------
        //

        if (std::memcmp(dst, src, size) != 0) {
          passed = false;
        }

        //
        // ------------------------------------------------------
        // 3. Prefix guard（前保护区）
        // ------------------------------------------------------
        //
        // 检查 dst 前面的数据有没有被错误修改。
        //
        for (std::size_t index = 0; index < k_guard_size; ++index) {
          if (dst[-static_cast<std::ptrdiff_t>(index + 1)] != k_guard_value) {
            passed = false;
            break;
          }
        }

        //
        // ------------------------------------------------------
        // 4. Tail guard（后保护区）
        // ------------------------------------------------------
        //

        for (std::size_t index = 0; index < k_guard_size; ++index) {
          if (dst[size + index] != k_guard_value) {
            passed = false;
            break;
          }
        }

        std::cout << "dst_offset=" << dst_offset

                  << " src_offset=" << src_offset

                  << " size=" << size

                  << " result=" << (passed ? "PASS" : "FAIL") << '\n';

        if (!passed) {
          all_passed = false;
        }
      }
    }
  }

  std::cout << "tests=" << test_count << '\n';

  if (!all_passed) {
    throw std::runtime_error("parallel NT correctness test failed");
  }

  std::cout << "Parallel NT correctness test: PASS\n";
}

void run_parallel_nt_stress_test() {
  using TLSS::MEMORY::INTERNAL::parallel_nt_copy;
  using TLSS::MEMORY::INTERNAL::ParallelCopyPool;

  constexpr int k_worker_cpu_ids[] = {0, 2, 4, 6, 8, 10, 12, 14};
  constexpr std::size_t k_worker_count = 8;
  constexpr std::size_t k_alignment = 4096;
  constexpr std::size_t k_guard_size = 256;
  constexpr std::uint8_t k_guard_value = 0xa5;
  constexpr std::size_t k_max_offset = 63;
  constexpr std::size_t k_max_copy_size = 512ULL * 1024;
  constexpr std::size_t k_test_count = 10000;
  constexpr std::size_t k_buffer_size =
      k_guard_size + k_max_offset + k_max_copy_size + k_guard_size;

  if (!pin_current_thread_to_cpu(16)) {
    throw std::runtime_error("failed to pin main thread");
  }

  ParallelCopyPool pool(k_worker_count, k_worker_cpu_ids);
  const std::vector<std::size_t> active_worker_indices = {0, 1, 2, 3, 4, 5, 6, 7};
  AlignedBuffer src_buffer(k_buffer_size, k_alignment);
  AlignedBuffer dst_buffer(k_buffer_size, k_alignment);
  auto* src_base = src_buffer.data();
  auto* dst_base = dst_buffer.data();

  //
  // 使用固定 seed（随机种子）。
  //
  // 这样一旦出现 FAIL，
  // 下一次可以完全重现。
  //
  std::mt19937_64 random_engine(0x123456789abcdef0ULL);
  std::uniform_int_distribution<std::size_t> offset_distribution(0, k_max_offset);
  std::uniform_int_distribution<std::size_t> size_distribution(1, k_max_copy_size);
  for (std::size_t test_index = 0; test_index < k_test_count; ++test_index) {
    const std::size_t dst_offset = offset_distribution(random_engine);
    const std::size_t src_offset = offset_distribution(random_engine);
    const std::size_t size = size_distribution(random_engine);
    std::memset(src_base, 0, k_buffer_size);
    std::memset(dst_base, k_guard_value, k_buffer_size);
    auto* src = src_base + k_guard_size + src_offset;
    auto* dst = dst_base + k_guard_size + dst_offset;
    //
    // position-dependent pattern
    // （位置相关数据模式）
    //
    for (std::size_t index = 0; index < size; ++index) {
      const std::uint64_t mixed =
          mix64(static_cast<std::uint64_t>(index) ^ (static_cast<std::uint64_t>(src_offset) << 32) ^
                (static_cast<std::uint64_t>(test_index) << 16));

      src[index] = static_cast<std::uint8_t>(mixed & 0xff);
    }

    void* const result = parallel_nt_copy(pool, active_worker_indices, dst, src, size);

    bool passed = true;

    if (result != dst) {
      passed = false;
    }

    if (std::memcmp(dst, src, size) != 0) {
      passed = false;
    }

    //
    // Front guard（前保护区）
    //
    for (std::size_t index = 0; index < k_guard_size; ++index) {
      if (dst[-static_cast<std::ptrdiff_t>(index + 1)] != k_guard_value) {
        passed = false;
        break;
      }
    }

    //
    // Back guard（后保护区）
    //
    for (std::size_t index = 0; index < k_guard_size; ++index) {
      if (dst[size + index] != k_guard_value) {
        passed = false;
        break;
      }
    }

    if (!passed) {
      std::cerr << "FAIL" << " test=" << test_index << " dst_offset=" << dst_offset
                << " src_offset=" << src_offset << " size=" << size << '\n';

      throw std::runtime_error("parallel NT stress test failed");
    }
  }

  std::cout << "Parallel NT stress test: PASS\n"
            << "tests=" << k_test_count << '\n';
}

void run_parallel_nt_alignment_benchmark(std::size_t size_kib, std::size_t dst_offset) {
  using TLSS::MEMORY::INTERNAL::parallel_nt_copy;
  using TLSS::MEMORY::INTERNAL::ParallelCopyPool;

  constexpr int k_worker_cpu_ids[] = {0, 2, 4, 6, 8, 10, 12, 14};

  constexpr std::size_t k_worker_count = 8;

  constexpr std::size_t k_alignment = 4096;

  constexpr std::size_t k_working_set = 2ULL * 1024 * 1024 * 1024;

  if (size_kib == 0) {
    throw std::invalid_argument("size_kib must be greater than zero");
  }

  if (dst_offset >= 32) {
    throw std::invalid_argument("dst_offset must be in [0, 31]");
  }

  const std::size_t copy_size = size_kib * 1024ULL;

  //
  // 当前测试尺寸使用 KiB，
  // 所以 copy_size 是 1024 的倍数，
  // 每个 slot（槽位）拥有相同的 dst alignment
  // （目标地址对齐状态）。
  //
  if (k_working_set % copy_size != 0) {
    throw std::invalid_argument("working_set must be divisible by copy_size");
  }

  if (!pin_current_thread_to_cpu(16)) {
    throw std::runtime_error("failed to pin main thread");
  }

  const std::size_t slot_count = k_working_set / copy_size;

  const std::size_t buffer_size = k_working_set + 64;

  AlignedBuffer src_buffer(buffer_size, k_alignment);

  AlignedBuffer dst_buffer(buffer_size, k_alignment);

  auto* src_base = src_buffer.data();

  auto* dst_base = dst_buffer.data();

  //
  // 本轮先固定 src aligned（源地址对齐），
  // 只研究 dst offset（目标地址偏移）。
  //
  auto* src = src_base;

  auto* dst = dst_base + dst_offset;

  //
  // 初始化放在 timing region（计时区域）之外。
  //
  for (std::size_t offset = 0; offset < k_working_set; offset += sizeof(std::uint64_t)) {
    const std::uint64_t value = mix64(static_cast<std::uint64_t>(offset));

    std::memcpy(src + offset, &value, sizeof(value));
  }

  std::memset(dst_base, 0, buffer_size);

  ParallelCopyPool pool(k_worker_count, k_worker_cpu_ids);

  //
  // warmup（预热）
  //
  constexpr std::size_t k_warmup_size = 8ULL * 1024 * 1024;

  for (std::size_t iteration = 0; iteration < 4; ++iteration) {
    /* parallel_nt_copy(pool, dst, src, k_warmup_size); */
  }

  const auto begin = std::chrono::steady_clock::now();

  for (std::size_t slot_index = 0; slot_index < slot_count; ++slot_index) {
    const std::size_t offset = slot_index * copy_size;

    /* parallel_nt_copy(pool, dst + offset, src + offset, copy_size); */
  }

  const auto end = std::chrono::steady_clock::now();

  const bool correctness = std::memcmp(dst, src, k_working_set) == 0;

  if (!correctness) {
    throw std::runtime_error("parallel NT alignment benchmark correctness failed");
  }

  const double elapsed_seconds = std::chrono::duration<double>(end - begin).count();

  const double total_gib =
      static_cast<double>(k_working_set) / static_cast<double>(1024ULL * 1024 * 1024);

  const double bandwidth_gib_s = total_gib / elapsed_seconds;

  std::cout << "Parallel NT alignment benchmark\n"
            << "workers        = " << k_worker_count << '\n'
            << "copy size      = " << size_kib << " KiB\n"
            << "dst offset     = " << dst_offset << '\n'
            << "total copy     = " << total_gib << " GiB\n"
            << "elapsed        = " << elapsed_seconds << " s\n"
            << "bandwidth      = " << bandwidth_gib_s << " GiB/s\n"
            << "correctness    = PASS\n";
}

void run_parallel_nt_policy_benchmark(std::size_t worker_count, std::size_t size_kib) {
  using TLSS::MEMORY::INTERNAL::parallel_nt_copy;
  using TLSS::MEMORY::INTERNAL::ParallelCopyPool;

  constexpr int k_worker_cpu_ids[] = {0, 2, 4, 6, 8, 10, 12, 14};

  constexpr std::size_t k_cpu_count = std::size(k_worker_cpu_ids);

  constexpr std::size_t k_alignment = 4096;

  constexpr std::size_t k_working_set = 2ULL * 1024 * 1024 * 1024;

  constexpr std::size_t k_warmup_size = 8ULL * 1024 * 1024;

  constexpr std::size_t k_warmup_iterations = 4;

  if (worker_count == 0 || worker_count > k_cpu_count) {
    throw std::invalid_argument("invalid worker_count");
  }

  if (size_kib == 0) {
    throw std::invalid_argument("size_kib must be greater than zero");
  }

  const std::size_t copy_size = size_kib * 1024ULL;

  if (copy_size > k_working_set) {
    throw std::invalid_argument("copy_size must not exceed working_set");
  }

  if (k_working_set % copy_size != 0) {
    throw std::invalid_argument("working_set must be divisible by copy_size");
  }

  //
  // main thread（主线程）固定在 E-core 16。
  //
  if (!pin_current_thread_to_cpu(16)) {
    throw std::runtime_error("failed to pin main thread");
  }

  const std::size_t slot_count = k_working_set / copy_size;

  //
  // ------------------------------------------------------------
  // Formal working set（正式工作集）
  // ------------------------------------------------------------
  //

  AlignedBuffer src_buffer(k_working_set, k_alignment);

  AlignedBuffer dst_buffer(k_working_set, k_alignment);

  auto* src = src_buffer.data();

  auto* dst = dst_buffer.data();

  for (std::size_t offset = 0; offset < k_working_set; offset += sizeof(std::uint64_t)) {
    const std::uint64_t value = mix64(static_cast<std::uint64_t>(offset));

    std::memcpy(src + offset, &value, sizeof(value));
  }

  std::memset(dst, 0, k_working_set);

  ParallelCopyPool pool(worker_count, k_worker_cpu_ids);

  //
  // ------------------------------------------------------------
  // Independent warmup（独立预热）
  // ------------------------------------------------------------
  //

  AlignedBuffer warmup_src_buffer(k_warmup_size, k_alignment);

  AlignedBuffer warmup_dst_buffer(k_warmup_size, k_alignment);

  auto* warmup_src = warmup_src_buffer.data();

  auto* warmup_dst = warmup_dst_buffer.data();

  for (std::size_t offset = 0; offset < k_warmup_size; offset += sizeof(std::uint64_t)) {
    const std::uint64_t value = mix64(static_cast<std::uint64_t>(offset));

    std::memcpy(warmup_src + offset, &value, sizeof(value));
  }

  std::memset(warmup_dst, 0, k_warmup_size);

  for (std::size_t iteration = 0; iteration < k_warmup_iterations; ++iteration) {
    /* parallel_nt_copy(pool, warmup_dst, warmup_src, k_warmup_size); */
  }

  //
  // ------------------------------------------------------------
  // Timing region（计时区域）
  // ------------------------------------------------------------
  //

  const auto begin = std::chrono::steady_clock::now();

  for (std::size_t slot_index = 0; slot_index < slot_count; ++slot_index) {
    const std::size_t offset = slot_index * copy_size;

    /* parallel_nt_copy(pool, dst + offset, src + offset, copy_size); */
  }

  const auto end = std::chrono::steady_clock::now();

  //
  // ------------------------------------------------------------
  // Correctness（正确性）
  // ------------------------------------------------------------
  //

  if (std::memcmp(dst, src, k_working_set) != 0) {
    throw std::runtime_error("parallel NT policy benchmark correctness failed");
  }

  const double elapsed_seconds = std::chrono::duration<double>(end - begin).count();

  const double total_gib =
      static_cast<double>(k_working_set) / static_cast<double>(1024ULL * 1024 * 1024);

  const double bandwidth_gib_s = total_gib / elapsed_seconds;

  std::cout << "Parallel NT policy benchmark\n"
            << "workers        = " << worker_count << '\n'
            << "copy size      = " << size_kib << " KiB\n"
            << "iterations     = " << slot_count << '\n'
            << "total copy     = " << total_gib << " GiB\n"
            << "elapsed        = " << elapsed_seconds << " s\n"
            << "bandwidth      = " << bandwidth_gib_s << " GiB/s\n"
            << "correctness    = PASS\n";
}

void run_copy_policy_test() {
  using TLSS::MEMORY::CopyHint;
  using TLSS::MEMORY::INTERNAL::CopyPlan;
  using TLSS::MEMORY::INTERNAL::CopyStrategy;
  using TLSS::MEMORY::INTERNAL::select_copy_plan;

  struct TestCase {
    std::size_t size;
    CopyHint hint;
    CopyStrategy expected_strategy;
    std::size_t expected_worker_count;
  };
  constexpr TestCase k_test_cases[] = {
      // Default（默认）
      {0, CopyHint::Default, CopyStrategy::Avx2Cached, 1},
      {2047, CopyHint::Default, CopyStrategy::Avx2Cached, 1},
      {2048, CopyHint::Default, CopyStrategy::Avx2Cached, 1},
      {2049, CopyHint::Default, CopyStrategy::RepMovsb, 1},

      // Streaming（流式）
      // <= 2 KiB → AVX2
      {0, CopyHint::Streaming, CopyStrategy::Avx2Cached, 1},
      {2047, CopyHint::Streaming, CopyStrategy::Avx2Cached, 1},
      {2048, CopyHint::Streaming, CopyStrategy::Avx2Cached, 1},

      // 2049 ~ 3071 B → glibc
      {2049, CopyHint::Streaming, CopyStrategy::LibcMemcpy, 1},
      {3 * 1024 - 1, CopyHint::Streaming, CopyStrategy::LibcMemcpy, 1},

      // 3 KiB ~ 8191 B → AVX2
      {3 * 1024, CopyHint::Streaming, CopyStrategy::Avx2Cached, 1},
      {3 * 1024 + 1, CopyHint::Streaming, CopyStrategy::Avx2Cached, 1},
      {8 * 1024 - 1, CopyHint::Streaming, CopyStrategy::Avx2Cached, 1},

      // 8 KiB ~ 65535 B → Direct NT（直接非临时复制）
      {8 * 1024, CopyHint::Streaming, CopyStrategy::DirectNt, 1},
      {8 * 1024 + 1, CopyHint::Streaming, CopyStrategy::DirectNt, 1},
      {64 * 1024 - 1, CopyHint::Streaming, CopyStrategy::DirectNt, 1},

      // 64 KiB ~ 127 KiB → Parallel NT 4W（4 工作线程并行 NT）
      {64 * 1024, CopyHint::Streaming, CopyStrategy::ParallelNt, 4},
      {64 * 1024 + 1, CopyHint::Streaming, CopyStrategy::ParallelNt, 4},
      {128 * 1024 - 1, CopyHint::Streaming, CopyStrategy::ParallelNt, 4},

      // >= 128 KiB → Parallel NT 8W（8 工作线程并行 NT）
      {128 * 1024, CopyHint::Streaming, CopyStrategy::ParallelNt, 8},
      {128 * 1024 + 1, CopyHint::Streaming, CopyStrategy::ParallelNt, 8},
      {1024 * 1024, CopyHint::Streaming, CopyStrategy::ParallelNt, 8},
  };
  bool all_passed = true;

  for (const auto& test_case : k_test_cases) {
    const CopyPlan plan = select_copy_plan(test_case.size, test_case.hint);

    const bool passed = plan.strategy == test_case.expected_strategy &&
                        plan.worker_count == test_case.expected_worker_count;

    std::cout << "size=" << test_case.size << " workers=" << plan.worker_count
              << " result=" << (passed ? "PASS" : "FAIL") << '\n';

    if (!passed) {
      all_passed = false;
    }
  }

  if (!all_passed) {
    throw std::runtime_error("copy policy test failed");
  }

  std::cout << "Copy policy test: PASS\n";
}

void run_copy_capability_policy_test() {
  using namespace TLSS::MEMORY::INTERNAL;

  const auto expect_plan = [](CopyPlan input, const CpuCapabilities& capabilities,
                              CopyStrategy expected_strategy, std::size_t expected_workers) {
    const CopyPlan output = adapt_auto_plan_for_capabilities(input, capabilities);
    if (output.strategy != expected_strategy || output.worker_count != expected_workers) {
      throw std::runtime_error("copy capability policy produced an unexpected plan");
    }
  };

  const CpuCapabilities no_avx2{};
  expect_plan({CopyStrategy::LibcMemcpy, 1}, no_avx2, CopyStrategy::LibcMemcpy, 1);
  expect_plan({CopyStrategy::RepMovsb, 1}, no_avx2, CopyStrategy::RepMovsb, 1);
  expect_plan({CopyStrategy::Avx2Cached, 1}, no_avx2, CopyStrategy::LibcMemcpy, 1);
  expect_plan({CopyStrategy::DirectNt, 1}, no_avx2, CopyStrategy::LibcMemcpy, 1);
  expect_plan({CopyStrategy::ParallelNt, 8}, no_avx2, CopyStrategy::LibcMemcpy, 1);

  CpuCapabilities hardware_without_ymm_state{};
  hardware_without_ymm_state.avx_hardware = true;
  hardware_without_ymm_state.avx2_hardware = true;
  hardware_without_ymm_state.osxsave = true;
  hardware_without_ymm_state.avx_usable = false;
  hardware_without_ymm_state.avx2_usable = false;
  expect_plan({CopyStrategy::ParallelNt, 8}, hardware_without_ymm_state,
              CopyStrategy::LibcMemcpy, 1);

  CpuCapabilities avx2{};
  avx2.avx_hardware = true;
  avx2.avx2_hardware = true;
  avx2.osxsave = true;
  avx2.avx_usable = true;
  avx2.avx2_usable = true;
  avx2.erms = true;
  expect_plan({CopyStrategy::LibcMemcpy, 1}, avx2, CopyStrategy::LibcMemcpy, 1);
  expect_plan({CopyStrategy::RepMovsb, 1}, avx2, CopyStrategy::RepMovsb, 1);
  expect_plan({CopyStrategy::Avx2Cached, 1}, avx2, CopyStrategy::Avx2Cached, 1);
  expect_plan({CopyStrategy::DirectNt, 1}, avx2, CopyStrategy::DirectNt, 1);
  expect_plan({CopyStrategy::ParallelNt, 8}, avx2, CopyStrategy::ParallelNt, 8);

  expect_plan({static_cast<CopyStrategy>(255), 8}, avx2, CopyStrategy::LibcMemcpy, 1);
  std::cout << "Copy capability policy test: PASS\n";
}

void run_auto_capability_integration_test() {
  using namespace TLSS::MEMORY;
  using namespace TLSS::MEMORY::INTERNAL;

  const CpuCapabilities no_avx2{};
  constexpr std::size_t k_streaming_sizes[] = {1024, 2560, 4096, 32 * 1024, 256 * 1024};
  for (std::size_t size : k_streaming_sizes) {
    const CopyPlan plan = select_safe_auto_copy_plan(size, CopyHint::Streaming, no_avx2);
    if (plan.strategy != CopyStrategy::LibcMemcpy || plan.worker_count != 1) {
      throw std::runtime_error("Auto capability fallback failed");
    }
    std::cout << "size=" << size << " strategy=LibcMemcpy result=PASS\n";
  }

  const CopyPlan default_small = select_safe_auto_copy_plan(1024, CopyHint::Default, no_avx2);
  const CopyPlan default_large = select_safe_auto_copy_plan(4096, CopyHint::Default, no_avx2);
  if (default_small.strategy != CopyStrategy::LibcMemcpy || default_small.worker_count != 1 ||
      default_large.strategy != CopyStrategy::RepMovsb || default_large.worker_count != 1) {
    throw std::runtime_error("Default Auto capability handling failed");
  }

  CpuCapabilities avx2{};
  avx2.avx2_usable = true;
  const CopyPlan capable = select_safe_auto_copy_plan(256 * 1024, CopyHint::Streaming, avx2);
  if (capable.strategy != CopyStrategy::ParallelNt || capable.worker_count != 8) {
    throw std::runtime_error("AVX2-capable Auto plan changed unexpectedly");
  }

  constexpr std::size_t k_copy_size = 256 * 1024;
  AlignedBuffer src_buffer(k_copy_size, 64);
  AlignedBuffer dst_buffer(k_copy_size, 64);
  auto* src = src_buffer.data();
  auto* dst = dst_buffer.data();
  for (std::size_t index = 0; index < k_copy_size; ++index) {
    src[index] = static_cast<std::uint8_t>((index * 131U + 17U) & 0xffU);
  }

  const CopyPlan fallback =
      select_safe_auto_copy_plan(k_copy_size, CopyHint::Streaming, no_avx2);
  std::memset(dst, 0xA5, k_copy_size);
  if (execute_copy_plan(fallback, nullptr, dst, src, k_copy_size) != dst ||
      std::memcmp(dst, src, k_copy_size) != 0) {
    throw std::runtime_error("injected Auto fallback execution failed");
  }

  for (std::size_t size : {1024U, 4096U}) {
    std::memset(dst, 0xA5, size);
    if (TLSS::MEMORY::memcpy(dst, src, size) != dst || std::memcmp(dst, src, size) != 0) {
      throw std::runtime_error("three-argument Auto copy failed");
    }
    std::memset(dst, 0xA5, size);
    if (TLSS::MEMORY::memcpy(dst, src, size, CopyBackend::Auto, CopyHint::Default) != dst ||
        std::memcmp(dst, src, size) != 0) {
      throw std::runtime_error("five-argument Default Auto copy failed");
    }
  }

  std::cout << "Auto capability integration test: PASS\n";
}

void run_explicit_backend_capability_test() {
  using namespace TLSS::MEMORY;
  using namespace TLSS::MEMORY::INTERNAL;

  const auto expect_support = [](CopyBackend backend, const CpuCapabilities& capabilities,
                                 bool expected) {
    if (is_backend_supported(backend, capabilities) != expected) {
      throw std::runtime_error("explicit backend capability decision mismatch");
    }
  };

  const CpuCapabilities no_avx2{};
  expect_support(CopyBackend::Auto, no_avx2, true);
  expect_support(CopyBackend::Avx2, no_avx2, false);
  expect_support(CopyBackend::NonTemporal, no_avx2, false);
  expect_support(CopyBackend::RepMovsb, no_avx2, true);

  CpuCapabilities os_disabled{};
  os_disabled.avx_hardware = true;
  os_disabled.avx2_hardware = true;
  os_disabled.erms = true;
  expect_support(CopyBackend::Avx2, os_disabled, false);
  expect_support(CopyBackend::NonTemporal, os_disabled, false);

  CpuCapabilities avx2{};
  avx2.avx_hardware = true;
  avx2.avx2_hardware = true;
  avx2.osxsave = true;
  avx2.avx_usable = true;
  avx2.avx2_usable = true;
  avx2.erms = true;
  expect_support(CopyBackend::Auto, avx2, true);
  expect_support(CopyBackend::Avx2, avx2, true);
  expect_support(CopyBackend::NonTemporal, avx2, true);
  expect_support(CopyBackend::RepMovsb, avx2, true);
  expect_support(static_cast<CopyBackend>(255), avx2, false);

  std::cout << "Explicit backend capability test: PASS\n";
}

void run_backend_plan_contract_test() {
  using namespace TLSS::MEMORY;
  using namespace TLSS::MEMORY::INTERNAL;

  const auto expect_plan = [](std::size_t size, CopyHint hint, CopyBackend backend,
                              const CpuCapabilities& capabilities, CopyStrategy strategy,
                              std::size_t worker_count) {
    const CopyPlan plan = make_copy_plan_for_backend(size, hint, backend, capabilities);
    if (plan.strategy != strategy || plan.worker_count != worker_count) {
      throw std::runtime_error("backend plan contract mismatch");
    }
  };
  const auto expect_unsupported = [](std::size_t size, CopyHint hint, CopyBackend backend,
                                     const CpuCapabilities& capabilities) {
    try {
      static_cast<void>(make_copy_plan_for_backend(size, hint, backend, capabilities));
    } catch (const std::runtime_error&) {
      return;
    }
    throw std::runtime_error("unsupported explicit backend was accepted");
  };

  const CpuCapabilities no_avx2{};
  expect_plan(256 * 1024, CopyHint::Streaming, CopyBackend::Auto, no_avx2,
              CopyStrategy::LibcMemcpy, 1);
  expect_unsupported(4096, CopyHint::Default, CopyBackend::Avx2, no_avx2);
  expect_unsupported(256 * 1024, CopyHint::Streaming, CopyBackend::NonTemporal, no_avx2);
  expect_plan(4096, CopyHint::Default, CopyBackend::RepMovsb, no_avx2,
              CopyStrategy::RepMovsb, 1);

  CpuCapabilities os_disabled{};
  os_disabled.avx_hardware = true;
  os_disabled.avx2_hardware = true;
  os_disabled.erms = true;
  expect_unsupported(4096, CopyHint::Default, CopyBackend::Avx2, os_disabled);
  expect_unsupported(256 * 1024, CopyHint::Streaming, CopyBackend::NonTemporal, os_disabled);

  CpuCapabilities avx2{};
  avx2.avx_hardware = true;
  avx2.avx2_hardware = true;
  avx2.osxsave = true;
  avx2.avx_usable = true;
  avx2.avx2_usable = true;
  avx2.erms = true;
  expect_plan(256 * 1024, CopyHint::Streaming, CopyBackend::Auto, avx2,
              CopyStrategy::ParallelNt, 8);
  expect_plan(4096, CopyHint::Default, CopyBackend::Avx2, avx2,
              CopyStrategy::Avx2Cached, 1);
  expect_plan(256 * 1024, CopyHint::Streaming, CopyBackend::NonTemporal, avx2,
              CopyStrategy::DirectNt, 1);
  expect_plan(4096, CopyHint::Default, CopyBackend::RepMovsb, avx2,
              CopyStrategy::RepMovsb, 1);
  expect_unsupported(4096, CopyHint::Default, static_cast<CopyBackend>(255), avx2);

  std::cout << "Backend plan contract test: PASS\n";
}

void run_tlss_memcpy_backend_test() {
  using TLSS::MEMORY::CopyBackend;
  using TLSS::MEMORY::CopyHint;
  using TLSS::MEMORY::INTERNAL::get_cpu_capabilities;

  struct BackendCase {
    CopyBackend backend;
    const char* name;
    bool requires_avx2;
  };
  constexpr BackendCase k_backends[] = {
      {CopyBackend::Auto, "Auto", false},
      {CopyBackend::Avx2, "Avx2", true},
      {CopyBackend::RepMovsb, "RepMovsb", false},
      {CopyBackend::NonTemporal, "NonTemporal", true},
  };
  constexpr std::size_t k_sizes[] = {1024, 4096, 32768, 131072, 1048576};
  constexpr std::size_t k_max_size = 1048576;
  constexpr std::size_t k_src_offset = 7;
  constexpr std::size_t k_dst_offset = 13;

  AlignedBuffer src_buffer(k_max_size + 64, 4096);
  AlignedBuffer dst_buffer(k_max_size + 64, 4096);
  auto* src = src_buffer.data() + k_src_offset;
  auto* dst = dst_buffer.data() + k_dst_offset;
  for (std::size_t index = 0; index < k_max_size; ++index) {
    src[index] = static_cast<std::uint8_t>((index * 131U + 17U) & 0xffU);
  }

  const bool avx2_usable = get_cpu_capabilities().avx2_usable;
  for (const BackendCase& test_case : k_backends) {
    for (std::size_t size : k_sizes) {
      std::memset(dst, 0xA5, size);
      void* result = nullptr;
      bool unsupported = false;
      try {
        result = TLSS::MEMORY::memcpy(dst, src, size, test_case.backend, CopyHint::Streaming);
      } catch (const std::runtime_error&) {
        unsupported = true;
      }

      if (test_case.requires_avx2 && !avx2_usable) {
        if (!unsupported) {
          throw std::runtime_error("unsupported explicit backend did not reject the CPU");
        }
      } else if (unsupported || result != dst || std::memcmp(dst, src, size) != 0) {
        throw std::runtime_error("public memcpy backend copy mismatch");
      }
      std::cout << "backend=" << test_case.name << " size=" << size << " result=PASS\n";
    }
  }

  std::cout << "TLSS memcpy backend test: PASS\n";
}

void run_copy_executor_test() {
  using namespace TLSS::MEMORY::INTERNAL;

  constexpr std::size_t k_size = 256 * 1024;

  AlignedBuffer src_buffer(k_size, 4096);

  AlignedBuffer dst_buffer(k_size, 4096);

  auto* src = src_buffer.data();
  auto* dst = dst_buffer.data();

  MemoryRuntime runtime;

  for (std::size_t i = 0; i < k_size; ++i) {
    src[i] = static_cast<std::uint8_t>(i * 131U + 17U);
  }

  //
  // Avx2Cached（缓存型 AVX2）
  //
  {
    std::memset(dst, 0, k_size);

    const CopyPlan plan{CopyStrategy::Avx2Cached, 1};

    void* result = execute_copy_plan(plan, &runtime, dst, src, 1024);

    if (result != dst || std::memcmp(dst, src, 1024) != 0) {
      throw std::runtime_error("Avx2Cached executor test failed");
    }
  }

  //
  // RepMovsb
  //
  {
    std::memset(dst, 0, k_size);

    const CopyPlan plan{CopyStrategy::RepMovsb, 1};

    void* result = execute_copy_plan(plan, &runtime, dst, src, 16 * 1024);

    if (result != dst || std::memcmp(dst, src, 16 * 1024) != 0) {
      throw std::runtime_error("RepMovsb executor test failed");
    }
  }

  //
  // DirectNt（直接非临时复制）
  //
  {
    constexpr std::size_t k_copy_size = 32773;

    constexpr std::size_t k_dst_offset = 13;

    constexpr std::size_t k_src_offset = 7;

    std::memset(dst, 0, k_size);

    const CopyPlan plan{CopyStrategy::DirectNt, 1};

    void* result =
        execute_copy_plan(plan, &runtime, dst + k_dst_offset, src + k_src_offset, k_copy_size);

    if (result != dst + k_dst_offset ||
        std::memcmp(dst + k_dst_offset, src + k_src_offset, k_copy_size) != 0) {
      throw std::runtime_error("DirectNt executor test failed");
    }
  }

  //
  // ParallelNt / 4W
  //
  {
    constexpr std::size_t k_copy_size = 64 * 1024;

    std::memset(dst, 0, k_size);

    const CopyPlan plan{CopyStrategy::ParallelNt, 4};

    void* result = execute_copy_plan(plan, &runtime, dst, src, k_copy_size);

    if (result != dst || std::memcmp(dst, src, k_copy_size) != 0) {
      throw std::runtime_error("ParallelNt 4W executor test failed");
    }
  }

  //
  // ParallelNt / 8W
  //
  {
    constexpr std::size_t k_copy_size = 128 * 1024;

    std::memset(dst, 0, k_size);

    const CopyPlan plan{CopyStrategy::ParallelNt, 8};

    void* result = execute_copy_plan(plan, &runtime, dst, src, k_copy_size);

    if (result != dst || std::memcmp(dst, src, k_copy_size) != 0) {
      throw std::runtime_error("ParallelNt 8W executor test failed");
    }
  }

  std::cout << "Copy executor test: PASS\n";
}

void run_memory_runtime_concurrency_test() {
  using namespace TLSS::MEMORY::INTERNAL;

  /* constexpr std::size_t k_copy_size = 128 * 1024; */
  constexpr std::size_t k_copy_size = 64ULL * 1024 * 1024;

  constexpr std::size_t k_alignment = 4096;

  MemoryRuntime runtime;

  AlignedBuffer src_1(k_copy_size, k_alignment);

  AlignedBuffer dst_1(k_copy_size, k_alignment);

  AlignedBuffer src_2(k_copy_size, k_alignment);

  AlignedBuffer dst_2(k_copy_size, k_alignment);

  auto* src1 = src_1.data();
  auto* dst1 = dst_1.data();

  auto* src2 = src_2.data();
  auto* dst2 = dst_2.data();

  for (std::size_t i = 0; i < k_copy_size; ++i) {
    src1[i] = static_cast<std::uint8_t>(i * 17U + 3U);

    src2[i] = static_cast<std::uint8_t>(i * 29U + 11U);
  }

  std::memset(dst1, 0, k_copy_size);

  std::memset(dst2, 0, k_copy_size);

  const CopyPlan plan{CopyStrategy::ParallelNt, 8};

  std::atomic<bool> start{false};

  std::thread thread_1([&]() {
    while (!start.load(std::memory_order_acquire)) {
      _mm_pause();
    }

    execute_copy_plan(plan, &runtime, dst1, src1, k_copy_size);
  });

  std::thread thread_2([&]() {
    while (!start.load(std::memory_order_acquire)) {
      _mm_pause();
    }

    execute_copy_plan(plan, &runtime, dst2, src2, k_copy_size);
  });

  //
  // 同时释放两个 caller（调用线程）。
  //
  start.store(true, std::memory_order_release);

  thread_1.join();
  thread_2.join();

  if (std::memcmp(dst1, src1, k_copy_size) != 0) {
    throw std::runtime_error("concurrency test copy 1 failed");
  }

  if (std::memcmp(dst2, src2, k_copy_size) != 0) {
    throw std::runtime_error("concurrency test copy 2 failed");
  }
  std::cout << "Memory runtime concurrency test: PASS\n";
}

void run_tlss_memcpy_auto_test() {
  using TLSS::MEMORY::CopyBackend;
  using TLSS::MEMORY::CopyHint;

  constexpr std::size_t k_size = 512 * 1024;

  AlignedBuffer src_buffer(k_size + 64, 4096);

  AlignedBuffer dst_buffer(k_size + 64, 4096);

  auto* src = src_buffer.data();
  auto* dst = dst_buffer.data();

  for (std::size_t i = 0; i < k_size + 64; ++i) {
    src[i] = static_cast<std::uint8_t>(i * 37U + 11U);
  }

  constexpr std::size_t k_sizes[] = {1024, 4096, 32 * 1024, 64 * 1024, 128 * 1024, 256 * 1024};

  for (std::size_t size : k_sizes) {
    constexpr std::size_t k_dst_offset = 13;
    constexpr std::size_t k_src_offset = 7;

    std::memset(dst, 0, k_size + 64);

    void* result = TLSS::MEMORY::memcpy(dst + k_dst_offset, src + k_src_offset, size,
                                        CopyBackend::Auto, CopyHint::Streaming);

    if (result != dst + k_dst_offset ||
        std::memcmp(dst + k_dst_offset, src + k_src_offset, size) != 0) {
      throw std::runtime_error("TLSS memcpy auto integration test failed");
    }

    std::cout << "size=" << size << " result=PASS\n";
  }

  std::cout << "TLSS memcpy auto test: PASS\n";
}

void run_tlss_memcpy_streaming_benchmark(std::size_t size_kib) {
  constexpr std::size_t k_alignment = 4096;

  constexpr std::size_t k_working_set = 2ULL * 1024 * 1024 * 1024;

  const std::size_t copy_size = size_kib * 1024ULL;

  if (copy_size == 0 || k_working_set % copy_size != 0) {
    throw std::invalid_argument("invalid copy size");
  }

  AlignedBuffer src_buffer(k_working_set, k_alignment);

  AlignedBuffer dst_buffer(k_working_set, k_alignment);

  auto* src = src_buffer.data();
  auto* dst = dst_buffer.data();

  for (std::size_t offset = 0; offset < k_working_set; offset += sizeof(std::uint64_t)) {
    const std::uint64_t value = mix64(static_cast<std::uint64_t>(offset));

    std::memcpy(src + offset, &value, sizeof(value));
  }

  std::memset(dst, 0, k_working_set);

  const std::size_t iteration_count = k_working_set / copy_size;

  //
  // 第一次调用可能触发 MemoryRuntime
  // lazy initialization（延迟初始化）。
  //
  // 所以放在正式 timing region（计时区间）之外。
  //
  TLSS::MEMORY::memcpy(dst, src, copy_size, TLSS::MEMORY::CopyBackend::Auto,
                       TLSS::MEMORY::CopyHint::Streaming);

  std::memset(dst, 0, k_working_set);

  const auto begin = std::chrono::steady_clock::now();

  for (std::size_t iteration = 0; iteration < iteration_count; ++iteration) {
    const std::size_t offset = iteration * copy_size;

    TLSS::MEMORY::memcpy(dst + offset, src + offset, copy_size, TLSS::MEMORY::CopyBackend::Auto,
                         TLSS::MEMORY::CopyHint::Streaming);
  }

  const auto end = std::chrono::steady_clock::now();

  if (std::memcmp(dst, src, k_working_set) != 0) {
    throw std::runtime_error("TLSS streaming benchmark correctness failed");
  }

  const double elapsed_seconds = std::chrono::duration<double>(end - begin).count();

  constexpr double k_total_gib = 2.0;

  std::cout << "TLSS streaming benchmark\n"
            << "copy size      = " << size_kib << " KiB\n"
            << "iterations     = " << iteration_count << '\n'
            << "total copy     = " << k_total_gib << " GiB\n"
            << "elapsed        = " << elapsed_seconds << " s\n"
            << "bandwidth      = " << k_total_gib / elapsed_seconds << " GiB/s\n"
            << "correctness    = PASS\n";
}

void run_tlss_memcpy_cpu8_benchmark(std::size_t size_kib) {
  constexpr std::size_t k_runtime_warmup_size = 256ULL * 1024ULL;
  AlignedBuffer warmup_src(k_runtime_warmup_size, 4096);
  AlignedBuffer warmup_dst(k_runtime_warmup_size, 4096);
  std::memset(warmup_src.data(), 0x5A, k_runtime_warmup_size);

  // Initialize the process-wide runtime before restricting the caller to CPU8.
  TLSS::MEMORY::memcpy(warmup_dst.data(), warmup_src.data(), k_runtime_warmup_size,
                       TLSS::MEMORY::CopyBackend::Auto, TLSS::MEMORY::CopyHint::Streaming);

  cpu_set_t original_cpu_set;
  CPU_ZERO(&original_cpu_set);
  if (pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &original_cpu_set) != 0) {
    throw std::runtime_error("failed to get caller affinity");
  }

  struct AffinityRestore {
    const cpu_set_t& original;
    ~AffinityRestore() noexcept {
      pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &original);
    }
  } restore{original_cpu_set};

  cpu_set_t caller_cpu_set;
  CPU_ZERO(&caller_cpu_set);
  CPU_SET(8, &caller_cpu_set);
  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &caller_cpu_set) != 0) {
    throw std::runtime_error("failed to pin caller to CPU8");
  }

  run_tlss_memcpy_streaming_benchmark(size_kib);
}

void run_memory_runtime_performance_benchmark(std::size_t size_kib) {
  using TLSS::MEMORY::INTERNAL::MemoryRuntime;

  constexpr std::size_t k_working_set = 2ULL * 1024 * 1024 * 1024;
  constexpr std::size_t k_alignment = 4096;
  constexpr int k_caller_cpu = 8;

  if (size_kib == 0 || size_kib > k_working_set / 1024) {
    throw std::invalid_argument("invalid copy size");
  }
  const std::size_t copy_size = size_kib * 1024ULL;
  if (k_working_set % copy_size != 0) {
    throw std::invalid_argument("invalid copy size");
  }

  // Capture the full caller affinity before pinning it to CPU8.
  MemoryRuntime runtime;

  cpu_set_t original_cpu_set;
  CPU_ZERO(&original_cpu_set);
  if (pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &original_cpu_set) != 0) {
    throw std::runtime_error("failed to get caller affinity");
  }

  struct AffinityRestore {
    const cpu_set_t& original;
    ~AffinityRestore() noexcept {
      pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &original);
    }
  } restore{original_cpu_set};

  cpu_set_t caller_cpu_set;
  CPU_ZERO(&caller_cpu_set);
  CPU_SET(k_caller_cpu, &caller_cpu_set);
  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &caller_cpu_set) != 0) {
    throw std::runtime_error("failed to pin caller to CPU8");
  }

  AlignedBuffer src_buffer(k_working_set, k_alignment);
  AlignedBuffer dst_buffer(k_working_set, k_alignment);
  auto* src = src_buffer.data();
  auto* dst = dst_buffer.data();
  std::memset(src, 0x5A, k_working_set);
  std::memset(dst, 0xA5, k_working_set);

  const std::size_t iteration_count = k_working_set / copy_size;

  runtime.execute_parallel_copy(8, dst, src, copy_size);
  std::memset(dst, 0xA5, k_working_set);

  const auto begin = std::chrono::steady_clock::now();
  for (std::size_t iteration = 0; iteration < iteration_count; ++iteration) {
    const std::size_t offset = iteration * copy_size;
    runtime.execute_parallel_copy(8, dst + offset, src + offset, copy_size);
  }
  const auto end = std::chrono::steady_clock::now();

  if (std::memcmp(dst, src, k_working_set) != 0) {
    throw std::runtime_error("MemoryRuntime performance benchmark mismatch");
  }

  const double elapsed_seconds = std::chrono::duration<double>(end - begin).count();
  constexpr double k_total_gib = 2.0;
  std::cout << "runtime caller_cpu=" << k_caller_cpu << " desired_workers=8 size=" << size_kib
            << " KiB bandwidth=" << k_total_gib / elapsed_seconds << " GiB/s\n";

  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &original_cpu_set) != 0) {
    throw std::runtime_error("failed to restore caller affinity");
  }
}

#ifdef TLSS_MEMORY_TESTING
void run_restricted_runtime_performance_benchmark(int caller_cpu, std::size_t size_kib,
                                                  std::size_t desired_worker_count) {
  using TLSS::MEMORY::INTERNAL::direct_nt_copy;
  using TLSS::MEMORY::INTERNAL::MemoryRuntime;

  constexpr std::size_t k_working_set = 2ULL * 1024 * 1024 * 1024;
  constexpr std::size_t k_alignment = 4096;

  if (size_kib == 0 || size_kib > k_working_set / 1024 || caller_cpu < 0 ||
      caller_cpu >= CPU_SETSIZE ||
      (desired_worker_count != 4 && desired_worker_count != 8)) {
    throw std::invalid_argument("invalid caller CPU or copy size");
  }
  const std::size_t copy_size = size_kib * 1024ULL;
  if (k_working_set % copy_size != 0) {
    throw std::invalid_argument("invalid copy size");
  }

  // Detect the taskset affinity before pinning the caller to one CPU.
  MemoryRuntime runtime;

  cpu_set_t original_cpu_set;
  CPU_ZERO(&original_cpu_set);
  if (pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &original_cpu_set) != 0) {
    throw std::runtime_error("failed to get caller affinity");
  }
  if (!CPU_ISSET(caller_cpu, &original_cpu_set)) {
    throw std::invalid_argument("caller CPU is outside the available affinity set");
  }

  struct AffinityRestore {
    const cpu_set_t& original;
    ~AffinityRestore() noexcept {
      pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &original);
    }
  } restore{original_cpu_set};

  cpu_set_t caller_cpu_set;
  CPU_ZERO(&caller_cpu_set);
  CPU_SET(caller_cpu, &caller_cpu_set);
  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &caller_cpu_set) != 0) {
    throw std::runtime_error("failed to pin caller");
  }

  AlignedBuffer src_buffer(k_working_set, k_alignment);
  AlignedBuffer runtime_dst_buffer(k_working_set, k_alignment);
  AlignedBuffer direct_dst_buffer(k_working_set, k_alignment);
  auto* src = src_buffer.data();
  auto* runtime_dst = runtime_dst_buffer.data();
  auto* direct_dst = direct_dst_buffer.data();

  std::memset(src, 0x5A, k_working_set);
  std::memset(runtime_dst, 0xA5, k_working_set);
  std::memset(direct_dst, 0xA5, k_working_set);

  const auto* active_workers =
      runtime.active_worker_indices_for_test(caller_cpu, desired_worker_count);
  const std::size_t available_workers = active_workers == nullptr ? 0 : active_workers->size();
  const std::size_t iteration_count = k_working_set / copy_size;

  const auto runtime_begin = std::chrono::steady_clock::now();
  for (std::size_t iteration = 0; iteration < iteration_count; ++iteration) {
    const std::size_t offset = iteration * copy_size;
    if (runtime.execute_parallel_copy(desired_worker_count, runtime_dst + offset, src + offset,
                                      copy_size) !=
        runtime_dst + offset) {
      throw std::runtime_error("runtime copy returned the wrong destination");
    }
  }
  const auto runtime_end = std::chrono::steady_clock::now();

  const auto direct_begin = std::chrono::steady_clock::now();
  for (std::size_t iteration = 0; iteration < iteration_count; ++iteration) {
    const std::size_t offset = iteration * copy_size;
    if (direct_nt_copy(direct_dst + offset, src + offset, copy_size) != direct_dst + offset) {
      throw std::runtime_error("direct NT copy returned the wrong destination");
    }
  }
  const auto direct_end = std::chrono::steady_clock::now();

  if (std::memcmp(runtime_dst, src, k_working_set) != 0) {
    throw std::runtime_error("runtime result mismatch");
  }
  if (std::memcmp(direct_dst, src, k_working_set) != 0) {
    throw std::runtime_error("direct NT result mismatch");
  }

  const double runtime_seconds =
      std::chrono::duration<double>(runtime_end - runtime_begin).count();
  const double direct_seconds = std::chrono::duration<double>(direct_end - direct_begin).count();
  constexpr double k_total_gib = 2.0;
  std::cout << "caller_cpu=" << caller_cpu << " desired_workers=" << desired_worker_count
            << " available_workers=" << available_workers
            << " size=" << size_kib << " KiB runtime=" << k_total_gib / runtime_seconds
            << " GiB/s direct_nt=" << k_total_gib / direct_seconds << " GiB/s\n";

  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &original_cpu_set) != 0) {
    throw std::runtime_error("failed to restore caller affinity");
  }
}
#endif

void run_runtime_selection_overhead_benchmark() {
  using namespace TLSS::MEMORY::INTERNAL;

  constexpr std::size_t k_iterations = 1000000;
  const std::vector<int> available_cpu_ids = detect_available_cpu_ids();
  const CpuTopology topology = detect_cpu_topology(available_cpu_ids);
  const std::vector<int> master_worker_cpu_ids = select_master_worker_cpu_ids(topology);

  cpu_set_t original_cpu_set;
  CPU_ZERO(&original_cpu_set);
  if (pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &original_cpu_set) != 0) {
    throw std::runtime_error("failed to get caller affinity");
  }

  struct AffinityRestore {
    const cpu_set_t& original;
    ~AffinityRestore() noexcept {
      pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &original);
    }
  } restore{original_cpu_set};

  cpu_set_t caller_cpu_set;
  CPU_ZERO(&caller_cpu_set);
  CPU_SET(8, &caller_cpu_set);
  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &caller_cpu_set) != 0) {
    throw std::runtime_error("failed to pin caller to CPU8");
  }

  std::size_t result_sum = 0;
  const auto begin = std::chrono::steady_clock::now();
  for (std::size_t iteration = 0; iteration < k_iterations; ++iteration) {
    const int caller_cpu = sched_getcpu();
    if (caller_cpu < 0) {
      throw std::runtime_error("sched_getcpu failed");
    }

    const WorkerCandidates candidates = select_worker_candidates(topology, caller_cpu);
    const WorkerSelection selection = select_worker_set(candidates, 8);
    const ActiveWorkerSelection active = select_active_workers(master_worker_cpu_ids, selection);
    result_sum += active.worker_indices.size();
  }
  const auto end = std::chrono::steady_clock::now();

  const double elapsed_ns = std::chrono::duration<double, std::nano>(end - begin).count();
  std::cout << "runtime worker selection: ns/call="
            << elapsed_ns / static_cast<double>(k_iterations) << " result_sum=" << result_sum
            << '\n';

  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &original_cpu_set) != 0) {
    throw std::runtime_error("failed to restore caller affinity");
  }
}

#ifdef TLSS_MEMORY_TESTING
void run_runtime_plan_lookup_overhead_benchmark() {
  using TLSS::MEMORY::INTERNAL::MemoryRuntime;

  constexpr std::size_t k_iterations = 1000000;
  MemoryRuntime runtime;

  cpu_set_t original_cpu_set;
  CPU_ZERO(&original_cpu_set);
  if (pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &original_cpu_set) != 0) {
    throw std::runtime_error("failed to get caller affinity");
  }

  struct AffinityRestore {
    const cpu_set_t& original;
    ~AffinityRestore() noexcept {
      pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &original);
    }
  } restore{original_cpu_set};

  cpu_set_t caller_cpu_set;
  CPU_ZERO(&caller_cpu_set);
  CPU_SET(8, &caller_cpu_set);
  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &caller_cpu_set) != 0) {
    throw std::runtime_error("failed to pin caller to CPU8");
  }

  std::size_t result_sum = 0;
  const auto begin = std::chrono::steady_clock::now();
  for (std::size_t iteration = 0; iteration < k_iterations; ++iteration) {
    const int caller_cpu = sched_getcpu();
    const auto* active = runtime.active_worker_indices_for_test(caller_cpu, 8);
    if (active == nullptr) {
      throw std::runtime_error("runtime plan lookup failed");
    }
    result_sum += active->size();
  }
  const auto end = std::chrono::steady_clock::now();

  const double elapsed_ns = std::chrono::duration<double, std::nano>(end - begin).count();
  std::cout << "runtime plan lookup: ns/call="
            << elapsed_ns / static_cast<double>(k_iterations) << " result_sum=" << result_sum
            << '\n';

  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &original_cpu_set) != 0) {
    throw std::runtime_error("failed to restore caller affinity");
  }
}

void run_runtime_fast_path_overhead_benchmark() {
  using TLSS::MEMORY::INTERNAL::MemoryRuntime;

  constexpr std::size_t k_iterations = 10000000;
  MemoryRuntime runtime;

  cpu_set_t original_cpu_set;
  CPU_ZERO(&original_cpu_set);
  if (pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &original_cpu_set) != 0) {
    throw std::runtime_error("failed to get caller affinity");
  }

  struct AffinityRestore {
    const cpu_set_t& original;
    ~AffinityRestore() noexcept {
      pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &original);
    }
  } restore{original_cpu_set};

  cpu_set_t caller_cpu_set;
  CPU_ZERO(&caller_cpu_set);
  CPU_SET(8, &caller_cpu_set);
  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &caller_cpu_set) != 0) {
    throw std::runtime_error("failed to pin caller to CPU8");
  }

  // Match the uncontended single-flight acquire/release in execute_parallel_copy().
  std::atomic_flag busy = ATOMIC_FLAG_INIT;
  std::size_t result_sum = 0;
  const auto begin = std::chrono::steady_clock::now();
  for (std::size_t iteration = 0; iteration < k_iterations; ++iteration) {
    const int caller_cpu = sched_getcpu();
    const auto* active = runtime.active_worker_indices_for_test(caller_cpu, 8);
    if (active == nullptr || active->empty()) {
      throw std::runtime_error("runtime fast path plan lookup failed");
    }
    if (busy.test_and_set(std::memory_order_acquire)) {
      throw std::runtime_error("runtime fast path busy flag was already set");
    }
    result_sum += active->size();
    busy.clear(std::memory_order_release);
  }
  const auto end = std::chrono::steady_clock::now();

  const double elapsed_ns = std::chrono::duration<double, std::nano>(end - begin).count();
  std::cout << "runtime fast path: ns/call="
            << elapsed_ns / static_cast<double>(k_iterations) << " result_sum=" << result_sum
            << '\n';

  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &original_cpu_set) != 0) {
    throw std::runtime_error("failed to restore caller affinity");
  }
}
#endif

void run_cpu_capabilities_test() {
  using TLSS::MEMORY::INTERNAL::detect_cpu_capabilities;

  const auto capabilities = detect_cpu_capabilities();

  std::cout << "CPU capabilities\n"
            << "avx hardware   = " << capabilities.avx_hardware << '\n'
            << "osxsave        = " << capabilities.osxsave << '\n'
            << "avx usable     = " << capabilities.avx_usable << '\n'
            << "avx2 hardware  = " << capabilities.avx2_hardware << '\n'
            << "avx2 usable    = " << capabilities.avx2_usable << '\n'
            << "erms           = " << capabilities.erms << '\n';
}

void run_available_cpu_test() {
  using TLSS::MEMORY::INTERNAL::detect_available_cpu_ids;

  const auto cpu_ids = detect_available_cpu_ids();

  std::cout << "available CPUs:";

  for (int cpu_id : cpu_ids) {
    std::cout << ' ' << cpu_id;
  }

  std::cout << '\n' << "count = " << cpu_ids.size() << '\n';
}

void run_cpu_topology_test() {
  using TLSS::MEMORY::INTERNAL::detect_available_cpu_ids;

  using TLSS::MEMORY::INTERNAL::detect_cpu_topology;

  const auto available_cpu_ids = detect_available_cpu_ids();

  const auto topology = detect_cpu_topology(available_cpu_ids);

  std::cout << "CPU topology\n";

  for (const auto& cpu : topology.cpus) {
    std::cout << "cpu=" << cpu.cpu_id << " package=" << cpu.package_id << " core=" << cpu.core_id
              << " known=" << cpu.topology_known << '\n';
  }

  std::cout << "\nPhysical cores\n";

  for (const auto& core : topology.physical_cores) {
    std::cout << "package=" << core.package_id << " core=" << core.core_id << " logical_cpus=";

    for (int cpu_id : core.logical_cpu_ids) {
      std::cout << cpu_id << ' ';
    }

    std::cout << '\n';
  }
}

void run_worker_candidate_test() {
  using namespace TLSS::MEMORY::INTERNAL;

  //
  // 1. 先取得完整 allowed CPU set
  // （允许 CPU 集合）。
  //
  const auto available_cpu_ids = detect_available_cpu_ids();

  const auto topology = detect_cpu_topology(available_cpu_ids);

  //
  // 2. 测试专用：
  // 再把当前 caller 固定到 CPU8。
  //
  constexpr int k_test_caller_cpu = 8;

  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);
  CPU_SET(k_test_caller_cpu, &cpu_set);

  const int affinity_result = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set);

  if (affinity_result != 0) {
    throw std::runtime_error("failed to pin caller thread");
  }

  //
  // 3. 现在查询 caller 实际所在 CPU。
  //
  const int caller_cpu = sched_getcpu();

  if (caller_cpu < 0) {
    throw std::runtime_error("sched_getcpu failed");
  }

  std::cout << "physical cores before selection:\n";

  for (const auto& core : topology.physical_cores) {
    std::cout << "package=" << core.package_id << " core=" << core.core_id
              << " type=" << cpu_core_type_name(core.core_type) << " logical_cpus=";

    for (int cpu_id : core.logical_cpu_ids) {
      std::cout << cpu_id << ' ';
    }

    std::cout << '\n';
  }

  //
  // 注意：
  // 这里仍然使用之前保存的完整 topology。
  const auto candidates = select_worker_candidates(topology, caller_cpu);

  std::cout << "caller cpu = " << caller_cpu << '\n';

  std::cout << "worker candidates:";

  for (int cpu_id : candidates.intel_core_cpu_ids) {
    std::cout << ' ' << cpu_id;
  }

  std::cout << '\n';
}

void run_cpu_core_type_test() {
  using TLSS::MEMORY::INTERNAL::cpu_core_type_name;

  using TLSS::MEMORY::INTERNAL::detect_available_cpu_ids;

  using TLSS::MEMORY::INTERNAL::detect_cpu_core_type;

  const auto cpu_ids = detect_available_cpu_ids();

  for (int cpu_id : cpu_ids) {
    const auto core_type = detect_cpu_core_type(cpu_id);

    std::cout << "cpu=" << cpu_id << " type=" << cpu_core_type_name(core_type) << '\n';
  }
}

void run_worker_candidates_test() {
  using TLSS::MEMORY::INTERNAL::CpuTopology;
  using TLSS::MEMORY::INTERNAL::detect_available_cpu_ids;
  using TLSS::MEMORY::INTERNAL::detect_cpu_topology;
  using TLSS::MEMORY::INTERNAL::select_worker_candidates;
  using TLSS::MEMORY::INTERNAL::WorkerCandidates;

  //
  // 1. 先获取完整的 available CPUs（可用 CPU 集合）
  //
  const auto available_cpu_ids = detect_available_cpu_ids();

  //
  // 2. 基于完整集合构造 CPU topology（CPU 拓扑）
  //
  const CpuTopology topology = detect_cpu_topology(available_cpu_ids);

  //
  // 3. 测试时把 caller（调用线程）固定到 CPU8。
  //
  constexpr int k_test_caller_cpu = 8;

  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);
  CPU_SET(k_test_caller_cpu, &cpu_set);

  const int affinity_result = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set);

  if (affinity_result != 0) {
    throw std::runtime_error("failed to pin caller thread");
  }

  //
  // 4. 确认真正运行在哪个 CPU。
  //
  const int caller_cpu = sched_getcpu();

  if (caller_cpu < 0) {
    throw std::runtime_error("sched_getcpu failed");
  }
  std::cout << "physical cores before selection:\n";

  for (const auto& core : topology.physical_cores) {
    std::cout << "package=" << core.package_id << " core=" << core.core_id
              << " type=" << cpu_core_type_name(core.core_type) << " logical_cpus=";

    for (int cpu_id : core.logical_cpu_ids) {
      std::cout << cpu_id << ' ';
    }

    std::cout << '\n';
  }

  //
  // 5. 生成 worker candidates（工作线程候选）。
  //
  const WorkerCandidates candidates = select_worker_candidates(topology, caller_cpu);

  std::cout << "caller cpu = " << caller_cpu << '\n';

  std::cout << "IntelCore candidates:";

  for (int cpu_id : candidates.intel_core_cpu_ids) {
    std::cout << ' ' << cpu_id;
  }

  std::cout << '\n';

  std::cout << "IntelAtom candidates:";

  for (int cpu_id : candidates.intel_atom_cpu_ids) {
    std::cout << ' ' << cpu_id;
  }

  std::cout << '\n';

  std::cout << "Unknown candidates:";

  for (int cpu_id : candidates.unknown_cpu_ids) {
    std::cout << ' ' << cpu_id;
  }

  std::cout << '\n';

  //
  // 6. correctness check（正确性检查）
  //
  //
  // CPU8 和 CPU9 属于同一个 physical core（物理核心），
  // 所以二者都不能进入候选列表。
  //
  const auto contains_cpu = [](const std::vector<int>& cpu_ids, int target_cpu) {
    return std::find(cpu_ids.begin(), cpu_ids.end(), target_cpu) != cpu_ids.end();
  };

  const bool caller_core_excluded = !contains_cpu(candidates.intel_core_cpu_ids, 8) &&
                                    !contains_cpu(candidates.intel_core_cpu_ids, 9) &&
                                    !contains_cpu(candidates.intel_atom_cpu_ids, 8) &&
                                    !contains_cpu(candidates.intel_atom_cpu_ids, 9) &&
                                    !contains_cpu(candidates.unknown_cpu_ids, 8) &&
                                    !contains_cpu(candidates.unknown_cpu_ids, 9);

  if (!caller_core_excluded) {
    throw std::runtime_error("caller physical core was not excluded");
  }

  std::cout << "Worker candidates test: PASS\n";
}

void run_dynamic_worker_benchmark(std::size_t size_kib, const std::vector<int>& worker_cpu_ids) {
  using TLSS::MEMORY::INTERNAL::parallel_nt_copy;
  using TLSS::MEMORY::INTERNAL::ParallelCopyPool;

  constexpr std::size_t k_working_set = 2ULL * 1024 * 1024 * 1024;

  constexpr std::size_t k_alignment = 4096;

  const std::size_t copy_size = size_kib * 1024ULL;

  if (worker_cpu_ids.empty()) {
    throw std::invalid_argument("worker_cpu_ids must not be empty");
  }

  if (copy_size == 0 || k_working_set % copy_size != 0) {
    throw std::invalid_argument("invalid copy size");
  }

  //
  // 先保存 caller（调用线程）的原始 affinity（亲和性）。
  //
  cpu_set_t original_cpu_set;
  CPU_ZERO(&original_cpu_set);

  if (pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &original_cpu_set) != 0) {
    throw std::runtime_error("failed to get caller affinity");
  }

  //
  // 注意：
  // pool 要在 caller 被 pin（绑定）到 CPU8 之前创建。
  //
  ParallelCopyPool pool(worker_cpu_ids.size(), worker_cpu_ids.data());

  //
  // caller 固定到 CPU8。
  //
  constexpr int k_caller_cpu = 8;

  cpu_set_t caller_cpu_set;
  CPU_ZERO(&caller_cpu_set);
  CPU_SET(k_caller_cpu, &caller_cpu_set);

  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &caller_cpu_set) != 0) {
    throw std::runtime_error("failed to pin caller");
  }

  AlignedBuffer src_buffer(k_working_set, k_alignment);

  AlignedBuffer dst_buffer(k_working_set, k_alignment);

  auto* src = src_buffer.data();
  auto* dst = dst_buffer.data();

  //
  // 初始化 source（源数据）。
  //
  for (std::size_t offset = 0; offset < k_working_set; offset += sizeof(std::uint64_t)) {
    const std::uint64_t value = static_cast<std::uint64_t>(offset * 1315423911ULL);

    std::memcpy(src + offset, &value, sizeof(value));
  }

  std::memset(dst, 0, k_working_set);

  const std::size_t iteration_count = k_working_set / copy_size;

  //
  // warm-up（预热）
  //
  /* parallel_nt_copy(pool, dst, src, copy_size); */

  std::memset(dst, 0, k_working_set);

  const auto begin = std::chrono::steady_clock::now();

  for (std::size_t iteration = 0; iteration < iteration_count; ++iteration) {
    const std::size_t offset = iteration * copy_size;

    /* parallel_nt_copy(pool, dst + offset, src + offset, copy_size); */
  }

  const auto end = std::chrono::steady_clock::now();

  if (std::memcmp(dst, src, k_working_set) != 0) {
    throw std::runtime_error("dynamic worker benchmark correctness failed");
  }

  const double elapsed_seconds = std::chrono::duration<double>(end - begin).count();

  constexpr double k_total_gib = 2.0;

  std::cout << "workers=";

  for (int cpu_id : worker_cpu_ids) {
    std::cout << cpu_id << ',';
  }

  std::cout << " size=" << size_kib << " KiB" << " bandwidth=" << k_total_gib / elapsed_seconds
            << " GiB/s\n";

  //
  // 恢复 caller 原 affinity（亲和性）。
  //
  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &original_cpu_set) != 0) {
    throw std::runtime_error("failed to restore caller affinity");
  }
}

void run_dynamic_worker_matrix(std::size_t size_kib) {
  const std::vector<int> k_workers_4p = {0, 2, 4, 6};

  const std::vector<int> k_workers_7p = {0, 2, 4, 6, 10, 12, 14};

  const std::vector<int> k_workers_7p_1e = {0, 2, 4, 6, 10, 12, 14, 16};

  run_dynamic_worker_benchmark(size_kib, k_workers_4p);

  run_dynamic_worker_benchmark(size_kib, k_workers_7p);

  run_dynamic_worker_benchmark(size_kib, k_workers_7p_1e);
}

void run_worker_selection_test() {
  using namespace TLSS::MEMORY::INTERNAL;

  WorkerCandidates candidates;

  candidates.intel_core_cpu_ids = {0, 2, 4, 6, 10, 12, 14};

  candidates.intel_atom_cpu_ids = {16, 17, 18, 19};

  //
  // desired = 8
  // available P-core = 7
  //
  {
    const WorkerSelection selection = select_worker_set(candidates, 8);

    const std::vector<int> expected = {0, 2, 4, 6, 10, 12, 14};

    if (selection.cpu_ids != expected) {
      throw std::runtime_error("desired 8 worker selection failed");
    }
  }

  //
  // desired = 4
  //
  {
    const WorkerSelection selection = select_worker_set(candidates, 4);

    const std::vector<int> expected = {0, 2, 4, 6};

    if (selection.cpu_ids != expected) {
      throw std::runtime_error("desired 4 worker selection failed");
    }
  }

  //
  // only 3 P-cores available
  //
  {
    WorkerCandidates limited_candidates;

    limited_candidates.intel_core_cpu_ids = {2, 6, 10};

    limited_candidates.intel_atom_cpu_ids = {16, 17, 18, 19};

    const WorkerSelection selection = select_worker_set(limited_candidates, 4);

    const std::vector<int> expected = {2, 6, 10};

    if (selection.cpu_ids != expected) {
      throw std::runtime_error("limited worker selection failed");
    }
  }

  //
  // no P-core available
  //
  {
    WorkerCandidates atom_only_candidates;

    atom_only_candidates.intel_atom_cpu_ids = {16, 17, 18};

    const WorkerSelection selection = select_worker_set(atom_only_candidates, 8);

    if (!selection.cpu_ids.empty()) {
      throw std::runtime_error("Atom fallback must not be used");
    }
  }

  //
  // desired = 0
  //
  {
    const WorkerSelection selection = select_worker_set(candidates, 0);

    if (!selection.cpu_ids.empty()) {
      throw std::runtime_error("zero worker selection failed");
    }
  }

  std::cout << "Worker selection test: PASS\n";
}

void run_sparse_active_worker_copy_test() {
  using TLSS::MEMORY::INTERNAL::parallel_nt_copy;
  using TLSS::MEMORY::INTERNAL::ParallelCopyPool;

  //
  // ------------------------------------------------------------
  // 1. 固定 8 个 persistent workers（常驻工作线程）
  // ------------------------------------------------------------
  //
  const std::vector<int> worker_cpu_ids = {0, 2, 4, 6, 8, 10, 12, 14};

  ParallelCopyPool pool(worker_cpu_ids.size(), worker_cpu_ids.data());

  //
  // ------------------------------------------------------------
  // 2. 模拟 caller（调用线程）运行在 CPU8
  //
  // CPU8 对应 pool slot（线程池槽位）4，
  // 所以 worker4 不参与本次复制。
  // ------------------------------------------------------------
  //
  const std::vector<std::size_t> active_worker_indices = {0, 1, 2, 3, 5, 6, 7};

  constexpr std::size_t k_copy_size = 1024ULL * 1024ULL;

  constexpr std::size_t k_alignment = 64;

  //
  // ------------------------------------------------------------
  // 3. 分配 src / dst
  // ------------------------------------------------------------
  //
  void* src_raw = nullptr;
  void* dst_raw = nullptr;

  if (posix_memalign(&src_raw, k_alignment, k_copy_size) != 0) {
    throw std::runtime_error("failed to allocate src");
  }

  if (posix_memalign(&dst_raw, k_alignment, k_copy_size) != 0) {
    std::free(src_raw);

    throw std::runtime_error("failed to allocate dst");
  }

  auto* src = static_cast<std::uint8_t*>(src_raw);

  auto* dst = static_cast<std::uint8_t*>(dst_raw);

  //
  // ------------------------------------------------------------
  // 4. 初始化测试数据
  // ------------------------------------------------------------
  //
  for (std::size_t index = 0; index < k_copy_size; ++index) {
    src[index] = static_cast<std::uint8_t>((index * 131U + 17U) & 0xFFU);
  }

  //
  // dst 初始化成完全不同的内容。
  //
  std::memset(dst, 0xA5, k_copy_size);

  //
  // ------------------------------------------------------------
  // 5. 执行 sparse active worker copy
  //    （稀疏活动工作线程复制）
  // ------------------------------------------------------------
  //
  parallel_nt_copy(pool, active_worker_indices, dst, src, k_copy_size);

  // Seven NT blocks are enough for seven active workers, even with eight
  // persistent workers in the pool.
  constexpr std::size_t k_min_sparse_copy_size = 7ULL * 8192ULL;
  std::memset(dst, 0xA5, k_min_sparse_copy_size);
  parallel_nt_copy(pool, active_worker_indices, dst, src, k_min_sparse_copy_size);

  // An inactive worker must still accept a later generation when selected.
  const std::vector<std::size_t> next_active_workers = {0, 1, 2, 3, 4, 5, 6};
  std::memset(dst, 0xA5, k_min_sparse_copy_size);
  parallel_nt_copy(pool, next_active_workers, dst, src, k_min_sparse_copy_size);

  std::memset(dst, 0xA5, k_min_sparse_copy_size);
  parallel_nt_copy(pool, active_worker_indices, dst, src, k_min_sparse_copy_size);

  //
  // ------------------------------------------------------------
  // 6. correctness check（正确性检查）
  // ------------------------------------------------------------
  //
  if (std::memcmp(dst, src, k_copy_size) != 0) {
    std::free(src_raw);
    std::free(dst_raw);

    throw std::runtime_error("sparse active worker copy mismatch");
  }

  std::free(src_raw);
  std::free(dst_raw);

  std::cout << "pool worker count = " << worker_cpu_ids.size() << '\n';

  std::cout << "active workers =";

  for (std::size_t worker_index : active_worker_indices) {
    std::cout << ' ' << worker_index;
  }

  std::cout << '\n';

  std::cout << "Sparse active worker copy: PASS\n";
}

void run_sparse_worker_benchmark(std::size_t size_kib) {
  using TLSS::MEMORY::INTERNAL::parallel_nt_copy;
  using TLSS::MEMORY::INTERNAL::ParallelCopyPool;

  const std::vector<int> worker_cpu_ids = {0, 2, 4, 6, 8, 10, 12, 14};

  const std::vector<std::size_t> active_worker_indices = {0, 1, 2, 3, 5, 6, 7};

  ParallelCopyPool pool(worker_cpu_ids.size(), worker_cpu_ids.data());

  constexpr std::size_t k_working_set = 2ULL * 1024 * 1024 * 1024;

  constexpr std::size_t k_alignment = 4096;

  const std::size_t copy_size = size_kib * 1024ULL;

  if (copy_size == 0 || k_working_set % copy_size != 0) {
    throw std::invalid_argument("invalid copy size");
  }

  //
  // caller 固定到 CPU8
  //
  cpu_set_t original_cpu_set;
  CPU_ZERO(&original_cpu_set);

  if (pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &original_cpu_set) != 0) {
    throw std::runtime_error("failed to get caller affinity");
  }

  cpu_set_t caller_cpu_set;
  CPU_ZERO(&caller_cpu_set);
  CPU_SET(8, &caller_cpu_set);

  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &caller_cpu_set) != 0) {
    throw std::runtime_error("failed to pin caller");
  }

  AlignedBuffer src_buffer(k_working_set, k_alignment);

  AlignedBuffer dst_buffer(k_working_set, k_alignment);

  auto* src = src_buffer.data();
  auto* dst = dst_buffer.data();

  for (std::size_t offset = 0; offset < k_working_set; offset += sizeof(std::uint64_t)) {
    const std::uint64_t value = static_cast<std::uint64_t>(offset * 1315423911ULL);

    std::memcpy(src + offset, &value, sizeof(value));
  }

  std::memset(dst, 0, k_working_set);

  const std::size_t iteration_count = k_working_set / copy_size;

  //
  // warm-up（预热）
  //
  parallel_nt_copy(pool, active_worker_indices, dst, src, copy_size);

  std::memset(dst, 0, k_working_set);

  const auto begin = std::chrono::steady_clock::now();

  for (std::size_t iteration = 0; iteration < iteration_count; ++iteration) {
    const std::size_t offset = iteration * copy_size;

    parallel_nt_copy(pool, active_worker_indices, dst + offset, src + offset, copy_size);
  }

  const auto end = std::chrono::steady_clock::now();

  if (std::memcmp(dst, src, k_working_set) != 0) {
    throw std::runtime_error("sparse worker benchmark mismatch");
  }

  const double elapsed_seconds = std::chrono::duration<double>(end - begin).count();

  constexpr double k_total_gib = 2.0;

  std::cout << "sparse7p" << " size=" << size_kib << " KiB"
            << " bandwidth=" << k_total_gib / elapsed_seconds << " GiB/s\n";

  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &original_cpu_set) != 0) {
    throw std::runtime_error("failed to restore caller affinity");
  }
}

void run_sparse_worker_stress_test() {
  using TLSS::MEMORY::INTERNAL::parallel_nt_copy;
  using TLSS::MEMORY::INTERNAL::ParallelCopyPool;

  const std::vector<int> worker_cpu_ids = {0, 2, 4, 6, 8, 10, 12, 14};

  ParallelCopyPool pool(worker_cpu_ids.size(), worker_cpu_ids.data());

  const std::vector<std::vector<std::size_t>> active_worker_sets = {

      {0, 1, 2, 3, 5, 6, 7},

      {0, 1, 3, 4, 5, 6, 7},

      {1, 2, 3, 4, 5, 6, 7},

      {0, 1, 2, 3, 4, 5, 6, 7},

      {0, 2, 4, 6},

      {1, 3, 5, 7}};

  constexpr std::size_t k_copy_size = 1024ULL * 1024ULL;

  constexpr std::size_t k_iterations = 10000;

  constexpr std::size_t k_alignment = 64;

  void* src_raw = nullptr;
  void* dst_raw = nullptr;

  if (posix_memalign(&src_raw, k_alignment, k_copy_size) != 0) {
    throw std::runtime_error("failed to allocate src");
  }

  if (posix_memalign(&dst_raw, k_alignment, k_copy_size) != 0) {
    std::free(src_raw);

    throw std::runtime_error("failed to allocate dst");
  }

  auto* src = static_cast<std::uint8_t*>(src_raw);

  auto* dst = static_cast<std::uint8_t*>(dst_raw);

  for (std::size_t index = 0; index < k_copy_size; ++index) {
    src[index] = static_cast<std::uint8_t>((index * 131U + 17U) & 0xFFU);
  }

  for (std::size_t iteration = 0; iteration < k_iterations; ++iteration) {
    std::memset(dst, 0xA5, k_copy_size);

    const auto& active_workers = active_worker_sets[iteration % active_worker_sets.size()];

    parallel_nt_copy(pool, active_workers, dst, src, k_copy_size);

    if (std::memcmp(dst, src, k_copy_size) != 0) {
      std::free(src_raw);
      std::free(dst_raw);

      throw std::runtime_error("sparse worker stress mismatch");
    }
  }

  std::free(src_raw);
  std::free(dst_raw);

  std::cout << "Sparse worker stress: PASS" << " iterations=" << k_iterations << '\n';
}

void run_master_worker_selection_test() {
  using TLSS::MEMORY::INTERNAL::CpuTopology;
  using TLSS::MEMORY::INTERNAL::detect_available_cpu_ids;
  using TLSS::MEMORY::INTERNAL::detect_cpu_topology;
  using TLSS::MEMORY::INTERNAL::select_master_worker_cpu_ids;

  const auto available_cpu_ids = detect_available_cpu_ids();
  const CpuTopology topology = detect_cpu_topology(available_cpu_ids);
  const std::vector<int> master_worker_cpu_ids = select_master_worker_cpu_ids(topology);
  std::cout << "Master worker CPUs:";

  for (int cpu_id : master_worker_cpu_ids) {
    std::cout << ' ' << cpu_id;
  }

  std::cout << '\n';

  if (master_worker_cpu_ids.empty()) {
    throw std::runtime_error("master worker set is empty");
  }

  //
  // 确保一个 physical core（物理核心）
  // 没有选择两个 SMT siblings（SMT 兄弟线程）。
  //
  for (std::size_t i = 0; i < master_worker_cpu_ids.size(); ++i) {
    for (std::size_t j = i + 1; j < master_worker_cpu_ids.size(); ++j) {
      const auto* core_i = find_physical_core(topology, master_worker_cpu_ids[i]);

      const auto* core_j = find_physical_core(topology, master_worker_cpu_ids[j]);

      if (core_i != nullptr && core_j != nullptr && core_i->package_id == core_j->package_id &&
          core_i->core_id == core_j->core_id) {
        throw std::runtime_error("master workers contain SMT siblings");
      }
    }
  }

  std::cout << "Master worker selection test: PASS\n";
}

#ifdef TLSS_MEMORY_TESTING
void run_memory_runtime_topology_test() {
  TLSS::MEMORY::INTERNAL::MemoryRuntime runtime;
  const auto& cpu_ids = runtime.master_worker_cpu_ids();

  std::cout << "MemoryRuntime master workers:";
  for (int cpu_id : cpu_ids) {
    std::cout << ' ' << cpu_id;
  }
  std::cout << '\n';

  const auto available_cpu_ids = TLSS::MEMORY::INTERNAL::detect_available_cpu_ids();
  const auto topology = TLSS::MEMORY::INTERNAL::detect_cpu_topology(available_cpu_ids);
  if (cpu_ids.empty()) {
    for (const auto& core : topology.physical_cores) {
      if (core.core_type == TLSS::MEMORY::INTERNAL::CpuCoreType::IntelCore &&
          !core.logical_cpu_ids.empty()) {
        throw std::runtime_error("MemoryRuntime omitted available P-cores");
      }
    }
  }

  for (int cpu_id : cpu_ids) {
    if (std::find(available_cpu_ids.begin(), available_cpu_ids.end(), cpu_id) ==
        available_cpu_ids.end()) {
      throw std::runtime_error("MemoryRuntime selected an unavailable CPU");
    }
  }

  std::cout << "MemoryRuntime topology test: PASS\n";
}

void run_lazy_runtime_test() {
  using TLSS::MEMORY::CopyBackend;
  using TLSS::MEMORY::CopyHint;
  using TLSS::MEMORY::INTERNAL::memory_runtime_construction_count_for_test;

  constexpr std::size_t k_size = 4096;
  AlignedBuffer src_buffer(k_size, 64);
  AlignedBuffer dst_buffer(k_size, 64);
  auto* src = src_buffer.data();
  auto* dst = dst_buffer.data();
  for (std::size_t index = 0; index < k_size; ++index) {
    src[index] = static_cast<std::uint8_t>(index & 0xffU);
  }

  const std::size_t before = memory_runtime_construction_count_for_test();
  void* result = TLSS::MEMORY::memcpy(dst, src, k_size, CopyBackend::RepMovsb,
                                      CopyHint::Default);
  const std::size_t after = memory_runtime_construction_count_for_test();
  if (result != dst || std::memcmp(dst, src, k_size) != 0) {
    throw std::runtime_error("lazy runtime copy result mismatch");
  }

  std::cout << "runtime constructions before=" << before << " after=" << after << '\n';
  std::cout << "Lazy runtime test: PASS\n";
}

void run_lazy_runtime_parallel_test() {
  using TLSS::MEMORY::CopyBackend;
  using TLSS::MEMORY::CopyHint;
  using TLSS::MEMORY::INTERNAL::CopyStrategy;
  using TLSS::MEMORY::INTERNAL::memory_runtime_construction_count_for_test;

  constexpr std::size_t k_size = 256ULL * 1024ULL;
  AlignedBuffer src_buffer(k_size, 64);
  AlignedBuffer dst_buffer(k_size, 64);
  auto* src = src_buffer.data();
  auto* dst = dst_buffer.data();

  for (std::size_t index = 0; index < k_size; ++index) {
    src[index] = static_cast<std::uint8_t>((index * 131U + 17U) & 0xffU);
  }

  const auto plan = TLSS::MEMORY::INTERNAL::make_copy_plan_for_backend(
      k_size, CopyHint::Streaming, CopyBackend::Auto,
      TLSS::MEMORY::INTERNAL::get_cpu_capabilities());
  if (plan.strategy != CopyStrategy::ParallelNt) {
    throw std::runtime_error("lazy runtime test requires a ParallelNt plan");
  }

  const std::size_t before = memory_runtime_construction_count_for_test();
  if (before != 0) {
    throw std::runtime_error("runtime was constructed before the first parallel copy");
  }

  void* first_result = TLSS::MEMORY::memcpy(dst, src, k_size, CopyBackend::Auto,
                                             CopyHint::Streaming);
  const std::size_t after_first = memory_runtime_construction_count_for_test();
  if (first_result != dst || std::memcmp(dst, src, k_size) != 0) {
    throw std::runtime_error("first parallel copy mismatch");
  }

  std::memset(dst, 0, k_size);
  void* second_result = TLSS::MEMORY::memcpy(dst, src, k_size, CopyBackend::Auto,
                                              CopyHint::Streaming);
  const std::size_t after_second = memory_runtime_construction_count_for_test();
  if (second_result != dst || std::memcmp(dst, src, k_size) != 0) {
    throw std::runtime_error("second parallel copy mismatch");
  }

  std::cout << "runtime constructions: before=" << before << " first=" << after_first
            << " second=" << after_second << '\n';

  if (after_first != 1) {
    throw std::runtime_error("runtime was not lazily constructed exactly once");
  }
  if (after_second != after_first) {
    throw std::runtime_error("runtime constructed more than once");
  }

  std::cout << "Lazy runtime parallel test: PASS\n";
}

void run_concurrent_first_use_test() {
  using TLSS::MEMORY::CopyBackend;
  using TLSS::MEMORY::CopyHint;
  using TLSS::MEMORY::INTERNAL::CopyStrategy;
  using TLSS::MEMORY::INTERNAL::memory_runtime_construction_count_for_test;

  constexpr std::size_t k_thread_count = 8;
  constexpr std::size_t k_copy_size = 256ULL * 1024ULL;

  const auto plan = TLSS::MEMORY::INTERNAL::make_copy_plan_for_backend(
      k_copy_size, CopyHint::Streaming, CopyBackend::Auto,
      TLSS::MEMORY::INTERNAL::get_cpu_capabilities());
  if (plan.strategy != CopyStrategy::ParallelNt) {
    throw std::runtime_error("concurrent first-use test requires a ParallelNt plan");
  }

  std::vector<std::vector<std::uint8_t>> sources(
      k_thread_count, std::vector<std::uint8_t>(k_copy_size));
  std::vector<std::vector<std::uint8_t>> destinations(
      k_thread_count, std::vector<std::uint8_t>(k_copy_size));
  for (std::size_t thread_index = 0; thread_index < k_thread_count; ++thread_index) {
    for (std::size_t index = 0; index < k_copy_size; ++index) {
      sources[thread_index][index] =
          static_cast<std::uint8_t>((index + thread_index * 37U) & 0xffU);
    }
  }

  std::atomic<std::size_t> ready_count{0};
  std::atomic<bool> start{false};
  std::atomic<std::size_t> failure_count{0};

  const std::size_t before = memory_runtime_construction_count_for_test();
  if (before != 0) {
    throw std::runtime_error("runtime was constructed before concurrent first use");
  }

  std::vector<std::thread> threads;
  threads.reserve(k_thread_count);
  try {
    for (std::size_t thread_index = 0; thread_index < k_thread_count; ++thread_index) {
      threads.emplace_back([&, thread_index]() {
        ready_count.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
          _mm_pause();
        }

        try {
          auto& dst = destinations[thread_index];
          const auto& src = sources[thread_index];
          void* result = TLSS::MEMORY::memcpy(dst.data(), src.data(), k_copy_size,
                                               CopyBackend::Auto, CopyHint::Streaming);
          if (result != dst.data() || std::memcmp(dst.data(), src.data(), k_copy_size) != 0) {
            failure_count.fetch_add(1, std::memory_order_relaxed);
          }
        } catch (...) {
          failure_count.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }
  } catch (...) {
    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
      thread.join();
    }
    throw;
  }

  while (ready_count.load(std::memory_order_acquire) != k_thread_count) {
    _mm_pause();
  }
  start.store(true, std::memory_order_release);

  for (std::thread& thread : threads) {
    thread.join();
  }

  const std::size_t after = memory_runtime_construction_count_for_test();
  const std::size_t failures = failure_count.load(std::memory_order_relaxed);
  std::cout << "runtime constructions: before=" << before << " after=" << after << '\n';
  std::cout << "failures=" << failures << '\n';

  if (after != 1) {
    throw std::runtime_error("MemoryRuntime was not constructed exactly once");
  }
  if (failures != 0) {
    throw std::runtime_error("concurrent first-use copy failed");
  }

  std::cout << "Concurrent first use test: PASS\n";
}

void run_concurrent_construction_failure_test() {
  using TLSS::MEMORY::CopyBackend;
  using TLSS::MEMORY::CopyHint;
  using TLSS::MEMORY::INTERNAL::CopyStrategy;
  using TLSS::MEMORY::INTERNAL::memory_runtime_construction_attempt_count_for_test;
  using TLSS::MEMORY::INTERNAL::memory_runtime_construction_count_for_test;
  using TLSS::MEMORY::INTERNAL::set_memory_runtime_construction_failure_for_test;

  constexpr std::size_t k_thread_count = 8;
  constexpr std::size_t k_copy_size = 256ULL * 1024ULL;

  const auto plan = TLSS::MEMORY::INTERNAL::make_copy_plan_for_backend(
      k_copy_size, CopyHint::Streaming, CopyBackend::Auto,
      TLSS::MEMORY::INTERNAL::get_cpu_capabilities());
  if (plan.strategy != CopyStrategy::ParallelNt) {
    throw std::runtime_error("concurrent failure test requires a ParallelNt plan");
  }

  std::vector<std::vector<std::uint8_t>> sources(
      k_thread_count, std::vector<std::uint8_t>(k_copy_size));
  std::vector<std::vector<std::uint8_t>> destinations(
      k_thread_count, std::vector<std::uint8_t>(k_copy_size));
  for (std::size_t thread_index = 0; thread_index < k_thread_count; ++thread_index) {
    for (std::size_t index = 0; index < k_copy_size; ++index) {
      sources[thread_index][index] =
          static_cast<std::uint8_t>((index + thread_index * 37U) & 0xffU);
    }
  }

  const std::size_t attempts_before = memory_runtime_construction_attempt_count_for_test();
  const std::size_t successes_before = memory_runtime_construction_count_for_test();
  if (attempts_before != 0 || successes_before != 0) {
    throw std::runtime_error("runtime was constructed before concurrent failure test");
  }

  std::atomic<std::size_t> ready_count{0};
  std::atomic<bool> start{false};
  std::atomic<std::size_t> failure_count{0};

  set_memory_runtime_construction_failure_for_test(true);
  std::vector<std::thread> threads;
  threads.reserve(k_thread_count);
  try {
    for (std::size_t thread_index = 0; thread_index < k_thread_count; ++thread_index) {
      threads.emplace_back([&, thread_index]() {
        ready_count.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
          _mm_pause();
        }

        try {
          auto& dst = destinations[thread_index];
          const auto& src = sources[thread_index];
          void* result = TLSS::MEMORY::memcpy(dst.data(), src.data(), k_copy_size,
                                               CopyBackend::Auto, CopyHint::Streaming);
          if (result != dst.data() || std::memcmp(dst.data(), src.data(), k_copy_size) != 0) {
            failure_count.fetch_add(1, std::memory_order_relaxed);
          }
        } catch (...) {
          failure_count.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }
  } catch (...) {
    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
      thread.join();
    }
    set_memory_runtime_construction_failure_for_test(false);
    throw;
  }

  while (ready_count.load(std::memory_order_acquire) != k_thread_count) {
    _mm_pause();
  }
  start.store(true, std::memory_order_release);

  for (std::thread& thread : threads) {
    thread.join();
  }
  set_memory_runtime_construction_failure_for_test(false);

  const std::size_t attempts_after_failure =
      memory_runtime_construction_attempt_count_for_test();
  const std::size_t successes_after_failure = memory_runtime_construction_count_for_test();
  const std::size_t failures = failure_count.load(std::memory_order_relaxed);
  if (failures != 0) {
    throw std::runtime_error("concurrent runtime failure leaked to memcpy");
  }
  if (successes_after_failure != successes_before) {
    throw std::runtime_error("runtime unexpectedly constructed successfully");
  }
  if (attempts_after_failure <= attempts_before) {
    throw std::runtime_error("runtime construction was not attempted");
  }

  std::vector<std::uint8_t> recovery_dst(k_copy_size);
  void* recovery_result = TLSS::MEMORY::memcpy(recovery_dst.data(), sources[0].data(),
                                               k_copy_size, CopyBackend::Auto,
                                               CopyHint::Streaming);
  const std::size_t attempts_after_recovery =
      memory_runtime_construction_attempt_count_for_test();
  const std::size_t successes_after_recovery = memory_runtime_construction_count_for_test();
  if (recovery_result != recovery_dst.data() ||
      std::memcmp(recovery_dst.data(), sources[0].data(), k_copy_size) != 0) {
    throw std::runtime_error("concurrent runtime failure recovery copy mismatch");
  }
  if (successes_after_recovery != successes_after_failure + 1) {
    throw std::runtime_error("runtime did not recover after concurrent failure");
  }

  std::cout << "runtime attempts: before=" << attempts_before
            << " after_failure=" << attempts_after_failure
            << " after_recovery=" << attempts_after_recovery << '\n';
  std::cout << "runtime successes: before=" << successes_before
            << " after_failure=" << successes_after_failure
            << " after_recovery=" << successes_after_recovery << '\n';
  std::cout << "copy failures=" << failures << '\n';
  std::cout << "Concurrent construction failure test: PASS\n";
}

void run_runtime_construction_failure_test() {
  using TLSS::MEMORY::CopyBackend;
  using TLSS::MEMORY::CopyHint;
  using TLSS::MEMORY::INTERNAL::CopyStrategy;
  using TLSS::MEMORY::INTERNAL::memory_runtime_construction_attempt_count_for_test;
  using TLSS::MEMORY::INTERNAL::memory_runtime_construction_count_for_test;
  using TLSS::MEMORY::INTERNAL::set_memory_runtime_construction_failure_for_test;

  constexpr std::size_t k_size = 256ULL * 1024ULL;
  std::vector<std::uint8_t> src(k_size);
  std::vector<std::uint8_t> dst(k_size);
  for (std::size_t index = 0; index < k_size; ++index) {
    src[index] = static_cast<std::uint8_t>((index * 131U + 17U) & 0xffU);
  }

  const auto plan = TLSS::MEMORY::INTERNAL::make_copy_plan_for_backend(
      k_size, CopyHint::Streaming, CopyBackend::Auto,
      TLSS::MEMORY::INTERNAL::get_cpu_capabilities());
  if (plan.strategy != CopyStrategy::ParallelNt) {
    throw std::runtime_error("construction failure test requires a ParallelNt plan");
  }

  const std::size_t before = memory_runtime_construction_count_for_test();
  const std::size_t attempts_before = memory_runtime_construction_attempt_count_for_test();
  if (before != 0 || attempts_before != 0) {
    throw std::runtime_error("runtime was constructed before failure injection");
  }

  set_memory_runtime_construction_failure_for_test(true);
  bool threw = false;
  try {
    void* result = TLSS::MEMORY::memcpy(dst.data(), src.data(), k_size,
                                        CopyBackend::Auto, CopyHint::Streaming);
    if (result != dst.data()) {
      throw std::runtime_error("fallback returned the wrong destination");
    }
  } catch (...) {
    threw = true;
  }
  const std::size_t after_failure = memory_runtime_construction_count_for_test();
  const std::size_t attempts_after_failure =
      memory_runtime_construction_attempt_count_for_test();
  set_memory_runtime_construction_failure_for_test(false);

  if (threw) {
    throw std::runtime_error("Auto memcpy leaked runtime construction failure");
  }
  if (std::memcmp(dst.data(), src.data(), k_size) != 0) {
    throw std::runtime_error("runtime construction fallback copy mismatch");
  }
  if (after_failure != 0) {
    throw std::runtime_error("failed runtime construction was counted as successful");
  }
  if (attempts_after_failure != 1) {
    throw std::runtime_error("runtime construction failure was not attempted exactly once");
  }

  std::memset(dst.data(), 0, k_size);
  void* recovery_result = TLSS::MEMORY::memcpy(dst.data(), src.data(), k_size,
                                               CopyBackend::Auto, CopyHint::Streaming);
  const std::size_t after_recovery = memory_runtime_construction_count_for_test();
  const std::size_t attempts_after_recovery =
      memory_runtime_construction_attempt_count_for_test();
  if (recovery_result != dst.data() || std::memcmp(dst.data(), src.data(), k_size) != 0) {
    throw std::runtime_error("runtime recovery copy mismatch");
  }
  if (after_recovery != 1) {
    throw std::runtime_error("runtime was not constructed after failure injection ended");
  }
  if (attempts_after_recovery != 2) {
    throw std::runtime_error("runtime construction was not retried exactly once");
  }

  std::cout << "threw=" << threw << '\n';
  std::cout << "runtime construction attempts: before=" << attempts_before
            << " after_failure=" << attempts_after_failure
            << " after_recovery=" << attempts_after_recovery << '\n';
  std::cout << "runtime construction successes: before=" << before
            << " after_failure=" << after_failure
            << " after_recovery=" << after_recovery << '\n';
  std::cout << "Runtime construction failure test: PASS\n";
}

void run_runtime_lifetime_test() {
  using TLSS::MEMORY::INTERNAL::MemoryRuntime;

  constexpr std::size_t k_iterations = 100;
  constexpr std::size_t k_copy_size = 1024ULL * 1024ULL;
  AlignedBuffer src_buffer(k_copy_size, 64);
  AlignedBuffer dst_buffer(k_copy_size, 64);
  auto* src = src_buffer.data();
  auto* dst = dst_buffer.data();

  for (std::size_t index = 0; index < k_copy_size; ++index) {
    src[index] = static_cast<std::uint8_t>((index * 131U + 17U) & 0xffU);
  }

  for (std::size_t iteration = 0; iteration < k_iterations; ++iteration) {
    std::memset(dst, 0, k_copy_size);
    {
      MemoryRuntime runtime;
      const auto* active_workers = runtime.active_worker_indices_for_test(sched_getcpu(), 8);
      if (active_workers == nullptr || active_workers->size() < 2) {
        throw std::runtime_error("runtime lifetime test requires parallel workers");
      }

      void* result = runtime.execute_parallel_copy(8, dst, src, k_copy_size);
      if (result != dst || std::memcmp(dst, src, k_copy_size) != 0) {
        throw std::runtime_error("runtime lifetime copy mismatch");
      }
    }
  }

  std::cout << "Runtime lifetime test: PASS iterations=" << k_iterations << '\n';
}

void run_static_destruction_order_test() {
  using TLSS::MEMORY::CopyBackend;
  using TLSS::MEMORY::CopyHint;
  using TLSS::MEMORY::INTERNAL::CopyStrategy;

  constexpr std::size_t k_size = 256ULL * 1024ULL;
  std::vector<std::uint8_t> src(k_size);
  std::vector<std::uint8_t> dst(k_size);
  for (std::size_t index = 0; index < k_size; ++index) {
    src[index] = static_cast<std::uint8_t>((index * 131U + 17U) & 0xffU);
  }

  const auto plan = TLSS::MEMORY::INTERNAL::make_copy_plan_for_backend(
      k_size, CopyHint::Streaming, CopyBackend::Auto,
      TLSS::MEMORY::INTERNAL::get_cpu_capabilities());
  if (plan.strategy != CopyStrategy::ParallelNt ||
      TLSS::MEMORY::INTERNAL::memory_runtime_construction_count_for_test() != 0) {
    throw std::runtime_error("static destruction order test requires first ParallelNt use");
  }

  void* result = TLSS::MEMORY::memcpy(dst.data(), src.data(), k_size,
                                      CopyBackend::Auto, CopyHint::Streaming);
  if (result != dst.data() || std::memcmp(dst.data(), src.data(), k_size) != 0) {
    throw std::runtime_error("shutdown order copy mismatch");
  }
  if (TLSS::MEMORY::INTERNAL::memory_runtime_construction_count_for_test() != 1) {
    throw std::runtime_error("shutdown order test did not construct MemoryRuntime");
  }

  g_shutdown_order_probe_enabled.store(true, std::memory_order_release);
  std::cout << "Static destruction order main phase: PASS\n" << std::flush;
}
#endif

void run_runtime_degradation_test() {
  TLSS::MEMORY::INTERNAL::MemoryRuntime runtime;

  constexpr std::size_t k_size = 1024ULL * 1024ULL;
  AlignedBuffer src_buffer(k_size, 64);
  AlignedBuffer dst_buffer(k_size, 64);
  auto* src = src_buffer.data();
  auto* dst = dst_buffer.data();

  for (std::size_t index = 0; index < k_size; ++index) {
    src[index] = static_cast<std::uint8_t>((index * 131U + 17U) & 0xffU);
  }
  std::memset(dst, 0xA5, k_size);

  void* result = runtime.execute_parallel_copy(8, dst, src, k_size);
  if (result != dst || std::memcmp(dst, src, k_size) != 0) {
    throw std::runtime_error("runtime degradation copy failed");
  }

  std::cout << "Runtime degradation test: PASS\n";
}

#ifdef TLSS_MEMORY_TESTING
void run_runtime_worker_plan_test() {
  using namespace TLSS::MEMORY::INTERNAL;

  MemoryRuntime runtime;

  const auto expect_plan = [&runtime](int caller_cpu, std::size_t desired_worker_count,
                                      const std::vector<std::size_t>& expected) {
    const auto* actual =
        runtime.active_worker_indices_for_test(caller_cpu, desired_worker_count);
    if (actual == nullptr || *actual != expected) {
      throw std::runtime_error("runtime worker plan mismatch for caller CPU " +
                               std::to_string(caller_cpu));
    }

    std::cout << "caller=" << caller_cpu << " desired=" << desired_worker_count << " slots=";
    for (std::size_t index = 0; index < actual->size(); ++index) {
      if (index != 0) {
        std::cout << ' ';
      }
      std::cout << (*actual)[index];
    }
    std::cout << '\n';
  };

  const CpuTopology topology = detect_cpu_topology(detect_available_cpu_ids());
  for (const CpuInfo& cpu : topology.cpus) {
    const WorkerCandidates candidates = select_worker_candidates(topology, cpu.cpu_id);
    for (std::size_t desired_worker_count : {4U, 8U}) {
      const WorkerSelection selection = select_worker_set(candidates, desired_worker_count);
      const ActiveWorkerSelection expected =
          select_active_workers(runtime.master_worker_cpu_ids(), selection);
      const auto* actual =
          runtime.active_worker_indices_for_test(cpu.cpu_id, desired_worker_count);
      if (actual == nullptr || *actual != expected.worker_indices) {
        throw std::runtime_error("runtime worker plan mismatch for caller CPU " +
                                 std::to_string(cpu.cpu_id));
      }
    }
  }

  if (runtime.master_worker_cpu_ids() == std::vector<int>{0, 2, 4, 6, 8, 10, 12, 14}) {
    expect_plan(8, 4, {0, 1, 2, 3});
    expect_plan(8, 8, {0, 1, 2, 3, 5, 6, 7});
    expect_plan(4, 4, {0, 1, 3, 4});
    expect_plan(16, 8, {0, 1, 2, 3, 4, 5, 6, 7});
  }

  if (runtime.active_worker_indices_for_test(-1, 8) != nullptr ||
      runtime.active_worker_indices_for_test(1024, 8) != nullptr ||
      runtime.active_worker_indices_for_test(8, 3) != nullptr) {
    throw std::runtime_error("runtime worker plan accepted an invalid lookup");
  }

  std::cout << "Runtime worker plan test: PASS\n";
}

void run_runtime_worker_plan_validation_test() {
  using namespace TLSS::MEMORY::INTERNAL;

  MemoryRuntime runtime;
  const auto& master_cpu_ids = runtime.master_worker_cpu_ids();
  const auto available_cpu_ids = detect_available_cpu_ids();

  std::cout << "MemoryRuntime master workers:";
  for (int cpu_id : master_cpu_ids) {
    std::cout << ' ' << cpu_id;
  }
  std::cout << '\n';

  const auto has_cpu = [&available_cpu_ids](int cpu_id) {
    return std::find(available_cpu_ids.begin(), available_cpu_ids.end(), cpu_id) !=
           available_cpu_ids.end();
  };

  const auto show_plan = [&](int caller_cpu) {
    const auto* active_4 = runtime.active_worker_indices_for_test(caller_cpu, 4);
    const auto* active_8 = runtime.active_worker_indices_for_test(caller_cpu, 8);
    std::cout << "caller_cpu=" << caller_cpu << '\n';

    if (!has_cpu(caller_cpu)) {
      if (active_4 != nullptr || active_8 != nullptr) {
        throw std::runtime_error("plan exists for unavailable caller CPU");
      }
      std::cout << "plan=none\n";
      return;
    }
    if (active_4 == nullptr || active_8 == nullptr) {
      throw std::runtime_error("missing plan for available caller CPU");
    }

    const auto print_slots = [&](std::string_view label, const std::vector<std::size_t>& slots) {
      std::cout << label;
      for (std::size_t slot : slots) {
        if (slot >= master_cpu_ids.size()) {
          throw std::runtime_error("runtime worker plan has an invalid slot");
        }
        std::cout << ' ' << slot;
      }
      std::cout << '\n';
    };
    print_slots("4W active slots:", *active_4);
    print_slots("8W active slots:", *active_8);
  };

  show_plan(8);
  show_plan(16);

  const auto expect_slots = [&](int caller_cpu, std::size_t desired_worker_count,
                                const std::vector<std::size_t>& expected) {
    const auto* actual = runtime.active_worker_indices_for_test(caller_cpu, desired_worker_count);
    if (actual == nullptr || *actual != expected) {
      throw std::runtime_error("unexpected runtime worker slots");
    }
  };

  bool full_affinity = available_cpu_ids.size() == 28;
  for (std::size_t index = 0; full_affinity && index < available_cpu_ids.size(); ++index) {
    full_affinity = available_cpu_ids[index] == static_cast<int>(index);
  }
  if (full_affinity) {
    if (master_cpu_ids != std::vector<int>{0, 2, 4, 6, 8, 10, 12, 14}) {
      throw std::runtime_error("unexpected full-affinity master workers");
    }
    expect_slots(8, 4, {0, 1, 2, 3});
    expect_slots(8, 8, {0, 1, 2, 3, 5, 6, 7});
    expect_slots(16, 4, {0, 1, 2, 3});
    expect_slots(16, 8, {0, 1, 2, 3, 4, 5, 6, 7});
  }
  if (available_cpu_ids == std::vector<int>{0, 1, 2, 3, 4, 5, 16, 17}) {
    if (master_cpu_ids != std::vector<int>{0, 2, 4}) {
      throw std::runtime_error("unexpected mixed-affinity master workers");
    }
    expect_slots(16, 4, {0, 1, 2});
    expect_slots(16, 8, {0, 1, 2});
  }

  cpu_set_t original_cpu_set;
  CPU_ZERO(&original_cpu_set);
  if (pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &original_cpu_set) != 0) {
    throw std::runtime_error("failed to get caller affinity");
  }
  struct AffinityRestore {
    const cpu_set_t& original;
    ~AffinityRestore() noexcept {
      pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &original);
    }
  } restore{original_cpu_set};

  constexpr std::size_t k_size = 1024ULL * 1024ULL;
  AlignedBuffer src_buffer(k_size, 64);
  AlignedBuffer dst_buffer(k_size, 64);
  auto* src = src_buffer.data();
  auto* dst = dst_buffer.data();
  for (std::size_t index = 0; index < k_size; ++index) {
    src[index] = static_cast<std::uint8_t>((index * 131U + 17U) & 0xffU);
  }

  for (int caller_cpu : {8, 16}) {
    if (!has_cpu(caller_cpu)) {
      continue;
    }
    cpu_set_t caller_cpu_set;
    CPU_ZERO(&caller_cpu_set);
    CPU_SET(caller_cpu, &caller_cpu_set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &caller_cpu_set) != 0 ||
        sched_getcpu() != caller_cpu) {
      throw std::runtime_error("failed to pin caller for plan validation");
    }
    std::memset(dst, 0xA5, k_size);
    if (runtime.execute_parallel_copy(8, dst, src, k_size) != dst ||
        std::memcmp(dst, src, k_size) != 0) {
      throw std::runtime_error("caller placement copy mismatch");
    }
  }

  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &original_cpu_set) != 0) {
    throw std::runtime_error("failed to restore caller affinity");
  }
  std::cout << "Runtime worker plan validation: PASS\n";
}
#endif

void run_memory_runtime_dynamic_worker_test() {
  TLSS::MEMORY::INTERNAL::MemoryRuntime runtime;

  cpu_set_t original_cpu_set;
  CPU_ZERO(&original_cpu_set);
  if (pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &original_cpu_set) != 0) {
    throw std::runtime_error("failed to get caller affinity");
  }

  struct AffinityRestore {
    const cpu_set_t& original;
    ~AffinityRestore() noexcept {
      pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &original);
    }
  } restore{original_cpu_set};

  cpu_set_t caller_cpu_set;
  CPU_ZERO(&caller_cpu_set);
  CPU_SET(8, &caller_cpu_set);
  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &caller_cpu_set) != 0) {
    throw std::runtime_error("failed to pin caller to CPU8");
  }

  constexpr std::size_t k_size = 1024ULL * 1024ULL;
  AlignedBuffer src_buffer(k_size);
  AlignedBuffer dst_buffer(k_size);
  auto* src = src_buffer.data();
  auto* dst = dst_buffer.data();

  for (std::size_t index = 0; index < k_size; ++index) {
    src[index] = static_cast<std::uint8_t>((index * 131U + 17U) & 0xFFU);
  }

  for (std::size_t desired_worker_count : {8U, 4U}) {
    std::memset(dst, 0xA5, k_size);
    void* result = runtime.execute_parallel_copy(desired_worker_count, dst, src, k_size);
    if (result != dst || std::memcmp(dst, src, k_size) != 0) {
      throw std::runtime_error("dynamic runtime copy mismatch");
    }
  }

  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &original_cpu_set) != 0) {
    throw std::runtime_error("failed to restore caller affinity");
  }

  std::cout << "MemoryRuntime dynamic worker test: PASS\n";
}

int main(int argc, char* argv[]) {
#ifdef TLSS_MEMORY_TESTING
  if (argc == 2 && std::string_view(argv[1]) == "static-destruction-order-test") {
    run_static_destruction_order_test();
    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "runtime-lifetime-test") {
    run_runtime_lifetime_test();
    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "concurrent-construction-failure-test") {
    run_concurrent_construction_failure_test();
    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "runtime-construction-failure-test") {
    run_runtime_construction_failure_test();
    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "concurrent-first-use-test") {
    run_concurrent_first_use_test();
    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "lazy-runtime-parallel-test") {
    run_lazy_runtime_parallel_test();
    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "lazy-runtime-test") {
    run_lazy_runtime_test();
    return 0;
  }
#endif

  if (argc == 2 && std::string_view(argv[1]) == "tlss-memcpy-backend-test") {
    run_tlss_memcpy_backend_test();
    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "backend-plan-contract-test") {
    run_backend_plan_contract_test();
    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "explicit-backend-capability-test") {
    run_explicit_backend_capability_test();
    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "auto-capability-integration-test") {
    run_auto_capability_integration_test();
    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "copy-capability-policy-test") {
    run_copy_capability_policy_test();
    return 0;
  }

#ifdef TLSS_MEMORY_TESTING
  if (argc == 2 && std::string_view(argv[1]) == "runtime-worker-plan-validation") {
    run_runtime_worker_plan_validation_test();
    return 0;
  }
#endif

#ifdef TLSS_MEMORY_TESTING
  if ((argc == 4 || argc == 5) &&
      std::string_view(argv[1]) == "restricted-runtime-perf") {
    const std::size_t desired_worker_count = argc == 5 ? std::stoull(argv[4]) : 8;
    run_restricted_runtime_performance_benchmark(std::stoi(argv[2]), std::stoull(argv[3]),
                                                 desired_worker_count);
    return 0;
  }
#endif

  if (argc == 2 && std::string_view(argv[1]) == "runtime-degradation-test") {
    run_runtime_degradation_test();
    return 0;
  }

#ifdef TLSS_MEMORY_TESTING
  if (argc == 2 && std::string_view(argv[1]) == "runtime-fast-path-overhead") {
    run_runtime_fast_path_overhead_benchmark();
    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "runtime-plan-lookup-overhead") {
    run_runtime_plan_lookup_overhead_benchmark();
    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "runtime-worker-plan-test") {
    run_runtime_worker_plan_test();
    return 0;
  }
#endif

  if (argc == 2 && std::string_view(argv[1]) == "runtime-selection-overhead") {
    run_runtime_selection_overhead_benchmark();
    return 0;
  }

  if (argc == 3 && std::string_view(argv[1]) == "memory-runtime-perf") {
    run_memory_runtime_performance_benchmark(std::stoull(argv[2]));
    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "memory-runtime-dynamic-worker-test") {
    run_memory_runtime_dynamic_worker_test();
    return 0;
  }

#ifdef TLSS_MEMORY_TESTING
  if (argc == 2 && std::string_view(argv[1]) == "memory-runtime-topology-test") {
    run_memory_runtime_topology_test();
    return 0;
  }
#endif

  if (argc == 2 && std::string_view(argv[1]) == "master-worker-selection-test") {
    run_master_worker_selection_test();
    return 0;
  }
  if (argc == 2 && std::string_view(argv[1]) == "sparse-worker-stress") {
    run_sparse_worker_stress_test();
    return 0;
  }
  if (argc == 3 && std::string_view(argv[1]) == "sparse-workers") {
    const std::size_t size_kib = std::stoull(argv[2]);

    run_sparse_worker_benchmark(size_kib);

    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "sparse-worker-copy-test") {
    run_sparse_active_worker_copy_test();
    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "worker-selection-test") {
    run_worker_selection_test();
    return 0;
  }

  if (argc == 3 && std::string_view(argv[1]) == "dynamic-workers") {
    const std::size_t size_kib = std::stoull(argv[2]);

    run_dynamic_worker_matrix(size_kib);

    return 0;
  }
  if (argc == 2 && std::string_view(argv[1]) == "worker-candidates") {
    run_worker_candidates_test();
    return 0;
  }
  if (argc == 2 && std::string_view(argv[1]) == "cpu-core-types") {
    run_cpu_core_type_test();
    return 0;
  }
  if (argc == 2 && std::string_view(argv[1]) == "worker-candidates") {
    run_worker_candidate_test();
    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "cpu-topology") {
    run_cpu_topology_test();
    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "available-cpus") {
    run_available_cpu_test();
    return 0;
  }
  if (argc == 2 && std::string_view(argv[1]) == "cpu-capabilities") {
    run_cpu_capabilities_test();
    return 0;
  }
  if (argc == 3 && std::string_view(argv[1]) == "tlss-streaming-cpu8-bench") {
    run_tlss_memcpy_cpu8_benchmark(std::stoull(argv[2]));
    return 0;
  }

  if (argc == 3 && std::string_view(argv[1]) == "tlss-streaming-bench") {
    const std::size_t size_kib = std::stoull(argv[2]);

    run_tlss_memcpy_streaming_benchmark(size_kib);

    return 0;
  }
  if (argc == 2 && std::string_view(argv[1]) == "tlss-memcpy-auto-test") {
    run_tlss_memcpy_auto_test();
    return 0;
  }
  if (argc == 2 && std::string_view(argv[1]) == "memory-runtime-concurrency-test") {
    run_memory_runtime_concurrency_test();
    return 0;
  }
  if (argc == 2 && std::string_view(argv[1]) == "copy-executor-test") {
    run_copy_executor_test();

    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "copy-policy-test") {
    run_copy_policy_test();
    return 0;
  }

  if (argc == 4 && std::string_view(argv[1]) == "parallel-nt-policy") {
    const std::size_t worker_count = std::stoull(argv[2]);

    const std::size_t size_kib = std::stoull(argv[3]);

    run_parallel_nt_policy_benchmark(worker_count, size_kib);

    return 0;
  }

  if (argc == 4 && std::string_view(argv[1]) == "parallel-nt-align") {
    const std::size_t size_kib = std::stoull(argv[2]);

    const std::size_t dst_offset = std::stoull(argv[3]);

    run_parallel_nt_alignment_benchmark(size_kib, dst_offset);

    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "parallel-nt-stress") {
    run_parallel_nt_stress_test();

    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "parallel-nt-correctness") {
    run_parallel_nt_correctness_test();

    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "copy-partition-test") {
    run_copy_partition_test();

    return 0;
  }

  if (argc == 4 && std::string_view(argv[1]) == "pool-sync") {
    const std::size_t worker_count = std::stoull(argv[2]);

    const std::size_t cpu_offset = std::stoull(argv[3]);

    run_pool_sync_benchmark(worker_count, cpu_offset);

    return 0;
  }

  if (argc == 5 && std::string_view(argv[1]) == "pool-copy-kib") {
    const std::size_t worker_count = std::stoull(argv[2]);

    const std::size_t size_kib = std::stoull(argv[3]);

    const std::size_t cpu_offset = std::stoull(argv[4]);

    run_pool_copy_benchmark(worker_count, size_kib, cpu_offset);

    return 0;
  }
  if (argc == 3 && std::string_view(argv[1]) == "direct-nt2-kib") {
    const std::size_t size_kib = std::stoull(argv[2]);

    run_direct_nt2_benchmark_kib(size_kib);
    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "memory-read") {
    run_memory_read_benchmark();

    return 0;
  }

  if (argc == 4 && std::string_view(argv[1]) == "pool-copy-kib") {
    const std::size_t worker_count = std::stoull(argv[2]);

    const std::size_t size_kib = std::stoull(argv[3]);

    run_pool_copy_benchmark(worker_count, size_kib);

    return 0;
  }

  if (argc == 3 && std::string_view(argv[1]) == "pool-copy") {
    const std::size_t worker_count = std::stoull(argv[2]);

    run_pool_copy_benchmark(worker_count);

    return 0;
  }

  if (argc == 3 && std::string_view(argv[1]) == "multi-worker-pool") {
    const std::size_t worker_count = std::stoull(argv[2]);

    run_multi_worker_pool_benchmark(worker_count);

    return 0;
  }

  if (argc == 3 && std::string_view(argv[1]) == "hybrid-worker-gap") {
    const auto gap_us = std::stoull(argv[2]);

    run_hybrid_worker_gap_benchmark(std::chrono::microseconds(gap_us));

    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "pause-loop") {
    run_pause_loop_benchmark();

    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "hybrid-worker-cold") {
    run_hybrid_worker_cold_benchmark();

    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "hybrid-worker-hot") {
    run_hybrid_worker_hot_benchmark();

    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "atomic-spin-worker") {
    run_atomic_spin_worker_benchmark();

    return 0;
  }
  constexpr int k_physical_cpu_ids[] = {0,  2,  4,  6,  8,  10, 12, 14, 16, 17,
                                        18, 19, 20, 21, 22, 23, 24, 25, 26, 27};
  constexpr int k_pcore_smt_cpu_ids[] = {// 每个 P-core 的第一个 logical CPU
                                         0, 2, 4, 6, 8, 10, 12, 14,

                                         // 对应的 SMT sibling
                                         1, 3, 5, 7, 9, 11, 13, 15};

  if (argc == 2 && std::string_view(argv[1]) == "persistent-worker") {
    run_persistent_worker_benchmark();

    return 0;
  }

  if (argc == 3 && std::string_view(argv[1]) == "thread-create") {
    const std::size_t thread_count = std::stoull(argv[2]);

    if (thread_count == 0) {
      throw std::invalid_argument("thread_count must be greater than 0");
    }

    run_thread_creation_benchmark(thread_count);

    return 0;
  }

  if (argc == 3 && std::string_view(argv[1]) == "parallel-nt2-timed") {
    const std::size_t thread_count = std::stoull(argv[2]);

    run_parallel_nt2_timed_benchmark(thread_count, k_physical_cpu_ids,
                                     std::size(k_physical_cpu_ids));

    return 0;
  }

  if (argc == 3 && std::string_view(argv[1]) == "parallel-nt2-smt") {
    const std::size_t thread_count = std::stoull(argv[2]);

    run_parallel_nt2_timed_benchmark(thread_count, k_pcore_smt_cpu_ids,
                                     std ::size(k_pcore_smt_cpu_ids));

    return 0;
  }

  if (argc == 3 && std::string_view(argv[1]) == "parallel-nt2") {
    const std::size_t thread_count = std::stoull(argv[2]);

    run_parallel_nt2_benchmark(thread_count);

    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "auto-v6-paired") {
    run_auto_v6_paired_map();
    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "hot-v6-nt2") {
    run_hot_v6_nt2_map();
    return 0;
  }
  if (argc == 2 && std::string_view(argv[1]) == "rep-nt2-paired") {
    run_rep_nt2_crossover_map();
    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "stream-paired") {
    run_streaming_paired_map();

    return 0;
  }

  if (argc == 2 && std::string_view(argv[1]) == "nt2-paired") {
    run_nt2_libc_paired();
    return 0;
  }
  //
  // streaming single benchmark
  //
  // ./benchmark stream libc 134217728 65536
  //
  if (argc == 5 && std::string_view(argv[1]) == "stream") {
    const std::string_view type = argv[2];

    const std::size_t working_set_size = std::stoull(argv[3]);

    const std::size_t block_size = std::stoull(argv[4]);

    return run_single_streaming_benchmark(type, working_set_size, block_size);
  }

  // --------------------------------------------------
  // 单项 perf 模式
  //
  // ./benchmark rep 4096 0
  // --------------------------------------------------

  if (argc == 4) {
    const std::string_view type = argv[1];
    const std::size_t size = std::stoull(argv[2]);
    const std::size_t offset = std::stoull(argv[3]);

    return run_single_benchmark(type, size, offset);
  }

  if (argc == 5) {
    const std::string_view type = argv[1];
    const std::size_t size = std::stoull(argv[2]);
    const std::size_t src_offset = std::stoull(argv[3]);
    const std::size_t dst_offset = std::stoull(argv[4]);

    return run_single_benchmark(type, size, src_offset, dst_offset);
  }

  // --------------------------------------------------
  // 不带参数：
  // 完整 benchmark
  // --------------------------------------------------

  if (argc == 1) {
    /* return run_full_benchmark(); */
    /* return run_full_v5_v4_benchmark(); */
    /* return run_full_once_ns_benchmark(); */
    /* return run_full_map_benchmark(); */
    /* return run_full_alignement_benchmark(); */
    /* return run_v4_v6_crossover_benchmark(); */
    /* return run_v6_upper_bound_benchmark(); */
    return run_phase3_streaming_benchmark();
    /* return run_phase3_working_set_sweep(); */
  }

  std::cerr << "Usage:\n\n"

            << "Full benchmark:\n"
            << "  ./benchmark\n\n"

            << "Single benchmark:\n"
            << "  ./benchmark "
               "<libc|avx2|avx2-api|rep|auto|auto-stream|nt|direct-nt> "
               "<size> <offset>\n\n"

            << "Examples:\n"
            << "  ./benchmark libc 4096 0\n"
            << "  ./benchmark avx2 4096 0\n"
            << "  ./benchmark rep 4096 0\n"
            << "  ./benchmark auto 4096 0\n"
            << "  ./benchmark nt 4096 0\n\n"
            << "Streaming benchmark:\n"
            << "  ./benchmark stream "
               "<libc|avx2|avx2-api|rep|auto|auto-stream|nt|direct-nt> "
               "<working_set_size> <block_size>\n\n"
            << "Example:\n"
            << "  ./benchmark stream nt 134217728 33554432\n";

  return 1;
}
