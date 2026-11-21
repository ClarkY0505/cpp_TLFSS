#include "ftp/upload_engine.h"

#include "ftp/metadata_service.h"
#include "ftp/object_coordinator.h"

#include <mutex>
#include <stdexcept>
#include <utility>

namespace TLSS::FileStorage {
UploadEngine::UploadEngine(MetadataService& metadata, ObjectStorageEngine& objects,
                           ObjectCoordinator& coordinator,
                           std::shared_ptr<OBSERVABILITY::StorageMetrics> metrics)
    : _metadata(metadata)
    , _objects(objects)
    , _coordinator(coordinator)
    , _metrics(std::move(metrics)) {}

StorageContext<std::string> UploadEngine::prepare(const StorageRequestContext& context,
                                                  const UploadRequest& request) const {
  if (!context.valid())
    return {};
  return _metadata.prepare_upload(context.username, context.directory, request.name);
}

ObjectStorageEngine::WriteHandle UploadEngine::create_temporary(std::uint64_t size) const {
  return _objects.create_temporary(size);
}

std::string UploadEngine::publish_and_commit(const StorageRequestContext& context,
                                             const std::string& parent_id, const std::string& name,
                                             Transfer::Receiver& receiver) const {
  if (!context.valid())
    throw std::runtime_error("invalid storage request context");
  std::lock_guard<std::mutex> lock(_coordinator.mutex_for(receiver.hash()));
  auto object = _objects.publish(receiver);
  const auto result =
      _metadata.commit_upload(context.username, parent_id, context.directory, name, object.hash);
  if (result == UploadCommitResult::outcome_unknown)
    throw std::runtime_error("upload commit outcome unknown");
  if (result != UploadCommitResult::committed)
    throw std::runtime_error("file stored but metadata registration failed");
  if (_metrics)
    _metrics->add(OBSERVABILITY::Metric::upload_committed);
  return object.hash;
}
}  // namespace TLSS::FileStorage
