#ifndef __INC_FTP_ASYNC_FILE_RECEIVER_H__
#define __INC_FTP_ASYNC_FILE_RECEIVER_H__
#include "common/NoCopy.h"
#include "common/base/tlss_thread_pool.h"
#include "common/net/buffer.h"
#include "common/net/event_loop.h"
#include "common/net/tcp_connection.h"
#include "ftp/file_transfer.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace TLSS::FileStorage::Transfer {
class AsyncReceiver;
class DiskExecutor : NoCopy {
 public:
  static constexpr std::size_t kMaxReceivers = 64;
  explicit DiskExecutor(const std::string& name = "FileDiskPool");
  ~DiskExecutor();

  void start(int threads = 2);
  void begin_shutdown();
  void stop();

 private:
  friend class AsyncReceiver;

  enum class Phase { created, running, closing, stopped };

  template <typename State>
  void attach(const std::shared_ptr<State>& state) {
    std::lock_guard<std::mutex> lock(_mutex);

    if (_phase != Phase::running) {
      throw std::runtime_error("disk executor is not accepting transfers");
    }

    if (_receivers.size() >= kMaxReceivers) {
      throw std::runtime_error("too many active file transfers");
    }

    const std::uint64_t token = next_token.fetch_add(1, std::memory_order_relaxed);
    if (token == 0) {
      throw std::runtime_error("file transfer token space exhausted");
    }

    // 在发布取消 hook 之前设置 token。
    state->token = token;
    std::function<void()> cancel_cb = [weak = std::weak_ptr<State>(state)] {
      if (auto locked_state = weak.lock()) {
        locked_state->cancel();
      }
    };

    _receivers.emplace(token, cancel_cb);
  }
  bool post(BASE::ThreadPool::Task task, bool cleanup);
  void release(std::uint64_t token);

  BASE::ThreadPool _pool;

  std::mutex _lifecycle_mutex;
  std::mutex _mutex;
  std::condition_variable _drained;

  Phase _phase = Phase::created;
  std::unordered_map<std::uint64_t, std::function<void()>> _receivers;
  inline static std::atomic<std::uint64_t> next_token{1};
};

class AsyncReceiver : NoCopy, public std::enable_shared_from_this<AsyncReceiver> {
 public:
  static constexpr std::size_t kChunkBytes = 64 * 1024;

  using Factory = std::function<std::unique_ptr<Receiver>()>;
  using Finalizer = std::function<std::string(Receiver&)>;
  using Ready = std::function<void()>;
  using Done = std::function<void(std::string)>;
  using Failed = std::function<void(std::string)>;
  static std::shared_ptr<AsyncReceiver> create(DiskExecutor& executor,
                                               const NET::TcpConnectionPtr& conn,
                                               std::uint64_t size, Factory factory,
                                               Finalizer finalizer, Ready ready, Done done,
                                               Failed failed);
  ~AsyncReceiver() {
    cancel();
  }
  // 以下状态接口由连接所属 EventLoop 调用。
  //
  // remaining 表示还没有从网络 Buffer 接收进独立数据块的字节数，
  // 并不表示还没有写入磁盘的字节数。
  std::uint64_t remaining() const {
    return _remaining;
  }

  std::size_t pending_bytes() const {
    return _pending_bytes;
  }

  std::size_t feed(NET::Buffer& buffer);
  void finish();
  void cancel() noexcept {
    _state->cancel();
  }

 private:
  enum class Operation { initialize, write, finish };

  struct Result {
    Operation operation = Operation::initialize;
    std::string value;
    std::string error;
  };
  struct State : public std::enable_shared_from_this<State> {
    State(DiskExecutor& executor_arg, NET::EventLoop* loop_arg, Factory factory_arg,
          Finalizer finalizer_arg)
        : executor(executor_arg)
        , loop(loop_arg)
        , factory(std::move(factory_arg))
        , finalizer(std::move(finalizer_arg)) {}

    ~State() {
      // Receiver 必须已在工作线程中销毁。
      assert(!receiver);
    }

    bool is_cancelled() {
      std::lock_guard<std::mutex> lock(mutex);
      return cancelled || !post_enabled;
    }

    bool launch(Operation operation, std::string data);
    void execute(Operation operation, std::string data);
    void post_result_locked(Result result);
    void cancel() noexcept;

    DiskExecutor& executor;
    NET::EventLoop* loop;
    std::uint64_t token = 0;

    Factory factory;
    Finalizer finalizer;

    std::weak_ptr<AsyncReceiver> owner;

    // 只允许工作线程访问这个对象。
    std::unique_ptr<Receiver> receiver;

    std::mutex mutex;
    bool busy = false;
    bool cancelled = false;
    bool terminal = false;
    bool post_enabled = true;
  };
  AsyncReceiver(const NET::TcpConnectionPtr& conn, std::uint64_t size, std::shared_ptr<State> state,
                Ready ready, Done done, Failed failed)
      : _connection(conn)
      , _remaining(size)
      , _state(std::move(state))
      , _ready(std::move(ready))
      , _done(std::move(done))
      , _failed(std::move(failed)) {}

  void submit(Operation operation, std::string data = {});
  void on_result(Result result);
  void report_error(const NET::TcpConnectionPtr& conn, std::string error);

  std::weak_ptr<NET::TcpConnection> _connection;

  std::uint64_t _remaining;
  std::size_t _pending_bytes = 0;

  std::shared_ptr<State> _state;

  Ready _ready;
  Done _done;
  Failed _failed;

  // 以下字段只由所属 EventLoop 访问。
  bool _busy = false;
  bool _finish_requested = false;
  bool _terminal = false;
};

}  // namespace TLSS::FileStorage::Transfer

#endif  // __INC_FTP_ASYNC_FILE_RECEIVER_H__
