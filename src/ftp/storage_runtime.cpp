#include "ftp/storage_runtime.h"
#include "ftp/service_logger.h"

#include <cstdlib>
#include <exception>
#include <future>
#include <stdexcept>
#include <utility>

namespace TLSS::FileStorage {
StorageRuntime::StorageRuntime(NET::EventLoop* loop, const NET::InetAddress& address,
                               const std::string& name, int io_threads, int worker_threads,
                               int worker_queue_size,
                               std::shared_ptr<OBSERVABILITY::StorageMetrics> metrics,
                               std::shared_ptr<MetadataCache> cache)
    : _metrics(std::move(metrics))
    , _thread_pool_threads_num(worker_threads)
    , _loop(loop)
    , _db_conn_pool(BASE::DbConnectionPool::get_connection_pool())
    , _th_pool("tlss_worker")
    , _user_repository(*_db_conn_pool)
    , _auth_service(_user_repository)
    , _metadata_service(*_db_conn_pool, std::move(cache))
    , _object_gc(*_db_conn_pool, _object_storage, _object_coordinator)
    , _object_gc_worker(_object_gc)
    , _recovery(*_db_conn_pool, _object_storage, _object_coordinator, _object_gc)
    , _upload_engine(_metadata_service, _object_storage, _object_coordinator, _metrics)
    , _download_engine(_metadata_service, _object_storage)
    , _service(_auth_service, _metadata_service, _upload_engine, _download_engine)
    , _server(loop, address, name, io_threads, _th_pool, _disk_executor, _service, _stopping,
              _metrics) {
  _metadata_service.set_metrics(_metrics);
  _object_storage.set_metrics(_metrics);
  _db_conn_pool->set_metrics(_metrics);
  _th_pool.set_max_queue_size(worker_queue_size);
  _th_pool.set_max_wait_queue_size(128);
  if (_metrics)
    _metrics->set(OBSERVABILITY::Metric::service_state, 1);
}
StorageRuntime::~StorageRuntime() {
  // 主循环停止后无法再执行停机回调；运行过的服务必须在析构前显式 stop。
  if (_started && !_stopped) {
    std::terminate();
  }
  if (!_started) {
    // 构造完成但未启动时，仍需释放已初始化的线程池和数据库资源。
    _th_pool.stop();
    _disk_executor.stop();
    _object_gc_worker.stop();
    _db_conn_pool->shutdown();
  }
}

void StorageRuntime::sample_metrics() {
  if (!_metrics)
    return;
  const auto workers = _th_pool.snapshot();
  const auto database = _db_conn_pool->snapshot();
  const auto disk = _disk_executor.snapshot();
  const auto cache = _metadata_service.metadata_cache_stats();
  using OBSERVABILITY::Metric;
  _metrics->set(Metric::worker_queue, workers.queued);
  _metrics->set(Metric::worker_waiting, workers.waiting);
  _metrics->set(Metric::worker_rejected, workers.rejected);
  _metrics->set(Metric::worker_timeouts, workers.timeouts);
  _metrics->set(Metric::db_total, database.total);
  _metrics->set(Metric::db_idle, database.idle);
  _metrics->set(Metric::db_borrowed, database.borrowed);
  _metrics->set(Metric::db_waiters, database.waiters);
  _metrics->set(Metric::disk_active, disk.active);
  _metrics->set(Metric::disk_rejected, disk.rejected);
  _metrics->set(Metric::cache_requests, cache.directory_list.requests +
                                            cache.download_hash.requests + cache.cd_path.requests);
  _metrics->set(Metric::cache_hits,
                cache.directory_list.hits + cache.download_hash.hits + cache.cd_path.hits);
  _metrics->set(Metric::cache_loads,
                cache.directory_list.loads + cache.download_hash.loads + cache.cd_path.loads);
}

void StorageRuntime::start() {
  // 先准备业务线程，再开放监听；停机时按相反顺序回收资源。
  _th_pool.start(_thread_pool_threads_num);
  // 这里可以设置异步磁盘任务线程池数量
  // _disk_executor.start(4);
  // 默认为2
  _disk_executor.start();
  if (!_object_gc_worker.start()) {
    throw std::runtime_error("Failed to start object GC worker");
  }
  _server.start();
  _started = true;
  if (_metrics)
    _metrics->set(OBSERVABILITY::Metric::service_state, 2);
}

bool StorageRuntime::stop(std::chrono::milliseconds grace_period) {
  // 停机过程等待主循环回调，因此不能在主循环线程内阻塞调用
  if (_loop->is_in_loop_thread()) {
    service_logger()->error("StorageRuntime::stop must run outside the base EventLoop thread");
    std::abort();
  }
  if (grace_period < std::chrono::milliseconds::zero()) {
    service_logger()->error("grace period must be nonnegative");
    std::abort();
  }

  std::unique_lock<std::mutex> lock(_stop_mutex);
  // 多个调用者只执行一次停机，其余调用者等待并共享最终结果
  if (_stop_started) {
    _stop_done.wait(lock, [this] {
      return _stopped;
    });
    return _stop_succeeded;
  }
  _stop_started = true;
  _stopping.store(true);
  if (_metrics)
    _metrics->set(OBSERVABILITY::Metric::service_state, 3);
  lock.unlock();

  // 关闭监听并暂停现有连接读取；屏障保证各 IO loop 已处理暂停请求
  auto paused = std::make_shared<std::promise<void>>();
  auto paused_future = paused->get_future();
  _server.stop_accepting([paused] {
    paused->set_value();
  });
  paused_future.wait();

  // 所有 IO 线程已越过停机屏障，现有任务仍能使用网络及数据库资源
  _th_pool.stop();
  // 磁盘任务仍可能访问数据库和向 IO loop 投递结果，必须先取消并等待收尾
  _disk_executor.stop();
  // 等待 GC 当前批次结束，后续不再访问数据库或对象目录
  _object_gc_worker.stop();

  // 任务结束后半关闭连接写端，等待对端关闭；超时则强制清理剩余连接。
  auto drained = std::make_shared<std::promise<void>>();
  auto drained_future = drained->get_future();
  _server.shutdown_connections([drained] {
    drained->set_value();
  });
  if (drained_future.wait_for(grace_period) == std::future_status::timeout) {
    _server.force_close_connections();
  }
  drained_future.wait();

  // 连接清理完成后再回收 IO 线程和数据库，最后退出主循环。
  _server.stop_io_threads();
  const bool db_stopped = _db_conn_pool->shutdown();
  _loop->quit();

  // 唤醒同时调用 stop 的线程，并返回数据库连接池的关闭结果。
  lock.lock();
  _stop_succeeded = db_stopped;
  _stopped = true;
  if (_metrics)
    _metrics->set(OBSERVABILITY::Metric::service_state, 4);
  lock.unlock();
  _stop_done.notify_all();
  return db_stopped;
}

bool StorageRuntime::prepare_storage() {
  return !_started && _recovery.prepare();
}
}  // namespace TLSS::FileStorage
