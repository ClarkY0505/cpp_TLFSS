#include "common/net/tcp_connection.h"

#include "common/net/channel.h"
#include "common/net/event_loop.h"
#include "common/net/net_logger.h"
#include "common/net/socket.h"
#include "common/utile/utile.h"
#include "spdlog/common.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <utility>

namespace TLSS::NET {
static_assert(sizeof(off_t) >= 8, "send_file requires 64-bit off_t");

void defaultConnectionCallback(const TcpConnectionPtr& conn) {
  net_logger()->trace("{} -> {} is {} ", conn->local_addr().to_ip_port(),
                      conn->peer_addr().to_ip_port(), conn->connected() ? "UP" : "DOWN");
}
void defaultMessageCallback(const TcpConnectionPtr& conn, Buffer* buffer,
                            TIME::Timestamp receive_time) {
  (void)conn;
  (void)receive_time;
  buffer->retrieve_all();
}

namespace {
constexpr std::size_t kMaxPendingBytes = 1024 * 1024;

// 限制已经进入连接发送队列的任务数量。
constexpr std::size_t kMaxPendingTasks = 1024;
constexpr std::size_t kMaxPendingFiles = 64;

// 每次写事件最多发送 1MiB，并限制循环次数。
constexpr std::size_t kWriteBudget = 1024 * 1024;
constexpr unsigned kWriteSteps = 64;

EventLoop* check_loop_not_null(EventLoop* loop) {
  if (loop == nullptr) {
    net_logger()->critical("{}:{}:{} TcpConnection loop is null", __FILE__, __FUNCTION__, __LINE__);
    std::abort();
  }

  return loop;
}

// sendfile 没有 MSG_NOSIGNAL 参数。
// 在当前线程临时屏蔽 SIGPIPE，清理由本次失败产生的信号，
// 最后恢复原信号掩码，不改变整个进程的信号处理方式。
ssize_t sendfile_no_sigpipe(int socket_fd, int file_fd, off_t* offset, std::size_t count) {
  sigset_t blocked;
  sigset_t old_mask;
  sigset_t pending;

  // 1. 创建一个只包含 SIGPIPE 的信号集合
  ::sigemptyset(&blocked);
  ::sigaddset(&blocked, SIGPIPE);

  // 2. 临时屏蔽当前线程的 SIGPIPE，保存原来的掩码
  const int mask_error = ::pthread_sigmask(SIG_BLOCK, &blocked, &old_mask);

  if (mask_error != 0) {
    errno = mask_error;
    return -1;
  }

  // 3. 记录调用前是否已经有待处理的 SIGPIPE
  if (::sigpending(&pending) == -1) {
    const int saved_error = errno;
    ::pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
    errno = saved_error;
    return -1;
  }

  const bool already_pending = ::sigismember(&pending, SIGPIPE) == 1;

  // 4. 发送文件
  const ssize_t result = ::sendfile(socket_fd, file_fd, offset, count);
  const int saved_error = errno;

  // 5. 如果本次失败产生了 SIGPIPE，取走它
  // 不消费调用前就已经存在的 SIGPIPE
  if (result == -1 && saved_error == EPIPE && !already_pending) {
    const timespec timeout{0, 0};

    while (::sigtimedwait(&blocked, nullptr, &timeout) == -1 && errno == EINTR) {
    }
  }

  // 6. 恢复当前线程原来的信号掩码
  ::pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);

  // 7. 保留 sendfile 的结果及错误码
  errno = saved_error;
  return result;
}

int get_socket_err(int sockfd) {
  int optval;
  socklen_t optlen = static_cast<socklen_t>(sizeof optval);
  if (::getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0) {
    return errno;
  }

  return optval;
}

}  // namespace

TcpConnection::FileHandle::~FileHandle() {
  if (fd >= 0) {
    ::close(fd);
  }
}

