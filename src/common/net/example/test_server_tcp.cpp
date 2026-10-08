#include <functional>
#include "common/net/event_loop.h"
#include "common/net/inet_address.h"
#include "common/net/tcp_server.h"
#include "logger/logger.h"
#include "spdlog/common.h"
using namespace TLSS::NET;

inline const TLSSLOG::Logger::LoggerPtr& server_logger() {
  static const auto logger = TLSSLOG::Logger::get("server");
  return logger;
}

class EchoServer {
 public:
  EchoServer(EventLoop* loop, const InetAddress& addr, const std::string& name)
      : _loop(loop)
      , _server(_loop, addr, name) {
    // 注册回调函数
    _server.set_connection_cb(std::bind(&EchoServer::on_connection, this, std::placeholders::_1));
    _server.set_message_cb(std::bind(&EchoServer::on_message, this, std::placeholders::_1,
                                     std::placeholders::_2, std::placeholders::_3));

    // 设置合适的loop线程
    _server.set_thread_num(3);
  }
  void start() {
    _server.start();
  }

 private:
  // 连接建立 断开连接的回调
  void on_connection(const TcpConnectionPtr& conn) {
    if (conn->connected()) {
      server_logger()->info("conn up : {}", conn->peer_addr().to_ip_port());
    } else {
      server_logger()->info("conn down : {}", conn->peer_addr().to_ip_port().c_str());
    }
  }

  // 可读写事件回调
  void on_message(const TcpConnectionPtr& conn, Buffer* buf, TLSS::TIME::Timestamp) {
    std::string msg = buf->retrieve_all_as_string();
    // 终端客户端按回车时，通常会附带 LF 或 CRLF。
    if (msg == "q" || msg == "q\n" || msg == "q\r\n") {
      conn->send(msg);
      conn->shutdow();
    } else {
      conn->send(msg);
    }
  }

  EventLoop* _loop;
  TcpServer _server;
};

int main() {
  TLSSLOG::Logger::initBoth("net", "logs/net.log", spdlog::level::info);
  TLSSLOG::Logger::initBoth("server", "logs/server.log", spdlog::level::info);
  TLSSLOG::Logger::get("net")->flush_on(spdlog::level::info);
  TLSSLOG::Logger::get("server")->flush_on(spdlog::level::info);

  EventLoop loop;
  InetAddress addr(8000);
  EchoServer server(&loop, addr, "EchoServer");
  server.start();
  loop.loop();

  return 0;
}
