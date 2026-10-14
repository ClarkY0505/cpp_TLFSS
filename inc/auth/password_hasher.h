#ifndef __INC_AUTH_PASSWORD_HASHER_H__
#define __INC_AUTH_PASSWORD_HASHER_H__

#include <string>
#include <string_view>

namespace TLSS::AUTH {

struct PasswordCredentials {
  std::string salt;
  std::string password_hash;
};

class PasswordHasher {
 public:
  // 密码长度要求为 8～128 字节。
  // 输入不合法或 OpenSSL 操作失败时抛出异常。
  static PasswordCredentials hash_password(std::string_view password);

  // 密码不匹配时返回 false。
  // 数据库存储格式损坏或 OpenSSL 操作失败时抛出异常。
  static bool verify_password(std::string_view password, const PasswordCredentials& credentials);
};

}  // namespace TLSS::AUTH

#endif  // __INC_AUTH_PASSWORD_HASHER_H__
