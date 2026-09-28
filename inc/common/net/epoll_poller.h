#ifndef __INC_COMMON_NET_EPOLL_POLLER_H__
#define __INC_COMMON_NET_EPOLL_POLLER_H__

#include "poller.h"
#include "timestamp.h"

#include <sys/epoll.h>
#include <vector>

namespace TLSS::NET {

class Channel;
class EventLoop;

//
// epoll的使用
// epoll_create
// epoll_ctl
// epoll_wait
class EpollPoller : public Poller {
 public:
  EpollPoller(EventLoop* loop);
  ~EpollPoller() override;

  TIME::Timestamp poll(int timeout_ms, ChannelList* active_channels) override;
  void update_channel(Channel* channel) override;
  void remove_channel(Channel* channel) override;

 private:
  static const int _k_init_event_list_size = 16;

  //
  // 填写活跃的链接
  void fill_active_channels(int num_events, ChannelList* active_channels) const;
  //
  // 更新channel通道
  void update(int operation, Channel* channel);

  using EventList = std::vector<epoll_event>;

  int _epoll_fd;
  EventList _events;
};
}  // namespace TLSS::NET

#endif  // __INC_COMMON_NET_EPOLL_POLLER_H__
