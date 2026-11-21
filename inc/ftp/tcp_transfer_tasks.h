#ifndef TLSS_FTP_TCP_TRANSFER_TASKS_H
#define TLSS_FTP_TCP_TRANSFER_TASKS_H

#include "common/base/tlss_thread_pool.h"
#include "ftp/async_file_receiver.h"
#include "ftp/async_file_source.h"
#include "ftp/server.h"
#include "ftp/transfer_task.h"

namespace TLSS::FileStorage {
class UploadTask final : public TransferTask, public std::enable_shared_from_this<UploadTask> {
 public:
  static std::shared_ptr<UploadTask> create(TransferContext context, UploadRequest request,
                                            BASE::ThreadPool& workers, Transfer::DiskExecutor& disk,
                                            StorageService& service,
                                            const std::atomic<bool>& stopping, Notify notify);
  ~UploadTask() override;
  void start() override;
  void consume(NET::Buffer& buffer) override;
  void cancel() noexcept override;

 private:
  UploadTask(TransferContext context, UploadRequest request, BASE::ThreadPool& workers,
             Transfer::DiskExecutor& disk, StorageService& service,
             const std::atomic<bool>& stopping, Notify notify);
  void on_prepared(StorageContext<std::string> parent, std::string error);
  void on_ready();
  void on_done(std::string hash);
  void on_failed(std::string error);

  UploadRequest _request;
  BASE::ThreadPool& _workers;
  Transfer::DiskExecutor& _disk;
  StorageService& _service;
  std::shared_ptr<Transfer::AsyncReceiver> _writer;
  bool _started = false;
};

class DownloadTask final : public TransferTask, public std::enable_shared_from_this<DownloadTask> {
 public:
  static std::shared_ptr<DownloadTask> create(TransferContext context, std::string name,
                                              Transfer::DiskExecutor& disk, StorageService& service,
                                              const std::atomic<bool>& stopping, Notify notify);
  ~DownloadTask() override;
  void start() override;
  void acknowledge() override;
  void cancel() noexcept override;

 private:
  DownloadTask(TransferContext context, std::string name, Transfer::DiskExecutor& disk,
               StorageService& service, const std::atomic<bool>& stopping, Notify notify);
  void on_ready(const Transfer::AsyncFileSource::Info& info);
  void on_failed(std::string error);
  void on_sent(std::error_code error);

  std::string _name;
  Transfer::DiskExecutor& _disk;
  StorageService& _service;
  std::shared_ptr<Transfer::AsyncFileSource> _source;
  bool _started = false;
};

}  // namespace TLSS::FileStorage

#endif  // TLSS_FTP_TCP_TRANSFER_TASKS_H
