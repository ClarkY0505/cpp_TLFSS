#ifndef TLSS_FTP_DOWNLOAD_ENGINE_H
#define TLSS_FTP_DOWNLOAD_ENGINE_H

#include "ftp/object_storage_engine.h"
#include "ftp/storage_request.h"

namespace TLSS::FileStorage {
class MetadataService;
struct DownloadObject {
  ObjectStorageEngine::ReadHandle source;
  std::string hash;
};

class DownloadEngine {
 public:
  DownloadEngine(const AUTH::AuthService& auth, MetadataService& metadata,
                 ObjectStorageEngine& objects)
      : _auth(auth)
      , _metadata(metadata)
      , _objects(objects) {}
  DownloadObject open(const StorageRequestContext& context, const std::string& name) const;

 private:
  const AUTH::AuthService& _auth;
  MetadataService& _metadata;
  ObjectStorageEngine& _objects;
};
}  // namespace TLSS::FileStorage

#endif  // TLSS_FTP_DOWNLOAD_ENGINE_H
