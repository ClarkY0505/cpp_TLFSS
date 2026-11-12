#ifndef __INC_FTP_OBJECT_GC_WORKER_H__
#define __INC_FTP_OBJECT_GC_WORKER_H__

#include <chrono>
#include <condition_variable>
#include <mutex>

#include "common/base/tlss_thread.h"
#include "ftp/object_garbage_collector.h"

namespace TLSS::FileStorage {

struct ObjectGcWorkerConfig {
  std::chrono::milliseconds interval{30000};  // 两批之间等待 30 秒。
  std::chrono::seconds grace_period{600};     // 无引用宽限期 10 分钟。
};

class ObjectGcWorker {
 public:
  explicit ObjectGcWorker(ObjectGarbageCollector& collector, ObjectGcWorkerConfig config = {});

  // 析构时停止并等待线程退出，collector 必须比本对象存活更久。
  ~ObjectGcWorker();

  ObjectGcWorker(const ObjectGcWorker&) = delete;
  ObjectGcWorker& operator=(const ObjectGcWorker&) = delete;

  // true：启动成功或已经运行。
  // false：已经停止，或创建线程失败。
  // 停止后不支持重新启动。
  bool start();

  // 从工作线程之外调用；允许重复调用。
  // 唤醒定时等待，并等待正在执行的批次结束后退出。
  void stop();

 private:
  void run();

  ObjectGarbageCollector& _collector;
  const ObjectGcWorkerConfig _config;

  enum class Phase { created, running, stopped };

  // 串行化 start/stop，保护阶段与线程句柄。
  std::mutex _lifecycle_mutex;
  Phase _phase = Phase::created;

  // 保护停止请求，供工作线程等待、停止线程唤醒。
  std::mutex _mutex;
  std::condition_variable _wake;
  bool _stop_requested = false;

  BASE::Thread _thread;
};

}  // namespace TLSS::FileStorage
#endif  // __INC_FTP_OBJECT_GC_WORKER_H__
