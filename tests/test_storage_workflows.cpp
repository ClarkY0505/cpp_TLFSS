#include "auth/user_repository.h"
#include "common/base/mysql_connection.h"
#include "ftp/metadata_service.h"
#include "ftp/object_coordinator.h"
#include "ftp/server.h"

#include <unistd.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace TLSS;
using namespace FileStorage;
void check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
int backend_calls = 0;
UploadCommitResult commit_result = UploadCommitResult::committed;
std::string stored_hash;
class Users final : public AUTH::UserRepository {
 public:
  AUTH::UserLookupResult find_active_user(const std::string&) override {
    return {AUTH::UserLookupStatus::k_not_found, {}};
  }
  AUTH::UserCreateResult create_user_with_root(const std::string&,
                                               const AUTH::PasswordCredentials&) override {
    return {AUTH::UserCreateStatus::k_outcome_unknown, 0};
  }
};

void exercise(StorageService& service, OBSERVABILITY::StorageMetrics& metrics) {
  check(service.authenticate({"alice", "password1"}).status ==
            AUTH::AuthStatus::k_invalid_credentials,
        "login dispatch");
  check(service.signup({"alice", "password1"}).status == AUTH::SignupStatus::k_outcome_unknown,
        "signup dispatch");
  StorageRequestContext context{1, "alice", "/docs"};
  for (const auto& invalid :
       {StorageRequestContext{0, "alice", "/docs"}, StorageRequestContext{1, "", "/docs"},
        StorageRequestContext{1, "alice", "/../docs"}}) {
    check(!service.prepare_upload(invalid, {3, "file"}).success, "invalid upload");
    check(service.delete_file(invalid, "file") == DeleteFileResult::failed, "invalid delete");
    check(!service.find_file_by_current_dir(invalid).success, "invalid listing");
    check(!service.move_dir_by_current_dir(invalid, "sub").success,
          "invalid move_dir_by_current_dir");
    check(service.create_dir(invalid, "sub") == CreateDirectoryResult::failed,
          "invalid create_dir");
    check(service.delete_empty_directory(invalid, "sub") == DeleteDirectoryResult::failed,
          "invalid delete_empty_directory");
    check(service.delete_directory_tree(invalid, "sub") == DeleteDirectoryTreeResult::failed,
          "invalid delete_directory_tree");
    check(service.rename_file(invalid, "old", "new") == RenameFileResult::failed,
          "invalid rename_file");
    check(service.move_file(invalid, "/target", "file") == MoveFileResult::failed,
          "invalid move_file");
    auto empty = service.create_temporary_upload(0);
    try {
      service.commit_upload(invalid, "parent", "file", *empty);
      throw std::logic_error("accepted invalid commit");
    } catch (const std::runtime_error&) {
    }
    try {
      service.open_download(invalid, "file");
      throw std::logic_error("accepted invalid download");
    } catch (const std::runtime_error&) {
    }
  }
  check(backend_calls == 0, "invalid context reached metadata");
  check(service.delete_file(context, "file") == DeleteFileResult::outcome_unknown,
        "delete outcome");
  check(service.move_dir_by_current_dir(context, "sub").result == "/docs/sub",
        "move_dir_by_current_dir result");
  check(service.create_dir(context, "sub") == CreateDirectoryResult::outcome_unknown,
        "create_dir result");
  check(service.delete_empty_directory(context, "sub") == DeleteDirectoryResult::not_empty,
        "delete_empty_directory result");
  check(service.delete_directory_tree(context, "sub") == DeleteDirectoryTreeResult::outcome_unknown,
        "delete_directory_tree result");
  check(service.rename_file(context, "old", "new") == RenameFileResult::outcome_unknown,
        "rename_file result");
  check(service.move_file(context, "/target", "file") == MoveFileResult::outcome_unknown,
        "move_file result");
  auto listing = service.find_file_by_current_dir(context);
  check(listing.success && listing.result.at(0).file_name == "file", "list results");
  auto parent = service.prepare_upload(context, {3, "file"});
  check(parent.success && parent.result == "parent", "upload preflight");
  for (const auto outcome : {UploadCommitResult::committed, UploadCommitResult::committed,
                             UploadCommitResult::failed, UploadCommitResult::outcome_unknown}) {
    commit_result = outcome;
    auto writer = service.create_temporary_upload(3);
    writer->append("abc", 3);
    try {
      auto hash = service.commit_upload(context, parent.result, "file", *writer);
      check(outcome == UploadCommitResult::committed && hash == stored_hash, "commit result");
    } catch (const std::runtime_error& error) {
      check(outcome != UploadCommitResult::committed &&
                std::string(error.what()) == (outcome == UploadCommitResult::failed
                                                  ? "file stored but metadata registration failed"
                                                  : "upload commit outcome unknown"),
            "commit error");
    }
  }
  auto object = service.open_download(context, "file");
  check(object.hash == stored_hash && ObjectStorageEngine::read(object.source, 0, 3) == "abc",
        "download bytes");
  check(metrics.get(OBSERVABILITY::Metric::upload_committed) == 2, "confirmed commit count");
}
}  // namespace

