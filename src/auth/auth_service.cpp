#include "auth/auth_service.h"

#include "auth/password_hasher.h"
#include "auth/user_repository.h"

#include <exception>
#include <algorithm>

namespace TLSS::AUTH {

AuthService::AuthService(UserRepository& users)
    : users_(users) {}

bool AuthService::valid_signup_username(std::string_view username) noexcept {
    if (username.size() < 3 || username.size() > 64) return false;
    return std::all_of(username.begin(), username.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '_';
    });
}

bool AuthService::valid_signup_password(std::string_view password) noexcept {
    if (password.size() < 8 || password.size() > 128) return false;
    // 不 trim 密码；拒绝无法通过行协议可靠输入的控制字符。
    return std::all_of(password.begin(), password.end(), [](unsigned char ch) {
        return ch >= 0x20 && ch != 0x7f;
    });
}

SignupResult AuthService::signup(const SignupRequest& request) const {
    if (!valid_signup_username(request.username)) {
        return {SignupStatus::k_invalid_username, 0, {}};
    }
    if (!valid_signup_password(request.password)) {
        return {SignupStatus::k_invalid_password, 0, {}};
    }
    try {
        std::string username = request.username;
        for (char& ch : username) {
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        }
        // 密码计算先于借用连接，避免占用连接池和数据库事务时间。
        const auto credentials = PasswordHasher::hash_password(request.password);
        const auto result = users_.create_user_with_root(username, credentials);
        switch (result.status) {
            case UserCreateStatus::k_created:
                if (result.user_id == 0) return {};
                return {SignupStatus::k_created, result.user_id, std::move(username)};
            case UserCreateStatus::k_username_taken:
                return {SignupStatus::k_username_taken, 0, {}};
            case UserCreateStatus::k_outcome_unknown:
                return {SignupStatus::k_outcome_unknown, 0, {}};
            case UserCreateStatus::k_backend_unavailable:
                return {};
        }
    } catch (const std::exception&) {
        // 不向客户端泄露 SQL、密码或内部错误信息。
        return {};
    }
    return {};
}

AuthResult AuthService::authenticate(const LoginRequest& request) const {
  if (request.username.empty() || request.username.size() > 64 || request.password.empty() ||
      request.password.size() > 128) {
    return {AuthStatus::k_invalid_credentials, 0, {}};
  }

  try {
    const auto lookup = users_.find_active_user(request.username);

    if (lookup.status == UserLookupStatus::k_not_found) {
      return {AuthStatus::k_invalid_credentials, 0, {}};
    }

    if (lookup.status != UserLookupStatus::k_found) {
      return {};
    }

    if (lookup.user.user_id == 0 || lookup.user.username.empty()) {
      return {};
    }

    if (!PasswordHasher::verify_password(request.password, lookup.user.credentials)) {
      return {AuthStatus::k_invalid_credentials, 0, {}};
    }

    return {
        AuthStatus::k_authenticated,
        lookup.user.user_id,
        lookup.user.username,
    };
  } catch (const std::exception&) {
    // 数据访问异常、凭据格式损坏、OpenSSL 失败统一映射为服务故障。
    // 不将异常文本、密码或密码哈希返回客户端。
    return {};
  }
}

}  // namespace TLSS::AUTH
