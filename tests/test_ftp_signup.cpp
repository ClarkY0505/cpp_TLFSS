#include "ftp/server.h"
#include "common/base/mysql_connection.h"
#include "logger/logger.h"

#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class Client {
public:
    explicit Client(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        require(fd_ >= 0, "socket failed");
        timeval timeout{5, 0};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            ::close(fd_);
            throw std::runtime_error("connect failed");
        }
    }
    ~Client() { ::close(fd_); }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void send(const std::string& data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const auto count = ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            require(count > 0, "send failed");
            sent += static_cast<std::size_t>(count);
        }
    }
    std::string until(const std::string& marker) {
        return until_any({marker});
    }
    std::string until_any(std::initializer_list<std::string> markers) {
        for (;;) {
            for (const auto& marker : markers) {
                const auto position = buffered_.find(marker);
                if (position != std::string::npos) {
                    auto result = buffered_.substr(0, position + marker.size());
                    buffered_.erase(0, result.size());
                    return result;
                }
            }
            char bytes[2048];
            const auto count = ::recv(fd_, bytes, sizeof(bytes), 0);
            if (count <= 0) {
                throw std::runtime_error("expected server response missing: " + *markers.begin());
            }
            buffered_.append(bytes, static_cast<std::size_t>(count));
            require(buffered_.size() < 16384, "unexpected response size");
        }
    }
private:
    int fd_ = -1;
    std::string buffered_;
};

std::uint16_t available_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    require(fd >= 0, "port reservation socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int bound = ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    socklen_t size = sizeof(address);
    const int named = ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size);
    ::close(fd);
    require(bound == 0 && named == 0, "port reservation failed");
    return ntohs(address.sin_port);
}

