#include "ftp/disk_executor.h"

#include <utility>
#include <vector>

namespace TLSS::FileStorage::Transfer {
DiskExecutor::DiskExecutor(const std::string& name)
    : _pool(name) {
  _pool.set_max_queue_size(static_cast<int>(kMaxReceivers));

  // 清理任务不进入可被停机直接丢弃的等待队列。
  _pool.set_max_wait_queue_size(0);
}

DiskExecutor::~DiskExecutor() {
  stop();
}

void DiskExecutor::start(int threads) {
  if (threads <= 0) {
    throw std::invalid_argument("disk executor requires worker threads");
  }

  std::lock_guard<std::mutex> lifecycle(_lifecycle_mutex);
  std::lock_guard<std::mutex> lock(_mutex);

  if (_phase != Phase::created) {
    throw std::logic_error("disk executor can only be started once");
  }

  try {
    _pool.start(threads);
    _phase = Phase::running;
  } catch (...) {
    _phase = Phase::stopped;
    throw;
  }
}

void DiskExecutor::begin_shutdown() {
  std::vector<std::function<void()>> cancellations;

  {
    std::lock_guard<std::mutex> lock(_mutex);

    if (_phase == Phase::closing || _phase == Phase::stopped) {
      return;
    }

    cancellations.reserve(_receivers.size());

    for (const auto& entry : _receivers) {
      cancellations.push_back(entry.second);
    }

    _phase = Phase::closing;
  }

  // 不能持有 executor 的锁调用 cancel：
  // cancel 可能向 executor 提交清理任务。
  for (auto& cancellation : cancellations) {
    cancellation();
  }
}
void DiskExecutor::stop() {
  std::lock_guard<std::mutex> lifecycle(_lifecycle_mutex);
  {
    std::lock_guard<std::mutex> lock(_mutex);

    if (_phase == Phase::stopped) {
      return;
    }
  }

  begin_shutdown();
  {
    std::unique_lock<std::mutex> lock(_mutex);

    _drained.wait(lock, [this] {
      return _receivers.empty();
    });
  }

  // 此时所有文件资源已清理，也不会再产生新的磁盘任务。
  // stop 继续等待已接收任务的函数体退出并回收工作线程。
  _pool.stop();
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _phase = Phase::stopped;
  }
}

bool DiskExecutor::post(BASE::ThreadPool::Task task, bool cleanup) {
  std::lock_guard<std::mutex> lock(_mutex);

  /* if (_phase != Phase::running && !(cleanup && _phase == Phase::closing)) */
  if (_phase != Phase::running && (!cleanup || _phase != Phase::closing)) {
    return false;
  }

  return _pool.try_run(std::move(task));
}

void DiskExecutor::release(std::uint64_t token) {
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _receivers.erase(token);
  }

  _drained.notify_all();
}

}  // namespace TLSS::FileStorage::Transfer
