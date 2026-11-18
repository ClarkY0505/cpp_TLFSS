#include "common/base/mysql_connection.h"
#include "ftp/metadata_service.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace TLSS::BASE;
using namespace TLSS::FileStorage;
using namespace std::chrono_literals;

void check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

struct Directory {
  std::string id, username, path;
};
struct File {
  std::string id, username, parent, name, hash;
  std::string path{};
};
struct Transaction {
  int queries = 0;
  std::string deleted_id;
  std::string deleted_directory_id;
  std::optional<Directory> created_directory;
  // 递归删除仅暂存 ID，COMMIT 才统一改变已提交数据，回滚直接丢弃。
  std::vector<std::string> deleted_tree_ids;
  std::optional<File> updated_file;  // 重命名或移动后的事务工作副本。
};

// 数据库替代层只模拟元数据及事务提交；不连接真实 MySQL，不创建对象文件。
// 缓存、MetadataService、文件重命名与删除、目录创建及删除均执行实际实现。
struct Database {
  std::mutex mutex;
  std::vector<Directory> directories{
      {"12", "alice", "/docs"}, {"13", "alice", "/other"}, {"22", "bob", "/docs"}};
  std::vector<File> files{{"128", "alice", "12", "report.txt", std::string(64, 'a')},
                          {"129", "alice", "12", "another.txt", std::string(64, 'b')},
                          {"130", "alice", "13", "report.txt", std::string(64, 'c')},
                          {"131", "bob", "22", "report.txt", std::string(64, 'd')}};
  std::map<DbConnection*, Transaction> transactions;
  std::atomic<int> borrows{0}, active_connections{0}, queries{0}, deletes{0}, inserts{0};
  std::atomic<int> begins{0}, commits{0}, rollbacks{0};
  bool borrow_fails = false, borrow_throws = false, begin_fails = false;
  int query_fails_at = 0, query_throws_at = 0;
  bool file_missing = false, delete_throws = false;
  DbExecuteResult delete_reply{true, 0, 1, 0};
  DbExecuteResult insert_reply{true, 0, 1, 500};
  bool insert_throws = false;
  DbCommitStatus commit_reply = DbCommitStatus::k_committed;
  bool unknown_applies = false;
  bool directory_deletion = false;
  bool recursive_deletion = false, tree_nonstandard_exception = false;
  int tree_delete_fails_at = 0, tree_delete_throws_at = 0;
  bool rename_mode = false, move_mode = false;
  std::atomic<int> updates{0};
  DbExecuteResult update_reply{true, 0, 1, 0};
  int update_exception = 0;  // 1 标准异常，2 非标准异常，均发生在事务暂存之后。
  // 按完整路径查询目录：仅替换数据库返回边界，校验逻辑仍运行真实服务方法。
  std::optional<DbQueryResult> directory_lookup_reply;
  int directory_lookup_exception = 0;  // 1 标准异常，2 非标准异常。
} database;

// 在 DELETE 已执行、COMMIT 尚未执行时阻塞，确定性验证缓存协调锁的范围。
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  bool enabled = false, entered = false, released = false;
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
} gate;

std::string value(const std::vector<DbValue>& params, std::size_t index) {
  return params.at(index).value();
}

const Directory* find_directory(const std::string& username, const std::string& path) {
  for (const auto& directory : database.directories) {
    if (directory.username == username && directory.path == path)
      return &directory;
  }
  return nullptr;
}

}  // namespace

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
  ++database.borrows;
  if (database.borrow_throws)
    throw std::runtime_error("injected connection exception");
  if (database.borrow_fails)
    return {nullptr, DbConnectionDeleter{}};
  auto* connection = new DbConnection;
  ++database.active_connections;
  return {connection, DbConnectionDeleter{}};
}
void DbConnectionDeleter::operator()(DbConnection* connection) const noexcept {
  if (connection)
    --database.active_connections;
  delete connection;
}
DbConnection::DbConnection()
    : _conn(nullptr)
    , _connected(false)
    , _transaction_active(false) {
  std::lock_guard<std::mutex> lock(database.mutex);
  database.transactions.emplace(this, Transaction{});
}
DbConnection::~DbConnection() {
  std::lock_guard<std::mutex> lock(database.mutex);
  database.transactions.erase(this);
}

DbQueryResult DbConnection::query_prepared(const std::string& sql,
                                           const std::vector<DbValue>& params) {
  ++database.queries;
  std::lock_guard<std::mutex> lock(database.mutex);
  if (sql.find("FOR UPDATE") != std::string::npos) {
    check(_transaction_active, "locking query ran outside the deletion transaction");
    auto& transaction = database.transactions.at(this);
    const int stage = ++transaction.queries;
    if (stage == database.query_throws_at)
      throw std::runtime_error("injected query exception");
    if (stage == database.query_fails_at)
      return {};
    if (stage == 1) {
      check(sql.find("FROM users") != std::string::npos, "user was not locked first");
      return {true, {{"7"}}};
    }
    if (stage == 2 || (database.move_mode && stage == 3)) {
      if (database.move_mode)
        check(sql.find("CAST(path AS BINARY) = CAST(? AS BINARY)") != std::string::npos &&
                  sql.find("file_type = 'directory'") != std::string::npos,
              "move directory lookup omitted exact path or directory type");
      const auto* directory = find_directory(value(params, 0), value(params, 1));
      return directory ? DbQueryResult{true, {{directory->id}}} : DbQueryResult{true, {}};
    }
    if (database.rename_mode || database.move_mode) {
      const int source_stage = database.move_mode ? 4 : 3;
      if (stage == source_stage) {
        check(sql.find("file_type = 'file'") != std::string::npos &&
                  sql.find("CAST(path AS BINARY) = CAST(? AS BINARY)") != std::string::npos,
              "rename source query omitted type or exact path guards");
        if (!database.file_missing) {
          for (const auto& file : database.files)
            if (file.username == value(params, 0) && file.parent == value(params, 1) &&
                file.name == value(params, 2) && file.path == value(params, 3))
              return {true, {{file.id}}};
        }
        return {true, {}};
      }
      check(stage == source_stage + 1 && sql.find("id <> ?") != std::string::npos &&
                sql.find("file_type") == std::string::npos,
            "rename conflict query omitted source exclusion or excluded directory conflicts");
      for (const auto& file : database.files)
        if (file.username == value(params, 0) && file.parent == value(params, 1) &&
            file.name == value(params, 2) && file.id != value(params, 3))
          return {true, {{file.id}}};
      for (const auto& directory : database.directories) {
        if (directory.username != value(params, 0) || directory.path == "/")
          continue;
        const auto split = directory.path.rfind('/');
        const auto parent_path = split == 0 ? "/" : directory.path.substr(0, split);
        const auto* parent = find_directory(directory.username, parent_path);
        if (parent && parent->id == value(params, 1) &&
            directory.path.substr(split + 1) == value(params, 2))
          return {true, {{directory.id}}};
      }
      return {true, {}};
    }
    if (database.directory_deletion || database.recursive_deletion) {
      if (stage == 3) {
        check(sql.find("CAST(path AS BINARY) = CAST(? AS BINARY)") != std::string::npos,
              "directory target lookup omitted byte-exact path comparison");
        const auto* target = find_directory(value(params, 0), value(params, 3));
        const auto separator = value(params, 3).rfind('/');
        const auto parent_path = separator == 0 ? "/" : value(params, 3).substr(0, separator);
        const auto* parent = find_directory(value(params, 0), parent_path);
        if (target && parent && parent->id == value(params, 1) &&
            value(params, 3).substr(separator + 1) == value(params, 2))
          return {true, {{target->id}}};
        return {true, {}};
      }
      if (database.recursive_deletion) {
        check(sql.find("SELECT id, file_name, path, file_type") == 0,
              "recursive deletion omitted complete child identities");
        DbQueryResult result{true, {}};
        const auto* parent = static_cast<const Directory*>(nullptr);
        for (const auto& directory : database.directories) {
          if (directory.username == value(params, 0) && directory.id == value(params, 1))
            parent = &directory;
        }
        check(parent != nullptr, "recursive deletion expanded a missing directory");
        for (const auto& file : database.files) {
          if (file.username == parent->username && file.parent == parent->id)
            result.rows.push_back({file.id, file.name, parent->path + "/" + file.name, "file"});
        }
        for (const auto& child : database.directories) {
          const auto separator = child.path.rfind('/');
          const auto parent_path = separator == 0 ? "/" : child.path.substr(0, separator);
          if (child.username == parent->username && child.path != "/" &&
              parent_path == parent->path)
            result.rows.push_back(
                {child.id, child.path.substr(separator + 1), child.path, "directory"});
        }
        return result;
      }
      check(stage == 4 && sql.find("file_type") == std::string::npos,
            "directory emptiness check was missing or excluded a child type");
      for (const auto& file : database.files) {
        if (file.username == value(params, 0) && file.parent == value(params, 1))
          return {true, {{file.id}}};
      }
      for (const auto& target : database.directories) {
        if (target.username != value(params, 0) || target.id != value(params, 1))
          continue;
        for (const auto& child : database.directories) {
          const auto separator = child.path.rfind('/');
          const auto parent_path = separator == 0 ? "/" : child.path.substr(0, separator);
          if (child.username == target.username && child.path != "/" && parent_path == target.path)
            return {true, {{child.id}}};
        }
      }
      return {true, {}};
    }
    check(stage == 3, "deletion transaction unexpectedly queried again");
    if (!database.file_missing) {
      for (const auto& file : database.files) {
        if (file.username == value(params, 0) && file.parent == value(params, 1) &&
            file.name == value(params, 2))
          return {true, {{file.id, file.hash}}};
      }
    }
    return {true, {}};
  }

  if (sql.find("SELECT path FROM virtual_files ") == 0) {
    check(!_transaction_active && params.size() == 2 && params[0] && params[1] &&
              sql.find("WHERE username = ?") != std::string::npos &&
              sql.find("CAST(path AS BINARY) = CAST(? AS BINARY)") != std::string::npos &&
              sql.find("file_type = 'directory'") != std::string::npos,
          "directory lookup omitted user, exact path or directory type guards");
    if (database.directory_lookup_exception == 1)
      throw std::runtime_error("injected directory lookup exception");
    if (database.directory_lookup_exception == 2)
      throw 42;
    if (database.directory_lookup_reply)
      return *database.directory_lookup_reply;
    const auto* directory = find_directory(value(params, 0), value(params, 1));
    return directory ? DbQueryResult{true, {{directory->path}}} : DbQueryResult{true, {}};
  }

  // 目录列表和下载哈希查询读取已提交数据，不暴露 DELETE 的事务内变化。
  if (sql.find("SELECT id FROM virtual_files") == 0) {
    const auto* directory = find_directory(value(params, 0), value(params, 1));
    return directory ? DbQueryResult{true, {{directory->id}}} : DbQueryResult{true, {}};
  }
  if (sql.find("select file_name") == 0) {
    DbQueryResult result{true, {}};
    for (const auto& file : database.files) {
      if (file.username == value(params, 0) && file.parent == value(params, 1))
        result.rows.push_back({file.name, "file", "created", "updated"});
    }
    for (const auto& parent : database.directories) {
      if (parent.username != value(params, 0) || parent.id != value(params, 1))
        continue;
      for (const auto& child : database.directories) {
        const auto separator = child.path.rfind('/');
        const auto parent_path = separator == 0 ? "/" : child.path.substr(0, separator);
        if (child.username == parent.username && child.path != "/" && parent_path == parent.path)
          result.rows.push_back(
              {child.path.substr(separator + 1), "directory", "created", "updated"});
      }
    }
    return result;
  }
  if (sql.find("SELECT f.object_hash") == 0) {
    const auto* directory = find_directory(value(params, 0), value(params, 1));
    if (directory) {
      for (const auto& file : database.files) {
        if (file.username == directory->username && file.parent == directory->id &&
            file.name == value(params, 2))
          return {true, {{file.hash}}};
      }
    }
    return {true, {}};
  }
  throw std::runtime_error("unexpected metadata deletion query: " + sql);
}

