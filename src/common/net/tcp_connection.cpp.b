#include "common/net/tcp_connection.h"

#include "common/net/channel.h"
#include "common/net/event_loop.h"
#include "common/net/net_logger.h"
#include "common/net/socket.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdlib>
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

  ::sigemptyset(&blocked);
  ::sigaddset(&blocked, SIGPIPE);

  const int mask_error = ::pthread_sigmask(SIG_BLOCK, &blocked, &old_mask);

  if (mask_error != 0) {
    errno = mask_error;
    return -1;
  }

  if (::sigpending(&pending) == -1) {
    const int saved_error = errno;
    ::pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
    errno = saved_error;
    return -1;
  }

  const bool already_pending = ::sigismember(&pending, SIGPIPE) == 1;
  const ssize_t result = ::sendfile(socket_fd, file_fd, offset, count);
  const int saved_error = errno;

  // 不消费调用前就已经存在的 SIGPIPE。
  if (result == -1 && saved_error == EPIPE && !already_pending) {
    const timespec timeout{0, 0};

    while (::sigtimedwait(&blocked, nullptr, &timeout) == -1 && errno == EINTR) {
    }
  }

  ::pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);

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
    , _reading(true)
    , _read_paused_for_shutdown(false)
    , _socket(new Socket(sockfd))
    , _channel(new Channel(loop, sockfd))
    , _local_addr(local_addr)
    , _peer_addr(peer_addr)
    , _conn_cb(defaultConnectionCallback)
    , _mess_cb(defaultMessageCallback)
    , _high_water_mark(std::size_t{64} * 1024 * 1024) {
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

/* void TcpConnection::send(const void* message, int len) {} */

// 增加高水位回调后，使用该方法可能存在 buf.c_str()在执行前失效
/* void TcpConnection::send(const std::string& buf) { */
/*   if (_state == k_connected) { */
/*     if (_loop->is_in_loop_thread()) { */
/*       send_in_loop(buf.c_str(), buf.size()); */
/*     } else { */
/*       _loop->run_in_loop(std::bind(&TcpConnection::send_in_loop, this, buf.c_str(), buf.size()));
 */
/*     } */
/*   } */
/* } */

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

void TcpConnection::set_high_water_mark(std::size_t bytes) {
  if (!_loop->is_in_loop_thread() || bytes == 0 || bytes > kMaxPendingBytes) {
    net_logger()->error("invalid high water mark");
    std::abort();
  }
  _high_water_mark = bytes;
}

// 增加高水位函数后对应策略
void TcpConnection::stop_read() {
  auto self = shared_from_this();
  _loop->run_in_loop([self] {
    // 回调可能排队了一段时间；执行时重新检查水位。
    if (self->_state.load() != k_connected || !self->_reading ||
        self->_output_buffer.readable_bytes() < self->_high_water_mark) {
      return;
    }

    self->_reading = false;
    self->_channel->disable_reading();
  });
}

// 增加高水位函数后对应策略
void TcpConnection::start_read() {
  auto self = shared_from_this();
  _loop->run_in_loop([self] {
    // 写缓冲区排空后可恢复读取，但停机暂停标志优先于普通背压控制。
    if (self->_state.load() != k_connected || self->_reading || self->_read_paused_for_shutdown ||
        self->_output_buffer.readable_bytes() != 0) {
      return;
    }

    self->_reading = true;
    self->_channel->enable_reading();
  });
}

void TcpConnection::pause_read_for_shutdown() {
  auto self = shared_from_this();
  _loop->run_in_loop([self] {
    // 在所属 loop 中设置标志，阻止已经排队的写完成回调重新打开读取。
    self->_read_paused_for_shutdown = true;
    if (self->_reading && self->_state.load() == k_connected) {
      self->_reading = false;
      self->_channel->disable_reading();
    }
  });
}

void TcpConnection::shutdown_for_server_stop() {
  auto self = shared_from_this();
  _loop->run_in_loop([self] {
    if (self->_state.load() != k_connected) {
      return;
    }
    self->set_state(k_disconnecting);
    // 重新监听读事件只为识别对端 FIN；收到的数据会被丢弃。
    if (!self->_reading) {
      self->_reading = true;
      self->_channel->enable_reading();
    }
    // 若输出缓冲区仍有数据，实际关闭写端由 handle_write 在排空后完成。
    self->shutdown_in_loop();
  });
}

// 增加高水位函数后对应策略
void TcpConnection::force_close() {
  auto self = shared_from_this();
  _loop->queue_in_loop([self] {
    self->handle_close();
  });
}

void TcpConnection::connect_established() {
  set_state(k_connected);
  _channel->tie(shared_from_this());
  _channel->enable_reading();

  _conn_cb(shared_from_this());
}

void TcpConnection::connect_destroyed() {
  if (_state == k_connected) {
    set_state(k_disconnected);
    // 把channel所有感兴趣的事件，从poller中del
    _channel->disable_all();
    _conn_cb(shared_from_this());
  }
  //
  // 把channel从poller中remove
  _channel->remove();
}

void TcpConnection::handle_read(TIME::Timestamp receive_time) {
  int save_err = 0;
  ssize_t n = _input_buffer.read_fd(_channel->fd(), &save_err);
  if (n > 0) {
    // 停机阶段仍读 socket 以捕获 EOF，但不再把新数据交给业务回调。
    if (_read_paused_for_shutdown) {
      _input_buffer.retrieve_all();
    } else {
      _mess_cb(shared_from_this(), &_input_buffer, receive_time);
    }
  } else if (n == 0) {
    handle_close();
  } else {
    errno = save_err;
    net_logger()->error("TcpConnection::handle_read");
    handle_error();
  }
}

void TcpConnection::handle_write() {
  int save_err = 0;
  if (_channel->is_writing()) {
    ssize_t n = _output_buffer.write_fd(_channel->fd(), &save_err);
    if (n > 0) {
      _output_buffer.retrieve(n);
      if (_output_buffer.readable_bytes() == 0) {
        _channel->disable_writing();
        if (_write_cb) {
          // 唤醒_loop对应的thread线程，执行回调
          _loop->queue_in_loop(std::bind(_write_cb, shared_from_this()));
        }
        if (_state.load() == k_disconnecting) {
          shutdown_in_loop();
        }
      }
    } else {
      net_logger()->error("TcpConnection::handle_write");
    }
  } else {
    net_logger()->error("TcpConnection fd = {} is down, no more writing", _channel->fd());
  }
}

/* void TcpConnection::handle_close() { */
/*   net_logger()->trace("fd = {} state = {}", _channel->fd(), state_to_string()); */
/*   set_state(k_disconnected); */
/*   _channel->disable_all(); */
/*   TcpConnectionPtr guard_this(shared_from_this()); */
/*   _conn_cb(guard_this); */
/*   _close_cb(guard_this);  // 执行由TcpSever 提供的TcpServer::remove_connection回调函数 */
/* } */

// 使用高水位设置后的修改函数
void TcpConnection::handle_close() {
  if (_state.load() == k_disconnected) {
    return;  // 防止网络关闭事件与 force_close 重复执行
  }

  set_state(k_disconnected);
  _channel->disable_all();

  auto self = shared_from_this();
  _conn_cb(self);
  _close_cb(self);
}

void TcpConnection::handle_error() {
  char err_buf[512];
  UTIL::memzero(err_buf, sizeof err_buf);
  int err = get_socket_err(_channel->fd());
  const char* err_msg = strerror_r(err, err_buf, sizeof err_buf);
  net_logger()->error("TcpConnection::handle_error [{}] - SO_ERROR = {} {}", _name, err, err_msg);
}

//
// 发送数据 应用写的快，内核发送数据慢
// 需要把待发送数据写入缓冲区
// 并且设置水位回调,防止发送过快
/* void TcpConnection::send_in_loop(const void* data, size_t len) { */
/*   ssize_t nwrote = 0; */
/*   size_t remaining = len; */
/*   bool fault_err = false; */
/*   if (_state.load() == k_disconnected) { */
/*     net_logger()->warn("disconnected, give up writing"); */
/*     return; */
/*   } */
/*   // */
/*   // 表示channel第一次开始写数据，而且缓冲区没有待发送数据 */
/*   if (!_channel->is_writing() && _output_buffer.readable_bytes() == 0) { */
/*     nwrote = ::write(_channel->fd(), data, len); */
/*     if (nwrote >= 0) { */
/*       remaining = len - nwrote; */
/*       if (remaining == 0 && _write_cb) { */
/*         // 此时一次性数据发送完成， */
/*         // 就不用在给channel设置epollout事件 */
/*         _loop->queue_in_loop(std::bind(_write_cb, shared_from_this())); */
/*       } */
/*     } else { */
/*       nwrote = 0; */
/*       if (errno != EWOULDBLOCK) { */
/*         net_logger()->error("TcpConnection::send_in_loop"); */
/*         if (errno == EPIPE || errno == ECONNRESET) { */
/*           fault_err = true; */
/*         } */
/*       } */
/*     } */
/*   } */

/*   if (!fault_err && remaining > 0) { */
/*     // 此时说明当前这一次write,并没有把数据全部发送出去， */
/*     // 剩余的数据需要保存到缓冲区中， */
/*     // 给channel注册epollout事件，poller发现tcp的缓冲区有空间， */
/*     // 会通知相应的sock->channel,调用handle_write回调方法 */
/*     // 最终通过handle_write方法，把缓冲区中所有数据发送完成 */
/*     size_t old_len = _output_buffer.readable_bytes(); */
/*     if (old_len + remaining >= _high_water_mark && old_len < _high_water_mark && _high_water_cb)
 * { */
/*       _loop->queue_in_loop(std::bind(_high_water_cb, shared_from_this(), old_len + remaining));
 */
/*     } */
/*     _output_buffer.append(static_cast<const char*>(data) + nwrote, remaining); */
/*     if (!_channel->is_writing()) { */
/*       _channel->enable_writing(); */
/*     } */
/*   } */
/* } */

/* void TcpConnection::shutdow() { */
/*   if (_state == k_connected) { */
/*     set_state(k_disconnecting); */
/*     _loop->run_in_loop(std::bind(&TcpConnection::shutdown_in_loop, this)); */
/*   } */
/* } */

// 新增高水位背压机制后的loop
void TcpConnection::send_in_loop(const void* data, std::size_t len) {
  if (_state.load() != k_connected) {
    return;  // 排队期间连接可能已经关闭
  }

  const std::size_t queued = _output_buffer.readable_bytes();
  if (len > kMaxPendingBytes - queued) {
    force_close();
    return;
  }

  ssize_t written = 0;
  std::size_t remaining = len;

  if (!_channel->is_writing() && queued == 0) {
    // 失败可能产生 SIGPIPE这个问题
    /* written = ::write(_channel->fd(), data, len); */
    written = ::send(_channel->fd(), data, len, MSG_NOSIGNAL);

    if (written < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        written = 0;
      } else {
        force_close();
        return;
      }
    } else {
      remaining -= static_cast<std::size_t>(written);
      if (remaining == 0 && _write_cb) {
        _loop->queue_in_loop(std::bind(_write_cb, shared_from_this()));
      }
    }
  }

  if (remaining == 0) {
    return;
  }

  const std::size_t old_size = _output_buffer.readable_bytes();
  _output_buffer.append(static_cast<const char*>(data) + written, remaining);
  const std::size_t new_size = _output_buffer.readable_bytes();

  // 沿用已有的“首次跨过高水位”通知机制。
  if (old_size < _high_water_mark && new_size >= _high_water_mark && _high_water_cb) {
    _loop->queue_in_loop(std::bind(_high_water_cb, shared_from_this(), new_size));
  }

  if (!_channel->is_writing()) {
    _channel->enable_writing();
  }
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

}  // namespace TLSS::NET
