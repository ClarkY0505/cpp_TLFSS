#include "ftp/object_gc_transaction.h"

#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace TLSS::BASE;
using namespace TLSS::FileStorage;
using Seconds = std::chrono::seconds;

const std::string object_hash(64, 'a');
const std::string previous_time = "2026-11-11 10:00:00.000000";
const std::string discovery_time = "2026-11-11 10:10:00.000000";

enum class WriteKind { none, clear_time, mark_time, mark_deleting, remove_record };
enum class ThrowPoint { none, object_query, reference_query, after_write };

class DatabaseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ObjectRecord {
    std::string state = "LIVE";
    std::optional<std::string> unreferenced_at = previous_time;
};

// 只替换数据库边界；事务流程和对象仓储均使用生产实现。
// 时间是否到期由配置的 UPDATE 返回值表达，不在这里复制 SQL 的日期算法。
// 这些测试不验证真实 InnoDB 锁等待、持久化或进程间并发。
struct Database {
    DbConnection* owner = nullptr;
    bool begin_ok = true;
    // object_exists 是已提交状态，pending_exists 是事务内工作副本。
    bool object_exists = true;
    bool pending_exists = true;
    bool object_locked = false;
    bool reference_checked = false;

    // 指定此字段可模拟查询失败或异常的行结构。
    std::optional<DbQueryResult> object_reply;
    DbQueryResult reference_reply{true, {}};
    DbExecuteResult write_reply{true, 0, 1, 0};
    DbCommitStatus commit_reply = DbCommitStatus::k_committed;
    bool unknown_applies = false;
    ThrowPoint throw_at = ThrowPoint::none;

    // BEGIN 创建工作副本；只有提交被应用时才改变 stored。
    ObjectRecord stored;
    ObjectRecord pending;
    WriteKind last_write = WriteKind::none;

    int begins = 0;
    int object_queries = 0;
    int reference_queries = 0;
    int writes = 0;
    int commits = 0;
    int rollbacks = 0;
} database;

