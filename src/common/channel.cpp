#include "common/channel.h"
#include "common/timestamp.h"
#include "logger/logger.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <memory>

namespace TLSS::NET {
const int Channel::_k_none_event = 0;
const int Channel::_k_read_event = EPOLLIN | EPOLLPRI;
const int Channel::_k_write_event = EPOLLOUT;

Channel::Channel(EventLoop* loop, int fd)
    : _loop(loop), _fd(fd), _revents(0), _index(-1), _tied(false) {}

Channel::~Channel() {}

// 该方法什么时候调用
void Channel::tie(const std::shared_ptr<void>& obj) {
  _tie = obj;
  _tied = true;
}

//
// 当改变channel所表示fd的events事件后，
// update负责在poller里面更改相应的事件 epoll_ctl
// EventLoop 包含了 ChannelList 和 Poller
void Channel::update() {
  // 通过channel所属的EventLoop,
  // 调用poller的相应方法，
  // 注册fd的events事件
}

//
// 在channel所属的EventLoop中，
// 把当前的channel删除掉
void Channel::remove() {}

//
// fd得到poller通知以后，
// 处理事件
void Channel::handle_event(TIME::Timestamp receive_time) {
    if(_tied){
        std::shared_ptr<void> guard = _tie.lock();
        if(guard){
            handle_event_with_guard(receive_time);
        }
    }else{
        handle_event_with_guard(receive_time);
    }
}

// 
// 更具poller通知的channel发生的具体事件，
// 由channel负责调用具体的回调操作
void Channel::handle_event_with_guard(TIME::Timestamp receive_time) {
    // LOG INFO "channel handle_event revents : %d"
    if((_revents & EPOLLHUP) && !(_revents & EPOLLIN)){
        if(_close_callback){
            _close_callback();
        }
    }

    if(_revents & EPOLLERR){
        if(_error_callback){
            _error_callback();
        }
    }

    if(_revents & (EPOLLIN | EPOLLPRI)){
        if(_read_callback){
            _read_callback(receive_time);
        }
    }

    if(_revents & EPOLLOUT){
        if(_write_callback){
            _write_callback();
        }
    }
}
}  // namespace TLSS::NET
