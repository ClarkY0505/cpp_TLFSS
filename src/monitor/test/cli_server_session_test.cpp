#include "cli_server.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace TLSSMON;
using namespace std::chrono_literals;

constexpr std::string_view DEFAULT_PROMPT{"[monitor]> "};

class ScopedFd final {
public:
  explicit ScopedFd(int fd = -1) noexcept : _fd(fd) {}

  ~ScopedFd() { reset(); }

  ScopedFd(const ScopedFd &) = delete;
  ScopedFd &operator=(const ScopedFd &) = delete;

  ScopedFd(ScopedFd &&other) noexcept : _fd(other.release()) {}

  ScopedFd &operator=(ScopedFd &&other) noexcept {
    if (this != &other) {
      reset(other.release());
    }

    return *this;
  }

  int get() const noexcept { return _fd; }

  int release() noexcept {
    const int fd = _fd;
    _fd = -1;
    return fd;
  }

  void reset(int fd = -1) noexcept {
    if (_fd >= 0) {
      (void)::close(_fd);
    }

    _fd = fd;
  }

private:
  int _fd{-1};
};

bool wait_for_phase(Engine &engine, EnginePhase expected,
                    std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (std::chrono::steady_clock::now() < deadline) {
    if (engine.get_phase() == expected) {
      return true;
    }

    std::this_thread::yield();
  }

  return engine.get_phase() == expected;
}

ScopedFd connect_loopback(std::uint16_t port) {
  ScopedFd client(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  assert(client.get() >= 0);

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);

  assert(::connect(client.get(), reinterpret_cast<const sockaddr *>(&address),
                   sizeof(address)) == 0);

  return client;
}

bool send_bytes(int fd, std::string_view data) {
  std::size_t offset = 0U;

  while (offset < data.size()) {
    const ssize_t sent =
        ::send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);

    if (sent > 0) {
      offset += static_cast<std::size_t>(sent);
      continue;
    }

    if (sent < 0 && errno == EINTR) {
      continue;
    }

    return false;
  }

  return true;
}

bool receive_exact(int fd, std::string_view expected,
                   std::chrono::milliseconds timeout = 2s) {
  std::string received;
  received.reserve(expected.size());
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (received.size() < expected.size()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return false;
    }

    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);

    pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = POLLIN;

    int poll_result;
    do {
      poll_result =
          ::poll(&descriptor, 1, std::max(1, static_cast<int>(remaining.count())));
    } while (poll_result < 0 && errno == EINTR);

    if (poll_result <= 0 ||
        (descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
      return false;
    }

    char buffer[4096];
    const std::size_t needed = expected.size() - received.size();
    const std::size_t capacity = std::min(needed, sizeof(buffer));
    const ssize_t count = ::recv(fd, buffer, capacity, 0);

    if (count > 0) {
      received.append(buffer, static_cast<std::size_t>(count));
      continue;
    }

    if (count < 0 && errno == EINTR) {
      continue;
    }

    return false;
  }

  return received == expected;
}

/* 建立一个已被服务端接纳、且初始提示符已经消费完毕的连接。 */
ScopedFd connect_cli(std::uint16_t port,
                     std::chrono::milliseconds timeout = 2s) {
  ScopedFd client = connect_loopback(port);
  assert(receive_exact(client.get(), DEFAULT_PROMPT, timeout));
  return client;
}

bool has_no_input(int fd, std::chrono::milliseconds timeout) {
  pollfd descriptor{};
  descriptor.fd = fd;
  descriptor.events = POLLIN;

  int result;
  do {
    result = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
  } while (result < 0 && errno == EINTR);

  return result == 0;
}

bool wait_for_peer_close(int fd,
                         std::chrono::milliseconds timeout = 2s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (std::chrono::steady_clock::now() < deadline) {
    pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = POLLIN | POLLHUP;

    int result;
    do {
      result = ::poll(&descriptor, 1, 50);
    } while (result < 0 && errno == EINTR);

    if (result < 0) {
      return false;
    }

    if (result == 0) {
      continue;
    }

    char byte{};
    const ssize_t count = ::recv(fd, &byte, sizeof(byte), 0);

    if (count == 0 || (count < 0 && errno == ECONNRESET)) {
      return true;
    }

    if (count < 0 && errno == EINTR) {
      continue;
    }

    if (count > 0) {
      continue;
    }

    return false;
  }

  return false;
}

