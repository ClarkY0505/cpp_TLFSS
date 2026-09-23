#include "parallel_copy_pool.h"

#include <immintrin.h>
#include <pthread.h>
#include <sched.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>

extern "C" {

void* tlss_avx2_nt_memcpy_2stream(void* dst, const void* src, std::size_t size);
}

namespace TLSS::MEMORY::INTERNAL {

namespace {

bool pin_current_thread_to_cpu(int cpu_id) noexcept {
  if (cpu_id < 0 || cpu_id >= CPU_SETSIZE) {
    return false;
  }

  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);
  CPU_SET(cpu_id, &cpu_set);
  const int result = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set);

  return result == 0;
}

bool is_aligned(const void* pointer, std::size_t alignment) noexcept {
  const auto address = reinterpret_cast<std::uintptr_t>(pointer);

  return (address & (alignment - 1)) == 0;
}

}  // namespace

ParallelCopyPool::ParallelCopyPool(std::size_t worker_count, const int* cpu_ids)
    : worker_count_(worker_count), tasks_(worker_count) {
  if (worker_count_ == 0) {
    throw std::invalid_argument("worker_count must be greater than zero");
  }

  if (cpu_ids == nullptr) {
    throw std::invalid_argument("cpu_ids must not be null");
  }

  worker_cpu_ids_.assign(cpu_ids, cpu_ids + worker_count_);

  // All worker states must exist before any thread can enter worker_loop().
  worker_states_.reserve(worker_count_);
  for (std::size_t worker_index = 0; worker_index < worker_count_; ++worker_index) {
    worker_states_.push_back(std::make_unique<WorkerState>());
  }

  workers_.reserve(worker_count_);
  try {
    for (std::size_t worker_index = 0; worker_index < worker_count_; ++worker_index) {
      const int cpu_id = worker_cpu_ids_[worker_index];

      workers_.emplace_back(
          [this, worker_index, cpu_id]() noexcept { worker_loop(worker_index, cpu_id); });
    }

  } catch (...) {
    stop_and_join();

    throw;
  }

  //
  // Worker startup barrier
  // constructor
  // 所有 worker 必须完成：
  //   thread startup
  //   CPU affinity
  std::unique_lock<std::mutex> lock(mutex_);

  startup_condition_.wait(lock, [this]() noexcept { return ready_count_ == worker_count_; });

  if (worker_start_failed_) {
    lock.unlock();

    stop_and_join();

    throw std::runtime_error("failed to initialize parallel copy workers");
  }
}

ParallelCopyPool::~ParallelCopyPool() noexcept {
  stop_and_join();
}

void ParallelCopyPool::validate_request(void* dst, const void* src, std::size_t size) const {
  if (dst == nullptr) {
    throw std::invalid_argument("dst must not be null");
  }

  if (src == nullptr) {
    throw std::invalid_argument("src must not be null");
  }

  if (size == 0) {
    throw std::invalid_argument("size must be greater than zero");
  }

  if (!is_aligned(dst, k_nt_alignment)) {
    throw std::invalid_argument("dst must be 32-byte aligned");
  }

  if (size % k_nt_block_size != 0) {
    throw std::invalid_argument("copy size must be divisible by 8192 bytes");
  }
}

