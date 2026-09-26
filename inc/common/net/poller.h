#ifndef __INC_COMMON_POLLER_H__
#define __INC_COMMON_POLLER_H__

#include "common/NoCopy.h"

#include <csignal>
#include <functional>
#include <unordered_map>
#include <vector>
namespace TLSS::NET{

class Channel;
class EventLoop;

//
// 多路事件分发器的核心IO复用模块
class Poller : NoCopy{
public:
    using ChannelList = std::vector<Channel*>;

    Poller(EventLoop *loop);
    virtual ~Poller();

protected:
    using ChannelMap = std::unordered_map<int, Channel*>;
    ChannelMap _channels;
private:
    EventLoop *owner_loop; // 定义Poller所属的事件循环 EventLoop

};
}

#endif // __INC_COMMON_POLLER_H__
