#include "cli_commands.h"
#include "cli_server.h"
#include "monitor_module_registry.h"

#include <algorithm>
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

namespace {

using namespace TLSSMON;
using namespace std::chrono_literals;

class ScopedFd final {
public:
  explicit ScopedFd(int fd = -1) noexcept : _fd(fd) {}
  ~ScopedFd() {
    if (_fd >= 0) {
      (void)::close(_fd);
    }
  }

  ScopedFd(const ScopedFd &) = delete;
  ScopedFd &operator=(const ScopedFd &) = delete;

  ScopedFd(ScopedFd &&other) noexcept : _fd(other.release()) {}
  ScopedFd &operator=(ScopedFd &&other) noexcept {
    if (this != &other) {
      if (_fd >= 0) {
        (void)::close(_fd);
      }
      _fd = other.release();
    }
    return *this;
  }

  int get() const noexcept { return _fd; }

private:
  int release() noexcept {
    const int fd = _fd;
    _fd = -1;
    return fd;
  }

  int _fd{-1};
};

bool wait_for_phase(Engine &engine, EnginePhase expected,
                    std::chrono::milliseconds timeout = 2s) {
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
  ScopedFd client{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
  assert(client.get() >= 0);

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);

  assert(::connect(client.get(), reinterpret_cast<const sockaddr *>(&address),
                   sizeof(address)) == 0);
  return client;
}

bool send_all(int fd, std::string_view text) {
  std::size_t offset = 0U;
  while (offset < text.size()) {
    const ssize_t count =
        ::send(fd, text.data() + offset, text.size() - offset, MSG_NOSIGNAL);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
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

    int ready;
    do {
      ready = ::poll(&descriptor, 1,
                     std::max(1, static_cast<int>(remaining.count())));
    } while (ready < 0 && errno == EINTR);

    if (ready <= 0 || (descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
      return false;
    }

    char buffer[4096];
    const std::size_t needed = expected.size() - received.size();
    const ssize_t count =
        ::recv(fd, buffer, std::min(needed, sizeof(buffer)), 0);
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

std::string receive_until_close(int fd,
                                std::chrono::milliseconds timeout = 2s) {
  std::string received;
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (std::chrono::steady_clock::now() < deadline) {
    pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = POLLIN | POLLHUP;

    int ready;
    do {
      ready = ::poll(&descriptor, 1, 50);
    } while (ready < 0 && errno == EINTR);

    if (ready < 0 || (descriptor.revents & POLLNVAL) != 0) {
      break;
    }
    if (ready == 0) {
      continue;
    }

    char buffer[4096];
    const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
    if (count > 0) {
      received.append(buffer, static_cast<std::size_t>(count));
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count == 0 || (count < 0 && errno == ECONNRESET)) {
      return received;
    }
    break;
  }

  assert(false && "CLI connection did not close before the deadline");
  return received;
}

class RunningModuleCli final {
public:
  RunningModuleCli()
      : _engine(MonConfig{"module-cli-test", 0U, 1U}),
        _server(_engine, _registry,
                CliServerConfig{0U, CLI_MAX_CLIENTS, "storage"}) {
    assert(_engine.init() == ENGINESTATE::SUCCESSFUL);
    assert(_modules.register_module({10U, "module-a", "first module"}) ==
           ModuleRegisterStatus::SUCCESS);
    assert(_modules.register_module({20U, "module-b", "second module"}) ==
           ModuleRegisterStatus::SUCCESS);

    assert(register_use_command(_registry, _modules) ==
           CliRegisterStatus::SUCCESS);
    assert(_registry.register_command(
               "ping", "return pong",
               [](const CliArguments &) { return std::string{"pong\n"}; }) ==
           CliRegisterStatus::SUCCESS);
    assert(_registry.register_command(
               "multi", "return multiple lines", [](const CliArguments &) {
                 return std::string{"first\nsecond\n"};
               }) == CliRegisterStatus::SUCCESS);
    assert(_registry.register_command(
               "stop", "stop engine", [this](const CliArguments &) {
                 _engine.stop();
                 return std::string{"stopping\n"};
               }) == CliRegisterStatus::SUCCESS);

    assert(_server.start());
    _future = _promise.get_future();
    _runner = std::thread([this] { _promise.set_value(_engine.run()); });
    assert(wait_for_phase(_engine, EnginePhase::RUNNING));
  }

  ~RunningModuleCli() {
    _engine.stop();
    if (_runner.joinable()) {
      _runner.join();
    }
    assert(_future.get() == ENGINESTATE::SUCCESSFUL);
    _server.close();
  }

  RunningModuleCli(const RunningModuleCli &) = delete;
  RunningModuleCli &operator=(const RunningModuleCli &) = delete;

  std::uint16_t port() const noexcept { return _server.bound_port(); }
  Engine &engine() noexcept { return _engine; }

private:
  Engine _engine;
  MonitorModuleRegistry _modules;
  CliRegistry _registry;
  CliServer _server;
  std::promise<ENGINESTATE> _promise;
  std::future<ENGINESTATE> _future;
  std::thread _runner;
};

/*
 * 新连接先收到服务名提示符；use 成功后，后续提示符和命令都使用该
 * Session 保存的模块上下文，use all 再恢复默认服务名。
 */
void test_initial_prompt_and_module_switching() {
  RunningModuleCli server;
  ScopedFd client = connect_loopback(server.port());

  assert(receive_exact(client.get(), "[storage]> "));
  assert(send_all(client.get(), "use module-a\n"));
  assert(receive_exact(client.get(),
                       "* using module: module-a\n[module-a]> "));

  assert(send_all(client.get(), "ping\n"));
  assert(receive_exact(client.get(), "* pong\n[module-a]> "));

  assert(send_all(client.get(), "use all\n"));
  assert(receive_exact(client.get(), "* using module: all\n[storage]> "));
}

/*
 * 空行只重画提示符；未知命令和无效模块会输出错误，但不能改变当前模块。
 */
void test_empty_and_failed_commands_keep_current_prompt() {
  RunningModuleCli server;
  ScopedFd client = connect_loopback(server.port());

  assert(receive_exact(client.get(), "[storage]> "));
  assert(send_all(client.get(), "use module-b\n"));
  assert(receive_exact(client.get(),
                       "* using module: module-b\n[module-b]> "));

  assert(send_all(client.get(), "\nmissing\nuse absent\n"));
  assert(receive_exact(client.get(),
                       "[module-b]> "
                       "* unknown command: missing\n[module-b]> "
                       "* unknown module: absent\n[module-b]> "));
}

/*
 * 一个 TCP 包内的多条命令按顺序执行；每条命令完成后立即发送当时的提示符。
 */
void test_multiple_commands_preserve_prompt_order() {
  RunningModuleCli server;
  ScopedFd client = connect_loopback(server.port());

  assert(receive_exact(client.get(), "[storage]> "));
  assert(send_all(client.get(),
                  "use module-a\nping\nuse module-b\nping\n"));
  assert(receive_exact(client.get(),
                       "* using module: module-a\n[module-a]> "
                       "* pong\n[module-a]> "
                       "* using module: module-b\n[module-b]> "
                       "* pong\n[module-b]> "));
}

/* Handler 的多行输出必须逐行增加系统回复前缀，提示符本身不加前缀。 */
void test_multiline_reply_prefixes_every_line() {
  RunningModuleCli server;
  ScopedFd client = connect_loopback(server.port());

  assert(receive_exact(client.get(), "[storage]> "));
  assert(send_all(client.get(), "multi\n"));
  assert(receive_exact(client.get(),
                       "* first\n* second\n[storage]> "));
}

/*
 * 超长行只返回协议错误并关闭连接，不能在错误文本后继续发送提示符。
 */
void test_oversized_line_closes_without_prompt() {
  RunningModuleCli server;
  ScopedFd client = connect_loopback(server.port());

  assert(receive_exact(client.get(), "[storage]> "));
  assert(send_all(client.get(),
                  std::string(CLI_MAX_LINE_SIZE + 1U, 'x')));
  assert(receive_until_close(client.get()) == "error: line too long\n");
  assert(server.engine().get_phase() == EnginePhase::RUNNING);
}

/*
 * stop Handler 的输出先送达；Engine 进入 STOPPING 后不再产生新提示符，
 * Session 随事件循环退出而关闭。
 */
void test_stop_closes_without_trailing_prompt() {
  RunningModuleCli server;
  ScopedFd client = connect_loopback(server.port());

  assert(receive_exact(client.get(), "[storage]> "));
  assert(send_all(client.get(), "stop\n"));
  assert(receive_until_close(client.get()) == "* stopping\n");
  assert(wait_for_phase(server.engine(), EnginePhase::STOPPED));
}

} // namespace

int main() {
  test_initial_prompt_and_module_switching();
  test_empty_and_failed_commands_keep_current_prompt();
  test_multiple_commands_preserve_prompt_order();
  test_multiline_reply_prefixes_every_line();
  test_oversized_line_closes_without_prompt();
  test_stop_closes_without_trailing_prompt();

  std::cout << "M7_STAGE9_MODULE_CLI_SERVER=PASS\n";
  return 0;
}
