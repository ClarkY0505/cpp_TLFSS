#ifndef TLSS_FTP_SERVER_H
#define TLSS_FTP_SERVER_H

#include "auth/auth_service.h"
#include "ftp/download_engine.h"
#include "ftp/upload_engine.h"

#include <vector>

namespace TLSS::FileStorage {
class StorageService {
 public:
  StorageService(AUTH::AuthService& auth, MetadataService& metadata, UploadEngine& uploads,
                 DownloadEngine& downloads)
      : _auth(auth)
      , _metadata(metadata)
      , _uploads(uploads)
      , _downloads(downloads) {}
  AUTH::AuthResult authenticate(const AUTH::LoginRequest& request) const;
  AUTH::SignupResult signup(const AUTH::SignupRequest& request) const;
  StorageContext<std::vector<FileInfo>> find_file_by_current_dir(
      const StorageRequestContext& context) const;
  StorageContext<std::string> move_dir_by_current_dir(const StorageRequestContext& context,
                                                      const std::string& directory) const;
  CreateDirectoryResult create_dir(const StorageRequestContext& context,
                                   const std::string& name) const;
  DeleteFileResult delete_file(const StorageRequestContext& context, const std::string& name) const;
  DeleteDirectoryResult delete_empty_directory(const StorageRequestContext& context,
                                               const std::string& name) const;
  DeleteDirectoryTreeResult delete_directory_tree(const StorageRequestContext& context,
                                                  const std::string& name) const;
  RenameFileResult rename_file(const StorageRequestContext& context, const std::string& old_name,
                               const std::string& new_name) const;
  MoveFileResult move_file(const StorageRequestContext& context,
                           const std::string& target_directory, const std::string& file_name) const;
  StorageContext<std::string> prepare_upload(const StorageRequestContext& context,
                                             const UploadRequest& request) const;
  ObjectStorageEngine::WriteHandle create_temporary_upload(std::uint64_t size) const;
  std::string commit_upload(const StorageRequestContext& context, const std::string& parent_id,
                            const std::string& name, Transfer::Receiver& receiver) const;
  DownloadObject open_download(const StorageRequestContext& context, const std::string& name) const;

 private:
  AUTH::AuthService& _auth;
  MetadataService& _metadata;
  UploadEngine& _uploads;
  DownloadEngine& _downloads;
};
}  // namespace TLSS::FileStorage

#endif  // TLSS_FTP_SERVER_H
