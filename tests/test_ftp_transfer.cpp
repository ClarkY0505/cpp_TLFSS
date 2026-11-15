#include "common/base/mysql_connection.h"
#include "common/net/event_loop.h"
#include "ftp/async_file_source.h"
#include "ftp/client.h"
#include "ftp/server.h"
#include "ftp/service_engine.h"
#include "ftp/storage_root_lock.h"
#include "ftp/transfer_validation.h"

#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <system_error>
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
    if (!enabled)
      return;
    const auto largest = largest_read.load();
    if (count > largest)
      largest_read.store(count);
    entered = true;
    changed.notify_all();
    changed.wait(lock, [&] {
      return released;
    });
  }
} cold_io;
}  // namespace

extern "C" ssize_t sendfile(int out, int in, off_t* offset, size_t count) noexcept {
  cold_io.wait(count);
  return static_cast<ssize_t>(::syscall(SYS_sendfile, out, in, offset, count));
}

extern "C" ssize_t pread(int fd, void* data, size_t count, off_t offset) {
  cold_io.wait(count);
  return static_cast<ssize_t>(::syscall(SYS_pread64, fd, data, count, offset));
}

// Database and authentication substitutes keep these regression checks local. /
// 使用数据库和认证替代实现，让这些回归检查在本地独立运行。 The actual FTP handlers, file receiver,
// client parser and storage engine run. / 运行实际的 FTP
// 处理函数、文件接收器、客户端解析器和存储引擎。
namespace {
std::atomic<int> inserts{0};
std::atomic<int> auth_status{2};
std::atomic<int> signup_status{4};
// 上传事务测试：模拟提交响应丢失，并检查没有重试或补发回滚。
std::atomic<bool> upload_outcome_unknown{false};
std::atomic<int> upload_commits{0};
std::atomic<int> upload_rollbacks{0};
// 启动准备测试的数据库预检结果及查询计数。
std::atomic<bool> object_table_available{true};
std::atomic<int> object_table_checks{0};
std::atomic<int> referenced_list_queries{0};
// 恢复入口模拟：0 为空清单，1 为查询失败，2 为非法哈希，3 为单个候选。
std::atomic<int> deleting_list_mode{0};
std::atomic<int> deleting_list_queries{0};
std::atomic<bool> startup_deleting_record{false};
std::atomic<bool> startup_object_referenced{false};
std::atomic<int> startup_object_deletes{0};
std::atomic<int> startup_disk_lookups{0};
std::atomic<int> startup_object_registrations{0};
std::atomic<bool> startup_registered_record{false};
// 0 正常，1 磁盘对象查询失败，2 磁盘对象登记失败。
std::atomic<int> startup_backfill_mode{0};
std::string startup_registered_state;
std::atomic<bool> single_connection_only{false};
std::atomic<int> borrowed_connections{0};
std::atomic<int> peak_borrowed_connections{0};
std::atomic<bool> fail_startup_sync{false};
std::atomic<int> startup_sync_calls{0};
std::string startup_temporary_path;
struct CommitGate {
  std::mutex mutex;
  std::condition_variable changed;
  bool enabled = false;
  bool entered = false;
  bool released = false;
  void wait() {
    std::unique_lock<std::mutex> lock(mutex);
    if (!enabled)
      return;
    entered = true;
    changed.notify_all();
    changed.wait(lock, [&] {
      return released;
    });
  }
  void release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    changed.notify_all();
  }
} commit_gate;
struct CommitRelease {
  ~CommitRelease() {
    commit_gate.release();
  }
};
std::atomic<bool> fail_metadata_insert{false};
std::atomic<bool> database_shutdown_called{false};
std::atomic<bool> finalizer_active{false};
std::atomic<bool> shutdown_during_finalize{false};
// 服务生命周期测试：在关闭数据库的边界检查 GC 已退出并归还连接。
TLSS::BASE::Thread* watched_gc_thread = nullptr;
std::atomic<bool> gc_callback_active{false};
std::atomic<bool> shutdown_before_gc_finished{false};
const std::string abc_hash = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
// rm 命令专用的数据库替代状态。其他 FTP 场景继续使用原有替代逻辑。
std::atomic<bool> rm_mode{false};
std::atomic<int> rm_queries{0}, rm_deletes{0}, rm_borrows{0};
std::atomic<bool> rm_exists{true}, rm_pending_delete{false};
bool rm_unknown_applies = false;
int rm_failure = 0; // 1 SQL 失败，2 零行，3 标准异常，4 非标准异常。
std::thread::id rm_io_thread;
const std::string rm_filename = "report with spaces.txt";
// mkdir 使用独立替代状态，验证新三态接口从命令分发一直传递到网络响应。
std::atomic<bool> mkdir_mode{false}, mkdir_exists{false}, mkdir_pending_insert{false};
std::atomic<int> mkdir_borrows{0}, mkdir_queries{0}, mkdir_inserts{0};
int mkdir_failure = 0; // 1 SQL 失败，2 零行，3 标准异常，4 非标准异常，5 父目录不存在。
bool mkdir_unknown_applies = false;
std::thread::id mkdir_io_thread;
// rmdir 的目录记录独立于共享对象；测试不能顺便删除对象文件或对象表记录。
std::atomic<bool> rmdir_mode{false}, rmdir_exists{true}, rmdir_pending_delete{false};
std::atomic<int> rmdir_borrows{0}, rmdir_queries{0}, rmdir_deletes{0};
int rmdir_children = 0; // 0 为空，1 含文件，2 含子目录。
// 1 DELETE 失败，2 零行，3/4 标准/非标准异常，5 父目录缺失，
// 6 子项查询失败，7 子项结果格式错误，8 借连接失败，9 借连接抛异常。
int rmdir_failure = 0;
bool rmdir_unknown_applies = false;
std::thread::id rmdir_io_thread;
const std::string rmdir_name = "empty with spaces";
void check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
}  // namespace

extern "C" int __real_fsync(int fd);
extern "C" int __wrap_fsync(int fd) {
  if (single_connection_only.load())
    ++startup_sync_calls;
  if (fail_startup_sync.load()) {
    errno = EIO;
    return -1;
  }
  return __real_fsync(fd);
}