class RunningCliServer final {
public:
  explicit RunningCliServer(std::size_t max_clients = CLI_MAX_CLIENTS)
      : _engine(MonConfig{"cli-session-test", 0U, 1U}),
        _server(_engine, _registry, CliServerConfig{0U, max_clients}) {
    assert(_engine.init() == ENGINESTATE::SUCCESSFUL);
    assert(_registry.register_command(
               "ping", "return pong",
               [](const CliArguments &) { return std::string{"pong\n"}; }) ==
           CliRegisterStatus::SUCCESS);
    assert(_server.start());

    _run_future = _run_promise.get_future();
    _runner = std::thread([this] {
      _run_promise.set_value(_engine.run());
    });

    assert(wait_for_phase(_engine, EnginePhase::RUNNING, 2s));
  }

  ~RunningCliServer() {
    _engine.stop();

    if (_runner.joinable()) {
      _runner.join();
    }

    assert(_run_future.get() == ENGINESTATE::SUCCESSFUL);

    _server.close();
  }

  RunningCliServer(const RunningCliServer &) = delete;
  RunningCliServer &operator=(const RunningCliServer &) = delete;

  std::uint16_t port() const noexcept { return _server.bound_port(); }

  CliRegistry &registry() noexcept { return _registry; }

  Engine &engine() noexcept { return _engine; }

  CliServer &server() noexcept { return _server; }

private:
  Engine _engine;
  CliRegistry _registry;
  CliServer _server;
  std::promise<ENGINESTATE> _run_promise;
  std::future<ENGINESTATE> _run_future;
  std::thread _runner;
};

/* 一条以 LF 结束的完整命令会执行并返回完整响应。 */
void test_single_command() {
  RunningCliServer server;
  ScopedFd client = connect_cli(server.port());

  assert(send_bytes(client.get(), "ping\n"));
  assert(receive_exact(client.get(), "* pong\n[monitor]> "));
}

/* 命令被拆成多个 TCP 包时，未收到换行前不能提前分派。 */
void test_fragmented_command_waits_for_newline() {
  RunningCliServer server;
  ScopedFd client = connect_cli(server.port());

  assert(send_bytes(client.get(), "pi"));
  assert(has_no_input(client.get(), 100ms));

  assert(send_bytes(client.get(), "ng\n"));
  assert(receive_exact(client.get(), "* pong\n[monitor]> "));
}

/* 多条命令粘在一个 TCP 包中时必须按原顺序全部执行。 */
void test_multiple_commands_in_one_packet() {
  RunningCliServer server;
  ScopedFd client = connect_cli(server.port());

  assert(send_bytes(client.get(), "ping\nping\nping\n"));
  assert(receive_exact(client.get(),
                       "* pong\n[monitor]> "
                       "* pong\n[monitor]> "
                       "* pong\n[monitor]> "));
}

/* LF、CRLF 都是合法行结束符；空行只重新发送当前提示符。 */
void test_lf_crlf_and_empty_lines() {
  RunningCliServer server;
  ScopedFd client = connect_cli(server.port());

  assert(send_bytes(client.get(), "\nping\r\n\r\nping\n"));
  assert(receive_exact(client.get(),
                       "[monitor]> "
                       "* pong\n[monitor]> "
                       "[monitor]> "
                       "* pong\n[monitor]> "));
}

/* 没有换行的最后半行即使内容是完整命令，也不能执行。 */
void test_unfinished_last_line_is_not_executed() {
  RunningCliServer server;
  std::atomic<unsigned int> calls{0U};

  assert(server.registry().register_command(
             "mark", "count calls",
             [&calls](const CliArguments &) {
               calls.fetch_add(1U, std::memory_order_relaxed);
               return std::string{"marked\n"};
             }) == CliRegisterStatus::SUCCESS);

  ScopedFd client = connect_cli(server.port());
  assert(send_bytes(client.get(), "mark"));
  assert(has_no_input(client.get(), 100ms));
  assert(calls.load(std::memory_order_relaxed) == 0U);

  assert(send_bytes(client.get(), "\n"));
  assert(receive_exact(client.get(), "* marked\n[monitor]> "));
  assert(calls.load(std::memory_order_relaxed) == 1U);
}

