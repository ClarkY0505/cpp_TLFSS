#ifndef __CPU_CAPABILITIES_H__
#define __CPU_CAPABILITIES_H__

namespace TLSS::MEMORY::INTERNAL {

struct CpuCapabilities {
  bool avx_hardware{false};
  bool avx2_hardware{false};

  bool osxsave{false};

  bool avx_usable{false};
  bool avx2_usable{false};

  bool erms{false};
};

const CpuCapabilities& get_cpu_capabilities() noexcept;
CpuCapabilities detect_cpu_capabilities() noexcept;

}  // namespace TLSS::MEMORY::INTERNAL
#endif  // __CPU_CAPABILITIES_H__
