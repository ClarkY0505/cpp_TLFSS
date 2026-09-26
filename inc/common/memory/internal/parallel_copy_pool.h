#ifndef __PARALLEL_COPY_POOL_H__
#define __PARALLEL_COPY_POOL_H__
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace TLSS::MEMORY::INTERNAL {

class ParallelCopyPool final {
 public:
  ParallelCopyPool(std::size_t worker_count, const int* cpu_ids);

  ~ParallelCopyPool() noexcept;

  ParallelCopyPool(const ParallelCopyPool&) = delete;
  ParallelCopyPool& operator=(const ParallelCopyPool&) = delete;
  ParallelCopyPool(ParallelCopyPool&&) = delete;
  ParallelCopyPool& operator=(ParallelCopyPool&&) = delete;

  void dispatch_copy(const std::vector<std::size_t>& active_worker_indices, void* dst,
                     const void* src, std::size_t size);
  void wait();

  [[nodiscard]]
  std::size_t worker_count() const noexcept {
    return worker_count_;
  }

 private:
  struct alignas(64) CopyTask {
    std::uint8_t* dst{nullptr};

    const std::uint8_t* src{nullptr};

    std::size_t size{0};
  };

  struct WorkerState {
    std::atomic<std::uint64_t> generation{0};

    std::mutex mutex;

    std::condition_variable condition;
  };

  void worker_loop(std::size_t worker_index, int cpu_id) noexcept;
  void validate_request(void* dst, const void* src, std::size_t size) const;
  void stop_and_join() noexcept;

  static constexpr std::size_t k_spin_iterations = 256;
  static constexpr std::size_t k_nt_block_size = 8192;
  static constexpr std::size_t k_nt_alignment = 32;

  const std::size_t worker_count_;
  std::vector<int> worker_cpu_ids_;
  std::vector<std::thread> workers_;
  std::vector<CopyTask> tasks_;
  std::vector<std::unique_ptr<WorkerState>> worker_states_;
  std::atomic<std::size_t> completed_count_{0};
  std::atomic<bool> task_in_flight_{false};
  std::atomic<bool> completion_waiting_{false};
  std::atomic<bool> stopping_{false};

  std::mutex mutex_;
  std::condition_variable startup_condition_;
  std::condition_variable completion_condition_;
  std::uint64_t submitted_generation_{0};
  std::size_t ready_count_{0};
  bool worker_start_failed_{false};

  std::size_t expected_completions_{0};
};

}  // namespace TLSS::MEMORY::INTERNAL
#endif  // __PARALLEL_COPY_POOL_H__