TcpConnection::TcpConnection(EventLoop* loop, const std::string& name, int sockfd,
                             const InetAddress& local_addr, const InetAddress& peer_addr)
    : _loop(check_loop_not_null(loop))
    , _name(name)
    , _state(k_connecting)
    , _reading(false)
    , _read_paused_for_shutdown(false)
    , _socket(new Socket(sockfd))
    , _channel(new Channel(loop, sockfd))
    , _local_addr(local_addr)
    , _peer_addr(peer_addr)
    , _conn_cb(defaultConnectionCallback)
    , _mess_cb(defaultMessageCallback)
    , _high_water_mark(std::size_t{512} * 1024) {
  // 停机读取标志从 false 开始；读写事件始终由连接所属的 loop 处理。
  // Poller 给channel通知感兴趣的事件发生了; channel会回调相应的操作函数
  _channel->set_read_cb(std::bind(&TcpConnection::handle_read, this, std::placeholders::_1));
  _channel->set_write_cb(std::bind(&TcpConnection::handle_write, this));
  _channel->set_close_cb(std::bind(&TcpConnection::handle_close, this));
  _channel->set_error_cb(std::bind(&TcpConnection::handle_error, this));
  net_logger()->info("TcpConnection::ctor[{}] at {} fd = {}", _name, static_cast<const void*>(this),
                     sockfd);
  _socket->set_keep_alive(true);
}

TcpConnection::~TcpConnection() {
  net_logger()->info("TcpConnection::dtor[{}] at {} fd = {} state = {}", _name,
                     static_cast<const void*>(this), _channel->fd(), state_to_string());
  cancel_pending(ECANCELED);
  // 客户端移交给外部的连接可能直接失去最后一个 owner，而没有收到 EOF。
  // 正常 connect_destroyed 已注销时不重复操作；剩余 Channel 仍须在 loop 收尾。
  if (_channel->index() != -1) {
    assert(_loop->is_in_loop_thread());
    if (!_channel->is_none_event()) {
      _channel->disable_all();
    }
    _channel->remove();
    // 本轮 active_channels 中可能还保留指针；tie 已失效，延迟释放可安全跳过事件。
    std::shared_ptr<Channel> retired(std::move(_channel));
    _loop->queue_in_loop([retired] {
    });
  }
}

std::string TcpConnection::state_to_string() const {
  switch (_state.load()) {
    case k_disconnected:
      return "kDisconnected";

    case k_connecting:
      return "kConnecting";

    case k_connected:
      return "kConnected";

    case k_disconnecting:
      return "kDisconnecting";

    default:
      return "unknown state";
  }
}

void TcpConnection::send(std::string message) {
  if (message.empty() || _state.load() != k_connected) {
    return;
  }

  if (_loop->is_in_loop_thread()) {
    send_in_loop(message.data(), message.size());
    return;
  }

  // 回调持有数据和连接，避免 c_str()、this 在执行前失效。
  auto self = shared_from_this();
  _loop->queue_in_loop([self, message = std::move(message)] {
    self->send_in_loop(message.data(), message.size());
  });
}

void TcpConnection::send_file(int fd, off_t offset, uint64_t count, FileSendCallback cb) {
  // 1. 创建文件任务
  PendingWrite task;
  task.kind = PendingWrite::Kind::file;
  task.file_offset = offset;
  task.remaining = count;
  task.cb = std::move(cb);

  // 2.创建资源管理对象，在获取文件描述符
  task.file = std::make_shared<FileHandle>();
  int prepare_error = 0;
  if (offset < 0) {
    prepare_error = EINVAL;
  } else {
    // 3. 复制描述符，任务独立持有自己的fd
    int dup_fd;
    do {
      dup_fd = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    } while (dup_fd == -1 && errno == EINTR);

    if (dup_fd == -1) {
      prepare_error = errno;
    } else {
      task.file->fd = dup_fd;
      // 4. 通过系统调用获取文件属性
      struct stat info {};
      // TLSS提供了一种内存序初始化的方式
      // 上面使用c++17 建议方式初始化
      /* TLSS::UTIL::memzero(&info,sizeof info); */
      int result;
      do {
        result = ::fstat(dup_fd, &info);
      } while (result == -1 && errno == EINTR);

      // 5.检查文件类型与发送范围
      if (result == -1) {
        prepare_error = errno;
      } else if (!S_ISREG(info.st_mode) || info.st_size < 0 || offset > info.st_size ||
                 count > static_cast<uint64_t>(info.st_size - offset)) {
        prepare_error = EINVAL;
      }
    }
  }

  // 6. 交给连接所属的 IO 线程。
  auto self = shared_from_this();
  _loop->run_in_loop([self, task = std::move(task), prepare_error]() mutable {
    self->send_file_in_loop(std::move(task), prepare_error);
  });

  // 6. 交给连接所属的 IO 线程。
  // 上述方法更直观，二者效果一样
  // 但是这里不能使用this指针，使用shared_from_this可以延长生命周期
  // 如果当前connection指针被析构或者销毁，如果使用了this,那么异步任务就会被终止
  /* _loop->run_in_loop(std::bind(&TcpConnection::send_file_in_loop, shared_from_this(), */
  /* std::move(task), prepare_error)); */
}

