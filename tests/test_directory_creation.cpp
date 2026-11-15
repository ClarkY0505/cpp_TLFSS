#include "ftp/directory_creation.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

using namespace TLSS::BASE;
using namespace TLSS::FileStorage;

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct Entry {
    std::string id, username, parent, name, path, kind;
    DbValue object_hash = std::nullopt;
    bool operator==(const Entry& other) const {
        return std::tie(id, username, parent, name, path, kind, object_hash) ==
               std::tie(other.id, other.username, other.parent, other.name,
                        other.path, other.kind, other.object_hash);
    }
};

// 执行真实目录创建函数，仅替换数据库连接和事务边界。
// stored 是已提交状态，pending 是事务工作副本；不连接 MySQL，不操作磁盘对象。
// SQL 加锁顺序由替代层检查，真实 InnoDB 锁等待及持久化仍需集成验证。
struct Database {
    std::string username = "alice", current_directory = "/docs", directory_name = "drafts";
    std::vector<Entry> stored{
        {"1", "alice", "-1", "/", "/", "directory"},
        {"12", "alice", "1", "docs", "/docs", "directory"},
        {"129", "alice", "12", "keep", "/docs/keep", "directory"},
        {"13", "alice", "1", "other", "/other", "directory"},
        {"130", "alice", "13", "drafts", "/other/drafts", "directory"},
        {"2", "bob", "-1", "/", "/", "directory"},
        {"22", "bob", "2", "docs", "/docs", "directory"},
        {"228", "bob", "22", "drafts", "/docs/drafts", "directory"}};
    std::vector<Entry> pending = stored;
    DbConnection* owner = nullptr;
    bool begin_ok = true, unknown_applies = false, nonstandard_exception = false;
    std::array<std::optional<DbQueryResult>, 2> replies;
    std::optional<DbExecuteResult> insert_reply;
    DbCommitStatus commit_reply = DbCommitStatus::k_committed;
    int begins = 0, queries = 0, inserts = 0, commits = 0, rollbacks = 0;
    int throw_at = 0; // 1 用户查询；2 父目录查询；3 INSERT 前；4 工作副本插入后。
    int parent_change_on_lock = 0; // 1 模拟此前提交的移动；2 移动后又在原路径新建父目录。
    std::string selected_parent;
} database;

void reset() { database = Database{}; }

std::string new_path() {
    return database.current_directory == "/" ? "/" + database.directory_name :
           database.current_directory + "/" + database.directory_name;
}

void maybe_throw(int stage) {
    if (database.throw_at == stage) {
        if (database.nonstandard_exception)
            throw 7;
        throw std::runtime_error("injected creation database exception");
    }
}

void require_sql(const std::string& sql, const std::vector<std::string>& fragments) {
    for (const auto& fragment : fragments)
        check(sql.find(fragment) != std::string::npos, "creation SQL omitted a required constraint");
    check(sql.find("storage_objects") == std::string::npos,
          "directory creation accessed the object catalog");
    check(sql.find("alice") == std::string::npos && sql.find("drafts") == std::string::npos,
          "creation SQL interpolated user input");
}

// 模拟等待用户锁期间，其他事务已经提交父目录移动。
// 同时更新父目录及其子项路径，保持测试目录树一致；本事务回滚不能撤销这些外部提交。
void move_parent(std::vector<Entry>& entries, bool replace) {
    for (auto& entry : entries) {
        if (entry.username != "alice")
            continue;
        if (entry.id == "12") {
            entry.name = "moved";
            entry.path = "/moved";
        } else if (entry.path.rfind("/docs/", 0) == 0) {
            entry.path = "/moved" + entry.path.substr(5);
        }
    }
    if (replace)
        entries.push_back({"99", "alice", "1", "docs", "/docs", "directory"});
}

CreateDirectoryResult execute() {
    // 测试可能在调用前调整初始目录树，保持尚未开启事务的两个副本一致。
    database.pending = database.stored;
    DbConnection connection;
    return create_directory_transaction(connection, database.username,
                                         database.current_directory, database.directory_name);
}

void check_finished(int commits, int rollbacks) {
    check(database.commits == commits && database.rollbacks == rollbacks,
          "creation used unexpected commit or rollback count");
    check(database.owner == nullptr && database.pending == database.stored,
          "creation left a transaction or pending changes active");
}

void check_unchanged(const std::vector<Entry>& before) {
    check(database.stored == before, "failed creation changed committed metadata");
}

