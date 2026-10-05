#ifndef __INC_COMMON_BASE_TLSS_THREAD_H__
#define __INC_COMMON_BASE_TLSS_THREAD_H__

#include "common/NoCopy.h"

#include <unistd.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace TLSS::BASE {
class Thread : public NoCopy {
 public:
  using ThreadFunc = std::function<void()>;
  explicit Thread(ThreadFunc, const std::string& name = std::string());
  ~Thread();

  void start();
  void join();

  bool started() const {
    return _started;
  }
  pid_t tid() const {
    return _tid;
  }

  const std::string& name() const {
    return _name;
  }
  static int num_created() {
    return static_cast<int>(_num_created);
  }

 private:
  void set_default_name();
  bool _started;
  bool _joined;
  //
  // 为什么使用智能指针，
  // 由于c++11 中的std::thread td定义后
  // 线程直接就启动了，
  // 此时就不能控制线程启动的时机
  std::shared_ptr<std::thread> _thread;
  pid_t _tid;
  ThreadFunc _func;
  std::string _name;

  static std::atomic<int> _num_created;
};
}  // namespace TLSS::BASE

#endif  // __INC_COMMON_BASE_TLSS_THREAD_H__
