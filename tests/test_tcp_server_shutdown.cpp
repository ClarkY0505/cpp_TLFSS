#include "common/net/event_loop.h"
#include "common/net/inet_address.h"
#include "common/net/socket.h"
#include "common/net/tcp_server.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <future>
#include <thread>

namespace {
using namespace std::chrono_literals;

// 断言失败直接终止测试进程，避免后续网络操作掩盖首个失败点。 / Terminate the test process immediately on assertion failure so later network operations cannot hide the first failure.
void require(bool condition) {
  if (!condition) {
    std::abort();
  }
}

// 让内核分配本机临时端口，供每个停机场景独立监听。 / Let the kernel assign a local ephemeral port for each shutdown scenario to listen independently.
uint16_t reserve_port() {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  require(fd >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
  socklen_t length = sizeof address;
  require(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
  const auto port = ntohs(address.sin_port);
  ::close(fd);
  return port;
}

// 建立测试客户端；连接失败时返回 -1，便于验证监听已关闭。 / Create a test client; return -1 on connection failure to verify that the listener has closed.
int connect_to(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  require(fd >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}
}  // namespace / 命名空间结束

// 验证停止接收后新连接被拒绝，旧连接完成半关闭并等待对端 FIN。 / Verify that new connections are rejected after accepting stops, while existing connections half-close and wait for the peer's FIN.
void graceful_close_after_peer_fin() {
  const uint16_t port = reserve_port();
  TLSS::NET::EventLoop loop;
  TLSS::NET::TcpServer server(&loop, TLSS::NET::InetAddress(port), "shutdown-test");
  server.set_thread_num(1);

  std::promise<void> accepted;
  server.set_connection_cb([&](const TLSS::NET::TcpConnectionPtr& connection) {
    if (connection->connected()) {
      accepted.set_value();
    }
  });
  server.start();

  std::thread controller([&] {
    int client = connect_to(port);
    require(client >= 0);
    require(accepted.get_future().wait_for(5s) == std::future_status::ready);

    std::promise<void> paused;
    auto paused_future = paused.get_future();
    server.stop_accepting([&] { paused.set_value(); });
    require(paused_future.wait_for(5s) == std::future_status::ready);
    require(connect_to(port) < 0);

    std::promise<void> drained;
    auto drained_future = drained.get_future();
    server.shutdown_connections([&] { drained.set_value(); });
    char byte = 0;
    require(::recv(client, &byte, 1, 0) == 0);
    ::close(client);
    require(drained_future.wait_for(5s) == std::future_status::ready);

    server.stop_io_threads();
    loop.quit();
  });

  loop.loop();
  controller.join();
}

// 验证零 IO 工作线程时，未主动关闭的客户端可被超时路径强制清理。 / Verify that with zero IO worker threads, the timeout path forcibly cleans up clients that do not close themselves.
void force_close_idle_peer_without_io_threads() {
  const uint16_t port = reserve_port();
  TLSS::NET::EventLoop loop;
  TLSS::NET::TcpServer server(&loop, TLSS::NET::InetAddress(port), "force-close-test");
  server.set_thread_num(0);
  std::promise<void> accepted;
  server.set_connection_cb([&](const TLSS::NET::TcpConnectionPtr& connection) {
    if (connection->connected()) {
      accepted.set_value();
    }
  });
  server.start();

  std::thread controller([&] {
    int client = connect_to(port);
    require(client >= 0);
    require(accepted.get_future().wait_for(5s) == std::future_status::ready);

    std::promise<void> paused;
    auto paused_future = paused.get_future();
    server.stop_accepting([&] { paused.set_value(); });
    require(paused_future.wait_for(5s) == std::future_status::ready);

    std::promise<void> drained;
    auto drained_future = drained.get_future();
    server.shutdown_connections([&] { drained.set_value(); });
    require(drained_future.wait_for(50ms) == std::future_status::timeout);
    server.force_close_connections();
    require(drained_future.wait_for(5s) == std::future_status::ready);
    ::close(client);
    server.stop_io_threads();
    loop.quit();
  });

  loop.loop();
  controller.join();
}

// 验证未连接 socket 的预期错误不会使 shutdown_write 终止进程。 / Verify that expected errors on an unconnected socket do not make shutdown_write terminate the process.
void shutdown_write_on_unconnected_socket_is_nonfatal() {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  require(fd >= 0);
  TLSS::NET::Socket socket(fd);
  socket.shutdown_write();
}

// 依次执行预期错误、优雅关闭和强制关闭三个回归场景。 / Run regression scenarios for expected errors, graceful shutdown, and forced shutdown in sequence.
int main() {
  shutdown_write_on_unconnected_socket_is_nonfatal();
  graceful_close_after_peer_fin();
  force_close_idle_peer_without_io_threads();
  return 0;
}
