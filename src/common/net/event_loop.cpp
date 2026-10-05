#include "common/base/current_thread.h"
#include "common/net/channel.h"
#include "common/net/event_loop.h"
#include "common/net/net_logger.h"
#include "common/net/poller.h"

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/poll.h>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <utility>
#include <vector>
using namespace TLSS::NET;
namespace {
//
// 防止一个线程创建多个EventLoop
// 其实为了移植性更好，可以使用 thread_local关键字
// thread_local 是C++11提供的一个TLS
// __thread是GCC/Clang编译器提供一个编译器扩展
// 本项目是为了学习而创建的，所以采用了__thread
__thread EventLoop* t_loop_in_this_thread = nullptr;

//
// 定义默认的Poller IO复用超时时间
const int k_poll_time_ms = 10000;

//
// 创建wakeupfd,用来notify 唤醒subRactor处理新来的channel
int create_event_fd() {
  int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (evtfd < 0) {
    net_logger()->critical("eventfd error:{}", errno);
    std::abort();
  }
  return evtfd;
}
}  // namespace

EventLoop::EventLoop()
    : _looping(false)
    , _quit(false)
    , _thread_id(BASE::CurrentThread::tid())
    , _poller(Poller::new_default_poller(this))
    , _wakeup_fd(create_event_fd())
    , _wakeup_channel(new Channel(this, _wakeup_fd))
    , _current_active_channel(nullptr)
    , _calling_pending_functors(false) {
  net_logger()->debug("EventLoop created {} in thread {}", static_cast<const void*>(this),
                      _thread_id);
  if (t_loop_in_this_thread) {
    net_logger()->critical("Another EventLoop {} exists in this thread {}",
                           static_cast<const void*>(t_loop_in_this_thread), _thread_id);
    std::abort();
  } else {
    t_loop_in_this_thread = this;
  }

  // 设置wakeupfd的事件类型以及发生事件后的回调
  _wakeup_channel->set_read_cb(std::bind(&EventLoop::handle_read, this));
  //
  // 每一个eventloop都将监听wakeupchannel的EPOLLIN的读事件
  _wakeup_channel->enable_reading();
}

EventLoop::~EventLoop() {
  /* _pending_channels.close(); */
  /* for (auto& item : _managed_channels) { */
  /* auto& channel = item.second; */

  /* channel->disable_all(); */
  /* channel->remove(); */
  /* } */
  /* _managed_channels.clear(); */

  _wakeup_channel->disable_all();
  _wakeup_channel->remove();
  ::close(_wakeup_fd);
  t_loop_in_this_thread = nullptr;
}

void EventLoop::loop() {
  _looping = true;
  _quit = false;
  net_logger()->info("EventLoop {} start loop ", static_cast<const void*>(this));
  while (!_quit) {
    _active_channels.clear();
    //
    // 监听两类fd
    // 1. client fd
    // 2. wakeup fd
    _poll_return_time = _poller->poll(k_poll_time_ms, &_active_channels);
    for (Channel* channel : _active_channels) {
      // Poller监听那些channel发生事件来，然后上报给EventLoop
      // 通知channel处理相应的事件
      channel->handle_event(_poll_return_time);
    }

    //
    // 消费待注册的 Channel。
    /* consume_pending_channels(); */
    //
    // 执行当前EventLoop事件循环需要处理的回调操作
    // IO线程 mainLoop accept fd
    // mainLoop事先注册一个回调 需要subLoop执行
    do_pending_functors();
  }
  net_logger()->info("EventLoop {} stop looping", static_cast<const void*>(this));
  _looping = false;
}
/**
 *           mainloop
 *
 *  ======================== 生产者-消费者 的线程安全队列
 *
 *    sub1     sub2    sub3
 * */
void EventLoop::quit() {
  _quit = true;

  //
  // 如果在其他线程中，调用quit,
  // 在一个subloop中调用了，
  // mainLoop的quit
  // 需要把其他线程唤醒一下
  if (!is_in_loop_thread()) {
    wakeup();
  }
}

