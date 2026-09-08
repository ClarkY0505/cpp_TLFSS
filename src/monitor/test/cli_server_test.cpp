#include "cli_server.h"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <future>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace TLSSMON {

/*
 * CliServer 为该类型提供了 friend 权限。
 *
 * 测试只借用 Listener fd 检查绑定地址和描述符标志，不关闭 fd，
 * 也不把原始描述符暴露成正式业务接口。
 */
struct CliServerTestAccess final {
  static int listener_fd(const CliServer &server) {
    std::lock_guard<std::mutex> lock(server._mutex);
    return server._listener_fd;
  }
};

} // namespace TLSSMON

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

  int release() noexcept {
    const int fd = _fd;
    _fd = -1;
    return fd;
  }

private:
  int _fd{-1};
};

void initialize(Engine &engine) {
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);
  assert(engine.get_phase() == EnginePhase::READY);
}

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

/*
 * 测试功能：Engine 未初始化时禁止启动 CLI Listener。
 *
 * 测试步骤：
 * 1. 构造仍处于 CREATED 的 Engine。
 * 2. 调用 start()。
 * 3. 验证启动失败且没有对外报告监听端口。
 */
void test_start_requires_ready_engine() {
  Engine engine{MonConfig{"cli-server-created", 0U, 1U}};
  CliRegistry registry;
  CliServer server{engine, registry, CliServerConfig{0U}};

  assert(!server.start());
  assert(server.bound_port() == 0U);
}

/*
 * 测试功能：拒绝无意义或超过协议硬上限的客户端数量。
 *
 * 测试步骤：
 * 1. 初始化 Engine。
 * 2. 分别传入 0 和 CLI_MAX_CLIENTS + 1。
 * 3. 验证两个 Listener 都无法启动。
 */
void test_invalid_max_clients_is_rejected() {
  Engine engine{MonConfig{"cli-server-limits", 0U, 1U}};
  initialize(engine);
  CliRegistry registry;

  CliServer zero_clients{engine, registry, CliServerConfig{0U, 0U}};
  assert(!zero_clients.start());
  assert(zero_clients.bound_port() == 0U);

  CliServer too_many_clients{
      engine, registry,
      CliServerConfig{0U, CLI_MAX_CLIENTS + std::size_t{1}}};
  assert(!too_many_clients.start());
  assert(too_many_clients.bound_port() == 0U);
}

/*
 * 测试功能：端口 0 获得真实端口，并且 Listener 只绑定 loopback。
 *
 * 测试步骤：
 * 1. 使用端口 0 启动 CliServer。
 * 2. 通过 getsockname() 读取实际监听地址。
 * 3. 验证地址为 127.0.0.1。
 * 4. 验证内核端口与 bound_port() 一致且不为 0。
 */
void test_ephemeral_port_and_loopback_binding() {
  Engine engine{MonConfig{"cli-server-loopback", 0U, 1U}};
  initialize(engine);
  CliRegistry registry;
  CliServer server{engine, registry, CliServerConfig{0U}};

  assert(server.start());

  const int listener_fd = CliServerTestAccess::listener_fd(server);
  assert(listener_fd >= 0);

  sockaddr_in address{};
  socklen_t address_size = sizeof(address);
  assert(::getsockname(listener_fd, reinterpret_cast<sockaddr *>(&address),
                       &address_size) == 0);

  assert(address_size == sizeof(sockaddr_in));
  assert(address.sin_family == AF_INET);
  assert(ntohl(address.sin_addr.s_addr) == INADDR_LOOPBACK);
  assert(server.bound_port() != 0U);
  assert(ntohs(address.sin_port) == server.bound_port());
}

/*
 * 测试功能：Listener 同时启用非阻塞和 close-on-exec。
 *
 * 测试步骤：
 * 1. 启动 Listener。
 * 2. 使用 fcntl 分别读取状态标志和描述符标志。
 * 3. 验证 O_NONBLOCK 与 FD_CLOEXEC 都已设置。
 */
void test_listener_descriptor_flags() {
  Engine engine{MonConfig{"cli-server-flags", 0U, 1U}};
  initialize(engine);
  CliRegistry registry;
  CliServer server{engine, registry, CliServerConfig{0U}};

  assert(server.start());

  const int listener_fd = CliServerTestAccess::listener_fd(server);
  assert(listener_fd >= 0);

  const int status_flags = ::fcntl(listener_fd, F_GETFL);
  const int descriptor_flags = ::fcntl(listener_fd, F_GETFD);
  assert(status_flags >= 0);
  assert(descriptor_flags >= 0);
  assert((status_flags & O_NONBLOCK) != 0);
  assert((descriptor_flags & FD_CLOEXEC) != 0);
}

/*
 * 测试功能：重复 start() 不会创建第二个 Listener 或 AIO 注册。
 *
 * 测试步骤：
 * 1. 第一次启动并保存 fd、端口。
 * 2. 再次调用 start()。
 * 3. 验证第二次失败，原 fd 和端口保持不变。
 */
void test_repeated_start_does_not_replace_listener() {
  Engine engine{MonConfig{"cli-server-repeated-start", 0U, 1U}};
  initialize(engine);
  CliRegistry registry;
  CliServer server{engine, registry, CliServerConfig{0U}};

  assert(server.start());

  const int first_fd = CliServerTestAccess::listener_fd(server);
  const std::uint16_t first_port = server.bound_port();
  assert(first_fd >= 0);
  assert(first_port != 0U);

  assert(!server.start());
  assert(CliServerTestAccess::listener_fd(server) == first_fd);
  assert(server.bound_port() == first_port);
}