TEST(FtpSignupIntegrationTest, RegistersLogsInListsAndSerializesDuplicateNames) {
    // Opt-in: uses config/mysql.cnf, creates unique test accounts, then removes them. / 显式启用：使用 config/mysql.cnf，创建名称唯一的测试账号，然后删除。
    const auto* enabled = std::getenv("TLSS_TEST_FTP_SIGNUP");
    if (!enabled || std::string(enabled) != "1") {
        GTEST_SKIP() << "Set TLSS_TEST_FTP_SIGNUP=1 for local TCP/MySQL integration";
    }
    for (const auto* name : {"mysql", "net", "storage_service"}) {
        TLSSLOG::Logger::initConsole(name, spdlog::level::off);
    }

    // Independent connection remains usable after StorageService stops its pool. / StorageService 关闭连接池后，独立连接仍可使用。
    const auto executable = std::filesystem::read_symlink("/proc/self/exe");
    std::ifstream input(executable.parent_path().parent_path() / "config/mysql.cnf");
    ASSERT_TRUE(input.good());
    std::map<std::string, std::string> config;
    std::string line;
    const auto trim = [](std::string value) {
        const auto begin = value.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) return std::string{};
        return value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
    };
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto equal = line.find('=');
        if (equal != std::string::npos) {
            config[trim(line.substr(0, equal))] = trim(line.substr(equal + 1));
        }
    }
    TLSS::BASE::DbConnection control;
    ASSERT_TRUE(control.connect(config.at("ip"),
                                static_cast<unsigned short>(std::stoi(config.at("port"))),
                                config.at("username"), config.at("password"), "tlss_db"));
    const std::string name = "tlss_test_" + std::to_string(::getpid()) + "_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string race = name + "r";
    const std::string abandoned = name + "a";
    const auto existing = control.query_prepared(
        "SELECT username FROM users WHERE username IN (?, ?, ?)", {name, race, abandoned});
    ASSERT_TRUE(existing.success);
    ASSERT_TRUE(existing.rows.empty());

    bool stopped = false;
    std::exception_ptr failure;
    {
        TLSS::NET::EventLoop loop;
        const auto port = available_port();
        TLSS::FileStorage::StorageService server(
            &loop, TLSS::NET::InetAddress(port), "signup-test", 2, 2, 16);
        server.start();
        std::thread controller([&] {
            try {
                Client client(port);
                client.until("tlss>");
                client.send("ls\r\n");
                client.until("Please enter login or signup:\r\ntlss>");
                client.send("signup\r\n");
                client.until("underscore):");
                client.send("../invalid\r\n");
                client.until("New username:");
                // Split a frame across two sends, then pipeline the remaining input. / 将一帧拆成两次发送，再流水线发送剩余输入。
                client.send(name.substr(0, 8));
                client.send(name.substr(8) + "\r\nshort\r\n");
                client.until("New password:");
                client.send(" password123 \r\nwrong\r\n");
                client.until("Passwords do not match.\r\nNew password:");
                client.send(" password123 \r\n password123 \r\nls\r\n");
                const auto registered = client.until("tlss>");
                require(registered.find("Signup successful") != std::string::npos,
                        "signup did not succeed");
                require(registered.find("Please wait.") != std::string::npos,
                        "command was not blocked during registration");
                client.send("ls\r\n");
                client.until("Please enter login or signup:\r\ntlss>");
                client.send("login\r\n" + name + "\r\nwrong_password\r\n");
                client.until("Invalid username or password.\r\nUsername:");
                client.send(name + "\r\n password123 \r\n");
                client.until("Login successful.\r\ntlss@" + name + ":/>");
                client.send("pwd\r\n");
                client.until("/\r\ntlss@" + name + ":/>");
                client.send("ls\r\n");
                require(client.until("tlss@" + name + ":/>").find("Query failed") ==
                            std::string::npos, "new user's root was not usable by ls");
                client.send("cd ..\r\n");
                client.until("tlss@" + name + ":/>");

                // Root and nested mkdir paths must remain usable by cd, pwd and ls. / 在根目录和嵌套目录通过 mkdir 创建的路径必须仍可供 cd、pwd 和 ls 使用。
                client.send("mkdir q\r\n");
                client.until("tlss@" + name + ":/>");
                client.send("cd q\r\n");
                client.until("tlss@" + name + ":/q>");
                client.send("pwd\r\n");
                client.until("/q\r\ntlss@" + name + ":/q>");
                client.send("ls\r\n");
                require(client.until("tlss@" + name + ":/q>").find("Query failed") ==
                            std::string::npos, "created directory path was not usable by ls");
                client.send("mkdir sub\r\n");
                client.until("tlss@" + name + ":/q>");
                client.send("cd sub\r\n");
                client.until("tlss@" + name + ":/q/sub>");
                client.send("cd ..\r\n");
                client.until("tlss@" + name + ":/q>");
                client.send("cd ..\r\n");
                client.until("tlss@" + name + ":/>");

                Client first(port), second(port);
                first.until("tlss>");
                second.until("tlss>");
                const auto signup = "signup\r\n" + race + "\r\npassword123\r\npassword123\r\n";
                first.send(signup);
                second.send(signup);
                const auto result1 = first.until_any({"tlss>", "\r\nNew username:"});
                const auto result2 = second.until_any({"tlss>", "\r\nNew username:"});
                const bool first_won = result1.find("Signup successful") != std::string::npos;
                const bool second_won = result2.find("Signup successful") != std::string::npos;
                require(first_won != second_won, "duplicate race must have exactly one winner");
                require((first_won ? result2 : result1).find("Username is already taken") !=
                            std::string::npos, "duplicate race did not report username taken");
                {
                    Client partial(port);
                    partial.until("tlss>");
                    partial.send("signup\r\n" + abandoned + "\r\npassword123\r\n");
                    partial.until("Confirm password:");
                    // Closing before confirmation must not create an account. / 确认前关闭连接不得创建账号。
                }
            } catch (...) {
                failure = std::current_exception();
            }
            stopped = server.stop(std::chrono::seconds(1));
        });
        loop.loop();
        controller.join();
    }

    // Always clean up our names after all worker tasks have drained, even on failure. / 所有工作任务结束后，始终清理本测试使用的名称，即使测试失败也要清理。
    const auto rows = control.query_prepared(
        "SELECT u.username, COUNT(v.id) FROM users u LEFT JOIN virtual_files v "
        "ON v.username=u.username AND v.parent_id=-1 "
        "WHERE u.username IN (?, ?, ?) GROUP BY u.username",
        {name, race, abandoned});
    {
        TLSS::BASE::DbTransaction cleanup(control);
        ASSERT_TRUE(cleanup.active());
        ASSERT_TRUE(control.execute_prepared(
            "DELETE FROM virtual_files WHERE username IN (?, ?, ?)", {name, race, abandoned}).success);
        ASSERT_TRUE(control.execute_prepared(
            "DELETE FROM users WHERE username IN (?, ?, ?)", {name, race, abandoned}).success);
        ASSERT_EQ(cleanup.commit(), TLSS::BASE::DbCommitStatus::k_committed);
    }
    EXPECT_TRUE(stopped);
    if (failure) {
        try { std::rethrow_exception(failure); }
        catch (const std::exception& error) { FAIL() << error.what(); }
    }
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 2U);
    for (const auto& row : rows.rows) EXPECT_EQ(row[1], "1");
}
}  // namespace / 命名空间结束
