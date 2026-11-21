#include "auth/auth_service.h"
#include "auth/user_repository.h"

#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {

using namespace TLSS::AUTH;

static_assert(std::is_final_v<AuthenticatedPrincipal>);
static_assert(!std::is_aggregate_v<AuthenticatedPrincipal>);
static_assert(!std::is_constructible_v<AuthenticatedPrincipal, std::uint64_t, std::string>);
static_assert(!std::is_copy_constructible_v<AuthenticatedPrincipal>);
static_assert(!std::is_copy_assignable_v<AuthenticatedPrincipal>);
static_assert(!std::is_move_constructible_v<AuthenticatedPrincipal>);
static_assert(!std::is_move_assignable_v<AuthenticatedPrincipal>);
static_assert(!std::is_copy_constructible_v<AuthService>);
static_assert(!std::is_copy_assignable_v<AuthService>);
static_assert(!std::is_move_constructible_v<AuthService>);
static_assert(!std::is_move_assignable_v<AuthService>);

class FakeUsers final : public UserRepository {
 public:
  UserLookupResult result;
  std::size_t lookups = 0;
  std::string requested_username;
  bool throw_on_lookup = false;

  UserLookupResult find_active_user(const std::string& username) override {
    ++lookups;
    requested_username = username;
    if (throw_on_lookup) {
      throw std::runtime_error("lookup failure");
    }
    return result;
  }

  UserCreateResult create_user_with_root(const std::string&, const PasswordCredentials&) override {
    throw std::logic_error("unexpected user creation");
  }
};

void check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void check_failure(const AuthResult& result, AuthStatus expected, const char* message) {
  check(result.status == expected, message);
  check(!result.principal, "a failed authentication produced a principal");
}

void check_issuance(FakeUsers& users) {
  const LoginRequest request{"CANONICAL", "valid-password"};
  const AuthService auth(users);
  const auto login = auth.authenticate(request);
  const auto second_login = auth.authenticate(request);
  check(users.requested_username == request.username, "lookup must use the requested username");
  check(login.status == AuthStatus::k_authenticated && login.principal,
        "login must issue a principal");
  check(second_login.status == AuthStatus::k_authenticated && second_login.principal,
        "the second login must issue a principal");
  check(login.principal != second_login.principal, "separate logins must have distinct principals");
  check(login.principal->user_id() == 42, "principal must carry the repository user ID");
  check(login.principal->username() == "canonical", "principal must carry the repository username");

  const AuthService other_auth(users);
  const auto lookups = users.lookups;
  check(auth.owns(login.principal), "issuing service must recognize its principal");
  check(auth.owns(second_login.principal),
        "issuing service must recognize each principal it issues");
  check(!other_auth.owns(login.principal), "independent service must reject a foreign principal");
  check(!auth.owns({}), "service must reject a null principal");
  check(users.lookups == lookups, "ownership checks must avoid repository lookups");
}

void check_failed_authentication(FakeUsers& users) {
  const AuthService auth(users);
  const LoginRequest request{"CANONICAL", "valid-password"};
  check_failure(auth.authenticate({"", request.password}), AuthStatus::k_invalid_credentials,
                "empty username must be rejected");
  check_failure(auth.authenticate({request.username, "wrong-password"}),
                AuthStatus::k_invalid_credentials, "wrong password must be rejected");

  users.result.status = UserLookupStatus::k_not_found;
  check_failure(auth.authenticate(request), AuthStatus::k_invalid_credentials,
                "missing user must be rejected");
  users.result.status = UserLookupStatus::k_backend_unavailable;
  check_failure(auth.authenticate(request), AuthStatus::k_backend_unavailable,
                "unavailable repository must yield a service error");
  users.throw_on_lookup = true;
  check_failure(auth.authenticate(request), AuthStatus::k_backend_unavailable,
                "throwing repository must yield a service error");

  users.throw_on_lookup = false;
  users.result.status = UserLookupStatus::k_found;
  users.result.user.user_id = 0;
  check_failure(auth.authenticate(request), AuthStatus::k_backend_unavailable,
                "zero repository user ID must yield a service error");
  users.result.user.user_id = 42;
  users.result.user.username.clear();
  check_failure(auth.authenticate(request), AuthStatus::k_backend_unavailable,
                "empty repository username must yield a service error");
}

void check_lifetime(FakeUsers& users) {
  PrincipalHandle principal;
  {
    const AuthService auth(users);
    auto login = auth.authenticate({"CANONICAL", "valid-password"});
    principal = login.principal;
    check(auth.owns(principal), "service must own the principal before destruction");
  }
  check(principal->user_id() == 42 && principal->username() == "canonical",
        "principal must retain its identity after its service is destroyed");
  const AuthService replacement(users);
  check(!replacement.owns(principal),
        "a new service must reject a principal from a destroyed issuer");
}

}  // namespace

int main() {
  try {
    FakeUsers users;
    const auto credentials = PasswordHasher::hash_password("valid-password");
    const UserLookupResult valid_user{UserLookupStatus::k_found, {42, "canonical", credentials}};
    users.result = valid_user;
    check_issuance(users);
    check_lifetime(users);
    check_failed_authentication(users);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