void TcpConnection::set_high_water_mark(std::size_t bytes) {
  if (!_loop->is_in_loop_thread() || bytes == 0 || bytes > kMaxPendingBytes) {
    net_logger()->error("invalid high water mark");
    std::abort();
  }
  _high_water_mark = bytes;
}

// 增加高水位函数后对应策略
void TcpConnection::stop_read() {
  std::shared_ptr<TcpConnection> self = shared_from_this();
  EventLoop::Functor cb = [self] {
    if (self->_state.load() != k_connected ||
        self->_pending_memory_bytes < self->_high_water_mark) {
      return;
    }

    // 即使已经因为磁盘暂停，也要记录发送背压原因。
    self->_send_read_paused = true;
    self->update_read_interest();
  };
  _loop->run_in_loop(cb);
}

// 增加高水位函数后对应策略
void TcpConnection::start_read() {
  std::shared_ptr<TcpConnection> self = shared_from_this();
  EventLoop::Functor cb = [self] {
    if (self->_state.load() != k_connected || self->_pending_memory_bytes != 0) {
      return;
    }

    // 这里只解除发送背压。
    self->_send_read_paused = false;

    self->schedule_buffered_input();
    self->update_read_interest();
  };
  _loop->run_in_loop(cb);
}

void TcpConnection::pause_read_for_shutdown() {
  std::shared_ptr<TcpConnection> self = shared_from_this();
  EventLoop::Functor cb = [self] {
    // 1. 设置停机标志，阻止普通背压恢复
    // 在所属 loop 中设置标志，阻止已经排队的写完成回调重新打开读取。
    self->_read_paused_for_shutdown = true;
    self->_read_eof_only = false;
    self->update_read_interest();
  };
  _loop->run_in_loop(cb);
}

void TcpConnection::shutdown_for_server_stop() {
  std::shared_ptr<TcpConnection> self = shared_from_this();
  EventLoop::Functor cb = [self] {
    if (self->_state.load() != k_connected) {
      return;
    }

    // 1. 拒绝新的发送任务，继续处理已有任务
    self->set_state(k_disconnecting);
    self->_read_paused_for_shutdown = true;
    self->_read_eof_only = true;

    self->_input_buffer.retrieve_all();
    self->update_read_interest();
    // 若输出缓冲区仍有数据，实际关闭写端由 handle_write 在排空后完成。
    self->shutdown_in_loop();
  };
  _loop->run_in_loop(cb);
}

// 增加高水位函数后对应策略
void TcpConnection::force_close() {
  auto self = shared_from_this();
  _loop->queue_in_loop([self] {
    self->handle_close();
  });
}

void TcpConnection::connect_established() {
  assert(_loop->is_in_loop_thread());

  set_state(k_connected);
  _channel->tie(shared_from_this());
  update_read_interest();

  if (_conn_cb) {
    _conn_cb(shared_from_this());
  }
}

void TcpConnection::connect_destroyed() {
  assert(_loop->is_in_loop_thread());

  // 记录是否还需要通知业务连接断开。
  const bool was_connected = _state.load() != k_disconnected;
  set_state(k_disconnected);
  // 确保已经停止监听所有事件。
  if (!_channel->is_none_event()) {
    _channel->disable_all();
  }

  _reading = false;
  // 确保未完成的发送任务已经清理。
  cancel_pending(ECANCELED);
  // 如果之前没有经过 handle_close()，在这里补发断开通知。
  if (was_connected && _conn_cb) {
    _conn_cb(shared_from_this());
  }
  // 正式从 Poller 的管理记录中移除。
  if (_channel->index() != -1) {
    _channel->remove();
  }
}

