#include "ftp/server.h"
#include "ftp/client.h"
#include "ftp/async_file_source.h"
#include "common/base/mysql_connection.h"
#include "common/net/event_loop.h"

#include <sys/socket.h>
#include <sys/sendfile.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <future>
#include <fstream>
#include <iostream>
#include <thread>

// Deterministically model a cold-file wait without changing system page caches.
namespace {
struct ColdIoGate {
  std::mutex mutex;
  std::condition_variable changed;
  bool enabled = false;
  bool entered = false;
  bool released = false;
  bool heartbeat = false;
  std::atomic<std::size_t> largest_read{0};

  void wait(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex);
    if (!enabled) return;
    const auto largest = largest_read.load();
    if (count > largest) largest_read.store(count);
    entered = true;
    changed.notify_all();
    changed.wait(lock, [&] { return released; });
  }
} cold_io;
}

extern "C" ssize_t sendfile(int out, int in, off_t* offset, size_t count) noexcept {
  cold_io.wait(count);
  return static_cast<ssize_t>(::syscall(SYS_sendfile, out, in, offset, count));
}

extern "C" ssize_t pread(int fd, void* data, size_t count, off_t offset) {
  cold_io.wait(count);
  return static_cast<ssize_t>(::syscall(SYS_pread64, fd, data, count, offset));
}

// Database and authentication substitutes keep these regression checks local. / 使用数据库和认证替代实现，让这些回归检查在本地独立运行。
// The actual FTP handlers, file receiver, client parser and storage engine run. / 运行实际的 FTP 处理函数、文件接收器、客户端解析器和存储引擎。
namespace {
std::atomic<int> inserts{0};
std::atomic<bool> database_shutdown_called{false};
std::atomic<bool> finalizer_active{false};
std::atomic<bool> shutdown_during_finalize{false};
const std::string abc_hash =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
}

namespace TLSS::BASE {
DbConnectionPool::DbConnectionPool()
    : _produce_thread([] {}), _scanner_thread([] {}),
      _connection_cnt(0), _stopping(false), _threads_joined(true),
      _borrowed_count(0), _active_getters(0) {}
DbConnectionPool::~DbConnectionPool() = default;
DbConnectionPool* DbConnectionPool::get_connection_pool() {
  static DbConnectionPool pool;
  return &pool;
}
DbConnectionHandle DbConnectionPool::get_connection() {
  return DbConnectionHandle(new DbConnection, DbConnectionDeleter{});
}
bool DbConnectionPool::shutdown(std::chrono::milliseconds) {
  if (finalizer_active.load()) shutdown_during_finalize.store(true);
  database_shutdown_called.store(true);
  return true;
}
void DbConnectionDeleter::operator()(DbConnection* connection) const noexcept {
  delete connection;
}
DbConnection::DbConnection() : _conn(nullptr) {}
DbConnection::~DbConnection() = default;
DbQueryResult DbConnection::query_prepared(
    const std::string& sql, const std::vector<DbValue>&) {
  if (sql.find("f.object_hash") != std::string::npos) {
    return {true, {{abc_hash}}};
  }
  return {true, {{std::string("42")}}};
}
bool DbConnection::update_prepared(
    const std::string&, const std::vector<DbValue>&) { return true; }
DbExecuteResult DbConnection::execute_prepared(
    const std::string&, const std::vector<DbValue>&) {
  ++inserts;
  return {true, 0, 1, 1};
}
}

namespace TLSS::AUTH {
MySqlUserRepository::MySqlUserRepository(BASE::DbConnectionPool&) {}
UserLookupResult MySqlUserRepository::find_active_user(const std::string&) {
  return {};
}
UserCreateResult MySqlUserRepository::create_user_with_root(
    const std::string&, const PasswordCredentials&) { return {}; }
AuthService::AuthService(UserRepository& users) : users_(users) {}
AuthResult AuthService::authenticate(const LoginRequest&) const { return {}; }
SignupResult AuthService::signup(const SignupRequest&) const { return {}; }
bool AuthService::valid_signup_username(std::string_view) noexcept { return true; }
bool AuthService::valid_signup_password(std::string_view) noexcept { return true; }
}

