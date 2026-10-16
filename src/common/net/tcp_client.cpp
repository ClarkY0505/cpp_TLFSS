#include "common/net/connector.h"
#include "common/net/net_logger.h"
#include "common/net/tcp_client.h"

#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace TLSS::NET {
namespace {
void destroy_connection(EventLoop* loop, const TcpConnectionPtr& conn) {
  // Channel 可能仍在本轮事件分发栈中，等事件处理结束后再注销。
  loop->queue_in_loop([conn] {
    conn->connect_destroyed();
  });
}
}  // namespace

struct TcpClient::State : std::enable_shared_from_this<TcpClient::State> {
  State(EventLoop* loop, const InetAddress& server_addr, const std::string& name)
      : _loop(loop)
      , _connector(std::make_shared<Connector>(loop, server_addr))
      , _name(name) {}

  TcpConnectionPtr connection() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _connection;
  }

  void new_connection(int sockfd) {
    assert(_loop->is_in_loop_thread());
    ConnectionCb connection_cb;
    MessageCb message_cb;
    WriteCompleteCb write_cb;
    {
      std::lock_guard<std::mutex> lock(_mutex);
      // stop 是异步的，已经开始的 Connector 回调仍可能到达这里。
      // socket 尚未移交给 TcpConnection，取消时由本回调负责关闭。
      if (_closing || !_connect || _connection) {
        ::close(sockfd);
        return;
      }
      connection_cb = _conn_cb;
      message_cb = _mess_cb;
      write_cb = _write_cb;
    }

    sockaddr_in local{};
    sockaddr_in peer{};
    socklen_t local_size = sizeof local;
    socklen_t peer_size = sizeof peer;
    if (::getsockname(sockfd, reinterpret_cast<sockaddr*>(&local), &local_size) < 0 ||
        ::getpeername(sockfd, reinterpret_cast<sockaddr*>(&peer), &peer_size) < 0) {
      net_logger()->error("TcpClient [{}] failed to get connection addresses", _name);
      ::close(sockfd);
      std::lock_guard<std::mutex> lock(_mutex);
      _connector->stop();
      return;
    }

    const InetAddress peer_addr(peer);
    const std::string conn_name =
        _name + ":" + peer_addr.to_ip_port() + "#" + std::to_string(_next_conn_id++);
    // 构造和业务回调都在锁外，避免用户回调重入或耗时构造阻塞析构。
    // 外部可在任意线程释放最后一个引用，真正销毁连接仍交回所属 loop。
    TcpConnectionPtr conn(
        new TcpConnection(_loop, conn_name, sockfd, InetAddress(local), peer_addr),
        [loop = _loop](TcpConnection* connection) {
          loop->run_in_loop([connection] {
            delete connection;
          });
        });
    conn->set_connection_cb(connection_cb);
    conn->set_message_cb(message_cb);
    conn->set_write_complete_cb(write_cb);
    std::weak_ptr<State> weak = shared_from_this();
    conn->set_close_cb([weak, loop = _loop](const TcpConnectionPtr& closed) {
      if (auto state = weak.lock()) {
        state->remove_connection(closed);
      } else {
        // 客户端已离开，连接仍可以独立完成 Channel 注销。
        destroy_connection(loop, closed);
      }
    });
    {
      std::lock_guard<std::mutex> lock(_mutex);
      // 构造期间可能发生取消：发布连接与析构移交使用同一把锁。
      // 析构先拿锁则拒绝发布；发布先拿锁则由析构接管该连接。
      if (_closing || !_connect || _connection) {
        return;
      }
      _connection = conn;
    }
    conn->connect_established();
  }

  void remove_connection(const TcpConnectionPtr& conn) {
    assert(_loop->is_in_loop_thread());
    {
      std::lock_guard<std::mutex> lock(_mutex);
      if (_connection == conn) {
        _connection.reset();
        // 判断重连和提交命令必须在同一临界区，避免判断后被 stop 越过。
        _connector->stop();
        if (!_closing && _retry && _connect) {
          _connector->restart();
        }
      }
    }
    destroy_connection(_loop, conn);
  }

  EventLoop* const _loop;
  const std::shared_ptr<Connector> _connector;
  const std::string _name;
  std::uint64_t _next_conn_id{1};  // 只在 loop 线程递增。

  // 下列状态均由同一把锁保护；锁内只提交异步任务，不调用业务回调。
  mutable std::mutex _mutex;
  ConnectionCb _conn_cb{defaultConnectionCallback};
  MessageCb _mess_cb{defaultMessageCallback};
  WriteCompleteCb _write_cb;
  bool _retry{false};
  bool _connect{false};
  bool _closing{false};
  TcpConnectionPtr _connection;
};

