#include "auth/user_repository.h"
#include "common/base/mysql_connection.h"
#include "logger/logger.h"

#include <gtest/gtest.h>
#include <cstdlib>
#include <memory>
#include <string>

namespace {
using namespace TLSS::AUTH;
using TLSS::BASE::DbConnection;

TEST(AuthRepositoryTest, HandlesUnavailableConnection) {
    MySqlUserRepository users([] { return std::shared_ptr<DbConnection>{}; });
    EXPECT_EQ(users.create_user_with_root("alice", {"salt", "hash"}).status,
              UserCreateStatus::k_backend_unavailable);
}

class MySqlSignupTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* host = std::getenv("TLSS_TEST_MYSQL_HOST");
        const auto* user = std::getenv("TLSS_TEST_MYSQL_USER");
        const auto* password = std::getenv("TLSS_TEST_MYSQL_PASSWORD");
        const auto* database = std::getenv("TLSS_TEST_MYSQL_DATABASE");
        if (!host || !user || !password || !database) GTEST_SKIP() << "MySQL test environment unset";
        const auto* configured_port = std::getenv("TLSS_TEST_MYSQL_PORT");
        const int port = configured_port ? std::stoi(configured_port) : 3306;
        ASSERT_GT(port, 0);
        ASSERT_LE(port, 65535);
        TLSSLOG::Logger::initConsole("mysql", spdlog::level::off);
        db = std::make_shared<DbConnection>();
        ASSERT_TRUE(db->connect(host, static_cast<unsigned short>(port), user, password, database));
        // Session-local tables shadow production names; no persistent rows are changed. / 会话内的临时表遮蔽生产表名，不修改持久化数据行。
        ASSERT_TRUE(db->update(
            "CREATE TEMPORARY TABLE users ("
            "id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY, username VARCHAR(64) UNIQUE,"
            "salt VARCHAR(64), password_hash VARCHAR(255), is_deleted TINYINT DEFAULT 0"
            ") ENGINE=InnoDB COLLATE=utf8mb4_0900_ai_ci"));
        ASSERT_TRUE(db->update(
            "CREATE TEMPORARY TABLE virtual_files ("
            "id BIGINT AUTO_INCREMENT PRIMARY KEY, username VARCHAR(64), file_name VARCHAR(255),"
            "parent_id BIGINT, path VARCHAR(1024), file_type ENUM('file','directory'),"
            "object_hash CHAR(64) DEFAULT NULL, UNIQUE(username,parent_id,file_name),"
            "CHECK(parent_id=-1 AND file_name='/' AND path='/' AND file_type='directory')"
            ") ENGINE=InnoDB COLLATE=utf8mb4_0900_ai_ci"));
        users = std::make_unique<MySqlUserRepository>([this] { return db; });
    }
    std::shared_ptr<DbConnection> db;
    std::unique_ptr<MySqlUserRepository> users;
};

TEST_F(MySqlSignupTest, CommitsUserAndCorrectRootAndReturnsUserId) {
    // Root ID must not overwrite the returned user ID. / 根目录 ID 不得覆盖返回的用户 ID。
    ASSERT_TRUE(db->update("ALTER TABLE virtual_files AUTO_INCREMENT=100"));
    const auto result = users->create_user_with_root("alice", {"salt", "hash"});
    ASSERT_EQ(result.status, UserCreateStatus::k_created);
    EXPECT_EQ(result.user_id, 1U);
    ASSERT_TRUE(db->update("ROLLBACK AND NO CHAIN NO RELEASE"));
    const auto lookup = users->find_active_user("ALICE");
    ASSERT_EQ(lookup.status, UserLookupStatus::k_found);
    EXPECT_EQ(lookup.user.user_id, result.user_id);
    EXPECT_EQ(lookup.user.username, "alice");
    const auto roots = db->query_prepared(
        "SELECT file_name, parent_id, path, file_type, object_hash FROM virtual_files");
    ASSERT_TRUE(roots.success);
    ASSERT_EQ(roots.rows.size(), 1U);
    EXPECT_EQ(roots.rows[0], (TLSS::BASE::DbRow{"/", "-1", "/", "directory", std::nullopt}));
    EXPECT_TRUE(db->is_reusable());
}

TEST_F(MySqlSignupTest, DuplicateAndSoftDeletedNamesRemainReserved) {
    ASSERT_EQ(users->create_user_with_root("alice", {"salt", "hash"}).status,
              UserCreateStatus::k_created);
    EXPECT_EQ(users->create_user_with_root("ALICE", {"other", "other"}).status,
              UserCreateStatus::k_username_taken);
    ASSERT_TRUE(db->update("UPDATE users SET is_deleted=1"));
    EXPECT_EQ(users->create_user_with_root("alice", {"other", "other"}).status,
              UserCreateStatus::k_username_taken);
    EXPECT_EQ(users->find_active_user("alice").status, UserLookupStatus::k_not_found);
    EXPECT_TRUE(db->is_reusable());
}

TEST_F(MySqlSignupTest, RootConflictRollsBackUserAndReportsBackendFailure) {
    ASSERT_TRUE(db->update(
        "INSERT INTO virtual_files(username,file_name,parent_id,path,file_type) "
        "VALUES('alice','/',-1,'/','directory')"));
    const auto result = users->create_user_with_root("alice", {"salt", "hash"});
    EXPECT_EQ(result.status, UserCreateStatus::k_backend_unavailable);
    EXPECT_EQ(result.user_id, 0U);
    EXPECT_EQ(users->find_active_user("alice").status, UserLookupStatus::k_not_found);
    EXPECT_TRUE(db->is_reusable());
}
}  // namespace / 命名空间结束
