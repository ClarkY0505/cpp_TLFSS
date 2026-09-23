#include "cpu_core_type.h"

#if defined(__linux__) && (defined(__x86_64__) || defined(__i386__))

#include <cpuid.h>
#include <pthread.h>
#include <sched.h>

#endif

namespace TLSS::MEMORY::INTERNAL {

namespace {

#if defined(__linux__) && (defined(__x86_64__) || defined(__i386__))

CpuCoreType detect_current_cpu_core_type() noexcept {
  const unsigned int max_leaf = __get_cpuid_max(0, nullptr);

  if (max_leaf < 0x1A) {
    return CpuCoreType::Unknown;
  }

  unsigned int eax = 0;
  unsigned int ebx = 0;
  unsigned int ecx = 0;
  unsigned int edx = 0;

  __cpuid_count(0x1A, 0, eax, ebx, ecx, edx);

  if (eax == 0) {
    return CpuCoreType::Unknown;
  }

  const unsigned int core_type = (eax >> 24) & 0xFFU;

  switch (core_type) {
    case 0x40:
      return CpuCoreType::IntelCore;

    case 0x20:
      return CpuCoreType::IntelAtom;

    default:
      return CpuCoreType::Unknown;
  }
}

#endif

}  // namespace

CpuCoreType detect_cpu_core_type(int cpu_id) noexcept {
#if defined(__linux__) && (defined(__x86_64__) || defined(__i386__))

  if (cpu_id < 0 || cpu_id >= CPU_SETSIZE) {
    return CpuCoreType::Unknown;
  }

  //
  // 保存 original affinity
  cpu_set_t original_cpu_set;
  CPU_ZERO(&original_cpu_set);
  if (pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &original_cpu_set) != 0) {
    return CpuCoreType::Unknown;
  }

  //
  // 临时 pin到目标 CPU。
  cpu_set_t target_cpu_set;
  CPU_ZERO(&target_cpu_set);
  CPU_SET(cpu_id, &target_cpu_set);

  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &target_cpu_set) != 0) {
    return CpuCoreType::Unknown;
  }

  // 当前线程已经只能运行在 cpu_id，
  // 此时读取 CPUID 0x1A。
  const CpuCoreType core_type = detect_current_cpu_core_type();

  // 恢复 original affinity（原始亲和性）。
  const int restore_result =
      pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &original_cpu_set);

  if (restore_result != 0) {
    // noexcept 接口暂时只能报告 Unknown。
    // 后续 production hardening
    // 可以重新设计错误报告。
    return CpuCoreType::Unknown;
  }

  return core_type;

#else

  (void)cpu_id;

  return CpuCoreType::Unknown;

#endif
}

const char* cpu_core_type_name(CpuCoreType core_type) noexcept {
  switch (core_type) {
    case CpuCoreType::IntelCore:
      return "IntelCore";

    case CpuCoreType::IntelAtom:
      return "IntelAtom";

    case CpuCoreType::Unknown:
      return "Unknown";
  }

  return "Unknown";
}

}  // namespace TLSS::MEMORY::INTERNAL