// 同一翻译单元也包含 mkdir/上传实现。未测试的写入必须显式拒绝，避免误走其他路径。
bool DbConnection::update_prepared(const std::string&, const std::vector<DbValue>&) {
  throw std::runtime_error("unexpected nontransactional metadata write");
}
DbExecuteResult DbConnection::execute_prepared(const std::string& sql,
                                               const std::vector<DbValue>& params) {
  if (sql.find("UPDATE virtual_files SET file_name") == 0 ||
      sql.find("UPDATE virtual_files SET parent_id") == 0) {
    const bool moving = sql.find("UPDATE virtual_files SET parent_id") == 0;
    check((moving ? database.move_mode : database.rename_mode) && _transaction_active &&
              params.size() == 7 &&
              sql.find("CAST(path AS BINARY) = CAST(? AS BINARY)") != std::string::npos,
          "rename UPDATE ran without its transaction or full identity guards");
    ++database.updates;
    {
      std::lock_guard<std::mutex> lock(database.mutex);
      auto& transaction = database.transactions.at(this);
      check(transaction.queries == (moving ? 5 : 4), "metadata updated before checking its target");
      for (const auto& file : database.files) {
        if (file.id == value(params, 2) && file.username == value(params, 3) &&
            file.parent == value(params, 4) && file.name == value(params, 5) &&
            file.path == value(params, 6)) {
          auto updated = file;
          if (moving)
            updated.parent = value(params, 0);
          else
            updated.name = value(params, 0);
          updated.path = value(params, 1);
          transaction.updated_file = std::move(updated);
        }
      }
      check(transaction.updated_file.has_value(), "metadata UPDATE changed its source identity");
      if (database.update_exception == 1)
        throw std::runtime_error(moving ? "injected move update exception"
                                        : "injected rename update exception");
      if (database.update_exception == 2)
        throw 42;
    }
    gate.wait();
    return database.update_reply;
  }
  if (sql.find("INSERT INTO virtual_files") == 0) {
    check(_transaction_active, "mkdir INSERT ran outside its transaction");
    ++database.inserts;
    {
      std::lock_guard<std::mutex> lock(database.mutex);
      auto& transaction = database.transactions.at(this);
      check(transaction.queries == 2, "mkdir did not lock user before parent lookup");
      if (database.insert_throws)
        throw std::runtime_error("injected insert exception");
      for (const auto& file : database.files) {
        if (file.username == value(params, 1) && file.parent == value(params, 2) &&
            file.name == value(params, 0))
          return {false, 1062, 0, 0};
      }
      for (const auto& directory : database.directories) {
        if (directory.username == value(params, 1) && directory.path == value(params, 3))
          return {false, 1062, 0, 0};
      }
      if (database.insert_reply.success && database.insert_reply.affected_rows == 1)
        transaction.created_directory = Directory{"500", value(params, 1), value(params, 3)};
    }
    gate.wait();
    return database.insert_reply;
  }
  check(_transaction_active && sql.find("DELETE FROM virtual_files") == 0,
        "unexpected write or deletion outside the transaction");
  ++database.deletes;
  {
    std::lock_guard<std::mutex> lock(database.mutex);
    auto& transaction = database.transactions.at(this);
    if (database.recursive_deletion) {
      check(transaction.queries >= 4 && params.size() == 6 &&
                sql.find("CAST(path AS BINARY) = CAST(? AS BINARY)") != std::string::npos,
            "recursive DELETE omitted its transaction or exact identity guards");
      const int stage = database.deletes;
      if (stage == database.tree_delete_throws_at) {
        if (database.tree_nonstandard_exception)
          throw 42;
        throw std::runtime_error("injected recursive delete exception");
      }
      if (stage == database.tree_delete_fails_at)
        return database.delete_reply;
      const auto already_deleted = [&](const std::string& id) {
        return std::find(transaction.deleted_tree_ids.begin(), transaction.deleted_tree_ids.end(),
                         id) != transaction.deleted_tree_ids.end();
      };
      check(!already_deleted(value(params, 0)), "recursive DELETE retried a node");
      bool found = false;
      if (value(params, 5) == "file") {
        for (const auto& file : database.files) {
          if (file.id != value(params, 0) || file.username != value(params, 1) ||
              file.parent != value(params, 2) || file.name != value(params, 3))
            continue;
          for (const auto& parent : database.directories)
            if (parent.username == file.username && parent.id == file.parent &&
                parent.path + "/" + file.name == value(params, 4))
              found = true;
        }
      } else {
        const auto* directory = find_directory(value(params, 1), value(params, 4));
        if (directory && directory->id == value(params, 0) && directory->path != "/") {
          const auto separator = directory->path.rfind('/');
          const auto parent_path = separator == 0 ? "/" : directory->path.substr(0, separator);
          const auto* parent = find_directory(directory->username, parent_path);
          found = parent && parent->id == value(params, 2) &&
                  directory->path.substr(separator + 1) == value(params, 3);
          // 仍未暂存删除的直接子项不能留给父目录之后处理。
          for (const auto& file : database.files)
            check(file.username != directory->username || file.parent != directory->id ||
                      already_deleted(file.id),
                  "recursive deletion removed a parent before a file");
          for (const auto& child : database.directories) {
            const auto split = child.path.rfind('/');
            const auto child_parent = split == 0 ? "/" : child.path.substr(0, split);
            check(child.username != directory->username || child.path == "/" ||
                      child_parent != directory->path || already_deleted(child.id),
                  "recursive deletion removed a parent before a subdirectory");
          }
        }
      }
      check(found, "recursive DELETE changed a record identity or user");
      transaction.deleted_tree_ids.push_back(value(params, 0));
    } else {
      check(transaction.queries == (database.directory_deletion ? 4 : 3),
            "DELETE ran before all locking queries");
      if (database.delete_throws)
        throw std::runtime_error("injected delete exception");
      if (database.directory_deletion) {
        check(sql.find("CAST(path AS BINARY) = CAST(? AS BINARY)") != std::string::npos,
              "directory DELETE omitted byte-exact path comparison");
        check(params.size() == 5, "directory DELETE omitted its identity parameters");
        const auto& path = value(params, 4);
        const auto separator = path.rfind('/');
        const auto parent_path = separator == 0 ? "/" : path.substr(0, separator);
        const auto* target = find_directory(value(params, 1), path);
        const auto* parent = find_directory(value(params, 1), parent_path);
        check(target && parent && target->id == value(params, 0) && parent->id == value(params, 2) &&
                  path.substr(separator + 1) == value(params, 3),
              "directory DELETE changed its target or owner");
        if (database.delete_reply.success && database.delete_reply.affected_rows == 1)
          transaction.deleted_directory_id = value(params, 0);
      } else if (database.delete_reply.success && database.delete_reply.affected_rows == 1) {
        transaction.deleted_id = value(params, 0);
      }
    }
  }
  gate.wait();
  if (database.recursive_deletion)
    return {true, 0, 1, 0};
  return database.delete_reply;
}

DbTransaction::DbTransaction(DbConnection& connection) noexcept
    : connection_(connection)
    , active_(!database.begin_fails && !connection._transaction_active) {
  ++database.begins;
  if (active_)
    connection_._transaction_active = true;
}
DbTransaction::~DbTransaction() noexcept {
  rollback();
}
DbCommitStatus DbTransaction::commit() noexcept {
  if (!active_)
    return DbCommitStatus::k_not_active;
  ++database.commits;
  std::lock_guard<std::mutex> lock(database.mutex);
  auto& transaction = database.transactions.at(&connection_);
  if (database.commit_reply == DbCommitStatus::k_committed ||
      (database.commit_reply == DbCommitStatus::k_outcome_unknown && database.unknown_applies)) {
    auto& files = database.files;
    files.erase(std::remove_if(files.begin(), files.end(),
                               [&](const File& file) {
                                 return file.id == transaction.deleted_id ||
                                        std::find(transaction.deleted_tree_ids.begin(),
                                                  transaction.deleted_tree_ids.end(),
                                                  file.id) != transaction.deleted_tree_ids.end();
                               }),
                files.end());
    if (transaction.created_directory)
      database.directories.push_back(*transaction.created_directory);
    if (transaction.updated_file) {
      for (auto& file : files)
        if (file.id == transaction.updated_file->id)
          file = *transaction.updated_file;
    }
    auto& directories = database.directories;
    directories.erase(std::remove_if(directories.begin(), directories.end(),
                                     [&](const Directory& directory) {
                                       return directory.id == transaction.deleted_directory_id ||
                                              std::find(transaction.deleted_tree_ids.begin(),
                                                        transaction.deleted_tree_ids.end(),
                                                        directory.id) !=
                                                  transaction.deleted_tree_ids.end();
                                     }),
                      directories.end());
  }
  // 未知提交不再补发回滚；测试分别模拟远端实际提交和未提交。
  transaction.deleted_id.clear();
  transaction.deleted_directory_id.clear();
  transaction.created_directory.reset();
  transaction.deleted_tree_ids.clear();
  transaction.updated_file.reset();
  active_ = false;
  connection_._transaction_active = false;
  return database.commit_reply;
}
bool DbTransaction::rollback() noexcept {
  if (active_) {
    ++database.rollbacks;
    std::lock_guard<std::mutex> lock(database.mutex);
    database.transactions.at(&connection_).deleted_id.clear();
    database.transactions.at(&connection_).deleted_directory_id.clear();
    database.transactions.at(&connection_).created_directory.reset();
    database.transactions.at(&connection_).deleted_tree_ids.clear();
    database.transactions.at(&connection_).updated_file.reset();
    active_ = false;
    connection_._transaction_active = false;
  }
  return true;
}

}  // namespace TLSS::BASE

