#ifndef TLSS_FTP_DOWNLOAD_ENGINE_H
#define TLSS_FTP_DOWNLOAD_ENGINE_H

#include "ftp/async_file_source.h"
#include "ftp/metadata_service.h"
#include "ftp/object_storage_engine.h"
#include "ftp/transfer_task.h"

namespace TLSS::FileStorage {
class DownloadTask final : public TransferTask, public std::enable_shared_from_this<DownloadTask> {
  public:
    ~DownloadTask() override;
    void start() override;
    void acknowledge() override;
    void cancel() noexcept override;

  private:
    friend class DownloadEngine;
    DownloadTask(TransferContext context, std::string name, Transfer::DiskExecutor& disk,
                 MetadataService& metadata, ObjectStorageEngine& objects,
                 const std::atomic<bool>& stopping, Notify notify);
    void on_ready(const Transfer::AsyncFileSource::Info& info);
    void on_failed(std::string error);
    void on_sent(std::error_code error);

    std::string _name;
    Transfer::DiskExecutor& _disk;
    MetadataService& _metadata;
    ObjectStorageEngine& _objects;
    std::shared_ptr<Transfer::AsyncFileSource> _source;
    bool _started = false;
};

class DownloadEngine {
  public:
    DownloadEngine(Transfer::DiskExecutor& disk, MetadataService& metadata,
                   ObjectStorageEngine& objects, const std::atomic<bool>& stopping)
        : _disk(disk), _metadata(metadata), _objects(objects), _stopping(stopping) {}
    std::shared_ptr<DownloadTask> create(TransferContext context, std::string name,
                                         TransferTask::Notify notify) const;

  private:
    Transfer::DiskExecutor& _disk;
    MetadataService& _metadata;
    ObjectStorageEngine& _objects;
    const std::atomic<bool>& _stopping;
};
}  // namespace TLSS::FileStorage
#endif
