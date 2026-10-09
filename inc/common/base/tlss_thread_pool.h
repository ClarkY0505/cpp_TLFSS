#ifndef __INC_COMMON_BASE_TLSS_THREAD_POOL_H__
#define __INC_COMMON_BASE_TLSS_THREAD_POOL_H__
#include "common/NoCopy.h"
#include "common/base/tlss_thread.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
namespace TLSS::BASE {
class ThreadPool : NoCopy {
 public:
  using Task = std::function<void()>;
  explicit ThreadPool(const std::string& name_arg = std::string("ThreadPool"));
  // Destruction must be performed by an external owner, after other callers exit.
  ~ThreadPool();

  // Configuration is only allowed before start(). Zero means an unbounded queue.
  void set_max_queue_size(int queue_max_size);
  void set_thread_init_cb(const Task& cb);

  // A pool can only be started once. Zero threads executes tasks in the caller.
  void start(int num_threads);
  // Reject new tasks, drain accepted tasks, then join. Repeated/concurrent calls
  // are safe. Calling from this pool's task/init callback throws logic_error.
  void stop();

  const std::string& name() const {
    return _name;
  }

  size_t queue_size() const;

  // False if not running, or a task in this pool submits to its full queue.
  // External callers wait for space. Empty tasks throw invalid_argument.
  // Async task/init exceptions retain the existing process-termination policy;
  // exceptions from zero-thread synchronous execution propagate to the caller.
  bool run(Task task);

 private:
  enum class State { k_created, k_running, k_stopping, k_stopped };
  bool is_full() const;  // Requires _mutex.
  void run_in_thread();
  Task take();
  void finish_inline_task();

  // Serializes start/stop, without holding _mutex while joining workers.
  std::mutex _lifecycle_mutex;
  mutable std::mutex _mutex;
  std::condition_variable _not_empty;
  std::condition_variable _not_full;
  std::condition_variable _inline_finished;
  std::string _name;
  Task _thread_init_cb;
  std::vector<std::unique_ptr<Thread>> _threads;
  std::deque<Task> _queue;
  size_t _max_queue_size;
  size_t _active_inline_tasks;
  State _state;
};
}  // namespace TLSS::BASE
#endif  // __INC_COMMON_BASE_TLSS_THREAD_POOL_H__
