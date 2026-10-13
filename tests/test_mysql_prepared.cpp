#include "common/base/mysql_connection.h"
#include "ftp/service_engine.h"
#include "logger/logger.h"

#include <gtest/gtest.h>

#include <charconv>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <system_error>

namespace {

class DbConnectionPreparedTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* host = std::getenv("TLSS_TEST_MYSQL_HOST");
        const char* user = std::getenv("TLSS_TEST_MYSQL_USER");
        const char* password = std::getenv("TLSS_TEST_MYSQL_PASSWORD");
        const char* database = std::getenv("TLSS_TEST_MYSQL_DATABASE");

        if (!host || !user || !password || !database) {
            GTEST_SKIP() << "Set TLSS_TEST_MYSQL_HOST/USER/PASSWORD/DATABASE";
        }

        unsigned int port = 3306;
        if (const char* configured = std::getenv("TLSS_TEST_MYSQL_PORT")) {
            const std::string text(configured);
            const char* end = text.data() + text.size();
            const auto parsed = std::from_chars(text.data(), end, port);
            ASSERT_TRUE(parsed.ec == std::errc{} && parsed.ptr == end &&
                        port > 0 && port <= 65535);
        }

        TLSSLOG::Logger::initConsole("mysql", spdlog::level::off);
        db_ = std::make_unique<TLSS::BASE::DbConnection>();
        ASSERT_TRUE(db_->connect(host, static_cast<unsigned short>(port),
                                 user, password, database));

        // 临时 users 表仅当前连接可见，会遮蔽同名持久表。
        // 所有测试数据均写入临时表，关闭连接后自动清理。
        ASSERT_TRUE(db_->update_prepared(
            "CREATE TEMPORARY TABLE users ("
            "id BIGINT UNSIGNED NOT NULL PRIMARY KEY, "
            "username VARCHAR(64) NOT NULL UNIQUE, "
            "salt VARCHAR(64), "
            "password_hash VARCHAR(255), "
            "is_deleted TINYINT NOT NULL DEFAULT 0"
            ") CHARACTER SET utf8mb4",
            {}));

        ASSERT_TRUE(insert_user(1, "alice", 0));
        ASSERT_TRUE(insert_user(2, "deleted_user", 1));
    }

    void TearDown() override {
        // 关闭连接时 MySQL 自动删除临时表。
        db_.reset();
    }

    bool insert_user(std::uint64_t id, const std::string& username,
                     std::int64_t deleted) {
        return db_->update_prepared(
            "INSERT INTO users "
            "(id, username, salt, password_hash, is_deleted) "
            "VALUES (?, ?, ?, ?, ?)",
            {id, username, std::string("salt"),
             std::string("hash"), deleted});
    }

    TLSS::BASE::DbQueryResult lookup(const std::string& username) {
        return db_->query_prepared(
            "SELECT id, salt, password_hash "
            "FROM users "
            "WHERE username = ? AND is_deleted = 0 LIMIT 1",
            {username});
    }

    TLSS::FileStorage::StorageContext<
        std::shared_ptr<TLSS::FileStorage::ConnectionSession>>
    lookup_production(const std::string& username) {
        auto session =
            std::make_shared<TLSS::FileStorage::ConnectionSession>();
        session->name = username;

        // 把真实连接移交生产查询；查询完成后句柄关闭连接和临时表。
        TLSS::FileStorage::StorageEngine engine([this] {
            return TLSS::BASE::DbConnectionHandle(
                db_.release(), TLSS::BASE::DbConnectionDeleter{});
        });
        return engine.find_user_by_name(session);
    }

    std::unique_ptr<TLSS::BASE::DbConnection> db_;
};

TEST_F(DbConnectionPreparedTest, FindsNormalUser) {
    const auto result = lookup("alice");
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1u);
    ASSERT_EQ(result.rows[0].size(), 3u);
    ASSERT_TRUE(result.rows[0][0].has_value());
    EXPECT_EQ(*result.rows[0][0], "1");
}

