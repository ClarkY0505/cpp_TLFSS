#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include "common/net/event_loop.h"
#include "ftp/async_file_receiver.h"
#include "ftp/file_transfer.h"
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

std::shared_ptr<AsyncReceiver> AsyncReceiver::create(DiskExecutor& executor,
                                                     const NET::TcpConnectionPtr& conn,
                                                     std::uint64_t size, Factory factory,
                                                     Finalizer finalizer, Ready ready, Done done,
                                                     Failed failed) {
  if (!conn || !conn->connected()) {
    throw std::runtime_error("connection is unavailable");
  }

  assert(conn->get_loop()->is_in_loop_thread());

  auto state =
      std::make_shared<State>(executor, conn->get_loop(), std::move(factory), std::move(finalizer));

  auto receiver = std::shared_ptr<AsyncReceiver>(
      new AsyncReceiver(conn, size, state, std::move(ready), std::move(done), std::move(failed)));

  state->owner = receiver;
  executor.attach(state);

  try {
    receiver->submit(Operation::initialize);
  } catch (...) {
    receiver->cancel();
    conn->resume_read_after_disk(state->token);
    throw;
  }

  return receiver;
}

std::size_t AsyncReceiver::feed(NET::Buffer& buffer) {
  assert(_state->loop->is_in_loop_thread());
  if (_terminal || _state->is_cancelled()) {
    throw std::runtime_error("file receiver is cancelled");
  }
  if (_busy || _finish_requested) {
    return 0;
  }

  const auto count = static_cast<std::size_t>(
      std::min<uint64_t>(_remaining, std::min(buffer.readable_bytes(), kChunkBytes)));
  if (count == 0) {
    return 0;
  }

  // 数据块拥有独立生命周期。
  std::string data(buffer.peek(), count);
  // 只有任务成功接收后，才消费网络 Buffer。
  submit(Operation::write, std::move(data));

  buffer.retrieve(count);
  _remaining -= count;
  _pending_bytes = count;

  return count;
}

void AsyncReceiver::finish() {
  assert(_state->loop->is_in_loop_thread());

  if (_terminal || _state->is_cancelled()) {
    throw std::runtime_error("file receiver is cancelled");
  }

  if (_remaining != 0) {
    throw std::runtime_error("file body is incomplete");
  }

  _finish_requested = true;

  // 最后一块可能仍在写入。
  // 对应完成回调会继续提交 finalize。
  if (!_busy) {
    submit(Operation::finish);
  }
}

bool AsyncReceiver::State::launch(Operation operation, std::string data) {
  {
    std::lock_guard<std::mutex> lock(mutex);

    if (cancelled || terminal || busy) {
      return false;
    }

    busy = true;
    std::shared_ptr<AsyncReceiver::State> self = shared_from_this();

    BASE::ThreadPool::Task task = [self, operation, data = std::move(data)]() mutable {
      self->execute(operation, std::move(data));
    };
    const bool accepted = executor.post(std::move(task), false);
    if (accepted) {
      return true;
    }

    busy = false;
  }

  // executor 可能刚进入停机状态。
  // 仍通过工作线程清理已打开的文件。
  cancel();
  return false;
}

void AsyncReceiver::State::execute(Operation operation, std::string data) {
  Result result;
  result.operation = operation;

  try {
    if (!is_cancelled()) {
      switch (operation) {
        case Operation::initialize:
          receiver = factory();

          if (!receiver) {
            throw std::runtime_error("file receiver factory returned null");
          }
          break;

        case Operation::write:
          if (!receiver) {
            throw std::runtime_error("file receiver is unavailable");
          }

          receiver->append(data.data(), data.size());
          break;

        case Operation::finish:
          if (!receiver) {
            throw std::runtime_error("file receiver is unavailable");
          }

          result.value = finalizer(*receiver);
          break;
      }
    }
  } catch (const std::exception& exception) {
    result.error = exception.what();
  } catch (...) {
    result.error = "unexpected disk operation failure";
  }

  // 在通知 EventLoop 之前释放本次数据块。
  // 新任务可能在本函数返回前就被 EventLoop 提交。
  std::string{}.swap(data);

  bool ending = false;

  {
    std::unique_lock<std::mutex> lock(mutex);

    ending = cancelled || !result.error.empty() || operation == Operation::finish;

    if (ending) {
      terminal = true;

      // close/unlink 也可能阻塞。
      // 不持有 cancel() 需要获取的 mutex 执行这些操作。
      lock.unlock();
      receiver.reset();
      lock.lock();
    }

    busy = false;

    // 与 cancel() 关闭投递开关使用同一把锁。
    post_result_locked(std::move(result));
  }

  // 必须在最后一次投递结束之后解除登记。
  // stop() 等待登记清空后，才允许 EventLoop 进入销毁阶段。
  if (ending) {
    executor.release(token);
  }

  // 非终结任务到这里以后，不再访问 Receiver。
}

