#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "ftp/object_repository.h"

namespace {

using namespace TLSS::BASE;

const std::string object_hash(64, 'a');

// 模拟数据库结果，记录真实仓储发出的查询、写入及事务提交。
struct Database {
    DbConnection* owner = nullptr;
    DbQueryResult lookup{true, {}};
    DbExecuteResult write_result{true, 0, 1, 0};
    int queries = 0;
    int writes = 0;
    int commits = 0;
    std::string write_sql;
    std::vector<DbValue> write_params;
} database;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

// 仅替换数据库边界，不连接 MySQL；仓储使用生产实现。
namespace TLSS::BASE {

DbConnection::DbConnection()
    : _conn(nullptr),
      _connected(false),
      _transaction_active(false) {}

DbConnection::~DbConnection() = default;

DbQueryResult DbConnection::query_prepared(
    const std::string& sql, const std::vector<DbValue>& params) {
    check(_transaction_active && database.owner == this,
          "lookup did not use the caller's transaction");
    check(sql.find("FOR UPDATE") != std::string::npos,
          "object lookup did not request a row lock");
    check(params == std::vector<DbValue>{object_hash},
          "object lookup used unexpected parameters");
    ++database.queries;
    return database.lookup;
}

DbExecuteResult DbConnection::execute_prepared(
    const std::string& sql, const std::vector<DbValue>& params) {
    check(_transaction_active && database.owner == this,
          "write did not use the caller's transaction");
    ++database.writes;
    database.write_sql = sql;
    database.write_params = params;
    return database.write_result;
}

// 模拟调用者开启事务，验证仓储使用同一连接且不自行提交或回滚。
// 此替代层不验证真实 InnoDB 的锁等待或持久化行为。
DbTransaction::DbTransaction(DbConnection& connection) noexcept
    : connection_(connection), active_(true) {
    connection_._transaction_active = true;
    database.owner = &connection_;
}

DbTransaction::~DbTransaction() noexcept {
    rollback();
}

DbCommitStatus DbTransaction::commit() noexcept {
    if (!active_) {
        return DbCommitStatus::k_not_active;
    }
    ++database.commits;
    connection_._transaction_active = false;
    database.owner = nullptr;
    active_ = false;
    return DbCommitStatus::k_committed;
}

bool DbTransaction::rollback() noexcept {
    if (active_) {
        connection_._transaction_active = false;
        database.owner = nullptr;
        active_ = false;
    }
    return true;
}

}  // namespace TLSS::BASE

namespace {

// 每个场景独立设置结果，并在调用者事务中执行真实仓储方法。
void run_case(const std::string& name,
              DbQueryResult lookup,
              DbExecuteResult write_result,
              bool expected_result,
              const std::string& expected_write_prefix,
              const std::vector<DbValue>& expected_params) {
    database = {};
    database.lookup = lookup;
    database.write_result = write_result;

    DbConnection connection;
    DbTransaction transaction(connection);
    TLSS::FileStorage::ObjectRepository repository(connection);

    const bool result = repository.prepare_reference_in_transaction(object_hash);
    check(result == expected_result, name + ": unexpected result");
    check(database.queries == 1, name + ": unexpected query count");

    const int expected_writes = expected_write_prefix.empty() ? 0 : 1;
    check(database.writes == expected_writes, name + ": unexpected write count");
    if (expected_writes != 0) {
        check(database.write_sql.find(expected_write_prefix) == 0,
              name + ": unexpected write operation");
        check(database.write_params == expected_params,
              name + ": unexpected write parameters");
    }

    // 文件引用尚未登记，仓储必须把最终提交留给调用者。
    check(transaction.active() && database.commits == 0,
          name + ": repository ended the caller's transaction");
}

}  // namespace

int main() {
    try {
        // 新对象必须插入一行；零行或执行失败都不能报告成功。
        run_case("insert new object", {true, {}}, {true, 0, 1, 0}, true,
                 "INSERT INTO storage_objects", {object_hash, "LIVE"});
        run_case("insert affected zero rows", {true, {}}, {true, 0, 0, 0}, false,
                 "INSERT INTO storage_objects", {object_hash, "LIVE"});
        run_case("insert failure", {true, {}}, {}, false,
                 "INSERT INTO storage_objects", {object_hash, "LIVE"});

        // LIVE 无需实际修改时，affected_rows 为零仍应成功。
        run_case("reuse LIVE object", {true, {{"LIVE", std::nullopt}}},
                 {true, 0, 0, 0}, true, "UPDATE storage_objects", {object_hash});
        // 正文已经由调用者验证，允许恢复 UNAVAILABLE。
        run_case("restore UNAVAILABLE object", {true, {{"UNAVAILABLE", std::nullopt}}},
                 {true, 0, 1, 0}, true, "UPDATE storage_objects", {object_hash});

        // 删除中或未知状态必须拒绝，不能清空计时或恢复为 LIVE。
        run_case("reject DELETING object", {true, {{"DELETING", std::nullopt}}},
                 {true, 0, 1, 0}, false, "", {});
        run_case("reject unknown state", {true, {{"UNKNOWN", std::nullopt}}},
                 {true, 0, 1, 0}, false, "", {});

        // 数据库失败必须传播，查询失败不能被视为记录不存在。
        run_case("lookup failure", {false, {}}, {true, 0, 1, 0}, false, "", {});
        run_case("update failure", {true, {{"LIVE", std::nullopt}}}, {}, false,
                 "UPDATE storage_objects", {object_hash});

        // 拒绝结构异常的结果，不进行数据库写入。
        run_case("NULL state", {true, {{std::nullopt, std::nullopt}}},
                 {true, 0, 1, 0}, false, "", {});
        run_case("invalid column count", {true, {{"LIVE"}}},
                 {true, 0, 1, 0}, false, "", {});
        run_case("unexpected multiple rows",
                 {true, {{"LIVE", std::nullopt}, {"LIVE", std::nullopt}}},
                 {true, 0, 1, 0}, false, "", {});

        // 非法哈希应在执行任何 SQL 之前被拒绝。
        database = {};
        DbConnection connection;
        DbTransaction transaction(connection);
        TLSS::FileStorage::ObjectRepository repository(connection);
        check(!repository.prepare_reference_in_transaction("invalid"),
              "invalid hash was accepted");
        check(database.queries == 0 && database.writes == 0,
              "invalid hash triggered database access");
        check(transaction.active() && database.commits == 0,
              "invalid hash ended the caller's transaction");

        std::cout << "PASS object reference\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
