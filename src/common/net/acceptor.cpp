#include "common/net/acceptor.h"
#include "common/net/inet_address.h"
#include "common/net/net_logger.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <cerrno>
#include <cstdlib>
namespace TLSS::NET {
namespace {
int create_nonblocking() {
  int sockfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
  if (sockfd < 0) {
    net_logger()->critical("{}:{}:{} acceptor::create nonblocking", __FILE__, __FUNCTION__,
                           __LINE__);
    std::abort();
  }
  return sockfd;
}
}  // namespace

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listen_addr, bool reuse_port)
    : _loop(loop)
    , _accept_socket(create_nonblocking())
    , _accept_channel(loop, _accept_socket.fd())
    , _listenning(false) {
  _accept_socket.set_reuse_addr(true);
  _accept_socket.set_reuse_port(reuse_port);
  _accept_socket.bind_address(listen_addr);
  // 在之后，如果有新用户连接，
  // 要执行一个回调，将connfd打包成一个channel，
  // 将channel递交给sub loop来监听
  _accept_channel.set_read_cb(std::bind(&Acceptor::handle_read, this));
}

Acceptor::~Acceptor() {
    _accept_channel.disable_all();
    _accept_channel.remove();
}

//
// listen fd有事件发生，有新用户连接
void Acceptor::handle_read() {
  InetAddress peer_addr;
  int connfd = _accept_socket.accept(&peer_addr);
  if (connfd >= 0) {
    if (_new_connection_cb) {
        //
        // 轮询获取subloop,
        // 唤醒，分发
        // 当前的新客户端的channel
      _new_connection_cb(connfd, peer_addr);
    } else {
      ::close(connfd);
    }
  } else {
    net_logger()->error("{}:{}:{} accept err:{}", __FILE__, __FUNCTION__, __LINE__, errno);
    if (errno == EMFILE) {
      // 当前进程没有可用的fd没有可用的fd在分配
      // 先输出错误日志
      // 后续在进行处理
      // 调整当前进程文件描述符的上限
      // TODO
      net_logger()->error("{}:{}:{} sockfd reached limit err:{}", __FILE__, __FUNCTION__, __LINE__);
    }
  }
}

void Acceptor::listen(){
    _listenning = true;
    _accept_socket.listen();
    // 唤醒poller
    _accept_channel.enable_reading();
}
}  // namespace TLSS::NET
