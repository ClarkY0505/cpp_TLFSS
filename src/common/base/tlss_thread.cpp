#include "common/base/current_thread.h"
#include "common/base/tlss_thread.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <semaphore.h>
namespace TLSS::BASE {

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
  // detach和join不能同时执行
  if (_started && !_joined) {
    //
    // 分离成守护线程后，
    // 当主线程结束，子线程会自动结束
    _thread->detach();
  }
}

void Thread::start() {
  _started = true;
  sem_t sem;
  sem_init(&sem, false, 0);

  /* _thread = std::shared_ptr<std::thread>(new std::thread([&](){})); */
  _thread = std::make_shared<std::thread>([&]() {
    // 获取线程的tid
    _tid = CurrentThread::tid();
    // 信号量增加
    sem_post(&sem);
    _func();
  });

  // 这里必须要等待上面线程开始执行后
  // 获取到当前线程的tid后才能执行
  sem_wait(&sem);
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
