#include "cpu_affinity.h"
#include "direct_nt_copy.h"
#include "memory_runtime.h"
#include "parallel_copy_pool.h"
#include "parallel_nt_copy.h"
#include "worker_selection.h"

#include <sched.h>

#include <atomic>
#include <stdexcept>
#include <utility>
#include <vector>

namespace TLSS::MEMORY::INTERNAL {

#ifdef TLSS_MEMORY_TESTING
namespace {

std::atomic<std::size_t> g_memory_runtime_construction_count{0};
std::atomic<std::size_t> g_memory_runtime_construction_attempt_count{0};
std::atomic<bool> g_fail_memory_runtime_construction_for_test{false};
std::atomic<bool> g_memory_runtime_destruction_started{false};

}  // namespace

std::size_t memory_runtime_construction_count_for_test() noexcept {
  return g_memory_runtime_construction_count.load(std::memory_order_relaxed);
}

std::size_t memory_runtime_construction_attempt_count_for_test() noexcept {
  return g_memory_runtime_construction_attempt_count.load(std::memory_order_relaxed);
}

void set_memory_runtime_construction_failure_for_test(bool enabled) noexcept {
  g_fail_memory_runtime_construction_for_test.store(enabled, std::memory_order_relaxed);
}

bool memory_runtime_destruction_started_for_test() noexcept {
  return g_memory_runtime_destruction_started.load(std::memory_order_acquire);
}
#endif

MemoryRuntime::MemoryRuntime() {
#ifdef TLSS_MEMORY_TESTING
  g_memory_runtime_construction_attempt_count.fetch_add(1, std::memory_order_relaxed);
  if (g_fail_memory_runtime_construction_for_test.load(std::memory_order_relaxed)) {
    throw std::runtime_error("injected MemoryRuntime construction failure");
  }
#endif

  const std::vector<int> available_cpu_ids = detect_available_cpu_ids();
  topology_ = detect_cpu_topology(available_cpu_ids);
  master_worker_cpu_ids_ = select_master_worker_cpu_ids(topology_);

  if (master_worker_cpu_ids_.empty()) {
#ifdef TLSS_MEMORY_TESTING
    g_memory_runtime_construction_count.fetch_add(1, std::memory_order_relaxed);
#endif
    return;
  }

  master_pool_ = std::make_unique<ParallelCopyPool>(master_worker_cpu_ids_.size(),
                                                    master_worker_cpu_ids_.data());
  build_runtime_worker_plans();
#ifdef TLSS_MEMORY_TESTING
  g_memory_runtime_construction_count.fetch_add(1, std::memory_order_relaxed);
#endif
}

#ifdef TLSS_MEMORY_TESTING
MemoryRuntime::~MemoryRuntime() noexcept {
  g_memory_runtime_destruction_started.store(true, std::memory_order_release);
}
#else
MemoryRuntime::~MemoryRuntime() noexcept = default;
#endif

void MemoryRuntime::build_runtime_worker_plans() {
  int max_cpu_id = -1;
  for (const CpuInfo& cpu : topology_.cpus) {
    if (cpu.cpu_id > max_cpu_id) {
      max_cpu_id = cpu.cpu_id;
    }
  }

  if (max_cpu_id < 0) {
    runtime_worker_plans_.clear();
    return;
  }

  runtime_worker_plans_.resize(static_cast<std::size_t>(max_cpu_id) + 1);
  for (const CpuInfo& cpu : topology_.cpus) {
    if (cpu.cpu_id < 0) {
      continue;
    }

    RuntimeWorkerPlan& plan = runtime_worker_plans_[static_cast<std::size_t>(cpu.cpu_id)];
    const WorkerCandidates candidates = select_worker_candidates(topology_, cpu.cpu_id);

    const WorkerSelection selected_4 = select_worker_set(candidates, 4);
    ActiveWorkerSelection active_4 = select_active_workers(master_worker_cpu_ids_, selected_4);
    const WorkerSelection selected_8 = select_worker_set(candidates, 8);
    ActiveWorkerSelection active_8 = select_active_workers(master_worker_cpu_ids_, selected_8);

    if (active_4.worker_indices.size() != selected_4.cpu_ids.size() ||
        active_8.worker_indices.size() != selected_8.cpu_ids.size()) {
      continue;
    }

    plan.active_workers_4 = std::move(active_4.worker_indices);
    plan.active_workers_8 = std::move(active_8.worker_indices);
    plan.valid = true;
  }
}

const MemoryRuntime::RuntimeWorkerPlan* MemoryRuntime::find_runtime_worker_plan(
    int caller_cpu) const noexcept {
  if (caller_cpu < 0) {
    return nullptr;
  }

  const std::size_t index = static_cast<std::size_t>(caller_cpu);
  if (index >= runtime_worker_plans_.size() || !runtime_worker_plans_[index].valid) {
    return nullptr;
  }

  return &runtime_worker_plans_[index];
}

#ifdef TLSS_MEMORY_TESTING
const std::vector<std::size_t>* MemoryRuntime::active_worker_indices_for_test(
    int caller_cpu, std::size_t desired_worker_count) const noexcept {
  const RuntimeWorkerPlan* plan = find_runtime_worker_plan(caller_cpu);
  if (plan == nullptr) {
    return nullptr;
  }

  switch (desired_worker_count) {
    case 4:
      return &plan->active_workers_4;
    case 8:
      return &plan->active_workers_8;
    default:
      return nullptr;
  }
}
#endif

void* MemoryRuntime::execute_parallel_copy(std::size_t desired_worker_count, void* dst,
                                           const void* src, std::size_t size) {
  if (master_pool_ == nullptr) {
    return direct_nt_copy(dst, src, size);
  }

  const int caller_cpu = sched_getcpu();
  const RuntimeWorkerPlan* plan = find_runtime_worker_plan(caller_cpu);
  if (plan == nullptr) {
    return direct_nt_copy(dst, src, size);
  }

  const std::vector<std::size_t>* active_worker_indices = nullptr;
  switch (desired_worker_count) {
    case 4:
      active_worker_indices = &plan->active_workers_4;
      break;
    case 8:
      active_worker_indices = &plan->active_workers_8;
      break;
    default:
      return direct_nt_copy(dst, src, size);
  }

  constexpr std::size_t k_min_parallel_worker_count = 2;
  if (active_worker_indices->size() < k_min_parallel_worker_count) {
    return direct_nt_copy(dst, src, size);
  }

  if (master_pool_busy_.test_and_set(std::memory_order_acquire)) {
    return direct_nt_copy(dst, src, size);
  }

  struct BusyGuard {
    std::atomic_flag& flag;

    ~BusyGuard() noexcept {
      flag.clear(std::memory_order_release);
    }
  };

  BusyGuard guard{master_pool_busy_};
  return parallel_nt_copy(*master_pool_, *active_worker_indices, dst, src, size);
}

}  // namespace TLSS::MEMORY::INTERNAL
