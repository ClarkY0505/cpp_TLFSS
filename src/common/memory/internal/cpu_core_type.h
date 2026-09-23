#ifndef __CPU_CORE_TYPE_H__
#define __CPU_CORE_TYPE_H__

namespace TLSS::MEMORY::INTERNAL {

enum class CpuCoreType {
  Unknown,
  IntelCore,
  IntelAtom,
};

CpuCoreType detect_cpu_core_type(int cpu_id) noexcept;

const char* cpu_core_type_name(CpuCoreType core_type) noexcept;

}  // namespace TLSS::MEMORY::INTERNAL
#endif  // __CPU_CORE_TYPE_H__