namespace {
using namespace TLSS;
using namespace TLSS::NET;
using namespace TLSS::FileStorage;

struct TemporaryDirectory {
  std::string path;
  TemporaryDirectory() {
    char pattern[] = "/tmp/tlss-ftp-regression-XXXXXX";
    char* created = ::mkdtemp(pattern);
    check(created != nullptr, "mkdtemp failed");
    path = created;
    check(::setenv("TLSS_STORAGE_ROOT", path.c_str(), 1) == 0,
          "setenv failed");
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

struct Fixture {
  EventLoop loop;
  StorageService service;
  TcpConnectionPtr conn;
  std::shared_ptr<ConnectionSession> session;
  int peer = -1;

  Fixture() : service(&loop, InetAddress(0), "regression", 0, 0, 32) {
    service._th_pool.start(0);
    service._disk_executor.start(2);
    int sockets[2];
    check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                       0, sockets) == 0, "socketpair failed");
    peer = sockets[1];
    conn = std::make_shared<TcpConnection>(
        &loop, "test", sockets[0], InetAddress(), InetAddress());
    conn->set_connection_cb([this](const TcpConnectionPtr& connection) {
      service.on_connection(connection);
    });
    conn->set_close_cb([this](const TcpConnectionPtr& connection) {
      loop.queue_in_loop([connection] { connection->connect_destroyed(); });
    });
    conn->set_message_cb([this](const TcpConnectionPtr& connection, Buffer* buffer,
                                TIME::Timestamp time) {
      service.on_message(connection, buffer, time);
    });
    conn->connect_established();
    session = service._sessions.at(conn.get());
    session->login_state = LoginState::k_authenticated;
    session->name = "test_user";
    session->user_id = 1;
    drain();
    read_wire();
  }

  ~Fixture() {
    service._disk_executor.stop();
    conn->set_connection_cb({});
    conn->set_message_cb({});
    conn->set_close_cb({});
    conn->connect_destroyed();
    conn.reset();
    service._sessions.clear();
    if (peer >= 0) ::close(peer);
  }

  void drain() {
    // Run one poll/dispatch cycle; completion is determined by until(), not a delay.
    loop.queue_in_loop([this] { loop.quit(); });
    loop.wakeup();
    loop.loop();
  }

  template <typename Predicate>
  void until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
      check(std::chrono::steady_clock::now() < deadline, "asynchronous operation timed out");
      drain();
      std::this_thread::yield();
    }
  }

  std::string await_wire(const std::string& marker) {
    std::string response;
    until([&] {
      response += read_wire();
      return response.find(marker) != std::string::npos;
    });
    return response;
  }

  std::string read_wire() {
    std::string result;
    char data[8192];
    for (;;) {
      const ssize_t n = ::recv(peer, data, sizeof data, 0);
      if (n > 0) result.append(data, static_cast<std::size_t>(n));
      else if (n == 0 || errno == EAGAIN || errno == EWOULDBLOCK) return result;
      else if (errno != EINTR) throw std::runtime_error("recv failed");
    }
  }

  void receive(const std::string& text) {
    // Exercise TcpConnection's real input Buffer and replay path.
    std::size_t offset = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (offset < text.size()) {
      const ssize_t n = ::send(peer, text.data() + offset, text.size() - offset, MSG_NOSIGNAL);
      if (n > 0) {
        offset += static_cast<std::size_t>(n);
      } else if (n == -1 && errno == EINTR) {
        continue;
      } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        check(std::chrono::steady_clock::now() < deadline, "sending test input timed out");
        drain();
      } else {
        throw std::runtime_error("cannot send test input");
      }
    }
  }

};

struct ColdController {
  std::thread thread;
  bool responsive = false;

