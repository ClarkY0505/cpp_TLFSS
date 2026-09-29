#include "common/net/channel.h"
#include "common/net/epoll_poller.h"
#include "common/net/net_logger.h"
#include "common/net/poller.h"
#include "common/net/timestamp.h"
#include "common/utile/utile.h"

#include <errno.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
namespace TLSS::NET {
namespace {
//
// 这里三个参数队员channel中的状态
// channel 未添加到poller中
const int k_new = -1;
// channel 已添加到poller中
const int k_added = 1;
// channel 从poller中删除
const int k_deleted = 2;
}  // namespace

EpollPoller::EpollPoller(EventLoop* loop)
    : Poller(loop), _epoll_fd(::epoll_create1(EPOLL_CLOEXEC)), _events(_k_init_event_list_size) {
  if (_epoll_fd < 0) {
    const int saved_err = errno;
    net_logger()->critical("epoll_create1 error={}, reason={}", saved_err,
                        std::error_code(saved_err, std::generic_category()).message());
    net_logger()->flush();
    std::abort();
  }
}

EpollPoller::~EpollPoller() {
  ::close(_epoll_fd);
}

// epoll_wait
TIME::Timestamp EpollPoller::poll(int timeout_ms, ChannelList* active_channels) {
  net_logger()->trace("func={} , fd total count {} ", __FUNCTION__, _channels.size());
  // _events是一个vector数组，在epoll_wait中需要传入这个容器的首个元素的地址
  /* int num_events = ::epoll_wait(_epoll_fd,
   * &*_events.begin(),static_cast<int>(_events.size()),timeout_ms); */
  // 这个等同于上面写法
  int num_events =
      ::epoll_wait(_epoll_fd, _events.data(), static_cast<int>(_events.size()), timeout_ms);
  int saved_err = errno;
  TIME::Timestamp now(TIME::Timestamp::now());
  if (num_events > 0) {
    net_logger()->trace("{} events happened", num_events);
    fill_active_channels(num_events, active_channels);
    // 进行扩容
    if (UTIL::implicit_cast<size_t>(num_events) == _events.size()) {
      _events.resize(_events.size() * 2);
    }
  } else if (num_events == 0) {
    net_logger()->trace("nothing happened");
  } else {
    if (saved_err != EINTR) {
      errno = saved_err;
      net_logger()->error("epoll_wait failed, func={}, errno={}, error={}", __FUNCTION__, saved_err,
                          std::strerror(saved_err));
    }
  }
  return now;
}

void EpollPoller::fill_active_channels(int num_events, ChannelList* active_channels) const {
  for (int i = 0; i < num_events; ++i) {
    Channel* channel = static_cast<Channel*>(_events[i].data.ptr);
    int fd = channel->fd();
    ChannelMap::const_iterator it = _channels.find(fd);
    if (it == _channels.end()) {
      net_logger()->critical("epoll returned unknown channel: fd={}, channel={}", fd,
                             static_cast<const void*>(channel));

      assert(false);
      continue;
    }

    if (it->second != channel) {
      net_logger()->critical("epoll channel mismatch: fd={}, returned={}, registered={}", fd,
                             static_cast<const void*>(channel),
                             static_cast<const void*>(it->second));

      assert(false);

      continue;
    }
    channel->set_revents(_events[i].events);
    // 此时EventLoop就拿到了对应的事件
    active_channels->push_back(channel);
  }
}

void EpollPoller::update(int operation, Channel* channel) {
  struct epoll_event event;
  UTIL::memzero(&event, sizeof event);
  event.events = channel->events();
  event.data.ptr = channel;
  int fd = channel->fd();
  net_logger()->trace("epoll_ctl op={} , fd ={}, event={}", operation, fd, channel->events());
  if (::epoll_ctl(_epoll_fd, operation, fd, &event) < 0) {
    if (operation == EPOLL_CTL_DEL) {
      net_logger()->error("epoll_ctl func={}, op={}, fd ={}", __FUNCTION__, operation, fd);
    } else {
      net_logger()->critical("epoll_ctl func={}, op={}, fd ={}", __FUNCTION__, operation, fd);
      net_logger()->flush();
      std::abort();
    }
  }
}

//
// EventLoop包含
// ChannelList    Poller
//              ChannelMap <fd, channel*>
void EpollPoller::update_channel(Channel* channel) {
  const int index = channel->index();
  net_logger()->info("func={}, fd={}, event={}, index={}", __FUNCTION__, channel->fd(),
                     channel->events(), index);
  if (index == k_new || index == k_deleted) {
    int fd = channel->fd();
    if (index == k_new) {
      _channels[fd] = channel;
    } else {  // index == k_deleted
      assert(_channels.find(fd) != _channels.end());
      assert(_channels[fd] == channel);
    }

    channel->set_index(k_added);
    update(EPOLL_CTL_ADD, channel);
  } else {  // channel已经在poller上注册过了
    if (channel->is_none_event()) {
      update(EPOLL_CTL_DEL, channel);
      channel->set_index(k_deleted);
    } else {
      update(EPOLL_CTL_MOD, channel);
    }
  }
}

void EpollPoller::remove_channel(Channel* channel) {
  int fd = channel->fd();
  _channels.erase(fd);

  int index = channel->index();
  if (index == k_added) {
    update(EPOLL_CTL_DEL, channel);
  }
  channel->set_index(k_new);
}
}  // namespace TLSS::NET