namespace TLSS::BASE {
DbConnectionPool::DbConnectionPool()
    : _produce_thread([] {
    })
    , _scanner_thread([] {
    })
    , _connection_cnt(0)
    , _stopping(false)
    , _threads_joined(true)
    , _borrowed_count(0)
    , _active_getters(0) {}
DbConnectionPool::~DbConnectionPool() = default;
DbConnectionPool* DbConnectionPool::get_connection_pool() {
  static DbConnectionPool pool;
  return &pool;
}
DbConnectionHandle DbConnectionPool::get_connection() {
  if (rmdir_mode.load()) {
    ++rmdir_borrows;
    check(std::this_thread::get_id() != rmdir_io_thread,
          "rmdir borrowed a connection on the IO thread");
    if (rmdir_failure == 8)
      return {nullptr, DbConnectionDeleter{}};
    if (rmdir_failure == 9)
      throw std::runtime_error("injected rmdir connection exception");
  }
  if (mkdir_mode.load()) {
    ++mkdir_borrows;
    check(std::this_thread::get_id() != mkdir_io_thread, "mkdir borrowed a connection on the IO thread");
  }
  if (rm_mode.load()) {
    ++rm_borrows;
    check(std::this_thread::get_id() != rm_io_thread,
          "rm borrowed a database connection on the IO thread");
  }
  if (single_connection_only.load()) {
    check(startup_temporary_path.empty() || !std::filesystem::exists(startup_temporary_path),
          "startup borrowed a database connection before cleaning temporary files");
    check(startup_sync_calls.load() > 0,
          "startup borrowed a database connection before directory synchronization");
  }
  // 启动测试模拟只有一条连接：作用域未归还连接时，后续借用立即失败。
  if (single_connection_only.load() && borrowed_connections.load() != 0)
    return DbConnectionHandle(nullptr, DbConnectionDeleter{});
  auto* connection = new DbConnection;
  const int borrowed = borrowed_connections.fetch_add(1) + 1;
  int peak = peak_borrowed_connections.load();
  while (peak < borrowed && !peak_borrowed_connections.compare_exchange_weak(peak, borrowed)) {
  }
  return DbConnectionHandle(connection, DbConnectionDeleter{});
}
bool DbConnectionPool::shutdown(std::chrono::milliseconds) {
  if (finalizer_active.load())
    shutdown_during_finalize.store(true);
  if (watched_gc_thread &&
      ((watched_gc_thread->started() && !watched_gc_thread->_joined) ||
       gc_callback_active.load() || borrowed_connections.load() != 0))
    shutdown_before_gc_finished.store(true);
  database_shutdown_called.store(true);
  return true;
}
void DbConnectionDeleter::operator()(DbConnection* connection) const noexcept {
  if (connection)
    --borrowed_connections;
  delete connection;
}
DbConnection::DbConnection()
    : _conn(nullptr)
    , _connected(false)
    , _transaction_active(false) {}
DbConnection::~DbConnection() = default;
DbQueryResult DbConnection::query_prepared(const std::string& sql, const std::vector<DbValue>& params) {
  if (rmdir_mode.load()) {
    check(_transaction_active && std::this_thread::get_id() != rmdir_io_thread &&
              sql.find("FOR UPDATE") != std::string::npos,
          "rmdir query ran outside its transaction, on the IO thread, or without a row lock");
    const int stage = ++rmdir_queries;
    if (stage == 1) {
      check(sql.find("SELECT id FROM users ") == 0 && params == std::vector<DbValue>{"test_user"},
            "rmdir did not lock the authenticated user first");
      return {true, {{"1"}}};
    }
    if (stage == 2) {
      check(sql.find("SELECT id FROM virtual_files ") == 0 &&
                params == std::vector<DbValue>{"test_user", "/"},
            "rmdir did not query the captured parent directory");
      return rmdir_failure == 5 ? DbQueryResult{true, {}} : DbQueryResult{true, {{"42"}}};
    }
    if (stage == 3) {
      check(sql.find("CAST(path AS BINARY) = CAST(? AS BINARY)") != std::string::npos &&
                params == std::vector<DbValue>{"test_user", "42", rmdir_name, "/" + rmdir_name},
            "rmdir changed its captured target or omitted byte-exact path comparison");
      return rmdir_exists.load() ? DbQueryResult{true, {{"128"}}} : DbQueryResult{true, {}};
    }
    check(stage == 4 && sql.find("file_type") == std::string::npos &&
              params == std::vector<DbValue>{"test_user", "128"},
          "rmdir skipped its emptiness check or excluded a child type");
    if (rmdir_failure == 6) return {};
    if (rmdir_failure == 7) return {true, {{"129", "extra column"}}};
    if (rmdir_children != 0)
      return {true, {{rmdir_children == 1 ? "129" : "130"}}};
    return {true, {}};
  }
  if (mkdir_mode.load()) {
    check(_transaction_active && std::this_thread::get_id() != mkdir_io_thread,
          "mkdir query ran outside its transaction or on the IO thread");
    const int stage = ++mkdir_queries;
    check(sql.find("FOR UPDATE") != std::string::npos, "mkdir omitted its row lock");
    if (stage == 1) {
      check(sql.find("SELECT id FROM users ") == 0 && params == std::vector<DbValue>{"test_user"},
            "mkdir did not lock the authenticated user first");
      return {true, {{"1"}}};
    }
    check(stage == 2 && sql.find("SELECT id FROM virtual_files ") == 0 &&
              params == std::vector<DbValue>{"test_user", "/"},
          "mkdir did not query the captured parent after locking the user");
    return mkdir_failure == 5 ? DbQueryResult{true, {}} : DbQueryResult{true, {{"42"}}};
  }
  if (rm_mode.load()) {
    check(_transaction_active && std::this_thread::get_id() != rm_io_thread,
          "rm query ran outside its transaction or on the IO thread");
    ++rm_queries;
    check(sql.find("FOR UPDATE") != std::string::npos, "rm omitted a required row lock");
    if (sql.find("SELECT id FROM users ") == 0) {
      check(params == std::vector<DbValue>{"test_user"}, "rm changed the authenticated user");
      return {true, {{"1"}}};
    }
    if (sql.find("SELECT id FROM virtual_files ") == 0) {
      check(params == std::vector<DbValue>{"test_user", "/"}, "rm changed the captured directory");
      return {true, {{"42"}}};
    }
    if (sql.find("SELECT id, object_hash FROM virtual_files ") == 0) {
      check(params == std::vector<DbValue>{"test_user", "42", rm_filename},
            "rm parsed the wrong filename or parent");
      return rm_exists.load() ? DbQueryResult{true, {{"128", abc_hash}}}
                             : DbQueryResult{true, {}};
    }
    throw std::runtime_error("unexpected rm query: " + sql);
  }
  // LIMIT 0 预检成功时没有结果行，不能据此判断对象表不可用。
  if (sql.find("SELECT object_hash, state, unreferenced_at") == 0) {
    ++object_table_checks;
    return {object_table_available.load(), {}};
  }
  // 模拟没有任何历史文件引用，避免访问真实 MySQL。
  if (sql.find("SELECT DISTINCT object_hash") == 0) {
    ++referenced_list_queries;
    return {true, {}};
  }
  if (sql.find("SELECT object_hash FROM storage_objects ") == 0) {
    ++deleting_list_queries;
    check(referenced_list_queries.load() == 1,
          "deleting recovery ran before historical backfill");
    check(borrowed_connections.load() == 1,
          "deleting list query retained a previous connection");
    const int mode = deleting_list_mode.load();
    if (mode == 1)
      return {false, {}};
    if (mode == 2)
      return {true, {{"invalid"}}};
    if (mode == 3)
      return {true, {{abc_hash}}};
    return {true, {}};
  }
  // 新上传登记先锁用户，再在同一事务中准备对象记录。
  if (sql.find("SELECT id FROM users ") == 0) {
    check(_transaction_active, "user lookup ran outside transaction");
    return {true, {{"1"}}};
  }
  if (sql.find("SELECT state, unreferenced_at") == 0) {
    if (sql.find("FOR UPDATE") != std::string::npos) {
      check(_transaction_active, "locking object lookup ran outside transaction");
    } else {
      check(single_connection_only.load() && !_transaction_active,
            "unexpected non-locking object lookup");
      ++startup_disk_lookups;
      check(referenced_list_queries.load() == 1 && deleting_list_queries.load() == 0,
            "disk registration ran before historical backfill or after deletion recovery");
      if (startup_backfill_mode.load() == 1)
        return {false, {}};
    }
    if (startup_deleting_record.load())
      return {true, {{"DELETING", "2026-11-11 10:00:00.000000"}}};
    if (startup_registered_record.load())
      return {true, {{startup_registered_state, std::nullopt}}};
    return {true, {}};
  }
  if (sql.find("SELECT id FROM virtual_files WHERE object_hash = ? ") == 0) {
    check(_transaction_active, "reference lookup ran outside transaction");
    if (startup_object_referenced.load())
      return {true, {{"42"}}};
    return {true, {}};
  }
  if (sql.find("f.object_hash") != std::string::npos) {
    return {true, {{abc_hash}}};
  }
  return {true, {{std::string("42")}}};
}
bool DbConnection::update_prepared(const std::string&, const std::vector<DbValue>&) {
  check(!rmdir_mode.load(), "rmdir attempted an unexpected nontransactional write");
  return true;
}
DbExecuteResult DbConnection::execute_prepared(const std::string& sql,
                                               const std::vector<DbValue>& params) {
  if (rmdir_mode.load()) {
    check(_transaction_active && std::this_thread::get_id() != rmdir_io_thread &&
              rmdir_queries.load() == 4 && sql.find("DELETE FROM virtual_files ") == 0 &&
              sql.find("CAST(path AS BINARY) = CAST(? AS BINARY)") != std::string::npos &&
              params == std::vector<DbValue>{"128", "test_user", "42", rmdir_name, "/" + rmdir_name},
          "rmdir DELETE changed its target, ran on the IO thread, or ran before the emptiness check");
    ++rmdir_deletes;
    commit_gate.wait();
    if (rmdir_failure == 1) return {false, 1213, 0, 0};
    if (rmdir_failure == 2) return {true, 0, 0, 0};
    if (rmdir_failure == 3) throw std::runtime_error("injected rmdir SQL exception");
    if (rmdir_failure == 4) throw 7;
    rmdir_pending_delete.store(true);
    return {true, 0, 1, 0};
  }
  if (mkdir_mode.load()) {
    check(_transaction_active && std::this_thread::get_id() != mkdir_io_thread &&
              sql.find("INSERT INTO virtual_files ") == 0 &&
              params == std::vector<DbValue>{"drafts", "test_user", "42", "/drafts"},
          "mkdir INSERT ran on the wrong thread, outside its transaction, or with incorrect parameters");
    ++mkdir_inserts;
    commit_gate.wait();
    if (mkdir_failure == 1) return {false, 1213, 0, 0};
    if (mkdir_failure == 2) return {true, 0, 0, 0};
    if (mkdir_failure == 3) throw std::runtime_error("injected mkdir SQL exception");
    if (mkdir_failure == 4) throw 7;
    mkdir_pending_insert.store(true);
    return {true, 0, 1, 500};
  }
  if (rm_mode.load()) {
    check(_transaction_active && std::this_thread::get_id() != rm_io_thread,
          "rm DELETE ran outside its transaction or on the IO thread");
    check(sql.find("DELETE FROM virtual_files ") == 0 &&
              params == std::vector<DbValue>{"128", "test_user", "42", rm_filename, abc_hash},
          "rm attempted another database write or deleted a different file");
    ++rm_deletes;
    commit_gate.wait();
    if (rm_failure == 1) return {false, 1213, 0, 0};
    if (rm_failure == 2) return {true, 0, 0, 0};
    if (rm_failure == 3) throw std::runtime_error("injected rm SQL exception");
    if (rm_failure == 4) throw 7;
    rm_pending_delete.store(true);
    return {true, 0, 1, 0};
  }
  // 启动回填使用普通 INSERT；上传登记则在显式事务中执行。
  if (!_transaction_active && single_connection_only.load() &&
      sql.find("INSERT INTO storage_objects ") == 0) {
    check(params.size() == 2 && params[0].has_value() && params[1].has_value() &&
              *params[0] == abc_hash,
          "unexpected startup object registration parameters");
    check(!startup_deleting_record.load() && !startup_registered_record.load(),
          "startup registration overwrote an existing object record");
    ++startup_object_registrations;
    if (startup_backfill_mode.load() == 2)
      return {false, 1213, 0, 0};
    startup_registered_record.store(true);
    startup_registered_state = *params[1];
    return {true, 0, 1, 0};
  }
  check(_transaction_active, "upload write ran outside transaction");

  // 保留 inserts 的原有含义：只统计文件登记，不混入对象表写入。
  if (sql.find("INSERT INTO storage_objects ") == 0 ||
      sql.find("UPDATE storage_objects ") == 0) {
    return {true, 0, 1, 0};
  }
  if (sql.find("INSERT INTO virtual_files ") == 0) {
    ++inserts;
    commit_gate.wait();
    if (fail_metadata_insert.load())
      return {};
    return {true, 0, 1, 1};
  }
  if (sql.find("DELETE FROM storage_objects ") == 0) {
    check(startup_deleting_record.load(), "recovery deleted an absent object record");
    ++startup_object_deletes;
    startup_deleting_record.store(false);
    return {true, 0, 1, 0};
  }
  throw std::runtime_error("unexpected upload write: " + sql);
}

// 每条借用连接独立维护事务状态，兼容并发传输场景。
DbTransaction::DbTransaction(DbConnection& connection) noexcept
    : connection_(connection)
    , active_(!connection._transaction_active) {
  if (active_)
    connection_._transaction_active = true;
}

DbTransaction::~DbTransaction() noexcept {
  rollback();
}

DbCommitStatus DbTransaction::commit() noexcept {
  if (!active_)
    return DbCommitStatus::k_not_active;

  ++upload_commits;
  if (rmdir_mode.load()) {
    if (rmdir_pending_delete.load() && (!upload_outcome_unknown.load() || rmdir_unknown_applies))
      rmdir_exists.store(false);
    rmdir_pending_delete.store(false);
  }
  if (mkdir_mode.load()) {
    if (mkdir_pending_insert.load() && (!upload_outcome_unknown.load() || mkdir_unknown_applies))
      mkdir_exists.store(true);
    mkdir_pending_insert.store(false);
  }
  if (rm_mode.load()) {
    if (rm_pending_delete.load() && (!upload_outcome_unknown.load() || rm_unknown_applies))
      rm_exists.store(false);
    rm_pending_delete.store(false);
  }
  connection_._transaction_active = false;
  active_ = false;
  // 未知结果仍结束客户端事务句柄，不能误报回滚已经撤销提交。
  return upload_outcome_unknown.load() ? DbCommitStatus::k_outcome_unknown
                                       : DbCommitStatus::k_committed;
}

bool DbTransaction::rollback() noexcept {
  if (active_) {
    if (rmdir_mode.load())
      rmdir_pending_delete.store(false);
    if (mkdir_mode.load())
      mkdir_pending_insert.store(false);
    if (rm_mode.load())
      rm_pending_delete.store(false);
    ++upload_rollbacks;
    connection_._transaction_active = false;
    active_ = false;
  }
  return true;
}
}  // namespace TLSS::BASE

namespace TLSS::AUTH {
MySqlUserRepository::MySqlUserRepository(BASE::DbConnectionPool&) {}
UserLookupResult MySqlUserRepository::find_active_user(const std::string&) {
  return {};
}
UserCreateResult MySqlUserRepository::create_user_with_root(const std::string&,
                                                            const PasswordCredentials&) {
  return {};
}
AuthService::AuthService(UserRepository& users)
    : users_(users) {}
AuthResult AuthService::authenticate(const LoginRequest&) const {
  return {static_cast<AuthStatus>(auth_status.load()), 1, "test_user"};
}
SignupResult AuthService::signup(const SignupRequest&) const {
  return {static_cast<SignupStatus>(signup_status.load()), 1, "test_user"};
}
bool AuthService::valid_signup_username(std::string_view) noexcept {
  return true;
}
bool AuthService::valid_signup_password(std::string_view) noexcept {
  return true;
}
}  // namespace TLSS::AUTH

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
    check(::setenv("TLSS_STORAGE_ROOT", path.c_str(), 1) == 0, "setenv failed");
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

struct Fixture {
  EventLoop loop;
  std::shared_ptr<OBSERVABILITY::StorageMetrics> metrics =
      std::make_shared<OBSERVABILITY::StorageMetrics>();
  StorageService service;
  TcpConnectionPtr conn;
  std::shared_ptr<ConnectionSession> session;
  int peer = -1;

