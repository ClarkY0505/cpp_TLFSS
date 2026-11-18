#include "ftp/file_move.h"

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

// 运行实际移动事务，仅替代连接边界。提交前更改 pending，提交成功才应用到 stored。
// 此替代层不模拟真实 InnoDB 锁等待，也不访问对象表或磁盘对象。
struct Database {
  std::string user = "alice", source = "/docs", target = "/archive", name = "report.txt";
  std::string source_id = "12", target_id = "20";
  std::vector<Node> stored{
      {"1", "alice", "-1", "/", "/", "directory", ""},
      {"12", "alice", "1", "docs", "/docs", "directory", ""},
      {"20", "alice", "1", "archive", "/archive", "directory", ""},
      {"13", "alice", "1", "other", "/other", "directory", ""},
      {"128", "alice", "12", "report.txt", "/docs/report.txt", "file", std::string(64, 'a')},
      {"129", "alice", "13", "report.txt", "/other/report.txt", "file", std::string(64, 'b')},
      {"2", "bob", "-1", "/", "/", "directory", ""},
      {"22", "bob", "2", "archive", "/archive", "directory", ""},
      {"228", "bob", "22", "report.txt", "/archive/report.txt", "file", std::string(64, 'a')}};
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
  std::string selected_source, selected_target, selected_file;
} database;

void reset() {
  database = Database{};
}
std::string path_in(const std::string& directory) {
  return directory == "/" ? "/" + database.name : directory + "/" + database.name;
}

// 只模拟 ASCII 大小写等价，验证目标冲突遵循名称比较；Unicode 排序规则需真实 MySQL 验证。
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
    check(sql.find(fragment) != std::string::npos, "move SQL omitted a locking or identity guard");
  check(sql.find("storage_objects") == std::string::npos &&
            sql.find("object_hash") == std::string::npos,
        "move accessed the object catalog or changed a content hash");
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

MoveFileResult execute() {
  DbConnection connection;
  return move_file_transaction(connection, database.user, database.source, database.target,
                               database.name);
}

void finished(int commits, int rollbacks) {
  check(database.commits == commits && database.rollbacks == rollbacks,
        "move committed, retried or rolled back an unexpected number of times");
  check(database.owner == nullptr && database.pending == database.stored,
        "move retained a transaction or uncommitted changes");
}

void records(const std::vector<Node>& before, bool applied) {
  auto expected = before;
  if (applied) {
    for (auto& node : expected) {
      if (node.id == "128") {
        node.parent = database.target_id;
        node.path = path_in(database.target);
      }
    }
  }
  // 完整比较所有字段，保证名称、用户、文件 ID、哈希以及其他记录全部保留。
  check(database.stored == expected,
        "move changed a file identity, object reference or unrelated record");
}

