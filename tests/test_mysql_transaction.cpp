#include "common/base/mysql_connection.h"
#include "logger/logger.h"

#include <mysql/errmsg.h>
#include <mysql/mysqld_error.h>
#include <gtest/gtest.h>

#include <charconv>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

using TLSS::BASE::DbCommitStatus;
using TLSS::BASE::DbConnection;
using TLSS::BASE::DbTransaction;

TEST(DbTransactionTest, RejectsUnconnectedHandle) {
    DbConnection connection;
    EXPECT_FALSE(connection.is_reusable());
    EXPECT_FALSE(connection.ping());

    DbTransaction transaction(connection);
    EXPECT_FALSE(transaction.active());
    EXPECT_EQ(transaction.commit(), DbCommitStatus::k_not_active);
    EXPECT_TRUE(transaction.rollback());

    const auto result = connection.execute_prepared("SELECT 1");
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_code, static_cast<unsigned int>(CR_SERVER_GONE_ERROR));
    EXPECT_EQ(result.insert_id, 0U);
}

class MySqlTransactionTest : public ::testing::Test {
protected:
    void SetUp() override {
        host_ = std::getenv("TLSS_TEST_MYSQL_HOST");
        user_ = std::getenv("TLSS_TEST_MYSQL_USER");
        password_ = std::getenv("TLSS_TEST_MYSQL_PASSWORD");
        database_ = std::getenv("TLSS_TEST_MYSQL_DATABASE");
        if (!host_ || !user_ || !password_ || !database_) {
            GTEST_SKIP() << "Set TLSS_TEST_MYSQL_HOST/USER/PASSWORD/DATABASE";
        }

        if (const char* configured = std::getenv("TLSS_TEST_MYSQL_PORT")) {
            const std::string text(configured);
            const auto parsed = std::from_chars(
                text.data(), text.data() + text.size(), port_);
            ASSERT_TRUE(parsed.ec == std::errc{} &&
                        parsed.ptr == text.data() + text.size() &&
                        port_ > 0 && port_ <= 65535);
        }

        TLSSLOG::Logger::initConsole("mysql", spdlog::level::off);
        db_ = std::make_unique<DbConnection>();
        ASSERT_TRUE(connect(*db_));

        // 临时 InnoDB 表只在本次连接中可见，断开时自动删除。 / The temporary InnoDB table is visible only to this connection and is automatically dropped on disconnect.
        ASSERT_TRUE(db_->update(
            "CREATE TEMPORARY TABLE tlss_signup_users_test ("
            "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
            "username VARCHAR(64) NOT NULL UNIQUE"
            ") ENGINE=InnoDB"));
        ASSERT_TRUE(db_->update(
            "CREATE TEMPORARY TABLE tlss_signup_roots_test ("
            "username VARCHAR(64) NOT NULL PRIMARY KEY,"
            "path VARCHAR(64) NOT NULL"
            ") ENGINE=InnoDB"));
    }

    void TearDown() override {
        db_.reset();
    }

    bool connect(DbConnection& connection) const {
        return connection.connect(host_, static_cast<unsigned short>(port_),
                                  user_, password_, database_);
    }

    void expect_count(const std::string& table, const std::string& expected) {
        // table 仅来自下面测试中的固定表名。 / table only comes from the fixed table names in the tests below.
        const auto result = db_->query_prepared("SELECT COUNT(*) FROM " + table);
        ASSERT_TRUE(result.success);
        ASSERT_EQ(result.rows.size(), 1U);
        ASSERT_EQ(result.rows[0].size(), 1U);
        ASSERT_TRUE(result.rows[0][0].has_value());
        EXPECT_EQ(*result.rows[0][0], expected);
    }

    void disconnect_test_session() {
        const auto result = db_->query_prepared("SELECT CONNECTION_ID()");
        ASSERT_TRUE(result.success);
        ASSERT_EQ(result.rows.size(), 1U);
        ASSERT_EQ(result.rows[0].size(), 1U);
        ASSERT_TRUE(result.rows[0][0].has_value());

        std::uint64_t id = 0;
        const auto& text = *result.rows[0][0];
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
        ASSERT_TRUE(parsed.ec == std::errc{} &&
                    parsed.ptr == text.data() + text.size() && id > 0);

        // 只终止本测试自己创建的连接，不影响其他会话。 / Terminate only connections created by this test without affecting other sessions.
        DbConnection control;
        ASSERT_TRUE(connect(control));
        ASSERT_TRUE(control.update("KILL CONNECTION " + std::to_string(id)));
    }

    const char* host_ = nullptr;
    const char* user_ = nullptr;
    const char* password_ = nullptr;
    const char* database_ = nullptr;
    unsigned int port_ = 3306;
    std::unique_ptr<DbConnection> db_;
};

TEST_F(MySqlTransactionTest, ReportsDuplicateKeyAndInsertId) {
    const auto first = db_->execute_prepared(
        "INSERT INTO tlss_signup_users_test(username) VALUES (?)",
        {std::string("alice")});
    ASSERT_TRUE(first.success);
    EXPECT_EQ(first.error_code, 0U);
    EXPECT_EQ(first.affected_rows, 1U);
    EXPECT_GT(first.insert_id, 0U);

    const auto duplicate = db_->execute_prepared(
        "INSERT INTO tlss_signup_users_test(username) VALUES (?)",
        {std::string("alice")});
    EXPECT_FALSE(duplicate.success);
    EXPECT_EQ(duplicate.error_code, static_cast<unsigned int>(ER_DUP_ENTRY));
    EXPECT_EQ(duplicate.insert_id, 0U);

    // 兼容的 bool 接口仍能在语句失败后继续使用此连接。 / The compatible bool interface can still use this connection after a statement fails.
    EXPECT_TRUE(db_->update_prepared(
        "INSERT INTO tlss_signup_users_test(username) VALUES (?)",
        {std::string("bob")}));
}

