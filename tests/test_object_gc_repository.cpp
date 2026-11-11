#include "ftp/object_repository.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace TLSS::BASE;
using TLSS::FileStorage::ObjectRepository;
using Seconds = std::chrono::seconds;

const std::string valid_hash(64, 'a');

class DatabaseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// 记录 SQL、参数和事务操作，由每个测试独立配置返回结果。
// 这里只验证仓储契约，不模拟 InnoDB 的时间计算或锁等待。
struct Database {
    DbConnection* owner = nullptr;

    DbQueryResult query_result{true, {}};
    DbExecuteResult write_result{true, 0, 1, 0};
    bool throw_query = false;
    bool throw_write = false;

    std::string query_sql;
    std::string write_sql;
    std::vector<DbValue> query_params;
    std::vector<DbValue> write_params;

    int queries = 0;
    int writes = 0;
    int commits = 0;
    int rollbacks = 0;
} database;

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void contains(const std::string& sql, const char* fragment) {
    check(sql.find(fragment) != std::string::npos,
          "required SQL condition is missing");
}

void check_no_access() {
    check(database.queries == 0 && database.writes == 0,
          "invalid arguments triggered database access");
}

void check_write_result(
    const DbExecuteResult& actual,
    const DbExecuteResult& expected) {
    check(actual.success == expected.success &&
              actual.error_code == expected.error_code &&
              actual.affected_rows == expected.affected_rows &&
              actual.insert_id == expected.insert_id,
          "repository changed the execution result");
}

}  // namespace

namespace TLSS::BASE {

// 替换连接实现，避免访问真实数据库。
DbConnection::DbConnection()
    : _conn(nullptr),
      _connected(false),
      _transaction_active(false) {}

DbConnection::~DbConnection() = default;

DbQueryResult DbConnection::query_prepared(
    const std::string& sql,
    const std::vector<DbValue>& params) {
    check(_transaction_active && database.owner == this,
          "query did not use the caller's transaction");

    ++database.queries;
    database.query_sql = sql;
    database.query_params = params;
    if (database.throw_query)
        throw DatabaseError("injected query failure");
    return database.query_result;
}

DbExecuteResult DbConnection::execute_prepared(
    const std::string& sql,
    const std::vector<DbValue>& params) {
    check(_transaction_active && database.owner == this,
          "write did not use the caller's transaction");

    ++database.writes;
    database.write_sql = sql;
    database.write_params = params;
    if (database.throw_write)
        throw DatabaseError("injected write failure");
    return database.write_result;
}

// 仓储只执行 SQL，事务的创建、提交和回滚由调用者负责。
DbTransaction::DbTransaction(DbConnection& connection) noexcept
    : connection_(connection),
      active_(true) {
    connection_._transaction_active = true;
    database.owner = &connection_;
}

DbTransaction::~DbTransaction() noexcept {
    rollback();
}

DbCommitStatus DbTransaction::commit() noexcept {
    if (!active_)
        return DbCommitStatus::k_not_active;

    ++database.commits;
    connection_._transaction_active = false;
    database.owner = nullptr;
    active_ = false;
    return DbCommitStatus::k_committed;
}

bool DbTransaction::rollback() noexcept {
    if (active_) {
        ++database.rollbacks;
        connection_._transaction_active = false;
        database.owner = nullptr;
        active_ = false;
    }
    return true;
}

}  // namespace TLSS::BASE

