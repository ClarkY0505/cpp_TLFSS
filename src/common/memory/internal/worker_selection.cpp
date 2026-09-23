#include "worker_selection.h"

#include <algorithm>

namespace TLSS::MEMORY::INTERNAL {

WorkerSelection select_worker_set(const WorkerCandidates& candidates,
                                  std::size_t desired_worker_count) {
  WorkerSelection selection;

  if (desired_worker_count == 0) {
    return selection;
  }

  const std::size_t actual_worker_count =
      std::min(desired_worker_count, candidates.intel_core_cpu_ids.size());

  selection.cpu_ids.insert(selection.cpu_ids.end(), candidates.intel_core_cpu_ids.begin(),
                           candidates.intel_core_cpu_ids.begin() + actual_worker_count);

  return selection;
}

ActiveWorkerSelection select_active_workers(const std::vector<int>& pool_cpu_ids,
                                            const WorkerSelection& worker_selection) {
  ActiveWorkerSelection selection;

  selection.worker_indices.reserve(worker_selection.cpu_ids.size());

  for (int selected_cpu_id : worker_selection.cpu_ids) {
    const auto iterator = std::find(pool_cpu_ids.begin(), pool_cpu_ids.end(), selected_cpu_id);

    if (iterator == pool_cpu_ids.end()) {
      continue;
    }

    const auto worker_index =
        static_cast<std::size_t>(std::distance(pool_cpu_ids.begin(), iterator));

    selection.worker_indices.push_back(worker_index);
  }

  return selection;
}

std::vector<int> select_master_worker_cpu_ids(const CpuTopology& topology) {
  std::vector<int> cpu_ids;
  cpu_ids.reserve(topology.physical_cores.size());

  for (const PhysicalCore& core : topology.physical_cores) {
    if (core.logical_cpu_ids.empty()) {
      continue;
    }

    if (core.core_type != CpuCoreType::IntelCore) {
      continue;
    }

    cpu_ids.push_back(core.logical_cpu_ids.front());
  }

  return cpu_ids;
}

}  // namespace TLSS::MEMORY::INTERNAL