  explicit Fixture(int workers = 0, int queue_size = 32)
      : service(&loop, InetAddress(0), "regression", 0, workers, queue_size, metrics) {
    service._th_pool.start(workers);
    service._disk_executor.start(2);
    int sockets[2];
    check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) == 0,
          "socketpair failed");
    peer = sockets[1];
    conn = std::make_shared<TcpConnection>(&loop, "test", sockets[0], InetAddress(), InetAddress());
    conn->set_connection_cb([this](const TcpConnectionPtr& connection) {
      service.on_connection(connection);
    });
    conn->set_close_cb([this](const TcpConnectionPtr& connection) {
      loop.queue_in_loop([connection] {
        connection->connect_destroyed();
      });
    });
    conn->set_message_cb(
        [this](const TcpConnectionPtr& connection, Buffer* buffer, TIME::Timestamp time) {
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
    if (peer >= 0)
      ::close(peer);
  }

  void drain() {
    // Run one poll/dispatch cycle; completion is determined by until(), not a delay.
    loop.queue_in_loop([this] {
      loop.quit();
    });
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

  std::string read_wire(int descriptor = -1) {
    if (descriptor == -1)
      descriptor = peer;
    std::string result;
    char data[8192];
    for (;;) {
      const ssize_t n = ::recv(descriptor, data, sizeof data, 0);
      if (n > 0)
        result.append(data, static_cast<std::size_t>(n));
      else if (n == 0 || errno == EAGAIN || errno == EWOULDBLOCK)
        return result;
      else if (errno != EINTR)
        throw std::runtime_error("recv failed");
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
    if (thread.joinable())
      thread.join();
    std::lock_guard<std::mutex> lock(cold_io.mutex);
    cold_io.enabled = false;
  }

  void join() {
    if (thread.joinable())
      thread.join();
  }
};

void check_cold_send(Fixture& fixture, const std::string& directory, bool cancel) {
  const std::string path = directory + "/cold.bin";
  std::string payload(3 * 65536 + 317, '\0');
  for (std::size_t i = 0; i < payload.size(); ++i)
    payload[i] = static_cast<char>(i % 251);
  std::ofstream(path, std::ios::binary).write(payload.data(), payload.size());
  int small = 4096;
  check(
      ::setsockopt(fixture.conn->_channel->fd(), SOL_SOCKET, SO_SNDBUF, &small, sizeof small) == 0,
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
        if (!cancel)
          fixture.conn->send(header);
        fixture.conn->send_file_async(0, info.size, source->reader(),
                                      [&](std::error_code error, std::uint64_t bytes) {
                                        source->cancel();
                                        completed(error, bytes);
                                      });
        if (!cancel)
          fixture.conn->send(trailer);
      },
      [](std::string error) {
        throw std::runtime_error(error);
      });
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
  fixture.until([&] {
    return reads.size() == 1;
  });
  reads[0]({}, "abc");
  fixture.until([&] {
    return reads.size() == 2;
  });
  reads[0]({}, "BAD");
  fixture.drain();
  check(completed == 1 && reads.size() == 2, "stale completion completed the next file");
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
    if (peer.fd < 0)
      peer.fd = ::accept4(listener.fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    auto conn = client._client.connection();
    return peer.fd >= 0 && conn && conn->connected();
  });
  auto receive = [&] {
    std::string bytes;
    char data[8192];
    for (;;) {
      const auto n = ::recv(peer.fd, data, sizeof data, 0);
      if (n > 0)
        bytes.append(data, static_cast<std::size_t>(n));
      else if (n == 0 || errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      else if (errno != EINTR)
        throw std::runtime_error("client upload recv failed");
    }
    return bytes;
  };
  auto control = [&](const std::string& text) {
    check(::send(peer.fd, text.data(), text.size(), MSG_NOSIGNAL) ==
              static_cast<ssize_t>(text.size()),
          "cannot send upload control");
  };
  const std::string path = directory + "/upload source.bin";
  std::string payload(3 * TcpConnection::kAsyncFileChunk + 317, '\0');
  for (std::size_t i = 0; i < payload.size(); ++i)
    payload[i] = static_cast<char>(i % 251);
  std::ofstream(path, std::ios::binary).write(payload.data(), payload.size());
  const auto relative = std::filesystem::relative(path).string();
  client.upload(Transfer::quote(relative) + " \"remote.bin\"");
  check(client._transfer_state == Client::FileTransferState::preparing_upload_source,
        "client did not defer source preparation");
  std::string request;
  fixture.until([&] {
    request += receive();
    return request.find('\n') != std::string::npos;
  });
  check(request == "@TLSS UPLOAD " + std::to_string(payload.size()) + " \"remote.bin\"\n",
        "async source preparation changed the upload request");
  control("\n@TLSS UREADY\n");
  std::string body;
  fixture.until([&] {
    body += receive();
    return body.size() >= payload.size();
  });
  check(body == payload, "client async upload content differs");
  auto digest = Transfer::make_digest();
  Transfer::update_digest(digest.get(), payload.data(), payload.size());
  control("\n@TLSS UOK " + Transfer::finish_digest(digest.get()) + "\n");
  fixture.until([&] {
    return client._transfer_state == Client::FileTransferState::idle;
  });
  check(!client._upload_source, "client retained the completed upload source");
  peer.reset();
  fixture.until([&] {
    return !client._client.connection();
  });
  fixture.drain();
}

void check_reader_edge(Fixture& fixture, bool empty) {
  bool read_called = false;
  int callbacks = 0;
  std::error_code result;
  std::uint64_t sent = 1;
  fixture.conn->send("A");
  fixture.conn->send_file_async(
      0, empty ? 0 : 3,
      [&](off_t, std::size_t, TcpConnection::FileReadCompletion) {
        read_called = true;
        throw std::runtime_error("reader failed synchronously");
      },
      [&](std::error_code error, std::uint64_t bytes) {
        ++callbacks;
        result = error;
        sent = bytes;
      });
  fixture.conn->send("B");
  std::string wire;
  fixture.until([&] {
    wire += fixture.read_wire();
    return callbacks == 1 && (empty ? wire.size() == 2 : fixture.conn->disconnected());
  });
  check(callbacks == 1 && sent == 0, "reader edge completion is incorrect");
  if (empty) {
    check(!read_called && !result && wire == "AB",
          "empty file invoked the reader or reordered bytes");
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
  fixture.conn->send_file(
      source->file.fd, 0, source->size, [&](std::error_code error, std::uint64_t sent) {
        check(!error && sent == payload.size(), "legacy send_file completion changed");
        ++callbacks;
      });
  source.reset();
  fixture.conn->send("TAIL");
  const auto wire = fixture.await_wire("TAIL");
  fixture.until([&] {
    return callbacks == 1;
  });
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
                                        ++callbacks;
                                        error = result;
                                        sent = count;
                                        source->cancel();
                                      });
      },
      [](std::string error) {
        throw std::runtime_error(error);
      });
  std::string wire;
  fixture.until([&] {
    wire += fixture.read_wire();
    return fixture.conn->disconnected() && callbacks == 1;
  });
  check(error == std::errc::io_error && sent == TcpConnection::kAsyncFileChunk,
        "early EOF was not reported with actual socket progress");
  check(wire == std::string(TcpConnection::kAsyncFileChunk, 'x'),
        "early EOF corrupted prior bytes");
}

void check_shutdown_waits(const std::string& directory) {
  EventLoop loop;
  StorageService service(&loop, InetAddress(0), "shutdown-regression", 0, 0, 32);
  service.start();

  int sockets[2];
  check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) == 0,
        "socketpair failed");
  auto conn = std::make_shared<TcpConnection>(&loop, "shutdown-test", sockets[0], InetAddress(),
                                              InetAddress());
  conn->set_connection_cb({});
  conn->connect_established();

  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto release_future = release.get_future().share();
  bool ready = false;
  auto receiver = Transfer::AsyncReceiver::create(
      service._disk_executor, conn, 0,
      [directory] {
        return std::make_unique<Transfer::Receiver>(directory, 0);
      },
      [&](Transfer::Receiver&) {
        finalizer_active.store(true);
        entered.set_value();
        release_future.wait();
        finalizer_active.store(false);
        return std::string{};
      },
      [&] {
        ready = true;
      },
      [](std::string) {
      },
      [](std::string) {
      });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!ready) {
    check(std::chrono::steady_clock::now() < deadline, "receiver initialization timed out");
    loop.queue_in_loop([&] {
      loop.quit();
    });
    loop.wakeup();
    loop.loop();
  }
  receiver->finish();
  check(entered_future.wait_until(deadline) == std::future_status::ready,
        "finalizer did not start");

  bool stopped = false;
  std::thread stopper([&] {
    stopped = service.stop();
  });
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
          std::chrono::steady_clock::now() >= deadline)
        break;
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

void check_gc_startup_cleanup(const std::string& scenario) {
  EventLoop loop;
  database_shutdown_called.store(false);
  shutdown_before_gc_finished.store(false);
  bool rejected = false;
  bool listener_closed = false;
  bool gc_was_started = false;
  bool service_was_started = false;
  {
    StorageService service(&loop, InetAddress(0), "gc-startup-regression", 0, 0, 32);
    watched_gc_thread = &service._object_gc_worker._thread;

    if (scenario == "gc_start_failure") {
      // 确定性地令 GC start() 返回 false，验证服务会向上传播启动失败。
      // 真正的线程创建失败及重试已由 object_gc_worker_creation_failure 覆盖。
      service._object_gc_worker.stop();
    } else if (scenario == "gc_late_start_failure") {
      // 零个 IO 子线程时，该回调由启动者同步执行，可以安全注入 TCP 启动异常。
      service._tcp_server.set_thread_init_cb([](EventLoop*) {
        throw std::runtime_error("injected TCP startup failure");
      });
    }

    if (scenario != "gc_unstarted_cleanup") {
      try {
        service.start();
      } catch (const std::runtime_error& error) {
        rejected = std::string(error.what()) ==
            (scenario == "gc_start_failure" ? "Failed to start object GC worker"
                                             : "injected TCP startup failure");
      }
    }
    listener_closed = !service._tcp_server._acceptor->listenning();
    gc_was_started = service._object_gc_worker._thread.started();
    service_was_started = service._started;
    // 离开作用域走未启动完成的析构路径，主事件循环始终没有运行。
  }
  watched_gc_thread = nullptr;
  check(!service_was_started && listener_closed,
        "failed or omitted startup marked service running or opened its listener");
  check(rejected == (scenario != "gc_unstarted_cleanup"),
        "startup failure was swallowed or the wrong exception escaped");
  check(gc_was_started == (scenario == "gc_late_start_failure"),
        "GC started at the wrong point in service startup");
  check(database_shutdown_called.load() && !shutdown_before_gc_finished.load(),
        "startup cleanup closed the database before joining GC or leaked resources");
}