void check_created(const std::vector<Entry>& before, bool applied, const std::string& parent = "12") {
    auto expected = before;
    if (applied)
        expected.push_back({"500", database.username, parent, database.directory_name,
                            new_path(), "directory", std::nullopt});
    check(database.stored == expected,
          "creation changed another record or inserted the wrong parent, path, type, or object hash");
}

} // namespace

namespace TLSS::BASE {

DbConnection::DbConnection()
    : _conn(nullptr), _connected(false), _transaction_active(false) {}
DbConnection::~DbConnection() = default;

DbQueryResult DbConnection::query_prepared(
    const std::string& sql, const std::vector<DbValue>& params) {
    check(_transaction_active && database.owner == this,
          "creation query did not use the active transaction connection");
    const int stage = ++database.queries;
    check(stage <= 2 && database.inserts == 0, "creation queried out of order or retried");
    require_sql(sql, {"FOR UPDATE"});
    maybe_throw(stage);
    DbQueryResult actual{true, {}};
    if (stage == 1) {
        require_sql(sql, {"SELECT id FROM users", "username = ?", "is_deleted = 0"});
        check(params == std::vector<DbValue>{database.username}, "creation changed its user parameter");
        if (database.parent_change_on_lock != 0) {
            move_parent(database.stored, database.parent_change_on_lock == 2);
            database.pending = database.stored;
        }
        actual.rows = {{"7"}};
    } else {
        require_sql(sql, {"SELECT id FROM virtual_files", "username = ?", "path = ?",
                          "file_type = 'directory'"});
        check(params == std::vector<DbValue>{database.username, database.current_directory},
              "creation looked up another user's directory or another path");
        for (const auto& entry : database.pending) {
            if (entry.username == *params[0] && entry.path == *params[1] && entry.kind == "directory")
                actual.rows.push_back({entry.id});
        }
    }
    const auto reply = database.replies[stage - 1].value_or(actual);
    if (stage == 2 && reply.success && reply.rows.size() == 1 &&
        reply.rows[0].size() == 1 && reply.rows[0][0])
        database.selected_parent = *reply.rows[0][0];
    return reply;
}

DbExecuteResult DbConnection::execute_prepared(
    const std::string& sql, const std::vector<DbValue>& params) {
    check(_transaction_active && database.owner == this, "creation INSERT used another connection");
    check(database.queries == 2 && ++database.inserts == 1,
          "creation inserted before locking the parent or retried INSERT");
    require_sql(sql, {"INSERT INTO virtual_files", "file_name, username, parent_id, path, file_type, object_hash",
                      "VALUES (?, ?, ?, ?, 'directory', NULL)"});
    check(sql.find("IGNORE") == std::string::npos && sql.find("ON DUPLICATE") == std::string::npos &&
              sql.find("REPLACE") == std::string::npos,
          "creation attempted to hide a duplicate or overwrite an existing entry");
    check(params == std::vector<DbValue>{database.directory_name, database.username,
                                         database.selected_parent, new_path()},
          "creation inserted the wrong name, user, parent, or path");
    maybe_throw(3);
    bool duplicate = false;
    for (const auto& entry : database.pending) {
        // 模拟 sibling 唯一索引：文件和目录共享同一名称空间。
        if (entry.username == *params[1] && entry.parent == *params[2] && entry.name == *params[0])
            duplicate = true;
    }
    const auto actual = duplicate ? DbExecuteResult{false, 1062, 0, 0} : DbExecuteResult{true, 0, 1, 500};
    const auto reply = database.insert_reply.value_or(actual);
    if (reply.success && reply.affected_rows != 0) {
        database.pending.push_back({"500", *params[1], *params[2], *params[0], *params[3],
                                     "directory", std::nullopt});
    }
    maybe_throw(4);
    return reply;
}

DbTransaction::DbTransaction(DbConnection& connection) noexcept
    : connection_(connection), active_(database.begin_ok && !connection._transaction_active) {
    ++database.begins;
    if (active_) {
        connection_._transaction_active = true;
        database.owner = &connection_;
        database.pending = database.stored;
    }
}
DbTransaction::~DbTransaction() noexcept { rollback(); }
DbCommitStatus DbTransaction::commit() noexcept {
    if (!active_)
        return DbCommitStatus::k_not_active;
    ++database.commits;
    if (database.commit_reply == DbCommitStatus::k_committed ||
        (database.commit_reply == DbCommitStatus::k_outcome_unknown && database.unknown_applies))
        database.stored = database.pending;
    // 提交结果未知也结束本地句柄，不能通过补发回滚断言远端没有提交。
    database.pending = database.stored;
    database.owner = nullptr;
    connection_._transaction_active = false;
    active_ = false;
    return database.commit_reply;
}
bool DbTransaction::rollback() noexcept {
    if (active_) {
        ++database.rollbacks;
        database.pending = database.stored;
        database.owner = nullptr;
        connection_._transaction_active = false;
        active_ = false;
    }
    return true;
}

} // namespace TLSS::BASE

