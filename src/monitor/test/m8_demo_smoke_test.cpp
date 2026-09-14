#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <iostream>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

constexpr std::uint16_t DEMO_PORT = 9001U;

/*
 * 启动 Demo 子进程并接管它的 stdout/stderr。
 *
 * 测试结束时如果 Demo 尚未退出，析构函数会终止并回收它，避免失败案例
 * 留下后台进程占用 9001 端口。
 */
class ChildProcess final {
public:
  ~ChildProcess() {
    terminate();

    if (_output_fd >= 0) {
      (void)::close(_output_fd);
    }
  }

  ChildProcess(const ChildProcess &) = delete;
  ChildProcess &operator=(const ChildProcess &) = delete;

  ChildProcess() = default;

  bool start(const char *executable) {
    int output[2]{-1, -1};

    if (::pipe(output) != 0) {
      return false;
    }

    const pid_t child = ::fork();

    if (child < 0) {
      (void)::close(output[0]);
      (void)::close(output[1]);
      return false;
    }

    if (child == 0) {
      (void)::close(output[0]);

      if (::dup2(output[1], STDOUT_FILENO) < 0 ||
          ::dup2(output[1], STDERR_FILENO) < 0) {
        _exit(126);
      }

      (void)::close(output[1]);
      ::execl(executable, executable, static_cast<char *>(nullptr));
      _exit(127);
    }

    (void)::close(output[1]);
    _pid = child;
    _output_fd = output[0];
    return true;
  }

  bool wait_for_exit(std::chrono::milliseconds timeout, int &exit_code) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
      int status = 0;
      const pid_t result = ::waitpid(_pid, &status, WNOHANG);

      if (result == _pid) {
        _pid = -1;
        exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        return true;
      }

      if (result < 0 && errno != EINTR) {
        return false;
      }

      std::this_thread::sleep_for(10ms);
    }

    return false;
  }

  std::string read_output() {
    std::string output;
    char buffer[4096];

    for (;;) {
      const ssize_t count = ::read(_output_fd, buffer, sizeof(buffer));

      if (count > 0) {
        output.append(buffer, static_cast<std::size_t>(count));
        continue;
      }

      if (count < 0 && errno == EINTR) {
        continue;
      }

      return output;
    }
  }

private:
  void terminate() noexcept {
    if (_pid <= 0) {
      return;
    }

    (void)::kill(_pid, SIGKILL);

    int status = 0;
    while (::waitpid(_pid, &status, 0) < 0 && errno == EINTR) {
    }

    _pid = -1;
  }

  pid_t _pid{-1};
  int _output_fd{-1};
};

bool contains(const std::string &text, std::string_view expected) {
  return text.find(expected) != std::string::npos;
}

/* 等待固定端口上的 M8 Demo 完成监听，避免依赖固定 sleep。 */
int connect_to_demo() {
  const auto deadline = std::chrono::steady_clock::now() + 3s;

  while (std::chrono::steady_clock::now() < deadline) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (fd < 0) {
      return -1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(DEMO_PORT);

    if (::connect(fd, reinterpret_cast<const sockaddr *>(&address),
                  sizeof(address)) == 0) {
      return fd;
    }

    (void)::close(fd);
    std::this_thread::sleep_for(20ms);
  }

  return -1;
}