namespace {

void warm(MetadataService& service) {
  const auto list = service.find_file_by_current_dir("alice", "/docs");
  const auto download = service.find_download_hash("alice", "/docs", "report.txt");
  check(list.success && list.result.size() == 2 && download.success &&
            download.result == std::string(64, 'a'),
        "initial metadata lookup failed");
  const int before = database.queries;
  service.find_file_by_current_dir("alice", "/docs");
  service.find_download_hash("alice", "/docs", "report.txt");
  check(database.queries == before, "metadata was not cached before deletion");
}

void check_refreshed(MetadataService& service, bool deleted) {
  const int before = database.queries;
  const auto list = service.find_file_by_current_dir("alice", "/docs");
  const auto download = service.find_download_hash("alice", "/docs", "report.txt");
  check(database.queries == before + 3, "deletion retained a stale list or download cache entry");
  check(list.success && list.result.size() == (deleted ? 1U : 2U) && download.success == !deleted,
        "cache refresh did not reflect committed metadata");
  check(list.result[0].file_name == (deleted ? "another.txt" : "report.txt"),
        "refresh removed the wrong file");
  if (!deleted)
    check(download.result == std::string(64, 'a'), "retained file hash changed");
  const int refreshed = database.queries;
  service.find_file_by_current_dir("alice", "/docs");
  check(database.queries == refreshed, "refreshed list was not cached");
}

void check_idle() {
  check(database.active_connections == 0, "deletion or cache refresh leaked a database connection");
}

void test_outcome(MetadataService& service, const std::string& scenario) {
  warm(service);
  const bool unknown = scenario != "committed";
  const bool applied = scenario != "unknown_not_applied";
  if (unknown) {
    database.commit_reply = DbCommitStatus::k_outcome_unknown;
    database.unknown_applies = applied;
  }
  const auto result = service.delete_file("alice", "/docs", "report.txt");
  check(result == (unknown ? DeleteFileResult::outcome_unknown : DeleteFileResult::committed),
        "service lost the transaction outcome");
  check(database.deletes == 1 && database.commits == 1 && database.rollbacks == 0,
        "service retried deletion or rolled back an unknown commit");
  check_refreshed(service, applied);
  // 同目录其他文件、其他目录及其他用户仍有自己的元数据。
  check(
      service.find_download_hash("alice", "/docs", "another.txt").result == std::string(64, 'b') &&
          service.find_download_hash("alice", "/other", "report.txt").result ==
              std::string(64, 'c') &&
          service.find_download_hash("bob", "/docs", "report.txt").result == std::string(64, 'd'),
      "deletion changed another file or namespace");
  check_idle();
}

void test_failure(MetadataService& service, const std::string& scenario) {
  warm(service);
  if (scenario == "borrow_failure")
    database.borrow_fails = true;
  else if (scenario == "begin_failure")
    database.begin_fails = true;
  else if (scenario == "query_failure")
    database.query_fails_at = 2;
  else if (scenario == "missing_file")
    database.file_missing = true;
  else if (scenario == "delete_failure")
    database.delete_reply = {false, 1213, 0, 0};
  else if (scenario == "delete_zero")
    database.delete_reply = {true, 0, 0, 0};
  const auto result = service.delete_file("alice", "/docs", "report.txt");
  check(result == DeleteFileResult::failed && database.commits == 0,
        "failed deletion was committed or reported successful");
  check(database.rollbacks == (scenario == "borrow_failure" || scenario == "begin_failure" ? 0 : 1),
        "failed deletion did not end its transaction correctly");
  database.borrow_fails = false;
  database.begin_fails = false;
  database.query_fails_at = 0;
  database.file_missing = false;
  check_refreshed(service, false);
  check_idle();
}

void test_exception(MetadataService& service, const std::string& scenario) {
  warm(service);
  if (scenario == "borrow_exception")
    database.borrow_throws = true;
  else if (scenario == "query_exception")
    database.query_throws_at = 3;
  else
    database.delete_throws = true;
  bool propagated = false;
  try {
    service.delete_file("alice", "/docs", "report.txt");
  } catch (const std::runtime_error& error) {
    const std::string expected = scenario == "borrow_exception"  ? "injected connection exception"
                                 : scenario == "query_exception" ? "injected query exception"
                                                                 : "injected delete exception";
    propagated = error.what() == expected;
  }
  check(propagated && database.commits == 0, "deletion exception was swallowed or replaced");
  check(database.rollbacks == (scenario == "borrow_exception" ? 0 : 1),
        "exception did not roll back its active transaction");
  database.borrow_throws = false;
  database.query_throws_at = 0;
  database.delete_throws = false;
  check_refreshed(service, false);
  check_idle();
}

void test_invalid(MetadataService& service) {
  warm(service);
  const int before = database.borrows;
  check(service.delete_file("", "/docs", "report.txt") == DeleteFileResult::failed &&
            service.delete_file("alice", "", "report.txt") == DeleteFileResult::failed &&
            service.delete_file("alice", "/docs", "../report.txt") == DeleteFileResult::failed,
        "invalid deletion arguments were accepted");
  service.find_file_by_current_dir("alice", "/docs");
  service.find_download_hash("alice", "/docs", "report.txt");
  check(database.borrows == before && database.begins == 0,
        "invalid request borrowed a connection or invalidated valid cache entries");
}

// 非法重命名请求必须由公开服务入口拒绝，不能等事务函数借用连接后再拒绝。
// 同时检查新旧名称，并通过查询计数证明原有缓存未被无效请求清除。
std::uint64_t test_rename_invalid(MetadataService& service) {
  warm(service);
  std::vector<std::vector<std::string>> cases{{"", "/docs", "report.txt", "summary.txt"},
                                              {"alice", "", "report.txt", "summary.txt"}};
  for (const auto& name :
       std::vector<std::string>{"", ".", "..", "/", "a/b", "a\\b", std::string(256, 'x'),
                                std::string("a\0b", 3), "a\nb", std::string(1, '\x7f')}) {
    cases.push_back({"alice", "/docs", name, "summary.txt"});
    cases.push_back({"alice", "/docs", "report.txt", name});
  }
  // 合法路径分量拼成 1024 字节父目录，新旧文件完整路径均会超限。
  const std::string long_parent = "/" + std::string(255, 'a') + "/" + std::string(255, 'b') + "/" +
                                  std::string(255, 'c') + "/" + std::string(255, 'd');
  cases.push_back({"alice", long_parent, "report.txt", "summary.txt"});
  for (const auto& args : cases) {
    const int borrows = database.borrows;
    const int queries = database.queries;
    check(service.rename_file(args[0], args[1], args[2], args[3]) == RenameFileResult::failed,
          "invalid rename arguments were accepted");
    check(database.borrows == borrows && database.queries == queries && database.begins == 0,
          "invalid rename request reached the database; validate both old_name and new_name");
    service.find_file_by_current_dir("alice", "/docs");
    service.find_download_hash("alice", "/docs", "report.txt");
    check(database.borrows == borrows && database.queries == queries,
          "invalid rename request invalidated previously cached metadata");
  }
  return cases.size();
}

// 从公开服务入口调用真实重命名事务，检查结果、指标、对象引用和三个缓存银行的失效。
// 数据库只暂存元数据；并发用例通过实际等待队列确认读取被协调，而非依赖固定睡眠。
std::uint64_t test_rename(MetadataService& service, const std::shared_ptr<MetadataCache>& cache,
                          const std::string& mode) {
  database.rename_mode = true;
  database.directories.push_back({"50", "alice", "/docs/child"});
  for (auto& file : database.files) {
    for (const auto& directory : database.directories)
      if (directory.username == file.username && directory.id == file.parent)
        file.path = directory.path + "/" + file.name;
  }
  const bool disabled = mode == "disabled";
  const bool unchanged = mode == "unchanged" || mode == "unchanged_missing";
  const bool duplicate_file = mode == "duplicate_file";
  const bool duplicate_directory = mode == "duplicate_directory";
  const std::string new_name = unchanged ? "report.txt" : "summary.txt";
  if (duplicate_file)
    database.files.push_back(
        {"145", "alice", "12", new_name, std::string(64, 'f'), "/docs/summary.txt"});
  if (duplicate_directory)
    database.directories.push_back({"51", "alice", "/docs/summary.txt"});
  MetadataService peer(*DbConnectionPool::get_connection_pool(), cache);
  MetadataService& reader = mode == "shared" ? peer : service;
  auto warm_positive = [&] {
    check(reader.find_file_by_current_dir("alice", "/docs").success &&
              reader.find_download_hash("alice", "/docs", "report.txt").result ==
                  std::string(64, 'a') &&
              reader.move_dir_by_current_dir("alice", "/docs", "child").result == "/docs/child",
          "could not warm rename metadata");
    if (duplicate_file)
      check(reader.find_download_hash("alice", "/docs", new_name).result == std::string(64, 'f'),
            "could not warm the occupied destination hash");
  };
  warm_positive();
  if (!disabled) {
    const int queries = database.queries;
    warm_positive();
    check(database.queries == queries, "rename metadata was not cached before the operation");
  }

  auto expected = RenameFileResult::committed;
  bool applied = true, exception = false;
  if (mode == "unknown_applied" || mode == "unknown_not_applied") {
    database.commit_reply = DbCommitStatus::k_outcome_unknown;
    database.unknown_applies = mode == "unknown_applied";
    applied = database.unknown_applies;
    expected = RenameFileResult::outcome_unknown;
  } else if (mode == "borrow_exception" || mode == "query_exception" ||
             mode == "update_exception" || mode == "nonstandard_exception") {
    expected = RenameFileResult::failed;
    applied = false;
    exception = true;
    if (mode == "borrow_exception")
      database.borrow_throws = true;
    else if (mode == "query_exception")
      database.query_throws_at = 3;
    else
      database.update_exception = mode == "update_exception" ? 1 : 2;
  } else if (mode == "borrow_failure" || mode == "begin_failure" || mode == "query_failure" ||
             mode == "missing_source" || mode == "unchanged_missing" || duplicate_file ||
             duplicate_directory || mode == "update_failure" || mode == "update_zero" ||
             mode == "update_many") {
    expected = RenameFileResult::failed;
    applied = false;
    if (mode == "borrow_failure")
      database.borrow_fails = true;
    else if (mode == "begin_failure")
      database.begin_fails = true;
    else if (mode == "query_failure")
      database.query_fails_at = 4;
    else if (mode == "missing_source" || mode == "unchanged_missing")
      database.file_missing = true;
    else if (mode == "update_failure")
      database.update_reply = {false, 1062, 0, 0};
    else if (mode == "update_zero")
      database.update_reply = {true, 0, 0, 0};
    else if (mode == "update_many")
      database.update_reply = {true, 0, 2, 0};
  } else if (mode != "committed" && mode != "unchanged" && mode != "shared" && mode != "disabled" &&
             mode != "concurrent") {
    throw std::invalid_argument("unknown rename service scenario: " + mode);
  }
  if (mode == "concurrent") {
    gate.enabled = true;
    std::future<RenameFileResult> renaming;
    std::future<bool> reading;
    struct Release {
      ~Release() {
        gate.release();
      }
    } release;
    renaming = std::async(std::launch::async, [&] {
      return service.rename_file("alice", "/docs", "report.txt", new_name);
    });
    {
      std::unique_lock<std::mutex> lock(gate.mutex);
      check(gate.changed.wait_for(lock, 3s,
                                  [&] {
                                    return gate.entered;
                                  }),
            "rename did not reach its transactional UPDATE boundary");
    }
    reading = std::async(std::launch::async, [&] {
      const auto list = reader.find_file_by_current_dir("alice", "/docs");
      return list.success &&
             std::any_of(list.result.begin(), list.result.end(), [&](const FileInfo& info) {
               return info.file_name == new_name;
             });
    });
    bool queued = false;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
      {
        auto& coordinator = cache->_subtree_coordinator;
        std::lock_guard<std::mutex> lock(coordinator._mutex);
        queued = !coordinator._waiting_requests.empty();
      }
      if (queued || reading.wait_for(0ms) == std::future_status::ready)
        break;
      std::this_thread::yield();
    }
    check(queued, "cached reader bypassed an active same-directory rename");
    gate.release();
    check(renaming.get() == expected && reading.get(), "waiting reader observed a stale file name");
  } else {
    bool caught = false;
    try {
      check(service.rename_file("alice", "/docs", "report.txt", new_name) == expected && !exception,
            "rename service lost its transaction result");
    } catch (const std::runtime_error& error) {
      const std::string message = mode == "borrow_exception"  ? "injected connection exception"
                                  : mode == "query_exception" ? "injected query exception"
                                                              : "injected rename update exception";
      caught = exception && error.what() == message;
      if (!caught)
        throw;
    } catch (int code) {
      caught = mode == "nonstandard_exception" && code == 42;
      if (!caught)
        throw;
    }
    check(caught == exception, "rename service swallowed or replaced an exception");
  }
  database.borrow_fails = database.borrow_throws = database.begin_fails = database.file_missing =
      false;
  database.query_fails_at = database.query_throws_at = database.update_exception = 0;
  const bool committed_attempt = expected != RenameFileResult::failed;
  const bool never_active =
      mode == "borrow_failure" || mode == "borrow_exception" || mode == "begin_failure";
  check(database.commits == (committed_attempt ? 1 : 0) &&
            database.rollbacks == (!committed_attempt && !never_active ? 1 : 0),
        "rename service retried a transaction, rolled back an unknown commit or leaked a "
        "transaction");
  if (unchanged)
    check(database.updates == 0, "unchanged name issued an UPDATE or skipped source validation");

  const bool renamed = applied && !unchanged;
  const int before_refresh = database.queries;
  const auto list = reader.find_file_by_current_dir("alice", "/docs");
  const auto old_hash = reader.find_download_hash("alice", "/docs", "report.txt");
  const auto new_hash = reader.find_download_hash("alice", "/docs", new_name);
  const auto cd = reader.move_dir_by_current_dir("alice", "/docs", "child");
  check(list.success && old_hash.success == !renamed &&
            new_hash.success == (unchanged || renamed || duplicate_file) &&
            cd.result == "/docs/child",
        "rename retained stale list, download or cd metadata");
  const auto listed = [&](const std::string& name) {
    return std::any_of(list.result.begin(), list.result.end(), [&](const FileInfo& info) {
      return info.file_name == name;
    });
  };
  check(listed("report.txt") == !renamed &&
            listed(new_name) == (unchanged || renamed || duplicate_file || duplicate_directory),
        "rename refreshed listing contains the wrong source or destination name");
  if (new_hash.success)
    check(new_hash.result == std::string(64, duplicate_file ? 'f' : 'a'),
          "rename returned another object's hash");
  if (mode != "concurrent")
    check(database.queries == before_refresh + (unchanged ? 4 : 5),
          "rename did not invalidate every affected cache bank on operation exit");
  if (!disabled) {
    const int queries = database.queries;
    reader.find_file_by_current_dir("alice", "/docs");
    reader.find_download_hash("alice", "/docs", renamed ? new_name : "report.txt");
    reader.move_dir_by_current_dir("alice", "/docs", "child");
    check(database.queries == queries, "refreshed rename metadata was not cached");
  }
  {
    std::lock_guard<std::mutex> lock(database.mutex);
    const auto source =
        std::find_if(database.files.begin(), database.files.end(), [](const File& file) {
          return file.id == "128";
        });
    check(source != database.files.end() && source->name == (renamed ? new_name : "report.txt") &&
              source->path == "/docs/" + (renamed ? new_name : "report.txt") &&
              source->parent == "12" && source->hash == std::string(64, 'a'),
          "rename changed file identity, parent or content reference");
  }
  check(reader.find_download_hash("alice", "/other", "report.txt").result == std::string(64, 'c') &&
            reader.find_download_hash("bob", "/docs", "report.txt").result == std::string(64, 'd'),
        "rename affected another directory or another user");
  return expected == RenameFileResult::committed ? 0 : 1;
}

