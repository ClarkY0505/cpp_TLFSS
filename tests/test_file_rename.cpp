#include "ftp/file_rename.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <map>
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

struct Node {
  std::string id, user, parent, name, path, type, hash;
  bool operator==(const Node& other) const {
    return std::tie(id, user, parent, name, path, type, hash) ==
           std::tie(other.id, other.user, other.parent, other.name, other.path, other.type,
                    other.hash);
  }
};

// 执行真实重命名函数；仅替代连接和事务边界，不连接 MySQL、不操作磁盘对象。
// stored 为已提交数据，pending 为事务副本，回滚不得改变 stored。
struct Database {
  std::string user = "alice", directory = "/docs", old_name = "report.txt",
              new_name = "summary.txt";
  std::vector<Node> stored{
      {"1", "alice", "-1", "/", "/", "directory", ""},
      {"12", "alice", "1", "docs", "/docs", "directory", ""},
      {"13", "alice", "1", "other", "/other", "directory", ""},
      {"128", "alice", "12", "report.txt", "/docs/report.txt", "file", std::string(64, 'a')},
      {"129", "alice", "13", "report.txt", "/other/report.txt", "file", std::string(64, 'b')},
      {"22", "bob", "-1", "/", "/", "directory", ""},
      {"228", "bob", "22", "report.txt", "/report.txt", "file", std::string(64, 'a')}};
  std::vector<Node> pending = stored;
  DbConnection* owner = nullptr;
  bool begin_ok = true, unknown_applies = false;
  DbCommitStatus commit_reply = DbCommitStatus::k_committed;
  std::map<int, DbQueryResult> replies;
  bool override_update = false;
  DbExecuteResult update_reply{true, 0, 1, 0};
  int throw_query_at = 0;
  bool throw_update = false, throw_after_update = false, nonstandard = false;
  int begins = 0, queries = 0, updates = 0, commits = 0, rollbacks = 0;
  std::string parent_id, file_id;
} database;

void reset() {
  database = Database{};
}
std::string full_path(const std::string& name) {
  return database.directory == "/" ? "/" + name : database.directory + "/" + name;
}

// 仅模拟 ASCII 大小写等价，用于目标名称冲突；不声称覆盖真实 MySQL 的 Unicode 排序规则。
bool name_equal(const std::string& first, const std::string& second) {
  if (first.size() != second.size())
    return false;
  for (std::size_t index = 0; index < first.size(); ++index)
    if (std::tolower(static_cast<unsigned char>(first[index])) !=
        std::tolower(static_cast<unsigned char>(second[index])))
      return false;
  return true;
}

void require_sql(const std::string& sql, const std::vector<std::string>& fragments) {
  for (const auto& fragment : fragments)
    check(sql.find(fragment) != std::string::npos,
          "rename SQL omitted an identity or locking guard");
  check(sql.find("storage_objects") == std::string::npos &&
            sql.find("object_hash") == std::string::npos,
        "rename accessed the object catalog or changed the content hash");
}

void remember_id(const DbQueryResult& result, std::string& id) {
  if (result.success && result.rows.size() == 1 && result.rows[0].size() == 1 && result.rows[0][0])
    id = *result.rows[0][0];
}

[[noreturn]] void injected(const char* message) {
  if (database.nonstandard)
    throw 42;
  throw std::runtime_error(message);
}

RenameFileResult execute() {
  DbConnection connection;
  return rename_file_transaction(connection, database.user, database.directory, database.old_name,
                                 database.new_name);
}

void finished(int commits, int rollbacks) {
  check(database.commits == commits && database.rollbacks == rollbacks,
        "rename committed, retried or rolled back an unexpected number of times");
  check(database.owner == nullptr && database.pending == database.stored,
        "rename retained a transaction or uncommitted changes");
}

void records(const std::vector<Node>& before, bool applied) {
  auto expected = before;
  if (applied) {
    for (auto& node : expected) {
      if (node.id == "128") {
        node.name = database.new_name;
        node.path = full_path(database.new_name);
      }
    }
  }
  // 比较完整记录，既检查更新字段，也检查 ID、父目录、哈希及其他用户记录保持不变。
  check(database.stored == expected,
        "rename changed the wrong metadata or lost an object reference");
}

void relocate_source() {
  for (auto& node : database.stored) {
    if (node.id == "12" && database.directory != "/")
      node.path = database.directory;
    if (node.id == "128") {
      node.name = database.old_name;
      node.path = full_path(database.old_name);
      node.parent = database.directory == "/" ? "1" : "12";
    }
  }
  database.pending = database.stored;
}

}  // namespace