void check_gc_shutdown(bool active) {
  EventLoop loop;
  CommitGate gate;
  gate.enabled = true;
  database_shutdown_called.store(false);
  shutdown_before_gc_finished.store(false);
  bool entered = !active;
  bool stopped = false;
  bool stopped_again = false;
  bool joined = false;
  bool requested_before_release = !active;
  bool database_closed_while_blocked = false;
  {
    StorageService service(&loop, InetAddress(0), "gc-stop-regression", 0, 0, 32);
    watched_gc_thread = &service._object_gc_worker._thread;
    if (active) {
      // 只替换线程回调，保留真实的 Worker start/stop、BASE::Thread 和服务停机流程。
      // 定时批处理本身由 object_gc_worker_* 覆盖；这里确定性模拟持有连接的 GC 工作。
      service._object_gc_worker._thread._func = [&] {
        auto connection = service._db_conn_pool->get_connection();
        gc_callback_active.store(true);
        gate.wait();
        connection.reset();
        gc_callback_active.store(false);
      };
    }
    service.start();
    if (active) {
      std::unique_lock<std::mutex> lock(gate.mutex);
      entered = gate.changed.wait_for(lock, std::chrono::seconds(3), [&] {
        return gate.entered;
      });
    }

    std::thread stopper([&] {
      stopped = service.stop();
      stopped_again = service.stop();  // 服务层重复停机也应复用结果。
    });
    std::thread unblock;
    if (active) {
      unblock = std::thread([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        for (;;) {
          {
            std::lock_guard<std::mutex> lock(service._object_gc_worker._mutex);
            requested_before_release = service._object_gc_worker._stop_requested;
          }
          if (requested_before_release || database_shutdown_called.load() ||
              std::chrono::steady_clock::now() >= deadline)
            break;
          std::this_thread::yield();
        }
        // 观察到停止请求后才放行，验证 stop 不能跳过正在执行的 GC 工作。
        database_closed_while_blocked = database_shutdown_called.load();
        gate.release();
      });
    }
    loop.loop();
    stopper.join();
    if (unblock.joinable())
      unblock.join();
    joined = service._object_gc_worker._thread.started() &&
             service._object_gc_worker._thread._joined &&
             service._object_gc_worker._phase == ObjectGcWorker::Phase::stopped;
  }
  watched_gc_thread = nullptr;
  check(entered && requested_before_release, "service did not stop the active GC worker");
  check(stopped && stopped_again && joined, "service stop did not join GC or was not repeatable");
  check(!database_closed_while_blocked && !shutdown_before_gc_finished.load() &&
            !gc_callback_active.load() && borrowed_connections.load() == 0 &&
            database_shutdown_called.load(),
        "service closed the database before GC finished and returned its connection");
}

void wait_rm_gate() {
  std::unique_lock<std::mutex> lock(commit_gate.mutex);
  check(commit_gate.changed.wait_for(lock, std::chrono::seconds(3), [] {
    return commit_gate.entered;
  }), "rm did not reach its database boundary");
}

// 实际网络分发 -> 实际业务线程 -> 实际删除事务 -> IO 回调。
// 数据库替代层只模拟提交状态；真实 MySQL 的锁等待另行验证。
void check_rm(const std::string& scenario, const std::string& directory) {
  rm_mode.store(true);
  rm_io_thread = std::this_thread::get_id();
  Fixture fixture(1, 1);
  // 断言抛异常时先释放数据库闸门，避免 Fixture 清理线程池时死锁。
  CommitRelease release;
  const auto request = "rm " + Transfer::quote(rm_filename) + "\n";
  const std::string prompt = "tlss@test_user:/>";

  if (scenario == "rm_help") {
    fixture.receive("help\n");
    check(fixture.await_wire(prompt).find(" rm ") != std::string::npos,
          "rm was omitted from help");
    check(rm_borrows.load() == 0, "help accessed the database");
    return;
  }
  if (scenario == "rm_invalid") {
    // 空参数、未闭合引号、多余参数，以及不能作为单个文件名的输入。
    for (const auto& args : std::vector<std::string>{"", "\"unterminated", "\"a\" \"b\"",
             "\"\"", "../a", "/a", ".", "..", std::string(256, 'x'),
             Transfer::quote("a\\b"), Transfer::quote(std::string("a\0b", 3))}) {
      fixture.receive("rm " + args + "\n");
      const auto wire = fixture.await_wire(prompt);
      check(wire.find("Usage: rm") != std::string::npos ||
                wire.find("Invalid filename.") != std::string::npos,
            "invalid rm arguments did not receive a local error");
      check(fixture.conn->connected() && !fixture.session->directory_change_pending,
            "invalid rm arguments closed or blocked the session");
    }
    check(rm_borrows.load() == 0 && rm_deletes.load() == 0,
          "invalid rm arguments reached the database");
    return;
  }
  if (scenario == "rm_unauthenticated") {
    fixture.session->login_state = LoginState::k_await_choice;
    // 先验证处理函数的防御性检查，再验证线上分发仍受认证交互拦截。
    fixture.service.handle_rm(fixture.conn, fixture.session, Transfer::quote(rm_filename));
    fixture.await_wire("Login required.");
    fixture.receive(request);
    fixture.drain();
    check(rm_borrows.load() == 0 && fixture.conn->connected(),
          "unauthenticated rm reached the database or closed the connection");
    return;
  }
  if (scenario == "rm_transfer_busy" || scenario == "rm_directory_busy") {
    const auto generation = fixture.session->transfer_generation;
    if (scenario == "rm_transfer_busy") {
      fixture.session->transfer = fixture.service._upload_engine.create(
          fixture.service.transfer_context(fixture.conn, fixture.session), {3, "file.bin"},
          [](TransferEvent) {});
    } else {
      fixture.session->directory_change_pending = true;
    }
    auto transfer = fixture.session->transfer;
    const auto current_generation = fixture.session->transfer_generation;
    // 活动传输时 on_message 有更上层的协议检查，这里单独验证 handle_rm 不取消传输。
    fixture.service.handle_rm(fixture.conn, fixture.session, Transfer::quote(rm_filename));
    fixture.await_wire("Please wait.");
    check(fixture.conn->connected() && fixture.session->transfer == transfer &&
              fixture.session->transfer_generation == current_generation &&
              rm_borrows.load() == 0, "busy rm cancelled a transfer or accessed the database");
    check(current_generation >= generation, "transfer generation moved backwards");
    fixture.session->transfer.reset();
    return;
  }
  if (scenario == "rm_queue_full" || scenario == "rm_pool_stopped") {
    if (scenario == "rm_queue_full") {
      commit_gate.enabled = true;
      check(fixture.service._th_pool.try_run([] { commit_gate.wait(); }),
            "could not occupy the business worker");
      wait_rm_gate();
      check(fixture.service._th_pool.try_run([] {}), "could not fill the business queue");
    } else {
      fixture.service._th_pool.stop();
    }
    fixture.receive(request);
    const auto wire = fixture.await_wire(prompt);
    check(wire.find("Server busy. Please retry.") != std::string::npos &&
              wire.find("Usage:") == std::string::npos,
          "task rejection was reported as an argument error");
    check(!fixture.session->directory_change_pending && rm_borrows.load() == 0 &&
              fixture.metrics->get(OBSERVABILITY::Metric::commands_rejected) == 1,
          "rejected rm left the session busy or executed a database operation");
    commit_gate.release();
    fixture.service._th_pool.stop();
    return;
  }

  const bool stale = scenario.rfind("rm_stale_", 0) == 0 || scenario == "rm_expired_session";
  const bool gated = stale || scenario == "rm_pending" || scenario == "rm_disconnect" ||
                     scenario == "rm_stopping";
  if (gated)
    commit_gate.enabled = true;
  if (scenario == "rm_missing") rm_exists.store(false);
  if (scenario == "rm_delete_failure") rm_failure = 1;
  if (scenario == "rm_zero") rm_failure = 2;
  if (scenario == "rm_exception") rm_failure = 3;
  if (scenario == "rm_nonstandard_exception") rm_failure = 4;
  const bool unknown = scenario == "rm_unknown_applied" || scenario == "rm_unknown_not_applied";
  upload_outcome_unknown.store(unknown);
  rm_unknown_applies = scenario == "rm_unknown_applied";

  // 数据库成功删引用后，磁盘对象也应保留；这里只写一个真实的临时对象文件。
  const auto object_path = directory + "/" + abc_hash;
  std::ofstream(object_path, std::ios::binary) << "abc";
  fixture.receive(request);
  // receive() 只写入对端套接字；先驱动 IO loop，等待命令实际进入业务线程。
  fixture.until([] { return rm_borrows.load() == 1; });
  std::shared_ptr<ConnectionSession> original_session;
  if (gated) {
    wait_rm_gate();
    check(fixture.session->directory_change_pending,
          "rm did not mark its session busy before the database operation");
    check(fixture.read_wire().empty(), "rm replied before its transaction finished");
    if (scenario == "rm_pending") {
      fixture.receive("pwd\n" + request);
      const auto busy = fixture.await_wire("Please wait.");
      check(busy.find("File removed.") == std::string::npos && rm_deletes.load() == 1,
            "pending rm allowed another deletion or replied early");
      // IO 线程可以继续运行回调，数据库闸门没有阻塞事件循环。
      bool heartbeat = false;
      fixture.loop.queue_in_loop([&] { heartbeat = true; });
      fixture.drain();
      check(heartbeat, "rm blocked the IO event loop");
    } else if (scenario == "rm_stale_session" || scenario == "rm_expired_session") {
      if (scenario == "rm_stale_session")
        original_session = fixture.session; // 保留旧对象，专门验证会话映射身份检查。
      auto replacement = std::make_shared<ConnectionSession>(*fixture.session);
      {
        std::lock_guard<std::mutex> lock(fixture.service.sessions_mutex_);
        fixture.service._sessions[fixture.conn.get()] = replacement;
      }
      fixture.session = std::move(replacement);
    } else if (scenario == "rm_stale_identity") {
      fixture.session->name = "another_user";
    } else if (scenario == "rm_stale_directory") {
      fixture.session->current_directory = "/changed";
    } else if (scenario == "rm_stale_login") {
      fixture.session->login_state = LoginState::k_await_choice;
    } else if (scenario == "rm_stale_pending") {
      fixture.session->directory_change_pending = false;
    } else if (scenario == "rm_disconnect") {
      fixture.conn->force_close();
      fixture.until([&] { return fixture.conn->disconnected(); });
    } else if (scenario == "rm_stopping") {
      fixture.service._stopping.store(true);
    }
    commit_gate.release();
  }

  // 等待工作线程退出后再执行 IO 回调，确保测试确实检查了最终响应。
  fixture.service._th_pool.stop();
  fixture.drain();
  // send() 将响应放入发送队列，EPOLLOUT 排空后才能检查线上响应或确认静默。
  fixture.until([&] { return !fixture.conn->_channel->is_writing(); });
  const auto wire = fixture.read_wire();
  if (stale || scenario == "rm_disconnect" || scenario == "rm_stopping") {
    check(wire.empty(), "obsolete rm completion replied to an invalid session");
    if (stale)
      check(fixture.session->directory_change_pending == (scenario != "rm_stale_pending"),
            "obsolete rm completion changed the current session's busy flag");
    if (scenario == "rm_stopping") {
      fixture.until([&] { return fixture.conn->disconnected(); });
      check(!fixture.session->directory_change_pending, "stopping rm retained its busy flag");
    }
  } else {
    const bool failed = scenario == "rm_missing" || rm_failure != 0;
    const std::string expected = unknown ? "Delete result is unknown." :
                                 failed ? "Cannot remove file." : "File removed.";
    check(wire.find(expected) != std::string::npos && wire.find(prompt) != std::string::npos,
          "rm returned the wrong result or omitted the prompt");
    check(!fixture.session->directory_change_pending && fixture.conn->connected(),
          "rm completion left the session busy or disconnected");
    if (unknown || failed)
      check(wire.find("File removed.") == std::string::npos,
            "rm reported success for a failed or unknown transaction");
    check(fixture.metrics->get(OBSERVABILITY::Metric::metadata_writes_failed) ==
              (failed || unknown ? 1U : 0U), "rm failure metric is incorrect");
  }

  const bool failed = scenario == "rm_missing" || rm_failure != 0;
  check(rm_borrows.load() == 1 && rm_queries.load() == 3 &&
            rm_deletes.load() == (scenario == "rm_missing" ? 0 : 1) &&
            upload_commits.load() == (failed ? 0 : 1) &&
            upload_rollbacks.load() == (failed ? 1 : 0) && borrowed_connections.load() == 0,
        "rm retried, leaked a connection, or ended the transaction incorrectly");
  check(rm_exists.load() == (scenario == "rm_missing" ? false :
                            failed || scenario == "rm_unknown_not_applied"),
        "rm changed the wrong committed database state");
  std::ifstream object(object_path, std::ios::binary);
  std::string bytes;
  object >> bytes;
  check(bytes == "abc" && startup_object_deletes.load() == 0,
        "rm removed the shared object or its catalog record");
}

