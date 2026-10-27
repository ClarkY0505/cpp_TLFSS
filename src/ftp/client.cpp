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
  _disk_lifetime.reset();
  reset_transfer();
  _disk_executor.stop();
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
  // 工作线程继承当前线程的信号屏蔽状态。
  _disk_executor.start(2);

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
      if (_transfer_state == FileTransferState::preparing_download ||
          _transfer_state == FileTransferState::finalizing_download) {
        // 文件初始化或发布期间，保留已收到的内容。
        return;
      }

      if (_transfer_state == FileTransferState::receiving_download) {
        if (!_download_writer) {
          throw std::runtime_error("download receiver is missing");
        }

        _download_writer->feed(*buffer);

        if (_download_writer->remaining() == 0) {
          _transfer_state = FileTransferState::waiting_download_done;
        }

        // 等本次磁盘任务完成后再处理 Buffer 剩余内容。
        return;
      }

      const std::string_view bytes(buffer->peek(), buffer->readable_bytes());

      const auto position = bytes.find(prefix);

      if (position == std::string_view::npos) {
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

  reset_transfer();

  // EventLoop 中只发起取消，不等待磁盘线程结束。
  _disk_executor.begin_shutdown();

  _client.stop();

  remove_channel(_input_channel);
  remove_channel(_signal_channel);
  remove_channel(_timer_channel);

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
  if (_download_writer) {
    _download_writer->cancel();
    _download_writer.reset();
  }

  if (_upload_source) {
    _upload_source->cancel();
    _upload_source.reset();
  }

  _download_destination.clear();
  _download_hash.clear();

  _transfer_state = FileTransferState::idle;
}

