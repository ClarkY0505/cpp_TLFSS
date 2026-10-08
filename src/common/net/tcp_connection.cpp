#include "common/net/callbacks.h"
#include "common/net/channel.h"
#include "common/net/event_loop.h"
#include "common/net/net_logger.h"
#include "common/net/socket.h"
#include "common/net/tcp_connection.h"
#include "common/utile/utile.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <functional>

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
    , _reading(true)
    , _socket(new Socket(sockfd))
    , _channel(new Channel(loop, sockfd))
    , _local_addr(local_addr)
    , _peer_addr(peer_addr)
    , _high_water_mark(std::size_t{64} * 1024 * 1024) {
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

void TcpConnection::send(const std::string& buf) {
  if (_state == k_connected) {
    if (_loop->is_in_loop_thread()) {
      send_in_loop(buf.c_str(), buf.size());
    } else {
      _loop->run_in_loop(std::bind(&TcpConnection::send_in_loop, this, buf.c_str(), buf.size()));
    }
  }
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
    _mess_cb(shared_from_this(), &_input_buffer, receive_time);
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

void TcpConnection::handle_close() {
  net_logger()->trace("fd = {} state = {}", _channel->fd(), state_to_string());
  set_state(k_disconnected);
  _channel->disable_all();
  TcpConnectionPtr guard_this(shared_from_this());
  _conn_cb(guard_this);
  _close_cb(guard_this);  // 执行由TcpSever 提供的TcpServer::remove_connection回调函数
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
void TcpConnection::send_in_loop(const void* data, size_t len) {
  ssize_t nwrote = 0;
  size_t remaining = len;
  bool fault_err = false;
  if (_state.load() == k_disconnected) {
    net_logger()->warn("disconnected, give up writing");
    return;
  }
  //
  // 表示channel第一次开始写数据，而且缓冲区没有待发送数据
  if (!_channel->is_writing() && _output_buffer.readable_bytes() == 0) {
    nwrote = ::write(_channel->fd(), data, len);
    if (nwrote >= 0) {
      remaining = len - nwrote;
      if (remaining == 0 && _write_cb) {
        // 此时一次性数据发送完成，
        // 就不用在给channel设置epollout事件
        _loop->queue_in_loop(std::bind(_write_cb, shared_from_this()));
      }
    } else {
      nwrote = 0;
      if (errno != EWOULDBLOCK) {
        net_logger()->error("TcpConnection::send_in_loop");
        if (errno == EPIPE || errno == ECONNRESET) {
          fault_err = true;
        }
      }
    }
  }

  if (!fault_err && remaining > 0) {
    // 此时说明当前这一次write,并没有把数据全部发送出去，
    // 剩余的数据需要保存到缓冲区中，
    // 给channel注册epollout事件，poller发现tcp的缓冲区有空间，
    // 会通知相应的sock->channel,调用handle_write回调方法
    // 最终通过handle_write方法，把缓冲区中所有数据发送完成
    size_t old_len = _output_buffer.readable_bytes();
    if (old_len + remaining >= _high_water_mark && old_len < _high_water_mark && _high_water_cb) {
      _loop->queue_in_loop(std::bind(_high_water_cb, shared_from_this(), old_len + remaining));
    }
    _output_buffer.append(static_cast<const char*>(data) + nwrote, remaining);
    if (!_channel->is_writing()) {
      _channel->enable_writing();
    }
  }
}

void TcpConnection::shutdow() {
  if (_state == k_connected) {
    set_state(k_disconnecting);
    _loop->run_in_loop(std::bind(&TcpConnection::shutdown_in_loop, this));
  }
}

void TcpConnection::shutdown_in_loop() {
  if (!_channel->is_writing()) {
    // 此时当前缓冲区已经全部发送完成
    _socket->shutdown_write();
  }
}

}  // namespace TLSS::NET
