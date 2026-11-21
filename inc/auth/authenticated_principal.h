#ifndef TLSS_AUTH_AUTHENTICATED_PRINCIPAL_H
#define TLSS_AUTH_AUTHENTICATED_PRINCIPAL_H

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace TLSS::AUTH {

class AuthService;

class AuthenticatedPrincipal final {
 public:
  AuthenticatedPrincipal(const AuthenticatedPrincipal&) = delete;
  AuthenticatedPrincipal& operator=(const AuthenticatedPrincipal&) = delete;

  std::uint64_t user_id() const noexcept {
    return user_id_;
  }
  const std::string& username() const noexcept {
    return username_;
  }

 private:
  friend class AuthService;
  struct Issuer {};

  AuthenticatedPrincipal(std::uint64_t user_id, std::string username,
                         std::shared_ptr<const Issuer> issuer)
      : user_id_(user_id)
      , username_(std::move(username))
      , issuer_(std::move(issuer)) {}

  const std::uint64_t user_id_;
  const std::string username_;
  const std::shared_ptr<const Issuer> issuer_;
};

using PrincipalHandle = std::shared_ptr<const AuthenticatedPrincipal>;

}  // namespace TLSS::AUTH

#endif  // TLSS_AUTH_AUTHENTICATED_PRINCIPAL_H
