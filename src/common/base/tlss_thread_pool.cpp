#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <utility>
#include "common/base/excption.h"
#include "common/base/tlss_thread_pool.h"
namespace TLSS::BASE {
namespace {
// 记录当前线程正在执行的线程池回调；保留完整调用栈，以识别 A -> B -> A 的嵌套调用。
class PoolExecution;
thread_local const PoolExecution* current_execution = nullptr;

class PoolExecution {
 public:
  explicit PoolExecution(const ThreadPool* pool)
      : _pool(pool)
      , _previous(current_execution) {
    // 进入回调时压入当前线程的执行上下文。
    current_execution = this;
  }
  ~PoolExecution() {
    // 离开回调时恢复上层上下文，避免嵌套调用丢失所属线程池。
    current_execution = _previous;
  }
  PoolExecution(const PoolExecution&) = delete;
  PoolExecution& operator=(const PoolExecution&) = delete;

  static bool contains(const ThreadPool* pool) {
    // 沿当前线程的回调调用链查找目标线程池，而非只检查最内层回调。
    for (auto* context = current_execution; context; context = context->_previous) {
      if (context->_pool == pool) {
        return true;
      }
    }
    return false;
  }

 private:
  const ThreadPool* _pool;
  const PoolExecution* _previous;
};
}  // namespace

// 初始状态为未启动；队列容量为 0 表示不设上限，同步任务数从 0 开始。
ThreadPool::ThreadPool(const std::string& name_arg)
    : _name(name_arg)
    , _max_queue_size(0)
    , _active_inline_tasks(0)
    , _state(State::k_created) {}

ThreadPool::~ThreadPool() {
  // 销毁前完成停机，等待已接受的任务和工作线程结束。
  stop();
}

void ThreadPool::set_max_queue_size(int queue_max_size) {
  // 负数无意义；0 表示队列不限制容量。
  if (queue_max_size < 0) {
    throw std::invalid_argument("ThreadPool queue size must be nonnegative");
  }
  std::lock_guard<std::mutex> lock(_mutex);
  // 配置与启动互斥，只允许在创建态修改。
  if (_state != State::k_created) {
    throw std::logic_error("ThreadPool configuration requires an unstarted pool");
  }
  _max_queue_size = static_cast<size_t>(queue_max_size);
}

void ThreadPool::set_thread_init_cb(const Task& cb) {
  std::lock_guard<std::mutex> lock(_mutex);
  // 初始化回调须在启动前固定，避免工作线程读取时被并发修改。
  if (_state != State::k_created) {
    throw std::logic_error("ThreadPool configuration requires an unstarted pool");
  }
  _thread_init_cb = cb;
}

void ThreadPool::start(int num_threads) {
  // 零线程表示由 run() 的调用者同步执行任务；负数无效。
  if (num_threads < 0) {
    throw std::invalid_argument("ThreadPool thread count must be nonnegative");
  }
  // 回调中重入启动会与外部生命周期操作相互等待，提前拒绝。
  if (PoolExecution::contains(this)) {
    throw std::logic_error("ThreadPool cannot start from its own callback");
  }
  // 串行化启动和停止；_mutex 保护状态、线程列表及任务队列。
  std::lock_guard<std::mutex> lifecycle_lock(_lifecycle_mutex);
  std::unique_lock<std::mutex> lock(_mutex);
  // 包括停止后的实例在内，线程池都只能启动一次。
  if (_state != State::k_created) {
    throw std::logic_error("ThreadPool can only be started once");
  }

  // 在启动任何工作线程前分配包装对象，以便安全地处理分配失败。
  std::vector<std::unique_ptr<Thread>> threads;
  threads.reserve(num_threads);
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back(std::make_unique<Thread>(std::bind(&ThreadPool::run_in_thread, this),
                                                  _name + std::to_string(i + 1)));
  }
  // 发布线程列表及运行态后启动工作线程；它们进入 take() 时会等待 _mutex。
  _threads = std::move(threads);
  _state = State::k_running;
  try {
    for (auto& th : _threads) {
      // 逐个启动；Thread::start() 会等待对应工作线程进入线程入口。
      th->start();
    }
  } catch (...) {
    // 启动失败时拒绝任务，并在释放队列锁后等待已启动的线程退出。
    _state = State::k_stopping;
    _not_full.notify_all();
    _not_empty.notify_all();
    lock.unlock();
    for (auto& th : _threads) {
      if (th->started()) {
        th->join();
      }
    }
    lock.lock();
    _state = State::k_stopped;
    throw;
  }
  // 初始化回调和任务执行都不应占用队列锁。
  lock.unlock();

  // 零线程模式没有工作线程，初始化回调只在启动者线程执行一次。
  if (num_threads == 0 && _thread_init_cb) {
    PoolExecution execution(this);
    try {
      _thread_init_cb();
    } catch (...) {
      // 初始化失败后拒绝新任务；stop() 仍需等待并发执行的调用者线程任务结束。
      lock.lock();
      _state = State::k_stopping;
      throw;
    }
  }
}