void check(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

void check_record(const ObjectRecord& expected, const std::string& name) {
    check(database.stored.state == expected.state &&
              database.stored.unreferenced_at == expected.unreferenced_at,
          name + ": unexpected committed object record");
}

void configure_write(WriteKind kind) {
    if (kind == WriteKind::clear_time)
        database.reference_reply = {true, {{"42"}}};
    else if (kind == WriteKind::mark_time)
        database.stored.unreferenced_at.reset();
}

ObjectRecord after_write(ObjectRecord record, WriteKind kind) {
    if (kind == WriteKind::clear_time)
        record.unreferenced_at.reset();
    else if (kind == WriteKind::mark_time)
        record.unreferenced_at = discovery_time;
    else if (kind == WriteKind::mark_deleting)
        record.state = "DELETING";
    return record;
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
    check(params == std::vector<DbValue>{object_hash},
          "unexpected lookup parameters");

    if (sql.find("SELECT state, unreferenced_at ") == 0) {
        ++database.object_queries;
        check(database.reference_queries == 0 && database.writes == 0,
              "object query happened after reference processing");
        if (database.throw_at == ThrowPoint::object_query)
            throw DatabaseError("injected object query failure");

        if (database.object_reply.has_value()) {
            database.object_locked = database.object_reply->success &&
                                     !database.object_reply->rows.empty();
            return *database.object_reply;
        }
        if (!database.pending_exists)
            return {true, {}};

        database.object_locked = true;
        return {true, {{database.pending.state, database.pending.unreferenced_at}}};
    }

    if (sql.find("SELECT id FROM virtual_files ") == 0) {
        ++database.reference_queries;
        check(database.object_locked, "reference checked before locking the object");
        check(database.writes == 0, "reference checked after modifying the object");
        if (database.throw_at == ThrowPoint::reference_query)
            throw DatabaseError("injected reference query failure");

        database.reference_checked = true;
        return database.reference_reply;
    }

    throw std::runtime_error("unexpected query: " + sql);
}

DbExecuteResult DbConnection::execute_prepared(
    const std::string& sql, const std::vector<DbValue>& params) {
    check(_transaction_active && database.owner == this,
          "write did not use the transaction connection");
    check(database.object_locked && database.reference_checked,
          "write happened before object and reference checks");
    check(database.reference_reply.success, "write happened after a failed reference query");

    ++database.writes;
    check(database.writes == 1, "object was updated more than once");

    if (sql.find("DELETE FROM storage_objects ") == 0) {
        database.last_write = WriteKind::remove_record;
        check(database.pending.state == "DELETING", "deleted an object outside DELETING");
        check(database.reference_reply.rows.empty(), "deleted a referenced object record");
        check(sql.find("WHERE object_hash = ? ") != std::string::npos &&
                  sql.find("AND state = 'DELETING'") != std::string::npos,
              "object deletion omitted the hash or state condition");
    } else if (sql.find("UPDATE storage_objects SET unreferenced_at = NULL ") == 0) {
        database.last_write = WriteKind::clear_time;
        check(!database.reference_reply.rows.empty(), "cleared timer without a reference");
    } else if (sql.find("UPDATE storage_objects SET unreferenced_at = CURRENT_TIMESTAMP(6) ") == 0) {
        database.last_write = WriteKind::mark_time;
        check(database.reference_reply.rows.empty(), "started timer despite a reference");
    } else if (sql.find("UPDATE storage_objects SET state = 'DELETING' ") == 0) {
        database.last_write = WriteKind::mark_deleting;
        check(database.reference_reply.rows.empty(), "marked referenced object for deletion");
    } else {
        throw std::runtime_error("unexpected write: " + sql);
    }

    const std::vector<DbValue> expected_params =
        database.last_write == WriteKind::mark_deleting
            ? std::vector<DbValue>{object_hash, "600"}
            : std::vector<DbValue>{object_hash};
    check(params == expected_params, "unexpected update parameters");

    if (database.write_reply.success && database.write_reply.affected_rows > 0) {
        if (database.last_write == WriteKind::remove_record)
            database.pending_exists = false;
        else
            database.pending = after_write(database.pending, database.last_write);
    }

    // 模拟 SQL 已改变事务内状态，随后响应处理抛异常，验证栈展开会回滚。
    if (database.throw_at == ThrowPoint::after_write)
        throw DatabaseError("injected failure after object write");
    return database.write_reply;
}

DbTransaction::DbTransaction(DbConnection& connection) noexcept
    : connection_(connection),
      active_(database.begin_ok && !connection._transaction_active) {
    ++database.begins;
    if (active_) {
        connection_._transaction_active = true;
        database.owner = &connection_;
        database.pending = database.stored;
        database.pending_exists = database.object_exists;
    }
}

DbTransaction::~DbTransaction() noexcept {
    rollback();
}

DbCommitStatus DbTransaction::commit() noexcept {
    if (!active_)
        return DbCommitStatus::k_not_active;
    ++database.commits;

    const bool applied = database.commit_reply == DbCommitStatus::k_committed ||
        (database.commit_reply == DbCommitStatus::k_outcome_unknown && database.unknown_applies);
    if (applied) {
        database.stored = database.pending;
        database.object_exists = database.pending_exists;
    }

    // 与生产事务句柄一致：提交结果未知也结束本地事务，不再自动回滚或重试。
    database.pending = database.stored;
    database.pending_exists = database.object_exists;
    connection_._transaction_active = false;
    database.owner = nullptr;
    active_ = false;
    return database.commit_reply;
}

bool DbTransaction::rollback() noexcept {
    if (active_) {
        ++database.rollbacks;
        database.pending = database.stored;
        database.pending_exists = database.object_exists;
        connection_._transaction_active = false;
        database.owner = nullptr;
        active_ = false;
    }
    return true;
}

}  // namespace TLSS::BASE

