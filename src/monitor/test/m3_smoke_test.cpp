#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

constexpr std::uint16_t kDemoPort = 9000;
constexpr auto kConnectTimeout = 2s;
constexpr auto kExitTimeout = 10s;

class ChildProcess {
public:
    ChildProcess() = default;

    ~ChildProcess()
    {
        terminate();

        if (_output_fd >= 0) {
            (void)::close(_output_fd);
        }
    }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    bool start(const char* executable)
    {
        int output_pipe[2]{-1, -1};
        if (::pipe(output_pipe) != 0) {
            return false;
        }

        const pid_t child = ::fork();
        if (child < 0) {
            (void)::close(output_pipe[0]);
            (void)::close(output_pipe[1]);
            return false;
        }

        if (child == 0) {
            (void)::close(output_pipe[0]);

            if (::dup2(output_pipe[1], STDOUT_FILENO) < 0
                || ::dup2(output_pipe[1], STDERR_FILENO) < 0) {
                _exit(126);
            }

            (void)::close(output_pipe[1]);
            ::execl(executable, executable, static_cast<char*>(nullptr));
            _exit(127);
        }

        (void)::close(output_pipe[1]);
        _pid = child;
        _output_fd = output_pipe[0];
        return true;
    }

    bool wait_for_exit(std::chrono::milliseconds timeout, int& exit_code)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        while (std::chrono::steady_clock::now() < deadline) {
            int status = 0;
            const pid_t result = ::waitpid(_pid, &status, WNOHANG);

            if (result == _pid) {
                _pid = -1;

                if (WIFEXITED(status)) {
                    exit_code = WEXITSTATUS(status);
                    return true;
                }

                exit_code = -1;
                return true;
            }

            if (result < 0 && errno != EINTR) {
                return false;
            }

            std::this_thread::sleep_for(10ms);
        }

        return false;
    }

    std::string read_output()
    {
        std::string output;
        char buffer[4096];

        while (true) {
            const ssize_t count = ::read(
                _output_fd,
                buffer,
                sizeof(buffer));

            if (count > 0) {
                output.append(
                    buffer,
                    static_cast<std::size_t>(count));
                continue;
            }

            if (count < 0 && errno == EINTR) {
                continue;
            }

            break;
        }

        return output;
    }

private:
    void terminate() noexcept
    {
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

int connect_to_demo(std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(kDemoPort);

        if (::inet_pton(
                AF_INET,
                "127.0.0.1",
                &address.sin_addr) != 1) {
            (void)::close(fd);
            return -1;
        }

        if (::connect(
                fd,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == 0) {
            return fd;
        }

        (void)::close(fd);
        std::this_thread::sleep_for(20ms);
    }

    return -1;
}

bool send_all(int fd, std::string_view message)
{
    std::size_t offset = 0;

    while (offset < message.size()) {
        const ssize_t count = ::send(
            fd,
            message.data() + offset,
            message.size() - offset,
            MSG_NOSIGNAL);

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

bool receive_echo(int fd)
{
    timeval timeout{};
    timeout.tv_sec = 2;

    if (::setsockopt(
            fd,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &timeout,
            sizeof(timeout)) != 0) {
        return false;
    }

    std::string response;
    char buffer[256];

    while (response.find('\n') == std::string::npos) {
        const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);

        if (count > 0) {
            response.append(
                buffer,
                static_cast<std::size_t>(count));
            continue;
        }

        if (count < 0 && errno == EINTR) {
            continue;
        }

        return false;
    }

    return response == "echo: hello-smoke\n";
}

bool contains(const std::string& output, std::string_view expected)
{
    return output.find(expected) != std::string::npos;
}

int fail(const char* message, const std::string& output = {})
{
    std::cerr << "[m3-smoke] " << message << '\n';

    if (!output.empty()) {
        std::cerr << "[m3-smoke] demo output:\n" << output;
    }

    return 1;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::cerr
            << "usage: m3_smoke_test <demo-executable>\n";
        return 2;
    }

    ChildProcess demo;
    if (!demo.start(argv[1])) {
        return fail("failed to start demo");
    }

    const int client_fd = connect_to_demo(kConnectTimeout);
    if (client_fd < 0) {
        return fail("demo did not accept TCP connections");
    }

    constexpr std::string_view request = "hello-smoke\n";
    const bool sent = send_all(client_fd, request);
    const bool echoed = sent && receive_echo(client_fd);
    (void)::close(client_fd);

    if (!sent) {
        return fail("failed to send smoke request");
    }

    if (!echoed) {
        return fail("demo did not return the expected echo");
    }

    int exit_code = -1;
    if (!demo.wait_for_exit(kExitTimeout, exit_code)) {
        return fail("demo did not stop after the shutdown timer");
    }

    const std::string output = demo.read_output();

    if (exit_code != 0) {
        return fail("demo exited with a non-zero status", output);
    }

    const std::string_view required_output[] = {
        "[heartbeat] tick #",
        "[slow] start",
        "[slow] done",
        "[server] accepted connection",
        "received: \"hello-smoke\"",
        "] disconnected",
        "[shutdown] stopping",
        "[demo] Engine stopped successfully",
        "callbacks=heartbeat count=",
        "callbacks=slow count=",
        "callbacks=accept count="
    };

    for (const std::string_view expected : required_output) {
        if (!contains(output, expected)) {
            std::cerr
                << "[m3-smoke] missing output: "
                << expected
                << '\n';
            return fail("demo output was incomplete", output);
        }
    }

    std::cout << "[m3-smoke] passed\n";
    return 0;
}
