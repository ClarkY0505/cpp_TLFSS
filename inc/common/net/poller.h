#ifndef __INC_COMMON_POLLER_H__
#define __INC_COMMON_POLLER_H__

#include "common/NoCopy.h"
#include "common/net/timestamp.h"

#include <csignal>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>
namespace TLSS::NET {

class Channel;
class EventLoop;

//
// 多路事件分发器的核心IO复用模块
class Poller : NoCopy {
 public:
  using ChannelList = std::vector<Channel*>;

  Poller(EventLoop* loop);
  virtual ~Poller();

  //
  // 给所有IO复用保留统一的接口
  virtual TIME::Timestamp poll(int timeouts, ChannelList* active_channles) = 0;
  virtual void update_channel(Channel* channel) = 0;
  virtual void remove_channel(Channel* channel) = 0;

  //
  // 判断channel是否在当前poller当中
  bool has_channle(Channel* channel) const;

  //
  // EventLoop 可以通过该接口该获取默认的IO复用的具体实现
  // 为什么选择给这个函数提供一个单独的中实现文件
  // 如果以后要扩展PollPoller EpollPoller这两个方法
  // 就需要在poller.cpp中添加对应的头文件
  // 由于Poller是基类
  // 一般情况是派生类引用基类
  // 而不是基类去引用派生类
  /* static Poller* new_default_poller(EventLoop* loop); */
  static std::unique_ptr<Poller> new_default_poller(EventLoop* loop);

 protected:
  //
  // map<key,value>
  using ChannelMap = std::unordered_map<int, Channel*>;
  ChannelMap _channels;

 private:
  EventLoop* _owner_loop;  // 定义Poller所属的事件循环 EventLoop
};
}  // namespace TLSS::NET

#endif  // __INC_COMMON_POLLER_H__
