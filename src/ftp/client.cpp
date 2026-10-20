#include "ftp/client.h"

#include "common/net/buffer.h"
#include "common/net/channel.h"
#include "common/net/event_loop.h"
#include "common/net/net_logger.h"
#include "common/net/tcp_connection.h"
#include "monitor/client.h"

#include <bits/types/sigset_t.h>
#include <pthread.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>
namespace TLSS::FileStorage {
namespace {
constexpr std::size_t k_max_command_bytes = 4096;
constexpr int k_shutdown_timeout_sec = 2;

void remove_channel(std::unique_ptr<NET::Channel>& channel) {
  if (channel && channel->index() != -1) {
    channel->disable_all();
    channel->remove();
  }
}
}  // namespace

Client::Client(NET::EventLoop* loop, const NET::InetAddress& server_addr)
    : _loop(loop)
    , _client(loop, server_addr, "tlss_cli") {
  if (!loop || !loop->is_in_loop_thread()) {
    NET::net_logger()->error("Client must be created in its EventLoop thread");
  }
  _client.set_connection_cb(std::bind(&Client::on_connection, this, std::placeholders::_1));
  _client.set_message_cb(std::bind(&Client::on_message, this, std::placeholders::_1,
                                   std::placeholders::_2, std::placeholders::_3));
}

Client::~Client() {
  // start() and the EventLoop must have finished before destruction.
  remove_channel(_input_channel);
  remove_channel(_signal_channel);
  remove_channel(_timer_channel);
  if (_signal_fd >= 0) {
    ::close(_signal_fd);
  }
  if (_timer_fd >= 0) {
    ::close(_timer_fd);
  }
  if (_mask_saved) {
    (void)::pthread_sigmask(SIG_SETMASK, &_old_mask, nullptr);
  }
}

void Client::start() {
  if (_started) {
    return;
  }

  _started = true;
  if (!::isatty(STDIN_FILENO)) {
    NET::net_logger()->error("tlss-cli requires an interactive terminal");
  }

  sigset_t mask{};
  ::sigemptyset(&mask);
  ::sigaddset(&mask, SIGINT);
  ::sigaddset(&mask, SIGTERM);

  const int err = ::pthread_sigmask(SIG_BLOCK, &mask, &_old_mask);
  if (err != 0) {
    NET::net_logger()->error("client pthread_sigmask");
  }
  _mask_saved = true;

  _signal_fd = ::signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
  if (_signal_fd < 0) {
    NET::net_logger()->error("client signalfd");
  }

  _timer_fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  if (_timer_fd < 0) {
    NET::net_logger()->error("client timerfd_create");
  }

  _input_channel = std::make_unique<NET::Channel>(_loop, STDIN_FILENO);
  _input_channel->set_read_cb(std::bind(&Client::on_input, this));
  _input_channel->enable_reading();

  _signal_channel = std::make_unique<NET::Channel>(_loop, _signal_fd);
  _signal_channel->set_read_cb(std::bind(&Client::on_signal, this));
  _signal_channel->enable_reading();

  _timer_channel = std::make_unique<NET::Channel>(_loop, _timer_fd);
  _timer_channel->set_read_cb(std::bind(&Client::on_timeout, this));
  _timer_channel->enable_reading();

  _client.enable_retry();
  _client.connect();
  // connect() queues work before loop() starts. Wake its first poll so
  // connecting does not wait for terminal input or the poll timeout.
  _loop->wakeup();
  std::cout << "Connecting... (exit to quit)\n" << std::flush;
}

void Client::on_connection(const NET::TcpConnectionPtr& conn) {
  if (conn->connected()) {
    _ever_connected = true;
    std::cout << "\n[connected] " << conn->peer_addr().to_ip_port() << '\n';
    return;
  }
  std::cout << "\n[disconnected]\n" << std::flush;
  if (_stopping) {
    finish_exit();
  } else if (_ever_connected) {
    // Do not reconnect after a session is established and then lost.
    request_exit();
  }
}

void Client::on_message(const NET::TcpConnectionPtr&, NET::Buffer* buffer, TIME::Timestamp) {
  std::cout << buffer->retrieve_all_as_string() << std::flush;
}

void Client::on_input() {
  char buf[1024];
  const ssize_t count = ::read(STDIN_FILENO, buf, sizeof buf);
  if (count == 0) {
    request_exit();
  }
  if (count < 0) {
    if (errno != EINTR && errno != EAGAIN) {
      request_exit();
    }
    return;
  }

  for (ssize_t i = 0; i < count && !_stopping; ++i) {
    const char ch = buf[i];
    if (ch == '\n') {
      if (_drop_line) {
        _drop_line = false;
        _input.clear();
        continue;
      }
      if (!_input.empty() && _input.back() == '\r') {
        _input.pop_back();
      }

      std::string command = std::move(_input);
      _input.clear();
      handle_command(command);
    } else if (!_drop_line) {
      if (_input.size() >= k_max_command_bytes) {
        _input.clear();
        _drop_line = true;
        std::cout << "[command too long]\n";
      } else {
        _input.push_back(ch);
      }
    }
  }
}

void Client::handle_command(std::string command) {
  if (command == "exit" || command == "q" || command == "quit") {
    request_exit();
    return;
  }
  auto conn = _client.connection();
  if (!conn || !conn->connected()) {
    std::cout << "[not connected]\n";
    return;
  }
  conn->send(std::move(command) + "\n");
}

void Client::on_signal() {
  signalfd_siginfo info{};
  const ssize_t n = ::read(_signal_fd, &info, sizeof info);
  if (n == static_cast<ssize_t>(sizeof info)) {
    request_exit();
  }
}

void Client::on_timeout() {
  std::uint64_t expiration = 0;
  ::read(_timer_fd, &expiration, sizeof expiration);
  if (auto conn = _client.connection()) {
    conn->force_close();
  }
  if (!_client.connection()) {
    finish_exit();
  }
}

void Client::request_exit() {
  if (_stopping) {
    return;
  }
  _stopping = true;
  remove_channel(_input_channel);
  _client.stop();  // Prevent new connections or retries.

  auto conn = _client.connection();
  if (!conn || !conn->connected()) {
    finish_exit();
    return;
  }

  // Give the server a chance to send "bye!" before forcing cleanup.
  conn->send("quit\n");
  conn->shutdow();  // Half-close after pending writes are flushed.
  itimerspec timer{};
  timer.it_value.tv_sec = k_shutdown_timeout_sec;
  ::timerfd_settime(_timer_fd, 0, &timer, nullptr);
}

void Client::finish_exit() {
  if (_finishing) {
    return;
  }
  _finishing = true;
  _client.stop();
  remove_channel(_input_channel);
  remove_channel(_signal_channel);
  remove_channel(_timer_channel);

  // TcpClient/Connector defer Channel removal through the EventLoop queue.
  // Let these cleanup tasks run before quitting the loop.
  auto* loop = _loop;
  loop->queue_in_loop([loop] {
    loop->queue_in_loop([loop] {
      loop->queue_in_loop([loop] {
        loop->quit();
      });
    });
  });
}

}  // namespace TLSS::FileStorage
