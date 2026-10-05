#ifndef __INC_COMMON_NET_EVENT_LOOP_THREAD_POOL_H__
#define __INC_COMMON_NET_EVENT_LOOP_THREAD_POOL_H__
#include <pthread.h>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "common/NoCopy.h"

namespace TLSS::NET {
class EventLoop;
class EventLoopThread;
class EventLoopThreadPool : public NoCopy {
 public:
  using ThreadInitCallback = std::function<void(EventLoop*)>;
  EventLoopThreadPool(EventLoop* base_loop, const std::string& name_arg);
  ~EventLoopThreadPool();

  void set_thread_num(int num_thread) {
    _num_thread = num_thread;
  }

  void start(const ThreadInitCallback& cb = ThreadInitCallback());
  //
  // 如果工作在多线程中
  // baseloop默认以轮询的方式分配channel给subloop
  EventLoop* get_next_loop();
  std::vector<EventLoop*> get_all_loops();

  bool started() const {
    return _started;
  }
  const std::string name() const {
    return _name;
  }

 private:
  EventLoop* _base_loop;
  std::string _name;
  bool _started;
  int _num_thread;
  std::size_t _next;
  std::vector<std::unique_ptr<EventLoopThread>> _threads;
  std::vector<EventLoop*> _loops;
};
}  // namespace TLSS::NET

#endif  // __INC_COMMON_NET_EVENT_LOOP_THREAD_POOL_H__