// rmdir 从实际网络分发进入业务线程；闸门让数据库等待与 IO 回调顺序可控。
// 每个 CTest 场景单独启动进程，替代数据库状态不会泄漏到下一个场景。
void check_rmdir(const std::string& scenario, const std::string& directory) {
  rmdir_mode.store(true);
  rmdir_io_thread = std::this_thread::get_id();
  Fixture fixture(1, 1);
  CommitRelease release; // 先释放闸门，再析构 Fixture 中的工作线程。
  const auto request = "rmdir " + Transfer::quote(rmdir_name) + "\n";
  const std::string prompt = "tlss@test_user:/>";

  if (scenario == "rmdir_help") {
    fixture.receive("help\n");
    check(fixture.await_wire(prompt).find(" rmdir ") != std::string::npos,
          "rmdir was omitted or misspelled in help");
    check(rmdir_borrows.load() == 0, "rmdir help accessed the database");
    return;
  }
  if (scenario == "rmdir_invalid") {
    // 覆盖缺参、引号、多参、根目录、路径穿越、过长名字和控制字符。
    for (const auto& args : std::vector<std::string>{"", "\"unterminated", "\"a\" \"b\"",
             "\"\"", "../a", "/a", "/", ".", "..", std::string(256, 'x'),
             Transfer::quote("a\\b"), Transfer::quote(std::string("a\0b", 3))}) {
      fixture.receive("rmdir " + args + "\n");
      const auto wire = fixture.await_wire(prompt);
      check(wire.find("Usage: rmdir") != std::string::npos ||
                wire.find("Invalid directory name.") != std::string::npos,
            "invalid rmdir arguments did not receive a local error");
      check(fixture.conn->connected() && !fixture.session->directory_change_pending,
            "invalid rmdir arguments closed or blocked the session");
    }
    check(rmdir_borrows.load() == 0 && rmdir_deletes.load() == 0,
          "invalid rmdir arguments reached the database");
    return;
  }
  if (scenario == "rmdir_unauthenticated") {
    fixture.session->login_state = LoginState::k_await_choice;
    fixture.service.handle_rmdir(fixture.conn, fixture.session, Transfer::quote(rmdir_name));
    fixture.await_wire("Login required.");
    fixture.receive(request);
    fixture.await_wire("login or signup");
    check(rmdir_borrows.load() == 0 && fixture.conn->connected(),
          "unauthenticated rmdir reached the database or closed the connection");
    return;
  }
  if (scenario == "rmdir_stopping_entry") {
    fixture.service._stopping.store(true);
    fixture.service.handle_rmdir(fixture.conn, fixture.session, Transfer::quote(rmdir_name));
    fixture.until([&] { return fixture.conn->disconnected(); });
    check(rmdir_borrows.load() == 0 && !fixture.session->directory_change_pending &&
              fixture.read_wire().empty(), "stopping rmdir submitted work or replied normally");
    return;
  }
  if (scenario == "rmdir_transfer_busy" || scenario == "rmdir_directory_busy") {
    if (scenario == "rmdir_transfer_busy") {
      fixture.session->transfer = fixture.service._upload_engine.create(
          fixture.service.transfer_context(fixture.conn, fixture.session), {3, "file.bin"},
          [](TransferEvent) {});
    } else {
      fixture.session->directory_change_pending = true;
    }
    auto transfer = fixture.session->transfer;
    const auto generation = fixture.session->transfer_generation;
    const bool pending = fixture.session->directory_change_pending;
    // 活动传输有自己的网络协议入口，直接验证处理函数不会取消既有传输。
    fixture.service.handle_rmdir(fixture.conn, fixture.session, Transfer::quote(rmdir_name));
    fixture.await_wire("Please wait.");
    check(fixture.conn->connected() && fixture.session->transfer == transfer &&
              fixture.session->transfer_generation == generation &&
              fixture.session->directory_change_pending == pending && rmdir_borrows.load() == 0,
          "busy rmdir changed the active operation or accessed the database");
    fixture.session->transfer.reset();
    return;
  }
  if (scenario == "rmdir_queue_full" || scenario == "rmdir_pool_stopped") {
    if (scenario == "rmdir_queue_full") {
      commit_gate.enabled = true;
      check(fixture.service._th_pool.try_run([] { commit_gate.wait(); }),
            "could not occupy the rmdir business worker");
      wait_rm_gate();
      check(fixture.service._th_pool.try_run([] {}), "could not fill the rmdir business queue");
    } else {
      fixture.service._th_pool.stop();
    }
    fixture.receive(request);
    const auto wire = fixture.await_wire(prompt);
    check(wire.find("Server busy. Please retry.") != std::string::npos &&
              wire.find("Usage:") == std::string::npos &&
              !fixture.session->directory_change_pending && rmdir_borrows.load() == 0 &&
              fixture.metrics->get(OBSERVABILITY::Metric::commands_rejected) == 1,
          "rejected rmdir ran a transaction, retained its pending flag, or omitted rejection metrics");
    commit_gate.release();
    fixture.service._th_pool.stop();
    return;
  }

  const bool stale = scenario.rfind("rmdir_stale_", 0) == 0 || scenario == "rmdir_expired_session";
  const bool gated = stale || scenario == "rmdir_pending" || scenario == "rmdir_disconnect" ||
                     scenario == "rmdir_stopping";
  if (gated) commit_gate.enabled = true;
  if (scenario == "rmdir_missing") rmdir_exists.store(false);
  if (scenario == "rmdir_delete_failure") rmdir_failure = 1;
  if (scenario == "rmdir_zero") rmdir_failure = 2;
  if (scenario == "rmdir_exception") rmdir_failure = 3;
  if (scenario == "rmdir_nonstandard_exception") rmdir_failure = 4;
  if (scenario == "rmdir_missing_parent") rmdir_failure = 5;
  if (scenario == "rmdir_child_query_failure") rmdir_failure = 6;
  if (scenario == "rmdir_child_malformed") rmdir_failure = 7;
  if (scenario == "rmdir_borrow_failure") rmdir_failure = 8;
  if (scenario == "rmdir_borrow_exception") rmdir_failure = 9;
  if (scenario == "rmdir_not_empty_file") rmdir_children = 1;
  if (scenario == "rmdir_not_empty_directory") rmdir_children = 2;
  const bool unknown = scenario == "rmdir_unknown_applied" || scenario == "rmdir_unknown_not_applied";
  upload_outcome_unknown.store(unknown);
  rmdir_unknown_applies = scenario == "rmdir_unknown_applied";

  // 即使删除目录成功，也不能触碰任何共享对象；临时对象用于检查实际磁盘内容。
  const auto object_path = directory + "/" + abc_hash;
  std::ofstream(object_path, std::ios::binary) << "abc";
  fixture.receive(request);
  fixture.until([] { return rmdir_borrows.load() == 1; });
  std::shared_ptr<ConnectionSession> original_session;
  if (gated) {
    wait_rm_gate();
    check(fixture.session->directory_change_pending && fixture.read_wire().empty(),
          "rmdir failed to mark its session pending or replied before commit");
    if (scenario == "rmdir_pending") {
      fixture.receive("pwd\n" + request);
      const auto busy = fixture.await_wire("Please wait.");
      check(busy.find("directory removed.") == std::string::npos && rmdir_deletes.load() == 1,
            "pending rmdir allowed a second deletion or replied early");
      bool heartbeat = false;
      fixture.loop.queue_in_loop([&] { heartbeat = true; });
      fixture.drain();
      check(heartbeat, "rmdir blocked its IO event loop while waiting for the database");
    } else if (scenario == "rmdir_stale_session" || scenario == "rmdir_expired_session") {
      if (scenario == "rmdir_stale_session")
        original_session = fixture.session; // 弱引用仍可提升，但连接已映射到新会话。
      auto replacement = std::make_shared<ConnectionSession>(*fixture.session);
      {
        std::lock_guard<std::mutex> lock(fixture.service.sessions_mutex_);
        fixture.service._sessions[fixture.conn.get()] = replacement;
      }
      fixture.session = std::move(replacement);
    } else if (scenario == "rmdir_stale_identity") {
      fixture.session->name = "another_user";
    } else if (scenario == "rmdir_stale_directory") {
      fixture.session->current_directory = "/changed";
    } else if (scenario == "rmdir_stale_login") {
      fixture.session->login_state = LoginState::k_await_choice;
    } else if (scenario == "rmdir_stale_pending") {
      fixture.session->directory_change_pending = false;
    } else if (scenario == "rmdir_disconnect") {
      fixture.conn->force_close();
      fixture.until([&] { return fixture.conn->disconnected(); });
    } else if (scenario == "rmdir_stopping") {
      fixture.service._stopping.store(true);
    }
    commit_gate.release();
  }

  // 先等待业务任务完成，再驱动 IO 回调和发送队列，避免把“尚未回复”误判为静默。
  fixture.service._th_pool.stop();
  fixture.drain();
  fixture.until([&] { return !fixture.conn->_channel->is_writing(); });
  const auto wire = fixture.read_wire();
  const bool failed = scenario == "rmdir_missing" || rmdir_failure != 0;
  const bool nonempty = rmdir_children != 0;
  if (stale || scenario == "rmdir_disconnect" || scenario == "rmdir_stopping") {
    check(wire.empty(), "obsolete rmdir completion replied to an invalid session");
    if (stale)
      check(fixture.session->directory_change_pending == (scenario != "rmdir_stale_pending"),
            "obsolete rmdir completion changed the current session's pending flag");
    if (scenario == "rmdir_stopping") {
      fixture.until([&] { return fixture.conn->disconnected(); });
      check(!fixture.session->directory_change_pending, "stopping rmdir retained its pending flag");
    }
  } else {
    const std::string expected = unknown ? "Delete result is unknown." :
                                nonempty ? "Directory is not empty." :
                                failed ? "Cannot remove directory." : "directory removed.";
    check(wire.find(expected) != std::string::npos && wire.find(prompt) != std::string::npos &&
              !fixture.session->directory_change_pending && fixture.conn->connected(),
          "rmdir returned the wrong result, omitted its prompt, or retained its pending flag");
    if (unknown || failed || nonempty)
      check(wire.find("directory removed.") == std::string::npos,
            "rmdir reported success for a nonempty directory, failure, or unknown commit");
    // 四种返回结果都结束本次命令等待；后续命令应能正常执行。
    fixture.receive("pwd\n");
    check(fixture.await_wire(prompt).find("/\r\n") != std::string::npos,
          "rmdir completion left subsequent commands blocked");
  }

  const int queries = rmdir_failure >= 8 ? 0 : rmdir_failure == 5 ? 2 :
                      scenario == "rmdir_missing" ? 3 : 4;
  const bool attempted_delete = queries == 4 && !nonempty && rmdir_failure != 6 && rmdir_failure != 7;
  const bool committed = !failed && !nonempty;
  check(rmdir_borrows.load() == 1 && rmdir_queries.load() == queries &&
            rmdir_deletes.load() == (attempted_delete ? 1 : 0) &&
            upload_commits.load() == (committed ? 1 : 0) &&
            upload_rollbacks.load() == (!committed && queries != 0 ? 1 : 0) &&
            borrowed_connections.load() == 0,
        "rmdir retried, leaked a connection, deleted a nonempty target, or ended its transaction incorrectly");
  check(rmdir_exists.load() == (scenario == "rmdir_missing" ? false :
                               !committed || scenario == "rmdir_unknown_not_applied"),
        "rmdir committed state did not match its simulated database outcome");
  check(fixture.metrics->get(OBSERVABILITY::Metric::metadata_writes_failed) ==
            (failed || nonempty || unknown ? 1U : 0U), "rmdir failure metric was missing or duplicated");
  std::ifstream object(object_path, std::ios::binary);
  std::string bytes;
  object >> bytes;
  check(bytes == "abc" && startup_object_deletes.load() == 0,
        "rmdir removed a shared object or its catalog record");
}

