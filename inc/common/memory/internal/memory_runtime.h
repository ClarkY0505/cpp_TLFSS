#ifndef __MEMORY_RUNTIME_H__
#define __MEMORY_RUNTIME_H__

#include "cpu_topology.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace TLSS::MEMORY::INTERNAL {

class ParallelCopyPool;

#ifdef TLSS_MEMORY_TESTING
// Test-only construction and shutdown diagnostics.
std::size_t memory_runtime_construction_count_for_test() noexcept;
std::size_t memory_runtime_construction_attempt_count_for_test() noexcept;
void set_memory_runtime_construction_failure_for_test(bool enabled) noexcept;
bool memory_runtime_destruction_started_for_test() noexcept;
#endif

class MemoryRuntime final {
 public:
  MemoryRuntime();

  ~MemoryRuntime() noexcept;

  MemoryRuntime(const MemoryRuntime&) = delete;
  MemoryRuntime& operator=(const MemoryRuntime&) = delete;

  MemoryRuntime(MemoryRuntime&&) = delete;
  MemoryRuntime& operator=(MemoryRuntime&&) = delete;

  void* execute_parallel_copy(std::size_t desired_worker_count, void* dst, const void* src,
                              std::size_t size);

#ifdef TLSS_MEMORY_TESTING
  const std::vector<int>& master_worker_cpu_ids() const noexcept {
    return master_worker_cpu_ids_;
  }

  const std::vector<std::size_t>* active_worker_indices_for_test(
      int caller_cpu, std::size_t desired_worker_count) const noexcept;
#endif

 private:
  struct RuntimeWorkerPlan {
    std::vector<std::size_t> active_workers_4;
    std::vector<std::size_t> active_workers_8;
    bool valid{false};
  };

  void build_runtime_worker_plans();
  const RuntimeWorkerPlan* find_runtime_worker_plan(int caller_cpu) const noexcept;

  CpuTopology topology_;

  std::vector<int> master_worker_cpu_ids_;
  std::unique_ptr<ParallelCopyPool> master_pool_;
  std::vector<RuntimeWorkerPlan> runtime_worker_plans_;
  std::atomic_flag master_pool_busy_ = ATOMIC_FLAG_INIT;
};

}  // namespace TLSS::MEMORY::INTERNAL
#endif  // __MEMORY_RUNTIME_H__
