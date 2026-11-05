#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

#include "common/net/event_loop.h"
#include "common/net/tcp_connection.h"

namespace {
using namespace TLSS::NET;

void check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

void pump(EventLoop& loop) {
  loop.queue_in_loop([&loop] {
    loop.quit();
  });
  loop.wakeup();
  loop.loop();
}

void late_completion_after_connection_destruction() {
  EventLoop loop;
  int sockets[2];
  check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) == 0,
        "socketpair failed");
  auto conn = std::make_shared<TcpConnection>(&loop, "late-read", sockets[0], InetAddress(0),
                                              InetAddress(0));
  conn->connect_established();
  TcpConnection::FileReadCompletion read;
  int callbacks = 0;
  std::error_code result;
  std::uint64_t sent = 1;
  conn->send_file_async(
      0, 3,
      [&](off_t, std::size_t, TcpConnection::FileReadCompletion completion) {
        read = std::move(completion);
      },
      [&](std::error_code error, std::uint64_t bytes) {
        ++callbacks;
        result = error;
        sent = bytes;
      });
  pump(loop);
  check(static_cast<bool>(read), "read was not requested");
  std::weak_ptr<TcpConnection> weak = conn;
  conn->connect_destroyed();
  conn.reset();
  check(weak.expired(), "completion retained the connection");
  std::thread worker([&] {
    read({}, "BAD");
  });
  worker.join();
  pump(loop);
  check(callbacks == 1 && result == std::errc::operation_canceled && sent == 0,
        "late read altered cancellation");
  char byte;
  check(::recv(sockets[1], &byte, 1, 0) == 0, "late read sent bytes after destruction");
  ::close(sockets[1]);
}
}  // namespace

int main() {
  try {
    late_completion_after_connection_destruction();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