namespace {

// 每个场景使用独立连接和事务。
// 在调用结束后、事务析构前，检查仓储没有擅自结束事务。
template <typename Operation>
void in_transaction(Operation operation) {
    database = {};

    DbConnection connection;
    DbTransaction transaction(connection);
    ObjectRepository repository(connection);

    operation(repository);

    check(transaction.active() &&
              database.commits == 0 &&
              database.rollbacks == 0,
          "repository ended the caller's transaction");
}

void test_reference_query() {
    // 分别验证：有引用、无引用、查询失败。
    const std::vector<DbQueryResult> results{
        {true, {{"42"}}},
        {true, {}},
        {false, {}},
    };

    for (const auto& expected : results) {
        in_transaction([&](ObjectRepository& repository) {
            database.query_result = expected;

            const auto result =
                repository.find_reference_for_update(valid_hash);

            check(result.success == expected.success &&
                      result.rows == expected.rows,
                  "reference query result was changed");
            check(database.queries == 1 && database.writes == 0,
                  "unexpected reference query operations");
            check(database.query_params ==
                      std::vector<DbValue>{valid_hash},
                  "reference query used unexpected parameters");

            contains(database.query_sql, "SELECT id ");
            contains(database.query_sql, "FROM virtual_files ");
            contains(database.query_sql, "WHERE object_hash = ? ");
            contains(database.query_sql, "AND file_type = 'file' ");
            contains(database.query_sql, "LIMIT 1 ");
            contains(database.query_sql, "FOR UPDATE");

            // 引用必须跨用户查询，不能只检查当前用户。
            check(database.query_sql.find("username") == std::string::npos,
                  "reference query was restricted to one user");
        });
    }
}

void test_mark_unreferenced() {
    in_transaction([](ObjectRepository& repository) {
        const auto result = repository.mark_unreferenced(valid_hash);

        check_write_result(result, database.write_result);
        check(database.queries == 0 && database.writes == 1,
              "unexpected mark-unreferenced operations");
        check(database.write_params ==
                  std::vector<DbValue>{valid_hash},
              "unexpected mark-unreferenced parameters");

        contains(database.write_sql, "UPDATE storage_objects ");
        contains(database.write_sql,
                 "SET unreferenced_at = CURRENT_TIMESTAMP(6) ");
        contains(database.write_sql, "WHERE object_hash = ? ");
        contains(database.write_sql,
                 "AND state IN ('LIVE', 'UNAVAILABLE') ");
        // 防止每次扫描都刷新首次发现时间。
        contains(database.write_sql, "AND unreferenced_at IS NULL");
    });
}

void test_clear_unreferenced() {
    in_transaction([](ObjectRepository& repository) {
        const auto result = repository.clear_unreferenced(valid_hash);

        check_write_result(result, database.write_result);
        check(database.queries == 0 && database.writes == 1,
              "unexpected clear-unreferenced operations");
        check(database.write_params ==
                  std::vector<DbValue>{valid_hash},
              "unexpected clear-unreferenced parameters");

        contains(database.write_sql, "SET unreferenced_at = NULL ");
        contains(database.write_sql, "WHERE object_hash = ? ");
        contains(database.write_sql,
                 "AND state IN ('LIVE', 'UNAVAILABLE') ");
        contains(database.write_sql, "AND unreferenced_at IS NOT NULL");

        // 清空时间不能擅自把 UNAVAILABLE 恢复成 LIVE。
        check(database.write_sql.find("SET state") == std::string::npos &&
                  database.write_sql.find(", state") == std::string::npos,
              "clearing the timestamp changed the object state");
    });
}

void test_mark_deleting() {
    in_transaction([](ObjectRepository& repository) {
        const auto result =
            repository.mark_deleting_if_expired(valid_hash, Seconds{600});

        check_write_result(result, database.write_result);
        check(database.queries == 0 && database.writes == 1,
              "unexpected mark-deleting operations");
        check(database.write_params ==
                  std::vector<DbValue>{valid_hash, "600"},
              "unexpected hash or grace-period parameters");

        contains(database.write_sql, "SET state = 'DELETING' ");
        contains(database.write_sql, "WHERE object_hash = ? ");
        contains(database.write_sql,
                 "AND state IN ('LIVE', 'UNAVAILABLE') ");
        contains(database.write_sql, "AND unreferenced_at IS NOT NULL ");
        contains(database.write_sql,
                 "AND unreferenced_at <= DATE_SUB("
                 "CURRENT_TIMESTAMP(6), INTERVAL ? SECOND)");
    });
}

void test_write_results() {
    // 零行更新必须保留为成功且零行；数据库失败也必须原样返回。
    const std::vector<DbExecuteResult> results{
        {true, 0, 0, 0},
        {false, 1213, 0, 0},
    };

    for (const auto& expected : results) {
        for (int method = 0; method != 3; ++method) {
            in_transaction([&](ObjectRepository& repository) {
                database.write_result = expected;

                DbExecuteResult actual;
                if (method == 0)
                    actual = repository.mark_unreferenced(valid_hash);
                else if (method == 1)
                    actual = repository.clear_unreferenced(valid_hash);
                else
                    actual = repository.mark_deleting_if_expired(
                        valid_hash, Seconds{600});

                check_write_result(actual, expected);
                check(database.queries == 0 && database.writes == 1,
                      "write result caused a retry or extra query");
            });
        }
    }
}

void test_invalid_arguments() {
    const std::vector<std::string> invalid_hashes{
        "",
        "invalid",
        std::string(63, 'a'),
        std::string(65, 'a'),
        std::string(64, 'A'),
        std::string(64, 'g'),
    };

    for (const auto& hash : invalid_hashes) {
        in_transaction([&](ObjectRepository& repository) {
            check(!repository.find_reference_for_update(hash).success,
                  "reference query accepted invalid hash");
            check(!repository.mark_unreferenced(hash).success,
                  "mark-unreferenced accepted invalid hash");
            check(!repository.clear_unreferenced(hash).success,
                  "clear-unreferenced accepted invalid hash");

            // 哈希非法，即使宽限期有效也必须拒绝。
            // 这一项能捕获误用 && 的问题。
            check(!repository.mark_deleting_if_expired(
                       hash, Seconds{600}).success,
                  "mark-deleting accepted invalid hash");
            check_no_access();
        });
    }

    for (const auto grace : {Seconds{0}, Seconds{-1}}) {
        in_transaction([&](ObjectRepository& repository) {
            // 哈希合法，宽限期非法，也必须在访问数据库前拒绝。
            check(!repository.mark_deleting_if_expired(
                       valid_hash, grace).success,
                  "non-positive grace period was accepted");
            check_no_access();
        });
    }
}

void test_exception_propagation() {
    // 给四个方法分别注入数据库异常，要求异常向上传播且不结束事务。
    for (int method = 0; method != 4; ++method) {
        in_transaction([&](ObjectRepository& repository) {
            database.throw_query = method == 0;
            database.throw_write = method != 0;
            bool caught = false;

            try {
                if (method == 0)
                    repository.find_reference_for_update(valid_hash);
                else if (method == 1)
                    repository.mark_unreferenced(valid_hash);
                else if (method == 2)
                    repository.clear_unreferenced(valid_hash);
                else
                    repository.mark_deleting_if_expired(valid_hash, Seconds{600});
            } catch (const DatabaseError&) {
                caught = true;
            }

            check(caught, "database exception was swallowed or changed");
            check(database.queries == (method == 0 ? 1 : 0) &&
                      database.writes == (method == 0 ? 0 : 1),
                  "database exception caused a retry or extra operation");
        });
    }
}

}  // namespace

int main() {
    try {
        test_reference_query();
        test_mark_unreferenced();
        test_clear_unreferenced();
        test_mark_deleting();
        test_write_results();
        test_invalid_arguments();
        test_exception_propagation();

        std::cout << "PASS object GC repository\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
