#ifndef __INC_COMMON_NET_SOCKET_H__
#define __INC_COMMON_NET_SOCKET_H__

#include <cmath>
#include "common/NoCopy.h"
namespace TLSS::NET {
class InetAddress;
class Socket : NoCopy {
 public:
  explicit Socket(int sockfd)
      : _sockfd(sockfd) {}
  ~Socket();

  int fd() const {
      return _sockfd;
  }

  void bind_address(const InetAddress &local_addr);
  void listen();
  int accept(InetAddress *peer_addr);

  void shutdown_write();
  
  void set_tcp_no_delay(bool on);
  void set_reuse_addr(bool on);
  void set_reuse_port(bool on);
  void set_keep_alive(bool on);

 private:
  const int _sockfd;
};
}  // namespace TLSS::NET

#endif  // __INC_COMMON_NET_SOCKET_H__