void configure_paths() {
  for (auto& node : database.stored) {
    if (node.id == "12" && database.source_id == "12")
      node.path = database.source;
    if (node.id == "20" && database.target_id == "20")
      node.path = database.target;
    if (node.id == "128") {
      node.name = database.name;
      node.parent = database.source_id;
      node.path = path_in(database.source);
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
  check(_transaction_active && database.owner == this, "move query ran outside its transaction");
  const int stage = ++database.queries;
  check(database.updates == 0, "move updated before completing its lookups");
  require_sql(sql, {"FOR UPDATE"});
  if (stage == database.throw_query_at)
    injected("injected move query exception");
  DbQueryResult result{true, {}};
  if (stage == 1) {
    require_sql(sql, {"SELECT id FROM users", "username = ?", "is_deleted = 0"});
    check(params == std::vector<DbValue>{database.user},
          "move did not lock the current user first");
    result.rows = {{"7"}};
  } else if (stage == 2 || stage == 3) {
    const auto& directory = stage == 2 ? database.source : database.target;
    require_sql(sql, {"SELECT id FROM virtual_files", "username = ?",
                      "CAST(path AS BINARY) = CAST(? AS BINARY)", "file_type = 'directory'"});
    check(params == std::vector<DbValue>{database.user, directory},
          "move changed a directory lookup scope");
    for (const auto& node : database.pending)
      if (node.user == database.user && node.path == directory && node.type == "directory")
        result.rows.push_back({node.id});
  } else if (stage == 4) {
    require_sql(sql, {"username = ?", "parent_id = ?", "file_name = ?",
                      "CAST(path AS BINARY) = CAST(? AS BINARY)", "file_type = 'file'"});
    check(params == std::vector<DbValue>{database.user, database.selected_source, database.name,
                                         path_in(database.source)},
          "move changed its source identity");
    for (const auto& node : database.pending)
      if (node.user == database.user && node.parent == database.selected_source &&
          name_equal(node.name, database.name) && node.path == path_in(database.source) &&
          node.type == "file")
        result.rows.push_back({node.id});
  } else {
    check(stage == 5, "move unexpectedly repeated a lookup");
    require_sql(sql, {"username = ?", "parent_id = ?", "file_name = ?", "id <> ?", "LIMIT 1"});
    check(sql.find("file_type") == std::string::npos,
          "move excluded directories from name conflicts");
    check(params == std::vector<DbValue>{database.user, database.selected_target, database.name,
                                         database.selected_file},
          "move checked the source directory or failed to exclude its own record");
    for (const auto& node : database.pending) {
      if (node.user == database.user && node.parent == database.selected_target &&
          name_equal(node.name, database.name) && node.id != database.selected_file) {
        result.rows.push_back({node.id});
        break;
      }
    }
  }
  const auto override = database.replies.find(stage);
  if (override != database.replies.end())
    result = override->second;
  if (stage == 2)
    remember_id(result, database.selected_source);
  if (stage == 3)
    remember_id(result, database.selected_target);
  if (stage == 4)
    remember_id(result, database.selected_file);
  return result;
}

DbExecuteResult DbConnection::execute_prepared(const std::string& sql,
                                               const std::vector<DbValue>& params) {
  check(_transaction_active && database.owner == this && database.queries == 5,
        "move UPDATE ran outside its transaction or before conflict checks");
  ++database.updates;
  require_sql(sql, {"UPDATE virtual_files SET parent_id = ?, path = ?", "id = ?", "username = ?",
                    "parent_id = ?", "file_name = ?", "CAST(path AS BINARY) = CAST(? AS BINARY)",
                    "file_type = 'file'"});
  const auto set_clause = sql.substr(0, sql.find("WHERE"));
  check(set_clause.find("file_name") == std::string::npos &&
            set_clause.find("username") == std::string::npos,
        "move renamed a file or changed its owner");
  check(params == std::vector<DbValue>{database.selected_target, path_in(database.target),
                                       database.selected_file, database.user,
                                       database.selected_source, database.name,
                                       path_in(database.source)},
        "move UPDATE lost its new parent, new path or source identity guards");
  if (database.throw_update && !database.throw_after_update)
    injected("injected move update exception");
  std::uint64_t count = 0;
  for (auto& node : database.pending) {
    if (node.id == database.selected_file && node.user == database.user &&
        node.parent == database.selected_source && node.name == database.name &&
        node.path == path_in(database.source) && node.type == "file") {
      ++count;
      node.parent = database.selected_target;
      node.path = path_in(database.target);
    }
  }
  // 覆盖工作副本已经变化之后才抛异常或返回失败，保证整个事务仍能回滚。
  if (database.throw_update)
    injected("injected move update exception");
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
    reset();
    database.name = name;
    check(execute() == MoveFileResult::failed && database.begins == 0 && database.queries == 0,
          "invalid move filename accessed the database");
  }
  for (int field = 0; field < 3; ++field) {
    reset();
    if (field == 0)
      database.user.clear();
    else if (field == 1)
      database.source.clear();
    else
      database.target.clear();
    check(execute() == MoveFileResult::failed && database.begins == 0,
          "empty move scope was accepted");
  }
}

void test_paths() {
  for (bool source_long : {false, true}) {
    for (std::size_t length : {std::size_t{1024}, std::size_t{1025}}) {
      reset();
      const auto size = length - database.name.size() - 1;
      std::string directory;
      while (directory.size() + 256 < size)
        directory += "/" + std::string(255, 'd');
      directory += "/" + std::string(size - directory.size() - 1, 'd');
      (source_long ? database.source : database.target) = directory;
      configure_paths();
      const auto before = database.stored;
      check(execute() == (length == 1024 ? MoveFileResult::committed : MoveFileResult::failed),
            "move did not check both complete path lengths");
      check(database.begins == (length == 1024 ? 1 : 0), "oversized move path began a transaction");
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
                                     {true, {{"12", "extra"}}},
                                     {true, {{"12"}, {"20"}}}};
  if (stage == 5)
    invalid.erase(invalid.begin() + 1);
  for (const auto& reply : invalid) {
    reset();
    database.replies[stage] = reply;
    const auto before = database.stored;
    check(execute() == MoveFileResult::failed && database.queries == stage && database.updates == 0,
          "move continued after a failed, absent or malformed lookup");
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
    check(execute() == MoveFileResult::failed && database.updates == 1,
          "move accepted a failed UPDATE or an unexpected affected row count");
    records(before, false);
    finished(0, 1);
  }
}

void test_exceptions() {
  for (bool nonstandard : {false, true}) {
    for (int position = 1; position <= 7; ++position) {
      reset();
      const auto before = database.stored;
      database.nonstandard = nonstandard;
      if (position <= 5)
        database.throw_query_at = position;
      else {
        database.throw_update = true;
        database.throw_after_update = position == 7;
      }
      bool caught = false;
      try {
        execute();
      } catch (const std::runtime_error& error) {
        const std::string expected =
            position <= 5 ? "injected move query exception" : "injected move update exception";
        caught = !nonstandard && error.what() == expected;
      } catch (int code) {
        caught = nonstandard && code == 42;
      }
      check(caught, "move swallowed or replaced an exception");
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
  if (mode == "source_directory_results") {
    test_results(2);
    return;
  }
  if (mode == "target_directory_results") {
    test_results(3);
    return;
  }
  if (mode == "source_results") {
    test_results(4);
    return;
  }
  if (mode == "conflict_results") {
    test_results(5);
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
    check(move_file_transaction(connection, database.user, database.source, database.target,
                                database.name) == MoveFileResult::failed &&
              database.owner == &connection && database.queries == 0 && database.rollbacks == 0,
          "move altered or rolled back the caller's active transaction");
    outer.rollback();
    finished(0, 1);
    return;
  }
  auto expected = MoveFileResult::committed;
  bool applied = true;
  int commits = 1, rollbacks = 0;
  if (mode == "begin_failure") {
    database.begin_ok = false;
    expected = MoveFileResult::failed;
    applied = false;
    commits = 0;
  } else if (mode == "missing_source_directory" || mode == "missing_target_directory" ||
             mode == "missing_source" || mode == "same_directory_missing") {
    if (mode == "same_directory_missing") {
      database.target = database.source;
      database.target_id = database.source_id;
    }
    const std::string id = mode == "missing_source_directory"   ? "12"
                           : mode == "missing_target_directory" ? "20"
                                                                : "128";
    database.stored.erase(std::remove_if(database.stored.begin(), database.stored.end(),
                                         [&](const Node& node) {
                                           return node.id == id;
                                         }),
                          database.stored.end());
    expected = MoveFileResult::failed;
    applied = false;
    commits = 0;
    rollbacks = 1;
  } else if (mode == "source_not_directory" || mode == "target_not_directory" ||
             mode == "source_is_directory" || mode == "file_exact_path") {
    for (auto& node : database.stored) {
      if (mode == "source_not_directory" && node.id == "12")
        node.type = "file";
      if (mode == "target_not_directory" && node.id == "20")
        node.type = "file";
      if (mode == "source_is_directory" && node.id == "128")
        node.type = "directory";
      if (mode == "file_exact_path" && node.id == "128")
        node.path = "/Docs/report.txt";
    }
    expected = MoveFileResult::failed;
    applied = false;
    commits = 0;
    rollbacks = 1;
  } else if (mode == "source_exact_path" || mode == "target_exact_path" ||
             mode == "foreign_target") {
    if (mode == "source_exact_path")
      database.source = "/Docs";
    else if (mode == "target_exact_path")
      database.target = "/Archive";
    else {
      database.target = "/foreign";
      database.stored.push_back({"99", "bob", "2", "foreign", "/foreign", "directory", ""});
    }
    expected = MoveFileResult::failed;
    applied = false;
    commits = 0;
    rollbacks = 1;
  } else if (mode == "duplicate_file" || mode == "duplicate_directory" ||
             mode == "case_collision") {
    const auto name = mode == "case_collision" ? "REPORT.TXT" : database.name;
    database.stored.push_back({"999", "alice", "20", name, "/archive/" + name,
                               mode == "duplicate_directory" ? "directory" : "file",
                               mode == "duplicate_directory" ? "" : std::string(64, 'f')});
    expected = MoveFileResult::failed;
    applied = false;
    commits = 0;
    rollbacks = 1;
  } else if (mode == "root_source") {
    database.source = "/";
    database.source_id = "1";
    configure_paths();
  } else if (mode == "root_target") {
    database.target = "/";
    database.target_id = "1";
    configure_paths();
  } else if (mode == "literal_name") {
    database.name = "file '%_\" 文件.txt";
    configure_paths();
  } else if (mode == "same_directory" || mode == "same_directory_unknown") {
    database.target = database.source;
    database.target_id = database.source_id;
    if (mode == "same_directory_unknown") {
      database.commit_reply = DbCommitStatus::k_outcome_unknown;
      expected = MoveFileResult::outcome_unknown;
      applied = false;
    }
  } else if (mode == "unknown_applied" || mode == "unknown_not_applied") {
    database.commit_reply = DbCommitStatus::k_outcome_unknown;
    database.unknown_applies = mode == "unknown_applied";
    applied = database.unknown_applies;
    expected = MoveFileResult::outcome_unknown;
  } else if (mode == "commit_not_active" || mode == "commit_fallback") {
    database.commit_reply = mode == "commit_not_active" ? DbCommitStatus::k_not_active
                                                        : static_cast<DbCommitStatus>(999);
    expected =
        mode == "commit_not_active" ? MoveFileResult::failed : MoveFileResult::outcome_unknown;
    applied = false;
  } else if (mode != "committed" && mode != "repeat") {
    throw std::invalid_argument("unknown move scenario: " + mode);
  }
  const auto before = database.stored;
  check(execute() == expected, "move returned the wrong transaction outcome");
  records(before, applied);
  finished(commits, rollbacks);
  if (mode == "same_directory" || mode == "same_directory_unknown")
    check(database.updates == 0, "same-directory move issued a pointless UPDATE");
  if (mode == "repeat") {
    const auto moved = database.stored;
    database.queries = database.updates = 0;
    check(execute() == MoveFileResult::failed && database.updates == 0 && database.stored == moved,
          "repeated move retried an update or moved another record");
    finished(1, 1);
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    check(argc == 2, "expected a move scenario");
    run(argv[1]);
    std::cout << "PASS file move " << argv[1] << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
