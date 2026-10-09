#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <utility>
#include "common/base/excption.h"
#include "common/base/tlss_thread_pool.h"
namespace TLSS::BASE {
namespace {
// Keep the whole call stack: a synchronous task in A may enter B then stop A.
class PoolExecution;
thread_local const PoolExecution* current_execution = nullptr;

class PoolExecution {
 public:
  explicit PoolExecution(const ThreadPool* pool)
      : _pool(pool), _previous(current_execution) {
    current_execution = this;
  }
  ~PoolExecution() { current_execution = _previous; }
  PoolExecution(const PoolExecution&) = delete;
  PoolExecution& operator=(const PoolExecution&) = delete;

  static bool contains(const ThreadPool* pool) {
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

ThreadPool::ThreadPool(const std::string& name_arg)
    : _name(name_arg)
    , _max_queue_size(0)
    , _active_inline_tasks(0)
    , _state(State::k_created) {}

ThreadPool::~ThreadPool() {
  stop();
}

void ThreadPool::set_max_queue_size(int queue_max_size) {
  if (queue_max_size < 0) {
    throw std::invalid_argument("ThreadPool queue size must be nonnegative");
  }
  std::lock_guard<std::mutex> lock(_mutex);
  if (_state != State::k_created) {
    throw std::logic_error("ThreadPool configuration requires an unstarted pool");
  }
  _max_queue_size = static_cast<size_t>(queue_max_size);
}

void ThreadPool::set_thread_init_cb(const Task& cb) {
  std::lock_guard<std::mutex> lock(_mutex);
  if (_state != State::k_created) {
    throw std::logic_error("ThreadPool configuration requires an unstarted pool");
  }
  _thread_init_cb = cb;
}

void ThreadPool::start(int num_threads) {
  if (num_threads < 0) {
    throw std::invalid_argument("ThreadPool thread count must be nonnegative");
  }
  if (PoolExecution::contains(this)) {
    throw std::logic_error("ThreadPool cannot start from its own callback");
  }
  std::lock_guard<std::mutex> lifecycle_lock(_lifecycle_mutex);
  std::unique_lock<std::mutex> lock(_mutex);
  if (_state != State::k_created) {
    throw std::logic_error("ThreadPool can only be started once");
  }

  // Allocate wrappers before starting any worker, so allocation failure is safe.
  std::vector<std::unique_ptr<Thread>> threads;
  threads.reserve(num_threads);
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back(std::make_unique<Thread>(
        std::bind(&ThreadPool::run_in_thread, this), _name + std::to_string(i + 1)));
  }
  _threads = std::move(threads);
  _state = State::k_running;
  for (auto& th : _threads) {
    // Thread::start() failure handling belongs to the underlying Thread wrapper.
    th->start();
  }
  lock.unlock();

  if (num_threads == 0 && _thread_init_cb) {
    PoolExecution execution(this);
    try {
      _thread_init_cb();
    } catch (...) {
      // stop() still needs to wait for any concurrent inline tasks.
      lock.lock();
      _state = State::k_stopping;
      throw;
    }
  }
}

void ThreadPool::run_in_thread() {
  PoolExecution execution(this);
  try {
    if (_thread_init_cb) {
      _thread_init_cb();
    }
    // take() returns an empty task only after stopping and draining the queue.
    while (Task task = take()) {
      task();
    }
  } catch (const TLSS::Exception& ex) {
    fprintf(stderr, "exception caught in ThreadPool %s\n", _name.c_str());
    fprintf(stderr, "reason: %s\n", ex.what());
    fprintf(stderr, "stack trace: %s\n", ex.stack_race());
    abort();
  } catch (const std::exception& ex) {
    fprintf(stderr, "exception caught in ThreadPool %s\n", _name.c_str());
    fprintf(stderr, "reason: %s\n", ex.what());
    abort();
  } catch (...) {
    fprintf(stderr, "unknown exception caught in ThreadPool %s\n", _name.c_str());
    throw;
  }
}

ThreadPool::Task ThreadPool::take() {
  std::unique_lock<std::mutex> lock(_mutex);
  _not_empty.wait(lock, [this] {
    return !_queue.empty() || _state != State::k_running;
  });
  if (_queue.empty()) {
    return {};
  }

  Task task = std::move(_queue.front());
  _queue.pop_front();
  if (_max_queue_size > 0) {
    _not_full.notify_one();
  }
  return task;
}

bool ThreadPool::run(Task task) {
  if (!task) {
    throw std::invalid_argument("ThreadPool task must not be empty");
  }
  std::unique_lock<std::mutex> lock(_mutex);
  if (_state != State::k_running) {

    return false;
  }
  if (_threads.empty()) {
    ++_active_inline_tasks;
    lock.unlock();
    PoolExecution execution(this);
    try {
      task();
    } catch (...) {
      finish_inline_task();
      throw;
    }
    finish_inline_task();
    return true;
  }

  // Workers cannot wait for queue space that only those same workers can free.
  if (PoolExecution::contains(this) && is_full()) {
    return false;
  }
  _not_full.wait(lock, [this] {
    return _state != State::k_running || !is_full();
  });
  if (_state != State::k_running) {
    return false;
  }
  _queue.push_back(std::move(task));
  _not_empty.notify_one();
  return true;
}

void ThreadPool::finish_inline_task() {
  std::lock_guard<std::mutex> lock(_mutex);
  if (--_active_inline_tasks == 0) {
    _inline_finished.notify_all();
  }
}

void ThreadPool::stop() {
  // Check before taking _lifecycle_mutex: an external stopper may hold it while
  // waiting for this callback to return.
  if (PoolExecution::contains(this)) {
    throw std::logic_error("ThreadPool cannot stop from its own callback");
  }
  std::lock_guard<std::mutex> lifecycle_lock(_lifecycle_mutex);
  {
    std::unique_lock<std::mutex> lock(_mutex);
    if (_state == State::k_stopped) {
      return;
    }
    _state = State::k_stopping;
    _not_full.notify_all();
    _not_empty.notify_all();
    _inline_finished.wait(lock, [this] { return _active_inline_tasks == 0; });
  }
  for (auto& th : _threads) {
    th->join();
  }
  std::lock_guard<std::mutex> lock(_mutex);
  _state = State::k_stopped;
}

bool ThreadPool::is_full() const {
  return _max_queue_size > 0 && _queue.size() >= _max_queue_size;
}

size_t ThreadPool::queue_size() const {
  std::lock_guard<std::mutex> lock(_mutex);
  return _queue.size();
}
}  // namespace TLSS::BASE
