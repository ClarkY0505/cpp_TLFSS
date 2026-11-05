#include "common/net/tcp_connection.h"

#include "common/net/write_scheduler.h"

#include "common/net/channel.h"
#include "common/net/event_loop.h"
#include "common/net/net_logger.h"
#include "common/net/socket.h"
#include "common/utile/utile.h"
#include "spdlog/common.h"

#include <sys/socket.h>

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <utility>

namespace TLSS::NET {

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
EventLoop* check_loop_not_null(EventLoop* loop) {
  if (loop == nullptr) {
    net_logger()->critical("{}:{}:{} TcpConnection loop is null", __FILE__, __FUNCTION__, __LINE__);
    std::abort();
  }

  return loop;
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
    , _mess_cb(defaultMessageCallback) {
  // 停机读取标志从 false 开始；读写事件始终由连接所属的 loop 处理。
  // Poller 给channel通知感兴趣的事件发生了; channel会回调相应的操作函数
  _channel->set_read_cb(std::bind(&TcpConnection::handle_read, this, std::placeholders::_1));
  _channel->set_write_cb(std::bind(&TcpConnection::handle_write, this));
  _channel->set_close_cb(std::bind(&TcpConnection::handle_close, this));
  _channel->set_error_cb(std::bind(&TcpConnection::handle_error, this));

  WriteScheduler::Hooks hooks;
  hooks.write_interest = [this](bool enabled) {
    if (enabled && _state.load() != k_disconnected && !_channel->is_writing())
      _channel->enable_writing();
    else if (!enabled && _channel->is_writing())
      _channel->disable_writing();
  };
  hooks.high_water = [this](std::size_t bytes) {
    on_write_high_water(bytes);
  };
  hooks.drained = [this] {
    on_write_drained();
  };
  hooks.failed = [this](int error) {
    fail_writes(error);
  };
  hooks.make_read_completion = [this](std::uint64_t sequence) {
    return make_file_read_completion(sequence);
  };
  _write_scheduler = std::make_unique<WriteScheduler>(_loop, sockfd, std::move(hooks));
  net_logger()->info("TcpConnection::ctor[{}] at {} fd = {}", _name, static_cast<const void*>(this),
                     sockfd);
  _socket->set_keep_alive(true);
}

TcpConnection::~TcpConnection() {
  net_logger()->info("TcpConnection::dtor[{}] at {} fd = {} state = {}", _name,
                     static_cast<const void*>(this), _channel->fd(), state_to_string());
  _write_scheduler->cancel_all(ECANCELED);
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
    send_in_loop(std::move(message));
    return;
  }

  // 回调持有数据和连接，避免 c_str()、this 在执行前失效。
  auto self = shared_from_this();
  _loop->queue_in_loop([self, message = std::move(message)]() mutable {
    self->send_in_loop(std::move(message));
  });
}

void TcpConnection::send_file(int fd, off_t offset, uint64_t count, FileSendCallback cb) {
  auto prepared = WriteScheduler::prepare_file(fd, offset, count, std::move(cb));
  auto self = shared_from_this();
  _loop->run_in_loop([self, prepared = std::move(prepared)]() mutable {
    self->_write_scheduler->enqueue_file(std::move(prepared), self->connected());
  });
}

void TcpConnection::send_file_async(off_t offset, std::uint64_t count, FileReadFunction reader,
                                    FileSendCallback cb) {
  auto prepared = WriteScheduler::prepare_async(offset, count, std::move(reader), std::move(cb));
  auto self = shared_from_this();
  _loop->run_in_loop([self, prepared = std::move(prepared)]() mutable {
    self->_write_scheduler->enqueue_file(std::move(prepared), self->connected());
  });
}

TcpConnection::FileReadCompletion TcpConnection::make_file_read_completion(std::uint64_t sequence) {
  auto weak = weak_from_this();
  return [weak, sequence](std::error_code error, std::string data) {
    if (auto conn = weak.lock()) {
      // Always queue, including synchronous reader completion. Each invocation holds
      // the connection alive while its internal scheduler processes the result.
      conn->_loop->queue_in_loop([weak, sequence, error, data = std::move(data)]() mutable {
        if (auto owner = weak.lock())
          owner->_write_scheduler->on_file_read(sequence, error, std::move(data));
      });
    }
  };
}

void TcpConnection::set_high_water_mark(std::size_t bytes) {
  _write_scheduler->set_high_water_mark(bytes);
}

// 增加高水位函数后对应策略
void TcpConnection::stop_read() {
  std::shared_ptr<TcpConnection> self = shared_from_this();
  EventLoop::Functor cb = [self] {
    if (self->_state.load() != k_connected || self->_write_scheduler->pending_memory_bytes() <
                                                  self->_write_scheduler->high_water_mark()) {
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
    if (self->_state.load() != k_connected || self->_write_scheduler->pending_memory_bytes() != 0) {
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
  _write_scheduler->cancel_all(ECANCELED);
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

void TcpConnection::handle_write() {
  assert(_loop->is_in_loop_thread());
  if (_state.load() != k_disconnected && _channel->is_writing())
    _write_scheduler->on_writable();
}

void TcpConnection::on_write_drained() {
  if (_write_cb) {
    auto self = shared_from_this();
    auto callback = _write_cb;
    _loop->queue_in_loop([self, callback] {
      callback(self);
    });
  }
  if (_state.load() == k_disconnecting)
    shutdown_in_loop();
}

void TcpConnection::on_write_high_water(std::size_t bytes) {
  if (_high_water_cb) {
    auto self = shared_from_this();
    auto callback = _high_water_cb;
    _loop->queue_in_loop([self, callback, bytes] {
      callback(self, bytes);
    });
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

  _write_scheduler->cancel_all(ECANCELED);

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

void TcpConnection::send_in_loop(std::string data) {
  assert(_loop->is_in_loop_thread());
  if (_state.load() == k_connected)
    _write_scheduler->enqueue_memory(std::move(data));
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
  // 等待读盘时 EPOLLOUT 已关闭，但发送队列可能仍未排空。
  if (_write_scheduler->empty()) {
    _socket->shutdown_write();
  }
}

void TcpConnection::fail_writes(int error) {
  net_logger()->error("TcpConnection [{}] send failed: {}", _name,
                      std::error_code(error, std::generic_category()).message());

  // 让所有待发送文件报告本次失败原因。
  _write_scheduler->cancel_all(error);

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
