#ifndef __WORKER_SELECTION_H__
#define __WORKER_SELECTION_H__

#include "cpu_topology.h"

#include <cstddef>
#include <vector>

namespace TLSS::MEMORY::INTERNAL {

struct WorkerSelection {
  std::vector<int> cpu_ids;
};

struct ActiveWorkerSelection {
  std::vector<std::size_t> worker_indices;
};

ActiveWorkerSelection select_active_workers(const std::vector<int>& pool_cpu_ids,
                                            const WorkerSelection& worker_selection);

WorkerSelection select_worker_set(const WorkerCandidates& candidates,
                                  std::size_t desired_worker_count);
std::vector<int> select_master_worker_cpu_ids(const CpuTopology& topology);

}  // namespace TLSS::MEMORY::INTERNAL
#endif  // __WORKER_SELECTION_H__
