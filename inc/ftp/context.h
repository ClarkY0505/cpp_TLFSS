#ifndef __INC_FTP_CONTEXT_H__
#define __INC_FTP_CONTEXT_H__

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>

namespace TLSS::FileStorage {
template <typename T>
struct StorageContext {
  bool success = false;  // 操作是否成功
  T result{};            // 操作结果
};


enum class AuthStatus {
  k_authenticated,
  k_invalid_credentials,
  k_backend_unavailable,
};

struct AuthResult {
  AuthStatus status = AuthStatus::k_backend_unavailable;
  std::uint64_t user_id = 0;  // 仅认证成功时有效
};

enum class UserLookupStatus {
  k_found,
  k_not_found,
  k_backend_unavailable,
};

struct UserLookupResult {
  UserLookupStatus status = UserLookupStatus::k_backend_unavailable;
  std::uint64_t user_id = 0;  // 仅找到用户时有效
};

enum class LoginState {
  k_await_username,
  k_await_password,
  k_verifying,
  k_authenticated,
};

struct ConnectionSession {
  // 这些字段只由该连接所属的 IO 线程读写。
  LoginState login_state = LoginState::k_await_username;
  std::string name;
  std::string salt;
  std::string pass;
  std::uint64_t user_id = 0;
  std::string current_directory = "/";
  bool directory_change_pending = false;
};

struct FileInfo {
  std::string file_name;
  std::string type;
  std::string create_at;
  std::string update_at;
};
}  // namespace TLSS::FileStorage

#endif  // __INC_FTP_CONTEXT_H__