void wait_mkdir_gate() {
  std::unique_lock<std::mutex> lock(commit_gate.mutex);
  check(commit_gate.changed.wait_for(lock, std::chrono::seconds(3), [] {
    return commit_gate.entered;
  }), "mkdir did not reach its blocked database operation");
}

void check_mkdir(const std::string& scenario) {
  mkdir_mode.store(true);
  mkdir_io_thread = std::this_thread::get_id();
  Fixture fixture(1, 1); // 使用真实业务线程，禁止借用零线程同步模式掩盖 IO 阻塞。
  CommitRelease release;
  const std::string prompt = "tlss@test_user:/>";
  if (scenario == "mkdir_unauthenticated") {
    fixture.session->login_state = LoginState::k_await_choice;
    fixture.receive("mkdir drafts\n");
    fixture.await_wire("login or signup");
    check(mkdir_borrows.load() == 0, "unauthenticated mkdir accessed the database");
    return;
  }
  if (scenario == "mkdir_queue_full" || scenario == "mkdir_pool_stopped") {
    if (scenario == "mkdir_queue_full") {
      commit_gate.enabled = true;
      check(fixture.service._th_pool.try_run([] { commit_gate.wait(); }), "could not occupy mkdir worker");
      wait_mkdir_gate();
      check(fixture.service._th_pool.try_run([] {}), "could not fill mkdir task queue");
    } else {
      fixture.service._th_pool.stop();
    }
    fixture.receive("mkdir drafts\n");
    fixture.await_wire("Server busy. Please retry.");
    check(mkdir_borrows.load() == 0 &&
              fixture.metrics->get(OBSERVABILITY::Metric::commands_rejected) == 1,
          "rejected mkdir ran a transaction or omitted its rejection metric");
    commit_gate.release();
    fixture.service._th_pool.stop();
    return;
  }

  const bool unknown = scenario == "mkdir_unknown_applied" || scenario == "mkdir_unknown_not_applied";
  upload_outcome_unknown.store(unknown);
  mkdir_unknown_applies = scenario == "mkdir_unknown_applied";
  if (scenario == "mkdir_sql_failure") mkdir_failure = 1;
  if (scenario == "mkdir_zero") mkdir_failure = 2;
  if (scenario == "mkdir_exception") mkdir_failure = 3;
  if (scenario == "mkdir_nonstandard_exception") mkdir_failure = 4;
  if (scenario == "mkdir_missing_parent") mkdir_failure = 5;
  const bool invalid = scenario == "mkdir_invalid";
  const bool gated = scenario == "mkdir_pending" || scenario == "mkdir_disconnect";
  if (gated)
    commit_gate.enabled = true;
  fixture.receive(invalid ? "mkdir ../drafts\n" : "mkdir drafts\n");
  if (gated) {
    // 先驱动 IO loop 将命令提交，再等待业务线程到达闸门。
    fixture.until([] { return mkdir_inserts.load() == 1; });
    wait_mkdir_gate();
    check(fixture.read_wire().empty(), "mkdir responded before COMMIT");
    if (scenario == "mkdir_pending") {
      fixture.receive("pwd\n");
      const auto wire = fixture.await_wire(prompt);
      check(wire.find("Directory created.") == std::string::npos,
            "mkdir replied before the blocked transaction finished");
    } else {
      fixture.conn->force_close();
      fixture.until([&] { return fixture.conn->disconnected(); });
    }
    commit_gate.release();
  }

  const bool failed = invalid || mkdir_failure != 0;
  if (scenario == "mkdir_disconnect") {
    fixture.service._th_pool.stop();
    fixture.drain();
    check(fixture.read_wire().empty(), "mkdir replied through a disconnected connection");
  } else {
    // send() 还需要 IO loop 排空发送队列，必须等到完整提示符再检查响应。
    const auto wire = fixture.await_wire(prompt);
    const std::string expected = unknown ? "Directory creation result is unknown." :
                                 failed ? "Cannot create directory." : "Directory created.";
    check(wire.find(expected) != std::string::npos && fixture.conn->connected(),
          "mkdir mapped its three-state result to an incorrect response");
    if (failed || unknown)
      check(wire.find("Directory created.") == std::string::npos,
            "mkdir reported success after failure or unknown commit");
    fixture.service._th_pool.stop();
  }
  check(mkdir_borrows.load() == (invalid ? 0 : 1) && mkdir_queries.load() == (invalid ? 0 : 2) &&
            mkdir_inserts.load() == (invalid || mkdir_failure == 5 ? 0 : 1) &&
            upload_commits.load() == (failed ? 0 : 1) &&
            upload_rollbacks.load() == (failed && !invalid ? 1 : 0) && borrowed_connections.load() == 0,
        "mkdir retried, leaked a connection, or ended its transaction incorrectly");
  check(mkdir_exists.load() == (!failed && (!unknown || mkdir_unknown_applies)),
        "mkdir committed state did not match the simulated database outcome");
  check(fixture.metrics->get(OBSERVABILITY::Metric::metadata_writes_failed) ==
            (failed || unknown ? 1U : 0U), "mkdir failure metric was missing or duplicated");
}

