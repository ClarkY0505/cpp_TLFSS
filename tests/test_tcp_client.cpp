#include "common/net/client.h"
#include "common/net/buffer.h"
#include "common/net/channel.h"
#include "common/net/connector.h"
#include "common/net/event_loop.h"
#include "common/net/event_loop_thread.h"
#include "common/net/inet_address.h"
#include "common/net/tcp_connection.h"
#include "common/net/net_logger.h"
#include "spdlog/sinks/base_sink.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace std::chrono_literals;
using namespace TLSS::NET;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::abort();
    }
}

template <typename T>
T await(std::future<T>& result) {
    require(result.wait_for(5s) == std::future_status::ready, "asynchronous operation timed out");
    return result.get();
}

void drain(EventLoop* loop) {
    // Channel 注销可能再排队一次；两个屏障覆盖这些延迟释放。 / Channel deregistration may enqueue another task; two barriers cover these deferred releases.
    for (int i = 0; i < 2; ++i) {
        std::promise<void> ready;
        auto result = ready.get_future();
        loop->queue_in_loop([&] { ready.set_value(); });
        await(result);
    }
}

class Listener {
public:
    explicit Listener(bool start = true) : _fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
        require(_fd >= 0, "socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(::bind(_fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0,
                "bind failed");
        socklen_t size = sizeof address;
        require(::getsockname(_fd, reinterpret_cast<sockaddr*>(&address), &size) == 0,
                "getsockname failed");
        _port = ntohs(address.sin_port);
        if (start) {
            listen();
        }
    }
    ~Listener() { ::close(_fd); }
    uint16_t port() const { return _port; }
    void listen() { require(::listen(_fd, 8) == 0, "listen failed"); }
    bool readable(int timeout_ms) const {
        pollfd event{_fd, POLLIN, 0};
        return ::poll(&event, 1, timeout_ms) > 0 && (event.revents & POLLIN);
    }
    int accept() const {
        require(readable(5000), "accept timed out");
        int fd = ::accept4(_fd, nullptr, nullptr, SOCK_CLOEXEC);
        require(fd >= 0, "accept failed");
        timeval timeout{5, 0};
        require(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) == 0,
                "setsockopt failed");
        return fd;
    }
private:
    int _fd;
    uint16_t _port;
};

void receive(int fd, const std::string& expected) {
    std::string message(expected.size(), '\0');
    require(::recv(fd, message.data(), message.size(), MSG_WAITALL) ==
                static_cast<ssize_t>(message.size()), "recv failed");
    require(message == expected, "unexpected message");
}

void connects_exchanges_messages_and_disconnects() {
    Listener server;
    EventLoopThread thread;
    EventLoop* loop = thread.start_loop();
    TcpClient client(loop, InetAddress(server.port()), "echo-client");
    require(client.get_loop() == loop && client.name() == "echo-client", "wrong client metadata");
    require(!client.retry() && !client.connection(), "wrong client defaults");
    std::promise<void> up, down, written;
    std::promise<std::string> message;
    auto up_result = up.get_future();
    auto down_result = down.get_future();
    auto written_result = written.get_future();
    auto message_result = message.get_future();
    client.set_connection_cb([&](const TcpConnectionPtr& conn) {
        require(loop->is_in_loop_thread(), "callback used wrong thread");
        if (conn->connected()) {
            require(conn->peer_addr().to_port() == server.port(), "wrong peer address");
            require(conn->local_addr().to_port() != 0, "missing local address");
            up.set_value();
        } else {
            down.set_value();
        }
    });
    client.set_message_cb([&](const TcpConnectionPtr&, Buffer* buffer, TLSS::TIME::Timestamp) {
        message.set_value(buffer->retrieve_all_as_string());
    });
    client.set_write_complete_cb([&](const TcpConnectionPtr&) { written.set_value(); });
    client.connect();
    client.connect();
    int peer = server.accept();
    await(up_result);
    client.connection()->send("request");
    receive(peer, "request");
    await(written_result);
    require(::send(peer, "reply", 5, MSG_NOSIGNAL) == 5, "send failed");
    require(await(message_result) == "reply", "message callback failed");
    client.disconnect();
    char byte;
    require(::recv(peer, &byte, 1, 0) == 0, "disconnect did not half-close");
    ::close(peer);
    await(down_result);
    drain(loop);
    require(!client.connection(), "disconnected connection was retained");
}

