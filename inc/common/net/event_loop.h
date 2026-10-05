#ifndef __INC_COMMON_EVENT_LOOP_H__
#define __INC_COMMON_EVENT_LOOP_H__

#include "common/NoCopy.h"
#include "common/base/bounded_queue.h"
#include "common/base/current_thread.h"
#include "common/net/timestamp.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace TLSS::NET {
class Channel;
class Poller;
class EventLoop : NoCopy {
 public:
  using Functor = std::function<void()>;
  /* using ChannelPtr = std::shared_ptr<Channel>; */
  /* using ChannelQueue = BASE::BoundedQueue<ChannelPtr>; */
  /* using ChannelPushResult = ChannelQueue::PushResult; */

  EventLoop();
  ~EventLoop();

  //
  // 开启事件循环
  void loop();
  //
  // 退出事件循环
  void quit();
  TIME::Timestamp poll_return_time() const {
    return _poll_return_time;
  }

  //
  // 成功时接管 channel；失败时调用者仍持有它。
  /* ChannelPushResult enqueue_channel(ChannelPtr&& channel); */

  // 请求注销已被本 loop 接管的 Channel。
  /* void release_channel(ChannelPtr channel); */

  //
  // 在当前loop中执行cb
  void run_in_loop(Functor cb);
  //
  // 把cb放入队列中，唤醒loop所在的线程，执行cb
  void queue_in_loop(Functor cb);

  //
  // 用来唤醒loop所在的线程
  void wakeup();

  // 调用poller的方法
  void update_channel(Channel* channel);
  void remove_channel(Channel* channel);
  bool has_channel(Channel* channel);

  //
  // 判断eventloop对象是否在自己的线程里面
  bool is_in_loop_thread() const {
    return _thread_id == BASE::CurrentThread::tid();
  }

 private:
  //
  // wakeup
  void handle_read();
  //
  // 执行回调
  void do_pending_functors();

  /* void consume_pending_channels(); */

  // 待消费的 Channel，允许跨线程入队。
  /* ChannelQueue _pending_channels{1024}; */

  // 已消费的 Channel，只允许所属 loop 线程操作。
  /* std::unordered_map<int, ChannelPtr> _managed_channels; */

  using ChannelList = std::vector<Channel*>;
  std::atomic<bool> _looping;
  std::atomic<bool> _quit;
  const pid_t _thread_id;                       // 记录当前loop所在线程的id
  TIME::Timestamp _poll_return_time;            // poller 返回发生事件的channels的时间点
  std::unique_ptr<Poller> _poller;

  //
  // 这个获取方式一般选择用eventfd()这个函数获取
  // 还有一种常见的获取方式socketpair()，获取两个可读可写的fd，与pipe有区别
  int _wakeup_fd;  // 主要作用， 当mainLoop获取一个新用户的channel,通过轮询算法选择一个subloop
  std::unique_ptr<Channel> _wakeup_channel;

  ChannelList _active_channels;
  Channel* _current_active_channel;

  mutable std::mutex _mutex;               // 保护下列变量在线程中来安全操作一致性
  std::atomic<bool> _calling_pending_functors;  // 标识当前loop是否有需要执行的回调操作
  std::vector<Functor> _pending_functors;  // 存储loop需要执行的所有回调操作
};
}  // namespace TLSS::NET

#endif  // __INC_COMMON_EVENT_LOOP_H__