void Client::upload(const std::string& args) {
  try {
    std::istringstream input(args);
    const auto local = Transfer::read_field(input);
    input >> std::ws;
    std::string remote;
    if (input.eof()) {
      remote = std::filesystem::path(local).filename().string();
    } else {
      remote = Transfer::read_field(input);
      Transfer::require_end(input);
    }
    if (local.empty() || local.find('\0') != std::string::npos || !Transfer::valid_name(remote)) {
      throw std::runtime_error("invalid filename");
    }
    auto conn = _client.connection();
    if (!conn || !conn->connected()) throw std::runtime_error("not connected");
    _transfer_state = FileTransferState::preparing_upload_source;
    auto lifetime = std::weak_ptr<int>(_disk_lifetime);
    auto weak_conn = std::weak_ptr<NET::TcpConnection>(conn);
    auto factory = [local]() -> Transfer::AsyncFileSource::Opened {
      return {Transfer::open_source(local), {}};
    };
    auto ready = [this, lifetime, weak_conn, remote](const Transfer::AsyncFileSource::Info& info) {
      if (lifetime.expired()) return;
      auto conn = weak_conn.lock();
      if (!conn || !conn->connected() || _stopping) return;
      if (_transfer_state != FileTransferState::preparing_upload_source) return;
      _transfer_state = FileTransferState::waiting_upload_ready;
      conn->send("@TLSS UPLOAD " + std::to_string(info.size) + " " + Transfer::quote(remote) + "\n");
    };
    auto failed = [this, lifetime, weak_conn](std::string error) {
      if (lifetime.expired()) return;
      const bool requested = _transfer_state != FileTransferState::preparing_upload_source;
      reset_transfer();
      std::cout << "[upload failed: " << error << "]\n" << std::flush;
      if (requested) {
        if (auto conn = weak_conn.lock()) conn->force_close();
      }
    };
    _upload_source = Transfer::AsyncFileSource::open(
        _disk_executor, conn, std::move(factory), std::move(ready), std::move(failed));
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

    auto conn = _client.connection();

    if (!conn || !conn->connected()) {
      throw std::runtime_error("not connected");
    }

    // 此处只构造 path，不访问磁盘。
    _download_destination = std::filesystem::path(local);

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
    if (!_upload_source) throw std::runtime_error("upload source is missing");
    const auto source = _upload_source;
    auto weak_conn = std::weak_ptr<NET::TcpConnection>(conn);
    _transfer_state = FileTransferState::waiting_upload_result;
    NET::TcpConnection::FileSendCallback completed =
        [source, weak_conn](std::error_code error, std::uint64_t sent) {
      source->cancel();
      if (error) {
        std::cout << "\n[send failed after " << sent << " bytes: " << error.message() << "]\n"
                  << std::flush;
        if (auto conn = weak_conn.lock()) conn->force_close();
        return;
      }
      std::cout << "\n[sent " << sent << " bytes; waiting for server confirmation]\n" << std::flush;
    };
    conn->send_file_async(0, source->size(), source->reader(), std::move(completed));
    // Keep the source until UOK/error so reset_transfer can cancel immediately.
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

    const std::uint64_t size = Transfer::parse_size(size_text);

    if (!Transfer::valid_hash(hash)) {
      throw std::runtime_error("invalid download hash");
    }

    _download_hash = hash;
    _transfer_state = FileTransferState::preparing_download;

    struct LocalTarget {
      std::filesystem::path destination;
    };

    auto target = std::make_shared<LocalTarget>();

    const auto requested_path = _download_destination;

    auto lifetime = std::weak_ptr<int>(_disk_lifetime);
    auto weak_conn = std::weak_ptr<NET::TcpConnection>(conn);

    auto factory = [requested_path, target, size] {
      // 所有访问文件系统的操作都在工作线程执行。
      target->destination = std::filesystem::absolute(requested_path);

      if (std::filesystem::exists(target->destination)) {
        throw std::runtime_error("local destination already exists");
      }

      if (!std::filesystem::is_directory(target->destination.parent_path())) {
        throw std::runtime_error("local destination directory does not exist");
      }

      return std::make_unique<Transfer::Receiver>(target->destination.parent_path(), size);
    };

    auto finalizer = [target, hash](Transfer::Receiver& receiver) {
      receiver.publish(target->destination, false, hash, 0600);

      return target->destination.string();
    };

    auto ready = [this, lifetime, weak_conn, size] {
      if (lifetime.expired()) {
        return;
      }

      auto conn = weak_conn.lock();

      if (!conn || !conn->connected() || _stopping) {
        return;
      }

      _transfer_state = size == 0 ? FileTransferState::waiting_download_done
                                  : FileTransferState::receiving_download;
    };

    auto done = [this, lifetime, weak_conn](std::string saved_path) {
      if (lifetime.expired()) {
        return;
      }

      auto conn = weak_conn.lock();

      if (!conn || !conn->connected() || _stopping) {
        return;
      }

      _download_destination = std::filesystem::path(std::move(saved_path));

      _download_writer.reset();

      _transfer_state = FileTransferState::waiting_download_ok;

      // 文件已完整写入、校验、同步并发布之后才确认。
      conn->send("@TLSS ACK\n");
    };

    auto failed = [this, lifetime, weak_conn](std::string error) {
      if (lifetime.expired()) {
        return;
      }

      std::cout << "\n[download failed: " << error << "]\n" << std::flush;

      reset_transfer();

      if (auto conn = weak_conn.lock()) {
        conn->force_close();
      }
    };

    _download_writer = Transfer::AsyncReceiver::create(
        _disk_executor, conn, size, std::move(factory), std::move(finalizer), std::move(ready),
        std::move(done), std::move(failed));

    return;
  }

  if (verb == "DONE" && _transfer_state == FileTransferState::waiting_download_done) {
    Transfer::require_end(input);

    if (!_download_writer || _download_writer->remaining() != 0) {
      throw std::runtime_error("download is incomplete");
    }

    _transfer_state = FileTransferState::finalizing_download;

    // 这里只提交工作线程任务。
    // 校验、fsync 和 publish 不在 EventLoop 执行。
    _download_writer->finish();

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
