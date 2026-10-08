#include "common/net/acceptor.h"
#include "common/net/callbacks.h"
#include "common/net/event_loop.h"
#include "common/net/event_loop_thread_pool.h"
#include "common/net/inet_address.h"
#include "common/net/net_logger.h"
#include "common/net/tcp_connection.h"
#include "common/net/tcp_server.h"
#include "common/utile/utile.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <functional>
namespace TLSS::NET {
namespace {
EventLoop* check_loop_not_null(EventLoop* loop) {
  if (loop == nullptr) {
    net_logger()->critical("{}:{}:{} mainloop is null", __FILE__, __FUNCTION__, __LINE__);
    std::abort();
  }

  return loop;
}

sockaddr_in get_local_addr(int sockfd) {
  sockaddr_in local_addr;
  UTIL::memzero(&local_addr, sizeof local_addr);
  socklen_t addr_len = static_cast<socklen_t>(sizeof local_addr);
  if (::getsockname(sockfd, reinterpret_cast<sockaddr*>(&local_addr), &addr_len) < 0) {
    net_logger()->error("{}:{}:{} getsockname", __FILE__, __FUNCTION__, __LINE__);
  }
  return local_addr;
}

}  // namespace

TcpServer::TcpServer(EventLoop* loop, const InetAddress& listen_addr, const std::string& name_arg,
                     Option option)
    : _loop(check_loop_not_null(loop))
    , _ip_port(listen_addr.to_ip_port())
    , _name(name_arg)
    , _acceptor(new Acceptor(loop, listen_addr, option == k_reuse_port))
    , _thread_pool(new EventLoopThreadPool(loop, _name))
    , _conn_cb(defaultConnectionCallback)
    , _mess_cb(defaultMessageCallback)
    , _started(0)
    , _next_conn_id(1) {
  // 当有新连接来临的时候回执行这个函数TcpServer::new_connection
  _acceptor->set_new_connection_callback(
      std::bind(&TcpServer::new_connection, this, std::placeholders::_1, std::placeholders::_2));
}

TcpServer::~TcpServer() {
  net_logger()->trace("TcpServer::~TcpServer [{}] destructing", _name);
  for (auto& it : _connections) {
    // TcpConnectionPtr它本身是一个智能指针
    // 通过这个方法可以自动释放map中存放的conn指针
    // 执行完销毁链接后
    // 离开这个作用域自动释放
    TcpConnectionPtr conn(it.second);
    it.second.reset();
    conn->get_loop()->run_in_loop(std::bind(&TcpConnection::connect_destroyed, conn));
  }
}

void TcpServer::set_thread_num(int num) {
  _thread_pool->set_thread_num(num);
}

void TcpServer::start() {
  int expected_value = 0;
  // 防止一个tcpserver对象多次被start
  if (_started.compare_exchange_strong(expected_value, 1)) {
    // 启动底层线程池
    _thread_pool->start(_thread_init_cb);
    _loop->run_in_loop(std::bind(&Acceptor::listen, _acceptor.get()));
  }
}

//
// 一个新客户的连接，就会调用这个回调函数
void TcpServer::new_connection(int sockfd, const InetAddress& peer_addr) {
  // 轮询方式，选择一个subloop来管理当前channel
  EventLoop* io_loop = _thread_pool->get_next_loop();
  char buf[64];
  snprintf(buf, sizeof buf, "-%s%d", _ip_port.c_str(), _next_conn_id);
  ++_next_conn_id;
  std::string conn_name = _name + buf;
  net_logger()->info("TcpServer::new_connection [{}] - new connection [{}] from {}", _name,
                     conn_name, peer_addr.to_ip_port());
  //
  // 通过socketfd获取绑定的本机的ip端口信息
  InetAddress local_addr(get_local_addr(sockfd));
  //
  // 根据连接成功的sockfd,创建tcp connection 对象
  TcpConnectionPtr conn(new TcpConnection(io_loop, conn_name, sockfd, local_addr, peer_addr));
  _connections[conn_name] = conn;
  //
  // 回调函数都是由用户设置给tcpsever => tcpConnection =>
  // Channel => Poller => notify channel调用回调
  conn->set_connection_cb(_conn_cb);
  conn->set_message_cb(_mess_cb);
  conn->set_write_complete_cb(_write_cb);

  // 设置如何关闭连接的回调
  conn->set_close_cb(std::bind(&TcpServer::remove_connection, this, std::placeholders::_1));

  io_loop->run_in_loop(std::bind(&TcpConnection::connect_established, conn));
}

void TcpServer::remove_connection(const TcpConnectionPtr& conn) {
  _loop->run_in_loop(std::bind(&TcpServer::remove_connection_in_loop, this, conn));
}

void TcpServer::remove_connection_in_loop(const TcpConnectionPtr& conn) {
  net_logger()->info("TcpServer::remove_connection_in_loop [{}] - connection {}", _name,
                     conn->name());
  size_t n = _connections.erase(conn->name());
  (void)n;
  EventLoop* io_loop = conn->get_loop();
  io_loop->queue_in_loop(std::bind(&TcpConnection::connect_destroyed, conn));
}

}  // namespace TLSS::NET