TcpClient::TcpClient(EventLoop* loop, const InetAddress& server_addr, const std::string& name_arg) {
  if (loop == nullptr) {
    throw std::invalid_argument("TcpClient loop cannot be null");
  }
  _state = std::make_shared<State>(loop, server_addr, name_arg);
  std::weak_ptr<State> weak = _state;
  _state->_connector->set_new_connection_cb([weak](int sockfd) {
    // lock 成功则 State 活到回调结束；失败时关闭未被接管的 socket。
    // 弱引用同时避免 State -> Connector -> 回调 -> State 的强引用环。
    if (auto state = weak.lock()) {
      state->new_connection(sockfd);
    } else {
      ::close(sockfd);
    }
  });
}

TcpClient::~TcpClient() {
  auto state = std::move(_state);
  TcpConnectionPtr conn;
  {
    std::lock_guard<std::mutex> lock(state->_mutex);
    state->_closing = true;
    state->_connect = false;
    // 即使已有连接也停止 Connector，统一取消建连及重试资源。
    state->_connector->stop();
    conn = std::move(state->_connection);
  }
  if (conn) {
    // 不再捕获 this；等当前事件回调的临时引用释放后再判断连接所有权。
    // 用户仍持有连接时允许继续使用，否则主动关闭。
    state->_loop->queue_in_loop([loop = state->_loop, conn = std::move(conn)] {
      conn->set_close_cb([loop](const TcpConnectionPtr& closed) {
        destroy_connection(loop, closed);
      });
      if (conn.use_count() == 1) {
        conn->force_close();
      }
    });
  }
  // stop 和 Channel 清理任务已经持有 Connector，无需空任务或固定延时保活。
}

void TcpClient::connect() {
  auto state = _state;
  std::lock_guard<std::mutex> lock(state->_mutex);
  state->_connect = true;
  state->_loop->queue_in_loop([state] {
    std::lock_guard<std::mutex> lock(state->_mutex);
    // 析构或 stop 可以取消尚未执行的启动，且禁止覆盖已有连接。
    if (!state->_closing && state->_connect && !state->_connection) {
      state->_connector->start();
    }
  });
}

void TcpClient::disconnect() {
  auto state = _state;
  std::lock_guard<std::mutex> lock(state->_mutex);
  state->_connect = false;
  state->_connector->stop();
  if (auto conn = state->_connection) {
    // 捕获当前连接，不在稍后的任务里误关闭后续新建的连接。
    state->_loop->queue_in_loop([conn] {
      conn->shutdow();
    });
  }
}

void TcpClient::stop() {
  auto state = _state;
  std::lock_guard<std::mutex> lock(state->_mutex);
  state->_connect = false;
  state->_connector->stop();
}

TcpConnectionPtr TcpClient::connection() const {
  return _state->connection();
}

EventLoop* TcpClient::get_loop() const {
  return _state->_loop;
}

const std::string& TcpClient::name() const {
  return _state->_name;
}

bool TcpClient::retry() const {
  std::lock_guard<std::mutex> lock(_state->_mutex);
  return _state->_retry;
}

void TcpClient::enable_retry() {
  std::lock_guard<std::mutex> lock(_state->_mutex);
  _state->_retry = true;
}

void TcpClient::set_connection_cb(const ConnectionCb& cb) {
  ConnectionCb replacement = cb ? cb : ConnectionCb(defaultConnectionCallback);
  {
    std::lock_guard<std::mutex> lock(_state->_mutex);
    _state->_conn_cb.swap(replacement);
  }
  // 旧回调捕获对象的析构也可能重入客户端，必须在释放状态锁后执行。
}

void TcpClient::set_message_cb(const MessageCb& cb) {
  MessageCb replacement = cb ? cb : MessageCb(defaultMessageCallback);
  {
    std::lock_guard<std::mutex> lock(_state->_mutex);
    _state->_mess_cb.swap(replacement);
  }
}

void TcpClient::set_write_complete_cb(const WriteCompleteCb& cb) {
  WriteCompleteCb replacement = cb;
  {
    std::lock_guard<std::mutex> lock(_state->_mutex);
    _state->_write_cb.swap(replacement);
  }
}
}  // namespace TLSS::NET
