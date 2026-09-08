#include "cli_commands.h"
#include "cli_registry.h"
#include "cli_server.h"
#include "engine.h"
#include "monitor_data.h"
#include "timer_types.h"
#include "udp_publisher.h"
#include "udp_receiver.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
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

constexpr MonData::MonitorKey TIMER_KEY{7U, 1U, 1U, 1U};
constexpr MonData::MonitorKey AIO_KEY{7U, 1U, 1U, 2U};

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

void initialize(Engine &engine) {
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);
  assert(engine.get_phase() == EnginePhase::READY);
}

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

template <typename Predicate>
bool wait_until(Predicate predicate,
                std::chrono::milliseconds timeout = 2s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
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
    const ssize_t sent =
        ::send(fd, text.data() + offset, text.size() - offset, MSG_NOSIGNAL);
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

std::string receive_until(int fd, std::string_view suffix,
                          std::chrono::milliseconds timeout = 2s) {
  std::string output;
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (output.find(suffix) == std::string::npos) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      break;
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

    if (ready <= 0 ||
        (descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
      break;
    }

    char buffer[4096];
    const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
    if (count > 0) {
      output.append(buffer, static_cast<std::size_t>(count));
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
  return output;
}

/* 建立连接并消费服务端主动发送的初始提示符。 */
ScopedFd connect_cli(std::uint16_t port) {
  ScopedFd client = connect_loopback(port);
  assert(receive_until(client.get(), "[monitor]> ") == "[monitor]> ");
  return client;
}

struct EngineRunner final {
  explicit EngineRunner(Engine &engine) : _engine(engine) {
    _future = _promise.get_future();
    _thread = std::thread([this] { _promise.set_value(_engine.run()); });
    assert(wait_for_phase(_engine, EnginePhase::RUNNING));
  }

  ~EngineRunner() {
    _engine.stop();
    if (_thread.joinable()) {
      _thread.join();
    }
  }

  ENGINESTATE stop_and_join() {
    _engine.stop();
    if (_thread.joinable()) {
      _thread.join();
    }
    return _future.get();
  }

private:
  Engine &_engine;
  std::promise<ENGINESTATE> _promise;
  std::future<ENGINESTATE> _future;
  std::thread _thread;
};

/* Engine 暴露原始 CLI 配置端口，READY/RUNNING/STOPPED 都不改变该值。 */
void test_cli_port_and_basic_lifecycle() {
  Engine engine{MonConfig{"m7-cli-port", 0U, 1U}};
  assert(engine.cli_port() == 0U);
  initialize(engine);
  assert(engine.cli_port() == 0U);

  CliRegistry registry;
  assert(register_help_command(registry) == CliRegisterStatus::SUCCESS);
  CliServer cli{engine, registry, CliServerConfig{engine.cli_port()}};
  assert(cli.start());
  assert(cli.bound_port() != 0U);

  EngineRunner runner{engine};
  ScopedFd client = connect_cli(cli.bound_port());
  assert(send_all(client.get(), "help\n"));
  const std::string output = receive_until(client.get(), "\n");
  assert(output.find("* help    list available commands\n") !=
         std::string::npos);

  assert(runner.stop_and_join() == ENGINESTATE::SUCCESSFUL);
  assert(engine.get_phase() == EnginePhase::STOPPED);
  assert(engine.cli_port() == 0U);
  cli.close();
}

/* Timer 与 AIO 回调写入的记录都必须能通过真实 TCP db_dump 查询。 */
void test_db_dump_observes_timer_and_aio_updates() {
  Engine engine{MonConfig{"m7-timer-aio", 0U, 1U}};
  initialize(engine);
  CliRegistry registry;
  assert(register_db_dump_command(registry, engine) ==
         CliRegisterStatus::SUCCESS);
  CliServer cli{engine, registry, CliServerConfig{0U}};
  assert(cli.start());

  int pipe_fds[2]{-1, -1};
  assert(::pipe(pipe_fds) == 0);
  ScopedFd input{pipe_fds[0]};
  ScopedFd output{pipe_fds[1]};

  assert(engine.add_aio(
      input.get(), MonCallback{"m7-test-aio", [&engine, fd = input.get()] {
        char byte{};
        ssize_t count;
        do {
          count = ::read(fd, &byte, sizeof(byte));
        } while (count < 0 && errno == EINTR);
        if (count == 1) {
          (void)engine.report_string(AIO_KEY, "aio-ready", "aio-record");
        }
        return 0;
      }, false}));

  assert(engine.set_timer(
      MonCallback{"m7-test-timer", [&engine] {
        (void)engine.report_count(TIMER_KEY, 42U, "timer-record");
        return 0;
      }, false},
      TimerFlags::ONCE, 10ms));

  EngineRunner runner{engine};
  const char signal = 'x';
  assert(::write(output.get(), &signal, sizeof(signal)) == 1);
  assert(wait_until([&engine] {
    return engine.find_data(TIMER_KEY).has_value() &&
           engine.find_data(AIO_KEY).has_value();
  }));

  ScopedFd client = connect_cli(cli.bound_port());
  assert(send_all(client.get(), "db_dump\n"));
  const std::string dump = receive_until(client.get(), "2 entries\n");
  assert(dump.find("* mid=") != std::string::npos);
  assert(dump.find("desc=\"timer-record\"") != std::string::npos);
  assert(dump.find("desc=\"aio-record\"") != std::string::npos);
  assert(dump.find("* 2 entries\n") != std::string::npos);

  assert(runner.stop_and_join() == ENGINESTATE::SUCCESSFUL);
  assert(engine.query_data().size() == 2U);
  cli.close();
}

/* M5 Publisher 与 CLI 查询共享 Store，但每个组件保持独立生命周期。 */
void test_m5_publisher_and_cli_can_work_together() {
  Engine engine{MonConfig{"m7-m5", 0U, 1U}};
  initialize(engine);
  std::atomic<unsigned int> published{0U};
  assert(engine.set_publisher([&published](MonData::StoredRecord) {
    published.fetch_add(1U, std::memory_order_relaxed);
  }));

  CliRegistry registry;
  assert(register_db_dump_command(registry, engine) ==
         CliRegisterStatus::SUCCESS);
  CliServer cli{engine, registry, CliServerConfig{0U}};
  assert(cli.start());
  EngineRunner runner{engine};

  const MonData::MonitorKey key{8U, 1U, 1U, 1U};
  assert(engine.report_count(key, 9U, "publisher-and-cli").changed());
  assert(published.load(std::memory_order_relaxed) == 1U);

  ScopedFd client = connect_cli(cli.bound_port());
  assert(send_all(client.get(), "db_dump\n"));
  const std::string dump = receive_until(client.get(), "1 entries\n");
  assert(dump.find("desc=\"publisher-and-cli\"") != std::string::npos);

  assert(runner.stop_and_join() == ENGINESTATE::SUCCESSFUL);
  cli.close();
}

/* M6 V2 UDP Publisher 发送期间，CLI 仍可查询相同记录。 */
void test_m6_udp_publisher_and_cli_can_work_together() {
  Wire::UdpReceiver receiver{{"127.0.0.1", 0U}};
  assert(receiver.ready());

  Engine engine{MonConfig{"m7-m6", 0U, 1U}};
  initialize(engine);
  auto publisher = std::make_shared<Wire::UdpPublisher>(
      Wire::UdpPublisherConfig{
          Wire::UdpEndpoint{"127.0.0.1", receiver.bound_port()},
          Wire::WireVersion::V2});
  assert(publisher->ready());
  assert(engine.set_publisher([publisher](MonData::StoredRecord record) {
    assert(publisher->send(record).success());
  }));

  CliRegistry registry;
  assert(register_db_dump_command(registry, engine) ==
         CliRegisterStatus::SUCCESS);
  CliServer cli{engine, registry, CliServerConfig{0U}};
  assert(cli.start());
  EngineRunner runner{engine};

  const MonData::MonitorKey key{9U, 1U, 1U, 1U};
  assert(engine.report_string(key, "udp-and-cli", "v2-record").changed());

  Wire::UdpReceiveResult received{};
  assert(wait_until([&receiver, &received] {
    received = receiver.receive_one();
    return received.success();
  }));
  assert(received._record.has_value());
  assert(received._record->_data._key == key);

  ScopedFd client = connect_cli(cli.bound_port());
  assert(send_all(client.get(), "db_dump\n"));
  const std::string dump = receive_until(client.get(), "1 entries\n");
  assert(dump.find("str=\"udp-and-cli\"") != std::string::npos);
  assert(dump.find("desc=\"v2-record\"") != std::string::npos);

  assert(runner.stop_and_join() == ENGINESTATE::SUCCESSFUL);
  cli.close();
}

/* STOPPING 后的完整命令不能进入 Registry Handler。 */
void test_stopping_rejects_new_commands() {
  Engine engine{MonConfig{"m7-stopping", 0U, 1U}};
  initialize(engine);
  CliRegistry registry;
  std::atomic<unsigned int> calls{0U};
  assert(registry.register_command(
             "mark", "count calls", [&calls](const CliArguments &) {
               calls.fetch_add(1U, std::memory_order_relaxed);
               return std::string{"marked\n"};
             }) == CliRegisterStatus::SUCCESS);
  CliServer cli{engine, registry, CliServerConfig{0U}};
  assert(cli.start());

  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool release = false;
  assert(engine.set_timer(
      MonCallback{"m7-stop-blocker", [&] {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        condition.notify_all();
        condition.wait(lock, [&release] { return release; });
        return 0;
      }, false},
      TimerFlags::RECURRING | TimerFlags::WORKER, 10ms));

  EngineRunner runner{engine};
  ScopedFd existing = connect_cli(cli.bound_port());

  /* 先证明该连接已经被接纳，且 Handler 在 RUNNING 中可以执行。 */
  assert(send_all(existing.get(), "mark\n"));
  assert(receive_until(existing.get(), "[monitor]> ") ==
         "* marked\n[monitor]> ");
  assert(calls.load(std::memory_order_relaxed) == 1U);

  {
    std::unique_lock<std::mutex> lock(mutex);
    assert(condition.wait_for(lock, 2s, [&entered] { return entered; }));
  }

  engine.stop();
  assert(wait_for_phase(engine, EnginePhase::STOPPING));
  assert(send_all(existing.get(), "mark\n"));

  /*
   * Listener socket 由外部 CliServer 管理，所以 STOPPING 窗口中的 TCP
   * connect() 可能先完成握手；无论握手结果如何，都不能形成可执行命令的
   * 新 Session。
   */
  ScopedFd newcomer{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
  assert(newcomer.get() >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(cli.bound_port());
  if (::connect(newcomer.get(), reinterpret_cast<const sockaddr *>(&address),
                sizeof(address)) == 0) {
    (void)send_all(newcomer.get(), "mark\n");
  }

  std::this_thread::sleep_for(30ms);
  assert(calls.load(std::memory_order_relaxed) == 1U);

  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  condition.notify_all();
  assert(runner.stop_and_join() == ENGINESTATE::SUCCESSFUL);
  assert(engine.get_phase() == EnginePhase::STOPPED);
  cli.close();
}

} // namespace

int main() {
  test_cli_port_and_basic_lifecycle();
  test_db_dump_observes_timer_and_aio_updates();
  test_m5_publisher_and_cli_can_work_together();
  test_m6_udp_publisher_and_cli_can_work_together();
  test_stopping_rejects_new_commands();

  std::cout << "M7_STAGE8_ENGINE_INTEGRATION=PASS\n";
  return 0;
}
