#include "ftp/file_deletion.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

using namespace TLSS::BASE;
using namespace TLSS::FileStorage;

const std::string hash(64, 'a');

void check(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}

struct File {
  std::string id, username, parent_id, name, object_hash;

  bool operator==(const File& other) const {
    return id == other.id && username == other.username && parent_id == other.parent_id &&
           name == other.name && object_hash == other.object_hash;
  }
};

// 区分事务工作副本与已提交记录。数据库边界由测试替代，不连接真实 MySQL。
// 本测试验证调用顺序、参数范围和状态处理，不验证 InnoDB 的真实锁等待或持久化。
struct Database {
  std::string directory = "/docs";
  std::string filename = "report.txt";
  std::vector<File> stored{
      {"128", "alice", "12", "report.txt", hash},
      {"129", "bob", "22", "report.txt", hash},      // 其他用户同名、同哈希文件。
      {"130", "alice", "13", "report.txt", hash},    // 当前用户其他目录的同名文件。
      {"131", "alice", "12", "another.txt", hash}};  // 同目录其他引用也必须保留。
  std::vector<File> pending = stored;
  DbConnection* owner = nullptr;
  bool begin_ok = true;
  std::optional<DbQueryResult> user_reply, parent_reply, file_reply;
  DbExecuteResult delete_reply{true, 0, 1, 0};
  DbCommitStatus commit_reply = DbCommitStatus::k_committed;
  bool unknown_applies = false;
  int throw_at = 0;  // 1 用户查询、2 目录查询、3 文件查询、4 删除。
  bool throw_nonstandard = false;
  int begins = 0, queries = 0, deletes = 0, commits = 0, rollbacks = 0;
} database;

void reset() {
  database = Database{};
}

void maybe_throw(int stage) {
  if (database.throw_at != stage)
    return;
  if (database.throw_nonstandard)
    throw 7;
  throw std::runtime_error("injected database exception");
}

void require_sql(const std::string& sql, const std::vector<std::string>& fragments) {
  for (const auto& fragment : fragments)
    check(sql.find(fragment) != std::string::npos, "missing SQL constraint: " + fragment);
  check(sql.find("storage_objects") == std::string::npos,
        "file deletion attempted to modify the object catalog");
  check(sql.find("alice") == std::string::npos && sql.find(database.filename) == std::string::npos,
        "SQL contains interpolated user input");
}

DeleteFileResult execute(const std::string& username = "alice") {
  DbConnection connection;
  return delete_file_transaction(connection, username, database.directory, database.filename);
}

void check_finished(int commits, int rollbacks) {
  check(database.commits == commits && database.rollbacks == rollbacks,
        "unexpected commit/rollback count");
  check(database.owner == nullptr, "transaction did not release its connection");
  check(database.pending == database.stored, "pending changes remained after transaction exit");
}

void check_preserved(const std::vector<File>& before) {
  check(database.stored == before, "failed deletion changed committed file records");
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
  check(_transaction_active && database.owner == this, "query used a different transaction");
  ++database.queries;
  check(database.queries <= 3 && database.deletes == 0, "query order changed or was retried");
  require_sql(sql, {"FOR UPDATE"});
  maybe_throw(database.queries);

  if (database.queries == 1) {
    require_sql(sql, {"SELECT id FROM users", "username = ?", "is_deleted = 0"});
    check(params == std::vector<DbValue>{"alice"}, "user query parameters changed");
    return database.user_reply.value_or(DbQueryResult{true, {{"7"}}});
  }
  if (database.queries == 2) {
    require_sql(sql, {"SELECT id FROM virtual_files", "username = ?", "path = ?",
                      "file_type = 'directory'"});
    check(params == std::vector<DbValue>{"alice", database.directory},
          "parent query did not restrict user and current directory");
    return database.parent_reply.value_or(DbQueryResult{true, {{"12"}}});
  }
  require_sql(sql, {"SELECT id, object_hash FROM virtual_files", "username = ?", "parent_id = ?",
                    "file_name = ?", "file_type = 'file'"});
  check(params == std::vector<DbValue>{"alice", "12", database.filename},
        "file query did not restrict user, parent, and filename");
  if (database.file_reply)
    return *database.file_reply;
  for (const auto& file : database.pending) {
    if (file.username == "alice" && file.parent_id == "12" && file.name == database.filename)
      return {true, {{file.id, file.object_hash}}};
  }
  return {true, {}};
}

