#ifndef __CPU_TOPOLOGY_H__
#define __CPU_TOPOLOGY_H__
#include "cpu_core_type.h"

#include <vector>

namespace TLSS::MEMORY::INTERNAL {
struct CpuInfo {
  int cpu_id{-1};
  int package_id{-1};
  int core_id{-1};
  bool topology_known{false};
  CpuCoreType core_type{CpuCoreType::Unknown};
};

struct PhysicalCore {
  int package_id{-1};
  int core_id{-1};

  CpuCoreType core_type{CpuCoreType::Unknown};

  std::vector<int> logical_cpu_ids;
};

struct CpuTopology {
  std::vector<CpuInfo> cpus;
  std::vector<PhysicalCore> physical_cores;
};

struct WorkerCandidates {
  std::vector<int> intel_core_cpu_ids;
  std::vector<int> intel_atom_cpu_ids;
  std::vector<int> unknown_cpu_ids;
};

WorkerCandidates select_worker_candidates(const CpuTopology& topology, int caller_cpu_id);
const PhysicalCore* find_physical_core(const CpuTopology& topology, int cpu_id) noexcept;
CpuTopology detect_cpu_topology(const std::vector<int>& available_cpu_ids);

}  // namespace TLSS::MEMORY::INTERNAL
#endif  // __CPU_TOPOLOGY_H__