void stop_preserves_connection_and_disables_reconnect() {
    Listener server;
    EventLoopThread thread;
    EventLoop* loop = thread.start_loop();
    TcpClient client(loop, InetAddress(server.port()), "stop-client");
    client.enable_retry();
    require(client.retry(), "enable_retry failed");
    std::promise<void> up, down;
    auto up_result = up.get_future();
    auto down_result = down.get_future();
    client.set_connection_cb([&](const TcpConnectionPtr& conn) {
        if (conn->connected()) up.set_value();
        else down.set_value();
    });
    client.connect();
    int peer = server.accept();
    await(up_result);
    client.stop();
    drain(loop);
    require(client.connection()->connected(), "stop closed the established connection");
    client.connection()->send("still-open");
    receive(peer, "still-open");
    ::close(peer);
    await(down_result);
    drain(loop);
    require(!server.readable(650), "stop allowed automatic reconnect");
}

void reconnects_after_peer_close(bool automatic) {
    Listener server;
    EventLoopThread thread;
    EventLoop* loop = thread.start_loop();
    TcpClient client(loop, InetAddress(server.port()), "reconnect-client");
    if (automatic) client.enable_retry();
    std::promise<std::string> first, second;
    std::promise<void> down;
    auto first_result = first.get_future();
    auto second_result = second.get_future();
    auto down_result = down.get_future();
    int connections = 0;
    client.set_connection_cb([&](const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            if (++connections == 1) first.set_value(conn->name());
            else second.set_value(conn->name());
        } else if (connections == 1) {
            down.set_value();
        }
    });
    client.connect();
    int peer = server.accept();
    const auto first_name = await(first_result);
    ::close(peer);
    await(down_result);
    if (!automatic) {
        drain(loop);
        require(!server.readable(650), "retry was unexpectedly enabled");
        client.connect();
    }
    peer = server.accept();
    require(await(second_result) != first_name, "reconnected connection name was reused");
    client.stop();
    ::close(peer);
    drain(loop);
}

void retries_initial_connection_and_cancels_retry(bool cancel, bool disconnect) {
    Listener server(false);
    EventLoopThread thread;
    EventLoop* loop = thread.start_loop();
    auto client = std::make_unique<TcpClient>(loop, InetAddress(server.port()), "retry-client");
    std::promise<void> up;
    auto up_result = up.get_future();
    client->set_connection_cb([&](const TcpConnectionPtr& conn) {
        if (conn->connected()) up.set_value();
    });
    client->connect();
    // 端口已绑定但尚未 listen，首次连接一定被拒绝。 / The port is bound but not yet listening, so the first connection must be refused.
    std::this_thread::sleep_for(100ms);
    drain(loop);
    if (cancel) {
        if (disconnect) client->disconnect();
        else client.reset();
        drain(loop);
    }
    server.listen();
    if (cancel) {
        require(!server.readable(750), "cancelled connection retried");
    } else {
        int peer = server.accept();
        await(up_result);
        client.reset();
        drain(loop);
        char byte;
        require(::recv(peer, &byte, 1, 0) == 0, "client destruction did not close connection");
        ::close(peer);
    }
}

void destruction_preserves_external_connection() {
    Listener server;
    EventLoopThread thread;
    EventLoop* loop = thread.start_loop();
    auto client = std::make_unique<TcpClient>(loop, InetAddress(server.port()), "owned-client");
    std::promise<void> up;
    auto up_result = up.get_future();
    client->set_connection_cb([&](const TcpConnectionPtr& conn) {
        if (conn->connected()) up.set_value();
    });
    client->connect();
    int peer = server.accept();
    await(up_result);
    TcpConnectionPtr external = client->connection();
    std::weak_ptr<TcpConnection> weak = external;
    client.reset();
    drain(loop);
    external->send("retained");
    receive(peer, "retained");
    external->force_close();
    drain(loop);
    require(external->disconnected(), "external connection close failed");
    external.reset();
    drain(loop);
    require(weak.expired(), "closed connection leaked");
    ::close(peer);
}

void destruction_before_loop_handles_queued_work() {
    Listener server;
    EventLoop loop;
    {
        TcpClient client(&loop, InetAddress(server.port()), "pending-client");
        client.connect();
        client.stop();
    }
    loop.queue_in_loop([&] { loop.quit(); });
    loop.wakeup();
    loop.loop();
    require(!server.readable(100), "destroyed client connected");
}

void rejects_null_loop() {
    bool rejected = false;
    try {
        TcpClient client(nullptr, InetAddress(1), "invalid-client");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "null loop was accepted");
}

void last_external_release_runs_on_loop() {
    Listener server;
    EventLoopThread thread;
    EventLoop* loop = thread.start_loop();
    std::promise<void> up;
    std::promise<bool> released;
    auto up_result = up.get_future();
    auto released_result = released.get_future();
    auto marker = std::shared_ptr<int>(new int(0), [&](int* value) {
        delete value;
        released.set_value(loop->is_in_loop_thread());
    });
    auto client = std::make_unique<TcpClient>(loop, InetAddress(server.port()), "release-client");
    client->set_connection_cb([marker, &up](const TcpConnectionPtr& conn) {
        if (conn->connected()) up.set_value();
    });
    marker.reset();
    client->connect();
    int peer = server.accept();
    await(up_result);
    auto external = client->connection();
    client.reset();
    drain(loop);
    external.reset();
    require(await(released_result), "last external release destroyed connection outside its loop");
    drain(loop);
    char byte;
    require(::recv(peer, &byte, 1, 0) == 0, "last external release left socket open");
    ::close(peer);
}

