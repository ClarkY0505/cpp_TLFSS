#include "common/net/inet_address.h"
#include "common/net/net_logger.h"
#include "common/net/socket.h"
#include "common/utile/utile.h"

/* #include <asm-generic/socket.h> */
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <cstdlib>

namespace TLSS::NET {
Socket::~Socket() {
  ::close(_sockfd);
}

void Socket::bind_address(const InetAddress& local_addr) {
  int ret =
      ::bind(_sockfd, local_addr.get_sock_addr(), static_cast<socklen_t>(sizeof(sockaddr_in)));
  if (ret < 0) {
    net_logger()->critical("bind sockfd:{} failed.", _sockfd);
    std::abort();
  }
}

void Socket::listen() {
  int ret = ::listen(_sockfd, 1024);
  if (ret < 0) {
    net_logger()->critical("listen sockfd:{} fail", _sockfd);
    std::abort();
  }
}

int Socket::accept(InetAddress* peer_addr) {
  sockaddr_in addr;
  socklen_t len = static_cast<socklen_t>(sizeof addr);
  UTIL::memzero(&addr, sizeof addr);
  int connfd = ::accept(_sockfd, reinterpret_cast<sockaddr*>(&addr), &len);
  if (connfd >= 0) {
    peer_addr->set_sock_addr(addr);
  }

  return connfd;
}

//
// 简单的关闭写端
// 先存在这个功能
// 后期完成屏障机制再来补充细节
// TODO
void Socket::shutdown_write() {
  if (::shutdown(_sockfd, SHUT_WR) < 0) {
    net_logger()->critical("shutdown write: error");
    std::abort();
  }
}

//
// 禁用 Nagle 算法，减少其导致的小数据发送等待
void Socket::set_tcp_no_delay(bool on) {
  int opt = on ? 1 : 0;
  ::setsockopt(_sockfd, IPPROTO_TCP, TCP_NODELAY, &opt, static_cast<socklen_t>(sizeof opt));
}

void Socket::set_reuse_addr(bool on) {
  int opt = on ? 1 : 0;
  ::setsockopt(_sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, static_cast<socklen_t>(sizeof opt));
}

void Socket::set_reuse_port(bool on) {
  int opt = on ? 1 : 0;
  ::setsockopt(_sockfd, SOL_SOCKET, SO_REUSEPORT, &opt, static_cast<socklen_t>(sizeof opt));
}

void Socket::set_keep_alive(bool on) {
  int opt = on ? 1 : 0;
  ::setsockopt(_sockfd, SOL_SOCKET, SO_KEEPALIVE, &opt, static_cast<socklen_t>(sizeof opt));
}
}  // namespace TLSS::NET
