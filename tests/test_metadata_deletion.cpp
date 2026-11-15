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
#include <vector>

namespace {

using namespace TLSS::BASE;
using namespace TLSS::FileStorage;
using namespace std::chrono_literals;

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct Directory { std::string id, username, path; };
struct File { std::string id, username, parent, name, hash; };
struct Transaction {
    int queries = 0;
    std::string deleted_id;
    std::string deleted_directory_id;
    std::optional<Directory> created_directory;
};

// 数据库替代层只模拟元数据及事务提交；不连接真实 MySQL，不创建对象文件。
// 缓存、MetadataService、文件删除、目录创建和空目录删除均执行项目实际实现。
struct Database {
    std::mutex mutex;
    std::vector<Directory> directories{{"12", "alice", "/docs"},
                                       {"13", "alice", "/other"}, {"22", "bob", "/docs"}};
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
        changed.wait(lock, [&] { return released; });
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
    : _produce_thread([] {}), _scanner_thread([] {}), _connection_cnt(0),
      _stopping(false), _threads_joined(true), _borrowed_count(0), _active_getters(0) {}
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
    : _conn(nullptr), _connected(false), _transaction_active(false) {
    std::lock_guard<std::mutex> lock(database.mutex);
    database.transactions.emplace(this, Transaction{});
}
DbConnection::~DbConnection() {
    std::lock_guard<std::mutex> lock(database.mutex);
    database.transactions.erase(this);
}

DbQueryResult DbConnection::query_prepared(
    const std::string& sql, const std::vector<DbValue>& params) {
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
        if (stage == 2) {
            const auto* directory = find_directory(value(params, 0), value(params, 1));
            return directory ? DbQueryResult{true, {{directory->id}}} : DbQueryResult{true, {}};
        }
        if (database.directory_deletion) {
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
                    result.rows.push_back({child.path.substr(separator + 1), "directory", "created", "updated"});
            }
        }
        return result;
    }
    if (sql.find("SELECT child.path") == 0) {
        const auto& parent_path = value(params, 2);
        const auto path = parent_path == "/" ? parent_path + value(params, 1) :
                                              parent_path + "/" + value(params, 1);
        const auto* child = find_directory(value(params, 0), path);
        return child ? DbQueryResult{true, {{child->path}}} : DbQueryResult{true, {}};
    }
    if (sql.find("SELECT parent.path") == 0) {
        const auto* child = find_directory(value(params, 0), value(params, 1));
        const auto separator = value(params, 1).rfind('/');
        const auto parent_path = separator == 0 ? "/" : value(params, 1).substr(0, separator);
        const auto* parent = find_directory(value(params, 0), parent_path);
        return child && parent ? DbQueryResult{true, {{parent->path}}} : DbQueryResult{true, {}};
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
DbExecuteResult DbConnection::execute_prepared(
    const std::string& sql, const std::vector<DbValue>& params) {
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
        check(transaction.queries == (database.directory_deletion ? 4 : 3),
              "DELETE ran before all locking queries");
        if (database.delete_throws)
            throw std::runtime_error("injected delete exception");
        if (database.directory_deletion) {
            check(sql.find("CAST(path AS BINARY) = CAST(? AS BINARY)") != std::string::npos,
                  "directory DELETE omitted byte-exact path comparison");
            check(params == std::vector<DbValue>{"30", "alice", "12", "empty", "/docs/empty"},
                  "directory DELETE changed its target or owner");
            if (database.delete_reply.success && database.delete_reply.affected_rows == 1)
                transaction.deleted_directory_id = value(params, 0);
        } else if (database.delete_reply.success && database.delete_reply.affected_rows == 1) {
            transaction.deleted_id = value(params, 0);
        }
    }
    gate.wait();
    return database.delete_reply;
}

DbTransaction::DbTransaction(DbConnection& connection) noexcept
    : connection_(connection), active_(!database.begin_fails && !connection._transaction_active) {
    ++database.begins;
    if (active_)
        connection_._transaction_active = true;
}
DbTransaction::~DbTransaction() noexcept { rollback(); }
DbCommitStatus DbTransaction::commit() noexcept {
    if (!active_)
        return DbCommitStatus::k_not_active;
    ++database.commits;
    std::lock_guard<std::mutex> lock(database.mutex);
    auto& transaction = database.transactions.at(&connection_);
    if (database.commit_reply == DbCommitStatus::k_committed ||
        (database.commit_reply == DbCommitStatus::k_outcome_unknown && database.unknown_applies)) {
        auto& files = database.files;
        files.erase(std::remove_if(files.begin(), files.end(), [&](const File& file) {
            return file.id == transaction.deleted_id;
        }), files.end());
        if (transaction.created_directory)
            database.directories.push_back(*transaction.created_directory);
        auto& directories = database.directories;
        directories.erase(std::remove_if(directories.begin(), directories.end(), [&](const Directory& directory) {
            return directory.id == transaction.deleted_directory_id;
        }), directories.end());
    }
    // 未知提交不再补发回滚；测试分别模拟远端实际提交和未提交。
    transaction.deleted_id.clear();
    transaction.deleted_directory_id.clear();
    transaction.created_directory.reset();
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
              download.result == std::string(64, 'a'), "initial metadata lookup failed");
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
    check(list.success && list.result.size() == (deleted ? 1U : 2U) &&
              download.success == !deleted, "cache refresh did not reflect committed metadata");
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
    check(service.find_download_hash("alice", "/docs", "another.txt").result == std::string(64, 'b') &&
              service.find_download_hash("alice", "/other", "report.txt").result == std::string(64, 'c') &&
              service.find_download_hash("bob", "/docs", "report.txt").result == std::string(64, 'd'),
          "deletion changed another file or namespace");
    check_idle();
}