void stale_connector_events_do_not_cancel_new_attempt() {
    Listener server(false);
    EventLoop loop;
    auto connector = std::make_shared<Connector>(&loop, InetAddress(server.port()));
    std::promise<void> connected;
    std::promise<void> started;
    auto connected_result = connected.get_future();
    auto started_result = started.get_future();
    connector->set_new_connection_cb([&](int fd) {
        ::close(fd);
        connected.set_value();
    });
    const int control_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    require(control_fd >= 0, "eventfd failed");
    Channel control(&loop, control_fd);
    control.set_read_cb([&](TLSS::TIME::Timestamp) {
        std::uint64_t value;
        require(::read(control_fd, &value, sizeof value) == sizeof value, "eventfd read failed");
        control.disable_all();
        control.remove();
        started.set_value();
        server.listen();
        connector->restart();
    });
    control.enable_reading();
    // start 现在统一入队：在同一批 pending functors 中先使 control 就绪， / start is now always queued: first make control ready within the same batch of pending functors,
    // 再执行 start，确保下一轮 poll 同时看到 control 和旧 socket 的失败事件。 / Then run start so the next poll sees both control and failure events from the old socket.
    loop.queue_in_loop([&] {
        const std::uint64_t one = 1;
        require(::write(control_fd, &one, sizeof one) == sizeof one, "eventfd write failed");
    });
    connector->start();
    loop.wakeup();
    bool completed = false;
    std::thread controller([&] {
        await(started_result);
        completed = connected_result.wait_for(5s) == std::future_status::ready;
        connector->stop();
        loop.queue_in_loop([&] { loop.quit(); });
    });
    loop.loop();
    controller.join();
    ::close(control_fd);
    require(completed, "connector did not complete the new attempt");
    const int peer = server.accept();
    ::close(peer);
    // 旧事件误取消新 attempt 会产生一次额外的退避重试和第二个已接收 socket。 / If an old event incorrectly cancels a new attempt, an extra backoff retry and a second accepted socket result.
    require(!server.readable(0), "stale connector event cancelled the new attempt");
}

void destroys_client_from_connection_callback() {
    Listener server;
    EventLoopThread thread;
    EventLoop* loop = thread.start_loop();
    std::promise<void> down;
    auto down_result = down.get_future();
    std::weak_ptr<TcpConnection> weak;
    auto client = std::make_unique<TcpClient>(loop, InetAddress(server.port()), "callback-client");
    client->set_connection_cb([&](const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            weak = conn;
            client.reset();
        } else {
            down.set_value();
        }
    });
    client->connect();
    int peer = server.accept();
    await(down_result);
    drain(loop);
    require(weak.expired(), "callback destruction retained the connection");
    char byte;
    require(::recv(peer, &byte, 1, 0) == 0, "callback destruction left socket open");
    ::close(peer);
}

// 在 TcpConnection 构造完成、State 发布连接之前，精确暂停 loop。 / Pause the loop precisely after TcpConnection construction and before State publishes the connection.
class ConstructionBarrier : public spdlog::sinks::base_sink<std::mutex> {
public:
    std::promise<void> entered;
    std::shared_future<void> resume;
private:
    void sink_it_(const spdlog::details::log_msg& message) override {
        const std::string text(message.payload.data(), message.payload.size());
        if (text.find("TcpConnection::ctor[publish-client:") != std::string::npos) {
            entered.set_value();
            resume.wait();
        }
    }
    void flush_() override {}
};

void destruction_during_connection_construction_cancels_publication() {
    Listener server;
    auto barrier = std::make_shared<ConstructionBarrier>();
    std::promise<void> resume;
    barrier->resume = resume.get_future().share();
    auto entered_result = barrier->entered.get_future();
    auto logger = net_logger();
    logger->sinks().push_back(barrier);
    std::atomic<int> callbacks{0};
    {
        EventLoopThread thread;
        EventLoop* loop = thread.start_loop();
        auto client = std::make_unique<TcpClient>(loop, InetAddress(server.port()), "publish-client");
        client->set_connection_cb([&](const TcpConnectionPtr&) { ++callbacks; });
        client->connect();
        int peer = server.accept();
        await(entered_result);
        client.reset();
        resume.set_value();
        drain(loop);
        require(callbacks.load() == 0, "destroyed client published a connection");
        ::close(peer);
    }
    logger->sinks().pop_back();
}

