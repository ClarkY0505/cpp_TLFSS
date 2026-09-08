#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
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
constexpr std::uint16_t DEMO_PORT = 9000U;

class ChildProcess final {
public:
  ~ChildProcess() {
    terminate();
    if (_output_fd >= 0) {
      (void)::close(_output_fd);
    }
  }

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

int connect_to_demo() {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
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

std::string receive_cli_output(int fd,
                               std::string_view terminator = " entries\n") {
  std::string response;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (response.find(terminator) == std::string::npos &&
         std::chrono::steady_clock::now() < deadline) {
    pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = POLLIN;
    int ready;
    do {
      ready = ::poll(&descriptor, 1, 200);
    } while (ready < 0 && errno == EINTR);
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

bool contains(const std::string &text, std::string_view expected) {
  return text.find(expected) != std::string::npos;
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc != 2) {
    std::cerr << "usage: m7_demo_smoke_test <demo-executable>\n";
    return 2;
  }

  ChildProcess demo;
  if (!demo.start(argv[1])) {
    std::cerr << "failed to start M7 demo\n";
    return 1;
  }

  const int client = connect_to_demo();
  if (client < 0) {
    std::cerr << "M7 demo did not open its CLI port\n";
    return 1;
  }

  /* 新连接先进入全局视图，使用服务名 storage 作为提示符。 */
  const std::string initial_prompt =
      receive_cli_output(client, "[storage]> ");

  /* modules 必须列出 Demo 注册的两个模块，并保留全局提示符。 */
  const bool modules_sent = send_all(client, "modules\n");
  const std::string modules_response =
      modules_sent ? receive_cli_output(client, "[storage]> ") : std::string{};

  /*
   * Timer 首次触发时间与进程调度有关，因此轮询 db_dump，而不依赖一个
   * 恰好足够的固定 sleep。直到两个模块的记录都出现，才继续验证过滤。
   */
  std::string all_dump;
  bool both_modules_ready = false;
  const auto records_deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < records_deadline) {
    if (!send_all(client, "db_dump\n")) {
      break;
    }
    all_dump = receive_cli_output(client, "[storage]> ");
    both_modules_ready = contains(all_dump, "* mid=256 ") &&
                         contains(all_dump, "* mid=512 ");
    if (both_modules_ready) {
      break;
    }
    std::this_thread::sleep_for(50ms);
  }

  /* help 的每一行都是系统回复，应带有统一的 "* " 前缀。 */
  const bool help_sent = send_all(client, "help\n");
  const std::string help_response =
      help_sent ? receive_cli_output(client, "[storage]> ") : std::string{};

  /* 切到 module-a 后，db_dump 只能看到 mid=256。 */
  const bool use_a_sent = send_all(client, "use module-a\n");
  const std::string use_a_response =
      use_a_sent ? receive_cli_output(client, "[module-a]> ") : std::string{};
  const bool dump_a_sent = send_all(client, "db_dump\n");
  const std::string module_a_dump =
      dump_a_sent ? receive_cli_output(client, "[module-a]> ") : std::string{};

  /* 每个 Session 可以继续切到 module-b，过滤结果随上下文改变。 */
  const bool use_b_sent = send_all(client, "use module-b\n");
  const std::string use_b_response =
      use_b_sent ? receive_cli_output(client, "[module-b]> ") : std::string{};
  const bool dump_b_sent = send_all(client, "db_dump\n");
  const std::string module_b_dump =
      dump_b_sent ? receive_cli_output(client, "[module-b]> ") : std::string{};

  /* use all 恢复全局上下文，随后由 CLI 主动停止 Demo。 */
  const bool use_all_sent = send_all(client, "use all\n");
  const std::string use_all_response =
      use_all_sent ? receive_cli_output(client, "[storage]> ") : std::string{};
  const bool stop_sent = send_all(client, "stop\n");
  const std::string stop_response =
      stop_sent ? receive_cli_output(client, "* stopping\n") : std::string{};
  (void)::close(client);

  const bool cli_valid =
      initial_prompt == "[storage]> " && modules_sent &&
      contains(modules_response, "* module-a mid=256\n") &&
      contains(modules_response, "* module-b mid=512\n") &&
      both_modules_ready && help_sent &&
      contains(help_response, "* db_dump    dump monitor records\n") &&
      contains(help_response, "* help       list available commands\n") &&
      contains(help_response, "* modules    list registered modules\n") &&
      contains(help_response, "* stop       stop monitor engine\n") &&
      contains(help_response, "* use        select active module\n") &&
      use_a_sent &&
      contains(use_a_response,
               "* using module: module-a\n[module-a]> ") &&
      dump_a_sent && contains(module_a_dump, "* mid=256 ") &&
      !contains(module_a_dump, "mid=512 ") &&
      contains(module_a_dump, "* 1 entries\n") && use_b_sent &&
      contains(use_b_response,
               "* using module: module-b\n[module-b]> ") &&
      dump_b_sent && contains(module_b_dump, "* mid=512 ") &&
      !contains(module_b_dump, "mid=256 ") &&
      contains(module_b_dump, "* 1 entries\n") && use_all_sent &&
      contains(use_all_response, "* using module: all\n[storage]> ") &&
      stop_sent && contains(stop_response, "* stopping\n");

  if (!cli_valid) {
    std::cerr << "M7 CLI response was incomplete:\n"
              << "initial: " << initial_prompt << "\nmodules:\n"
              << modules_response << "\nall dump:\n"
              << all_dump << "\nhelp:\n"
              << help_response << "\nuse module-a:\n"
              << use_a_response << "\nmodule-a dump:\n"
              << module_a_dump << "\nuse module-b:\n"
              << use_b_response << "\nmodule-b dump:\n"
              << module_b_dump << "\nuse all:\n"
              << use_all_response << "\nstop:\n"
              << stop_response;
    return 1;
  }

  int exit_code = -1;
  if (!demo.wait_for_exit(10s, exit_code)) {
    std::cerr << "M7 demo did not stop after the CLI stop command\n";
    return 1;
  }

  const std::string output = demo.read_output();
  const std::string_view required[] = {
      "[demo] CLI listening on 127.0.0.1:9000",
      "[demo] modules: module-a, module-b",
      "[module-a] value=",
      "[module-b] start",
      "[module-b] value=",
      "[demo] stop requested from CLI",
      "[demo] Engine stopped successfully",
      "callbacks=module-a-heartbeat count=",
      "callbacks=slow count=",
      "callbacks=cli-listener count="};

  if (exit_code != 0) {
    std::cerr << "M7 demo exited with code " << exit_code << "\n" << output;
    return 1;
  }
  for (const std::string_view expected : required) {
    if (!contains(output, expected)) {
      std::cerr << "missing demo output: " << expected << "\n" << output;
      return 1;
    }
  }

  std::cout << "M7_STAGE11_DEMO_SMOKE=PASS\n";
  return 0;
}