void AsyncReceiver::State::post_result_locked(Result result) {
  if (cancelled || !post_enabled) {
    return;
  }

  NET::EventLoop::Functor cb = [weak = owner, result = std::move(result)]() mutable {
    if (auto receiver = weak.lock()) {
      receiver->on_result(std::move(result));
    }
  };
  loop->queue_in_loop(std::move(cb));
}

void AsyncReceiver::State::cancel() noexcept {
  std::shared_ptr<State> self;

  {
    std::lock_guard<std::mutex> lock(mutex);

    cancelled = true;

    // 必须先关闭开关。
    // terminal=true 时，工作线程也可能还在清理文件。
    post_enabled = false;

    if (token == 0 || terminal || busy) {
      return;
    }

    busy = true;
    self = shared_from_this();
  }

  BASE::ThreadPool::Task task = [self] {
    {
      std::lock_guard<std::mutex> lock(self->mutex);
      self->terminal = true;
    }

    // 所有文件资源都在工作线程释放。
    self->receiver.reset();

    {
      std::lock_guard<std::mutex> lock(self->mutex);
      self->busy = false;
    }

    self->executor.release(self->token);
  };
  const bool accepted = executor.post(std::move(task), true);
  // 这是内部不变量：
  // 每个已登记 State 最多有一个排队任务，
  // 队列容量与登记上限相同；
  // 登记未清空时线程池不会停止。
  if (!accepted) {
    std::terminate();
  }
}

void AsyncReceiver::submit(Operation operation, std::string data) {
  auto conn = _connection.lock();

  if (!conn || !conn->connected()) {
    throw std::runtime_error("connection is unavailable");
  }

  assert(conn->get_loop()->is_in_loop_thread());

  _busy = true;
  conn->pause_read_for_disk(_state->token);

  if (!_state->launch(operation, std::move(data))) {
    _busy = false;
    throw std::runtime_error("disk executor is stopping or unavailable");
  }
}

void AsyncReceiver::on_result(Result result) {
  assert(_state->loop->is_in_loop_thread());

  // 投递开关撤不掉已经进入 EventLoop 队列的回调，
  // 因此执行时还要检查一次。
  if (_terminal || _state->is_cancelled()) {
    return;
  }

  auto conn = _connection.lock();

  if (!conn || !conn->connected()) {
    cancel();
    return;
  }

  _busy = false;
  _pending_bytes = 0;

  if (!result.error.empty()) {
    report_error(conn, std::move(result.error));
    return;
  }

  try {
    if (result.operation == Operation::finish) {
      _terminal = true;

      if (_done) {
        _done(std::move(result.value));
      }

      // 先更新业务状态，再处理 Buffer 中剩余的数据。
      conn->resume_read_after_disk(_state->token);
      return;
    }

    if (result.operation == Operation::initialize && _ready) {
      _ready();
    }

    // ready() 也可能已经调用 finish() 提交了 finalize。
    if (_finish_requested && !_busy && !_terminal) {
      submit(Operation::finish);
      return;
    }

    if (!_busy && !_terminal) {
      conn->resume_read_after_disk(_state->token);
    }
  } catch (const std::exception& exception) {
    report_error(conn, exception.what());
  } catch (...) {
    report_error(conn, "unexpected file completion failure");
  }
}

void AsyncReceiver::report_error(const NET::TcpConnectionPtr& conn, std::string error) {
  _state->cancel();

  _terminal = true;
  _busy = false;
  _pending_bytes = 0;

  try {
    if (_failed) {
      _failed(std::move(error));
    } else {
      conn->force_close();
    }
  } catch (...) {
    conn->force_close();
  }

  conn->resume_read_after_disk(_state->token);
}

}  // namespace TLSS::FileStorage::Transfer