void test_failure(MetadataService& service, const std::string& scenario) {
    warm(service);
    if (scenario == "borrow_failure") database.borrow_fails = true;
    else if (scenario == "begin_failure") database.begin_fails = true;
    else if (scenario == "query_failure") database.query_fails_at = 2;
    else if (scenario == "missing_file") database.file_missing = true;
    else if (scenario == "delete_failure") database.delete_reply = {false, 1213, 0, 0};
    else if (scenario == "delete_zero") database.delete_reply = {true, 0, 0, 0};
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
    if (scenario == "borrow_exception") database.borrow_throws = true;
    else if (scenario == "query_exception") database.query_throws_at = 3;
    else database.delete_throws = true;
    bool propagated = false;
    try {
        service.delete_file("alice", "/docs", "report.txt");
    } catch (const std::runtime_error& error) {
        const std::string expected = scenario == "borrow_exception" ? "injected connection exception" :
                                     scenario == "query_exception" ? "injected query exception" :
                                                                      "injected delete exception";
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

void test_concurrent(MetadataService& service) {
    warm(service);
    gate.enabled = true;
    auto deletion = std::async(std::launch::async, [&] {
        return service.delete_file("alice", "/docs", "report.txt");
    });
    // 析构时先放行闸门，再等待 async future，避免断言异常使测试自身死锁。
    struct Release { ~Release() { gate.release(); } } release;
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        check(gate.changed.wait_for(lock, 3s, [&] { return gate.entered; }),
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
    check(list.success && list.result.size() == (applied ? 3U : 2U) &&
              download.success && download.result == std::string(64, 'a'),
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
    struct Release { ~Release() { gate.release(); } } release;
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        check(gate.changed.wait_for(lock, 3s, [&] { return gate.entered; }),
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
        shared_reader = std::make_unique<MetadataService>(*DbConnectionPool::get_connection_pool(), cache);
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
    const bool exception = mode == "borrow_exception" || mode == "query_exception" || mode == "insert_exception";
    if (unknown) {
        expected = CreateDirectoryResult::outcome_unknown;
        database.commit_reply = DbCommitStatus::k_outcome_unknown;
        applied = database.unknown_applies = mode == "unknown_applied";
    } else if (mode != "committed" && mode != "shared") {
        expected = CreateDirectoryResult::failed;
        applied = false;
    }
    if (mode == "borrow_failure") database.borrow_fails = true;
    if (mode == "begin_failure") database.begin_fails = true;
    if (mode == "query_failure") database.query_fails_at = 2;
    if (mode == "insert_failure") database.insert_reply = {false, 1213, 0, 0};
    if (mode == "insert_zero") database.insert_reply = {true, 0, 0, 0};
    if (mode == "borrow_exception") database.borrow_throws = true;
    if (mode == "query_exception") database.query_throws_at = 2;
    if (mode == "insert_exception") database.insert_throws = true;
    bool propagated = false;
    try {
        const auto result = service.create_dir("alice", "/docs", mode == "duplicate_file" ? "report.txt" : "drafts");
        check(!exception && result == expected, "mkdir lost its three-state result");
    } catch (const std::runtime_error& error) {
        const std::string message = mode == "borrow_exception" ? "injected connection exception" :
                                    mode == "query_exception" ? "injected query exception" : "injected insert exception";
        propagated = exception && error.what() == message;
        check(propagated, "mkdir swallowed or replaced a database exception");
    }
    check(propagated == exception, "mkdir exception propagation differed from its contract");
    const bool borrowed = mode != "borrow_failure" && mode != "borrow_exception";
    check(database.commits == (expected == CreateDirectoryResult::failed ? 0 : 1) &&
              database.rollbacks == (expected == CreateDirectoryResult::failed && borrowed && mode != "begin_failure" ? 1 : 0),
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
    const int before = database.queries;
    const auto parent = service.find_file_by_current_dir("alice", "/docs");
    const auto target = service.find_file_by_current_dir("alice", "/docs/empty");
    const auto download = service.find_download_hash("alice", "/docs", "report.txt");
    const auto down = service.move_dir_by_current_dir("alice", "/docs", "empty");
    const auto up = service.move_dir_by_current_dir("alice", "/docs/empty", "..");
    // 删除后目标列表在查找目录 ID 时即失败，少一次子项查询。
    check(database.queries == before + (removed ? 6 : 7),
          "rmdir left a stale list, download hash, or cd entry in either directory");
    check(parent.success && parent.result.size() == (removed ? 2U : 3U) &&
              target.success == !removed && (removed || target.result.size() == children) &&
              download.success && download.result == std::string(64, 'a') &&
              down.success == !removed && up.success == !removed,
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
    struct Release { ~Release() { gate.release(); } } release;
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        check(gate.changed.wait_for(lock, 3s, [&] { return gate.entered; }),
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
        check(service.delete_empty_directory("alice", "/docs", "empty") == DeleteDirectoryResult::committed,
              "disabled cache prevented empty directory deletion");
        check(service.find_file_by_current_dir("alice", "/docs").result.size() == 2 &&
                  !service.find_file_by_current_dir("alice", "/docs/empty").success &&
                  !service.move_dir_by_current_dir("alice", "/docs", "empty").success &&
                  !service.move_dir_by_current_dir("alice", "/docs/empty", "..").success,
              "disabled-cache reads did not reflect the deleted directory");
        return 0;
    }

    std::unique_ptr<MetadataService> shared_reader;
    if (mode == "shared")
        shared_reader = std::make_unique<MetadataService>(*DbConnectionPool::get_connection_pool(), cache);
    auto& reader = shared_reader ? *shared_reader : service;
    warm_rmdir(reader, not_empty ? 1 : 0);
    if (mode == "invalid_arguments") {
        const int before = database.borrows;
        check(service.delete_empty_directory("", "/docs", "empty") == DeleteDirectoryResult::failed &&
                  service.delete_empty_directory("alice", "", "empty") == DeleteDirectoryResult::failed &&
                  service.delete_empty_directory("alice", "/docs", "../empty") == DeleteDirectoryResult::failed &&
                  service.delete_empty_directory("alice", "/", "/") == DeleteDirectoryResult::failed &&
                  service.delete_empty_directory("alice", "/" + std::string(1023, 'x'), "empty") == DeleteDirectoryResult::failed,
              "rmdir accepted invalid names, a root request, or an overlong path");
        warm_rmdir(reader, 0);
        check(database.borrows == before && database.begins == 0,
              "invalid rmdir request touched the database or invalidated valid cache entries");
        return 5;
    }

    DeleteDirectoryResult expected = DeleteDirectoryResult::committed;
    bool removed = true;
    const bool unknown = mode == "unknown_applied" || mode == "unknown_not_applied";
    const bool exception = mode == "borrow_exception" || mode == "query_exception" || mode == "delete_exception";
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
    if (mode == "borrow_failure") database.borrow_fails = true;
    if (mode == "begin_failure") database.begin_fails = true;
    // 子项查询失败必须是 failed，不能被误认为目录非空或目录为空。
    if (mode == "query_failure") database.query_fails_at = 4;
    if (mode == "delete_failure") database.delete_reply = {false, 1213, 0, 0};
    if (mode == "delete_zero") database.delete_reply = {true, 0, 0, 0};
    if (mode == "borrow_exception") database.borrow_throws = true;
    if (mode == "query_exception") database.query_throws_at = 4;
    if (mode == "delete_exception") database.delete_throws = true;
    bool propagated = false;
    try {
        const auto result = service.delete_empty_directory("alice", "/docs", mode == "wrong_case" ? "EMPTY" : "empty");
        check(!exception && result == expected, "rmdir lost its four-state result");
    } catch (const std::runtime_error& error) {
        const std::string message = mode == "borrow_exception" ? "injected connection exception" :
                                    mode == "query_exception" ? "injected query exception" : "injected delete exception";
        propagated = exception && error.what() == message;
        check(propagated, "rmdir swallowed or replaced a database exception");
    }
    check(propagated == exception, "rmdir exception propagation differed from its contract");
    const bool commits = expected == DeleteDirectoryResult::committed || unknown;
    const bool active = mode != "borrow_failure" && mode != "borrow_exception" && mode != "begin_failure";
    check(database.commits == (commits ? 1 : 0) && database.rollbacks == (!commits && active ? 1 : 0),
          "rmdir retried an unknown commit or left its failed transaction active");
    check(database.deletes == (commits || mode == "delete_failure" || mode == "delete_zero" || mode == "delete_exception" ? 1 : 0),
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
              service.find_download_hash("alice", "/other", "report.txt").result == std::string(64, 'c') &&
              service.find_download_hash("bob", "/docs", "report.txt").result == std::string(64, 'd'),
          "rmdir changed another directory or another user's metadata");
    return expected == DeleteDirectoryResult::committed ? 0 : 1;
}

void run(const std::string& scenario) {
    MetadataCacheConfig config;
    // 不依赖 TTL 恰好到期，失效必须由写入后的版本推进触发。
    config.list_ttl = config.download_ttl = config.cd_ttl = std::chrono::hours(1);
    if (scenario == "disabled" || scenario == "creation_disabled" || scenario == "rmdir_disabled") {
        config.list_capacity = 0;
        config.download_capacity = 0;
        config.cd_capacity = 0;
    }
    auto cache = std::make_shared<MetadataCache>(config);
    MetadataService service(*DbConnectionPool::get_connection_pool(), cache);
    auto metrics = std::make_shared<TLSS::OBSERVABILITY::StorageMetrics>();
    service.set_metrics(metrics);
    std::uint64_t failures = 1;
    if (scenario.rfind("rmdir_", 0) == 0) {
        failures = test_rmdir(service, cache, scenario.substr(6));
    } else if (scenario.rfind("creation_", 0) == 0) {
        failures = test_creation(service, cache, scenario.substr(9));
    } else if (scenario == "committed" || scenario == "unknown_applied" || scenario == "unknown_not_applied") {
        test_outcome(service, scenario);
        if (scenario == "committed") failures = 0;
    } else if (scenario == "borrow_failure" || scenario == "begin_failure" || scenario == "query_failure" ||
               scenario == "missing_file" || scenario == "delete_failure" || scenario == "delete_zero") {
        test_failure(service, scenario);
    } else if (scenario == "borrow_exception" || scenario == "query_exception" || scenario == "delete_exception") {
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