void ThreadPool::run_in_thread() {
  // 将整个工作线程生命周期标记为本线程池的执行上下文。
  PoolExecution execution(this);
  try {
    // 每个工作线程先执行一次初始化回调，再处理任务。
    if (_thread_init_cb) {
      _thread_init_cb();
    }
    // 循环取任务；停止后仍会取完队列，空任务表示可以退出。
    while (Task task = take()) {
      task();
    }
  } catch (const TLSS::Exception& ex) {
    // 工作线程异常无法交还给提交者，记录异常及其调用栈后终止进程。
    fprintf(stderr, "exception caught in ThreadPool %s\n", _name.c_str());
    fprintf(stderr, "reason: %s\n", ex.what());
    fprintf(stderr, "stack trace: %s\n", ex.stack_race());
    abort();
  } catch (const std::exception& ex) {
    // 标准异常同样终止进程，但没有项目异常提供的调用栈。
    fprintf(stderr, "exception caught in ThreadPool %s\n", _name.c_str());
    fprintf(stderr, "reason: %s\n", ex.what());
    abort();
  } catch (...) {
    // 未知异常继续向线程入口传播，由 C++ 线程异常机制终止进程。
    fprintf(stderr, "unknown exception caught in ThreadPool %s\n", _name.c_str());
    throw;
  }
}

ThreadPool::Task ThreadPool::take() {
  std::unique_lock<std::mutex> lock(_mutex);
  // 队列空且仍在运行时等待；停机通知会唤醒等待中的工作线程。
  _not_empty.wait(lock, [this] {
    return !_queue.empty() || _state != State::k_running;
  });
  // 队列已清空时返回空任务，通知工作线程退出。
  if (_queue.empty()) {
    return {};
  }

  // 停机期间也继续取走已入队任务，直到队列排空。
  Task task = std::move(_queue.front());
  _queue.pop_front();
  // 有界队列释放一个位置后，唤醒一个等待提交的调用者。
  if (_max_queue_size > 0) {
    _not_full.notify_one();
  }
  return task;
}

bool ThreadPool::run(Task task) {
  // 空 std::function 不能执行，直接报告调用错误。
  if (!task) {
    throw std::invalid_argument("ThreadPool task must not be empty");
  }
  std::unique_lock<std::mutex> lock(_mutex);
  // 只有运行态接收新任务；未启动或停止后提交者收到 false。
  if (_state != State::k_running) {
    return false;
  }
  // 零线程模式在调用者线程同步执行，同时登记正在执行的任务供 stop() 等待。
  if (_threads.empty()) {
    ++_active_inline_tasks;
    lock.unlock();
    PoolExecution execution(this);
    try {
      task();
    } catch (...) {
      // 即使任务抛异常，也要完成计数和通知，再原样传播异常。
      finish_inline_task();
      throw;
    }
    finish_inline_task();
    return true;
  }

  // 工作线程不能等待只有自身才能释放的队列空位；满队列时拒绝嵌套提交。
  if (PoolExecution::contains(this) && is_full()) {
    return false;
  }
  // 外部提交者等待空位；停机也会唤醒它，以便返回 false。
  _not_full.wait(lock, [this] {
    return _state != State::k_running || !is_full();
  });
  // 等待期间可能已停机，入队前必须再次检查状态。
  if (_state != State::k_running) {
    return false;
  }
  // 成功入队后唤醒一个等待任务的工作线程。
  _queue.push_back(std::move(task));
  _not_empty.notify_one();
  return true;
}

void ThreadPool::finish_inline_task() {
  std::lock_guard<std::mutex> lock(_mutex);
  // 最后一个同步任务结束时，唤醒等待所有同步任务完成的 stop()。
  if (--_active_inline_tasks == 0) {
    _inline_finished.notify_all();
  }
}

void ThreadPool::stop() {
  // 在获取 _lifecycle_mutex 前检查：外部停止者可能正持有该锁，
  // 并等待当前回调返回。
  if (PoolExecution::contains(this)) {
    throw std::logic_error("ThreadPool cannot stop from its own callback");
  }
  // 与 start() 及其他 stop() 串行执行，确保工作线程只被 join 一次。
  std::lock_guard<std::mutex> lifecycle_lock(_lifecycle_mutex);
  {
    std::unique_lock<std::mutex> lock(_mutex);
    // 已完成停机时直接返回，重复调用不再操作线程。
    if (_state == State::k_stopped) {
      return;
    }
    // 拒绝新任务，并唤醒等待队列空位或任务到来的线程。
    _state = State::k_stopping;
    _not_full.notify_all();
    _not_empty.notify_all();
    // 零线程模式下，等待其他调用者线程里已接受的任务执行完。
    _inline_finished.wait(lock, [this] {
      return _active_inline_tasks == 0;
    });
  }
  // 释放队列锁后等待工作线程；它们仍需持锁取完队列中的任务。
  for (auto& th : _threads) {
    th->join();
  }
  // 所有已接受任务及工作线程结束后，发布最终状态。
  std::lock_guard<std::mutex> lock(_mutex);
  _state = State::k_stopped;
}

bool ThreadPool::is_full() const {
  // 调用者须持有 _mutex；容量为 0 时队列永不视为满。
  return _max_queue_size > 0 && _queue.size() >= _max_queue_size;
}

size_t ThreadPool::queue_size() const {
  // 在队列锁保护下读取长度，避免与入队、出队并发访问。
  std::lock_guard<std::mutex> lock(_mutex);
  return _queue.size();
}
}  // namespace TLSS::BASE
