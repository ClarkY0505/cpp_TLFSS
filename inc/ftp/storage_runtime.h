#ifndef TLSS_FTP_STORAGE_RUNTIME_H
#define TLSS_FTP_STORAGE_RUNTIME_H

#include "auth/auth_service.h"
#include "auth/user_repository.h"
#include "common/base/mysql_connection_pool.h"
#include "common/base/tlss_thread_pool.h"
#include "ftp/disk_executor.h"
#include "ftp/download_engine.h"
#include "ftp/metadata_service.h"
#include "ftp/object_coordinator.h"
#include "ftp/object_garbage_collector.h"
#include "ftp/object_gc_worker.h"
#include "ftp/server.h"
#include "ftp/storage_recovery_engine.h"
#include "ftp/tcp_storage_server.h"
#include "ftp/upload_engine.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

namespace TLSS::FileStorage {
// Composition root and lifecycle owner. Construct/configure on the base loop,
// prepare under StorageRootLock, and stop from a separate control thread.
class StorageRuntime {
 public:
  explicit StorageRuntime(NET::EventLoop* loop, const NET::InetAddress& address,
                          const std::string& name = "tlss_service", int io_threads = 4,
                          int worker_threads = 8, int worker_queue_size = 256,
                          std::shared_ptr<OBSERVABILITY::StorageMetrics> metrics = {},
                          std::shared_ptr<MetadataCache> cache = {});
  ~StorageRuntime();
  StorageRuntime(const StorageRuntime&) = delete;
  StorageRuntime& operator=(const StorageRuntime&) = delete;
  bool prepare_storage();
  void start();
  bool stop(std::chrono::milliseconds grace_period = std::chrono::seconds(5));
  void sample_metrics();

 private:
  std::shared_ptr<OBSERVABILITY::StorageMetrics> _metrics;
  int _thread_pool_threads_num;
  NET::EventLoop* _loop;
  BASE::DbConnectionPool* _db_conn_pool;
  BASE::ThreadPool _th_pool;
  Transfer::DiskExecutor _disk_executor{"tlss_disk"};
  std::atomic<bool> _stopping{false};
  std::mutex _stop_mutex;
  std::condition_variable _stop_done;
  bool _started = false;
  bool _stop_started = false;
  bool _stopped = false;
  bool _stop_succeeded = false;

  AUTH::MySqlUserRepository _user_repository;
  AUTH::AuthService _auth_service;
  MetadataService _metadata_service;
  ObjectStorageEngine _object_storage;
  ObjectCoordinator _object_coordinator;
  ObjectGarbageCollector _object_gc;
  ObjectGcWorker _object_gc_worker;
  StorageRecoveryEngine _recovery;
  UploadEngine _upload_engine;
  DownloadEngine _download_engine;
  StorageService _service;
  TcpStorageServer _server;
};
}  // namespace TLSS::FileStorage
#endif