namespace {

void test_invalid() {
    for (const auto& name : std::vector<std::string>{"", ".", "..", "/", "a/b", "a\\b",
             std::string(256, 'x'), std::string("a\0b", 3), "a\nb", std::string(1, '\x7f')}) {
        reset();
        database.directory_name = name;
        const auto before = database.stored;
        check(execute() == CreateDirectoryResult::failed, "creation accepted an invalid directory name");
        check(database.begins == 0 && database.queries == 0 && database.inserts == 0,
              "invalid creation arguments accessed the database");
        check_finished(0, 0);
        check_unchanged(before);
    }
    reset();
    database.username.clear();
    check(execute() == CreateDirectoryResult::failed && database.begins == 0, "empty username accepted");
    reset();
    database.current_directory.clear();
    check(execute() == CreateDirectoryResult::failed && database.begins == 0, "empty parent path accepted");
}

void test_paths() {
    for (const std::size_t size : {std::size_t{1024}, std::size_t{1025}}) {
        reset();
        database.current_directory = "/" + std::string(size - database.directory_name.size() - 2, 'd');
        database.stored[1].path = database.current_directory;
        const auto before = database.stored;
        check(execute() == (size == 1024 ? CreateDirectoryResult::committed : CreateDirectoryResult::failed),
              "creation full path length boundary was handled incorrectly");
        check(database.begins == (size == 1024 ? 1 : 0), "overlong path began a transaction");
        check_finished(size == 1024 ? 1 : 0, 0);
        check_created(before, size == 1024);
    }
    reset();
    database.directory_name = std::string(255, 'x');
    const auto before = database.stored;
    check(execute() == CreateDirectoryResult::committed, "255-byte directory name rejected");
    check_created(before, true);
}

void test_begin(bool nested) {
    reset();
    const auto before = database.stored;
    if (!nested) {
        database.begin_ok = false;
        check(execute() == CreateDirectoryResult::failed && database.queries == 0,
              "creation continued after failed BEGIN");
        check_finished(0, 0);
    } else {
        DbConnection connection;
        {
            DbTransaction outer(connection);
            check(create_directory_transaction(connection, "alice", "/docs", "drafts") ==
                      CreateDirectoryResult::failed && outer.active() && database.owner == &connection,
                  "nested creation disturbed the caller transaction");
            check(database.queries == 0 && database.commits == 0 && database.rollbacks == 0,
                  "nested creation queried or ended the caller transaction");
        }
        check_finished(0, 1);
    }
    check_unchanged(before);
}

void test_query_results(int stage) {
    for (const auto& reply : std::vector<DbQueryResult>{{false, {}}, {false, {{"7"}}}, {true, {}},
             {true, {{}}}, {true, {{std::nullopt}}}, {true, {{"7", "extra"}}}, {true, {{"7"}, {"8"}}}}) {
        reset();
        const auto before = database.stored;
        database.replies[stage - 1] = reply;
        check(execute() == CreateDirectoryResult::failed, "creation accepted a failed or malformed query");
        check(database.queries == stage && database.inserts == 0, "invalid query continued to INSERT");
        check_finished(0, 1);
        check_unchanged(before);
    }
}

void test_invalid_parents() {
    for (int mode = 0; mode < 3; ++mode) {
        reset();
        if (mode == 0) database.stored[1].path = "/somewhere_else";
        if (mode == 1) database.stored[1].username = "another_user";
        if (mode == 2) database.stored[1].kind = "file";
        const auto before = database.stored;
        check(execute() == CreateDirectoryResult::failed && database.inserts == 0,
              "creation used a missing, foreign, or non-directory parent");
        check_finished(0, 1);
        check_unchanged(before);
    }
}

void test_duplicate(const std::string& kind) {
    reset();
    database.stored.push_back({"400", "alice", "12", "drafts", "/docs/drafts", kind,
                               kind == "file" ? DbValue{std::string(64, 'a')} : std::nullopt});
    const auto before = database.stored;
    check(execute() == CreateDirectoryResult::failed && database.inserts == 1,
          "creation accepted or overwrote a same-name file/directory");
    check_finished(0, 1);
    check_unchanged(before);
}

void test_insert_results() {
    for (const auto& reply : {DbExecuteResult{false, 1213, 0, 0}, DbExecuteResult{false, 1213, 1, 0},
                              DbExecuteResult{true, 0, 0, 0}, DbExecuteResult{true, 0, 2, 500}}) {
        reset();
        const auto before = database.stored;
        database.insert_reply = reply;
        check(execute() == CreateDirectoryResult::failed && database.inserts == 1,
              "creation accepted invalid affected rows or retried INSERT");
        check_finished(0, 1);
        check_unchanged(before);
    }
}

void test_commit(DbCommitStatus status, bool applied) {
    reset();
    const auto before = database.stored;
    database.commit_reply = status;
    database.unknown_applies = applied;
    const auto expected = status == DbCommitStatus::k_committed ? CreateDirectoryResult::committed :
                          status == DbCommitStatus::k_not_active ? CreateDirectoryResult::failed :
                                                                  CreateDirectoryResult::outcome_unknown;
    check(execute() == expected, "creation commit outcome mapped incorrectly");
    check(database.begins == 1 && database.queries == 2 && database.inserts == 1,
          "creation skipped an operation or retried");
    check_finished(1, 0);
    check_created(before, applied);
}

void test_root_or_quoted(bool root) {
    reset();
    if (root) database.current_directory = "/";
    else database.directory_name = "notes '2026'";
    const auto before = database.stored;
    check(execute() == CreateDirectoryResult::committed, "root parent or quoted name creation failed");
    check_finished(1, 0);
    check_created(before, true, root ? "1" : "12");
}

void test_parent_change(bool replace) {
    reset();
    auto expected = database.stored;
    move_parent(expected, replace);
    database.parent_change_on_lock = replace ? 2 : 1;
    const auto result = execute();
    check(result == (replace ? CreateDirectoryResult::committed : CreateDirectoryResult::failed),
          "creation used a parent state read before acquiring the user lock");
    check(database.queries == 2 && database.inserts == (replace ? 1 : 0),
          "creation inserted into the old parent or retried after its change");
    check_finished(replace ? 1 : 0, replace ? 0 : 1);
    check_created(expected, replace, "99");
}

void test_exceptions() {
    for (int stage = 1; stage <= 4; ++stage) {
        for (const bool nonstandard : {false, true}) {
            reset();
            const auto before = database.stored;
            database.throw_at = stage;
            database.nonstandard_exception = nonstandard;
            bool propagated = false;
            try {
                execute();
            } catch (const std::runtime_error& error) {
                propagated = !nonstandard && std::string(error.what()) == "injected creation database exception";
            } catch (int value) {
                propagated = nonstandard && value == 7;
            }
            check(propagated, "creation swallowed or replaced a database exception");
            check_finished(0, 1);
            check_unchanged(before);
        }
    }
}

void test_repeat() {
    reset();
    check(execute() == CreateDirectoryResult::committed, "initial creation failed");
    const auto before = database.stored;
    database.begins = database.queries = database.inserts = database.commits = database.rollbacks = 0;
    check(execute() == CreateDirectoryResult::failed && database.inserts == 1,
          "repeated creation was reported successful or retried");
    check_finished(0, 1);
    check_unchanged(before);
}

} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 2, "expected one scenario argument");
        const std::string scenario(argv[1]);
        if (scenario == "invalid_arguments") test_invalid();
        else if (scenario == "path_limits") test_paths();
        else if (scenario == "begin_failure") test_begin(false);
        else if (scenario == "existing_transaction") test_begin(true);
        else if (scenario == "user_results") test_query_results(1);
        else if (scenario == "parent_results") test_query_results(2);
        else if (scenario == "invalid_parents") test_invalid_parents();
        else if (scenario == "duplicate_file") test_duplicate("file");
        else if (scenario == "duplicate_directory") test_duplicate("directory");
        else if (scenario == "insert_results") test_insert_results();
        else if (scenario == "committed") test_commit(DbCommitStatus::k_committed, true);
        else if (scenario == "root_parent") test_root_or_quoted(true);
        else if (scenario == "quoted_name") test_root_or_quoted(false);
        else if (scenario == "unknown_applied") test_commit(DbCommitStatus::k_outcome_unknown, true);
        else if (scenario == "unknown_not_applied") test_commit(DbCommitStatus::k_outcome_unknown, false);
        else if (scenario == "commit_not_active") test_commit(DbCommitStatus::k_not_active, false);
        else if (scenario == "parent_moved") test_parent_change(false);
        else if (scenario == "parent_replaced") test_parent_change(true);
        else if (scenario == "exceptions") test_exceptions();
        else if (scenario == "repeat") test_repeat();
        else throw std::invalid_argument("unknown scenario: " + scenario);
        std::cout << "PASS directory creation " << scenario << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