// 服务层移动测试同时观察两个目录的列表、下载哈希与 cd 缓存。
// 默认选不同目录分片，避免哈希碰撞掩盖只失效源目录的错误。
std::uint64_t test_move(MetadataService& service, const std::shared_ptr<MetadataCache>& cache,
                        const std::string& mode) {
  database.move_mode = true;
  const bool same = mode == "same_directory" || mode == "same_directory_missing";
  const bool disabled = mode == "disabled";
  const bool empty_target = mode == "empty_target";
  const bool duplicate_file = mode == "duplicate_file";
  const bool duplicate_directory = mode == "duplicate_directory";
  const std::string source = mode == "root_source" ? "/" : "/docs";
  const std::string source_id = source == "/" ? "1" : "12";
  std::string target;
  if (same)
    target = source;
  else if (mode == "root_target")
    target = "/";
  else {
    auto& source_gate = cache->gate_for(MetadataCache::make_key("alice", source));
    for (int index = 0; index < 4096; ++index) {
      const auto candidate = index == 0 ? "/archive" : "/archive_" + std::to_string(index);
      auto& target_gate = cache->gate_for(MetadataCache::make_key("alice", candidate));
      if ((&source_gate == &target_gate) == (mode == "gate_collision")) {
        target = candidate;
        break;
      }
    }
  }
  check(!target.empty(), "could not select a move cache fixture");
  const std::string target_id = same ? source_id : target == "/" ? "1" : "20";
  const auto child_path = [](const std::string& parent, const std::string& name) {
    return parent == "/" ? parent + name : parent + "/" + name;
  };
  if (source == "/" || target == "/")
    database.directories.push_back({"1", "alice", "/"});
  if (!same && target != "/")
    database.directories.push_back({"20", "alice", target});
  database.directories.push_back({"50", "alice", child_path(source, "child")});
  if (!same && !empty_target)
    database.directories.push_back({"51", "alice", child_path(target, "child")});
  for (auto& file : database.files) {
    if (file.id == "128")
      file.parent = source_id;
    for (const auto& directory : database.directories)
      if (directory.username == file.username && directory.id == file.parent)
        file.path = child_path(directory.path, file.name);
  }
  if (duplicate_file)
    database.files.push_back({"145", "alice", target_id, "report.txt", std::string(64, 'f'),
                              child_path(target, "report.txt")});
  if (duplicate_directory)
    database.directories.push_back({"52", "alice", child_path(target, "report.txt")});
  MetadataService peer(*DbConnectionPool::get_connection_pool(), cache);
  MetadataService& reader = mode == "shared" ? peer : service;
  auto warm_positive = [&] {
    check(reader.find_file_by_current_dir("alice", source).success &&
              reader.find_download_hash("alice", source, "report.txt").result ==
                  std::string(64, 'a') &&
              reader.move_dir_by_current_dir("alice", source, "child").result ==
                  child_path(source, "child"),
          "could not warm source move metadata");
    if (!same) {
      check(reader.find_file_by_current_dir("alice", target).success,
            "could not warm the target listing");
      if (!empty_target)
        check(reader.move_dir_by_current_dir("alice", target, "child").result ==
                  child_path(target, "child"),
              "could not warm target cd metadata");
      if (duplicate_file)
        check(
            reader.find_download_hash("alice", target, "report.txt").result == std::string(64, 'f'),
            "could not warm occupied target download metadata");
    }
  };
  warm_positive();
  if (!disabled) {
    const int queries = database.queries;
    warm_positive();
    check(database.queries == queries, "move metadata was not cached before the operation");
  }

  if (mode == "invalid_arguments") {
    std::vector<std::vector<std::string>> cases{{"", source, target, "report.txt"},
                                                {"alice", "", target, "report.txt"},
                                                {"alice", source, "", "report.txt"}};
    for (const auto& name :
         std::vector<std::string>{"", ".", "..", "/", "a/b", "a\\b", std::string(256, 'x'),
                                  std::string("a\0b", 3), "a\nb", std::string(1, '\x7f')})
      cases.push_back({"alice", source, target, name});
    const std::string long_parent = "/" + std::string(255, 'a') + "/" + std::string(255, 'b') +
                                    "/" + std::string(255, 'c') + "/" + std::string(255, 'd');
    cases.push_back({"alice", long_parent, target, "report.txt"});
    cases.push_back({"alice", source, long_parent, "report.txt"});
    for (const auto& args : cases) {
      const int borrows = database.borrows;
      const int queries = database.queries;
      check(service.move_file(args[0], args[1], args[2], args[3]) == MoveFileResult::failed &&
                database.borrows == borrows && database.begins == 0,
            "invalid move arguments reached the database");
      warm_positive();
      check(database.queries == queries && database.borrows == borrows,
            "invalid move arguments invalidated source or target metadata");
    }
    return cases.size();
  }

  auto expected = MoveFileResult::committed;
  bool applied = true, exception = false;
  if (mode == "unknown_applied" || mode == "unknown_not_applied") {
    database.commit_reply = DbCommitStatus::k_outcome_unknown;
    database.unknown_applies = mode == "unknown_applied";
    applied = database.unknown_applies;
    expected = MoveFileResult::outcome_unknown;
  } else if (mode == "borrow_exception" || mode == "query_exception" ||
             mode == "update_exception" || mode == "nonstandard_exception") {
    expected = MoveFileResult::failed;
    applied = false;
    exception = true;
    if (mode == "borrow_exception")
      database.borrow_throws = true;
    else if (mode == "query_exception")
      database.query_throws_at = 4;
    else
      database.update_exception = mode == "update_exception" ? 1 : 2;
  } else if (mode == "borrow_failure" || mode == "begin_failure" || mode == "query_failure" ||
             mode == "missing_source" || mode == "same_directory_missing" || duplicate_file ||
             duplicate_directory || mode == "update_failure" || mode == "update_zero" ||
             mode == "update_many") {
    expected = MoveFileResult::failed;
    applied = false;
    if (mode == "borrow_failure")
      database.borrow_fails = true;
    else if (mode == "begin_failure")
      database.begin_fails = true;
    else if (mode == "query_failure")
      database.query_fails_at = 3;
    else if (mode == "missing_source" || mode == "same_directory_missing")
      database.file_missing = true;
    else if (mode == "update_failure")
      database.update_reply = {false, 1062, 0, 0};
    else if (mode == "update_zero")
      database.update_reply = {true, 0, 0, 0};
    else if (mode == "update_many")
      database.update_reply = {true, 0, 2, 0};
  } else if (mode != "committed" && mode != "same_directory" && mode != "shared" &&
             mode != "disabled" && mode != "concurrent_source" && mode != "concurrent_target" &&
             mode != "root_source" && mode != "root_target" && mode != "gate_collision" &&
             mode != "empty_target") {
    throw std::invalid_argument("unknown move service scenario: " + mode);
  }
  const bool concurrent = mode == "concurrent_source" || mode == "concurrent_target";
  if (concurrent) {
    gate.enabled = true;
    std::future<MoveFileResult> moving;
    std::future<bool> reading;
    struct Release {
      ~Release() {
        gate.release();
      }
    } release;
    moving = std::async(std::launch::async, [&] {
      return service.move_file("alice", source, target, "report.txt");
    });
    {
      std::unique_lock<std::mutex> lock(gate.mutex);
      check(gate.changed.wait_for(lock, 3s,
                                  [&] {
                                    return gate.entered;
                                  }),
            "move did not reach its UPDATE boundary");
    }
    const std::string watched = mode == "concurrent_source" ? source : target;
    reading = std::async(std::launch::async, [&] {
      const auto list = reader.find_file_by_current_dir("alice", watched);
      const bool listed =
          std::any_of(list.result.begin(), list.result.end(), [](const FileInfo& info) {
            return info.file_name == "report.txt" && info.type == "file";
          });
      return list.success && listed == (watched == target);
    });
    bool queued = false;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
      {
        auto& coordinator = cache->_subtree_coordinator;
        std::lock_guard<std::mutex> lock(coordinator._mutex);
        queued = !coordinator._waiting_requests.empty();
      }
      if (queued || reading.wait_for(0ms) == std::future_status::ready)
        break;
      std::this_thread::yield();
    }
    check(queued, "cached source or target reader bypassed the active move");
    gate.release();
    check(moving.get() == expected && reading.get(), "waiting move reader observed stale metadata");
  } else {
    bool caught = false;
    try {
      check(service.move_file("alice", source, target, "report.txt") == expected && !exception,
            "move service lost its transaction result");
    } catch (const std::runtime_error& error) {
      const std::string message = mode == "borrow_exception"  ? "injected connection exception"
                                  : mode == "query_exception" ? "injected query exception"
                                                              : "injected move update exception";
      caught = exception && error.what() == message;
      if (!caught)
        throw;
    } catch (int code) {
      caught = mode == "nonstandard_exception" && code == 42;
      if (!caught)
        throw;
    }
    check(caught == exception, "move service swallowed or replaced an exception");
  }
  database.borrow_fails = database.borrow_throws = database.begin_fails = database.file_missing =
      false;
  database.query_fails_at = database.query_throws_at = database.update_exception = 0;
  const bool committed_attempt = expected != MoveFileResult::failed;
  const bool never_active =
      mode == "borrow_failure" || mode == "borrow_exception" || mode == "begin_failure";
  check(database.commits == (committed_attempt ? 1 : 0) &&
            database.rollbacks == (!committed_attempt && !never_active ? 1 : 0),
        "move service retried, rolled back an unknown commit or leaked a transaction");
  if (same)
    check(database.updates == 0,
          "same-directory move issued an UPDATE or skipped source validation");

  const bool moved = applied && !same;
  const int before_refresh = database.queries;
  const auto view = [&](const std::string& directory, bool has_file, char hash, bool has_child) {
    const auto list = reader.find_file_by_current_dir("alice", directory);
    const bool listed =
        std::any_of(list.result.begin(), list.result.end(), [](const FileInfo& info) {
          return info.file_name == "report.txt" && info.type == "file";
        });
    const auto download = reader.find_download_hash("alice", directory, "report.txt");
    check(list.success && listed == has_file && download.success == has_file &&
              (!has_file || download.result == std::string(64, hash)),
          "move retained stale source or target listing/download metadata");
    if (has_child)
      check(reader.move_dir_by_current_dir("alice", directory, "child").result ==
                child_path(directory, "child"),
            "move changed an unrelated child directory");
  };
  view(source, !moved, 'a', true);
  if (!same)
    view(target, moved || duplicate_file, duplicate_file ? 'f' : 'a', !empty_target);
  // 返回值可能没有变化，尤其是回滚路径；用查询计数证明两个范围的缓存都失效。
  if (!concurrent)
    check(database.queries == before_refresh + (same           ? 4
                                                : empty_target ? 7
                                                               : 8),
          "move did not invalidate every cache bank in both directory scopes");
  if (!disabled) {
    const int queries = database.queries;
    reader.find_file_by_current_dir("alice", source);
    reader.move_dir_by_current_dir("alice", source, "child");
    reader.find_download_hash("alice", moved ? target : source, "report.txt");
    if (!same) {
      reader.find_file_by_current_dir("alice", target);
      if (!empty_target)
        reader.move_dir_by_current_dir("alice", target, "child");
    }
    check(database.queries == queries, "refreshed move metadata was not cached");
  }
  {
    std::lock_guard<std::mutex> lock(database.mutex);
    const auto file =
        std::find_if(database.files.begin(), database.files.end(), [](const File& item) {
          return item.id == "128";
        });
    check(file != database.files.end() && file->username == "alice" && file->name == "report.txt" &&
              file->parent == (moved ? target_id : source_id) &&
              file->path == child_path(moved ? target : source, "report.txt") &&
              file->hash == std::string(64, 'a'),
          "move changed file identity, name, hash or the wrong parent/path");
  }
  check(reader.find_download_hash("alice", "/other", "report.txt").result == std::string(64, 'c') &&
            reader.find_download_hash("bob", "/docs", "report.txt").result == std::string(64, 'd'),
        "move changed another directory or user");
  return expected == MoveFileResult::committed ? 0 : 1;
}