void TcpConnection::handle_read(TIME::Timestamp receive_time) {
  if (!should_read_in_loop() || (_buffered_input_pending && !_read_eof_only)) {
    return;
  }

  int saved_error = 0;

  const ssize_t n = _input_buffer.read_fd(_channel->fd(), &saved_error);
  if (n > 0) {
    _last_receive_time = receive_time;

    if (_read_paused_for_shutdown) {
      _input_buffer.retrieve_all();
    } else if (_mess_cb) {
      _mess_cb(shared_from_this(), &_input_buffer, receive_time);
    }
  } else if (n == 0) {
    handle_close();
  } else if (saved_error != EAGAIN && saved_error != EWOULDBLOCK && saved_error != EINTR) {
    fail_writes(saved_error);
  }
}

void TcpConnection::enable_write_event() {
  if (!_channel->is_writing()) {
    _channel->enable_writing();
  }
}

void TcpConnection::notify_file(PendingWrite task, int error) {
  // 1. 释放本任务持有的文件资源引用
  task.file.reset();
  // 2. 业务没有提供回调，资源释放后即可返回
  if (!task.cb) {
    return;
  }

  // 3. 取出回调和实际已发送字节数
  auto cb = std::move(task.cb);
  const uint64_t sent = task.file_sent;

  // 4. 将整数错误码转换为 error_code
  std::error_code result;
  if (error != 0) {
    result = std::error_code(error, std::generic_category());
  }

  // 5. 安排业务回调，稍后由 EventLoop 执行。
  _loop->queue_in_loop([cb = std::move(cb), result, sent] {
    cb(result, sent);
  });
  /* _loop->queue_in_loop(std::bind(std::move(cb),result,sent)); */
}

void TcpConnection::complete_front() {
  // 1. 先取走任务，在删除队首元素
  PendingWrite task = std::move(_writes.front());
  _writes.pop_front();

  if (task.kind == PendingWrite::Kind::memory) {
    // 2. 普通数据
    // 扣除它占用的内存统计
    _pending_memory_bytes -= task.data.size();
  } else {
    // 3. 文件任务
    // 减少文件数量， 报告成功
    --_pending_files;
    notify_file(std::move(task), 0);
  }
}

void TcpConnection::handle_write() {
  assert(_loop->is_in_loop_thread());

  if (_state.load() == k_disconnected || !_channel->is_writing()) {
    return;
  }

  std::size_t budget = kWriteBudget;
  unsigned steps = 0;
  while (!_writes.empty() && budget > 0 && steps < kWriteSteps) {
    ++steps;
    PendingWrite& task = _writes.front();

    // 空文件任务不需要执行 sendfile
    if (task.kind == PendingWrite::Kind::file && task.remaining == 0) {
      complete_front();
      continue;
    }

    ssize_t n;
    if (task.kind == PendingWrite::Kind::memory) {
      // 普通数据
      // 从上次发送结束的位置继续发送
      const std::size_t remaining = task.data.size() - task.data_offset;
      const std::size_t count = std::min(remaining, budget);
      n = ::send(_channel->fd(), task.data.data() + task.data_offset, count, MSG_NOSIGNAL);
    } else {
      // 文件
      // 从保存的文件偏移量继续
      const std::size_t count =
          static_cast<std::size_t>(std::min<uint64_t>(task.remaining, budget));
      n = sendfile_no_sigpipe(_channel->fd(), task.file->fd, &task.file_offset, count);
    }

    if (n > 0) {
      const std::size_t written = static_cast<std::size_t>(n);
      budget -= written;
      bool completed;
      if (task.kind == PendingWrite::Kind::memory) {
        task.data_offset += written;
        completed = task.data_offset == task.data.size();
      } else {
        // file_offset 已由 sendfile 更新
        // 在这里不需要累加
        task.remaining -= written;
        task.file_sent += written;
        completed = task.remaining == 0;
      }

      if (completed) {
        complete_front();
      }

      continue;
    }

    if (n == 0) {
      // 请求发送的长度大于0
      // 提前读到了EOF
      fail_writes(EIO);
      return;
    }
    const int error = errno;
    if (error == EINTR) {
      continue;
    }

    if (error == EAGAIN || error == EWOULDBLOCK) {
      // 暂时不可写，保留队列和发送进度
      return;
    }
    fail_writes(error);
    return;
  }

  if (!_writes.empty()) {
    // 本轮预算用完
    // 保留写事件监听
    return;
  }

  // 全部任务完成，避免无意义的可写事件。
  _channel->disable_writing();

  if (_write_cb) {
    auto self = shared_from_this();
    auto callback = _write_cb;

    _loop->queue_in_loop([self, callback] {
      callback(self);
    });
  }

  // 优雅关闭：等全部任务发送完，再关闭写方向。
  if (_state.load() == k_disconnecting) {
    shutdown_in_loop();
  }
}

