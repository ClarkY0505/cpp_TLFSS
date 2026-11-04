#include <iostream>
#include <stdexcept>

#include "auth/user_repository.h"
#include "common/base/mysql_connection.h"
#include "common/base/mysql_connection_pool.h"

namespace {
struct Database {
    TLSS::BASE::DbConnection* connection = nullptr;
    bool active = false;
    bool root_failure = false;
    bool unknown_commit = false;
    bool duplicate = false;
    int borrows = 0;
    int statements = 0;
    int commits = 0;
    int rollbacks = 0;
} database;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}  // namespace

// Substitute only the DB boundary; execute the real repository and root creator.
namespace TLSS::BASE {
DbConnection::DbConnection() : _conn(nullptr) {}
DbConnection::~DbConnection() = default;
DbConnectionHandle DbConnectionPool::get_connection() { return {}; }
void DbConnectionDeleter::operator()(DbConnection* connection) const noexcept { delete connection; }
DbQueryResult DbConnection::query_prepared(const std::string&, const std::vector<DbValue>&) {
    return {};
}
DbExecuteResult DbConnection::execute_prepared(const std::string& sql,
                                               const std::vector<DbValue>& params) {
    check(database.active && database.connection == this,
          "signup statements did not share the active transaction connection");
    ++database.statements;
    check(params.at(0) == DbValue("alice"), "signup identity differs between statements");
    if (sql.find("INSERT INTO users") == 0) {
        return database.duplicate ? DbExecuteResult{false, 1062, 0, 0}
                                  : DbExecuteResult{true, 0, 1, 7};
    }
    check(sql.find("INSERT INTO virtual_files") == 0 &&
              sql.find("'/', -1, '/', 'directory', NULL") != std::string::npos,
          "root metadata differs");
    return {!database.root_failure, 0, database.root_failure ? 0U : 1U, 100};
}
DbTransaction::DbTransaction(DbConnection& connection) noexcept
    : connection_(connection), active_(true) {
    database.connection = &connection;
    database.active = true;
}
DbTransaction::~DbTransaction() noexcept {
    if (active_) rollback();
}
DbCommitStatus DbTransaction::commit() noexcept {
    ++database.commits;
    database.active = false;
    active_ = false;
    return database.unknown_commit ? DbCommitStatus::k_outcome_unknown
                                   : DbCommitStatus::k_committed;
}
bool DbTransaction::rollback() noexcept {
    ++database.rollbacks;
    database.active = false;
    active_ = false;
    return true;
}
}  // namespace TLSS::BASE

int main() {
    using namespace TLSS::AUTH;
    try {
        auto connection = std::make_shared<TLSS::BASE::DbConnection>();
        MySqlUserRepository users([&] {
            ++database.borrows;
            return connection;
        });
        const auto created = users.create_user_with_root("alice", {"salt", "hash"});
        check(created.status == UserCreateStatus::k_created && created.user_id == 7,
              "root insert replaced the created user ID");
        check(database.borrows == 1 && database.statements == 2 && database.commits == 1,
              "signup did not use one connection and one commit");

        database = {};
        database.root_failure = true;
        check(users.create_user_with_root("alice", {"salt", "hash"}).status ==
                  UserCreateStatus::k_backend_unavailable,
              "root failure accepted");
        check(database.commits == 0 && database.rollbacks == 1, "root failure did not roll back");

        database = {};
        database.unknown_commit = true;
        check(users.create_user_with_root("alice", {"salt", "hash"}).status ==
                  UserCreateStatus::k_outcome_unknown,
              "unknown commit was reported as definite success or failure");
        check(database.statements == 2 && database.commits == 1 && database.borrows == 1,
              "uncertain registration was retried");

        database = {};
        database.duplicate = true;
        check(users.create_user_with_root("alice", {"salt", "hash"}).status ==
                  UserCreateStatus::k_username_taken,
              "duplicate username mapping changed");
        check(database.statements == 1 && database.commits == 0 && database.rollbacks == 1,
              "duplicate registration created a root or committed");
        std::cout << "PASS signup transaction boundary\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
