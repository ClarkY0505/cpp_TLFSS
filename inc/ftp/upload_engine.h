#ifndef TLSS_FTP_UPLOAD_ENGINE_H
#define TLSS_FTP_UPLOAD_ENGINE_H

#include "common/base/tlss_thread_pool.h"
#include "ftp/async_file_receiver.h"
#include "ftp/metadata_service.h"
#include "ftp/object_storage_engine.h"
#include "ftp/transfer_task.h"

namespace TLSS::FileStorage {
struct UploadRequest {
  std::uint64_t size = 0;
  std::string name;
};

class UploadTask final : public TransferTask, public std::enable_shared_from_this<UploadTask> {
 public:
  ~UploadTask() override;
  void start() override;
  void consume(NET::Buffer& buffer) override;
  void cancel() noexcept override;

 private:
  friend class UploadEngine;
  UploadTask(TransferContext context, UploadRequest request, BASE::ThreadPool& workers,
             Transfer::DiskExecutor& disk, MetadataService& metadata, ObjectStorageEngine& objects,
             const std::atomic<bool>& stopping, Notify notify);
  void on_prepared(StorageContext<std::string> parent, std::string error);
  void on_ready();
  void on_done(std::string hash);
  void on_failed(std::string error);

  UploadRequest _request;
  BASE::ThreadPool& _workers;
  Transfer::DiskExecutor& _disk;
  MetadataService& _metadata;
  ObjectStorageEngine& _objects;
  std::string _parent_id;
  std::shared_ptr<Transfer::AsyncReceiver> _writer;
  bool _started = false;
};

class UploadEngine {
 public:
  UploadEngine(BASE::ThreadPool& workers, Transfer::DiskExecutor& disk, MetadataService& metadata,
               ObjectStorageEngine& objects, const std::atomic<bool>& stopping)
      : _workers(workers)
      , _disk(disk)
      , _metadata(metadata)
      , _objects(objects)
      , _stopping(stopping) {}
  std::shared_ptr<UploadTask> create(TransferContext context, UploadRequest request,
                                     TransferTask::Notify notify) const;

 private:
  BASE::ThreadPool& _workers;
  Transfer::DiskExecutor& _disk;
  MetadataService& _metadata;
  ObjectStorageEngine& _objects;
  const std::atomic<bool>& _stopping;
};
}  // namespace TLSS::FileStorage
#endif
