#include <mutex>
#include "common/net/event_loop.h"
#include "common/net/event_loop_thread.h"
using namespace TLSS::NET;
EventLoopThread::EventLoopThread(const ThreadInitCallback& cb, const std::string& name)
    : _loop(nullptr)
    , _exiting(false)
    , _thread(std::bind(&EventLoopThread::thread_func, this), name)
    , _cb(cb) {}

EventLoopThread::~EventLoopThread() {
  _exiting = true;
  if (_loop != nullptr) {
    _loop->quit();
    _thread.join();
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
