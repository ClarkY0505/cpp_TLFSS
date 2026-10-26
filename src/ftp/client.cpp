#include "common/net/buffer.h"
#include "common/net/channel.h"
#include "common/net/event_loop.h"
#include "common/net/net_logger.h"
#include "common/net/tcp_connection.h"
#include "ftp/client.h"
#include "ftp/file_transfer.h"

#include <bits/types/sigset_t.h>
#include <pthread.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <string_view>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
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
  _loop->wakeup();
  std::cout << "Connecting... (exit to quit)\n" << std::flush;
}

void Client::on_connection(const NET::TcpConnectionPtr& conn) {
  if (conn->connected()) {
    _ever_connected = true;
    _authenticated = false;

    std::cout << "\n[connected] " << conn->peer_addr().to_ip_port() << '\n';
    return;
  }

  _authenticated = false;
  reset_transfer();
  std::cout << "\n[disconnected]\n" << std::flush;
  if (_stopping) {
    finish_exit();
  } else if (_ever_connected) {
    // Do not reconnect after a session is established and then lost.
    request_exit();
  }
}

void Client::on_message(const NET::TcpConnectionPtr& conn, NET::Buffer* buffer,
                        TIME::Timestamp timestamp) {
  (void)timestamp;
  try {
    const std::string_view prefix = Transfer::kResponsePrefix;
    while (buffer->readable_bytes() != 0) {
      if (_transfer_state == FileTransferState::receiving_download) {
        _download_receiver->consume(*buffer);
        if (_download_receiver->remaining() == 0) {
          _transfer_state = FileTransferState::waiting_download_done;
        }
        continue;
      }

      const std::string_view bytes(buffer->peek(), buffer->readable_bytes());
      const auto position = bytes.find(prefix);

      if (position == std::string_view::npos) {
        // 保留可能属于跨包控制前缀的末尾部分。
        std::size_t keep = std::min(bytes.size(), prefix.size() - 1);
        while (keep != 0 && bytes.substr(bytes.size() - keep) != prefix.substr(0, keep)) {
          --keep;
        }
        const std::size_t printable = bytes.size() - keep;

        if (printable != 0) {
          std::cout.write(buffer->peek(), static_cast<std::streamsize>(printable));
          std::cout.flush();
          buffer->retrieve(printable);
        }

        return;
      }

      if (position != 0) {
        std::cout.write(buffer->peek(), static_cast<std::streamsize>(position));
        std::cout.flush();

        buffer->retrieve(position);
        continue;
      }

      const auto end = bytes.find('\n', prefix.size());

      if (end == std::string_view::npos) {
        if (bytes.size() > Transfer::kMaxControlBytes) {
          throw std::runtime_error("server control response too long");
        }
        return;
      }

      if (end > Transfer::kMaxControlBytes) {
        throw std::runtime_error("server control response too long");
      }

      const std::string line(bytes.substr(prefix.size(), end - prefix.size()));

      buffer->retrieve(end + 1);
      handle_transfer_response(conn, line);
    }
  } catch (const std::exception& exception) {
    std::cout << "\n[transfer failed: " << exception.what() << "]\n" << std::flush;
    reset_transfer();
    conn->force_close();
  }
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
  if (_transfer_state != FileTransferState::idle) {
    std::cout << "[transfer in progress; exit cancels it]\n";
    return;
  }

  auto conn = _client.connection();
  if (!conn || !conn->connected()) {
    std::cout << "[not connected]\n";
    return;
  }

  std::istringstream input{command};
  std::string verb;
  std::string args;

  input >> verb;
  std::getline(input >> std::ws, args);
  if (verb == "upload" || verb == "download") {
    if (!_authenticated) {
      std::cout << "[please log in first]\n";
      return;
    }

    if (verb == "upload") {
      upload(args);
    } else {
      download(args);
    }
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
    reset_transfer();
    finish_exit();
    return;
  }

  if (_transfer_state != FileTransferState::idle) {
    // 不能把 quit 文本插入正在传输的协议流程。
    reset_transfer();
    conn->force_close();
  } else {
    conn->send("quit\n");
    conn->shutdow();
  }

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

void Client::reset_transfer() {
  _upload_source.reset();
  _download_receiver.reset();

  _download_destination.clear();
  _download_hash.clear();

  _transfer_state = FileTransferState::idle;
}

void Client::upload(const std::string& args) {
  // FIXME 在修改了file_transfer中的数据结构后，不再抛出异常
  try {
    std::istringstream input(args);
    const std::string local = Transfer::read_field(input);

    input >> std::ws;
    std::string remote;
    if (input.eof()) {
      remote = std::filesystem::path(local).filename().string();
    } else {
      remote = Transfer::read_field(input);
      Transfer::require_end(input);
    }

    if (local.find('\0') != std::string::npos || !Transfer::valid_name(remote)) {
      throw std::runtime_error("invalid filename");
    }

    auto source = Transfer::open_source(local);

    auto conn = _client.connection();
    if (!conn || !conn->connected()) {
      throw std::runtime_error("not connected");
    }

    _upload_source = std::move(source);
    _transfer_state = FileTransferState::waiting_upload_ready;

    conn->send("@TLSS UPLOAD " + std::to_string(_upload_source->size) + " " +
               Transfer::quote(remote) + "\n");

  } catch (const std::exception& exception) {
    reset_transfer();
    std::cout << "[upload failed: " << exception.what() << "]\n";
  }
}

void Client::download(const std::string& args) {
  try {
    std::istringstream input(args);
    const std::string remote = Transfer::read_field(input);

    if (!Transfer::valid_name(remote)) {
      throw std::runtime_error("invalid remote filename");
    }

    input >> std::ws;

    std::string local;
    if (input.eof()) {
      local = remote;
    } else {
      local = Transfer::read_field(input);
      Transfer::require_end(input);
    }

    if (local.empty() || local.find('\0') != std::string::npos) {
      throw std::runtime_error("invalid local path");
    }

    const auto destination = std::filesystem::absolute(local);

    if (std::filesystem::exists(destination)) {
      throw std::runtime_error("local destination already exists");
    }

    if (!std::filesystem::is_directory(destination.parent_path())) {
      throw std::runtime_error("local destination directory does not exist");
    }

    auto conn = _client.connection();
    if (!conn || !conn->connected()) {
      throw std::runtime_error("not connected");
    }

    _download_destination = destination;
    _transfer_state = FileTransferState::waiting_download_header;

    conn->send("@TLSS DOWNLOAD " + Transfer::quote(remote) + "\n");
  } catch (const std::exception& exception) {
    reset_transfer();
    std::cout << "[download failed: " << exception.what() << "]\n";
  }
}

void Client::handle_transfer_response(const NET::TcpConnectionPtr& conn, const std::string& line) {
  std::istringstream input(line);
  std::string verb;
  input >> verb;

  if (verb == "AUTH_OK") {
    Transfer::require_end(input);
    _authenticated = true;
    return;
  }

  if (verb == "ERROR") {
    const std::string error = Transfer::read_field(input);
    Transfer::require_end(input);

    reset_transfer();

    std::cout << "\n[transfer failed: " << error << "]\n" << std::flush;
    return;
  }

  if (verb == "UREADY" && _transfer_state == FileTransferState::waiting_upload_ready) {
    Transfer::require_end(input);

    if (!_upload_source) {
      throw std::runtime_error("upload source is missing");
    }

    const std::shared_ptr<Transfer::SourceFile> source = _upload_source;
    auto weak_conn = std::weak_ptr<NET::TcpConnection>(conn);

    _transfer_state = FileTransferState::waiting_upload_result;
    NET::TcpConnection::FileSendCallback cb = [weak_conn](std::error_code error, uint64_t sent) {
      if (error) {
        std::cout << "\n[send failed after " << sent << " bytes: " << error.message() << "]\n"
                  << std::flush;

        if (auto conn = weak_conn.lock()) {
          conn->force_close();
        }
        return;
      }

      std::cout << "\n[sent " << sent << " bytes; waiting for server confirmation]\n" << std::flush;
    };
    conn->send_file(source->file.fd, 0, source->size, cb);

    _upload_source.reset();
    return;
  }

  if (verb == "UOK" && _transfer_state == FileTransferState::waiting_upload_result) {
    std::string hash;
    input >> hash;
    Transfer::require_end(input);
    if (!Transfer::valid_hash(hash)) {
      throw std::runtime_error("invalid upload hash");
    }

    reset_transfer();
    std::cout << "\n[upload complete; SHA-256 " << hash << "]\n" << std::flush;
    return;
  }

  if (verb == "DATA" && _transfer_state == FileTransferState::waiting_download_header) {
    std::string size_text;
    std::string hash;
    if (!(input >> size_text >> hash)) {
      throw std::runtime_error("invalid download header");
    }

    Transfer::require_end(input);
    const uint64_t size = Transfer::parse_size(size_text);

    if (!Transfer::valid_hash(hash)) {
      throw std::runtime_error("invalid download hash");
    }

    _download_hash = hash;

    _download_receiver =
        std::make_shared<Transfer::Receiver>(_download_destination.parent_path(), size);

    _transfer_state = size == 0 ? FileTransferState::waiting_download_done
                                : FileTransferState::receiving_download;

    return;
  }

  if (verb == "DONE" && _transfer_state == FileTransferState::waiting_download_done) {
    Transfer::require_end(input);

    if (!_download_receiver || _download_receiver->remaining() != 0) {
      throw std::runtime_error("download is incomplete");
    }

    // 校验、同步并发布本地文件，不覆盖已有目标。
    _download_receiver->publish(_download_destination, false, _download_hash, 0600);
    _download_receiver.reset();
    _transfer_state = FileTransferState::waiting_download_ok;

    // 只有正文完整、校验通过并收到 DONE 后才确认。
    conn->send("@TLSS ACK\n");
    return;
  }

  if (verb == "OK" && _transfer_state == FileTransferState::waiting_download_ok) {
    Transfer::require_end(input);

    const auto destination = _download_destination;
    reset_transfer();

    std::cout << "\n[download complete: " << destination.string() << "]\n" << std::flush;
    return;
  }

  throw std::runtime_error("unexpected server transfer response");
}
}  // namespace TLSS::FileStorage
