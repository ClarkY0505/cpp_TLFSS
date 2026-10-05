#ifndef __INC_COMMON_NET_ACCEPTOR_H__
#define __INC_COMMON_NET_ACCEPTOR_H__

#include <functional>
#include "common/NoCopy.h"
#include "common/net/channel.h"
#include "common/net/socket.h"
namespace TLSS::NET {
class EventLoop;
class InetAddress;
class Acceptor : NoCopy {
 public:
  using NewConnectionCallback = std::function<void(int sockfd, const InetAddress&)>;
  Acceptor(EventLoop* loop, const InetAddress& listen_addr, bool reuse_port);
  ~Acceptor();

  void set_new_connection_callback(const NewConnectionCallback& cb) {
    _new_connection_cb = cb;
  }
  void listen();

  bool listenning() const {
    return _listenning;
  }

 private:

  void handle_read();
  //
  // acceptor 用的是用户定义的baseloop，
  // 在reactor模型中也就是main loop
  EventLoop* _loop;
  Socket _accept_socket;
  Channel _accept_channel;
  NewConnectionCallback _new_connection_cb;
  bool _listenning;
};
}  // namespace TLSS::NET

#endif  // __INC_COMMON_NET_ACCEPTOR_H__
