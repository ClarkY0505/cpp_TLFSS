#include "ftp/directory_deletion.h"

#include <algorithm>
#include <array>
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
    bool operator==(const Entry& other) const {
        return std::tie(id, username, parent, name, path, kind) ==
               std::tie(other.id, other.username, other.parent, other.name, other.path, other.kind);
    }
};

// 真实删除事务函数配合数据库替代层：pending 是事务工作副本，stored 是已提交状态。
// 不连接 MySQL，不验证真实 InnoDB 的锁等待，也不操作对象目录。
struct Database {
    std::string username = "alice", current_directory = "/docs", directory_name = "drafts";
    std::vector<Entry> stored{
        {"1", "alice", "-1", "/", "/", "directory"},
        {"12", "alice", "1", "docs", "/docs", "directory"},
        {"128", "alice", "12", "drafts", "/docs/drafts", "directory"},
        {"129", "alice", "12", "keep", "/docs/keep", "directory"},
        {"13", "alice", "1", "other", "/other", "directory"},
        {"130", "alice", "13", "drafts", "/other/drafts", "directory"},
        {"131", "alice", "12", "notes.txt", "/docs/notes.txt", "file"},
        {"2", "bob", "-1", "/", "/", "directory"},
        {"22", "bob", "2", "docs", "/docs", "directory"},
        {"228", "bob", "22", "drafts", "/docs/drafts", "directory"}};
    std::vector<Entry> pending = stored;
    DbConnection* owner = nullptr;
    bool begin_ok = true, unknown_applies = false, nonstandard_exception = false;
    std::array<std::optional<DbQueryResult>, 4> replies;
    std::optional<DbExecuteResult> delete_reply;
    DbCommitStatus commit_reply = DbCommitStatus::k_committed;
    int begins = 0, queries = 0, deletes = 0, commits = 0, rollbacks = 0;
    int throw_at = 0; // 1~4 查询；5 DELETE 前；6 DELETE 已改变工作副本后。
    std::string selected_parent, selected_target;
} database;

void reset() { database = Database{}; }

std::string target_path() {
    return database.current_directory == "/" ? "/" + database.directory_name :
           database.current_directory + "/" + database.directory_name;
}

void maybe_throw(int stage) {
    if (database.throw_at == stage) {
        if (database.nonstandard_exception)
            throw 7;
        throw std::runtime_error("injected directory database exception");
    }
}

void require_sql(const std::string& sql, const std::vector<std::string>& fragments) {
    for (const auto& fragment : fragments)
        check(sql.find(fragment) != std::string::npos, "required SQL guard is missing");
    check(sql.find("storage_objects") == std::string::npos && sql.find("object_hash") == std::string::npos,
          "empty directory deletion accessed the object catalog");
    check(sql.find("alice") == std::string::npos && sql.find("drafts") == std::string::npos,
          "SQL interpolated user input");
}

void remember_id(const DbQueryResult& reply, std::string& destination) {
    if (reply.success && reply.rows.size() == 1 && reply.rows[0].size() == 1 && reply.rows[0][0])
        destination = *reply.rows[0][0];
}

DeleteDirectoryResult execute() {
    DbConnection connection;
    return delete_empty_directory_transaction(connection, database.username,
                                               database.current_directory, database.directory_name);
}

void check_finished(int commits, int rollbacks) {
    check(database.commits == commits && database.rollbacks == rollbacks,
          "unexpected commit or rollback count");
    check(database.owner == nullptr && database.pending == database.stored,
          "transaction remained active or left pending changes");
}

void check_unchanged(const std::vector<Entry>& before) {
    check(database.stored == before, "failed/refused deletion changed committed records");
}

void check_committed_records(const std::vector<Entry>& before, bool applied) {
    auto expected = before;
    if (applied) {
        expected.erase(std::remove_if(expected.begin(), expected.end(), [](const Entry& entry) {
            return entry.id == "128";
        }), expected.end());
    }
    check(database.stored == expected, "deletion changed another user's record, parent, or sibling");
}

} // namespace

