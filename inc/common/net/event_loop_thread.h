#ifndef __INC_COMMON_NET_EVENT_LOOP_THREAD_H__
#define __INC_COMMON_NET_EVENT_LOOP_THREAD_H__

#include "common/NoCopy.h"
#include "common/base/tlss_thread.h"

#include <condition_variable>
#include <functional>
#include <mutex>
namespace TLSS::NET {
class EventLoop;
class EventLoopThread : NoCopy {
 public:
  using ThreadInitCallback = std::function<void(EventLoop*)>;
  EventLoopThread(const ThreadInitCallback& cb = ThreadInitCallback(),
                  const std::string& name = std::string());
  ~EventLoopThread();

  EventLoop* start_loop();

 private:
  void thread_func();

  EventLoop* _loop;
  bool _exiting;
  BASE::Thread _thread;
  std::mutex _mutex;
  std::condition_variable _cond;
  ThreadInitCallback _cb;
};
}  // namespace TLSS::NET

#endif  // __INC_COMMON_NET_EVENT_LOOP_THREAD_H__