namespace TLSS::BASE {
// Keep a real, inert pool object for MetadataService's constructor. No worker starts.
DbConnectionPool::DbConnectionPool()
    : _produce_thread([] {
    })
    , _scanner_thread([] {
    }) {}
DbConnectionPool::~DbConnectionPool() = default;
DbConnection::~DbConnection() = default;
DbConnectionPool* DbConnectionPool::get_connection_pool() {
  static DbConnectionPool pool;
  return &pool;
}
}  // namespace TLSS::BASE

namespace TLSS::FileStorage {
MetadataService::MetadataService(BASE::DbConnectionPool& pool)
    : _db_conn_pool(pool) {}
StorageContext<std::vector<FileInfo>> MetadataService::find_file_by_current_dir(
    const std::string& username, const std::string& directory) {
  ++backend_calls;
  check(username == "alice" && directory == "/docs", "list arguments");
  return {true, {{"file", "file", {}, {}}}};
}
StorageContext<std::string> MetadataService::move_dir_by_current_dir(const std::string& username,
                                                                     const std::string& directory,
                                                                     const std::string& target) {
  ++backend_calls;
  check(username == "alice" && directory == "/docs" && target == "sub",
        "move_dir_by_current_dir arguments");
  return {true, "/docs/sub"};
}
CreateDirectoryResult MetadataService::create_dir(const std::string& username,
                                                  const std::string& directory,
                                                  const std::string& name) {
  ++backend_calls;
  check(username == "alice" && directory == "/docs" && name == "sub", "create_dir arguments");
  return CreateDirectoryResult::outcome_unknown;
}
DeleteFileResult MetadataService::delete_file(const std::string& username,
                                              const std::string& directory,
                                              const std::string& name) {
  ++backend_calls;
  check(username == "alice" && directory == "/docs" && name == "file", "delete arguments");
  return DeleteFileResult::outcome_unknown;
}
DeleteDirectoryResult MetadataService::delete_empty_directory(const std::string& username,
                                                              const std::string& directory,
                                                              const std::string& name) {
  ++backend_calls;
  check(username == "alice" && directory == "/docs" && name == "sub",
        "delete_empty_directory arguments");
  return DeleteDirectoryResult::not_empty;
}
DeleteDirectoryTreeResult MetadataService::delete_directory_tree(const std::string& username,
                                                                 const std::string& directory,
                                                                 const std::string& name) {
  ++backend_calls;
  check(username == "alice" && directory == "/docs" && name == "sub",
        "delete_directory_tree arguments");
  return DeleteDirectoryTreeResult::outcome_unknown;
}
RenameFileResult MetadataService::rename_file(const std::string& username,
                                              const std::string& directory,
                                              const std::string& old_name,
                                              const std::string& new_name) {
  ++backend_calls;
  check(username == "alice" && directory == "/docs" && old_name == "old" && new_name == "new",
        "rename_file arguments");
  return RenameFileResult::outcome_unknown;
}
MoveFileResult MetadataService::move_file(const std::string& username, const std::string& directory,
                                          const std::string& target_directory,
                                          const std::string& file_name) {
  ++backend_calls;
  check(username == "alice" && directory == "/docs" && target_directory == "/target" &&
            file_name == "file",
        "move_file arguments");
  return MoveFileResult::outcome_unknown;
}

StorageContext<std::string> MetadataService::prepare_upload(const std::string& username,
                                                            const std::string& directory,
                                                            const std::string& name) {
  ++backend_calls;
  check(username == "alice" && directory == "/docs" && name == "file", "preflight arguments");
  return {true, "parent"};
}
UploadCommitResult MetadataService::commit_upload(const std::string& username,
                                                  const std::string& parent,
                                                  const std::string& directory,
                                                  const std::string& name,
                                                  const std::string& hash) {
  ++backend_calls;
  check(username == "alice" && directory == "/docs" && name == "file" && parent == "parent",
        "commit arguments");
  check(std::filesystem::exists(Transfer::object_root() / hash), "publish before commit");
  stored_hash = hash;
  return commit_result;
}
StorageContext<std::string> MetadataService::find_download_hash(const std::string& username,
                                                                const std::string& directory,
                                                                const std::string& name) {
  ++backend_calls;
  check(username == "alice" && directory == "/docs" && name == "file", "download arguments");
  return {true, stored_hash};
}
}  // namespace TLSS::FileStorage

int main() {
  char path[] = "/tmp/tlss-workflows-XXXXXX";
  if (!::mkdtemp(path))
    return 1;
  int result = 0;
  try {
    check(::setenv("TLSS_STORAGE_ROOT", path, 1) == 0, "set object root");
    Users users;
    AUTH::AuthService auth(users);
    MetadataService metadata(*BASE::DbConnectionPool::get_connection_pool());
    ObjectStorageEngine objects;
    ObjectCoordinator coordinator;
    auto metrics = std::make_shared<OBSERVABILITY::StorageMetrics>();
    UploadEngine uploads(metadata, objects, coordinator, metrics);
    DownloadEngine downloads(metadata, objects);
    StorageService service(auth, metadata, uploads, downloads);
    exercise(service, *metrics);
    check(objects.list_object_hashes().size() == 1, "reuse identical object");
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    result = 1;
  }
  std::filesystem::remove_all(path);
  return result;
}