namespace TLSS::BASE {

DbConnection::DbConnection()
    : _conn(nullptr), _connected(false), _transaction_active(false) {}
DbConnection::~DbConnection() = default;

DbQueryResult DbConnection::query_prepared(
    const std::string& sql, const std::vector<DbValue>& params) {
    check(_transaction_active && database.owner == this, "query used the wrong transaction connection");
    const int stage = ++database.queries;
    check(stage <= 4 && database.deletes == 0, "query was retried or ran after DELETE");
    require_sql(sql, {"FOR UPDATE"});
    maybe_throw(stage);
    DbQueryResult actual{true, {}};
    if (stage == 1) {
        require_sql(sql, {"SELECT id FROM users", "username = ?", "is_deleted = 0"});
        check(params == std::vector<DbValue>{database.username}, "user lookup parameters changed");
        actual.rows = {{"7"}};
    } else if (stage == 2) {
        require_sql(sql, {"SELECT id FROM virtual_files", "username = ?", "path = ?",
                          "file_type = 'directory'"});
        check(params == std::vector<DbValue>{database.username, database.current_directory},
              "parent query did not identify the current user's directory");
        for (const auto& entry : database.pending) {
            if (entry.username == *params[0] && entry.path == *params[1] && entry.kind == "directory")
                actual.rows.push_back({entry.id});
        }
    } else if (stage == 3) {
        require_sql(sql, {"SELECT id FROM virtual_files", "username = ?", "parent_id = ?",
                          "file_name = ?", "CAST(path AS BINARY) = CAST(? AS BINARY)", "file_type = 'directory'",
                          "parent_id > 0", "path <> '/'"});
        check(params == std::vector<DbValue>{database.username, database.selected_parent,
                                             database.directory_name, target_path()},
              "target query parameters changed the user, parent, name, or path");
        for (const auto& entry : database.pending) {
            if (entry.username == *params[0] && entry.parent == *params[1] &&
                entry.name == *params[2] && entry.path == *params[3] && entry.kind == "directory" &&
                entry.parent != "-1" && entry.path != "/")
                actual.rows.push_back({entry.id});
        }
    } else {
        require_sql(sql, {"SELECT id FROM virtual_files", "username = ?", "parent_id = ?", "LIMIT 1"});
        check(sql.find("file_type") == std::string::npos,
              "emptiness query excluded files or subdirectories");
        check(params == std::vector<DbValue>{database.username, database.selected_target},
              "emptiness query checked another directory");
        for (const auto& entry : database.pending) {
            if (entry.username == *params[0] && entry.parent == *params[1]) {
                actual.rows.push_back({entry.id});
                break;
            }
        }
    }
    const auto reply = database.replies[stage - 1].value_or(actual);
    if (stage == 2) remember_id(reply, database.selected_parent);
    if (stage == 3) remember_id(reply, database.selected_target);
    return reply;
}

DbExecuteResult DbConnection::execute_prepared(
    const std::string& sql, const std::vector<DbValue>& params) {
    check(_transaction_active && database.owner == this, "DELETE used a different connection");
    check(database.queries == 4 && ++database.deletes == 1, "DELETE preceded emptiness check or was retried");
    require_sql(sql, {"DELETE FROM virtual_files", "id = ?", "username = ?", "parent_id = ?",
                      "file_name = ?", "CAST(path AS BINARY) = CAST(? AS BINARY)",
                      "file_type = 'directory'", "parent_id > 0", "path <> '/'"});
    check(params == std::vector<DbValue>{database.selected_target, database.username,
                                         database.selected_parent, database.directory_name, target_path()},
          "DELETE did not identify the locked directory");
    maybe_throw(5);
    auto& entries = database.pending;
    const auto matches = [&](const Entry& entry) {
        return entry.id == *params[0] && entry.username == *params[1] && entry.parent == *params[2] &&
               entry.name == *params[3] && entry.path == *params[4] && entry.kind == "directory" &&
               entry.parent != "-1" && entry.path != "/";
    };
    const auto count = static_cast<std::uint64_t>(std::count_if(entries.begin(), entries.end(), matches));
    const auto reply = database.delete_reply.value_or(DbExecuteResult{true, 0, count, 0});
    if (reply.success && reply.affected_rows != 0) {
        entries.erase(std::remove_if(entries.begin(), entries.end(), matches), entries.end());
        // 异常多行结果也模拟产生工作副本变化，确认上层会整体回滚。
        if (reply.affected_rows > 1 && !entries.empty())
            entries.pop_back();
    }
    maybe_throw(6);
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
    // 未知提交结束本地句柄，不再用 ROLLBACK 假装撤销远端可能已经完成的提交。
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
        check(execute() == DeleteDirectoryResult::failed, "invalid directory name accepted");
        check(database.begins == 0 && database.queries == 0, "invalid name accessed the database");
        check_unchanged(before);
    }
    reset();
    database.username.clear();
    check(execute() == DeleteDirectoryResult::failed && database.begins == 0, "empty user accepted");
    reset();
    database.current_directory.clear();
    check(execute() == DeleteDirectoryResult::failed && database.begins == 0, "empty path accepted");
}