  ColdController(Fixture& fixture, bool cancel) {
    {
      std::lock_guard<std::mutex> lock(cold_io.mutex);
      cold_io.enabled = true;
      cold_io.entered = false;
      cold_io.released = false;
      cold_io.heartbeat = false;
      cold_io.largest_read.store(0);
    }
    thread = std::thread([this, &fixture, cancel] {
      std::unique_lock<std::mutex> lock(cold_io.mutex);
      const bool entered = cold_io.changed.wait_for(lock, std::chrono::seconds(5), [] {
        return cold_io.entered;
      });
      lock.unlock();
      if (entered) {
        if (cancel) {
          fixture.loop.queue_in_loop([&fixture] {
            fixture.service._disk_executor.begin_shutdown();
          });
        } else {
          // This executes while the async file task has EPOLLOUT disabled.
          fixture.conn->shutdow();
        }
        fixture.loop.queue_in_loop([] {
          std::lock_guard<std::mutex> lock(cold_io.mutex);
          cold_io.heartbeat = true;
          cold_io.changed.notify_all();
        });
      }
      lock.lock();
      responsive = entered && cold_io.changed.wait_for(lock, std::chrono::seconds(1), [] {
        return cold_io.heartbeat;
      });
      cold_io.released = true;
      lock.unlock();
      cold_io.changed.notify_all();
    });
  }

  ~ColdController() {
    {
      std::lock_guard<std::mutex> lock(cold_io.mutex);
      cold_io.released = true;
      cold_io.changed.notify_all();
    }
    if (thread.joinable()) thread.join();
    std::lock_guard<std::mutex> lock(cold_io.mutex);
    cold_io.enabled = false;
  }

  void join() {
    if (thread.joinable()) thread.join();
  }
};

void check_cold_send(Fixture& fixture, const std::string& directory, bool cancel) {
  const std::string path = directory + "/cold.bin";
  std::string payload(3 * 65536 + 317, '\0');
  for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(i % 251);
  std::ofstream(path, std::ios::binary).write(payload.data(), payload.size());
  int small = 4096;
  check(::setsockopt(fixture.conn->_channel->fd(), SOL_SOCKET, SO_SNDBUF, &small, sizeof small) == 0,
        "setsockopt failed");
  ColdController controller(fixture, cancel);
  int callbacks = 0;
  std::error_code completion_error;
  std::uint64_t sent = 0;
  auto completed = [&](std::error_code error, std::uint64_t bytes) {
    ++callbacks;
    completion_error = error;
    sent = bytes;
  };
  const std::string header = "--ASYNC-HEAD--\n";
  const std::string trailer = "\n--ASYNC-TAIL--\n";
  std::shared_ptr<Transfer::AsyncFileSource> source;
  std::atomic<bool> opened_in_worker{false};
  source = Transfer::AsyncFileSource::open(
      fixture.service._disk_executor, fixture.conn,
      [&]() -> Transfer::AsyncFileSource::Opened {
        opened_in_worker.store(!fixture.loop.is_in_loop_thread());
        return {Transfer::open_source(path), {}};
      },
      [&](const Transfer::AsyncFileSource::Info& info) {
        if (!cancel) fixture.conn->send(header);
        fixture.conn->send_file_async(0, info.size, source->reader(),
            [&](std::error_code error, std::uint64_t bytes) {
              source->cancel();
              completed(error, bytes);
            });
        if (!cancel) fixture.conn->send(trailer);
      },
      [](std::string error) { throw std::runtime_error(error); });
  std::string wire;
  if (cancel) {
    fixture.until([&] {
      wire += fixture.read_wire();
      return fixture.conn->disconnected() && callbacks == 1;
    });
  } else {
    fixture.until([&] {
      wire += fixture.read_wire();
      return wire.find(trailer) != std::string::npos && callbacks == 1;
    });
  }
  controller.join();
  check(controller.responsive, "cold file I/O blocked the EventLoop heartbeat");
  check(callbacks == 1, "file completion was not delivered exactly once");
  check(opened_in_worker.load(), "source was opened in the EventLoop");
  check(cold_io.largest_read.load() <= TcpConnection::kAsyncFileChunk,
        "file read exceeded the bounded chunk size");
  check(fixture.conn->_pending_memory_bytes == 0 && fixture.conn->_pending_files == 0,
        "file send leaked queue accounting");
  if (cancel) {
    check(completion_error == std::errc::operation_canceled,
          "executor cancellation did not fail the waiting send");
    check(sent == 0 && wire.empty(), "cancelled file data was sent");
  } else {
    check(!completion_error && sent == payload.size(), "wrong successful file completion");
    check(wire == header + payload + trailer, "header/body/trailer was interleaved or truncated");
    char byte;
    check(::recv(fixture.peer, &byte, 1, 0) == 0, "FIN did not follow all queued bytes");
  }
}

