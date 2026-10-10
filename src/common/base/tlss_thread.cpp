#include "common/base/current_thread.h"
#include "common/base/tlss_thread.h"

#include <atomic>
#include <cstdio>
#include <future>
#include <memory>
#include <thread>
#include <utility>

namespace TLSS::BASE {

namespace {
// 线程入口独立持有回调和启动通知，避免分离后访问已析构的 Thread 对象。
struct ThreadData {
  // 复制包装对象中的回调；线程启动失败时原回调仍可用于重试。
  explicit ThreadData(Thread::ThreadFunc callback)
      : func(std::move(callback)) {}

  Thread::ThreadFunc func;
  // 工作线程公布 tid 后，start() 才会返回。
  std::promise<pid_t> started;
};
}  // namespace

std::atomic<int> Thread::_num_created = 0;

Thread::Thread(ThreadFunc func, const std::string& name)
    : _started(false)
    , _joined(false)
    , _tid(0)
    , _func(std::move(func))
    , _name(name) {
  set_default_name();
}

Thread::~Thread() {
  // 没有调用 join() 时按现有接口约定分离线程；分离不会停止回调。
  if (_started && !_joined) {
    // 回调数据已由线程入口持有，包装对象可以先于工作线程析构。
    _thread->detach();
  }
}

void Thread::start() {
  // 在线程创建前准备独立数据，使线程入口不依赖本对象的生命周期。
  auto data = std::make_shared<ThreadData>(_func);
  // 等待工作线程公布 tid，保持 start() 返回后 tid() 可用的约定。
  auto started = data->started.get_future();
  // 线程只捕获独立数据；Thread 析构并 detach 后，数据仍存活到回调结束。
  _thread = std::make_shared<std::thread>([data]() {
    // 先通知启动者，再运行用户回调；之后不再访问 Thread 对象。
    data->started.set_value(CurrentThread::tid());
    data->func();
  });
  // 创建失败会抛异常，此时状态仍为未启动；成功后才允许 join/detach。
  _started = true;
  // 通过 future 接收工作线程的 tid，由启动线程独自写入 _tid。
  _tid = started.get();
}

void Thread::join() {
  _joined = true;
  _thread->join();
}

void Thread::set_default_name() {
  int num = ++_num_created;
  if (_name.empty()) {
    char buf[32] = {0};
    snprintf(buf, sizeof buf, "Thread%d", num);
    _name = buf;
  }
}
}  // namespace TLSS::BASE