TEST_F(DbConnectionPreparedTest, InjectionCannotChangeLookupConditions) {
    for (const std::string& input :
         {std::string("x' OR 1=1 -- "),
          std::string("deleted_user' -- "),
          std::string("x' UNION SELECT 1, 'salt', 'hash' -- "),
          std::string("deleted_user")}) {
        SCOPED_TRACE(input);
        const auto result = lookup(input);
        ASSERT_TRUE(result.success);
        EXPECT_TRUE(result.rows.empty());
    }
}

TEST_F(DbConnectionPreparedTest, SqlLookingUsernameIsLiteralData) {
    const std::string username = "x' OR 1=1 -- ";
    ASSERT_TRUE(insert_user(3, username, 0));

    const auto result = lookup(username);
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1u);
    ASSERT_TRUE(result.rows[0][0].has_value());
    EXPECT_EQ(*result.rows[0][0], "3");
}

TEST_F(DbConnectionPreparedTest, PreservesNullEmptyLongAndEmbeddedNullStrings) {
    const std::string long_text(8192, 'x');
    const std::string binary_text("ab\0cd", 5);

    const auto result = db_->query_prepared(
        "SELECT ?, ?, ?, ?",
        {nullptr, std::string{}, long_text, binary_text});

    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1u);
    ASSERT_EQ(result.rows[0].size(), 4u);

    EXPECT_FALSE(result.rows[0][0].has_value());
    ASSERT_TRUE(result.rows[0][1].has_value());
    EXPECT_TRUE(result.rows[0][1]->empty());
    ASSERT_TRUE(result.rows[0][2].has_value());
    EXPECT_EQ(*result.rows[0][2], long_text);
    ASSERT_TRUE(result.rows[0][3].has_value());
    EXPECT_EQ(*result.rows[0][3], binary_text);
}

TEST_F(DbConnectionPreparedTest, PreservesNumericParameterTypes) {
    const auto result = db_->query_prepared(
        "SELECT ?, ?, ?",
        {std::numeric_limits<std::int64_t>::min(),
         std::numeric_limits<std::uint64_t>::max(), 1.5});

    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1u);
    ASSERT_EQ(result.rows[0].size(), 3u);
    ASSERT_TRUE(result.rows[0][0].has_value());
    ASSERT_TRUE(result.rows[0][1].has_value());
    ASSERT_TRUE(result.rows[0][2].has_value());
    EXPECT_EQ(*result.rows[0][0], "-9223372036854775808");
    EXPECT_EQ(*result.rows[0][1], "18446744073709551615");
    EXPECT_EQ(*result.rows[0][2], "1.5");
}

TEST_F(DbConnectionPreparedTest, ReusesResultBuffersAcrossLongNullAndShortRows) {
    const std::string long_text(8192, 'x');
    const auto result = db_->query_prepared(
        "SELECT n, v FROM ("
        "SELECT 1 AS n, ? AS v UNION ALL "
        "SELECT 2, ? UNION ALL SELECT 3, ?"
        ") AS sample ORDER BY n",
        {long_text, nullptr, std::string("short")});
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 3u);
    for (const auto& row : result.rows) {
        ASSERT_EQ(row.size(), 2u);
    }
    ASSERT_TRUE(result.rows[0][1].has_value());
    EXPECT_EQ(*result.rows[0][1], long_text);
    EXPECT_FALSE(result.rows[1][1].has_value());
    ASSERT_TRUE(result.rows[2][1].has_value());
    EXPECT_EQ(*result.rows[2][1], "short");
}

TEST_F(DbConnectionPreparedTest, ConnectionRemainsUsableAfterStatementErrors) {
    EXPECT_FALSE(db_->query_prepared("SELECT ?", {}).success);
    EXPECT_FALSE(db_->query_prepared("SELECT ?", {nullptr, nullptr}).success);
    EXPECT_FALSE(db_->query_prepared("SELECT FROM", {}).success);
    EXPECT_FALSE(db_->update_prepared("SELECT 1", {}));

    const auto result = lookup("alice");
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1u);
}

