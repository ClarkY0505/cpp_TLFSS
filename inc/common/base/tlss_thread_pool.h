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
  // 其他调用者退出后，必须由外部持有者销毁线程池。
  ~ThreadPool();

  // 只能在调用 start() 前配置。队列大小为 0 表示不限制容量。
  void set_max_queue_size(int queue_max_size);
  void set_thread_init_cb(const Task& cb);

  // 线程池只能启动一次。线程数为 0 时，任务在调用者线程中执行。
  void start(int num_threads);
  // 拒绝新任务，执行完已接收的任务，然后等待工作线程结束。
  // 重复或并发调用都是安全的；从本线程池的任务或初始化回调中调用会抛出 logic_error。
  void stop();

  const std::string& name() const {
    return _name;
  }

  size_t queue_size() const;

  // 线程池未运行，或池内任务向已满的队列提交任务时，返回 false。
  // 外部调用者会等待队列空位。空任务会抛出 invalid_argument。
  // 异步任务或初始化回调中的异常沿用现有的进程终止策略；
  // 零线程模式下同步执行产生的异常会传播给调用者。
  bool run(Task task);
  // 不等待队列空位；未运行或队列已满时返回 false。
  // 空任务抛出 invalid_argument。
  // 零线程模式与 run() 一样，在调用者线程执行任务。
  bool try_run(Task task);

  // 新增异步线程等待
  // 必须在 start() 前调用；0 表示不允许等待任务。
  void set_max_wait_queue_size(int queue_max_size);
  // 成功接收任务返回 true；未运行、零工作线程、截止时间已到，
  // 或执行队列与等待队列都满时返回 false。
  // on_timeout 只在“已接收但等待入执行队列时超时”才调用。
  bool submit_async(Task task, std::chrono::steady_clock::time_point deadline, Task on_timeout);

 private:
  enum class State { k_created, k_running, k_stopping, k_stopped };
  bool is_full() const;  // 调用时必须持有 _mutex。
  void run_in_thread();
  Task take();
  void finish_inline_task();

  // 串行化 start()/stop()；等待工作线程结束时不持有 _mutex。
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

  // 新增对异步线程等待成员
  struct PendingTask {
    Task task;
    std::chrono::steady_clock::time_point deadline;
    Task on_timeout;
  };

  void deadline_loop();
  // 与现有 _not_empty、_not_full 放在一起
  std::condition_variable _deadline_changed;
  // 与现有 _threads、_queue 放在一起
  std::thread _deadline_thread;
  std::deque<PendingTask> _waiters;
  // 放在现有 _max_queue_size 之后，并按此顺序初始化
  size_t _max_wait_queue_size;
};
}  // namespace TLSS::BASE
#endif  // __INC_COMMON_BASE_TLSS_THREAD_POOL_H__