void test_concurrent(MetadataService& service) {
  warm(service);
  gate.enabled = true;
  auto deletion = std::async(std::launch::async, [&] {
    return service.delete_file("alice", "/docs", "report.txt");
  });
  // 析构时先放行闸门，再等待 async future，避免断言异常使测试自身死锁。
  struct Release {
    ~Release() {
      gate.release();
    }
  } release;
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    check(gate.changed.wait_for(lock, 3s,
                                [&] {
                                  return gate.entered;
                                }),
          "deletion did not reach the commit boundary");
  }
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  auto reader = std::async(std::launch::async, [&] {
    entered.set_value();
    const auto list = service.find_file_by_current_dir("alice", "/docs");
    const auto download = service.find_download_hash("alice", "/docs", "report.txt");
    return list.success && list.result.size() == 1 && !download.success;
  });
  const bool scheduled = entered_future.wait_for(3s) == std::future_status::ready;
  const bool waiting = reader.wait_for(20ms) == std::future_status::timeout;
  gate.release();
  check(deletion.get() == DeleteFileResult::committed && reader.get() && scheduled && waiting,
        "cache reader bypassed deletion coordination or observed a stale entry");
  check_idle();
}

// 目录创建不会改变已有文件的下载哈希，但仍需清除旧的同目录缓存版本。
// 用查询计数证明缓存已失效，不能只比较返回值是否碰巧相同。
void check_creation_refreshed(MetadataService& service, bool applied) {
  const int before = database.queries;
  const auto list = service.find_file_by_current_dir("alice", "/docs");
  const auto download = service.find_download_hash("alice", "/docs", "report.txt");
  check(database.queries == before + 3, "mkdir left an old list or download hash cached");
  check(list.success && list.result.size() == (applied ? 3U : 2U) && download.success &&
            download.result == std::string(64, 'a'),
        "mkdir refresh did not reflect the committed directory tree");
  if (applied)
    check(list.result.back().file_name == "drafts" && list.result.back().type == "directory",
          "mkdir inserted the wrong kind of child");
  const int refreshed = database.queries;
  service.find_file_by_current_dir("alice", "/docs");
  service.find_download_hash("alice", "/docs", "report.txt");
  check(database.queries == refreshed, "refreshed mkdir metadata was not cached");
}

void test_creation_concurrent(MetadataService& service) {
  warm(service);
  gate.enabled = true;
  auto creation = std::async(std::launch::async, [&] {
    return service.create_dir("alice", "/docs", "drafts");
  });
  struct Release {
    ~Release() {
      gate.release();
    }
  } release;
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    check(gate.changed.wait_for(lock, 3s,
                                [&] {
                                  return gate.entered;
                                }),
          "mkdir did not reach its commit boundary");
  }
  std::promise<void> started;
  auto starting = started.get_future();
  auto reader = std::async(std::launch::async, [&] {
    started.set_value();
    const auto list = service.find_file_by_current_dir("alice", "/docs");
    return list.success && list.result.size() == 3;
  });
  const bool scheduled = starting.wait_for(3s) == std::future_status::ready;
  const bool waiting = reader.wait_for(20ms) == std::future_status::timeout;
  gate.release();
  check(creation.get() == CreateDirectoryResult::committed && reader.get() && scheduled && waiting,
        "cache read bypassed mkdir coordination or returned the old list");
}

std::uint64_t test_creation(MetadataService& service, const std::shared_ptr<MetadataCache>& cache,
                            const std::string& mode) {
  if (mode == "disabled") {
    check(service.create_dir("alice", "/docs", "drafts") == CreateDirectoryResult::committed,
          "disabled cache prevented mkdir");
    check(service.find_file_by_current_dir("alice", "/docs").result.size() == 3,
          "mkdir did not persist with caching disabled");
    return 0;
  }
  if (mode == "concurrent") {
    test_creation_concurrent(service);
    return 0;
  }

  std::unique_ptr<MetadataService> shared_reader;
  if (mode == "shared")
    shared_reader =
        std::make_unique<MetadataService>(*DbConnectionPool::get_connection_pool(), cache);
  auto& reader = shared_reader ? *shared_reader : service;
  warm(reader);
  if (mode == "invalid_arguments") {
    const int before = database.borrows;
    check(service.create_dir("", "/docs", "drafts") == CreateDirectoryResult::failed &&
              service.create_dir("alice", "", "drafts") == CreateDirectoryResult::failed &&
              service.create_dir("alice", "/docs", "../drafts") == CreateDirectoryResult::failed,
          "mkdir accepted invalid arguments");
    service.find_file_by_current_dir("alice", "/docs");
    service.find_download_hash("alice", "/docs", "report.txt");
    check(database.borrows == before && database.begins == 0,
          "invalid mkdir request borrowed a connection or discarded valid cache entries");
    return 3;
  }

  CreateDirectoryResult expected = CreateDirectoryResult::committed;
  bool applied = true;
  const bool unknown = mode == "unknown_applied" || mode == "unknown_not_applied";
  const bool exception =
      mode == "borrow_exception" || mode == "query_exception" || mode == "insert_exception";
  if (unknown) {
    expected = CreateDirectoryResult::outcome_unknown;
    database.commit_reply = DbCommitStatus::k_outcome_unknown;
    applied = database.unknown_applies = mode == "unknown_applied";
  } else if (mode != "committed" && mode != "shared") {
    expected = CreateDirectoryResult::failed;
    applied = false;
  }
  if (mode == "borrow_failure")
    database.borrow_fails = true;
  if (mode == "begin_failure")
    database.begin_fails = true;
  if (mode == "query_failure")
    database.query_fails_at = 2;
  if (mode == "insert_failure")
    database.insert_reply = {false, 1213, 0, 0};
  if (mode == "insert_zero")
    database.insert_reply = {true, 0, 0, 0};
  if (mode == "borrow_exception")
    database.borrow_throws = true;
  if (mode == "query_exception")
    database.query_throws_at = 2;
  if (mode == "insert_exception")
    database.insert_throws = true;
  bool propagated = false;
  try {
    const auto result =
        service.create_dir("alice", "/docs", mode == "duplicate_file" ? "report.txt" : "drafts");
    check(!exception && result == expected, "mkdir lost its three-state result");
  } catch (const std::runtime_error& error) {
    const std::string message = mode == "borrow_exception"  ? "injected connection exception"
                                : mode == "query_exception" ? "injected query exception"
                                                            : "injected insert exception";
    propagated = exception && error.what() == message;
    check(propagated, "mkdir swallowed or replaced a database exception");
  }
  check(propagated == exception, "mkdir exception propagation differed from its contract");
  const bool borrowed = mode != "borrow_failure" && mode != "borrow_exception";
  check(database.commits == (expected == CreateDirectoryResult::failed ? 0 : 1) &&
            database.rollbacks ==
                (expected == CreateDirectoryResult::failed && borrowed && mode != "begin_failure"
                     ? 1
                     : 0),
        "mkdir retried COMMIT or did not finish its failed transaction");
  database.borrow_fails = database.borrow_throws = database.begin_fails = false;
  database.query_fails_at = database.query_throws_at = 0;
  check_creation_refreshed(reader, applied);
  const auto cd = reader.move_dir_by_current_dir("alice", "/docs", "drafts");
  check(cd.success == applied && (!applied || cd.result == "/docs/drafts"),
        "created directory lookup disagrees with committed state");
  return expected == CreateDirectoryResult::committed ? 0 : 1;
}

// 同时预热两个目录：父目录的列表、下载和向下 cd，以及目标目录的空列表和向上 cd。
// 空列表也属于成功查询，必须缓存并在删除后失效。
void warm_rmdir(MetadataService& service, std::size_t children) {
  const auto parent = service.find_file_by_current_dir("alice", "/docs");
  const auto target = service.find_file_by_current_dir("alice", "/docs/empty");
  const auto download = service.find_download_hash("alice", "/docs", "report.txt");
  const auto down = service.move_dir_by_current_dir("alice", "/docs", "empty");
  const auto up = service.move_dir_by_current_dir("alice", "/docs/empty", "..");
  check(parent.success && parent.result.size() == 3 && target.success &&
            target.result.size() == children && download.success &&
            download.result == std::string(64, 'a') && down.success &&
            down.result == "/docs/empty" && up.success && up.result == "/docs",
        "rmdir cache warm-up did not reflect the initial directory tree");
  const int before = database.queries;
  service.find_file_by_current_dir("alice", "/docs");
  service.find_file_by_current_dir("alice", "/docs/empty");
  service.find_download_hash("alice", "/docs", "report.txt");
  service.move_dir_by_current_dir("alice", "/docs", "empty");
  service.move_dir_by_current_dir("alice", "/docs/empty", "..");
  check(database.queries == before, "rmdir did not start with both directory caches populated");
}

void check_rmdir_refreshed(MetadataService& service, bool removed, std::size_t children) {
  const auto& cache = service._metadata_cache;
  const auto* up_gate = &cache->gate_for(cache->make_key("alice", "/"));
  // 向上 cd 的目标是 /docs，缓存归属 /；仅物理分片碰撞时才被本次写入一并失效。
  const bool up_invalidated = up_gate == &cache->gate_for(cache->make_key("alice", "/docs")) ||
                              up_gate == &cache->gate_for(cache->make_key("alice", "/docs/empty"));
  const int before = database.queries;
  const auto parent = service.find_file_by_current_dir("alice", "/docs");
  const auto target = service.find_file_by_current_dir("alice", "/docs/empty");
  const auto download = service.find_download_hash("alice", "/docs", "report.txt");
  const auto down = service.move_dir_by_current_dir("alice", "/docs", "empty");
  const auto up = service.move_dir_by_current_dir("alice", "/docs/empty", "..");
  // 删除后目标列表在查找目录 ID 时即失败，少一次子项查询。
  check(database.queries == before + (removed ? 5 : 6) + (up_invalidated ? 1 : 0),
        "rmdir left a stale list, download hash, or cd entry in either directory");
  check(parent.success && parent.result.size() == (removed ? 2U : 3U) &&
            target.success == !removed && (removed || target.result.size() == children) &&
            download.success && download.result == std::string(64, 'a') &&
            down.success == !removed && up.success && up.result == "/docs",
        "rmdir cache refresh disagreed with the committed directory tree");
  if (!removed)
    check(down.result == "/docs/empty" && up.result == "/docs",
          "retained directory changed its navigation paths");
}