/*
 * 测试功能：并发 start() 也只能产生一次成功注册。
 *
 * 测试步骤：
 * 1. 八个线程共享同一个 CliServer。
 * 2. 所有线程同时调用 start()。
 * 3. 验证成功次数严格等于 1。
 */
void test_concurrent_start_only_succeeds_once() {
  Engine engine{MonConfig{"cli-server-concurrent-start", 0U, 1U}};
  initialize(engine);
  CliRegistry registry;
  CliServer server{engine, registry, CliServerConfig{0U}};

  std::atomic<unsigned int> success_count{0U};
  std::vector<std::thread> workers;
  workers.reserve(8U);

  for (unsigned int index = 0U; index < 8U; ++index) {
    workers.emplace_back([&server, &success_count] {
      if (server.start()) {
        success_count.fetch_add(1U, std::memory_order_relaxed);
      }
    });
  }

  for (std::thread &worker : workers) {
    worker.join();
  }

  assert(success_count.load(std::memory_order_relaxed) == 1U);
  assert(server.bound_port() != 0U);
}

/*
 * 测试功能：端口冲突时 start() 失败并回滚公开状态。
 *
 * 测试步骤：
 * 1. 用独立 socket 占用一个 loopback 临时端口。
 * 2. 让 CliServer 尝试监听同一端口。
 * 3. 验证启动失败且 bound_port() 仍为 0。
 */
void test_occupied_port_is_rejected() {
  ScopedFd blocker(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  assert(blocker.get() >= 0);

  sockaddr_in occupied_address{};
  occupied_address.sin_family = AF_INET;
  occupied_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  occupied_address.sin_port = htons(0U);

  assert(::bind(blocker.get(),
                reinterpret_cast<const sockaddr *>(&occupied_address),
                sizeof(occupied_address)) == 0);
  assert(::listen(blocker.get(), 1) == 0);

  socklen_t address_size = sizeof(occupied_address);
  assert(::getsockname(blocker.get(),
                       reinterpret_cast<sockaddr *>(&occupied_address),
                       &address_size) == 0);

  const std::uint16_t occupied_port = ntohs(occupied_address.sin_port);
  assert(occupied_port != 0U);

  Engine engine{MonConfig{"cli-server-port-conflict", 0U, 1U}};
  initialize(engine);
  CliRegistry registry;
  CliServer server{engine, registry, CliServerConfig{occupied_port}};

  assert(!server.start());
  assert(server.bound_port() == 0U);
}

/*
 * 测试功能：Listener 已接入 Engine AIO，连接处理不会终止事件循环。
 *
 * 测试步骤：
 * 1. 在 READY 阶段启动 CliServer。
 * 2. 独立线程运行 Engine。
 * 3. 客户端连接实际端口。
 * 4. 阶段 7 的 Session 保持连接，不再要求 Listener 立即关闭客户端。
 * 5. 验证 Engine 仍处于 RUNNING。
 * 6. 主动 stop()，验证 run() 正常返回。
 */
void test_accept_does_not_stop_engine() {
  Engine engine{MonConfig{"cli-server-accept", 0U, 1U}};
  initialize(engine);
  CliRegistry registry;
  CliServer server{engine, registry, CliServerConfig{0U}};
  assert(server.start());

  std::promise<ENGINESTATE> run_promise;
  std::future<ENGINESTATE> run_result = run_promise.get_future();
  std::thread runner([&engine, &run_promise] {
    run_promise.set_value(engine.run());
  });

  assert(wait_for_phase(engine, EnginePhase::RUNNING, 2s));

  ScopedFd client = connect_loopback(server.bound_port());
  std::this_thread::sleep_for(20ms);

  assert(engine.get_phase() == EnginePhase::RUNNING);
  engine.stop();

  assert(run_result.wait_for(2s) == std::future_status::ready);
  runner.join();
  assert(run_result.get() == ENGINESTATE::SUCCESSFUL);
  assert(engine.get_phase() == EnginePhase::STOPPED);
}

/*
 * 测试功能：close() 是幂等操作，并清除 CliServer 的公开监听状态。
 *
 * 测试步骤：
 * 1. 启动 Listener。
 * 2. 连续调用两次 close()。
 * 3. 验证端口归零且测试侧不再获得 Listener fd。
 * 4. 函数结束时析构再次 close() 也必须安全。
 */
void test_close_is_idempotent() {
  Engine engine{MonConfig{"cli-server-close", 0U, 1U}};
  initialize(engine);
  CliRegistry registry;
  CliServer server{engine, registry, CliServerConfig{0U}};

  assert(server.start());
  assert(server.bound_port() != 0U);

  server.close();
  server.close();

  assert(server.bound_port() == 0U);
  assert(CliServerTestAccess::listener_fd(server) == -1);
}

} // namespace

int main() {
  test_start_requires_ready_engine();
  test_invalid_max_clients_is_rejected();
  test_ephemeral_port_and_loopback_binding();
  test_listener_descriptor_flags();
  test_repeated_start_does_not_replace_listener();
  test_concurrent_start_only_succeeds_once();
  test_occupied_port_is_rejected();
  test_accept_does_not_stop_engine();
  test_close_is_idempotent();

  std::cout << "M7_STAGE6_CLI_SERVER=PASS\n";
  return 0;
}
