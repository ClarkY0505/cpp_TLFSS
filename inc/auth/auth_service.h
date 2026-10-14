#ifndef __INC_AUTH_AUTH_SERVICE_H__
#define __INC_AUTH_AUTH_SERVICE_H__
/*
 * 只是尝试一下依赖倒置原则
 * 实际上这里使用没有什么意义
 * 因为本项目的连接池它并不是遵守依赖倒置原则
 * 所以更换数据库之后，连接池不能直接复用
 * 哪怕这里更改了数据库操作的实现
 * 整体系统依然不能使用，必须先修改连接池实现
 * */

#include <cstdint>
#include <string>
#include <string_view>

namespace TLSS::AUTH {

class UserRepository;

struct LoginRequest {
  std::string username;
  std::string password;
};

enum class AuthStatus {
  k_authenticated,
  k_invalid_credentials,
  k_backend_unavailable,
};

struct AuthResult {
  AuthStatus status = AuthStatus::k_backend_unavailable;
  std::uint64_t user_id = 0;
  std::string username;
};

struct SignupRequest {
    std::string username;
    std::string password;
};

enum class SignupStatus {
    k_created,
    k_invalid_username,
    k_invalid_password,
    k_username_taken,
    k_backend_unavailable,
    k_outcome_unknown,
};

struct SignupResult {
    SignupStatus status = SignupStatus::k_backend_unavailable;
    std::uint64_t user_id = 0;
    std::string username;
};

class AuthService {
 public:
  explicit AuthService(UserRepository& users);

  // 同步业务接口，由调用方安排到工作线程执行。
  AuthResult authenticate(const LoginRequest& request) const;
  SignupResult signup(const SignupRequest& request) const;

  // TCP 交互可提前校验；signup 仍独立校验，供将来的 RPC 调用复用。
  static bool valid_signup_username(std::string_view username) noexcept;
  static bool valid_signup_password(std::string_view password) noexcept;

 private:
  UserRepository& users_;
};

}  // namespace TLSS::AUTH
#endif  // __INC_AUTH_AUTH_SERVICE_H__