// 使用高水位设置后的修改函数
void TcpConnection::handle_close() {
  if (_state.load() == k_disconnected) {
    return;  // 防止网络关闭事件与 force_close 重复执行
  }

  set_state(k_disconnected);
  _channel->disable_all();
  _reading = false;

  cancel_pending(ECANCELED);

  auto self = shared_from_this();
  // 通知业务层：连接状态已经变化
  if (_conn_cb) {
    _conn_cb(self);
  }

  // 通知管理者：移除这个连接。
  if (_close_cb) {
    _close_cb(self);
  }
}

void TcpConnection::handle_error() {
  char err_buf[512];
  UTIL::memzero(err_buf, sizeof err_buf);
  int err = get_socket_err(_channel->fd());
  const char* err_msg = strerror_r(err, err_buf, sizeof err_buf);
  net_logger()->error("TcpConnection::handle_error [{}] - SO_ERROR = {} {}", _name, err, err_msg);
}

// 新增高水位背压机制后的loop
void TcpConnection::send_in_loop(const void* data, std::size_t len) {
  assert(_loop->is_in_loop_thread());
  // 1. 执行时再次检查连接状态
  if (_state.load() != k_connected || len == 0) {
    return;
  }

  // 2. 检查任务数量和待发送内存上限
  if (_writes.size() >= kMaxPendingTasks || len > kMaxPendingBytes - _pending_memory_bytes) {
    fail_writes(ENOBUFS);
    return;
  }

  // 3. 创建普通数据发送任务，复制数据
  PendingWrite task;
  task.kind = PendingWrite::Kind::memory;
  task.data.assign(static_cast<const char*>(data), len);
  const std::size_t old_size = _pending_memory_bytes;

  // 4. 放入队尾，更新内存统计
  _writes.push_back(std::move(task));
  _pending_memory_bytes += len;

  // 5. 开启socket监听
  enable_write_event();

  // 6. 首次跨过高水位，通知应用层
  if (old_size < _high_water_mark && _pending_memory_bytes >= _high_water_mark && _high_water_cb) {
    auto self = shared_from_this();
    auto cb = _high_water_cb;
    const std::size_t size = _pending_memory_bytes;
    _loop->queue_in_loop([self, cb, size] {
      cb(self, size);
    });
  }
}

void TcpConnection::send_file_in_loop(PendingWrite task, int prepare_error) {
  // 其实这里检查是否在当前loop线程中没有必要
  // 当前tlss的设计应该不会出现阻塞
  // 在tlss中不会在线程池中自己关闭自己
  // 虽然当前线程池设计了优雅退出能力，将主loop放入线程池中
  // 可以通过回调完成自己关闭自己的能力
  assert(_loop->is_in_loop_thread());

  // 1. 执行时再次检查连接状态
  if (_state.load() != k_connected) {
    notify_file(std::move(task), ENOTCONN);
    return;
  }

  // 2.处理上一层准备文件时发生的错误
  if (prepare_error != 0) {
    notify_file(std::move(task), prepare_error);
    fail_writes(prepare_error);
    return;
  }

  // 3. 检查待发送任务数量和文件数量
  if (_writes.size() >= kMaxPendingTasks || _pending_files >= kMaxPendingFiles) {
    notify_file(std::move(task), ENOBUFS);
    fail_writes(ENOBUFS);
    return;
  }

  // 4. 放入队尾
  _writes.push_back(std::move(task));
  ++_pending_files;

  // 5. 等待可写事件，由handle_write()真正发送
  enable_write_event();
}