DbExecuteResult DbConnection::execute_prepared(const std::string& sql,
                                               const std::vector<DbValue>& params) {
  check(_transaction_active && database.owner == this, "delete used a different transaction");
  check(database.queries == 3 && ++database.deletes == 1,
        "delete ran before locking or was retried");
  require_sql(sql, {"DELETE FROM virtual_files", "id = ?", "username = ?", "parent_id = ?",
                    "file_name = ?", "file_type = 'file'", "object_hash = ?"});
  check(params == std::vector<DbValue>{"128", "alice", "12", database.filename, hash},
        "delete parameters did not identify the locked file");
  maybe_throw(4);
  if (database.delete_reply.success && database.delete_reply.affected_rows != 0) {
    auto& files = database.pending;
    files.erase(std::remove_if(files.begin(), files.end(),
                               [&](const File& file) {
                                 return file.id == *params[0] && file.username == *params[1] &&
                                        file.parent_id == *params[2] && file.name == *params[3] &&
                                        file.object_hash == *params[4];
                               }),
                files.end());
    // 模拟异常的多行删除，要求上层拒绝提交并回滚所有待提交变化。
    if (database.delete_reply.affected_rows > 1 && !files.empty())
      files.pop_back();
  }
  return database.delete_reply;
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

  // 提交响应丢失后客户端句柄结束，不补发 ROLLBACK 来假装撤销远端提交。
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

void test_invalid_arguments() {
  const std::vector<std::string> invalid_names{"",
                                               ".",
                                               "..",
                                               "/",
                                               "a/b",
                                               "a\\b",
                                               std::string(256, 'x'),
                                               std::string("a\0b", 3),
                                               "a\nb",
                                               std::string(1, '\x7f')};
  for (const auto& name : invalid_names) {
    reset();
    database.filename = name;
    const auto before = database.stored;
    check(execute() == DeleteFileResult::failed, "invalid filename was accepted");
    check(database.begins == 0 && database.queries == 0 && database.deletes == 0,
          "invalid arguments accessed the database");
    check_preserved(before);
  }
  reset();
  check(execute("") == DeleteFileResult::failed && database.begins == 0,
        "empty username was accepted");
  reset();
  database.directory.clear();
  check(execute() == DeleteFileResult::failed && database.begins == 0,
        "empty directory was accepted");
}

void test_path_limits() {
  for (const std::size_t size : {std::size_t{1024}, std::size_t{1025}}) {
    reset();
    // 构造绝对目录路径，使拼接后的完整路径恰好位于边界两侧。
    database.directory = "/" + std::string(size - database.filename.size() - 2, 'd');
    const auto before = database.stored;
    const auto result = execute();
    check(result == (size == 1024 ? DeleteFileResult::committed : DeleteFileResult::failed),
          "full path length boundary was handled incorrectly");
    if (size == 1025) {
      check(database.begins == 0, "overlong path began a transaction");
      check_preserved(before);
    }
  }
  // 根目录下的普通文件可以删除；文件名中空格和引号仍通过绑定参数传递。
  reset();
  database.directory = "/";
  database.filename = "a 'quoted' file.txt";
  database.stored[0].name = database.filename;
  check(execute() == DeleteFileResult::committed, "root directory or quoted filename rejected");
  reset();
  database.filename = std::string(255, 'x');
  database.stored[0].name = database.filename;
  check(execute() == DeleteFileResult::committed, "255-byte filename rejected");
}

void test_begin_failure() {
  reset();
  database.begin_ok = false;
  const auto before = database.stored;
  check(execute() == DeleteFileResult::failed, "begin failure reported success");
  check(database.begins == 1 && database.queries == 0 && database.deletes == 0,
        "begin failure continued to SQL");
  check_finished(0, 0);
  check_preserved(before);
}

void test_existing_transaction() {
  reset();
  DbConnection connection;
  const auto before = database.stored;
  {
    DbTransaction outer(connection);
    const auto result = delete_file_transaction(connection, "alice", "/docs", "report.txt");
    check(result == DeleteFileResult::failed && outer.active(),
          "nested deletion disturbed the caller's transaction");
    check(database.owner == &connection && database.queries == 0 && database.commits == 0 &&
              database.rollbacks == 0,
          "nested deletion queried or ended the caller's transaction");
  }
  check_finished(0, 1);
  check_preserved(before);
}

void test_query_results(int stage) {
  std::vector<DbQueryResult> invalid{
      {false, {}},  {false, {{"7"}}},         {true, {}},
      {true, {{}}}, {true, {{std::nullopt}}}, {true, {{"7"}, {"8"}}}};
  if (stage < 3) {
    invalid.push_back({true, {{"7", "extra"}}});
  } else {
    invalid.push_back({true, {{"128"}}});
    invalid.push_back({true, {{std::nullopt, hash}}});
    invalid.push_back({true, {{"128", std::nullopt}}});
    invalid.push_back({true, {{"128", hash, "extra"}}});
    invalid.push_back({true, {{"128", hash}, {"129", hash}}});
    for (const auto& value : {std::string{}, std::string(63, 'a'), std::string(65, 'a'),
                              std::string(64, 'A'), std::string(64, 'g')})
      invalid.push_back({true, {{"128", value}}});
  }
  for (const auto& reply : invalid) {
    reset();
    if (stage == 1)
      database.user_reply = reply;
    else if (stage == 2)
      database.parent_reply = reply;
    else
      database.file_reply = reply;
    const auto before = database.stored;
    check(execute() == DeleteFileResult::failed, "malformed/failed query was accepted");
    check(database.queries == stage && database.deletes == 0,
          "query failure continued to another operation");
    check_finished(0, 1);
    check_preserved(before);
  }
}

void test_delete_results() {
  for (const auto& reply : {DbExecuteResult{false, 1213, 0, 0}, DbExecuteResult{false, 1213, 1, 0},
                            DbExecuteResult{true, 0, 0, 0}, DbExecuteResult{true, 0, 2, 0}}) {
    reset();
    database.delete_reply = reply;
    const auto before = database.stored;
    check(execute() == DeleteFileResult::failed, "invalid affected rows reported success");
    check(database.deletes == 1, "deletion was skipped or retried");
    check_finished(0, 1);
    check_preserved(before);
  }
}

void test_commit(DbCommitStatus status, bool applies) {
  reset();
  const auto before = database.stored;
  database.commit_reply = status;
  database.unknown_applies = applies;
  const auto expected = status == DbCommitStatus::k_committed ? DeleteFileResult::committed
                        : status == DbCommitStatus::k_not_active
                            ? DeleteFileResult::failed
                            : DeleteFileResult::outcome_unknown;
  check(execute() == expected, "commit outcome mapped incorrectly");
  check(database.begins == 1 && database.queries == 3 && database.deletes == 1,
        "deletion did not execute once in the expected transaction");
  check_finished(1, 0);
  auto remaining = before;
  if (status == DbCommitStatus::k_committed ||
      (status == DbCommitStatus::k_outcome_unknown && applies))
    remaining.erase(remaining.begin());
  check(database.stored == remaining,
        "deletion changed another user's file or another reference to the same object");
}

void test_exceptions() {
  for (int stage = 1; stage <= 4; ++stage) {
    for (const bool nonstandard : {false, true}) {
      reset();
      database.throw_at = stage;
      database.throw_nonstandard = nonstandard;
      const auto before = database.stored;
      bool propagated = false;
      try {
        execute();
      } catch (const std::runtime_error& error) {
        propagated = !nonstandard && std::string(error.what()) == "injected database exception";
      } catch (int value) {
        propagated = nonstandard && value == 7;
      }
      check(propagated, "database exception was swallowed or replaced");
      check_finished(0, 1);
      check_preserved(before);
    }
  }
}

void test_repeat() {
  reset();
  check(execute() == DeleteFileResult::committed, "first deletion failed");
  const auto before = database.stored;
  // 保留已提交状态，仅重置本次调用的计数，第二次应在查询目标时停止。
  database.begins = database.queries = database.deletes = database.commits = database.rollbacks = 0;
  check(execute() == DeleteFileResult::failed, "missing file was reported deleted again");
  check(database.deletes == 0 && database.queries == 3, "missing file still issued DELETE");
  check_finished(0, 1);
  check_preserved(before);
}

void test_object_retention() {
  // 只剩最后一条文件引用时，也必须保留磁盘对象供后续 GC 处理。
  char pattern[] = "/tmp/tlss-delete-test-XXXXXX";
  const char* root = ::mkdtemp(pattern);
  check(root != nullptr, "mkdtemp failed");
  struct Directory {
    std::filesystem::path path;
    ~Directory() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } directory{root};
  // 测试由 CTest 单独进程运行，环境变量不会影响其他用例。
  check(::setenv("TLSS_STORAGE_ROOT", root, 1) == 0, "setenv failed");
  const auto object = directory.path / hash;
  {
    std::ofstream output(object, std::ios::binary);
    output << "object bytes must remain";
    check(output.good(), "write retained object failed");
  }
  reset();
  database.stored.resize(1);
  check(execute() == DeleteFileResult::committed && database.stored.empty(),
        "last reference was not deleted");
  check_finished(1, 0);
  std::ifstream input(object, std::ios::binary);
  std::string content;
  std::getline(input, content);
  check(content == "object bytes must remain", "deleting metadata removed or changed the object");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    check(argc == 2, "expected one scenario argument");
    const std::string scenario(argv[1]);
    if (scenario == "invalid_arguments")
      test_invalid_arguments();
    else if (scenario == "path_limits")
      test_path_limits();
    else if (scenario == "begin_failure")
      test_begin_failure();
    else if (scenario == "existing_transaction")
      test_existing_transaction();
    else if (scenario == "user_results")
      test_query_results(1);
    else if (scenario == "parent_results")
      test_query_results(2);
    else if (scenario == "file_results")
      test_query_results(3);
    else if (scenario == "delete_results")
      test_delete_results();
    else if (scenario == "committed")
      test_commit(DbCommitStatus::k_committed, true);
    else if (scenario == "unknown_applied")
      test_commit(DbCommitStatus::k_outcome_unknown, true);
    else if (scenario == "unknown_not_applied")
      test_commit(DbCommitStatus::k_outcome_unknown, false);
    else if (scenario == "commit_not_active")
      test_commit(DbCommitStatus::k_not_active, false);
    else if (scenario == "exceptions")
      test_exceptions();
    else if (scenario == "repeat")
      test_repeat();
    else if (scenario == "object_retention")
      test_object_retention();
    else
      throw std::invalid_argument("unknown scenario: " + scenario);
    std::cout << "PASS file deletion " << scenario << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