TEST_F(MySqlTransactionTest, CommitsUserAndRootTogether) {
    // 验证事务结束不会受到服务器默认 CHAIN 设置影响。 / Verify that ending a transaction is unaffected by the server's default CHAIN setting.
    ASSERT_TRUE(db_->update("SET SESSION completion_type = 1"));
    {
        DbTransaction transaction(*db_);
        ASSERT_TRUE(transaction.active());
        EXPECT_FALSE(db_->is_reusable());
        ASSERT_TRUE(db_->execute_prepared(
            "INSERT INTO tlss_signup_users_test(username) VALUES (?)",
            {std::string("alice")}).success);
        ASSERT_TRUE(db_->execute_prepared(
            "INSERT INTO tlss_signup_roots_test(username, path) VALUES (?, ?)",
            {std::string("alice"), std::string("/")}).success);
        ASSERT_EQ(transaction.commit(), DbCommitStatus::k_committed);
        EXPECT_FALSE(transaction.active());
        EXPECT_EQ(transaction.commit(), DbCommitStatus::k_not_active);
    }

    // 若 commit() 只更改本地标志而未实际提交，这次 ROLLBACK 会暴露问题。 / If commit() only changes local flags without committing, this ROLLBACK exposes the problem.
    ASSERT_TRUE(db_->update("ROLLBACK AND NO CHAIN NO RELEASE"));
    expect_count("tlss_signup_users_test", "1");
    expect_count("tlss_signup_roots_test", "1");
    EXPECT_TRUE(db_->is_reusable());
}

TEST_F(MySqlTransactionTest, RollsBackBothRowsOnScopeExit) {
    {
        DbTransaction transaction(*db_);
        ASSERT_TRUE(transaction.active());
        ASSERT_TRUE(db_->execute_prepared(
            "INSERT INTO tlss_signup_users_test(username) VALUES (?)",
            {std::string("alice")}).success);
        ASSERT_TRUE(db_->execute_prepared(
            "INSERT INTO tlss_signup_roots_test(username, path) VALUES (?, ?)",
            {std::string("alice"), std::string("/")}).success);
    }
    expect_count("tlss_signup_users_test", "0");
    expect_count("tlss_signup_roots_test", "0");
    EXPECT_TRUE(db_->is_reusable());
}

TEST_F(MySqlTransactionTest, RollsBackUserWhenRootInsertFails) {
    ASSERT_TRUE(db_->execute_prepared(
        "INSERT INTO tlss_signup_roots_test(username, path) VALUES (?, ?)",
        {std::string("alice"), std::string("/")}).success);
    {
        DbTransaction transaction(*db_);
        ASSERT_TRUE(transaction.active());
        ASSERT_TRUE(db_->execute_prepared(
            "INSERT INTO tlss_signup_users_test(username) VALUES (?)",
            {std::string("alice")}).success);
        const auto root = db_->execute_prepared(
            "INSERT INTO tlss_signup_roots_test(username, path) VALUES (?, ?)",
            {std::string("alice"), std::string("/")});
        ASSERT_FALSE(root.success);
        EXPECT_EQ(root.error_code, static_cast<unsigned int>(ER_DUP_ENTRY));
    }
    expect_count("tlss_signup_users_test", "0");
    expect_count("tlss_signup_roots_test", "1");
    EXPECT_TRUE(db_->is_reusable());
}

TEST_F(MySqlTransactionTest, RejectsNestedTransaction) {
    {
        DbTransaction outer(*db_);
        ASSERT_TRUE(outer.active());
        ASSERT_TRUE(db_->execute_prepared(
            "INSERT INTO tlss_signup_users_test(username) VALUES (?)",
            {std::string("alice")}).success);
        {
            DbTransaction inner(*db_);
            EXPECT_FALSE(inner.active());
            EXPECT_EQ(inner.commit(), DbCommitStatus::k_not_active);
        }
        EXPECT_TRUE(outer.active());
    }
    expect_count("tlss_signup_users_test", "0");
    EXPECT_TRUE(db_->is_reusable());
}

TEST_F(MySqlTransactionTest, RollsBackWhenExceptionLeavesScope) {
    EXPECT_THROW({
        DbTransaction transaction(*db_);
        ASSERT_TRUE(transaction.active());
        ASSERT_TRUE(db_->execute_prepared(
            "INSERT INTO tlss_signup_users_test(username) VALUES (?)",
            {std::string("alice")}).success);
        throw std::runtime_error("Simulated application failure");
    }, std::runtime_error);
    expect_count("tlss_signup_users_test", "0");
    EXPECT_TRUE(db_->is_reusable());
}

TEST_F(MySqlTransactionTest, FailedCommitInvalidatesConnection) {
    DbTransaction transaction(*db_);
    ASSERT_TRUE(transaction.active());
    ASSERT_NO_FATAL_FAILURE(disconnect_test_session());
    EXPECT_EQ(transaction.commit(), DbCommitStatus::k_outcome_unknown);
    EXPECT_FALSE(transaction.active());
    EXPECT_FALSE(db_->is_reusable());
    EXPECT_FALSE(db_->ping());
}

TEST_F(MySqlTransactionTest, FailedRollbackInvalidatesConnection) {
    DbTransaction transaction(*db_);
    ASSERT_TRUE(transaction.active());
    ASSERT_NO_FATAL_FAILURE(disconnect_test_session());
    EXPECT_FALSE(transaction.rollback());
    EXPECT_FALSE(transaction.active());
    EXPECT_FALSE(db_->is_reusable());
    EXPECT_FALSE(db_->ping());
}

}  // namespace / 命名空间结束
