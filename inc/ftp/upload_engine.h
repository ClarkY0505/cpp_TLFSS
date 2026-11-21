#ifndef TLSS_FTP_UPLOAD_ENGINE_H
#define TLSS_FTP_UPLOAD_ENGINE_H

#include "ftp/metadata_types.h"
#include "ftp/object_storage_engine.h"
#include "ftp/storage_request.h"

namespace TLSS::FileStorage {
class MetadataService;
class ObjectCoordinator;

struct UploadRequest {
  std::uint64_t size = 0;
  std::string name;
};

class UploadEngine {
 public:
  UploadEngine(MetadataService& metadata, ObjectStorageEngine& objects,
               ObjectCoordinator& coordinator,
               std::shared_ptr<OBSERVABILITY::StorageMetrics> metrics = {});
  StorageContext<std::string> prepare(const StorageRequestContext& context,
                                      const UploadRequest& request) const;
  ObjectStorageEngine::WriteHandle create_temporary(std::uint64_t size) const;
  std::string publish_and_commit(const StorageRequestContext& context, const std::string& parent_id,
                                 const std::string& name, Transfer::Receiver& receiver) const;

 private:
  MetadataService& _metadata;
  ObjectStorageEngine& _objects;
  ObjectCoordinator& _coordinator;
  std::shared_ptr<OBSERVABILITY::StorageMetrics> _metrics;
};
}  // namespace TLSS::FileStorage

#endif  // TLSS_FTP_UPLOAD_ENGINE_H
