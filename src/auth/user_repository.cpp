#include "auth/user_repository.h"

#include "common/base/mysql_connection.h"
#include "common/base/mysql_connection_pool.h"

#include <charconv>
#include <cstdint>
#include <system_error>
#include <utility>
#include <mysql/mysqld_error.h>

namespace TLSS::AUTH {

MySqlUserRepository::MySqlUserRepository(BASE::DbConnectionPool& pool)
    : MySqlUserRepository([&pool] {
        return std::shared_ptr<BASE::DbConnection>(pool.get_connection());
      }) {}

MySqlUserRepository::MySqlUserRepository(ConnectionProvider provider)
    : connection_provider_(std::move(provider)) {}

UserCreateResult MySqlUserRepository::create_user_with_root(
    const std::string& username, const PasswordCredentials& credentials) {
    auto connection = connection_provider_();
    if (!connection) return {};

    BASE::DbTransaction transaction(*connection);
    if (!transaction.active()) return {};

    // 直接 INSERT，由唯一索引裁决并发注册；软删除的用户名仍被保留。
    const auto user = connection->execute_prepared(
        "INSERT INTO users(username, salt, password_hash) VALUES (?, ?, ?)",
        {username, credentials.salt, credentials.password_hash});
    if (!user.success) {
        if (user.error_code == ER_DUP_ENTRY) {
            if (!transaction.rollback()) return {};
            return {UserCreateStatus::k_username_taken, 0};
        }
        return {};
    }
    if (user.insert_id == 0 || user.affected_rows != 1) return {};

    const auto root = connection->execute_prepared(
        "INSERT INTO virtual_files(username, file_name, parent_id, path, file_type, object_hash) "
        "VALUES (?, '/', -1, '/', 'directory', NULL)", {username});
    if (!root.success || root.affected_rows != 1) return {};

    // 失败时可能已经提交，不能自动重试或谎报注册失败。
    switch (transaction.commit()) {
        case BASE::DbCommitStatus::k_committed:
            return {UserCreateStatus::k_created, user.insert_id};
        case BASE::DbCommitStatus::k_outcome_unknown:
            return {UserCreateStatus::k_outcome_unknown, 0};
        case BASE::DbCommitStatus::k_not_active:
            return {};
    }
    return {};
}

UserLookupResult MySqlUserRepository::find_active_user(const std::string& username) {
  auto connection = connection_provider_();

  if (!connection) {
    return {};
  }

  const auto result = connection->query_prepared(
      "SELECT id, username, salt, password_hash "
      "FROM users "
      "WHERE username = ? AND is_deleted = 0 "
      "LIMIT 1",
      {username});

  if (!result.success) {
    return {};
  }

  if (result.rows.empty()) {
    return {UserLookupStatus::k_not_found, {}};
  }

  const auto& row = result.rows.front();

  if (row.size() != 4 || !row[0] || !row[1] || !row[2] || !row[3] || row[1]->empty() ||
      row[2]->empty() || row[3]->empty()) {
    return {};
  }

  std::uint64_t user_id = 0;

  const auto& id_text = *row[0];
  const auto parsed = std::from_chars(id_text.data(), id_text.data() + id_text.size(), user_id);

  if (parsed.ec != std::errc{} || parsed.ptr != id_text.data() + id_text.size() || user_id == 0) {
    return {};
  }

  return {
      UserLookupStatus::k_found,
      {
          user_id,
          *row[1],
          {*row[2], *row[3]},
      },
  };
}

}  // namespace TLSS::AUTH
