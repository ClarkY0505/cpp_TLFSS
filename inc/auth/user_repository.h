#ifndef __INC_AUTH_USER_REPOSITORY_H__
#define __INC_AUTH_USER_REPOSITORY_H__

#include "auth/password_hasher.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace TLSS::BASE {
class DbConnectionPool;
class DbConnection;
}

namespace TLSS::AUTH {

struct UserRecord {
  std::uint64_t user_id = 0;
  std::string username;
  PasswordCredentials credentials;
};

enum class UserLookupStatus {
  k_found,
  k_not_found,
  k_backend_unavailable,
};

struct UserLookupResult {
  UserLookupStatus status = UserLookupStatus::k_backend_unavailable;
  UserRecord user;
};

enum class UserCreateStatus {
    k_created,
    k_username_taken,
    k_backend_unavailable,
    k_outcome_unknown,
};

struct UserCreateResult {
    UserCreateStatus status = UserCreateStatus::k_backend_unavailable;
    std::uint64_t user_id = 0;
};

class UserRepository {
 public:
  virtual ~UserRepository() = default;

  // AuthService 可能被多个业务线程同时调用。
  // 实现类应支持并发查询。
  virtual UserLookupResult find_active_user(const std::string& username) = 0;
  // 用户与虚拟根目录必须原子创建；仅提交成功时返回 user_id。
  virtual UserCreateResult create_user_with_root(
      const std::string& username, const PasswordCredentials& credentials) = 0;
};

class MySqlUserRepository final : public UserRepository {
 public:
  explicit MySqlUserRepository(BASE::DbConnectionPool& pool);
  // provider 必须线程安全，并保证每个调用在句柄存活期间独占连接。
  // 默认构造方式仍从连接池借用，最后一个句柄释放时自动归还。
  using ConnectionProvider = std::function<std::shared_ptr<BASE::DbConnection>()>;
  explicit MySqlUserRepository(ConnectionProvider provider);

  UserLookupResult find_active_user(const std::string& username) override;
  UserCreateResult create_user_with_root(
      const std::string& username, const PasswordCredentials& credentials) override;

 private:
  ConnectionProvider connection_provider_;
};

}  // namespace TLSS::AUTH

#endif  // __INC_AUTH_USER_REPOSITORY_H__
