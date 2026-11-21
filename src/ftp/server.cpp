#include "ftp/server.h"

#include "ftp/metadata_service.h"
#include "ftp/transfer_validation.h"

namespace TLSS::FileStorage {
bool StorageRequestContext::valid() const {
  return user_id != 0 && !username.empty() && Transfer::valid_directory_path(directory);
}

AUTH::AuthResult StorageService::authenticate(const AUTH::LoginRequest& request) const {
  return _auth.authenticate(request);
}

AUTH::SignupResult StorageService::signup(const AUTH::SignupRequest& request) const {
  return _auth.signup(request);
}

StorageContext<std::vector<FileInfo>> StorageService::find_file_by_current_dir(
    const StorageRequestContext& context) const {
  if (!context.valid())
    return {};
  return _metadata.find_file_by_current_dir(context.username, context.directory);
}

StorageContext<std::string> StorageService::move_dir_by_current_dir(
    const StorageRequestContext& context, const std::string& directory) const {
  if (!context.valid())
    return {};
  return _metadata.move_dir_by_current_dir(context.username, context.directory, directory);
}

CreateDirectoryResult StorageService::create_dir(const StorageRequestContext& context,
                                                 const std::string& name) const {
  if (!context.valid())
    return CreateDirectoryResult::failed;
  return _metadata.create_dir(context.username, context.directory, name);
}

DeleteFileResult StorageService::delete_file(const StorageRequestContext& context,
                                             const std::string& name) const {
  if (!context.valid())
    return DeleteFileResult::failed;
  return _metadata.delete_file(context.username, context.directory, name);
}

DeleteDirectoryResult StorageService::delete_empty_directory(const StorageRequestContext& context,
                                                             const std::string& name) const {
  if (!context.valid())
    return DeleteDirectoryResult::failed;
  return _metadata.delete_empty_directory(context.username, context.directory, name);
}

DeleteDirectoryTreeResult StorageService::delete_directory_tree(
    const StorageRequestContext& context, const std::string& name) const {
  if (!context.valid())
    return DeleteDirectoryTreeResult::failed;
  return _metadata.delete_directory_tree(context.username, context.directory, name);
}

RenameFileResult StorageService::rename_file(const StorageRequestContext& context,
                                             const std::string& old_name,
                                             const std::string& new_name) const {
  if (!context.valid())
    return RenameFileResult::failed;
  return _metadata.rename_file(context.username, context.directory, old_name, new_name);
}

MoveFileResult StorageService::move_file(const StorageRequestContext& context,
                                         const std::string& target_directory,
                                         const std::string& file_name) const {
  if (!context.valid())
    return MoveFileResult::failed;
  return _metadata.move_file(context.username, context.directory, target_directory, file_name);
}

StorageContext<std::string> StorageService::prepare_upload(const StorageRequestContext& context,
                                                           const UploadRequest& request) const {
  return _uploads.prepare(context, request);
}

ObjectStorageEngine::WriteHandle StorageService::create_temporary_upload(std::uint64_t size) const {
  return _uploads.create_temporary(size);
}

std::string StorageService::commit_upload(const StorageRequestContext& context,
                                          const std::string& parent_id, const std::string& name,
                                          Transfer::Receiver& receiver) const {
  return _uploads.publish_and_commit(context, parent_id, name, receiver);
}

DownloadObject StorageService::open_download(const StorageRequestContext& context,
                                             const std::string& name) const {
  return _downloads.open(context, name);
}

}  // namespace TLSS::FileStorage