namespace {

ObjectGcPrepareResult execute(const std::string& hash = object_hash,
                              Seconds grace_period = Seconds{600}) {
    DbConnection connection;
    return prepare_object_gc_transaction(connection, hash, grace_period);
}

void verify_case(const std::string& name, ObjectGcPrepareResult expected,
                 int commits, int rollbacks, int writes,
                 int reference_queries = 1, int object_queries = 1) {
    const auto result = execute();
    check(result == expected, name + ": unexpected preparation result");
    check(database.begins == 1 && database.commits == commits &&
              database.rollbacks == rollbacks,
          name + ": unexpected transaction operations");
    check(database.writes == writes && database.reference_queries == reference_queries &&
              database.object_queries == object_queries,
          name + ": unexpected SQL counts or retry");
    check(database.owner == nullptr, name + ": transaction remained active");
}

void test_invalid_arguments() {
    for (const auto& hash : {std::string{}, std::string("invalid"), std::string(63, 'a'),
                             std::string(65, 'a'), std::string(64, 'A'), std::string(64, 'g')}) {
        database = {};
        check(execute(hash) == ObjectGcPrepareResult::failed, "invalid hash was accepted");
        check(database.begins == 0 && database.object_queries == 0 &&
                  database.reference_queries == 0 && database.writes == 0,
              "invalid hash accessed the database");
    }
    for (const auto grace : {Seconds{0}, Seconds{-1}}) {
        database = {};
        check(execute(object_hash, grace) == ObjectGcPrepareResult::failed,
              "non-positive grace period was accepted");
        check(database.begins == 0 && database.object_queries == 0 &&
                  database.reference_queries == 0 && database.writes == 0,
              "invalid grace period accessed the database");
    }
}

void test_begin_and_lookup() {
    database = {};
    database.begin_ok = false;
    verify_case("begin failure", ObjectGcPrepareResult::failed, 0, 0, 0, 0, 0);

    database = {};
    database.object_reply = DbQueryResult{false, {}};
    verify_case("object lookup failure", ObjectGcPrepareResult::failed, 0, 1, 0, 0);

    database = {};
    database.object_exists = false;
    verify_case("missing object", ObjectGcPrepareResult::skipped, 0, 1, 0, 0);
}

void test_malformed_results() {
    const std::vector<DbQueryResult> object_results{
        {true, {{}}},
        {true, {{"LIVE"}}},
        {true, {{"LIVE", std::nullopt, "extra"}}},
        {true, {{std::nullopt, std::nullopt}}},
        {true, {{"UNKNOWN", std::nullopt}}},
        {true, {{"LIVE", std::nullopt}, {"LIVE", std::nullopt}}},
    };
    for (const auto& reply : object_results) {
        database = {};
        database.object_reply = reply;
        verify_case("malformed object", ObjectGcPrepareResult::failed, 0, 1, 0, 0);
    }

    const std::vector<DbQueryResult> reference_results{
        {false, {}},
        {true, {{}}},
        {true, {{std::nullopt}}},
        {true, {{"42", "extra"}}},
        {true, {{"42"}, {"43"}}},
    };
    for (const auto& reply : reference_results) {
        database = {};
        database.reference_reply = reply;
        verify_case("invalid reference result", ObjectGcPrepareResult::failed, 0, 1, 0);
    }
}

void test_references() {
    // 有引用时清空时间，并保留 LIVE 或 UNAVAILABLE 状态。
    for (const auto* state : {"LIVE", "UNAVAILABLE"}) {
        for (const bool already_clear : {false, true}) {
            database = {};
            database.stored.state = state;
            configure_write(WriteKind::clear_time);
            if (already_clear) {
                database.stored.unreferenced_at.reset();
                database.write_reply.affected_rows = 0;
            }
            verify_case("referenced object", ObjectGcPrepareResult::skipped, 1, 0, 1);
            check_record({state, std::nullopt}, "referenced object");
            check(database.last_write == WriteKind::clear_time, "wrong reference update");
        }
    }

    database = {};
    database.stored.state = "DELETING";
    database.reference_reply = {true, {{"42"}}};
    const auto original = database.stored;
    verify_case("referenced DELETING object", ObjectGcPrepareResult::failed, 0, 1, 0);
    check_record(original, "referenced DELETING object");
}

void test_unreferenced() {
    for (const auto* state : {"LIVE", "UNAVAILABLE"}) {
        database = {};
        database.stored.state = state;
        configure_write(WriteKind::mark_time);
        verify_case("first unreferenced observation", ObjectGcPrepareResult::skipped, 1, 0, 1);
        check_record({state, discovery_time}, "first observation");
        check(database.last_write == WriteKind::mark_time, "first observation tried deletion");

        database = {};
        database.stored.state = state;
        database.write_reply.affected_rows = 0;
        verify_case("grace period pending", ObjectGcPrepareResult::skipped, 1, 0, 1);
        check_record({state, previous_time}, "grace period pending");
        check(database.last_write == WriteKind::mark_deleting, "wrong expiry update");

        database = {};
        database.stored.state = state;
        verify_case("grace period expired", ObjectGcPrepareResult::ready_to_delete, 1, 0, 1);
        check_record({"DELETING", previous_time}, "grace period expired");
    }

    // 已经删除中的对象不重新计时，也不需要重复 UPDATE。
    database = {};
    database.stored.state = "DELETING";
    verify_case("resume DELETING", ObjectGcPrepareResult::ready_to_delete, 1, 0, 0);
    check_record({"DELETING", previous_time}, "resume DELETING");
}

void test_write_failures() {
    for (const auto kind : {WriteKind::clear_time, WriteKind::mark_time, WriteKind::mark_deleting}) {
        for (const auto reply : {DbExecuteResult{false, 1213, 0, 0},
                                 DbExecuteResult{true, 0, 2, 0}}) {
            database = {};
            configure_write(kind);
            database.write_reply = reply;
            const auto original = database.stored;
            verify_case("failed or invalid update", ObjectGcPrepareResult::failed, 0, 1, 1);
            check_record(original, "failed or invalid update");
        }
    }

    // 首次登记计时必须实际修改一行，零行不能报告登记成功。
    database = {};
    configure_write(WriteKind::mark_time);
    database.write_reply.affected_rows = 0;
    const auto original = database.stored;
    verify_case("timer update affected zero rows", ObjectGcPrepareResult::failed, 0, 1, 1);
    check_record(original, "timer update affected zero rows");
}

void test_commit_results() {
    for (const auto kind : {WriteKind::clear_time, WriteKind::mark_time, WriteKind::mark_deleting}) {
        for (const bool applied : {false, true}) {
            database = {};
            configure_write(kind);
            const auto original = database.stored;
            database.commit_reply = DbCommitStatus::k_outcome_unknown;
            database.unknown_applies = applied;
            verify_case("unknown commit", ObjectGcPrepareResult::outcome_unknown, 1, 0, 1);
            check_record(applied ? after_write(original, kind) : original, "unknown commit");
        }

        database = {};
        configure_write(kind);
        const auto original = database.stored;
        database.commit_reply = DbCommitStatus::k_not_active;
        verify_case("inactive commit", ObjectGcPrepareResult::failed, 1, 0, 1);
        check_record(original, "inactive commit");
    }

    // 即使当前已是 DELETING，本次提交结果未知也不能返回允许删除。
    database = {};
    database.stored.state = "DELETING";
    database.commit_reply = DbCommitStatus::k_outcome_unknown;
    verify_case("unknown read-only commit", ObjectGcPrepareResult::outcome_unknown, 1, 0, 0);

    // 覆盖未来新增提交状态时的保守分支。
    database = {};
    database.commit_reply = static_cast<DbCommitStatus>(99);
    const auto original = database.stored;
    verify_case("unrecognized commit status", ObjectGcPrepareResult::outcome_unknown, 1, 0, 1);
    check_record(original, "unrecognized commit status");
}

void verify_exception(ThrowPoint point, WriteKind kind) {
    database = {};
    configure_write(kind);
    database.throw_at = point;
    const auto original = database.stored;
    bool caught = false;
    try {
        execute();
    } catch (const DatabaseError&) {
        caught = true;
    }
    check(caught, "database exception was swallowed or changed");
    check(database.begins == 1 && database.commits == 0 && database.rollbacks == 1,
          "exception did not roll back the transaction exactly once");
    check(database.object_queries == 1 &&
              database.reference_queries == (point == ThrowPoint::object_query ? 0 : 1) &&
              database.writes == (point == ThrowPoint::after_write ? 1 : 0),
          "exception caused a retry or extra SQL operation");
    check(database.owner == nullptr, "exception left an active transaction");
    check_record(original, "exception rollback");
}

void test_exceptions() {
    verify_exception(ThrowPoint::object_query, WriteKind::none);
    verify_exception(ThrowPoint::reference_query, WriteKind::none);
    for (const auto kind : {WriteKind::clear_time, WriteKind::mark_time, WriteKind::mark_deleting})
        verify_exception(ThrowPoint::after_write, kind);
}

// 收尾测试的前提是调用者已完成磁盘删除和目录同步。
// 本组只验证数据库收尾，不调用磁盘存储引擎。
void reset_finalization(bool record_exists = true) {
    database = {};
    database.stored.state = "DELETING";
    database.object_exists = record_exists;
}

ObjectGcFinalizeResult execute_finalization(const std::string& hash = object_hash) {
    DbConnection connection;
    return finalize_object_gc_transaction(connection, hash);
}

void verify_finalization(const std::string& name, ObjectGcFinalizeResult expected,
                         int commits, int rollbacks, int writes,
                         int reference_queries = 1, int object_queries = 1) {
    const auto original = database.stored;
    const auto result = execute_finalization();
    check(result == expected, name + ": unexpected finalization result");
    check(database.begins == 1 && database.commits == commits &&
              database.rollbacks == rollbacks,
          name + ": unexpected finalization transaction operations");
    check(database.writes == writes && database.reference_queries == reference_queries &&
              database.object_queries == object_queries,
          name + ": unexpected finalization SQL counts or retry");
    check(database.owner == nullptr, name + ": finalization left an active transaction");
    check(database.pending_exists == database.object_exists,
          name + ": transaction working copy was not released");
    if (writes != 0)
        check(database.last_write == WriteKind::remove_record,
              name + ": finalization modified the object instead of deleting it");
    // 无论是否删除记录，收尾都不能恢复 LIVE 或修改原有计时。
    check_record(original, name);
}

void test_finalize_invalid_arguments() {
    for (const auto& hash : {std::string{}, std::string("../escape"), std::string(63, 'a'),
                             std::string(65, 'a'), std::string(64, 'A'), std::string(64, 'g')}) {
        reset_finalization();
        check(execute_finalization(hash) == ObjectGcFinalizeResult::failed,
              "finalization accepted an invalid hash");
        check(database.begins == 0 && database.object_queries == 0 &&
                  database.reference_queries == 0 && database.writes == 0 &&
                  database.commits == 0 && database.rollbacks == 0,
              "invalid finalization hash accessed the database");
        check(database.object_exists, "invalid hash removed the record");
    }
}

void test_finalize_lookup() {
    reset_finalization();
    database.begin_ok = false;
    verify_finalization("finalization begin failure", ObjectGcFinalizeResult::failed,
                        0, 0, 0, 0, 0);

    reset_finalization();
    database.object_reply = DbQueryResult{false, {}};
    verify_finalization("finalization lookup failure", ObjectGcFinalizeResult::failed,
                        0, 1, 0, 0);
    check(database.object_exists, "failed lookup removed the record");

    reset_finalization();
    verify_finalization("successful finalization", ObjectGcFinalizeResult::completed,
                        1, 0, 1);
    check(!database.object_exists, "successful finalization retained the record");

    // 保留删除后的数据库状态，重置连接与计数，模拟下一次调用。
    const bool record_exists = database.object_exists;
    reset_finalization(record_exists);
    verify_finalization("repeated finalization", ObjectGcFinalizeResult::completed,
                        0, 1, 0, 0);
    check(!database.object_exists, "repeated finalization recreated the record");
}

void test_finalize_malformed_results() {
    const std::vector<DbQueryResult> replies{
        {true, {{}}},
        {true, {{"DELETING"}}},
        {true, {{"DELETING", std::nullopt, "extra"}}},
        {true, {{std::nullopt, std::nullopt}}},
        {true, {{"UNKNOWN", std::nullopt}}},
        {true, {{"DELETING", std::nullopt}, {"DELETING", std::nullopt}}},
    };
    for (const auto& reply : replies) {
        reset_finalization();
        database.object_reply = reply;
        verify_finalization("malformed finalization object", ObjectGcFinalizeResult::failed,
                            0, 1, 0, 0);
        check(database.object_exists, "malformed object result allowed deletion");
    }
}

void test_finalize_states() {
    for (const auto* state : {"LIVE", "UNAVAILABLE"}) {
        reset_finalization();
        database.stored.state = state;
        verify_finalization("non-DELETING object", ObjectGcFinalizeResult::failed,
                            0, 1, 0, 0);
        check(database.object_exists, "non-DELETING object record was removed");
    }

    // 收尾依赖 DELETING 状态，而不要求计时字段非 NULL。
    reset_finalization();
    database.stored.unreferenced_at.reset();
    verify_finalization("DELETING without timestamp", ObjectGcFinalizeResult::completed,
                        1, 0, 1);
    check(!database.object_exists, "DELETING record without timestamp was retained");
}

void test_finalize_references() {
    const std::vector<DbQueryResult> replies{
        {true, {{"42"}}},  // 仍有引用，拒绝收尾。
        {false, {}},      // 查询失败不能当成无引用。
        {true, {{}}},
        {true, {{std::nullopt}}},
        {true, {{"42", "extra"}}},
        {true, {{"42"}, {"43"}}},
    };
    for (const auto& reply : replies) {
        reset_finalization();
        database.reference_reply = reply;
        verify_finalization("referenced or invalid finalization result",
                            ObjectGcFinalizeResult::failed, 0, 1, 0);
        check(database.object_exists, "reference result incorrectly allowed deletion");
    }
}

void test_finalize_delete_failures() {
    for (const auto reply : {DbExecuteResult{false, 1213, 0, 0},
                             DbExecuteResult{true, 0, 0, 0},
                             DbExecuteResult{true, 0, 2, 0}}) {
        reset_finalization();
        database.write_reply = reply;
        verify_finalization("failed or invalid DELETE", ObjectGcFinalizeResult::failed,
                            0, 1, 1);
        check(database.object_exists, "failed DELETE did not preserve the committed record");
    }
}

void test_finalize_commit_results() {
    for (const bool applied : {false, true}) {
        reset_finalization();
        database.commit_reply = DbCommitStatus::k_outcome_unknown;
        database.unknown_applies = applied;
        verify_finalization("unknown finalization commit", ObjectGcFinalizeResult::outcome_unknown,
                            1, 0, 1);
        check(database.object_exists == !applied,
              "unknown commit did not simulate the configured remote outcome");

        // 无论上次是否实际提交，都允许通过新事务重新查询确认。
        // 已删除时直接完成；仍存在时再次 DELETE 并提交。
        const bool record_exists = database.object_exists;
        reset_finalization(record_exists);
        verify_finalization("confirm unknown finalization", ObjectGcFinalizeResult::completed,
                            record_exists ? 1 : 0, record_exists ? 0 : 1,
                            record_exists ? 1 : 0, record_exists ? 1 : 0);
        check(!database.object_exists, "confirmation left the object record behind");
    }

    reset_finalization();
    database.commit_reply = DbCommitStatus::k_not_active;
    verify_finalization("inactive finalization commit", ObjectGcFinalizeResult::failed,
                        1, 0, 1);
    check(database.object_exists, "inactive commit applied DELETE");

    reset_finalization();
    database.commit_reply = static_cast<DbCommitStatus>(99);
    verify_finalization("unrecognized finalization commit", ObjectGcFinalizeResult::outcome_unknown,
                        1, 0, 1);
    check(database.object_exists, "unrecognized commit applied DELETE");
}

void test_finalize_exceptions() {
    for (const auto point : {ThrowPoint::object_query, ThrowPoint::reference_query,
                             ThrowPoint::after_write}) {
        reset_finalization();
        database.throw_at = point;
        bool caught = false;
        try {
            execute_finalization();
        } catch (const DatabaseError&) {
            caught = true;
        }
        check(caught, "finalization swallowed or changed the database exception");
        check(database.begins == 1 && database.commits == 0 && database.rollbacks == 1,
              "finalization exception did not roll back exactly once");
        check(database.object_queries == 1 &&
                  database.reference_queries == (point == ThrowPoint::object_query ? 0 : 1) &&
                  database.writes == (point == ThrowPoint::after_write ? 1 : 0),
              "finalization exception caused a retry or extra SQL operation");
        // after_write 在事务内先删除记录再抛异常，必须恢复已提交的存在状态。
        check(database.object_exists && database.pending_exists && database.owner == nullptr,
              "finalization exception did not restore the record or close the transaction");
        check_record({"DELETING", previous_time}, "finalization exception");
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 2, "expected one scenario argument");
        const std::string scenario(argv[1]);
        if (scenario == "invalid_arguments")
            test_invalid_arguments();
        else if (scenario == "begin_and_lookup")
            test_begin_and_lookup();
        else if (scenario == "malformed_results")
            test_malformed_results();
        else if (scenario == "references")
            test_references();
        else if (scenario == "unreferenced")
            test_unreferenced();
        else if (scenario == "write_failures")
            test_write_failures();
        else if (scenario == "commit_results")
            test_commit_results();
        else if (scenario == "exceptions")
            test_exceptions();
        else if (scenario == "finalize_invalid_arguments")
            test_finalize_invalid_arguments();
        else if (scenario == "finalize_lookup")
            test_finalize_lookup();
        else if (scenario == "finalize_malformed_results")
            test_finalize_malformed_results();
        else if (scenario == "finalize_states")
            test_finalize_states();
        else if (scenario == "finalize_references")
            test_finalize_references();
        else if (scenario == "finalize_delete_failures")
            test_finalize_delete_failures();
        else if (scenario == "finalize_commit_results")
            test_finalize_commit_results();
        else if (scenario == "finalize_exceptions")
            test_finalize_exceptions();
        else
            throw std::invalid_argument("unknown scenario: " + scenario);
        std::cout << "PASS object GC transaction " << scenario << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