/* 未知命令只返回错误，不关闭连接，后续合法命令仍能执行。 */
void test_unknown_command_does_not_poison_session() {
  RunningCliServer server;
  ScopedFd client = connect_cli(server.port());

  assert(send_bytes(client.get(), "missing\nping\n"));
  assert(receive_exact(client.get(),
                       "* unknown command: missing\n[monitor]> "
                       "* pong\n[monitor]> "));
}

/* 255 字节正文是合法边界，且 255 字节加 CRLF 也必须允许。 */
void test_maximum_length_line_is_allowed() {
  RunningCliServer server;
  const std::string command(CLI_MAX_LINE_SIZE, 'x');

  assert(server.registry().register_command(
             command, "maximum command",
             [](const CliArguments &) { return std::string{"maximum\n"}; }) ==
         CliRegisterStatus::SUCCESS);

  ScopedFd client = connect_cli(server.port());
  assert(send_bytes(client.get(), command + "\r\n"));
  assert(receive_exact(client.get(), "* maximum\n[monitor]> "));
}

/* 255 字节正文后的 CR 和 LF 即使分属两个 TCP 包也必须合法。 */
void test_maximum_length_crlf_can_be_fragmented() {
  RunningCliServer server;
  const std::string command(CLI_MAX_LINE_SIZE, 'y');

  assert(server.registry().register_command(
             command, "fragmented CRLF",
             [](const CliArguments &) { return std::string{"fragmented\n"}; }) ==
         CliRegisterStatus::SUCCESS);

  ScopedFd client = connect_cli(server.port());
  assert(send_bytes(client.get(), command + "\r"));
  assert(has_no_input(client.get(), 100ms));
  assert(send_bytes(client.get(), "\n"));
  assert(receive_exact(client.get(), "* fragmented\n[monitor]> "));
}

/* 未收到换行时正文达到 256 字节，也必须立即报错并关闭连接。 */
void test_oversized_line_is_rejected() {
  RunningCliServer server;
  ScopedFd client = connect_cli(server.port());
  const std::string oversized(CLI_MAX_LINE_SIZE + 1U, 'x');

  assert(send_bytes(client.get(), oversized));
  assert(receive_exact(client.get(), "error: line too long\n"));
  assert(wait_for_peer_close(client.get()));
  assert(server.engine().get_phase() == EnginePhase::RUNNING);
}

/* Handler 输出中的内嵌 NUL 必须按 std::string 长度完整发送。 */
void test_binary_handler_output_is_preserved() {
  RunningCliServer server;
  const std::string handler_output{"A\0B\n", 4U};
  const std::string expected = std::string{"* "} + handler_output;

  assert(server.registry().register_command(
             "binary", "binary output",
             [handler_output](const CliArguments &) { return handler_output; }) ==
         CliRegisterStatus::SUCCESS);

  ScopedFd client = connect_cli(server.port());
  assert(send_bytes(client.get(), "binary\n"));
  assert(receive_exact(client.get(), expected + std::string{DEFAULT_PROMPT}));
}

/* 八个活动客户端正常工作，第九个连接由服务端立即关闭。 */
void test_ninth_concurrent_client_is_rejected() {
  RunningCliServer server{CLI_MAX_CLIENTS};
  std::vector<ScopedFd> clients;
  clients.reserve(CLI_MAX_CLIENTS);

  for (std::size_t index = 0U; index < CLI_MAX_CLIENTS; ++index) {
    clients.push_back(connect_cli(server.port()));
    assert(send_bytes(clients.back().get(), "ping\n"));
    assert(receive_exact(clients.back().get(), "* pong\n[monitor]> "));
  }

  ScopedFd ninth = connect_loopback(server.port());
  assert(wait_for_peer_close(ninth.get()));

  assert(send_bytes(clients.front().get(), "ping\n"));
  assert(receive_exact(clients.front().get(), "* pong\n[monitor]> "));
}