void ParallelCopyPool::dispatch_copy(const std::vector<std::size_t>& active_worker_indices,
                                     void* dst, const void* src, std::size_t size) {
  validate_request(dst, src, size);

  if (active_worker_indices.empty()) {
    throw std::invalid_argument("active worker set is empty");
  }

  //
  // 先验证 worker index（工作线程索引）。
  //
  for (std::size_t index = 0;
       index < active_worker_indices.size();
       ++index) {

    const std::size_t worker_index =
        active_worker_indices[index];

    if (worker_index >= worker_count_) {
      throw std::out_of_range(
          "active worker index out of range");
    }

    //
    // 防止重复 worker。
    //
    for (std::size_t next_index =
             index + 1;
         next_index <
             active_worker_indices.size();
         ++next_index) {

      if (worker_index ==
          active_worker_indices[next_index]) {

        throw std::invalid_argument(
            "duplicate active worker index");
      }
    }
  }

  auto* dst_bytes = static_cast<std::uint8_t*>(dst);
  const auto* src_bytes = static_cast<const std::uint8_t*>(src);
  const std::size_t block_count = size / k_nt_block_size;
  // 一个 worker 至少分配一个 NT block（非临时复制块）。
  const std::size_t active_worker_count = active_worker_indices.size();
  if (block_count < active_worker_count) {
    throw std::invalid_argument(
        "not enough NT blocks for active workers");
  }
  const std::size_t base_block_count = block_count / active_worker_count;
  const std::size_t extra_block_count = block_count % active_worker_count;
  std::size_t current_offset = 0;
  //
  // 当前 dispatch（任务发布）
  // 暂时仍然使用 mutex（互斥锁）。
  //
  // 下一轮 benchmark 后再考虑
  // 是否把它移出 fast path（快速路径）。
  {
    std::lock_guard<std::mutex> lock(mutex_);

    if (stopping_.load(std::memory_order_acquire)) {
      throw std::runtime_error("parallel copy pool is stopping");
    }

    // 当前只支持 single-flight
    if (task_in_flight_.load(std::memory_order_acquire)) {
      throw std::logic_error("previous copy task is still in flight");
    }

    for (CopyTask& task : tasks_) {
      task = {nullptr, nullptr, 0};
    }

    for (std::size_t active_rank = 0; active_rank < active_worker_count; ++active_rank) {
      const std::size_t worker_index = active_worker_indices[active_rank];

      const std::size_t worker_block_count = base_block_count + (active_rank < extra_block_count ? 1 : 0);
      const std::size_t worker_size = worker_block_count * k_nt_block_size;

      tasks_[worker_index] = {dst_bytes + current_offset, src_bytes + current_offset, worker_size};

      current_offset += worker_size;
    }

    if (current_offset != size) {
      throw std::logic_error("parallel copy task partition mismatch");
    }

    expected_completions_ = active_worker_count;
    completed_count_.store(0, std::memory_order_relaxed);
    completion_waiting_.store(false, std::memory_order_seq_cst);
    ++submitted_generation_;
    task_in_flight_.store(true, std::memory_order_release);

    for (std::size_t worker_index : active_worker_indices) {
      WorkerState& worker_state = *worker_states_[worker_index];
      // Synchronize publication with this worker's predicate check and sleep.
      std::lock_guard<std::mutex> worker_lock(worker_state.mutex);
      worker_state.generation.store(submitted_generation_, std::memory_order_release);
    }
  }

  for (std::size_t worker_index : active_worker_indices) {
    worker_states_[worker_index]->condition.notify_one();
  }
}

void ParallelCopyPool::wait() {
  if (!task_in_flight_.load(std::memory_order_acquire)) {
    return;
  }
  for (std::size_t spin_index = 0; spin_index < k_spin_iterations; ++spin_index) {
    if (completed_count_.load(std::memory_order_seq_cst) == expected_completions_) {
      task_in_flight_.store(false, std::memory_order_release);

      return;
    }

    _mm_pause();
  }

  if (completed_count_.load(std::memory_order_seq_cst) == expected_completions_) {
    task_in_flight_.store(false, std::memory_order_release);

    return;
  }

  std::unique_lock<std::mutex> lock(mutex_);

  // 告诉最后一个 worker：
  // caller（调用线程）准备进入 sleep
  // 这里使用 seq_cst（顺序一致）
  // 和 completed_count_ 的 seq_cst RMW/load
  // 建立严格的 wake-up handshake
  completion_waiting_.store(true, std::memory_order_seq_cst);

  // 设置 waiting 以后必须重新检查 completion。
  // 防止 worker 在：
  //   spin timeout
  //       ↓
  //   waiting = true
  // 之间已经完成。
  if (completed_count_.load(std::memory_order_seq_cst) != expected_completions_ &&
      !stopping_.load(std::memory_order_acquire)) {
    completion_condition_.wait(lock, [this]() noexcept {
      return completed_count_.load(std::memory_order_seq_cst) == expected_completions_ ||
             stopping_.load(std::memory_order_acquire);
    });
  }

  // caller 已经不再需要 completion notification
  completion_waiting_.store(false, std::memory_order_seq_cst);

  if (completed_count_.load(std::memory_order_seq_cst) != expected_completions_) {
    throw std::runtime_error("parallel copy pool stopped before task completion");
  }

  task_in_flight_.store(false, std::memory_order_release);
}