void run(const std::string& scenario, const std::string& directory) {
  if (scenario.rfind("rmdir_", 0) == 0) {
    check_rmdir(scenario, directory);
    return;
  }
  if (scenario.rfind("mkdir_", 0) == 0) {
    check_mkdir(scenario);
    return;
  }
  if (scenario.rfind("rm_", 0) == 0) {
    check_rm(scenario, directory);
    return;
  }
  if (scenario == "gc_start_failure" || scenario == "gc_late_start_failure" ||
      scenario == "gc_unstarted_cleanup") {
    check_gc_startup_cleanup(scenario);
    return;
  }
  if (scenario == "gc_shutdown_idle" || scenario == "gc_shutdown_wait") {
    check_gc_shutdown(scenario == "gc_shutdown_wait");
    return;
  }
  if (scenario.find("prepare_") == 0) {
    check(scenario == "prepare_missing_table" || scenario == "prepare_empty_list" ||
              scenario == "prepare_recovery_query_failure" ||
              scenario == "prepare_recovery_invalid_hash" || scenario == "prepare_recovery_skip" ||
              scenario == "prepare_recovery_resume" || scenario == "prepare_recovery_referenced" ||
              scenario == "prepare_recovery_unknown" ||
              scenario == "prepare_cleanup_sync_failure" || scenario == "prepare_cleanup_started" ||
              scenario == "prepare_orphan_valid" || scenario == "prepare_orphan_corrupt" ||
              scenario == "prepare_orphan_existing" || scenario == "prepare_orphan_lookup_failure" ||
              scenario == "prepare_orphan_insert_failure",
          "unknown preparation scenario");
    const bool table_exists = scenario != "prepare_missing_table";
    const bool resume = scenario == "prepare_recovery_resume";
    const bool referenced = scenario == "prepare_recovery_referenced";
    const bool unknown = scenario == "prepare_recovery_unknown";
    const bool cleanup_failure = scenario == "prepare_cleanup_sync_failure";
    const bool already_started = scenario == "prepare_cleanup_started";
    const bool orphan = scenario.find("prepare_orphan_") == 0;
    const bool orphan_corrupt = scenario == "prepare_orphan_corrupt";
    const bool orphan_existing = scenario == "prepare_orphan_existing";
    const bool lookup_failure = scenario == "prepare_orphan_lookup_failure";
    const bool insert_failure = scenario == "prepare_orphan_insert_failure";
    const bool backfill_failure = lookup_failure || insert_failure;
    const bool has_record = resume || referenced || unknown;
    const bool has_disk_object = has_record || orphan;
    const bool expected_ready = table_exists &&
        scenario != "prepare_recovery_query_failure" &&
        scenario != "prepare_recovery_invalid_hash" && !referenced && !unknown &&
        !cleanup_failure && !already_started && !backfill_failure;

    object_table_available.store(table_exists);
    object_table_checks.store(0);
    referenced_list_queries.store(0);
    deleting_list_queries.store(0);
    deleting_list_mode.store(scenario == "prepare_recovery_query_failure" ? 1 :
        scenario == "prepare_recovery_invalid_hash" ? 2 :
        (has_record || scenario == "prepare_recovery_skip") ? 3 : 0);
    startup_deleting_record.store(has_record);
    startup_object_referenced.store(referenced);
    startup_object_deletes.store(0);
    startup_disk_lookups.store(0);
    startup_object_registrations.store(0);
    startup_registered_record.store(orphan_existing);
    startup_registered_state = orphan_existing ? "UNAVAILABLE" : "";
    startup_backfill_mode.store(lookup_failure ? 1 : insert_failure ? 2 : 0);
    upload_outcome_unknown.store(unknown);
    upload_commits.store(0);
    single_connection_only.store(true);
    peak_borrowed_connections.store(0);
    fail_startup_sync.store(cleanup_failure);
    startup_sync_calls.store(0);
    inserts.store(0);

    // 与正式入口一样，先独占对象目录，再运行启动准备。
    StorageRootLock root_lock(directory);
    startup_temporary_path = directory + "/.tlss-part-Ab12c3";
    {
      std::ofstream output(startup_temporary_path);
      output << "interrupted upload";
      output.close();
      check(output.good(), "create startup temporary file failed");
    }

    if (has_disk_object) {
      std::ofstream output(directory + "/" + abc_hash);
      output << (orphan_corrupt ? "bad" : "abc");
      output.close();
      check(output.good(), "create recovery object failed");
    }

    // 只执行准备阶段，不启动服务；端口 0 避免占用固定业务端口。
    EventLoop loop;
    StorageService service(&loop, InetAddress(0), "prepare-regression", 0, 0, 32);
    // 只设置标志验证启动后的防护，不实际启动监听或业务线程。
    service._started = already_started;
    bool ready = false;
    bool cleanup_threw = false;
    try {
      ready = service.prepare_storage();
      check(service._started == already_started,
            "storage preparation unexpectedly changed the started flag");
    } catch (const std::system_error& error) {
      service._started = false;
      fail_startup_sync.store(false);
      if (!cleanup_failure)
        throw;
      check(error.code().value() == EIO, "startup cleanup lost the synchronization error");
      cleanup_threw = true;
    } catch (...) {
      service._started = false;
      fail_startup_sync.store(false);
      throw;
    }
    service._started = false;
    fail_startup_sync.store(false);

    check(cleanup_threw == cleanup_failure, "startup cleanup exception was swallowed or unexpected");
    check(std::filesystem::is_regular_file(directory + "/.tlss.lock"),
          "startup cleanup removed the directory lock file");
    if (cleanup_failure || already_started) {
      check(!ready && object_table_checks.load() == 0 && referenced_list_queries.load() == 0 &&
                deleting_list_queries.load() == 0 && borrowed_connections.load() == 0 &&
                peak_borrowed_connections.load() == 0 && startup_object_deletes.load() == 0,
            "startup continued after failed cleanup or an already-started guard");
      check(std::filesystem::exists(startup_temporary_path) == already_started,
            "startup guard or cleanup failure changed the wrong temporary file state");
      check(startup_sync_calls.load() == (already_started ? 0 : 1),
            "startup guard or cleanup failure performed extra synchronization");
      return;
    }

    check(ready == expected_ready,
          "storage preparation ignored a backfill or recovery result");
    check(object_table_checks.load() == 1,
          "object table was not checked exactly once");
    check(!std::filesystem::exists(startup_temporary_path),
          "startup did not clean the interrupted upload before database preparation");

    // 表缺失时立即停止；表存在时允许成功查询到空清单。
    check(referenced_list_queries.load() == (table_exists ? 1 : 0),
          "historical list query ran at the wrong preparation stage");
    check(deleting_list_queries.load() == (table_exists && !backfill_failure ? 1 : 0),
          "deleting list query ran at the wrong preparation stage");
    check(startup_disk_lookups.load() == (has_disk_object && table_exists ? 1 : 0),
          "startup did not enumerate disk objects before recovering deletions");
    check(startup_object_registrations.load() ==
              (orphan && !orphan_existing && !lookup_failure ? 1 : 0),
          "startup registered an existing object or skipped a disk orphan");
    check(borrowed_connections.load() == 0 && peak_borrowed_connections.load() == 1,
          "preparation leaked a connection or borrowed before returning the previous one");
    check(inserts.load() == 0,
          "empty-list preparation unexpectedly wrote metadata");
    check(startup_object_deletes.load() == (resume ? 1 : 0),
          "startup recovery performed an unexpected catalog deletion");
    if (has_disk_object) {
      check(std::filesystem::exists(directory + "/" + abc_hash) == !resume,
            "startup recovery removed or retained the wrong disk object");
      check(startup_deleting_record.load() == (has_record && !resume),
            "startup recovery changed the wrong catalog state");
      check(upload_commits.load() == (resume ? 2 : unknown ? 1 : 0),
            "startup recovery committed or retried unexpectedly");
    }
    if (orphan) {
      check(startup_registered_record.load() == !backfill_failure,
            "startup ignored an orphan registration result");
      if (!backfill_failure) {
        check(startup_registered_state == (orphan_corrupt || orphan_existing ? "UNAVAILABLE" : "LIVE"),
              "startup changed the wrong orphan or existing object state");
      }
      check(upload_commits.load() == 0 && startup_object_deletes.load() == 0,
            "startup immediately reclaimed a newly registered orphan");
    }
    check(!service._started,
          "storage preparation unexpectedly started the service");
    return;
  }
  if (scenario == "metrics_pool") {
    BASE::ThreadPool pool("metrics-regression");
    pool.set_max_queue_size(1);
    pool.set_max_wait_queue_size(1);
    pool.start(1);
    std::promise<void> entered, release, expired;
    auto entered_future = entered.get_future();
    auto release_future = release.get_future().share();
    auto expired_future = expired.get_future();
    pool.try_run([&] {
      entered.set_value();
      release_future.wait();
    });
    const bool entered_ok =
        entered_future.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
    const bool queued = pool.try_run([] {
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    const bool waiting = pool.submit_async(
        [] {
        },
        deadline,
        [&] {
          expired.set_value();
        });
    const auto initial = pool.snapshot();
    const bool rejected = !pool.submit_async(
        [] {
        },
        deadline, {});
    const bool expired_ok =
        expired_future.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
    const auto final = pool.snapshot();
    release.set_value();
    pool.stop();
    check(entered_ok && queued && waiting && rejected && expired_ok,
          "pool metrics regression did not exercise queue, waiter and timeout boundaries");
    check(initial.queued == 1 && initial.waiting == 1 && final.waiting == 0 &&
              final.rejected == 1 && final.timeouts == 1,
          "pool snapshot lost queue pressure, rejection or an expired waiter");
    return;
  }
  if (scenario == "metrics_storage") {
    using OBSERVABILITY::Metric;
    auto metrics = std::make_shared<OBSERVABILITY::StorageMetrics>();
    ObjectStorageEngine objects;
    objects.set_metrics(metrics);
    std::ofstream(directory + "/" + abc_hash, std::ios::binary) << "bad";
    auto writer = objects.create_temporary(3);
    writer->append("abc", 3);
    bool publish_failed = false, open_failed = false;
    try {
      objects.publish(*writer);
    } catch (...) {
      publish_failed = true;
    }
    try {
      objects.open_read("invalid");
    } catch (...) {
      open_failed = true;
    }
    check(publish_failed && open_failed && metrics->get(Metric::object_publish_failed) == 1 &&
              metrics->get(Metric::verification_failed) == 1 &&
              metrics->get(Metric::object_open_failed) == 1 &&
              metrics->get(Metric::object_published) == 0,
          "object failure counters missed corrupt content or counted failed publish as success");
    writer.reset();
    std::filesystem::remove(directory + "/" + abc_hash);
    writer = objects.create_temporary(3);
    writer->append("abc", 3);
    objects.publish(*writer);
    check(metrics->get(Metric::object_published) == 1 && objects.open_read(abc_hash)->size == 3,
          "successful object publication did not update its counter");
    return;
  }
  if (scenario == "source_type") {
    bool rejected = false;
    try {
      Transfer::open_source(directory);
    } catch (const std::exception&) {
      rejected = true;
    }
    check(rejected, "directory accepted as an upload source");
    return;
  }
  if (scenario == "source_fifo") {
    const std::string path = directory + "/fifo";
    check(::mkfifo(path.c_str(), 0600) == 0, "mkfifo failed");
    ::alarm(2);
    bool rejected = false;
    try {
      Transfer::open_source(path);
    } catch (const std::exception&) {
      rejected = true;
    }
    ::alarm(0);
    check(rejected, "FIFO accepted as an upload source");
    return;
  }
  if (scenario == "commit_hash") {
    StorageEngine engine(*BASE::DbConnectionPool::get_connection_pool());
    check(engine.commit_upload("test_user", "42", "/", "file.bin", abc_hash) ==
              UploadCommitResult::committed,
          "valid SHA-256 rejected before INSERT");
    check(inserts == 1, "valid metadata did not reach INSERT");
    check(engine.commit_upload("test_user", "42", "/", "bad.bin", "bad") ==
              UploadCommitResult::failed,
          "invalid SHA-256 accepted");
    check(inserts == 1, "invalid metadata reached INSERT");
    return;
  }

  if (scenario == "shutdown_wait") {
    check_shutdown_waits(directory);
    return;
  }

  Fixture fixture(scenario == "metrics_auth" ? 1 : 0);
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
  if (scenario == "metrics_auth") {
    using OBSERVABILITY::Metric;
    for (int status = 0; status < 3; ++status) {
      auth_status.store(status);
      fixture.session->pending_username = "test_user";
      fixture.session->login_state = LoginState::k_await_password;
      fixture.service.handle_auth_command(fixture.conn, fixture.session, "password");
      // Invalidate only the completion; backend work must still be observed.
      ++fixture.session->auth_attempt;
      const auto metric = status == 0   ? Metric::login_success
                          : status == 1 ? Metric::login_invalid
                                        : Metric::auth_backend_failures;
      fixture.until([&] {
        return fixture.metrics->get(metric) == 1;
      });
      fixture.drain();
      check(fixture.session->login_state == LoginState::k_verifying,
            "stale authentication completion modified the session");
    }
    for (int status : {0, 3}) {
      signup_status.store(status);
      fixture.session->pending_username = "test_user";
      fixture.session->signup_password = "password";
      fixture.session->login_state = LoginState::k_signup_confirm;
      fixture.service.handle_signup_command(fixture.conn, fixture.session, "password");
      ++fixture.session->auth_attempt;
      const auto metric = status == 0 ? Metric::signup_success : Metric::signup_failed;
      fixture.until([&] {
        return fixture.metrics->get(metric) == 1;
      });
      fixture.drain();
    }
    check(fixture.metrics->get(Metric::auth_backend_failures) == 1,
          "credential and signup rejection incorrectly counted backend failures");
  } else if (scenario == "metrics_snapshots") {
    using OBSERVABILITY::Metric;
    fixture.service.on_connection(fixture.conn);
    check(fixture.metrics->get(Metric::connections) == 1,
          "duplicate connection callback incremented the gauge");
    fixture.service._metadata_service.find_download_hash("test_user", "/", "file.bin");
    fixture.service._metadata_service.find_download_hash("test_user", "/", "file.bin");
    fixture.service.sample_metrics();
    check(fixture.metrics->get(Metric::cache_requests) == 2 &&
              fixture.metrics->get(Metric::cache_hits) == 1 &&
              fixture.metrics->get(Metric::cache_loads) == 1,
          "cache counters were not sampled from the shared cache");
    fixture.service._th_pool.stop();
    fixture.service.handle_ls(fixture.conn, fixture.session);
    fixture.service._disk_executor.stop();
    check(!fixture.service._disk_executor.post(
              [] {
              },
              false),
          "stopped disk executor accepted work");
    fixture.service.sample_metrics();
    check(fixture.metrics->get(Metric::worker_rejected) == 1 &&
              fixture.metrics->get(Metric::commands_rejected) == 1 &&
              fixture.metrics->get(Metric::disk_rejected) == 1 &&
              fixture.metrics->get(Metric::disk_active) == 0,
          "resource snapshots lost queue rejections or active transfers");
    fixture.conn->force_close();
    fixture.until([&] {
      return fixture.conn->disconnected();
    });
    fixture.service.on_connection(fixture.conn);
    check(fixture.metrics->get(Metric::connections) == 0,
          "duplicate disconnect callback underflowed the gauge");
  } else if (scenario == "metrics_stale_commit") {
    using OBSERVABILITY::Metric;
    CommitRelease unblock_on_exit;
    {
      std::lock_guard<std::mutex> lock(commit_gate.mutex);
      commit_gate.enabled = true;
    }
    fixture.receive("@TLSS UPLOAD 3 \"file.bin\"\n");
    fixture.await_wire("\n@TLSS UREADY\n");
    fixture.receive("abc");
    fixture.until([&] {
      std::lock_guard<std::mutex> lock(commit_gate.mutex);
      return commit_gate.entered;
    });
    fixture.conn->force_close();
    fixture.until([&] {
      return fixture.conn->disconnected();
    });
    fixture.session->transfer->cancel();
    fixture.session->transfer.reset();
    commit_gate.release();
    fixture.service._disk_executor.stop();
    fixture.drain();
    check(fixture.metrics->get(Metric::upload_committed) == 1 &&
              fixture.metrics->get(Metric::upload_cancelled) == 1 &&
              fixture.metrics->get(Metric::upload_active) == 0 &&
              fixture.metrics->get(Metric::upload_failed) == 0,
          "disconnected task lost durable commit or counted a terminal outcome twice");
    check(fixture.read_wire().find("UOK") == std::string::npos,
          "stale committed upload emitted a success callback");
  } else if (scenario == "upload") {
    fixture.receive("@TLSS UPLOAD 3 \"file.bin\"\n");
    check(fixture.await_wire("\n@TLSS UREADY\n").find("\n@TLSS UREADY\n") != std::string::npos,
          "successful upload preparation did not produce UREADY");
    fixture.receive("abc");
    check(fixture.await_wire("\n@TLSS UOK " + abc_hash + "\n")
                  .find("\n@TLSS UOK " + abc_hash + "\n") != std::string::npos,
          "completed upload did not produce UOK");
    check(inserts == 1, "upload did not register metadata");
    check(fixture.metrics->get(OBSERVABILITY::Metric::upload_started) == 1 &&
              fixture.metrics->get(OBSERVABILITY::Metric::upload_active) == 0 &&
              fixture.metrics->get(OBSERVABILITY::Metric::upload_committed) == 1 &&
              fixture.metrics->get(OBSERVABILITY::Metric::upload_cancelled) == 0,
          "upload metrics did not record exactly one completed transfer");
    std::ifstream stored(directory + "/" + abc_hash, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(stored)), {});
    check(bytes == "abc", "published object content differs");
  } else if (scenario == "metadata_unknown") {
    // 验证三态结果经过缓存包装和真实上传回调后，仍不会误报成功。
    upload_outcome_unknown.store(true);
    upload_commits.store(0);
    upload_rollbacks.store(0);

    fixture.receive("@TLSS UPLOAD 3 \"file.bin\"\n");
    fixture.await_wire("\n@TLSS UREADY\n");
    fixture.receive("abc");
    const auto response = fixture.await_wire("upload commit outcome unknown");

    check(response.find("UOK") == std::string::npos && !fixture.session->transfer,
          "unknown commit outcome completed the upload");
    check(std::filesystem::exists(directory + "/" + abc_hash),
          "unknown commit outcome deleted the object");
    check(inserts == 1 && upload_commits.load() == 1,
          "unknown commit outcome retried registration");
    check(upload_rollbacks.load() == 0,
          "unknown commit outcome attempted rollback");
    check(fixture.metrics->get(OBSERVABILITY::Metric::upload_committed) == 0 &&
              fixture.metrics->get(OBSERVABILITY::Metric::upload_active) == 0,
          "unknown commit outcome was counted as committed or remained active");
  } else if (scenario == "metadata_failure") {
    fail_metadata_insert.store(true);
    fixture.receive("@TLSS UPLOAD 3 \"file.bin\"\n");
    fixture.await_wire("\n@TLSS UREADY\n");
    fixture.receive("abc");
    const auto response = fixture.await_wire("metadata registration failed");
    check(response.find("UOK") == std::string::npos && !fixture.session->transfer,
          "failed metadata registration completed the upload");
    check(std::filesystem::exists(directory + "/" + abc_hash),
          "failed metadata registration deleted the shared object");
    check(inserts == 1, "metadata registration was skipped or retried");
    check(fixture.metrics->get(OBSERVABILITY::Metric::upload_committed) == 0 &&
              fixture.metrics->get(OBSERVABILITY::Metric::upload_failed) == 1 &&
              fixture.metrics->get(OBSERVABILITY::Metric::upload_active) == 0 &&
              fixture.metrics->get(OBSERVABILITY::Metric::upload_cancelled) == 0 &&
              fixture.metrics->get(OBSERVABILITY::Metric::metadata_writes_failed) == 1,
          "failed metadata commit was counted as success, cancellation or twice");
  } else if (scenario == "stale_prepare") {
    fixture.service.handle_upload(fixture.conn, fixture.session, "3 \"old.bin\"");
    const auto old = fixture.session->transfer;
    ++fixture.session->transfer_generation;
    fixture.drain();
    check(fixture.read_wire().find("UREADY") == std::string::npos,
          "stale preparation started a receiver");
    check(old->phase() == TransferPhase::preparing_upload, "stale callback advanced the old task");
    old->cancel();
    fixture.session->transfer.reset();
    fixture.receive("@TLSS UPLOAD 3 \"new.bin\"\n");
    fixture.await_wire("\n@TLSS UREADY\n");
    fixture.receive("abc");
    fixture.await_wire("\n@TLSS UOK " + abc_hash + "\n");
    check(inserts == 1, "stale preparation registered a file");
    old->cancel();
    check(fixture.metrics->get(OBSERVABILITY::Metric::upload_started) == 2 &&
              fixture.metrics->get(OBSERVABILITY::Metric::upload_cancelled) == 1 &&
              fixture.metrics->get(OBSERVABILITY::Metric::upload_committed) == 1 &&
              fixture.metrics->get(OBSERVABILITY::Metric::upload_active) == 0,
          "stale upload cancellation did not close its metrics exactly once");
  } else if (scenario == "upload_cancel") {
    fixture.receive("@TLSS UPLOAD 3 \"file.bin\"\n");
    fixture.await_wire("\n@TLSS UREADY\n");
    fixture.conn->force_close();
    fixture.until([&] {
      return fixture.conn->disconnected();
    });
    fixture.service._disk_executor.stop();
    fixture.drain();
    check(inserts == 0, "cancelled upload registered metadata");
    check(fixture.metrics->get(OBSERVABILITY::Metric::upload_cancelled) == 1 &&
              fixture.metrics->get(OBSERVABILITY::Metric::upload_active) == 0 &&
              fixture.metrics->get(OBSERVABILITY::Metric::connections) == 0,
          "disconnect failed to record cancellation and connection removal");
    check(std::filesystem::is_empty(directory), "cancelled upload leaked temporary content");
    check(fixture.read_wire().find("UOK") == std::string::npos,
          "cancelled upload returned success");
  } else if (scenario == "task_isolation") {
    int sockets[2];
    check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) == 0,
          "second socketpair failed");
    auto second = std::make_shared<TcpConnection>(&fixture.loop, "second", sockets[0],
                                                  InetAddress(), InetAddress());
    second->set_connection_cb([&](const TcpConnectionPtr& connection) {
      fixture.service.on_connection(connection);
    });
    second->set_close_cb([&](const TcpConnectionPtr& connection) {
      fixture.loop.queue_in_loop([connection] {
        connection->connect_destroyed();
      });
    });
    second->connect_established();
    auto session = fixture.service._sessions.at(second.get());
    session->login_state = LoginState::k_authenticated;
    session->name = "second_user";
    session->user_id = 2;
    fixture.read_wire(sockets[1]);
    fixture.service.handle_upload(fixture.conn, fixture.session, "3 \"first.bin\"");
    fixture.service.handle_upload(second, session, "3 \"second.bin\"");
    std::string second_wire;
    fixture.until([&] {
      second_wire += fixture.read_wire(sockets[1]);
      return second_wire.find("UREADY") != std::string::npos;
    });
    fixture.await_wire("UREADY");
    Buffer body;
    body.append("abc", 3);
    fixture.service.on_message(second, &body, TIME::Timestamp());
    fixture.until([&] {
      second_wire += fixture.read_wire(sockets[1]);
      return second_wire.find("UOK") != std::string::npos;
    });
    check(!session->transfer && fixture.session->transfer &&
              fixture.session->transfer->phase() == TransferPhase::receiving_upload,
          "one connection's completion changed another connection's task");
    check(inserts == 1, "second upload did not register exactly once");
    second->force_close();
    fixture.until([&] {
      return second->disconnected();
    });
    second->set_connection_cb({});
    second->set_close_cb({});
    second->connect_destroyed();
    second.reset();
    ::close(sockets[1]);
  } else if (scenario == "download") {
    std::ofstream(directory + "/" + abc_hash, std::ios::binary) << "abc";
    fixture.receive("@TLSS DOWNLOAD \"file.bin\"\n");
    const std::string response = fixture.await_wire("\n@TLSS DONE\n");
    check(response.find("\n@TLSS DATA 3 " + abc_hash + "\nabc\n@TLSS DONE\n") != std::string::npos,
          "download header, body or DONE is missing or out of order");
    check(fixture.metrics->get(OBSERVABILITY::Metric::download_completed) == 0 &&
              fixture.metrics->get(OBSERVABILITY::Metric::download_active) == 1,
          "download completed before client ACK");
    fixture.receive("@TLSS ACK\n");
    check(fixture.await_wire("\n@TLSS OK\n").find("\n@TLSS OK\n") != std::string::npos,
          "download ACK did not produce OK");
    check(!fixture.session->transfer, "download did not return to idle");
    check(fixture.metrics->get(OBSERVABILITY::Metric::download_completed) == 1 &&
              fixture.metrics->get(OBSERVABILITY::Metric::download_active) == 0 &&
              fixture.metrics->get(OBSERVABILITY::Metric::download_cancelled) == 0,
          "download ACK metrics were missing or destructor counted cancellation");
  } else if (scenario == "error_frame") {
    fixture.service.transfer_failed(fixture.conn, fixture.session, "not available");
    check(fixture.await_wire("\n@TLSS ERROR \"not available\"\n")
                  .find("\n@TLSS ERROR \"not available\"\n") != std::string::npos,
          "failure response is not a parseable ERROR frame");
  } else if (scenario == "busy_reply") {
    fixture.session->directory_change_pending = true;
    fixture.service.handle_upload(fixture.conn, fixture.session, "3 \"file.bin\"");
    check(fixture.await_wire("\n@TLSS ERROR \"session is busy\"\n")
                  .find("\n@TLSS ERROR \"session is busy\"\n") != std::string::npos,
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
    auto task = fixture.service._upload_engine.create(
        fixture.service.transfer_context(fixture.conn, fixture.session), {3, "broken.bin"},
        [](TransferEvent) {
        });
    fixture.session->transfer = task;
    task->_writer = Transfer::AsyncReceiver::create(
        fixture.service._disk_executor, fixture.conn, 3, std::move(factory),
        [](Transfer::Receiver&) {
          return std::string{};
        },
        [&] {
          task->_phase = TransferPhase::receiving_upload;
          ready = true;
        },
        [](std::string) {
        },
        [&](std::string) {
          failed = true;
          fixture.conn->force_close();
        });
    fixture.until([&] {
      return ready;
    });
    fixture.receive("abc");
    fixture.until([&] {
      return fixture.conn->disconnected();
    });
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
    fixture.conn->set_message_cb(
        [&](const TcpConnectionPtr& conn, Buffer* buffer, TIME::Timestamp time) {
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
    fixture.until([&] {
      return client._transfer_state == Client::FileTransferState::idle;
    });
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
}  // namespace

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