TEST_F(DbConnectionPreparedTest, UpdateBindsSqlLookingValue) {
    const std::string salt = "x', is_deleted = 1 -- ";

    ASSERT_TRUE(db_->update_prepared(
        "UPDATE users "
        "SET salt = ? WHERE username = ?",
        {salt, std::string("alice")}));

    const auto result = lookup("alice");
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1u);
    ASSERT_TRUE(result.rows[0][1].has_value());
    EXPECT_EQ(*result.rows[0][1], salt);
}

TEST_F(DbConnectionPreparedTest, ProductionLookupPopulatesSession) {
    const auto result = lookup_production("alice");
    ASSERT_TRUE(result.success);
    ASSERT_NE(result.result, nullptr);
    EXPECT_EQ(result.result->user_id, 1u);
    EXPECT_EQ(result.result->salt, "salt");
    EXPECT_EQ(result.result->pass, "hash");
    EXPECT_EQ(result.result->login_state,
              TLSS::FileStorage::LoginState::k_await_password);
}

TEST_F(DbConnectionPreparedTest, ProductionLookupRejectsInjection) {
    const auto result = lookup_production("x' OR 1=1 -- ");
    EXPECT_FALSE(result.success);
    ASSERT_NE(result.result, nullptr);
    EXPECT_EQ(result.result->user_id, 0u);
    EXPECT_EQ(result.result->login_state,
              TLSS::FileStorage::LoginState::k_await_username);
}

TEST_F(DbConnectionPreparedTest, ProductionLookupRejectsDeletedUserInjection) {
    EXPECT_FALSE(lookup_production("deleted_user' -- ").success);
}

TEST_F(DbConnectionPreparedTest, ProductionLookupRejectsDeletedUser) {
    EXPECT_FALSE(lookup_production("deleted_user").success);
}

TEST_F(DbConnectionPreparedTest, ProductionLookupFindsLiteralSqlLookingName) {
    const std::string username = "x' OR 1=1 -- ";
    ASSERT_TRUE(insert_user(3, username, 0));
    const auto result = lookup_production(username);
    ASSERT_TRUE(result.success);
    ASSERT_NE(result.result, nullptr);
    EXPECT_EQ(result.result->user_id, 3u);
}

TEST_F(DbConnectionPreparedTest, ProductionLookupRejectsNullCredentials) {
    ASSERT_TRUE(db_->update_prepared(
        "UPDATE users SET salt = ? WHERE username = ?",
        {nullptr, std::string("alice")}));
    EXPECT_FALSE(lookup_production("alice").success);
}

TEST_F(DbConnectionPreparedTest, ProductionLookupRejectsZeroUserId) {
    ASSERT_TRUE(insert_user(0, "zero_id", 0));
    EXPECT_FALSE(lookup_production("zero_id").success);
}

TEST(StorageEngineInputTest, RejectsMissingProviderAndEmptyOrNullSession) {
    TLSS::FileStorage::StorageEngine engine(
        TLSS::FileStorage::StorageEngine::ConnectionProvider{});
    std::shared_ptr<TLSS::FileStorage::ConnectionSession> session;
    EXPECT_FALSE(engine.find_user_by_name(session).success);

    session = std::make_shared<TLSS::FileStorage::ConnectionSession>();
    EXPECT_FALSE(engine.find_user_by_name(session).success);
    session->name = "alice";
    EXPECT_FALSE(engine.find_user_by_name(session).success);
}

TEST(StorageEngineInputTest, RejectsUnavailableConnection) {
    TLSS::FileStorage::StorageEngine engine([] {
        return TLSS::BASE::DbConnectionHandle{};
    });
    auto session = std::make_shared<TLSS::FileStorage::ConnectionSession>();
    session->name = "alice";
    EXPECT_FALSE(engine.find_user_by_name(session).success);
    EXPECT_EQ(session->login_state,
              TLSS::FileStorage::LoginState::k_await_username);
}

}  // namespace