void check_stale_read(Fixture& fixture) {
  std::vector<TcpConnection::FileReadCompletion> reads;
  auto reader = [&](off_t, std::size_t, TcpConnection::FileReadCompletion done) {
    reads.push_back(std::move(done));
  };
  int completed = 0;
  auto done = [&](std::error_code error, std::uint64_t sent) {
    check(!error && sent == 3, "incorrect async send result");
    ++completed;
  };
  fixture.conn->send_file_async(0, 3, reader, done);
  fixture.conn->send_file_async(0, 3, reader, done);
  fixture.until([&] { return reads.size() == 1; });
  reads[0]({}, "abc");
  fixture.until([&] { return reads.size() == 2; });
  reads[0]({}, "BAD");
  fixture.drain();
  check(fixture.conn->_writes.front().read_pending, "stale completion replaced the next file read");
  reads[1]({}, "xyz");
  std::string wire;
  fixture.until([&] {
    wire += fixture.read_wire();
    return completed == 2 && wire.size() == 6;
  });
  check(wire == "abcxyz", "stale read corrupted the next file");
}

void check_client_upload(Fixture& fixture, const std::string& directory) {
  Transfer::File listener;
  listener.fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  check(listener.fd >= 0, "cannot create upload test listener");
  InetAddress address(0);
  check(::bind(listener.fd, address.get_sock_addr(), sizeof(sockaddr_in)) == 0,
        "cannot bind upload test listener");
  check(::listen(listener.fd, 4) == 0, "cannot listen for upload client");
  sockaddr_in actual{};
  socklen_t length = sizeof actual;
  check(::getsockname(listener.fd, reinterpret_cast<sockaddr*>(&actual), &length) == 0,
        "cannot obtain listener address");
  Client client(&fixture.loop, InetAddress(actual));
  client._disk_executor.start(2);
  client._client.connect();
  Transfer::File peer;
  fixture.until([&] {
    if (peer.fd < 0) peer.fd = ::accept4(listener.fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    auto conn = client._client.connection();
    return peer.fd >= 0 && conn && conn->connected();
  });
  auto receive = [&] {
    std::string bytes;
    char data[8192];
    for (;;) {
      const auto n = ::recv(peer.fd, data, sizeof data, 0);
      if (n > 0) bytes.append(data, static_cast<std::size_t>(n));
      else if (n == 0 || errno == EAGAIN || errno == EWOULDBLOCK) break;
      else if (errno != EINTR) throw std::runtime_error("client upload recv failed");
    }
    return bytes;
  };
  auto control = [&](const std::string& text) {
    check(::send(peer.fd, text.data(), text.size(), MSG_NOSIGNAL) ==
              static_cast<ssize_t>(text.size()), "cannot send upload control");
  };
  const std::string path = directory + "/upload source.bin";
  std::string payload(3 * TcpConnection::kAsyncFileChunk + 317, '\0');
  for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(i % 251);
  std::ofstream(path, std::ios::binary).write(payload.data(), payload.size());
  const auto relative = std::filesystem::relative(path).string();
  client.upload(Transfer::quote(relative) + " \"remote.bin\"");
  check(client._transfer_state == Client::FileTransferState::preparing_upload_source,
        "client did not defer source preparation");
  std::string request;
  fixture.until([&] { request += receive(); return request.find('\n') != std::string::npos; });
  check(request == "@TLSS UPLOAD " + std::to_string(payload.size()) + " \"remote.bin\"\n",
        "async source preparation changed the upload request");
  control("\n@TLSS UREADY\n");
  std::string body;
  fixture.until([&] { body += receive(); return body.size() >= payload.size(); });
  check(body == payload, "client async upload content differs");
  auto digest = Transfer::make_digest();
  Transfer::update_digest(digest.get(), payload.data(), payload.size());
  control("\n@TLSS UOK " + Transfer::finish_digest(digest.get()) + "\n");
  fixture.until([&] { return client._transfer_state == Client::FileTransferState::idle; });
  check(!client._upload_source, "client retained the completed upload source");
  peer.reset();
  fixture.until([&] { return !client._client.connection(); });
  fixture.drain();
}

void check_reader_edge(Fixture& fixture, bool empty) {
  bool read_called = false;
  int callbacks = 0;
  std::error_code result;
  std::uint64_t sent = 1;
  fixture.conn->send("A");
  fixture.conn->send_file_async(0, empty ? 0 : 3,
      [&](off_t, std::size_t, TcpConnection::FileReadCompletion) {
        read_called = true;
        throw std::runtime_error("reader failed synchronously");
      },
      [&](std::error_code error, std::uint64_t bytes) {
        ++callbacks; result = error; sent = bytes;
      });
  fixture.conn->send("B");
  std::string wire;
  fixture.until([&] {
    wire += fixture.read_wire();
    return callbacks == 1 && (empty ? wire.size() == 2 : fixture.conn->disconnected());
  });
  check(callbacks == 1 && sent == 0, "reader edge completion is incorrect");
  if (empty) {
    check(!read_called && !result && wire == "AB", "empty file invoked the reader or reordered bytes");
  } else {
    check(read_called && result == std::errc::io_error, "synchronous reader failure was lost");
  }
}

void check_legacy_send(Fixture& fixture, const std::string& directory) {
  const std::string path = directory + "/legacy.bin";
  const std::string payload(2 * 65536 + 17, 'x');
  std::ofstream(path, std::ios::binary).write(payload.data(), payload.size());
  auto source = Transfer::open_source(path);
  int callbacks = 0;
  fixture.conn->send("HEAD");
  fixture.conn->send_file(source->file.fd, 0, source->size,
      [&](std::error_code error, std::uint64_t sent) {
        check(!error && sent == payload.size(), "legacy send_file completion changed");
        ++callbacks;
      });
  source.reset();
  fixture.conn->send("TAIL");
  const auto wire = fixture.await_wire("TAIL");
  fixture.until([&] { return callbacks == 1; });
  check(wire == "HEAD" + payload + "TAIL", "legacy send_file changed wire order");
}

void check_source_eof(Fixture& fixture, const std::string& directory) {
  const std::string path = directory + "/truncated.bin";
  const std::string payload(TcpConnection::kAsyncFileChunk + 1, 'x');
  std::ofstream(path, std::ios::binary).write(payload.data(), payload.size());
  std::shared_ptr<Transfer::AsyncFileSource> source;
  int callbacks = 0;
  std::error_code error;
  std::uint64_t sent = 0;
  source = Transfer::AsyncFileSource::open(
      fixture.service._disk_executor, fixture.conn,
      [&]() -> Transfer::AsyncFileSource::Opened {
        auto file = Transfer::open_source(path);
        std::filesystem::resize_file(path, TcpConnection::kAsyncFileChunk);
        return {std::move(file), {}};
      },
      [&](const Transfer::AsyncFileSource::Info& info) {
        fixture.conn->send_file_async(0, info.size, source->reader(),
            [&](std::error_code result, std::uint64_t count) {
              ++callbacks; error = result; sent = count;
              source->cancel();
            });
      }, [](std::string error) { throw std::runtime_error(error); });
  std::string wire;
  fixture.until([&] {
    wire += fixture.read_wire();
    return fixture.conn->disconnected() && callbacks == 1;
  });
  check(error == std::errc::io_error && sent == TcpConnection::kAsyncFileChunk,
        "early EOF was not reported with actual socket progress");
  check(wire == std::string(TcpConnection::kAsyncFileChunk, 'x'), "early EOF corrupted prior bytes");
}

void check_shutdown_waits(const std::string& directory) {
  EventLoop loop;
  StorageService service(&loop, InetAddress(0), "shutdown-regression", 0, 0, 32);
  service.start();

  int sockets[2];
  check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) == 0,
        "socketpair failed");
  auto conn = std::make_shared<TcpConnection>(&loop, "shutdown-test", sockets[0],
                                             InetAddress(), InetAddress());
  conn->set_connection_cb({});
  conn->connect_established();

  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto release_future = release.get_future().share();
  bool ready = false;
  auto receiver = Transfer::AsyncReceiver::create(
      service._disk_executor, conn, 0,
      [directory] { return std::make_unique<Transfer::Receiver>(directory, 0); },
      [&](Transfer::Receiver&) {
        finalizer_active.store(true);
        entered.set_value();
        release_future.wait();
        finalizer_active.store(false);
        return std::string{};
      },
      [&] { ready = true; }, [](std::string) {}, [](std::string) {});

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!ready) {
    check(std::chrono::steady_clock::now() < deadline, "receiver initialization timed out");
    loop.queue_in_loop([&] { loop.quit(); });
    loop.wakeup();
    loop.loop();
  }
  receiver->finish();
  check(entered_future.wait_until(deadline) == std::future_status::ready,
        "finalizer did not start");

  bool stopped = false;
  std::thread stopper([&] { stopped = service.stop(); });
  std::thread unblock([&] {
    // Release only after shutdown reaches either the disk barrier (fixed)
    // or database shutdown (the regression), without assuming a fixed delay.
    for (;;) {
      bool closing = false;
      {
        std::lock_guard<std::mutex> lock(service._disk_executor._mutex);
        closing = service._disk_executor._phase == Transfer::DiskExecutor::Phase::closing;
      }
      if (closing || database_shutdown_called.load() ||
          std::chrono::steady_clock::now() >= deadline) break;
      std::this_thread::yield();
    }
    release.set_value();
  });

  loop.loop();
  stopper.join();
  unblock.join();
  receiver->cancel();
  service._disk_executor.stop();
  conn->connect_destroyed();
  conn.reset();
  ::close(sockets[1]);

  check(stopped, "service shutdown failed");
  check(!shutdown_during_finalize.load(),
        "database was shut down before the active disk finalizer finished");
}