// stop 从外部线程排队，随后 loop 线程提交 start；后者不能越过旧 stop。 / Queue stop from an external thread, then submit start on the loop thread; start must not overtake the earlier stop.
void connector_commands_preserve_submission_order() {
    Listener server(false);
    EventLoop loop;
    auto connector = std::make_shared<Connector>(&loop, InetAddress(server.port()));
    std::promise<void> connected;
    auto result = connected.get_future();
    connector->set_new_connection_cb([&](int fd) {
        ::close(fd);
        connected.set_value();
    });
    connector->start();
    std::thread stopper([connector] { connector->stop(); });
    stopper.join();
    server.listen();
    connector->start();
    bool completed = false;
    std::thread observer([&] {
        completed = result.wait_for(2s) == std::future_status::ready;
        connector->stop();
        loop.queue_in_loop([&] { loop.quit(); });
    });
    loop.loop();
    observer.join();
    require(completed, "later start was cancelled by an earlier queued stop");
}

void connector_restart_from_another_thread() {
    Listener server;
    EventLoopThread thread;
    auto* loop = thread.start_loop();
    auto connector = std::make_shared<Connector>(loop, InetAddress(server.port()));
    std::promise<void> connected;
    auto result = connected.get_future();
    connector->set_new_connection_cb([&](int fd) {
        require(loop->is_in_loop_thread(), "connector callback used wrong thread");
        ::close(fd);
        connected.set_value();
    });
    connector->restart();
    await(result);
    connector->stop();
    drain(loop);
}

// 业务回调可以替换下一次连接的回调，正在执行的 std::function 必须保活。 / A business callback may replace the callback for the next connection; the currently executing std::function must stay alive.
void connector_callback_can_replace_itself() {
    Listener server;
    EventLoopThread thread;
    auto* loop = thread.start_loop();
    auto connector = std::make_shared<Connector>(loop, InetAddress(server.port()));
    std::promise<void> replaced;
    auto result = replaced.get_future();
    auto marker = std::make_shared<int>(42);
    connector->set_new_connection_cb([connector, marker, &replaced](int fd) {
        ::close(fd);
        connector->set_new_connection_cb({});
        require(*marker == 42, "running callback lost its captures");
        replaced.set_value();
    });
    marker.reset();
    connector->start();
    await(result);
    connector->stop();
    drain(loop);
}

// 替换业务回调会释放旧捕获对象，其析构可能重入客户端接口。 / Replacing a business callback releases the old captured object, whose destructor may reenter the client API.
void replacing_client_callback_allows_reentrant_release() {
    EventLoop loop;
    {
        TcpClient client(&loop, InetAddress(1), "callback-release-client");
        bool released = false;
        auto marker = std::shared_ptr<int>(new int(1), [&](int* value) {
            delete value;
            client.stop();
            released = true;
        });
        client.set_message_cb([marker](const TcpConnectionPtr&, Buffer*, TLSS::TIME::Timestamp) {});
        marker.reset();
        client.set_message_cb({});
        require(released, "replaced callback retained its captures");
    }
    loop.queue_in_loop([&] { loop.quit(); });
    loop.wakeup();
    loop.loop();
}
}  // namespace / 命名空间结束

int main(int argc, char** argv) {
    if (argc == 2) {
        const std::string scenario = argv[1];
        if (scenario == "callback_release") replacing_client_callback_allows_reentrant_release();
        else if (scenario == "connector_order") connector_commands_preserve_submission_order();
        else if (scenario == "connector_restart") connector_restart_from_another_thread();
        else if (scenario == "connector_callback") connector_callback_can_replace_itself();
        else if (scenario == "release") last_external_release_runs_on_loop();
        else if (scenario == "stale") stale_connector_events_do_not_cancel_new_attempt();
        else if (scenario == "publish") destruction_during_connection_construction_cancels_publication();
        else require(false, "unknown test scenario");
        return 0;
    }
    replacing_client_callback_allows_reentrant_release();
    connector_commands_preserve_submission_order();
    connector_restart_from_another_thread();
    connector_callback_can_replace_itself();
    last_external_release_runs_on_loop();
    stale_connector_events_do_not_cancel_new_attempt();
    destruction_during_connection_construction_cancels_publication();
    destroys_client_from_connection_callback();
    rejects_null_loop();
    connects_exchanges_messages_and_disconnects();
    stop_preserves_connection_and_disables_reconnect();
    reconnects_after_peer_close(false);
    reconnects_after_peer_close(true);
    retries_initial_connection_and_cancels_retry(false, false);
    retries_initial_connection_and_cancels_retry(true, false);
    retries_initial_connection_and_cancels_retry(true, true);
    destruction_preserves_external_connection();
    destruction_before_loop_handles_queued_work();
}
