#include "ftp/directory_deletion.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#include <iostream>

namespace {

using namespace TLSS::BASE;
using namespace TLSS::FileStorage;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Entry {
    std::string id, user, parent, name, path, type;
    bool operator==(const Entry& other) const {
        return std::tie(id, user, parent, name, path, type) ==
               std::tie(other.id, other.user, other.parent, other.name, other.path, other.type);
    }
};

// 执行真实递归删除函数，仅替代数据库连接和事务边界。
// stored 为已提交数据；pending 为当前事务的工作副本。
// 不连接 MySQL，不模拟 InnoDB 锁等待，不操作磁盘对象。
struct Database {
    std::string user = "alice", directory = "/docs", name = "drafts";
    std::vector<Entry> stored{
        {"1", "alice", "-1", "/", "/", "directory"},
        {"12", "alice", "1", "docs", "/docs", "directory"},
        {"128", "alice", "12", "drafts", "/docs/drafts", "directory"},
        {"301", "alice", "128", "notes.txt", "/docs/drafts/notes.txt", "file"},
        {"302", "alice", "128", "sub", "/docs/drafts/sub", "directory"},
        {"303", "alice", "128", "empty", "/docs/drafts/empty", "directory"},
        {"304", "alice", "302", "item.bin", "/docs/drafts/sub/item.bin", "file"},
        {"305", "alice", "302", "leaf", "/docs/drafts/sub/leaf", "directory"},
        {"306", "alice", "305", "deep.txt", "/docs/drafts/sub/leaf/deep.txt", "file"},
        {"129", "alice", "12", "drafts-copy", "/docs/drafts-copy", "directory"},
        {"401", "alice", "129", "keep.txt", "/docs/drafts-copy/keep.txt", "file"},
        {"13", "alice", "1", "other", "/other", "directory"},
        {"130", "alice", "13", "drafts", "/other/drafts", "directory"},
        {"402", "alice", "130", "keep.txt", "/other/drafts/keep.txt", "file"},
        {"2", "bob", "-1", "/", "/", "directory"},
        {"22", "bob", "2", "docs", "/docs", "directory"},
        {"228", "bob", "22", "drafts", "/docs/drafts", "directory"},
        {"403", "bob", "228", "keep.txt", "/docs/drafts/keep.txt", "file"}};
    std::vector<Entry> pending = stored;
    DbConnection* owner = nullptr;
    bool begin_ok = true, unknown_applies = false, nonstandard = false;
    DbCommitStatus commit_reply = DbCommitStatus::k_committed;
    std::map<int, DbQueryResult> query_replies;
    std::map<std::string, DbQueryResult> children_replies;
    int fail_query_at = 0, throw_query_at = 0;
    int override_delete_at = 0, throw_delete_at = 0;
    bool throw_after_delete = false;
    DbExecuteResult delete_reply{false, 1213, 0, 0};
    int begins = 0, queries = 0, deletes = 0, commits = 0, rollbacks = 0;
    std::string selected_parent, selected_target;
    std::vector<std::string> expanded, attempted_deletes;
} database;

void reset() { database = Database{}; }

std::string target_path() {
    return database.directory == "/" ? "/" + database.name : database.directory + "/" + database.name;
}

void require_sql(const std::string& sql, const std::vector<std::string>& fragments) {
    for (const auto& fragment : fragments)
        check(sql.find(fragment) != std::string::npos, "tree SQL omitted a required guard");
    check(sql.find("storage_objects") == std::string::npos && sql.find("object_hash") == std::string::npos,
          "tree deletion accessed the object catalog");
    check(sql.find(database.user) == std::string::npos && sql.find(database.name) == std::string::npos,
          "tree SQL interpolated user input");
}

void remember_id(const DbQueryResult& result, std::string& output) {
    if (result.success && result.rows.size() == 1 && result.rows[0].size() == 1 && result.rows[0][0])
        output = *result.rows[0][0];
}

[[noreturn]] void inject_exception(const char* message) {
    if (database.nonstandard) throw 7;
    throw std::runtime_error(message);
}

DeleteDirectoryTreeResult execute() {
    DbConnection connection;
    return delete_directory_tree_transaction(connection, database.user, database.directory, database.name);
}

void finished(int commits, int rollbacks) {
    check(database.commits == commits && database.rollbacks == rollbacks,
          "tree transaction committed, retried, or rolled back an unexpected number of times");
    check(database.owner == nullptr && database.pending == database.stored,
          "tree transaction remained active or left uncommitted changes");
}

// 期望结果按 parent_id 关系计算，不用路径前缀，避免把 /drafts-copy 当作 /drafts 的后代。
std::set<std::string> subtree_ids(const std::vector<Entry>& entries) {
    std::set<std::string> ids{"128"};
    std::vector<std::string> pending{"128"};
    for (std::size_t index = 0; index < pending.size(); ++index) {
        const auto parent = pending[index];
        for (const auto& entry : entries) {
            if (entry.user == database.user && entry.parent == parent && ids.insert(entry.id).second)
                pending.push_back(entry.id);
        }
    }
    return ids;
}

void check_records(const std::vector<Entry>& before, bool applied) {
    auto expected = before;
    if (applied) {
        const auto ids = subtree_ids(before);
        expected.erase(std::remove_if(expected.begin(), expected.end(), [&](const Entry& entry) {
            return entry.user == database.user && ids.count(entry.id) != 0;
        }), expected.end());
    }
    check(database.stored == expected, "tree deletion changed the wrong committed records");
}

void check_child_first(const std::vector<Entry>& before) {
    const auto ids = subtree_ids(before);
    check(database.attempted_deletes.size() == ids.size() && database.attempted_deletes.back() == "128",
          "tree deletion omitted nodes or did not delete its root last");
    std::map<std::string, std::size_t> positions;
    for (std::size_t index = 0; index < database.attempted_deletes.size(); ++index)
        check(positions.emplace(database.attempted_deletes[index], index).second, "tree DELETE was retried");
    for (const auto& entry : before) {
        if (ids.count(entry.id) && ids.count(entry.parent))
            check(positions.at(entry.id) < positions.at(entry.parent), "parent was deleted before its child");
    }
}

void empty_target() {
    const auto ids = subtree_ids(database.stored);
    database.stored.erase(std::remove_if(database.stored.begin(), database.stored.end(), [&](const Entry& entry) {
        return entry.id != "128" && ids.count(entry.id) != 0;
    }), database.stored.end());
    database.pending = database.stored;
}

void rename_target(const std::string& new_path) {
    const std::string old_path = "/docs/drafts";
    const auto ids = subtree_ids(database.stored);
    for (auto& entry : database.stored) {
        if (ids.count(entry.id)) entry.path = new_path + entry.path.substr(old_path.size());
        if (entry.id == "128") entry.name = database.name;
    }
    database.pending = database.stored;
}

} // namespace