void test_rmdir_concurrent(MetadataService& service) {
  warm_rmdir(service, 0);
  gate.enabled = true;
  auto deletion = std::async(std::launch::async, [&] {
    return service.delete_empty_directory("alice", "/docs", "empty");
  });
  struct Release {
    ~Release() {
      gate.release();
    }
  } release;
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    check(gate.changed.wait_for(lock, 3s,
                                [&] {
                                  return gate.entered;
                                }),
          "rmdir did not reach its commit boundary");
  }
  std::promise<void> parent_started, target_started;
  auto parent_start = parent_started.get_future();
  auto target_start = target_started.get_future();
  auto parent_reader = std::async(std::launch::async, [&] {
    parent_started.set_value();
    return service.find_file_by_current_dir("alice", "/docs");
  });
  auto target_reader = std::async(std::launch::async, [&] {
    target_started.set_value();
    return service.find_file_by_current_dir("alice", "/docs/empty");
  });
  const bool scheduled = parent_start.wait_for(3s) == std::future_status::ready &&
                         target_start.wait_for(3s) == std::future_status::ready;
  const bool parent_waiting = parent_reader.wait_for(20ms) == std::future_status::timeout;
  const bool target_waiting = target_reader.wait_for(20ms) == std::future_status::timeout;
  gate.release();
  const auto outcome = deletion.get();
  const auto parent = parent_reader.get();
  const auto target = target_reader.get();
  check(scheduled && parent_waiting && target_waiting &&
            outcome == DeleteDirectoryResult::committed && parent.success &&
            parent.result.size() == 2 && !target.success,
        "rmdir did not coordinate reads in both directory scopes through commit");
}

std::uint64_t test_rmdir(MetadataService& service, const std::shared_ptr<MetadataCache>& cache,
                         const std::string& mode) {
  database.directory_deletion = true;
  database.directories.push_back({"30", "alice", "/docs/empty"});
  database.directories.push_back({"31", "bob", "/docs/empty"});
  const bool not_empty = mode == "not_empty_file" || mode == "not_empty_directory";
  if (mode == "not_empty_file")
    database.files.push_back({"132", "alice", "30", "child.txt", std::string(64, 'e')});
  if (mode == "not_empty_directory")
    database.directories.push_back({"32", "alice", "/docs/empty/nested"});
  if (mode == "concurrent") {
    test_rmdir_concurrent(service);
    return 0;
  }
  if (mode == "disabled") {
    check(service.delete_empty_directory("alice", "/docs", "empty") ==
              DeleteDirectoryResult::committed,
          "disabled cache prevented empty directory deletion");
    check(service.find_file_by_current_dir("alice", "/docs").result.size() == 2 &&
              !service.find_file_by_current_dir("alice", "/docs/empty").success &&
              !service.move_dir_by_current_dir("alice", "/docs", "empty").success &&
              service.move_dir_by_current_dir("alice", "/docs/empty", "..").result == "/docs",
          "disabled-cache reads did not reflect the deleted directory");
    return 0;
  }

  std::unique_ptr<MetadataService> shared_reader;
  if (mode == "shared")
    shared_reader =
        std::make_unique<MetadataService>(*DbConnectionPool::get_connection_pool(), cache);
  auto& reader = shared_reader ? *shared_reader : service;
  warm_rmdir(reader, not_empty ? 1 : 0);
  if (mode == "invalid_arguments") {
    const int before = database.borrows;
    check(
        service.delete_empty_directory("", "/docs", "empty") == DeleteDirectoryResult::failed &&
            service.delete_empty_directory("alice", "", "empty") == DeleteDirectoryResult::failed &&
            service.delete_empty_directory("alice", "/docs", "../empty") ==
                DeleteDirectoryResult::failed &&
            service.delete_empty_directory("alice", "/", "/") == DeleteDirectoryResult::failed &&
            service.delete_empty_directory("alice", "/" + std::string(1023, 'x'), "empty") ==
                DeleteDirectoryResult::failed,
        "rmdir accepted invalid names, a root request, or an overlong path");
    warm_rmdir(reader, 0);
    check(database.borrows == before && database.begins == 0,
          "invalid rmdir request touched the database or invalidated valid cache entries");
    return 5;
  }

  DeleteDirectoryResult expected = DeleteDirectoryResult::committed;
  bool removed = true;
  const bool unknown = mode == "unknown_applied" || mode == "unknown_not_applied";
  const bool exception =
      mode == "borrow_exception" || mode == "query_exception" || mode == "delete_exception";
  if (unknown) {
    expected = DeleteDirectoryResult::outcome_unknown;
    database.commit_reply = DbCommitStatus::k_outcome_unknown;
    removed = database.unknown_applies = mode == "unknown_applied";
  } else if (not_empty) {
    expected = DeleteDirectoryResult::not_empty;
    removed = false;
  } else if (mode != "committed" && mode != "shared") {
    expected = DeleteDirectoryResult::failed;
    removed = false;
  }
  if (mode == "borrow_failure")
    database.borrow_fails = true;
  if (mode == "begin_failure")
    database.begin_fails = true;
  // 子项查询失败必须是 failed，不能被误认为目录非空或目录为空。
  if (mode == "query_failure")
    database.query_fails_at = 4;
  if (mode == "delete_failure")
    database.delete_reply = {false, 1213, 0, 0};
  if (mode == "delete_zero")
    database.delete_reply = {true, 0, 0, 0};
  if (mode == "borrow_exception")
    database.borrow_throws = true;
  if (mode == "query_exception")
    database.query_throws_at = 4;
  if (mode == "delete_exception")
    database.delete_throws = true;
  bool propagated = false;
  try {
    const auto result =
        service.delete_empty_directory("alice", "/docs", mode == "wrong_case" ? "EMPTY" : "empty");
    check(!exception && result == expected, "rmdir lost its four-state result");
  } catch (const std::runtime_error& error) {
    const std::string message = mode == "borrow_exception"  ? "injected connection exception"
                                : mode == "query_exception" ? "injected query exception"
                                                            : "injected delete exception";
    propagated = exception && error.what() == message;
    check(propagated, "rmdir swallowed or replaced a database exception");
  }
  check(propagated == exception, "rmdir exception propagation differed from its contract");
  const bool commits = expected == DeleteDirectoryResult::committed || unknown;
  const bool active =
      mode != "borrow_failure" && mode != "borrow_exception" && mode != "begin_failure";
  check(database.commits == (commits ? 1 : 0) && database.rollbacks == (!commits && active ? 1 : 0),
        "rmdir retried an unknown commit or left its failed transaction active");
  check(database.deletes == (commits || mode == "delete_failure" || mode == "delete_zero" ||
                                     mode == "delete_exception"
                                 ? 1
                                 : 0),
        "rmdir deleted before confirming an empty target directory");
  database.borrow_fails = database.borrow_throws = database.begin_fails = false;
  database.query_fails_at = database.query_throws_at = 0;
  if (mode == "wrong_case") {
    // 锁定和失效的是请求中的路径；真实的小写目录必须保持完整。
    check(reader.find_file_by_current_dir("alice", "/docs").result.size() == 3 &&
              reader.find_file_by_current_dir("alice", "/docs/empty").success &&
              reader.move_dir_by_current_dir("alice", "/docs", "empty").success &&
              reader.move_dir_by_current_dir("alice", "/docs/empty", "..").success,
          "case-mismatched rmdir deleted the canonical directory");
  } else {
    check_rmdir_refreshed(reader, removed, not_empty ? 1 : 0);
  }
  check(service.find_file_by_current_dir("bob", "/docs/empty").success &&
            service.find_download_hash("alice", "/other", "report.txt").result ==
                std::string(64, 'c') &&
            service.find_download_hash("bob", "/docs", "report.txt").result == std::string(64, 'd'),
        "rmdir changed another directory or another user's metadata");
  return expected == DeleteDirectoryResult::committed ? 0 : 1;
}

struct TreeFixture {
  std::string parent = "/docs", name = "drafts", target, sub, deep, empty;
};

TreeFixture prepare_tree(const std::shared_ptr<MetadataCache>& cache, bool root_parent) {
  TreeFixture tree;
  if (root_parent) {
    tree.parent = "/";
    database.directories.push_back({"1", "alice", "/"});
  }
  tree.target = tree.parent == "/" ? "/drafts" : tree.parent + "/drafts";
  tree.sub = tree.target + "/sub";
  tree.empty = tree.target + "/empty";
  // 深层目录必须位于另一目录分片，避免碰撞使精确目录版本掩盖子树失效缺陷。
  auto& parent_gate = cache->gate_for(MetadataCache::make_key("alice", tree.parent));
  auto& target_gate = cache->gate_for(MetadataCache::make_key("alice", tree.target));
  for (int index = 0; index < 4096; ++index) {
    const auto candidate = tree.sub + "/leaf-" + std::to_string(index);
    auto& candidate_gate = cache->gate_for(MetadataCache::make_key("alice", candidate));
    if (&candidate_gate != &parent_gate && &candidate_gate != &target_gate) {
      tree.deep = candidate;
      break;
    }
  }
  check(!tree.deep.empty(), "could not select an independent descendant cache gate");
  database.recursive_deletion = true;
  for (const auto& directory : std::vector<Directory>{{"30", "alice", tree.target},
                                                      {"31", "alice", tree.sub},
                                                      {"32", "alice", tree.empty},
                                                      {"33", "alice", tree.deep},
                                                      {"34", "alice", tree.target + "-copy"},
                                                      {"40", "bob", "/docs/drafts"}})
    database.directories.push_back(directory);
  for (const auto& file :
       std::vector<File>{{"140", "alice", "30", "notes.txt", std::string(64, 'a')},
                         {"141", "alice", "31", "item.bin", std::string(64, 'b')},
                         {"142", "alice", "33", "deep.txt", std::string(64, 'c')},
                         {"143", "bob", "40", "notes.txt", std::string(64, 'd')},
                         {"144", "alice", "34", "keep.txt", std::string(64, 'e')}})
    database.files.push_back(file);
  return tree;
}

// 同时覆盖父目录、目标目录、深层目录和成功的空目录缓存，以及 cd 上下行和下载哈希。
void check_tree_view(MetadataService& service, const TreeFixture& tree, bool removed) {
  const auto parent = service.find_file_by_current_dir("alice", tree.parent);
  check(parent.success, "tree deletion removed the parent directory");
  const bool listed =
      std::any_of(parent.result.begin(), parent.result.end(), [&](const FileInfo& info) {
        return info.file_name == tree.name && info.type == "directory";
      });
  check(listed == !removed, "parent listing retained or lost the wrong child directory");
  const auto target = service.find_file_by_current_dir("alice", tree.target);
  const auto deep = service.find_file_by_current_dir("alice", tree.deep);
  const auto empty = service.find_file_by_current_dir("alice", tree.empty);
  const auto download = service.find_download_hash("alice", tree.target, "notes.txt");
  const auto deep_download = service.find_download_hash("alice", tree.deep, "deep.txt");
  const auto cd = service.move_dir_by_current_dir("alice", tree.parent, tree.name);
  const auto down = service.move_dir_by_current_dir("alice", tree.target, "sub");
  const auto up = service.move_dir_by_current_dir("alice", tree.deep, "..");
  check(target.success == !removed && deep.success == !removed && empty.success == !removed &&
            download.success == !removed && deep_download.success == !removed &&
            cd.success == !removed && down.success == !removed && up.success == !removed,
        "tree deletion left stale list, download or cd cache entries");
  if (!removed) {
    check(target.result.size() == 3 && deep.result.size() == 1 && empty.result.empty() &&
              download.result == std::string(64, 'a') &&
              deep_download.result == std::string(64, 'c') && cd.result == tree.target &&
              down.result == tree.sub && up.result == tree.sub,
          "tree metadata was changed by a rolled-back deletion");
  }
}

