#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ftp/upload_registration.h"

namespace {

using namespace TLSS::BASE;
using namespace TLSS::FileStorage;

const std::string object_hash(64, 'a');

// 区分事务内状态与已提交状态，验证两项登记一起提交或回滚。
// 此替代层不连接 MySQL，也不验证真实 InnoDB 的持久化和锁等待。
struct Database {
    DbConnection* owner = nullptr;
    bool begin_ok = true;
    bool user_exists = true;
    bool object_write_ok = true;
    bool throw_file_write = false;
    DbExecuteResult file_result{true, 0, 1, 0};
    DbCommitStatus commit_result = DbCommitStatus::k_committed;

    // COMMIT 响应丢失时，模拟数据库实际提交或实际未提交。
    bool unknown_applies = false;
    bool user_locked = false;
    bool pending_object = false;
    bool pending_file = false;
    bool stored_object = false;
    bool stored_file = false;

    int begins = 0;
    int commits = 0;
    int rollbacks = 0;
    int object_writes = 0;
    int file_writes = 0;
} database;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

namespace TLSS::BASE {

DbConnection::DbConnection()
    : _conn(nullptr),
      _connected(false),
      _transaction_active(false) {}

DbConnection::~DbConnection() = default;

DbQueryResult DbConnection::query_prepared(
    const std::string& sql, const std::vector<DbValue>& params) {
    check(_transaction_active && database.owner == this,
          "query did not use the transaction connection");
    check(sql.find("FOR UPDATE") != std::string::npos,
          "query did not request a row lock");

    if (sql.find("SELECT id FROM users ") == 0) {
        check(params == std::vector<DbValue>{"alice"},
              "unexpected user lookup parameters");
        database.user_locked = true;
        if (!database.user_exists) {
            return {true, {}};
        }
        return {true, {{"7"}}};
    }

    if (sql.find("SELECT state, unreferenced_at ") == 0) {
        check(database.user_locked, "object was locked before the user");
        check(params == std::vector<DbValue>{object_hash},
              "unexpected object lookup parameters");
        // 本组测试模拟首次登记：对象表尚无记录。
        return {true, {}};
    }

    throw std::runtime_error("unexpected query: " + sql);
}

DbExecuteResult DbConnection::execute_prepared(
    const std::string& sql, const std::vector<DbValue>& params) {
    check(_transaction_active && database.owner == this,
          "write did not use the transaction connection");
    check(database.user_locked, "write happened before locking the user");

    if (sql.find("INSERT INTO storage_objects ") == 0) {
        check(params == std::vector<DbValue>{object_hash, "LIVE"},
              "unexpected object insert parameters");
        ++database.object_writes;
        if (!database.object_write_ok) {
            return {};
        }
        database.pending_object = true;
        return {true, 0, 1, 0};
    }

    if (sql.find("INSERT INTO virtual_files ") == 0) {
        check(database.pending_object,
              "file reference was inserted before preparing the object");
        check(params == std::vector<DbValue>{
                  "file.bin", "/docs/file.bin", object_hash, "42", "alice", "/docs"},
              "unexpected file insert parameters");
        ++database.file_writes;
        if (database.throw_file_write) {
            throw std::runtime_error("injected file write failure");
        }
        if (database.file_result.success && database.file_result.affected_rows == 1) {
            database.pending_file = true;
        }
        return database.file_result;
    }

    throw std::runtime_error("unexpected write: " + sql);
}

// 开启事务只创建工作副本，不修改已提交状态。
DbTransaction::DbTransaction(DbConnection& connection) noexcept
    : connection_(connection),
      active_(database.begin_ok && !connection._transaction_active) {
    ++database.begins;
    if (active_) {
        connection_._transaction_active = true;
        database.owner = &connection_;
        database.pending_object = database.stored_object;
        database.pending_file = database.stored_file;
    }
}

DbTransaction::~DbTransaction() noexcept {
    rollback();
}

DbCommitStatus DbTransaction::commit() noexcept {
    if (!active_) {
        return DbCommitStatus::k_not_active;
    }
    ++database.commits;

    const bool applied =
        database.commit_result == DbCommitStatus::k_committed ||
        (database.commit_result == DbCommitStatus::k_outcome_unknown && database.unknown_applies);
    if (applied) {
        database.stored_object = database.pending_object;
        database.stored_file = database.pending_file;
    }

    // 未知结果也结束客户端事务句柄，不再用回滚假装撤销可能已完成的提交。
    database.pending_object = database.stored_object;
    database.pending_file = database.stored_file;
    connection_._transaction_active = false;
    database.owner = nullptr;
    active_ = false;
    return database.commit_result;
}

bool DbTransaction::rollback() noexcept {
    if (active_) {
        ++database.rollbacks;
        database.pending_object = database.stored_object;
        database.pending_file = database.stored_file;
        connection_._transaction_active = false;
        database.owner = nullptr;
        active_ = false;
    }
    return true;
}

}  // namespace TLSS::BASE

namespace {

UploadCommitResult execute_registration() {
    DbConnection connection;
    return commit_upload_transaction(
        connection, "alice", "42", "/docs", "file.bin", object_hash);
}

void verify_case(const std::string& name,
                 UploadCommitResult expected,
                 int expected_commits,
                 int expected_rollbacks,
                 bool expected_stored) {
    const auto result = execute_registration();
    check(result == expected, name + ": unexpected result");
    check(database.begins == 1, name + ": unexpected transaction count");
    check(database.commits == expected_commits, name + ": unexpected commit count");
    check(database.rollbacks == expected_rollbacks, name + ": unexpected rollback count");
    check(database.stored_object == expected_stored && database.stored_file == expected_stored,
          name + ": unexpected committed state");
    check(database.owner == nullptr, name + ": transaction remained active");

    // 成功或未知提交都只能执行一轮写入，不能自动重试。
    if (expected_commits == 1) {
        check(database.object_writes == 1 && database.file_writes == 1,
              name + ": writes were skipped or retried");
    }
}

}  // namespace

int main() {
    try {
        // 两项登记一起提交一次。
        database = {};
        verify_case("success", UploadCommitResult::committed, 1, 0, true);

        // 文件插入失败，之前的对象登记也必须回滚。
        database = {};
        database.file_result = {};
        verify_case("file insert failure", UploadCommitResult::failed, 0, 1, false);
        check(database.object_writes == 1 && database.file_writes == 1,
              "file failure did not exercise both writes");

        // SQL 成功但插入零行，同样不能提交对象记录。
        database = {};
        database.file_result = {true, 0, 0, 0};
        verify_case("file insert affected zero rows", UploadCommitResult::failed, 0, 1, false);

        // 对象准备失败时，不允许继续建立文件引用。
        database = {};
        database.object_write_ok = false;
        verify_case("object insert failure", UploadCommitResult::failed, 0, 1, false);
        check(database.file_writes == 0, "object failure still inserted a file reference");

        database = {};
        database.user_exists = false;
        verify_case("missing user", UploadCommitResult::failed, 0, 1, false);
        check(database.object_writes == 0 && database.file_writes == 0,
              "missing user triggered writes");

        // 开启事务失败，不执行提交或回滚命令。
        database = {};
        database.begin_ok = false;
        verify_case("begin failure", UploadCommitResult::failed, 0, 0, false);

        // 响应丢失的两种真实结果，对调用者都必须报告 outcome_unknown。
        database = {};
        database.commit_result = DbCommitStatus::k_outcome_unknown;
        database.unknown_applies = true;
        verify_case("unknown outcome applied", UploadCommitResult::outcome_unknown, 1, 0, true);

        database = {};
        database.commit_result = DbCommitStatus::k_outcome_unknown;
        database.unknown_applies = false;
        verify_case("unknown outcome not applied", UploadCommitResult::outcome_unknown, 1, 0, false);

        // 写入抛异常时继续传播，并由事务析构执行回滚。
        database = {};
        database.throw_file_write = true;
        bool propagated = false;
        try {
            execute_registration();
        } catch (const std::runtime_error& error) {
            propagated = std::string(error.what()) == "injected file write failure";
        }
        check(propagated, "file write exception was swallowed");
        check(database.rollbacks == 1 && database.commits == 0,
              "exception did not roll back exactly once");
        check(!database.stored_object && !database.stored_file,
              "exception left partially committed metadata");
        check(database.owner == nullptr, "exception left the transaction active");

        std::cout << "PASS upload registration\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
