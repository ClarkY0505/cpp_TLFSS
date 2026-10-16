#include "common/net/connector.h"
#include "common/net/channel.h"
#include "common/net/event_loop.h"
#include "common/net/net_logger.h"

#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <system_error>

namespace TLSS::NET {
namespace {
int socket_error(int sockfd) {
    int error = 0;
    socklen_t size = sizeof error;
    return ::getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 ? error : errno;
}

bool is_self_connection(int sockfd) {
    sockaddr_in local{};
    sockaddr_in peer{};
    socklen_t local_size = sizeof local;
    socklen_t peer_size = sizeof peer;
    return ::getsockname(sockfd, reinterpret_cast<sockaddr*>(&local), &local_size) == 0 &&
           ::getpeername(sockfd, reinterpret_cast<sockaddr*>(&peer), &peer_size) == 0 &&
           local.sin_port == peer.sin_port && local.sin_addr.s_addr == peer.sin_addr.s_addr;
}
}  // namespace

Connector::Connector(EventLoop* loop, const InetAddress& server_addr)
    : _loop(loop)
    , _server_addr(server_addr)
    , _connect(false)
    , _state(k_disconnected)
    , _retry_delay_ms(k_init_retry_delay_ms) {
    if (loop == nullptr) {
        throw std::invalid_argument("Connector loop cannot be null");
    }
}

Connector::~Connector() {
    // shared_ptr 只保证对象存活，不保证 loop 会执行任务。
    // 所有者须先 stop()，让 loop 完成 Channel 注销后再退出。
    assert(!_channel && !_retry_channel);
}

void Connector::start() {
    auto self = shared_from_this();
    // 即使调用者就是 loop 线程也排队，防止新命令越过其他线程已提交的 stop。
    _loop->queue_in_loop([self] {
        self->_connect = true;
        self->start_in_loop();
    });
}

void Connector::start_in_loop() {
    assert(_loop->is_in_loop_thread());
    // 重复 start 不重建 socket，也不绕过正在等待的退避定时器。
    if (_connect && _state == k_disconnected && !_retry_pending) {
        connect();
    }
}

void Connector::stop() {
    auto self = shared_from_this();
    // self 保证 stop_in_loop 执行前对象存活；Channel 清理还会继续持有 self。
    _loop->queue_in_loop([self] {
        self->_connect = false;
        self->stop_in_loop();
    });
}

void Connector::stop_in_loop() {
    assert(_loop->is_in_loop_thread());
    cancel_retry();
    if (_channel) {
        ::close(remove_and_reset_channel());
    }
    set_state(k_disconnected);
}

void Connector::restart() {
    auto self = shared_from_this();
    _loop->queue_in_loop([self] {
        self->stop_in_loop();
        self->_retry_delay_ms = k_init_retry_delay_ms;
        self->_connect = true;
        self->start_in_loop();
    });
}

void Connector::set_new_connection_cb(NewConnectionCb cb) {
    auto self = shared_from_this();
    _loop->queue_in_loop([self, cb = std::move(cb)]() mutable {
        // 回调读写统一在 loop，回调中替换自身也不会销毁正在执行的函数对象。
        self->_new_connection_cb = std::move(cb);
    });
}

void Connector::connect() {
    int sockfd = ::socket(_server_addr.family(), SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                          IPPROTO_TCP);
    if (sockfd < 0) {
        net_logger()->error("Connector socket failed: {}", errno);
        retry(-1);
        return;
    }
    const int result = ::connect(sockfd, _server_addr.get_sock_addr(), sizeof(sockaddr_in));
    const int error = result == 0 ? 0 : errno;
    switch (error) {
        case 0:
        case EINPROGRESS:
        case EINTR:
        case EISCONN:
            connecting(sockfd);
            break;
        case EAGAIN:
        case EADDRINUSE:
        case EADDRNOTAVAIL:
        case ECONNREFUSED:
        case ENETUNREACH:
        case EHOSTUNREACH:
        case ETIMEDOUT:
            retry(sockfd);
            break;
        default:
            net_logger()->error("Connector connect to {} failed: {}", _server_addr.to_ip_port(),
                                std::error_code(error, std::generic_category()).message());
            ::close(sockfd);
            break;
    }
}

void Connector::connecting(int sockfd) {
    assert(!_channel);
    set_state(k_connecting);
    _channel = std::make_unique<Channel>(_loop, sockfd);
    _channel->tie(shared_from_this());
    // 已注销的 Channel 仍可能携带本轮事件，只允许当前 Channel 驱动状态机。
    const Channel* channel = _channel.get();
    _channel->set_write_cb([this, channel] {
        if (_channel.get() == channel) handle_write();
    });
    _channel->set_error_cb([this, channel] {
        if (_channel.get() == channel) handle_error();
    });
    _channel->set_close_cb([this, channel] {
        if (_channel.get() == channel) handle_error();
    });
    _channel->enable_writing();
}

int Connector::remove_and_reset_channel() {
    _channel->disable_all();
    _channel->remove();
    const int sockfd = _channel->fd();
    // 旧 Channel 可能还在本轮 active_channels 中，延迟释放但立即允许重新连接。
    std::shared_ptr<Channel> retired(std::move(_channel));
    _loop->queue_in_loop([self = shared_from_this(), retired] {});
    return sockfd;
}

void Connector::handle_write() {
    if (_state != k_connecting) {
        return;
    }
    const int sockfd = remove_and_reset_channel();
    const int error = socket_error(sockfd);
    if (error != 0 || is_self_connection(sockfd)) {
        retry(sockfd);
        return;
    }
    if (_connect && _new_connection_cb) {
        set_state(k_connected);
        // 重试成功后立即注销已无用途的 timerfd/Channel，避免留到析构才发现。
        cancel_retry();
        _retry_delay_ms = k_init_retry_delay_ms;
        _new_connection_cb(sockfd);
    } else {
        set_state(k_disconnected);
        ::close(sockfd);
    }
}

void Connector::handle_error() {
    if (_state == k_connecting) {
        const int sockfd = remove_and_reset_channel();
        net_logger()->debug("Connector connection to {} failed: {}", _server_addr.to_ip_port(),
                            socket_error(sockfd));
        retry(sockfd);
    }
}

void Connector::retry(int sockfd) {
    if (sockfd >= 0) {
        ::close(sockfd);
    }
    set_state(k_disconnected);
    if (!_connect) {
        return;
    }
    if (_retry_fd < 0) {
        _retry_fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (_retry_fd < 0) {
            net_logger()->error("Connector timerfd_create failed: {}", errno);
            return;
        }
        _retry_channel = std::make_unique<Channel>(_loop, _retry_fd);
        _retry_channel->tie(shared_from_this());
        const Channel* channel = _retry_channel.get();
        _retry_channel->set_read_cb([this, channel](TIME::Timestamp) {
            if (_retry_channel.get() == channel) handle_retry();
        });
        _retry_channel->enable_reading();
    }
    itimerspec timer{};
    timer.it_value.tv_sec = _retry_delay_ms / 1000;
    timer.it_value.tv_nsec = (_retry_delay_ms % 1000) * 1000000L;
    if (::timerfd_settime(_retry_fd, 0, &timer, nullptr) < 0) {
        net_logger()->error("Connector timerfd_settime failed: {}", errno);
        cancel_retry();
        return;
    }
    _retry_pending = true;
    net_logger()->debug("Connector retry {} in {} ms", _server_addr.to_ip_port(), _retry_delay_ms);
    _retry_delay_ms = std::min(_retry_delay_ms * 2, k_max_retry_delay_ms);
}

void Connector::handle_retry() {
    std::uint64_t expirations = 0;
    const ssize_t bytes = ::read(_retry_fd, &expirations, sizeof expirations);
    if (bytes == static_cast<ssize_t>(sizeof expirations) && _retry_pending) {
        _retry_pending = false;
        start_in_loop();
    }
}

void Connector::cancel_retry() {
    _retry_pending = false;
    if (_retry_channel) {
        _retry_channel->disable_all();
        _retry_channel->remove();
        std::shared_ptr<Channel> retired(std::move(_retry_channel));
        _loop->queue_in_loop([self = shared_from_this(), retired] {});
        ::close(_retry_fd);
        _retry_fd = -1;
    }
}
}  // namespace TLSS::NET