void ParallelCopyPool::worker_loop(std::size_t worker_index, int cpu_id) noexcept {
  WorkerState& worker_state = *worker_states_[worker_index];
  // Worker startup
  const bool affinity_ok = pin_current_thread_to_cpu(cpu_id);
  std::uint64_t observed_generation = worker_state.generation.load(std::memory_order_acquire);

  {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!affinity_ok) {
      worker_start_failed_ = true;
    }

    ++ready_count_;
  }

  startup_condition_.notify_all();

  if (!affinity_ok) {
    return;
  }

  // Worker main loop
  while (true) {
    if (stopping_.load(std::memory_order_acquire)) {
      return;
    }

    std::uint64_t current_generation = worker_state.generation.load(std::memory_order_acquire);

    // Fast path
    // 先 spin等待下一代任务。
    if (current_generation == observed_generation) {
      for (std::size_t spin_index = 0; spin_index < k_spin_iterations; ++spin_index) {
        if (stopping_.load(std::memory_order_acquire)) {
          return;
        }

        current_generation = worker_state.generation.load(std::memory_order_acquire);

        if (current_generation != observed_generation) {
          break;
        }

        _mm_pause();
      }
    }

    // Slow path
    // spin 没等到任务，
    // 进入 condition_variable sleep
    if (current_generation == observed_generation) {
      std::unique_lock<std::mutex> lock(worker_state.mutex);

      worker_state.condition.wait(lock, [this, &worker_state, observed_generation]() noexcept {
        return stopping_.load(std::memory_order_acquire) ||
               worker_state.generation.load(std::memory_order_acquire) != observed_generation;
      });

      if (stopping_.load(std::memory_order_acquire)) {
        return;
      }

      current_generation = worker_state.generation.load(std::memory_order_acquire);
    }

    observed_generation = current_generation;

    // generation release/acquire
    // 已经保证 tasks_ 可见。
    const CopyTask task = tasks_[worker_index];
    if (task.size == 0) {
      continue;
    }
    const std::size_t expected_completions = expected_completions_;
    // Copy kernel
    tlss_avx2_nt_memcpy_2stream(task.dst, task.src, task.size);
    // Completion
    // seq_cst RMW（顺序一致原子读改写）
    // 同时：
    // 1. 统计完成 worker 数量
    // 2. 参与 slow-path wake-up handshake
    const std::size_t completed = completed_count_.fetch_add(1, std::memory_order_seq_cst) + 1;

    // 不是最后一个 worker，
    // 不需要做 completion notification
    if (completed != expected_completions) {
      continue;
    }

    // Fast completion path
    // caller 还在 spin：
    //   no mutex
    //   no notify
    //
    if (!completion_waiting_.load(std::memory_order_seq_cst)) {
      continue;
    }

    // Slow completion path
    // caller 已经准备 sleep。
    // worker 获取相同 mutex，
    // 确保不会在：
    //
    //   predicate check
    //       ↓
    //   condition_variable wait
    // 中间丢失 notification
    {
      std::lock_guard<std::mutex> lock(mutex_);

      // caller 可能已经在我们拿到 mutex
      // 以前完成退出，所以再次确认。
      if (!completion_waiting_.load(std::memory_order_seq_cst)) {
        continue;
      }
    }
    completion_condition_.notify_one();
  }
}

void ParallelCopyPool::stop_and_join() noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);

    stopping_.store(true, std::memory_order_release);
  }

  for (const auto& worker_state : worker_states_) {
    // Pair with the worker's condition wait to avoid a lost shutdown wake-up.
    {
      std::lock_guard<std::mutex> worker_lock(worker_state->mutex);
    }
    worker_state->condition.notify_one();
  }
  startup_condition_.notify_all();
  completion_condition_.notify_all();

  for (std::thread& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }

  workers_.clear();
}

}  // namespace TLSS::MEMORY::INTERNAL