bool send_all(int fd, std::string_view text) {
  std::size_t offset = 0U;
  const auto deadline = std::chrono::steady_clock::now() + 1s;

  while (offset < text.size() &&
         std::chrono::steady_clock::now() < deadline) {
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

  return offset == text.size();
}

/*
 * 每条命令都读取到下一次提示符；这样不会把上一条命令残留的数据误算进
 * 下一条命令的响应。
 */
std::string receive_until(int fd, std::string_view terminator,
                          std::chrono::milliseconds timeout = 3s) {
  std::string response;
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (!contains(response, terminator) &&
         std::chrono::steady_clock::now() < deadline) {
    pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = POLLIN;

    int ready;
    do {
      ready = ::poll(&descriptor, 1, 200);
    } while (ready < 0 && errno == EINTR &&
             std::chrono::steady_clock::now() < deadline);

    if (ready <= 0) {
      continue;
    }

    char buffer[4096];
    const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);

    if (count > 0) {
      response.append(buffer, static_cast<std::size_t>(count));
      continue;
    }

    if (count < 0 && errno == EINTR) {
      continue;
    }

    break;
  }

  return response;
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc != 2) {
    std::cerr << "usage: m8_demo_smoke_test <demo-executable>\n";
    return 2;
  }

  ChildProcess demo;

  if (!demo.start(argv[1])) {
    std::cerr << "failed to start M8 demo\n";
    return 1;
  }

  const int client = connect_to_demo();

  if (client < 0) {
    std::cerr << "M8 demo did not open its CLI port\n";
    return 1;
  }

  /* 新连接必须从 Engine 的全局模块视图开始。 */
  const std::string initial_prompt = receive_until(client, "[storage]> ");

  const bool modules_sent = send_all(client, "modules\n");
  const std::string modules_response =
      modules_sent ? receive_until(client, "[storage]> ") : std::string{};

  /*
   * error 记录在 run() 前写入；heartbeat 由 Timer 产生。轮询到两条记录，
   * 同时验证错误 level 被规范化、描述被补全且首次错误零值被保存。
   */
  std::string dump;
  bool records_ready = false;
  const auto records_deadline = std::chrono::steady_clock::now() + 5s;

  while (std::chrono::steady_clock::now() < records_deadline) {
    if (!send_all(client, "db_dump\n")) {
      break;
    }

    dump = receive_until(client, "[storage]> ");
    records_ready =
        contains(dump, "* mid=256 lvl=info fid=1 eid=0 ") &&
        contains(dump, "state=0 desc=\"heartbeat\"") &&
        contains(dump, "* mid=256 lvl=warn fid=1 eid=1 num=0 state=2 "
                       "desc=\"metadata timeout\"") &&
        contains(dump, "* 2 entries\n") && !contains(dump, "lvl=?");

    if (records_ready) {
      break;
    }

    std::this_thread::sleep_for(50ms);
  }

  /* use 必须读取 Engine 内部注册表并更新当前 Session 的提示符。 */
  const bool use_sent = send_all(client, "use module-a\n");
  const std::string use_response =
      use_sent ? receive_until(client, "[module-a]> ") : std::string{};

  const bool module_dump_sent = send_all(client, "db_dump\n");
  const std::string module_dump =
      module_dump_sent ? receive_until(client, "[module-a]> ") : std::string{};

  const bool stop_sent = send_all(client, "stop\n");
  const std::string stop_response =
      stop_sent ? receive_until(client, "* stopping\n") : std::string{};

  (void)::close(client);

  const bool cli_valid =
      initial_prompt == "[storage]> " && modules_sent &&
      contains(modules_response, "* module-a mid=256\n") && records_ready &&
      use_sent &&
      contains(use_response, "* using module: module-a\n[module-a]> ") &&
      module_dump_sent &&
      contains(module_dump, "* mid=256 lvl=info fid=1 eid=0 ") &&
      contains(module_dump, "* mid=256 lvl=warn fid=1 eid=1 ") &&
      contains(module_dump, "* 2 entries\n") && !contains(module_dump, "lvl=?") &&
      stop_sent && contains(stop_response, "* stopping\n");

  if (!cli_valid) {
    std::cerr << "M8 CLI response was incomplete:\n"
              << "initial:\n"
              << initial_prompt << "\nmodules:\n"
              << modules_response << "\ndump:\n"
              << dump << "\nuse:\n"
              << use_response << "\nmodule dump:\n"
              << module_dump << "\nstop:\n"
              << stop_response;
    return 1;
  }

  int exit_code = -1;
  if (!demo.wait_for_exit(10s, exit_code)) {
    std::cerr << "M8 demo did not stop after CLI stop\n";
    return 1;
  }

  const std::string process_output = demo.read_output();
  const std::string_view required_output[] = {
      "[demo] M8 module registered",
      "[demo] CLI listening on 127.0.0.1:9001",
      "[demo] module: module-a",
      "[module-a] heartbeat=",
      "[demo] stop requested from CLI",
      "[demo] Engine stopped successfully",
      "[demo] final records=2",
      "callbacks=m8-heartbeat count=",
      "callbacks=cli-listener count="};

  if (exit_code != 0) {
    std::cerr << "M8 demo exited with code " << exit_code << '\n'
              << process_output;
    return 1;
  }

  for (const std::string_view expected : required_output) {
    if (!contains(process_output, expected)) {
      std::cerr << "missing demo output: " << expected << '\n'
                << process_output;
      return 1;
    }
  }

  std::cout << "M8_STAGE13_DEMO_SMOKE=PASS\n";
  return 0;
}