// 使用了高水位函数后的shutdow
void TcpConnection::shutdow() {
  auto self = shared_from_this();
  _loop->run_in_loop([self] {
    if (self->_state.load() != k_connected) {
      return;
    }

    self->set_state(k_disconnecting);
    self->shutdown_in_loop();
  });
}

void TcpConnection::shutdown_in_loop() {
  if (!_channel->is_writing()) {
    // 此时当前缓冲区已经全部发送完成
    _socket->shutdown_write();
  }
}

void TcpConnection::cancel_pending(int error) {
  while (!_writes.empty()) {
    // 1. 取走队首任务，并从队列移除。
    PendingWrite task = std::move(_writes.front());
    _writes.pop_front();

    // 2. 文件任务需要释放资源，并安排失败回调。
    if (task.kind == PendingWrite::Kind::file) {
      notify_file(std::move(task), error);
    }

    // 普通数据任务在本轮循环结束时析构，
    // 自动释放 task.data 持有的内存。
  }

  // 3. 队列已经清空，重置统计。
  _pending_memory_bytes = 0;
  _pending_files = 0;
}

void TcpConnection::fail_writes(int error) {
  net_logger()->error("TcpConnection [{}] send failed: {}", _name,
                      std::error_code(error, std::generic_category()).message());

  // 让所有待发送文件报告本次失败原因。
  cancel_pending(error);

  // 进入连接关闭流程。
  handle_close();
}

bool TcpConnection::should_read_in_loop() const {
  const StateE state = _state.load();
  if (state == k_connecting || state == k_disconnected) {
    return false;
  }

  // 服务端停机时，允许读取并丢弃数据，以观察 FIN。
  if (_read_eof_only) {
    return true;
  }

  return !_read_paused_for_shutdown && !_send_read_paused && _disk_read_pauses.empty();
}

void TcpConnection::update_read_interest() {
  assert(_loop->is_in_loop_thread());
  const bool wanted = should_read_in_loop() && (_read_eof_only || !_buffered_input_pending);
  if (wanted == _reading) {
    return;
  }

  _reading = wanted;

  if (wanted) {
    _channel->enable_reading();
  } else {
    _channel->disable_reading();
  }
}

void TcpConnection::schedule_buffered_input() {
  assert(_loop->is_in_loop_thread());

  if (_buffered_input_pending || !should_read_in_loop() || _read_eof_only ||
      _read_paused_for_shutdown || _input_buffer.readable_bytes() == 0 || !_mess_cb) {
    return;
  }

  _buffered_input_pending = true;

  // 先处理已经读入 Buffer 的内容，再接收新的 socket 数据。
  update_read_interest();

  std::shared_ptr<TcpConnection> self = shared_from_this();
  EventLoop::Functor cb = [self]() {
    self->_buffered_input_pending = false;
    if (self->should_read_in_loop() && !self->_read_eof_only && !self->_read_paused_for_shutdown &&
        self->_input_buffer.readable_bytes() != 0 && self->_mess_cb) {
      self->_mess_cb(self, &self->_input_buffer, self->_last_receive_time);
    }
    self->update_read_interest();
  };
  _loop->queue_in_loop(cb);
}

void TcpConnection::pause_read_for_disk(std::uint64_t token) {
  if (token == 0) {
    return;
  }

  std::shared_ptr<TcpConnection> self = shared_from_this();
  EventLoop::Functor cb = [self, token] {
    if (self->_state.load() == k_disconnected) {
      return;
    }
    self->_disk_read_pauses.insert(token);
    self->update_read_interest();
  };
  _loop->run_in_loop(cb);
}

void TcpConnection::resume_read_after_disk(std::uint64_t token) {
  if (token == 0) {
    return;
  }

  std::shared_ptr<TcpConnection> self = shared_from_this();
  EventLoop::Functor cb = [self, token] {
    self->_disk_read_pauses.erase(token);
    self->schedule_buffered_input();
    self->update_read_interest();
  };
  _loop->run_in_loop(cb);
}
}  // namespace TLSS::NET
