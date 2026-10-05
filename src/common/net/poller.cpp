#include "common/net/channel.h"
#include "common/net/event_loop.h"
#include "common/net/net_logger.h"
#include "common/net/poller.h"

namespace TLSS::NET {
Poller::Poller(EventLoop* loop) : _owner_loop(loop) {}

Poller::~Poller() = default;

bool Poller::has_channle(Channel* channel) const {
  ChannelMap::const_iterator it = _channels.find(channel->fd());
  if (it == _channels.end()) {
    net_logger()->critical("epoll returned unknown channel: fd={}, channel={}", channel->fd(),
                           static_cast<const void*>(channel));

    assert(false);
    return false;
  }

  if (it->second != channel) {
    net_logger()->critical("epoll channel mismatch: fd={}, returned={}, registered={}",
                           channel->fd(), static_cast<const void*>(channel),
                           static_cast<const void*>(it->second));

    assert(false);
    return false;
  }
  return it != _channels.end() && it->second == channel;
}
}  // namespace TLSS::NET