namespace TLSS::BASE {

DbConnection::DbConnection() : _conn(nullptr), _connected(false), _transaction_active(false) {}
DbConnection::~DbConnection() = default;

DbQueryResult DbConnection::query_prepared(const std::string& sql, const std::vector<DbValue>& params) {
    check(_transaction_active && database.owner == this, "tree query used another connection or no transaction");
    const int stage = ++database.queries;
    check(database.deletes == 0, "tree deletion began before subtree enumeration finished");
    require_sql(sql, {"FOR UPDATE"});
    if (stage == database.throw_query_at) inject_exception("injected tree query exception");
    if (stage == database.fail_query_at) return {};
    DbQueryResult actual{true, {}};
    if (stage == 1) {
        require_sql(sql, {"SELECT id FROM users", "username = ?", "is_deleted = 0"});
        check(params == std::vector<DbValue>{database.user}, "tree deletion did not lock its user first");
        actual.rows = {{"7"}};
    } else if (stage == 2) {
        require_sql(sql, {"SELECT id FROM virtual_files", "username = ?", "path = ?", "file_type = 'directory'"});
        check(params == std::vector<DbValue>{database.user, database.directory}, "tree parent lookup changed scope");
        for (const auto& entry : database.pending) {
            if (entry.user == *params[0] && entry.path == *params[1] && entry.type == "directory")
                actual.rows.push_back({entry.id});
        }
    } else if (stage == 3) {
        require_sql(sql, {"SELECT id FROM virtual_files", "username = ?", "parent_id = ?", "file_name = ?",
                          "CAST(path AS BINARY) = CAST(? AS BINARY)", "file_type = 'directory'",
                          "parent_id > 0", "path <> '/'"});
        check(params == std::vector<DbValue>{database.user, database.selected_parent, database.name, target_path()},
              "tree target lookup changed user, parent, name, or exact path");
        for (const auto& entry : database.pending) {
            if (entry.user == *params[0] && entry.parent == *params[1] && entry.name == *params[2] &&
                entry.path == *params[3] && entry.type == "directory" && entry.parent != "-1" && entry.path != "/")
                actual.rows.push_back({entry.id});
        }
    } else {
        require_sql(sql, {"SELECT id, file_name, path, file_type FROM virtual_files", "username = ?", "parent_id = ?"});
        check(sql.find("LIMIT") == std::string::npos && sql.find("LIKE") == std::string::npos &&
                  sql.find("file_type =") == std::string::npos && params.size() == 2 && params[0] == database.user && params[1],
              "tree enumeration limited children, filtered files, or used a path prefix");
        const auto found = std::find_if(database.pending.begin(), database.pending.end(), [&](const Entry& entry) {
            return entry.id == *params[1] && entry.user == database.user && entry.type == "directory";
        });
        check(found != database.pending.end(), "tree enumeration expanded a file or an unowned directory");
        database.expanded.push_back(*params[1]);
        for (const auto& entry : database.pending) {
            if (entry.user == *params[0] && entry.parent == *params[1])
                actual.rows.push_back({entry.id, entry.name, entry.path, entry.type});
        }
        const auto override = database.children_replies.find(*params[1]);
        if (override != database.children_replies.end()) actual = override->second;
    }
    const auto override = database.query_replies.find(stage);
    const auto result = override == database.query_replies.end() ? actual : override->second;
    if (stage == 2) remember_id(result, database.selected_parent);
    if (stage == 3) remember_id(result, database.selected_target);
    return result;
}

DbExecuteResult DbConnection::execute_prepared(const std::string& sql, const std::vector<DbValue>& params) {
    check(_transaction_active && database.owner == this, "tree DELETE used another transaction connection");
    const int stage = ++database.deletes;
    require_sql(sql, {"DELETE FROM virtual_files", "id = ?", "username = ?", "parent_id = ?", "file_name = ?",
                      "CAST(path AS BINARY) = CAST(? AS BINARY)", "file_type = ?", "parent_id > 0", "path <> '/'"});
    check(params.size() == 6 && params[1] == database.user, "tree DELETE changed its owner or parameters");
    for (const auto& param : params) check(param.has_value(), "tree DELETE bound a NULL identity field");
    database.attempted_deletes.push_back(*params[0]);
    if (stage == database.throw_delete_at && !database.throw_after_delete)
        inject_exception("injected tree delete exception");
    const auto matches = [&](const Entry& entry) {
        return entry.id == *params[0] && entry.user == *params[1] && entry.parent == *params[2] &&
               entry.name == *params[3] && entry.path == *params[4] && entry.type == *params[5] &&
               entry.parent != "-1" && entry.path != "/";
    };
    const auto count = static_cast<std::uint64_t>(std::count_if(database.pending.begin(), database.pending.end(), matches));
    const auto reply = stage == database.override_delete_at ? database.delete_reply : DbExecuteResult{true, 0, count, 0};
    if (reply.success && reply.affected_rows != 0)
        database.pending.erase(std::remove_if(database.pending.begin(), database.pending.end(), matches), database.pending.end());
    // 也覆盖 DELETE 已改变事务副本后抛异常，验证前面所有删除仍会整体撤销。
    if (stage == database.throw_delete_at && database.throw_after_delete)
        inject_exception("injected tree delete exception");
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
    if (!active_) return DbCommitStatus::k_not_active;
    ++database.commits;
    if (database.commit_reply == DbCommitStatus::k_committed ||
        (database.commit_reply == DbCommitStatus::k_outcome_unknown && database.unknown_applies))
        database.stored = database.pending;
    // 未知结果结束客户端句柄；不假装随后 ROLLBACK 能撤销可能已经提交的删除。
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
    for (const auto& name : std::vector<std::string>{"", ".", "..", "/", "a/b", "a\\b", std::string(256, 'x'),
             std::string("a\0b", 3), "a\nb", std::string(1, '\x7f')}) {
        reset();
        database.name = name;
        const auto before = database.stored;
        check(execute() == DeleteDirectoryTreeResult::failed && database.begins == 0 && database.queries == 0,
              "invalid tree name accessed the database");
        check_records(before, false);
    }
    for (bool empty_user : {false, true}) {
        reset();
        if (empty_user) database.user.clear(); else database.directory.clear();
        check(execute() == DeleteDirectoryTreeResult::failed && database.begins == 0,
              "empty tree user or current directory accepted");
    }
}

void test_paths() {
    for (std::size_t size : {std::size_t{1024}, std::size_t{1025}}) {
        reset();
        empty_target();
        database.directory = "/" + std::string(size - database.name.size() - 2, 'd');
        for (auto& entry : database.stored) {
            if (entry.id == "12") entry.path = database.directory;
        }
        rename_target(target_path());
        const auto before = database.stored;
        check(execute() == (size == 1024 ? DeleteDirectoryTreeResult::committed : DeleteDirectoryTreeResult::failed),
              "tree path boundary handled incorrectly");
        check(database.begins == (size == 1024 ? 1 : 0), "overlong tree path began a transaction");
        check_records(before, size == 1024);
        finished(size == 1024 ? 1 : 0, 0);
    }
}

void test_transaction(bool existing) {
    reset();
    const auto before = database.stored;
    if (!existing) {
        database.begin_ok = false;
        check(execute() == DeleteDirectoryTreeResult::failed && database.queries == 0, "failed tree BEGIN continued");
        finished(0, 0);
    } else {
        DbConnection connection;
        {
            DbTransaction outer(connection);
            check(delete_directory_tree_transaction(connection, "alice", "/docs", "drafts") == DeleteDirectoryTreeResult::failed &&
                      outer.active() && database.owner == &connection && database.queries == 0 &&
                      database.commits == 0 && database.rollbacks == 0,
                  "tree deletion started inside or ended its caller's transaction");
        }
        finished(0, 1);
    }
    check_records(before, false);
}

void test_lookup(int stage) {
    for (const auto& reply : std::vector<DbQueryResult>{{false, {}}, {false, {{"7"}}}, {true, {}}, {true, {{}}},
             {true, {{std::nullopt}}}, {true, {{"7", "extra"}}}, {true, {{"7"}, {"8"}}}}) {
        reset();
        const auto before = database.stored;
        database.query_replies[stage] = reply;
        check(execute() == DeleteDirectoryTreeResult::failed && database.queries == stage && database.deletes == 0,
              "failed or malformed tree lookup proceeded to deletion");
        finished(0, 1);
        check_records(before, false);
    }
}

void test_success(const std::string& mode) {
    reset();
    if (mode == "empty_tree" || mode == "wide_tree" || mode == "deep_tree") empty_target();
    if (mode == "wide_tree") {
        for (int index = 0; index < 300; ++index) {
            const auto name = "file_" + std::to_string(index);
            database.stored.push_back({std::to_string(1000 + index), "alice", "128", name, target_path() + "/" + name, "file"});
        }
    }
    if (mode == "deep_tree") {
        std::string parent = "128", path = target_path();
        for (int index = 0; index < 250; ++index) {
            const auto id = std::to_string(1000 + index);
            path += "/d";
            database.stored.push_back({id, "alice", parent, "d", path, "directory"});
            parent = id;
        }
        database.stored.push_back({"2000", "alice", parent, "x", path + "/x", "file"});
    }
    if (mode == "literal_names") {
        database.name = "drafts_% ' quoted";
        rename_target(target_path());
    }
    if (mode == "root_child") {
        database.directory = "/";
        for (auto& entry : database.stored) if (entry.id == "128") entry.parent = "1";
        rename_target(target_path());
    }
    const auto before = database.stored;
    const auto ids = subtree_ids(before);
    const auto directories = std::count_if(before.begin(), before.end(), [&](const Entry& entry) {
        return ids.count(entry.id) != 0 && entry.type == "directory";
    });
    check(execute() == DeleteDirectoryTreeResult::committed, "valid tree deletion did not commit");
    check(database.begins == 1 && database.queries == 3 + directories &&
              static_cast<std::size_t>(database.deletes) == ids.size(), "tree enumeration skipped, retried, or omitted nodes");
    check_child_first(before);
    check_records(before, true);
    finished(1, 0);
}

void test_targets(bool exact_path) {
    for (int mode = 0; mode < (exact_path ? 2 : 4); ++mode) {
        reset();
        if (exact_path) {
            if (mode == 0) database.name = "DRAFTS";
            else database.stored[2].path = "/docs/DRAFTS";
        } else {
            if (mode == 0) database.stored.erase(database.stored.begin() + 1);
            if (mode == 1) database.stored.erase(database.stored.begin() + 2);
            if (mode == 2) database.stored[1].type = "file";
            if (mode == 3) database.stored[2].type = "file";
        }
        const auto before = database.stored;
        check(execute() == DeleteDirectoryTreeResult::failed && database.deletes == 0,
              "tree deletion accepted a missing, non-directory, or case-mismatched target");
        finished(0, 1);
        check_records(before, false);
    }
}

void test_children(bool values) {
    std::vector<DbQueryResult> replies;
    if (!values) {
        replies = {{false, {}}, {true, {{}}}, {true, {{"900"}}}, {true, {{"900", "x", "/docs/drafts/x"}}},
                   {true, {{"900", "x", "/docs/drafts/x", "file", "extra"}}}};
        for (std::size_t column = 0; column < 4; ++column) {
            DbQueryResult reply{true, {{"900", "x", "/docs/drafts/x", "file"}}};
            reply.rows[0][column] = std::nullopt;
            replies.push_back(reply);
        }
    } else {
        for (const auto& row : std::vector<std::vector<DbValue>>{
                 {"", "x", "/docs/drafts/x", "file"}, {"900", "", "/docs/drafts/", "file"},
                 {"900", "..", "/docs/drafts/..", "directory"}, {"900", "a/b", "/docs/drafts/a/b", "file"},
                 {"900", "x", "/docs/drafts/x", "unknown"}, {"900", "x", "/other/x", "file"},
                 {"900", "x", "/docs/drafts/nested/x", "file"}, {"900", "x", "/docs/DRAFTS/x", "file"},
                 {"900", "x", "/" + std::string(1024, 'x'), "file"}})
            replies.push_back({true, {{"901", "valid.txt", "/docs/drafts/valid.txt", "file"}, row}});
    }
    for (const auto& reply : replies) {
        reset();
        const auto before = database.stored;
        database.children_replies["128"] = reply;
        check(execute() == DeleteDirectoryTreeResult::failed && database.deletes == 0,
              "malformed child row or inconsistent child path reached DELETE");
        finished(0, 1);
        check_records(before, false);
    }
}

void test_duplicate_ids() {
    for (int mode = 0; mode < 3; ++mode) {
        reset();
        const auto before = database.stored;
        if (mode == 0)
            database.children_replies["128"] = {true, {{"128", "cycle", "/docs/drafts/cycle", "directory"}}};
        if (mode == 1)
            database.children_replies["128"] = {true, {{"301", "one", "/docs/drafts/one", "file"},
                                                       {"301", "two", "/docs/drafts/two", "file"}}};
        if (mode == 2)
            database.children_replies["302"] = {true, {{"301", "again", "/docs/drafts/sub/again", "file"}}};
        check(execute() == DeleteDirectoryTreeResult::failed && database.deletes == 0,
              "duplicate child ID or cyclic result was silently accepted");
        finished(0, 1);
        check_records(before, false);
    }
}

void test_query_failure() {
    for (int stage = 1; stage <= 7; ++stage) {
        reset();
        const auto before = database.stored;
        database.fail_query_at = stage;
        check(execute() == DeleteDirectoryTreeResult::failed && database.queries == stage && database.deletes == 0,
              "failed subtree enumeration continued to deletion");
        finished(0, 1);
        check_records(before, false);
    }
}

void test_delete_failure(const std::string& mode) {
    // 在每一个删除位置注入失败，覆盖尚未修改、已部分删除及最后删除根节点的回滚。
    for (int stage = 1; stage <= 7; ++stage) {
        reset();
        const auto before = database.stored;
        database.override_delete_at = stage;
        database.delete_reply = mode == "delete_zero" ? DbExecuteResult{true, 0, 0, 0} :
                                mode == "delete_many" ? DbExecuteResult{true, 0, 2, 0} : DbExecuteResult{false, 1213, 0, 0};
        check(execute() == DeleteDirectoryTreeResult::failed && database.deletes == stage,
              "failed DELETE did not abort tree deletion immediately");
        finished(0, 1);
        check_records(before, false);
    }
}

void test_commit(DbCommitStatus status, bool applied) {
    reset();
    const auto before = database.stored;
    database.commit_reply = status;
    database.unknown_applies = applied;
    const auto expected = status == DbCommitStatus::k_committed ? DeleteDirectoryTreeResult::committed :
                          status == DbCommitStatus::k_not_active ? DeleteDirectoryTreeResult::failed : DeleteDirectoryTreeResult::outcome_unknown;
    check(execute() == expected && database.begins == 1 && database.deletes == 7, "tree commit outcome was lost or retried");
    check_records(before, applied);
    check_child_first(before);
    finished(1, 0);
}

void test_exceptions() {
    for (int boundary = 0; boundary < 3; ++boundary) {
        for (int stage = 1; stage <= 7; ++stage) {
            for (bool nonstandard : {false, true}) {
                reset();
                const auto before = database.stored;
                database.nonstandard = nonstandard;
                if (boundary == 0) database.throw_query_at = stage;
                else database.throw_delete_at = stage;
                database.throw_after_delete = boundary == 2;
                bool propagated = false;
                try { execute(); }
                catch (const std::runtime_error& error) {
                    propagated = !nonstandard && std::string(error.what()) ==
                        (boundary == 0 ? "injected tree query exception" : "injected tree delete exception");
                } catch (int value) { propagated = nonstandard && value == 7; }
                check(propagated, "tree deletion swallowed or replaced a database exception");
                finished(0, 1);
                check_records(before, false);
            }
        }
    }
}

void test_repeat() {
    reset();
    check(execute() == DeleteDirectoryTreeResult::committed, "initial tree deletion failed");
    const auto before = database.stored;
    database.begins = database.queries = database.deletes = database.commits = database.rollbacks = 0;
    check(execute() == DeleteDirectoryTreeResult::failed && database.queries == 3 && database.deletes == 0,
          "already removed tree was committed or deleted again");
    finished(0, 1);
    check_records(before, false);
}

void run(const std::string& mode) {
    if (mode == "invalid_arguments") test_invalid();
    else if (mode == "path_limits") test_paths();
    else if (mode == "begin_failure") test_transaction(false);
    else if (mode == "existing_transaction") test_transaction(true);
    else if (mode == "user_results") test_lookup(1);
    else if (mode == "parent_results") test_lookup(2);
    else if (mode == "target_results") test_lookup(3);
    else if (mode == "invalid_targets") test_targets(false);
    else if (mode == "exact_path") test_targets(true);
    else if (mode == "children_results") test_children(false);
    else if (mode == "child_values") test_children(true);
    else if (mode == "duplicate_ids") test_duplicate_ids();
    else if (mode == "query_failure") test_query_failure();
    else if (mode == "delete_failure" || mode == "delete_zero" || mode == "delete_many") test_delete_failure(mode);
    else if (mode == "committed") test_commit(DbCommitStatus::k_committed, true);
    else if (mode == "unknown_applied") test_commit(DbCommitStatus::k_outcome_unknown, true);
    else if (mode == "unknown_not_applied") test_commit(DbCommitStatus::k_outcome_unknown, false);
    else if (mode == "commit_not_active") test_commit(DbCommitStatus::k_not_active, false);
    else if (mode == "exceptions") test_exceptions();
    else if (mode == "repeat") test_repeat();
    else if (mode == "empty_tree" || mode == "nested_tree" || mode == "wide_tree" || mode == "deep_tree" ||
             mode == "literal_names" || mode == "root_child") test_success(mode);
    else throw std::invalid_argument("unknown tree scenario: " + mode);
}

} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 2, "expected one tree scenario");
        run(argv[1]);
        std::cout << "PASS directory tree deletion " << argv[1] << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