void run(const std::string& scenario, const std::string& directory) {
  if (scenario == "source_type") {
    bool rejected = false;
    try { Transfer::open_source(directory); }
    catch (const std::exception&) { rejected = true; }
    check(rejected, "directory accepted as an upload source");
    return;
  }
  if (scenario == "source_fifo") {
    const std::string path = directory + "/fifo";
    check(::mkfifo(path.c_str(), 0600) == 0, "mkfifo failed");
    ::alarm(2);
    bool rejected = false;
    try { Transfer::open_source(path); }
    catch (const std::exception&) { rejected = true; }
    ::alarm(0);
    check(rejected, "FIFO accepted as an upload source");
    return;
  }
  if (scenario == "commit_hash") {
    StorageEngine engine(*BASE::DbConnectionPool::get_connection_pool());
    check(engine.commit_upload("test_user", "42", "/", "file.bin", abc_hash),
          "valid SHA-256 rejected before INSERT");
    check(inserts == 1, "valid metadata did not reach INSERT");
    check(!engine.commit_upload("test_user", "42", "/", "bad.bin", "bad"),
          "invalid SHA-256 accepted");
    check(inserts == 1, "invalid metadata reached INSERT");
    return;
  }

  if (scenario == "shutdown_wait") {
    check_shutdown_waits(directory);
    return;
  }

  Fixture fixture;
  if (scenario == "cold_send" || scenario == "source_cancel") {
    check_cold_send(fixture, directory, scenario == "source_cancel");
    return;
  }
  if (scenario == "stale_read") {
    check_stale_read(fixture);
    return;
  }
  if (scenario == "legacy_send") {
    check_legacy_send(fixture, directory);
    return;
  }
  if (scenario == "source_eof") {
    check_source_eof(fixture, directory);
    return;
  }
  if (scenario == "client_upload") {
    check_client_upload(fixture, directory);
    return;
  }
  if (scenario == "empty_send" || scenario == "reader_error") {
    check_reader_edge(fixture, scenario == "empty_send");
    return;
  }
  if (scenario == "upload") {
    fixture.receive("@TLSS UPLOAD 3 \"file.bin\"\n");
    check(fixture.await_wire("\n@TLSS UREADY\n").find("\n@TLSS UREADY\n") != std::string::npos,
          "successful upload preparation did not produce UREADY");
    fixture.receive("abc");
    check(fixture.await_wire("\n@TLSS UOK " + abc_hash + "\n").find("\n@TLSS UOK " + abc_hash + "\n") != std::string::npos,
          "completed upload did not produce UOK");
    check(inserts == 1, "upload did not register metadata");
    std::ifstream stored(directory + "/" + abc_hash, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(stored)), {});
    check(bytes == "abc", "published object content differs");
  } else if (scenario == "download") {
    std::ofstream(directory + "/" + abc_hash, std::ios::binary) << "abc";
    fixture.receive("@TLSS DOWNLOAD \"file.bin\"\n");
    const std::string response = fixture.await_wire("\n@TLSS DONE\n");
    check(response.find("\n@TLSS DATA 3 " + abc_hash + "\nabc\n@TLSS DONE\n") != std::string::npos,
          "download header, body or DONE is missing or out of order");
    fixture.receive("@TLSS ACK\n");
    check(fixture.await_wire("\n@TLSS OK\n").find("\n@TLSS OK\n") != std::string::npos,
          "download ACK did not produce OK");
    check(fixture.session->transfer.phase == TransferPhase::idle,
          "download did not return to idle");
  } else if (scenario == "error_frame") {
    fixture.service.transfer_failed(fixture.conn, fixture.session, "not available");
    check(fixture.await_wire("\n@TLSS ERROR \"not available\"\n").find("\n@TLSS ERROR \"not available\"\n") != std::string::npos,
          "failure response is not a parseable ERROR frame");
  } else if (scenario == "busy_reply") {
    fixture.session->directory_change_pending = true;
    fixture.service.handle_upload(fixture.conn, fixture.session, "3 \"file.bin\"");
    check(fixture.await_wire("\n@TLSS ERROR \"session is busy\"\n").find("\n@TLSS ERROR \"session is busy\"\n") != std::string::npos,
          "busy upload leaves the client waiting for a control response");
    check(fixture.session->directory_change_pending,
          "upload rejection cancelled the existing directory operation");
  } else if (scenario == "receive_error") {
    bool ready = false;
    bool failed = false;
    auto factory = [directory] {
      auto receiver = std::make_unique<Transfer::Receiver>(directory, 3);
      receiver->file_.reset(::open("/dev/full", O_WRONLY | O_CLOEXEC));
      check(receiver->file_.fd >= 0, "cannot open /dev/full");
      return receiver;
    };
    fixture.session->transfer.writer = Transfer::AsyncReceiver::create(
        fixture.service._disk_executor, fixture.conn, 3, std::move(factory),
        [](Transfer::Receiver&) { return std::string{}; },
        [&] {
          fixture.session->transfer.phase = TransferPhase::receiving_upload;
          ready = true;
        },
        [](std::string) {},
        [&](std::string) { failed = true; fixture.conn->force_close(); });
    fixture.until([&] { return ready; });
    fixture.receive("abc");
    fixture.until([&] { return fixture.conn->disconnected(); });
    check(failed, "disk error callback was not delivered");

  } else if (scenario == "upload_chunked") {
    std::string payload(3 * Transfer::AsyncReceiver::kChunkBytes + 317, '\0');
    for (std::size_t i = 0; i < payload.size(); ++i) {
      payload[i] = static_cast<char>(i % 251);
    }
    auto digest = Transfer::make_digest();
    Transfer::update_digest(digest.get(), payload.data(), payload.size());
    const auto hash = Transfer::finish_digest(digest.get());
    fixture.receive("@TLSS UPLOAD " + std::to_string(payload.size()) + " \"large.bin\"\n");
    fixture.await_wire("\n@TLSS UREADY\n");
    fixture.receive(payload + "pwd\n");
    const auto response = fixture.await_wire("/\r\ntlss@test_user:/>");
    check(response.find("\n@TLSS UOK " + hash + "\n") != std::string::npos,
          "chunked upload did not finish before the following command");
    std::ifstream stored(directory + "/" + hash, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(stored)), {});
    check(bytes == payload, "chunked binary upload lost or reordered bytes");
    fixture.receive("@TLSS UPLOAD 0 \"empty.bin\"\n");
    const auto empty_digest = Transfer::make_digest();
    const auto empty_hash = Transfer::finish_digest(empty_digest.get());
    fixture.await_wire("\n@TLSS UOK " + empty_hash + "\n");
    check(std::filesystem::file_size(directory + "/" + empty_hash) == 0,
          "empty upload was not published");
    check(inserts == 2, "chunked or empty upload metadata is missing");
  } else if (scenario == "client_download") {
    Client client(&fixture.loop, InetAddress(0));
    client._disk_executor.start(2);
    client._download_destination = directory + "/download.bin";
    client._transfer_state = Client::FileTransferState::waiting_download_header;
    fixture.conn->set_message_cb([&](const TcpConnectionPtr& conn, Buffer* buffer,
                                    TIME::Timestamp time) {
      client.on_message(conn, buffer, time);
    });
    fixture.receive("\n@TLSS DATA 3 " + abc_hash + "\nabc\n@TLSS DONE\n");
    fixture.await_wire("@TLSS ACK\n");
    check(client._transfer_state == Client::FileTransferState::waiting_download_ok,
          "client ACK preceded finalization");
    std::ifstream stored(directory + "/download.bin", std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(stored)), {});
    check(bytes == "abc", "client ACK preceded local file publication");
    fixture.receive("\n@TLSS OK\n");
    fixture.until([&] { return client._transfer_state == Client::FileTransferState::idle; });
    fixture.conn->set_message_cb({});
  } else if (scenario == "control_limit") {
    {
      Client client(&fixture.loop, InetAddress(0));
      client._transfer_state = Client::FileTransferState::waiting_upload_result;
      const std::string response = "\n@TLSS UOK " + abc_hash + std::string(5000, ' ') + "\n";
      Buffer buffer;
      buffer.append(response.data(), response.size());
      client.on_message(fixture.conn, &buffer, TIME::Timestamp());
      fixture.drain();
      check(fixture.conn->disconnected(),
            "complete oversized control frame bypassed the size limit");
    }
    fixture.drain();
  } else {
    throw std::runtime_error("unknown scenario");
  }
}
}

int main(int argc, char** argv) {
  try {
    check(argc == 2, "one scenario argument is required");
    TemporaryDirectory temporary;
    run(argv[1], temporary.path);
    std::cout << "PASS " << argv[1] << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
