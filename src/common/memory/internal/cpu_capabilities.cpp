#include "cpu_capabilities.h"

#if defined(__x86_64__) || defined(__i386__)

#include <cpuid.h>

#include <cstdint>

#endif

namespace TLSS::MEMORY::INTERNAL {

namespace {

#if defined(__x86_64__) || defined(__i386__)

std::uint64_t read_xcr0() noexcept {
  std::uint32_t eax = 0;
  std::uint32_t edx = 0;

  asm volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));

  return (static_cast<std::uint64_t>(edx) << 32) | eax;
}

#endif

}  // namespace

CpuCapabilities detect_cpu_capabilities() noexcept {
  CpuCapabilities capabilities{};

#if defined(__x86_64__) || defined(__i386__)

  unsigned int eax = 0;
  unsigned int ebx = 0;
  unsigned int ecx = 0;
  unsigned int edx = 0;

  //
  // CPUID leaf 1
  //
  if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
    constexpr unsigned int k_osxsave_bit = 1U << 27;

    constexpr unsigned int k_avx_bit = 1U << 28;

    capabilities.osxsave = (ecx & k_osxsave_bit) != 0;

    capabilities.avx_hardware = (ecx & k_avx_bit) != 0;

    if (capabilities.avx_hardware && capabilities.osxsave) {
      const std::uint64_t xcr0 = read_xcr0();

      constexpr std::uint64_t k_xmm_state = 1ULL << 1;

      constexpr std::uint64_t k_ymm_state = 1ULL << 2;

      constexpr std::uint64_t k_required_state = k_xmm_state | k_ymm_state;

      capabilities.avx_usable = (xcr0 & k_required_state) == k_required_state;
    }
  }

  //
  // CPUID leaf 7, subleaf 0
  //
  const unsigned int max_leaf = __get_cpuid_max(0, nullptr);

  if (max_leaf >= 7) {
    __cpuid_count(7, 0, eax, ebx, ecx, edx);

    constexpr unsigned int k_avx2_bit = 1U << 5;

    constexpr unsigned int k_erms_bit = 1U << 9;

    capabilities.avx2_hardware = (ebx & k_avx2_bit) != 0;

    capabilities.erms = (ebx & k_erms_bit) != 0;

    capabilities.avx2_usable = capabilities.avx2_hardware && capabilities.avx_usable;
  }

#endif

  return capabilities;
}

const CpuCapabilities& get_cpu_capabilities() noexcept {
  static const CpuCapabilities capabilities = detect_cpu_capabilities();

  return capabilities;
}

}  // namespace TLSS::MEMORY::INTERNAL