void test_path_limits() {
    for (const std::size_t size : {std::size_t{1024}, std::size_t{1025}}) {
        reset();
        database.current_directory = "/" + std::string(size - database.directory_name.size() - 2, 'd');
        database.stored[1].path = database.current_directory;
        database.stored[2].path = target_path();
        // 这是调用前的初始数据；超长路径不创建事务，因此也不会复制 stored。
        database.pending = database.stored;
        const auto before = database.stored;
        check(execute() == (size == 1024 ? DeleteDirectoryResult::committed : DeleteDirectoryResult::failed),
              "full path boundary handled incorrectly");
        check_committed_records(before, size == 1024);
        check_finished(size == 1024 ? 1 : 0, 0);
        check(database.begins == (size == 1024 ? 1 : 0), "overlong path began a transaction");
    }
    reset();
    database.directory_name = std::string(255, 'x');
    database.stored[2].name = database.directory_name;
    database.stored[2].path = target_path();
    check(execute() == DeleteDirectoryResult::committed, "255-byte directory name rejected");
}

void test_transaction(bool nested) {
    reset();
    const auto before = database.stored;
    if (!nested) {
        database.begin_ok = false;
        check(execute() == DeleteDirectoryResult::failed && database.queries == 0,
              "failed BEGIN continued to directory queries");
        check_finished(0, 0);
    } else {
        DbConnection connection;
        {
            DbTransaction outer(connection);
            check(delete_empty_directory_transaction(connection, "alice", "/docs", "drafts") ==
                      DeleteDirectoryResult::failed && outer.active() && database.owner == &connection,
                  "nested deletion disturbed the caller transaction");
            check(database.queries == 0 && database.commits == 0 && database.rollbacks == 0,
                  "nested deletion executed SQL or ended the caller transaction");
        }
        check_finished(0, 1);
    }
    check_unchanged(before);
}

void test_query_results(int stage) {
    for (const auto& reply : std::vector<DbQueryResult>{{false, {}}, {false, {{"7"}}},
             {true, {{}}}, {true, {{std::nullopt}}}, {true, {{"7", "extra"}}}, {true, {{"7"}, {"8"}}}}) {
        reset();
        const auto before = database.stored;
        database.replies[stage - 1] = reply;
        check(execute() == DeleteDirectoryResult::failed, "failed/malformed query accepted");
        check(database.queries == stage && database.deletes == 0, "invalid query continued to DELETE");
        check_finished(0, 1);
        check_unchanged(before);
    }
    if (stage < 4) {
        reset();
        database.replies[stage - 1] = DbQueryResult{true, {}};
        const auto before = database.stored;
        check(execute() == DeleteDirectoryResult::failed, "missing user/directory accepted");
        check(database.deletes == 0, "missing target still issued DELETE");
        check_finished(0, 1);
        check_unchanged(before);
    }
}

void test_nonempty(const std::string& kind) {
    reset();
    database.stored.push_back({"500", "alice", "128", "child", "/docs/drafts/child", kind});
    const auto before = database.stored;
    check(execute() == DeleteDirectoryResult::not_empty, "nonempty directory was not refused");
    check(database.queries == 4 && database.deletes == 0, "nonempty directory issued DELETE");
    check_finished(0, 1);
    check_unchanged(before);
}

void test_targets() {
    // 父目录不存在、目标不存在、父节点是文件，以及同名目标实际为文件。
    for (int mode = 0; mode < 4; ++mode) {
        reset();
        if (mode == 0) database.stored.erase(database.stored.begin() + 1);
        if (mode == 1) database.stored.erase(database.stored.begin() + 2);
        if (mode == 2) database.stored[1].kind = "file";
        if (mode == 3) database.stored[2].kind = "file";
        const auto before = database.stored;
        check(execute() == DeleteDirectoryResult::failed && database.deletes == 0,
              "missing target or non-directory was deleted");
        check_finished(0, 1);
        check_unchanged(before);
    }
}

void test_root() {
    reset();
    database.current_directory = "/";
    database.directory_name = "/";
    const auto root_before = database.stored;
    check(execute() == DeleteDirectoryResult::failed && database.begins == 0,
          "root deletion was accepted");
    check_unchanged(root_before);
    // 从根目录删除普通子目录是合法操作；这里将目标放到用户根下。
    reset();
    database.current_directory = "/";
    database.stored[2].parent = "1";
    database.stored[2].path = "/drafts";
    const auto before = database.stored;
    check(execute() == DeleteDirectoryResult::committed, "root's ordinary child could not be deleted");
    check_committed_records(before, true);
    check_finished(1, 0);
}