namespace TLSS::BASE {

DbConnection::DbConnection()
    : _conn(nullptr)
    , _connected(false)
    , _transaction_active(false) {}
DbConnection::~DbConnection() = default;

DbQueryResult DbConnection::query_prepared(const std::string& sql,
                                           const std::vector<DbValue>& params) {
  check(_transaction_active && database.owner == this, "rename query ran outside its transaction");
  const int stage = ++database.queries;
  check(database.updates == 0, "rename updated metadata before completing all lookups");
  require_sql(sql, {"FOR UPDATE"});
  if (stage == database.throw_query_at)
    injected("injected rename query exception");
  DbQueryResult result{true, {}};
  if (stage == 1) {
    require_sql(sql, {"SELECT id FROM users", "username = ?", "is_deleted = 0"});
    check(params == std::vector<DbValue>{database.user}, "rename did not lock its user first");
    result.rows = {{"7"}};
  } else if (stage == 2) {
    require_sql(sql, {"SELECT id FROM virtual_files", "username = ?", "path = ?",
                      "file_type = 'directory'"});
    check(params == std::vector<DbValue>{database.user, database.directory},
          "rename changed its parent scope");
    for (const auto& node : database.pending)
      if (node.user == database.user && node.path == database.directory && node.type == "directory")
        result.rows.push_back({node.id});
  } else if (stage == 3) {
    require_sql(sql, {"username = ?", "parent_id = ?", "file_name = ?",
                      "CAST(path AS BINARY) = CAST(? AS BINARY)", "file_type = 'file'"});
    check(params == std::vector<DbValue>{database.user, database.parent_id, database.old_name,
                                         full_path(database.old_name)},
          "rename changed its source identity");
    for (const auto& node : database.pending)
      if (node.user == database.user && node.parent == database.parent_id &&
          name_equal(node.name, database.old_name) && node.path == full_path(database.old_name) &&
          node.type == "file")
        result.rows.push_back({node.id});
  } else {
    check(stage == 4, "rename unexpectedly repeated a lookup");
    require_sql(sql, {"username = ?", "parent_id = ?", "file_name = ?", "id <> ?", "LIMIT 1"});
    check(sql.find("file_type") == std::string::npos,
          "rename excluded directories from target conflicts");
    check(params == std::vector<DbValue>{database.user, database.parent_id, database.new_name,
                                         database.file_id},
          "rename failed to exclude its own source record from conflicts");
    for (const auto& node : database.pending) {
      if (node.user == database.user && node.parent == database.parent_id &&
          name_equal(node.name, database.new_name) && node.id != database.file_id) {
        result.rows.push_back({node.id});
        break;
      }
    }
  }
  const auto override = database.replies.find(stage);
  if (override != database.replies.end())
    result = override->second;
  if (stage == 2)
    remember_id(result, database.parent_id);
  if (stage == 3)
    remember_id(result, database.file_id);
  return result;
}

DbExecuteResult DbConnection::execute_prepared(const std::string& sql,
                                               const std::vector<DbValue>& params) {
  check(_transaction_active && database.owner == this && database.queries == 4,
        "rename UPDATE ran outside its transaction or before conflict checks");
  ++database.updates;
  require_sql(sql, {"UPDATE virtual_files SET file_name = ?, path = ?", "id = ?", "username = ?",
                    "parent_id = ?", "file_name = ?", "CAST(path AS BINARY) = CAST(? AS BINARY)",
                    "file_type = 'file'"});
  const auto set_clause = sql.substr(0, sql.find("WHERE"));
  check(set_clause.find("parent_id") == std::string::npos &&
            set_clause.find("username") == std::string::npos,
        "same-directory rename changed the file's parent or owner");
  check(params == std::vector<DbValue>{database.new_name, full_path(database.new_name),
                                       database.file_id, database.user, database.parent_id,
                                       database.old_name, full_path(database.old_name)},
        "rename UPDATE lost its new values or old identity guards");
  if (database.throw_update && !database.throw_after_update)
    injected("injected rename update exception");
  std::uint64_t count = 0;
  for (auto& node : database.pending) {
    if (node.id == database.file_id && node.user == database.user &&
        node.parent == database.parent_id && node.name == database.old_name &&
        node.path == full_path(database.old_name) && node.type == "file") {
      ++count;
      node.name = database.new_name;
      node.path = full_path(database.new_name);
    }
  }
  // 也模拟工作副本已经改变后才返回失败/抛异常，验证整个事务仍被撤销。
  if (database.throw_update)
    injected("injected rename update exception");
  return database.override_update ? database.update_reply : DbExecuteResult{true, 0, count, 0};
}

DbTransaction::DbTransaction(DbConnection& connection) noexcept
    : connection_(connection)
    , active_(database.begin_ok && !connection._transaction_active) {
  ++database.begins;
  if (active_) {
    connection_._transaction_active = true;
    database.owner = &connection_;
    database.pending = database.stored;
  }
}
DbTransaction::~DbTransaction() noexcept {
  rollback();
}
DbCommitStatus DbTransaction::commit() noexcept {
  if (!active_)
    return DbCommitStatus::k_not_active;
  ++database.commits;
  if (database.commit_reply == DbCommitStatus::k_committed ||
      (database.commit_reply == DbCommitStatus::k_outcome_unknown && database.unknown_applies))
    database.stored = database.pending;
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

}  // namespace TLSS::BASE

namespace {

void test_invalid() {
  for (const auto& name :
       std::vector<std::string>{"", ".", "..", "/", "a/b", "a\\b", std::string(256, 'x'),
                                std::string("a\0b", 3), "a\nb", std::string(1, '\x7f')}) {
    for (bool old : {false, true}) {
      reset();
      (old ? database.old_name : database.new_name) = name;
      check(execute() == RenameFileResult::failed && database.begins == 0 && database.queries == 0,
            "invalid rename name accessed the database");
    }
  }
  for (bool user : {false, true}) {
    reset();
    (user ? database.user : database.directory).clear();
    check(execute() == RenameFileResult::failed && database.begins == 0,
          "empty rename scope was accepted");
  }
}

void test_paths() {
  for (bool old_long : {false, true}) {
    for (std::size_t length : {std::size_t{1024}, std::size_t{1025}}) {
      reset();
      database.old_name = old_long ? std::string(10, 'o') : "a";
      database.new_name = old_long ? "a" : std::string(10, 'n');
      const auto size = length - 11;
      database.directory.clear();
      while (database.directory.size() + 256 < size)
        database.directory += "/" + std::string(255, 'd');
      database.directory += "/" + std::string(size - database.directory.size() - 1, 'd');
      relocate_source();
      const auto before = database.stored;
      const auto result = execute();
      check(result == (length == 1024 ? RenameFileResult::committed : RenameFileResult::failed),
            "rename did not check both complete path lengths");
      check(database.begins == (length == 1024 ? 1 : 0),
            "oversized rename path began a transaction");
      records(before, length == 1024);
      finished(length == 1024 ? 1 : 0, 0);
    }
  }
}

void test_results(int stage) {
  std::vector<DbQueryResult> invalid{{false, {}},
                                     {true, {}},
                                     {true, {{}}},
                                     {true, {{std::nullopt}}},
                                     {true, {{"7", "extra"}}},
                                     {true, {{"7"}, {"8"}}}};
  // 目标查询成功且为空表示可用；其他结果都应当失败，包括格式异常的非空结果。
  if (stage == 4)
    invalid.erase(invalid.begin() + 1);
  for (const auto& reply : invalid) {
    reset();
    const auto before = database.stored;
    database.replies[stage] = reply;
    check(
        execute() == RenameFileResult::failed && database.queries == stage && database.updates == 0,
        "rename continued after a failed, absent or malformed lookup");
    records(before, false);
    finished(0, 1);
  }
}

void test_update_results() {
  for (const auto& reply : std::vector<DbExecuteResult>{
           {false, 1062, 0, 0}, {false, 1213, 0, 0}, {true, 0, 0, 0}, {true, 0, 2, 0}}) {
    reset();
    const auto before = database.stored;
    database.override_update = true;
    database.update_reply = reply;
    check(execute() == RenameFileResult::failed && database.updates == 1,
          "rename accepted a failed UPDATE or an unexpected affected row count");
    records(before, false);
    finished(0, 1);
  }
}

void test_exceptions() {
  for (bool nonstandard : {false, true}) {
    for (int position = 1; position <= 6; ++position) {
      reset();
      const auto before = database.stored;
      database.nonstandard = nonstandard;
      if (position <= 4)
        database.throw_query_at = position;
      else {
        database.throw_update = true;
        database.throw_after_update = position == 6;
      }
      bool caught = false;
      try {
        execute();
      } catch (const std::runtime_error& error) {
        const std::string expected =
            position <= 4 ? "injected rename query exception" : "injected rename update exception";
        caught = !nonstandard && error.what() == expected;
      } catch (int code) {
        caught = nonstandard && code == 42;
      }
      check(caught, "rename swallowed or replaced an exception");
      records(before, false);
      finished(0, 1);
    }
  }
}

void run(const std::string& mode) {
  reset();
  if (mode == "invalid_arguments") {
    test_invalid();
    return;
  }
  if (mode == "path_limits") {
    test_paths();
    return;
  }
  if (mode == "user_results") {
    test_results(1);
    return;
  }
  if (mode == "parent_results") {
    test_results(2);
    return;
  }
  if (mode == "source_results") {
    test_results(3);
    return;
  }
  if (mode == "target_results") {
    test_results(4);
    return;
  }
  if (mode == "update_results") {
    test_update_results();
    return;
  }
  if (mode == "exceptions") {
    test_exceptions();
    return;
  }
  if (mode == "existing_transaction") {
    DbConnection connection;
    DbTransaction outer(connection);
    check(rename_file_transaction(connection, database.user, database.directory, database.old_name,
                                  database.new_name) == RenameFileResult::failed &&
              database.owner == &connection && database.queries == 0 && database.rollbacks == 0,
          "rename changed or rolled back the caller's active transaction");
    outer.rollback();
    finished(0, 1);
    return;
  }
  bool applied = true;
  int commits = 1, rollbacks = 0;
  auto expected = RenameFileResult::committed;
  if (mode == "begin_failure") {
    database.begin_ok = false;
    expected = RenameFileResult::failed;
    applied = false;
    commits = 0;
  } else if (mode == "missing_source" || mode == "unchanged_missing") {
    database.stored.erase(database.stored.begin() + 3);
    if (mode == "unchanged_missing")
      database.new_name = database.old_name;
    expected = RenameFileResult::failed;
    applied = false;
    commits = 0;
    rollbacks = 1;
  } else if (mode == "source_directory" || mode == "parent_not_directory" || mode == "exact_path") {
    for (auto& node : database.stored) {
      if (mode == "source_directory" && node.id == "128")
        node.type = "directory";
      if (mode == "parent_not_directory" && node.id == "12")
        node.type = "file";
      if (mode == "exact_path" && node.id == "128")
        node.path = "/Docs/report.txt";
    }
    expected = RenameFileResult::failed;
    applied = false;
    commits = 0;
    rollbacks = 1;
  } else if (mode == "duplicate_file" || mode == "duplicate_directory" ||
             mode == "case_collision") {
    const auto name = mode == "case_collision" ? "SUMMARY.TXT" : database.new_name;
    database.stored.push_back({"999", "alice", "12", name, "/docs/" + name,
                               mode == "duplicate_directory" ? "directory" : "file",
                               std::string(64, 'f')});
    expected = RenameFileResult::failed;
    applied = false;
    commits = 0;
    rollbacks = 1;
  } else if (mode == "namespace_scope") {
    database.stored.push_back(
        {"998", "alice", "13", database.new_name, "/other/summary.txt", "file", "other"});
    database.stored.push_back(
        {"999", "bob", "22", database.new_name, "/summary.txt", "file", "bob"});
  } else if (mode == "root_parent") {
    database.directory = "/";
    relocate_source();
  } else if (mode == "literal_name") {
    database.old_name = "old '%_\" 文件.txt";
    database.new_name = "new '%_\" 文件.txt";
    relocate_source();
  } else if (mode == "unchanged") {
    database.new_name = database.old_name;
  } else if (mode == "case_only") {
    database.old_name = "Report.txt";
    database.new_name = "report.txt";
    relocate_source();
  } else if (mode == "unknown_applied" || mode == "unknown_not_applied") {
    database.commit_reply = DbCommitStatus::k_outcome_unknown;
    database.unknown_applies = mode == "unknown_applied";
    applied = database.unknown_applies;
    expected = RenameFileResult::outcome_unknown;
  } else if (mode == "commit_not_active" || mode == "commit_fallback") {
    database.commit_reply = mode == "commit_not_active" ? DbCommitStatus::k_not_active
                                                        : static_cast<DbCommitStatus>(999);
    applied = false;
    expected =
        mode == "commit_not_active" ? RenameFileResult::failed : RenameFileResult::outcome_unknown;
  } else if (mode != "committed" && mode != "repeat") {
    throw std::invalid_argument("unknown rename scenario: " + mode);
  }
  const auto before = database.stored;
  check(execute() == expected, "rename returned the wrong transaction outcome");
  records(before, applied);
  finished(commits, rollbacks);
  if (mode == "unchanged")
    check(database.updates == 0, "unchanged name issued a pointless UPDATE");
  if (mode == "repeat") {
    const auto renamed = database.stored;
    database.queries = database.updates = 0;
    check(execute() == RenameFileResult::failed && database.updates == 0 &&
              database.stored == renamed,
          "repeated old-name request renamed another record or retried the update");
    finished(1, 1);
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    check(argc == 2, "expected a rename scenario");
    run(argv[1]);
    std::cout << "PASS file rename " << argv[1] << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
