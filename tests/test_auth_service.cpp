#include "auth/auth_service.h"
#include "auth/password_hasher.h"
#include "auth/user_repository.h"

#include <gtest/gtest.h>
#include <stdexcept>
#include <string>

namespace {
using namespace TLSS::AUTH;

class FakeUsers final : public UserRepository {
public:
    UserLookupResult find_active_user(const std::string&) override { return lookup; }
    UserCreateResult create_user_with_root(const std::string& username,
                                          const PasswordCredentials& credentials) override {
        ++calls;
        saved_username = username;
        saved_credentials = credentials;
        if (throws) throw std::runtime_error("private backend details");
        return result;
    }
    int calls = 0;
    bool throws = false;
    std::string saved_username;
    PasswordCredentials saved_credentials;
    UserCreateResult result{UserCreateStatus::k_created, 42};
    UserLookupResult lookup;
};

TEST(AuthSignupTest, ValidatesBeforeRepositoryAccess) {
    FakeUsers users;
    AuthService service(users);
    for (const auto& username : {"", "ab", "alice bob", "../alice", "alice;", "中文"}) {
        EXPECT_EQ(service.signup({username, "password123"}).status,
                  SignupStatus::k_invalid_username);
    }
    EXPECT_EQ(service.signup({std::string(65, 'a'), "password123"}).status,
              SignupStatus::k_invalid_username);
    for (const auto& password : {std::string(), std::string("short"),
                                std::string(129, 'x'), std::string("pass\nword"),
                                std::string("pass\0word", 9)}) {
        EXPECT_EQ(service.signup({"alice", password}).status,
                  SignupStatus::k_invalid_password);
    }
    EXPECT_EQ(users.calls, 0);
}

TEST(AuthSignupTest, HashesPasswordAndNormalizesUsername) {
    FakeUsers users;
    AuthService service(users);
    const auto result = service.signup({"Alice_123", " password123 "});
    ASSERT_EQ(result.status, SignupStatus::k_created);
    EXPECT_EQ(result.user_id, 42U);
    EXPECT_EQ(result.username, "alice_123");
    EXPECT_EQ(users.saved_username, "alice_123");
    EXPECT_TRUE(PasswordHasher::verify_password(" password123 ", users.saved_credentials));
    EXPECT_FALSE(PasswordHasher::verify_password("password123", users.saved_credentials));
    users.lookup = {UserLookupStatus::k_found, {42, users.saved_username, users.saved_credentials}};
    EXPECT_EQ(service.authenticate({"alice_123", " password123 "}).status,
              AuthStatus::k_authenticated);
}

TEST(AuthSignupTest, MapsRepositoryFailuresWithoutIdentity) {
    FakeUsers users;
    AuthService service(users);
    for (const auto& pair : {
             std::make_pair(UserCreateStatus::k_username_taken, SignupStatus::k_username_taken),
             std::make_pair(UserCreateStatus::k_backend_unavailable, SignupStatus::k_backend_unavailable),
             std::make_pair(UserCreateStatus::k_outcome_unknown, SignupStatus::k_outcome_unknown)}) {
        users.result = {pair.first, 99};
        const auto result = service.signup({"alice", "password123"});
        EXPECT_EQ(result.status, pair.second);
        EXPECT_EQ(result.user_id, 0U);
        EXPECT_TRUE(result.username.empty());
    }
    users.throws = true;
    EXPECT_EQ(service.signup({"alice", "password123"}).status,
              SignupStatus::k_backend_unavailable);
}

TEST(AuthSignupTest, RejectsMalformedSuccessfulRepositoryResult) {
    FakeUsers users;
    users.result.user_id = 0;
    AuthService service(users);
    EXPECT_EQ(service.signup({"alice", "password123"}).status,
              SignupStatus::k_backend_unavailable);
}
}  // namespace / 命名空间结束