void test_exact_path() {
    // 目标路径是缓存协调范围的一部分，不能按忽略大小写的等价路径删除另一条记录。
    // 本替代层验证精确路径和 SQL 中的二进制比较；真实 MySQL 排序规则另行验证。
    for (int mode = 0; mode < 2; ++mode) {
        reset();
        if (mode == 0) database.directory_name = "DRAFTS";
        else database.stored[2].path = "/docs/DRAFTS";
        const auto before = database.stored;
        check(execute() == DeleteDirectoryResult::failed && database.deletes == 0,
              "directory deletion accepted a different byte spelling of its target path");
        check_finished(0, 1);
        check_unchanged(before);
    }
}

void test_delete_results() {
    for (const auto& reply : {DbExecuteResult{false, 1213, 0, 0}, DbExecuteResult{false, 1213, 1, 0},
                              DbExecuteResult{true, 0, 0, 0}, DbExecuteResult{true, 0, 2, 0}}) {
        reset();
        const auto before = database.stored;
        database.delete_reply = reply;
        check(execute() == DeleteDirectoryResult::failed && database.deletes == 1,
              "invalid affected rows accepted or DELETE retried");
        check_finished(0, 1);
        check_unchanged(before);
    }
}

void test_commit(DbCommitStatus status, bool applied) {
    reset();
    const auto before = database.stored;
    database.commit_reply = status;
    database.unknown_applies = applied;
    const auto expected = status == DbCommitStatus::k_committed ? DeleteDirectoryResult::committed :
                          status == DbCommitStatus::k_not_active ? DeleteDirectoryResult::failed :
                                                                  DeleteDirectoryResult::outcome_unknown;
    check(execute() == expected, "directory commit outcome mapped incorrectly");
    check(database.begins == 1 && database.queries == 4 && database.deletes == 1,
          "directory deletion skipped operations or retried");
    check_finished(1, 0);
    check_committed_records(before, applied);
}

void test_exceptions() {
    for (int stage = 1; stage <= 6; ++stage) {
        for (const bool nonstandard : {false, true}) {
            reset();
            const auto before = database.stored;
            database.throw_at = stage;
            database.nonstandard_exception = nonstandard;
            bool propagated = false;
            try {
                execute();
            } catch (const std::runtime_error& error) {
                propagated = !nonstandard && std::string(error.what()) == "injected directory database exception";
            } catch (int value) {
                propagated = nonstandard && value == 7;
            }
            check(propagated, "directory deletion swallowed or replaced an exception");
            check_finished(0, 1);
            check_unchanged(before);
        }
    }
}

void test_repeat() {
    reset();
    check(execute() == DeleteDirectoryResult::committed, "first directory deletion failed");
    const auto before = database.stored;
    database.begins = database.queries = database.deletes = database.commits = database.rollbacks = 0;
    check(execute() == DeleteDirectoryResult::failed && database.queries == 3 && database.deletes == 0,
          "already deleted directory was deleted or committed again");
    check_finished(0, 1);
    check_unchanged(before);
}

} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 2, "expected one scenario argument");
        const std::string scenario(argv[1]);
        if (scenario == "invalid_arguments") test_invalid();
        else if (scenario == "path_limits") test_path_limits();
        else if (scenario == "begin_failure") test_transaction(false);
        else if (scenario == "existing_transaction") test_transaction(true);
        else if (scenario == "user_results") test_query_results(1);
        else if (scenario == "parent_results") test_query_results(2);
        else if (scenario == "target_results") test_query_results(3);
        else if (scenario == "children_results") test_query_results(4);
        else if (scenario == "nonempty_file") test_nonempty("file");
        else if (scenario == "nonempty_directory") test_nonempty("directory");
        else if (scenario == "invalid_targets") test_targets();
        else if (scenario == "root_protection") test_root();
        else if (scenario == "exact_path") test_exact_path();
        else if (scenario == "delete_results") test_delete_results();
        else if (scenario == "committed") test_commit(DbCommitStatus::k_committed, true);
        else if (scenario == "unknown_applied") test_commit(DbCommitStatus::k_outcome_unknown, true);
        else if (scenario == "unknown_not_applied") test_commit(DbCommitStatus::k_outcome_unknown, false);
        else if (scenario == "commit_not_active") test_commit(DbCommitStatus::k_not_active, false);
        else if (scenario == "exceptions") test_exceptions();
        else if (scenario == "repeat") test_repeat();
        else throw std::invalid_argument("unknown scenario: " + scenario);
        std::cout << "PASS directory deletion " << scenario << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
