#include <mutex>
#include "common/net/event_loop.h"
#include "common/net/event_loop_thread.h"
using namespace TLSS::NET;
// EventLoop 在工作线程栈上创建，_loop 仅在该线程存活期间有效。
EventLoopThread::EventLoopThread(const ThreadInitCallback& cb, const std::string& name)
    : _loop(nullptr)
    , _thread(std::bind(&EventLoopThread::thread_func, this), name)
    , _cb(cb) {}

EventLoopThread::~EventLoopThread() {
  // 统一退出并回收线程，即使 loop 已自行停止也不能跳过 join。
  stop();
}

void EventLoopThread::stop() {
  // 持锁期间请求退出，防止工作线程先清空指针并销毁栈上的 EventLoop。
  {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_loop != nullptr) {
      _loop->quit();
    }
  }
  // 即使 loop 已自行退出，也必须 join，不能由 Thread 析构时 detach。
  if (_thread.started() && !_joined) {
    _thread.join();
    _joined = true;
  }
}

EventLoop* EventLoopThread::start_loop() {
  // 启动底层新线程
  _thread.start();

  EventLoop* loop = nullptr;
  {
    std::unique_lock<std::mutex> lock(_mutex);
    while (_loop == nullptr) {
      _cond.wait(lock);
    }
    loop = _loop;
  }

  return loop;
}

//
// 这个方法在单独的新线程中运行
void EventLoopThread::thread_func() {
  // 创建一个独立的eventloop,和上面的线程一一对应的
  // one loop per thread
  EventLoop loop;
  if (_cb) {
    _cb(&loop);
  }

  {
    std::unique_lock<std::mutex> lock(_mutex);
    _loop = &loop;
    _cond.notify_one();
  }

  //
  // EventLoop -> Poller.poll
  loop.loop();

  //
  // 如果说能走到这里，说明loop.loop()中结束了监听
  std::unique_lock<std::mutex> lock(_mutex);
  _loop = nullptr;
}
