#include "ftp/object_gc_worker.h"
#include "ftp/transfer_logger.h"

#include <exception>
#include <stdexcept>
#include <string>
namespace TLSS::FileStorage {
namespace {

void report_gc_error(const char* message) noexcept {
  try {
    Transfer::transfer_logger()->error("Object GC: {}", message);
  } catch (...) {
    // 日志失败不能让异常逃出工作线程。
  }
}
}  // namespace

ObjectGcWorker::ObjectGcWorker(ObjectGarbageCollector& collector, ObjectGcWorkerConfig config)
    : _collector(collector)
    , _config(config)
    , _thread(
          [this] {
            run();
          },
          "tlss_object_gc") {
  if (_config.interval.count() <= 0 || _config.grace_period.count() <= 0) {
    throw std::invalid_argument("GC interval and grace period must be positive");
  }
}

ObjectGcWorker::~ObjectGcWorker() {
  stop();
}

bool ObjectGcWorker::start() {
  std::lock_guard<std::mutex> lock(_lifecycle_mutex);
  if (_phase == Phase::running) {
    return true;
  }
  if (_phase == Phase::stopped) {
    return false;
  }

  try {
    _thread.start();
    _phase = Phase::running;
  } catch (const std::exception& e) {
    return false;
  }
  return true;
}

void ObjectGcWorker::stop() {
  std::lock_guard<std::mutex> lock(_lifecycle_mutex);
  if (_phase == Phase::stopped) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _stop_requested = true;
  }
  _wake.notify_all();
  if (_phase == Phase::running) {
    _thread.join();
  }

  _phase = Phase::stopped;
}

void ObjectGcWorker::run() {
  std::string after_hash;
  while (true) {
    {
      std::unique_lock<std::mutex> lock(_mutex);

      if (_wake.wait_for(lock, _config.interval, [this] {
            return _stop_requested;
          })) {
        return;
      }
    }
    try {
      if (!_collector.collect_batch(after_hash, _config.grace_period)) {
        report_gc_error("batch incomplete; continuing on the next interval");
      }
    } catch (const std::exception& error) {
      report_gc_error(error.what());
    } catch (...) {
      report_gc_error("unknown exception during batch collection");
    }
  }
}

}  // namespace TLSS::FileStorage