/* 客户端断开后活动槽位会恢复，累计连接数可以超过配置上限。 */
void test_disconnected_client_slot_is_reused() {
  RunningCliServer server{1U};

  for (unsigned int round = 0U; round < 10U; ++round) {
    bool served = false;
    const auto deadline = std::chrono::steady_clock::now() + 2s;

    while (!served && std::chrono::steady_clock::now() < deadline) {
      ScopedFd client = connect_loopback(server.port());

      if (receive_exact(client.get(), DEFAULT_PROMPT, 100ms) &&
          send_bytes(client.get(), "ping\n") &&
          receive_exact(client.get(), "* pong\n[monitor]> ", 100ms)) {
        served = true;
      }

      client.reset();

      if (!served) {
        std::this_thread::sleep_for(10ms);
      }
    }

    assert(served);
  }
}

/* 一个客户端以 RST 异常断开，不影响其他 Session 和 Engine。 */
void test_broken_client_does_not_affect_other_clients() {
  RunningCliServer server;
  ScopedFd broken = connect_loopback(server.port());

  linger reset_on_close{};
  reset_on_close.l_onoff = 1;
  reset_on_close.l_linger = 0;
  assert(::setsockopt(broken.get(), SOL_SOCKET, SO_LINGER, &reset_on_close,
                      sizeof(reset_on_close)) == 0);
  broken.reset();

  ScopedFd healthy = connect_cli(server.port());
  assert(send_bytes(healthy.get(), "ping\n"));
  assert(receive_exact(healthy.get(), "* pong\n[monitor]> "));
  assert(server.engine().get_phase() == EnginePhase::RUNNING);
}

/* 慢客户端填满发送缓冲区时，有限 poll 超时不能永久阻塞事件循环。 */
void test_slow_reader_does_not_block_engine_forever() {
  RunningCliServer server;

  assert(server.registry().register_command(
             "large", "large response",
             [](const CliArguments &) {
               return std::string(16U * 1024U * 1024U, 'L');
             }) == CliRegisterStatus::SUCCESS);

  ScopedFd slow = connect_cli(server.port());
  assert(send_bytes(slow.get(), "large\n"));

  const auto start = std::chrono::steady_clock::now();
  ScopedFd healthy = connect_cli(server.port());
  assert(send_bytes(healthy.get(), "ping\n"));
  assert(receive_exact(healthy.get(), "* pong\n[monitor]> ", 2s));

  const auto elapsed = std::chrono::steady_clock::now() - start;
  assert(elapsed < 2s);
  assert(server.engine().get_phase() == EnginePhase::RUNNING);
}

/* CliServer::close() 会停止 Listener，并最终关闭所有活动 Session。 */
void test_server_close_closes_all_sessions() {
  RunningCliServer server;
  ScopedFd first = connect_cli(server.port());
  ScopedFd second = connect_cli(server.port());

  assert(send_bytes(first.get(), "ping\n"));
  assert(receive_exact(first.get(), "* pong\n[monitor]> "));
  assert(send_bytes(second.get(), "ping\n"));
  assert(receive_exact(second.get(), "* pong\n[monitor]> "));

  server.server().close();

  assert(server.server().bound_port() == 0U);
  assert(wait_for_peer_close(first.get()));
  assert(wait_for_peer_close(second.get()));
  assert(server.engine().get_phase() == EnginePhase::RUNNING);
}

} // namespace

int main() {
  test_single_command();
  test_fragmented_command_waits_for_newline();
  test_multiple_commands_in_one_packet();
  test_lf_crlf_and_empty_lines();
  test_unfinished_last_line_is_not_executed();
  test_unknown_command_does_not_poison_session();
  test_maximum_length_line_is_allowed();
  test_maximum_length_crlf_can_be_fragmented();
  test_oversized_line_is_rejected();
  test_binary_handler_output_is_preserved();
  test_ninth_concurrent_client_is_rejected();
  test_disconnected_client_slot_is_reused();
  test_broken_client_does_not_affect_other_clients();
  test_slow_reader_does_not_block_engine_forever();
  test_server_close_closes_all_sessions();

  std::cout << "M7_STAGE7_CLI_SESSION=PASS\n";
  return 0;
}