std::uint64_t test_tree(MetadataService& service, const std::shared_ptr<MetadataCache>& cache,
                        const std::string& mode) {
  const bool disabled = mode == "disabled";
  const auto tree = prepare_tree(cache, mode == "root_parent");
  MetadataService peer(*DbConnectionPool::get_connection_pool(), cache);
  MetadataService& reader = mode == "shared" ? peer : service;
  check_tree_view(reader, tree, false);
  if (!disabled) {
    const int before = database.queries;
    check_tree_view(reader, tree, false);
    check(database.queries == before, "tree metadata was not cached before deletion");
  }
  if (mode == "invalid_arguments") {
    const int borrows = database.borrows;
    const std::string long_parent = "/" + std::string(255, 'a') + "/" + std::string(255, 'b') +
                                    "/" + std::string(255, 'c') + "/" + std::string(255, 'd');
    for (const auto& args :
         std::vector<std::vector<std::string>>{{"", tree.parent, tree.name},
                                               {"alice", "", tree.name},
                                               {"alice", tree.parent, "../drafts"},
                                               {"alice", tree.parent, ".."},
                                               {"alice", long_parent, tree.name}})
      check(service.delete_directory_tree(args[0], args[1], args[2]) ==
                DeleteDirectoryTreeResult::failed,
            "invalid recursive deletion arguments were accepted");
    check_tree_view(reader, tree, false);
    check(database.borrows == borrows && database.begins == 0,
          "invalid tree request entered the database or invalidated warmed cache entries");
    return 5;
  }

  auto expected = DeleteDirectoryTreeResult::committed;
  bool applied = true, exception = false;
  if (mode == "unknown_applied" || mode == "unknown_not_applied") {
    database.commit_reply = DbCommitStatus::k_outcome_unknown;
    database.unknown_applies = mode == "unknown_applied";
    expected = DeleteDirectoryTreeResult::outcome_unknown;
    applied = database.unknown_applies;
  } else if (mode == "borrow_failure" || mode == "begin_failure" || mode == "query_failure" ||
             mode == "missing_target" || mode == "wrong_case" || mode == "delete_failure" ||
             mode == "delete_zero" || mode == "commit_not_active") {
    expected = DeleteDirectoryTreeResult::failed;
    applied = false;
    if (mode == "borrow_failure")
      database.borrow_fails = true;
    else if (mode == "begin_failure")
      database.begin_fails = true;
    else if (mode == "query_failure")
      database.query_fails_at = 5;
    else if (mode == "commit_not_active")
      database.commit_reply = DbCommitStatus::k_not_active;
    else if (mode == "delete_failure" || mode == "delete_zero") {
      // 前两个节点已暂存删除，再让第三次 DELETE 失败，验证整次回滚。
      database.tree_delete_fails_at = 3;
      database.delete_reply = mode == "delete_failure" ? DbExecuteResult{false, 1213, 0, 0}
                                                       : DbExecuteResult{true, 0, 0, 0};
    }
  } else if (mode == "borrow_exception" || mode == "query_exception" ||
             mode == "delete_exception" || mode == "nonstandard_exception") {
    expected = DeleteDirectoryTreeResult::failed;
    applied = false;
    exception = true;
    if (mode == "borrow_exception")
      database.borrow_throws = true;
    else if (mode == "query_exception")
      database.query_throws_at = 5;
    else {
      database.tree_delete_throws_at = 3;
      database.tree_nonstandard_exception = mode == "nonstandard_exception";
    }
  }
  const auto name = mode == "missing_target" ? "missing"
                    : mode == "wrong_case"   ? "DRAFTS"
                                             : tree.name;
  if (mode == "concurrent_parent" || mode == "concurrent_descendant") {
    gate.enabled = true;
    std::future<DeleteDirectoryTreeResult> deletion;
    std::future<bool> reading;
    struct Release {
      ~Release() {
        gate.release();
      }
    } release;
    deletion = std::async(std::launch::async, [&] {
      return service.delete_directory_tree("alice", tree.parent, name);
    });
    {
      std::unique_lock<std::mutex> lock(gate.mutex);
      check(gate.changed.wait_for(lock, 3s,
                                  [&] {
                                    return gate.entered;
                                  }),
            "recursive deletion did not reach its transactional delete boundary");
    }
    reading = std::async(std::launch::async, [&] {
      if (mode == "concurrent_descendant")
        return !reader.find_download_hash("alice", tree.deep, "deep.txt").success;
      const auto list = reader.find_file_by_current_dir("alice", tree.parent);
      return list.success &&
             std::none_of(list.result.begin(), list.result.end(), [&](const FileInfo& info) {
               return info.file_name == tree.name;
             });
    });
    bool queued = false;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
      {
        auto& coordinator = cache->_subtree_coordinator;
        std::lock_guard<std::mutex> lock(coordinator._mutex);
        queued = !coordinator._waiting_requests.empty();
      }
      if (queued || reading.wait_for(0ms) == std::future_status::ready)
        break;
      std::this_thread::yield();
    }
    check(queued, "cached tree reader bypassed the active subtree deletion");
    gate.release();
    check(deletion.get() == expected && reading.get(),
          "waiting tree reader observed stale metadata or deletion lost its result");
  } else {
    bool caught = false;
    try {
      const auto result = service.delete_directory_tree("alice", tree.parent, name);
      check(!exception && result == expected, "recursive service lost the transaction result");
    } catch (const std::runtime_error& error) {
      const std::string message = mode == "borrow_exception" ? "injected connection exception"
                                  : mode == "query_exception"
                                      ? "injected query exception"
                                      : "injected recursive delete exception";
      caught = exception && error.what() == message;
      if (!caught)
        throw;
    } catch (int code) {
      caught = mode == "nonstandard_exception" && code == 42;
      if (!caught)
        throw;
    }
    check(caught == exception, "recursive service swallowed or changed a business exception");
  }
  database.borrow_fails = database.borrow_throws = database.begin_fails = false;
  database.query_fails_at = database.query_throws_at = 0;
  database.tree_delete_fails_at = database.tree_delete_throws_at = 0;

  const bool committed_attempt =
      expected != DeleteDirectoryTreeResult::failed || mode == "commit_not_active";
  check(database.commits == (committed_attempt ? 1 : 0),
        "recursive service retried or omitted COMMIT");
  const bool never_active =
      mode == "borrow_failure" || mode == "borrow_exception" || mode == "begin_failure";
  check(database.rollbacks == (!committed_attempt && !never_active ? 1 : 0),
        "recursive service rolled back an unknown commit or leaked an active transaction");
  if (mode == "delete_failure" || mode == "delete_zero" || mode == "delete_exception" ||
      mode == "nonstandard_exception")
    check(database.deletes == 3, "recursive failure was not injected after two successful DELETEs");
  const int before_refresh = database.queries;
  check_tree_view(reader, tree, applied);
  // 回滚后的值可能与旧缓存相同。用查询次数证明所有相关条目仍经过了失效重查。
  // 四次列表各两条 SQL、两次哈希各一条、两个 cd 目标各一条，总计十二条。
  // 向下进入 sub 与从 deep 返回上级解析到同一目标，共用同一条缓存。
  if (!applied && mode != "missing_target" && mode != "wrong_case")
    check(database.queries == before_refresh + 12,
          "failed or uncertain deletion retained a warmed subtree cache entry");
  if (!disabled) {
    const int before = database.queries;
    reader.find_file_by_current_dir("alice", tree.parent);
    check(database.queries == before, "refreshed parent listing was not cached");
  }
  // 相似路径、同名目录、其他用户和仍有引用的同哈希文件均保留。
  check(
      reader.find_download_hash("alice", tree.target + "-copy", "keep.txt").result ==
              std::string(64, 'e') &&
          reader.find_download_hash("bob", "/docs/drafts", "notes.txt").result ==
              std::string(64, 'd') &&
          reader.find_download_hash("alice", "/other", "report.txt").result ==
              std::string(64, 'c') &&
          reader.find_download_hash("alice", "/docs", "report.txt").result == std::string(64, 'a'),
      "recursive deletion changed an unrelated namespace or a shared object reference");
  if (expected == DeleteDirectoryTreeResult::committed)
    check(database.deletes == 7, "recursive service did not delete exactly one seven-node subtree");
  return expected == DeleteDirectoryTreeResult::committed ? 0 : 1;
}

// 直接测试数据库私有方法，不经缓存或指标包装；无须连接真实 MySQL。
void test_directory_lookup(MetadataService& service, const std::string& scenario) {
  const auto lookup = [&](const std::string& user, const std::string& path, bool expected) {
    const int borrows = database.borrows.load();
    const int queries = database.queries.load();
    const auto result = service.find_directory_by_path_db(user, path);
    check(result.success == expected && result.result == (expected ? path : std::string{}),
          "directory lookup returned an incorrect success flag or path");
    check(database.borrows == borrows + 1 && database.queries == queries + 1,
          "directory lookup reused a cached result or retried its query");
    check_idle();
  };

  if (scenario == "invalid_arguments") {
    for (const auto& path : std::vector<std::string>{
             "", "docs", "/docs/", "//docs", "/docs//a", "/docs/.", "/docs/..",
             "/a\\b", "/" + std::string(256, 'x'), std::string("/a\0b", 4)}) {
      const auto result = service.find_directory_by_path_db("alice", path);
      check(!result.success && result.result.empty(), "invalid lookup path was accepted");
    }
    const auto result = service.find_directory_by_path_db("", "/docs");
    check(!result.success && result.result.empty() && database.borrows == 0 && database.queries == 0,
          "invalid directory lookup borrowed a connection");
  } else if (scenario == "success") {
    lookup("alice", "/docs", true);
  } else if (scenario == "root") {
    database.directories.push_back({"1", "alice", "/"});
    lookup("alice", "/", true);
  } else if (scenario == "missing") {
    lookup("alice", "/missing", false);
  } else if (scenario == "users") {
    lookup("alice", "/docs", true);
    lookup("bob", "/docs", true);
    lookup("bob", "/other", false);  // /other 仅属于 alice。
    lookup("carol", "/docs", false);
  } else if (scenario == "file") {
    lookup("alice", "/docs/report.txt", false);
  } else if (scenario == "case_sensitive") {
    lookup("alice", "/Docs", false);
    lookup("alice", "/docs", true);
  } else if (scenario == "literal_names") {
    const std::string path = "/资料/with spaces/a%_/'quoted'";
    database.directories.push_back({"50", "alice", path});
    lookup("alice", path, true);
  } else if (scenario == "uncached") {
    lookup("alice", "/docs", true);
    lookup("alice", "/docs", true);
    database.directories.erase(database.directories.begin());
    lookup("alice", "/docs", false);
  } else if (scenario == "borrow_failure") {
    database.borrow_fails = true;
    const auto result = service.find_directory_by_path_db("alice", "/docs");
    check(!result.success && result.result.empty() && database.borrows == 1 && database.queries == 0,
          "directory lookup ignored connection borrowing failure");
  } else if (scenario == "borrow_exception" || scenario == "query_exception" ||
             scenario == "nonstandard_exception") {
    database.borrow_throws = scenario == "borrow_exception";
    database.directory_lookup_exception = scenario == "query_exception" ? 1
                                         : scenario == "nonstandard_exception" ? 2 : 0;
    bool caught = false;
    try {
      service.find_directory_by_path_db("alice", "/docs");
    } catch (const std::runtime_error& error) {
      caught = std::string(error.what()) == (database.borrow_throws ? "injected connection exception"
                                                                   : "injected directory lookup exception");
    } catch (int error) {
      caught = scenario == "nonstandard_exception" && error == 42;
    }
    check(caught && database.borrows == 1 && database.queries == (database.borrow_throws ? 0 : 1),
          "directory lookup swallowed an exception or retried");
  } else {
    // 查询结果格式是服务层职责，不应把失败、重复记录或损坏数据当作成功。
    if (scenario == "query_failure") database.directory_lookup_reply = DbQueryResult{false, {{"/docs"}}};
    else if (scenario == "multiple_rows") database.directory_lookup_reply = DbQueryResult{true, {{"/docs"}, {"/docs"}}};
    else if (scenario == "no_columns") database.directory_lookup_reply = DbQueryResult{true, {{}}};
    else if (scenario == "extra_columns") database.directory_lookup_reply = DbQueryResult{true, {{"/docs", "extra"}}};
    else if (scenario == "null_value") database.directory_lookup_reply = DbQueryResult{true, {{std::nullopt}}};
    else if (scenario == "empty_path") database.directory_lookup_reply = DbQueryResult{true, {{""}}};
    else if (scenario == "wrong_path") database.directory_lookup_reply = DbQueryResult{true, {{"/Docs"}}};
    else throw std::invalid_argument("unknown directory lookup scenario");
    lookup("alice", "/docs", false);
  }
  check(database.begins == 0 && database.commits == 0 && database.rollbacks == 0 &&
            database.updates == 0 && database.inserts == 0 && database.deletes == 0,
        "directory lookup unexpectedly started a transaction or changed metadata");
  check_idle();
}