void EventLoop::handle_read() {
  uint64_t one = 1;
  ssize_t n = read(_wakeup_fd, &one, sizeof one);
  if (n != sizeof(one)) {
    net_logger()->error("{} reads {} bytes instead of 8", __FUNCTION__, n);
  }
}

void EventLoop::run_in_loop(Functor cb) {
  //
  // 在当前的loop线程中，
  // 直接执行
  if (is_in_loop_thread()) {
    cb();
  } else {  // 在非当前loop线程中执行cb，需要唤醒loop所在线程来执行cb
    queue_in_loop(std::move(cb));
  }
}

void EventLoop::queue_in_loop(Functor cb) {
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _pending_functors.push_back(std::move(cb));
  }

  // 唤醒相应的，需要执行上面回调操作的loop的线程
  // 或者
  // 在当前loop的中正在执行上一轮do_pending_functors()里的回调，
  // 且还没有执行完上一轮回调时 _calling_pending_functors = true，
  // 当上一轮do_pending_functors()执行完后，第二轮会阻塞在poll中，
  // 此时需要唤醒当前loop
  if (!is_in_loop_thread() || _calling_pending_functors) {
    wakeup();
  }
}

void EventLoop::wakeup() {
  uint64_t one = 1;
  ssize_t n = ::write(_wakeup_fd, &one, sizeof one);
  if (n != sizeof(one)) {
    net_logger()->error("{} writes {} bytes instead of 8", __FUNCTION__, n);
  }
}

void EventLoop::update_channel(Channel* channel) {
  _poller->update_channel(channel);
}

void EventLoop::remove_channel(Channel* channel) {
  _poller->remove_channel(channel);
}

bool EventLoop::has_channel(Channel* channel) {
  return _poller->has_channle(channel);
}

void EventLoop::do_pending_functors() {
  std::vector<Functor> functors;
  _calling_pending_functors = true;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    functors.swap(_pending_functors);
  }

  for (const Functor& functor : functors) {
    //
    // 当前loop执行的回调
    functor();
  }

  _calling_pending_functors = false;
}

/* EventLoop::ChannelPushResult EventLoop::enqueue_channel(ChannelPtr&& channel) { */
/* if (!channel) { */
/* throw std::invalid_argument("Channel cannot be null"); */
/* } */

/* if (channel->owner_loop() != this) { */
/* throw std::invalid_argument("Channel belongs to another EventLoop"); */
/* } */

/* if (channel->index() != -1 || channel->is_none_event()) { */
/* throw std::logic_error("Channel must be prepared and not registered"); */
/* } */

/* auto result = _pending_channels.try_push(std::move(channel)); */

/* if (result == ChannelPushResult::success) { */
// 先入队，再通知消费者。
/* wakeup(); */
/* } */

/* return result; */
/* } */

/* void EventLoop::consume_pending_channels() { */
/* assert(is_in_loop_thread()); */

/* auto channels = _pending_channels.take_batch(64); */

/* for (auto& channel : channels) { */
/* const int fd = channel->fd(); */

// 同一个 fd 只能对应一个有效 Channel。
/* auto [it, inserted] = _managed_channels.try_emplace(fd, channel); */

/* if (!inserted) { */
/* net_logger()->error("Channel already managed: fd={}", fd); */
/* continue; */
/* } */

// 先持有对象，再注册到 Poller。
/* it->second->register_in_loop(); */
/* } */

/* if (!_pending_channels.empty()) { */
// 本轮只消费了一批。
// 保证剩余元素不会等待下一次外部通知。
/* wakeup(); */
/* } */
/* } */

/* void EventLoop::release_channel(ChannelPtr channel) { */
/* if (!channel || channel->owner_loop() != this) { */
/* throw std::invalid_argument("Invalid Channel for this EventLoop"); */
/* } */

// 即使当前就在 loop 线程，也先排队。
// 注销操作将在本轮事件分发结束后执行。
/* queue_in_loop([this, channel = std::move(channel)] { */
/* auto it = _managed_channels.find(channel->fd()); */

/* if (it == _managed_channels.end() || it->second.get() != channel.get()) { */
/* return; */
/* } */

/* channel->disable_all(); */
/* channel->remove(); */

/* _managed_channels.erase(it); */
/* }); */
/* } */