// 运行真实 cd 服务、路径解析和目录缓存；目录是否存在由进程内数据库替代层回答。
std::uint64_t test_cd_service(MetadataService& service, const std::shared_ptr<MetadataCache>& cache,
                              const std::string& scenario) {
  std::string archive = "/archive";
  if (scenario == "source_independent") {
    // 独立目录不应因物理分片碰撞而误判逻辑协调范围。
    for (int index = 0; &cache->gate_for(cache->make_key("alice", archive)) ==
                        &cache->gate_for(cache->make_key("alice", "/docs"));
         ++index) {
      check(index < 4096, "could not find independent cd cache gates");
      archive = "/archive_" + std::to_string(index);
    }
  }
  const std::string target = archive + "/sub";
  database.directories.insert(database.directories.end(), {{"1", "alice", "/"},
                                                           {"50", "alice", archive},
                                                           {"51", "alice", target},
                                                           {"52", "alice", target + "/deep"},
                                                           {"53", "alice", "/docs/child"},
                                                           {"54", "alice", "/docs/child/deep"}});
  std::uint64_t failures = 0;
  const auto visit = [&](const std::string& user, const std::string& current,
                         const std::string& argument, const std::optional<std::string>& expected) {
    const auto result = service.move_dir_by_current_dir(user, current, argument);
    check(result.success == expected.has_value() && result.result == expected.value_or(""),
          "cd returned an incorrect success flag or canonical target path");
    if (!expected)
      ++failures;
    check_idle();
  };

  if (scenario == "absolute") {
    visit("alice", "/docs", target, target);
  } else if (scenario == "relative") {
    visit("alice", "/docs", "child/deep", "/docs/child/deep");
  } else if (scenario == "parent") {
    visit("alice", "/docs/child", "../..", "/");
  } else if (scenario == "dot") {
    visit("alice", "/docs", "./child/../.", "/docs");
  } else if (scenario == "root" || scenario == "root_missing") {
    if (scenario == "root_missing") {
      database.directories.erase(
          std::remove_if(database.directories.begin(), database.directories.end(),
                         [](const Directory& directory) {
                           return directory.username == "alice" && directory.path == "/";
                         }),
          database.directories.end());
    }
    visit("alice", "/", "..", scenario == "root" ? std::optional<std::string>{"/"} : std::nullopt);
    check(database.queries == 1, "root cd skipped its metadata query");
  } else if (scenario == "aliases") {
    visit("alice", "/docs", target, target);
    visit("alice", "/", "archive/sub", target);
    visit("alice", "/docs", "../archive/./sub", target);
    visit("alice", "/other", target, target);
    check(database.queries == 1 && database.borrows == 1,
          "cd aliases did not share one target cache entry");
  } else if (scenario == "invalid") {
    visit("", "/docs", "child", std::nullopt);
    visit("alice", "docs", "child", std::nullopt);
    for (const auto& argument :
         std::vector<std::string>{"", "a//b", "a/", "a\\b", std::string(256, 'x')})
      visit("alice", "/docs", argument, std::nullopt);
    check(database.borrows == 0 && database.queries == 0, "invalid cd input reached the database");
  } else if (scenario == "users") {
    visit("alice", "/docs", target, target);
    visit("bob", "/docs", target, std::nullopt);
    check(database.queries == 2, "cd target cache was shared between different users");
  } else if (scenario == "missing" || scenario == "file" || scenario == "wrong_case") {
    const auto path = scenario == "missing" ? "/missing"
                      : scenario == "file"  ? "/docs/report.txt"
                                            : "/Docs";
    visit("alice", "/docs", path, std::nullopt);
    visit("alice", "/docs", path, std::nullopt);
    check(database.queries == 2, "failed cd lookup was admitted into the success cache");
  } else if (scenario == "target_invalidation") {
    database.directory_deletion = true;
    database.directories.erase(
        std::remove_if(database.directories.begin(), database.directories.end(),
                       [&](const Directory& directory) {
                         return directory.path == target + "/deep";
                       }),
        database.directories.end());
    visit("alice", "/docs", target, target);
    visit("alice", "/other", target, target);
    check(database.queries == 1, "absolute cd target was not cached");
    check(
        service.delete_empty_directory("alice", archive, "sub") == DeleteDirectoryResult::committed,
        "could not delete the cached cd target");
    const int before = database.queries;
    visit("alice", "/docs", target, std::nullopt);
    check(database.queries == before + 1,
          "deleted cd target remained cached under its old source directory");
    // cd .. 只验证规范目标，允许从已删除的工作路径回到仍存在的父目录。
    visit("alice", target, "..", archive);
  } else if (scenario == "ancestor_invalidation") {
    database.recursive_deletion = true;
    visit("alice", "/docs", target + "/deep", target + "/deep");
    check(service.delete_directory_tree("alice", "/", "archive") ==
              DeleteDirectoryTreeResult::committed,
          "could not delete the cached cd target's ancestor");
    visit("alice", "/docs", target + "/deep", std::nullopt);
  } else if (scenario == "target_coordination" || scenario == "source_independent") {
    visit("alice", "/docs", target, target);
    std::promise<void> entered, release;
    auto entered_future = entered.get_future();
    auto released = release.get_future().share();
    std::future<bool> writer;
    std::future<StorageContext<std::string>> reader;
    struct Release {
      std::promise<void>& promise;
      bool active = true;
      ~Release() {
        if (active)
          promise.set_value();
      }
    } unblock{release};  // 比 futures 先析构，断言失败时也会释放写任务。
    const auto directory = scenario == "target_coordination" ? archive : std::string{"/docs"};
    writer = std::async(std::launch::async, [&] {
      return cache->write("alice", directory, [&] {
        entered.set_value();
        released.wait();
        return true;
      });
    });
    check(entered_future.wait_for(3s) == std::future_status::ready,
          "cd test writer did not acquire its scope");
    reader = std::async(std::launch::async, [&] {
      return service.move_dir_by_current_dir("alice", "/docs", target);
    });
    bool correct = false;
    if (scenario == "source_independent") {
      correct = reader.wait_for(3s) == std::future_status::ready;
    } else {
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      while (std::chrono::steady_clock::now() < deadline) {
        {
          std::lock_guard<std::mutex> lock(cache->_subtree_coordinator._mutex);
          correct = !cache->_subtree_coordinator._waiting_requests.empty();
        }
        if (correct || reader.wait_for(0s) == std::future_status::ready)
          break;
        std::this_thread::yield();
      }
    }
    release.set_value();
    unblock.active = false;
    check(writer.get(), "cd test writer failed");
    const auto result = reader.get();
    check(correct && result.success && result.result == target,
          "cd did not coordinate with its target parent or was blocked by its unrelated source");
    check(database.queries == (scenario == "source_independent" ? 1 : 2),
          "cd target cache version was associated with the wrong directory");
  } else if (scenario == "query_failure" || scenario == "malformed") {
    database.directory_lookup_reply =
        scenario == "query_failure" ? DbQueryResult{false, {}} : DbQueryResult{true, {{"/wrong"}}};
    visit("alice", "/docs", target, std::nullopt);
    database.directory_lookup_reply.reset();
    visit("alice", "/docs", target, target);
    check(database.queries == 2, "failed cd lookup prevented a later successful query");
  } else if (scenario == "borrow_failure") {
    database.borrow_fails = true;
    visit("alice", "/docs", target, std::nullopt);
    database.borrow_fails = false;
    visit("alice", "/docs", target, target);
  } else if (scenario == "exception" || scenario == "nonstandard_exception") {
    database.directory_lookup_exception = scenario == "exception" ? 1 : 2;
    bool caught = false;
    try {
      service.move_dir_by_current_dir("alice", "/docs", target);
    } catch (const std::runtime_error& error) {
      caught = scenario == "exception" &&
               std::string(error.what()) == "injected directory lookup exception";
    } catch (int error) {
      caught = scenario == "nonstandard_exception" && error == 42;
    }
    check(caught, "cd swallowed its database exception");
    ++failures;
    database.directory_lookup_exception = 0;
    visit("alice", "/docs", target, target);
  } else {
    throw std::invalid_argument("unknown cd service scenario");
  }
  check_idle();
  return failures;
}

void run(const std::string& scenario) {
  MetadataCacheConfig config;
  // 不依赖 TTL 恰好到期，失效必须由写入后的版本推进触发。
  config.list_ttl = config.download_ttl = config.cd_ttl = std::chrono::hours(1);
  if (scenario == "disabled" || scenario == "creation_disabled" || scenario == "rmdir_disabled" ||
      scenario == "tree_disabled" || scenario == "rename_disabled" || scenario == "move_disabled") {
    config.list_capacity = 0;
    config.download_capacity = 0;
    config.cd_capacity = 0;
  }
  auto cache = std::make_shared<MetadataCache>(config);
  MetadataService service(*DbConnectionPool::get_connection_pool(), cache);
  auto metrics = std::make_shared<TLSS::OBSERVABILITY::StorageMetrics>();
  service.set_metrics(metrics);
  if (scenario.rfind("cd_service_", 0) == 0) {
    const auto failures = test_cd_service(service, cache, scenario.substr(11));
    check(metrics->get(TLSS::OBSERVABILITY::Metric::metadata_queries_failed) == failures &&
              metrics->get(TLSS::OBSERVABILITY::Metric::metadata_writes_failed) == 0,
          "cd service recorded incorrect query or write failure metrics");
    return;
  }
  if (scenario.rfind("directory_lookup_", 0) == 0) {
    test_directory_lookup(service, scenario.substr(17));
    check(metrics->get(TLSS::OBSERVABILITY::Metric::metadata_queries_failed) == 0 &&
              metrics->get(TLSS::OBSERVABILITY::Metric::metadata_writes_failed) == 0,
          "private directory lookup duplicated public-layer metrics");
    return;
  }
  std::uint64_t failures = 1;
  if (scenario.rfind("move_", 0) == 0) {
    failures = test_move(service, cache, scenario.substr(5));
  } else if (scenario == "rename_invalid_arguments") {
    failures = test_rename_invalid(service);
  } else if (scenario.rfind("rename_", 0) == 0) {
    failures = test_rename(service, cache, scenario.substr(7));
  } else if (scenario.rfind("tree_", 0) == 0) {
    failures = test_tree(service, cache, scenario.substr(5));
  } else if (scenario.rfind("rmdir_", 0) == 0) {
    failures = test_rmdir(service, cache, scenario.substr(6));
  } else if (scenario.rfind("creation_", 0) == 0) {
    failures = test_creation(service, cache, scenario.substr(9));
  } else if (scenario == "committed" || scenario == "unknown_applied" ||
             scenario == "unknown_not_applied") {
    test_outcome(service, scenario);
    if (scenario == "committed")
      failures = 0;
  } else if (scenario == "borrow_failure" || scenario == "begin_failure" ||
             scenario == "query_failure" || scenario == "missing_file" ||
             scenario == "delete_failure" || scenario == "delete_zero") {
    test_failure(service, scenario);
  } else if (scenario == "borrow_exception" || scenario == "query_exception" ||
             scenario == "delete_exception") {
    test_exception(service, scenario);
  } else if (scenario == "invalid_arguments") {
    test_invalid(service);
    failures = 3;
  } else if (scenario == "concurrent") {
    test_concurrent(service);
    failures = 0;
  } else if (scenario == "shared") {
    MetadataService reader(*DbConnectionPool::get_connection_pool(), cache);
    warm(reader);
    check(service.delete_file("alice", "/docs", "report.txt") == DeleteFileResult::committed,
          "shared-cache deletion failed");
    check_refreshed(reader, true);
    failures = 0;
  } else if (scenario == "disabled") {
    check(service.delete_file("alice", "/docs", "report.txt") == DeleteFileResult::committed,
          "disabled cache prevented deletion");
    check(!service.find_download_hash("alice", "/docs", "report.txt").success,
          "deleted file remained readable with cache disabled");
    failures = 0;
  } else {
    throw std::invalid_argument("unknown scenario: " + scenario);
  }
  check(metrics->get(TLSS::OBSERVABILITY::Metric::metadata_writes_failed) == failures,
        "deletion failure metric was missing, duplicated, or counted success");
  check_idle();
}

}  // namespace

int main(int argc, char** argv) {
  try {
    check(argc == 2, "expected one scenario argument");
    run(argv[1]);
    std::cout << "PASS metadata deletion " << argv[1] << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
