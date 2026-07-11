#ifndef __INC_COMMON_NET_H__
#define __INC_COMMON_NET_H__

#include "NoCopy.h"

#include <arpa/inet.h>   // inet_addr(), inet_pton(), inet_ntop()
#include <netinet/in.h>  // sockaddr_in, htons(), htonl()
#include <sys/epoll.h>
#include <sys/socket.h>  // socket(), bind(), listen(), accept(), connect()
#include <unistd.h>      // close()
#include <cstdint>
#include <string>

namespace TLSSNET {

enum class STATUS : std::int8_t {
  UNINITIALIZED = -1,
  SUCCESS = 0,
  TCPINIT_SOCKET_ERR = -100,
  TCPINIT_SETSOCKOPT_ERR = -101,
  TCPINIT_IP_ERR = -102,
  TCPINIT_BIND_ERR = -103,
  TCPINIT_LISTEN_ERR = -104,
  EPOLL_ADD_ERR = -105,
  EPOLL_DEL_ERR = -106
};

class Socket : public NoCopy {
 public:
  Socket();
  explicit Socket(int fd);
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  ~Socket();

  TLSSNET::STATUS status();

  [[nodiscard]] int get_fd() const noexcept;

 private:
  int _fd;
  TLSSNET::STATUS _status; 
};

class InetAddress {
 public:
  InetAddress(const std::string& ip, uint16_t port);
  InetAddress(const struct sockaddr_in& addr);
  InetAddress(const InetAddress&) = default;
  InetAddress& operator=(const InetAddress&) = default;
  InetAddress(InetAddress&&) noexcept = default;
  InetAddress& operator=(InetAddress&&) noexcept = default;

  ~InetAddress() = default;
  [[nodiscard]] std::string get_ip() const;
  [[nodiscard]] uint16_t get_port() const;
  TLSSNET::STATUS status();

  const struct sockaddr_in* get_int_address_ptr();

 private:
  struct sockaddr_in _addr;
  TLSSNET::STATUS _status;
};

STATUS tcp_init(const std::string& ip, uint16_t port, int& sockfd);
STATUS epoll_add(int epfd, int fd);
STATUS epoll_del(int epfd, int fd);
}  // namespace TLSSNET

#endif  // __INC_COMMON_NET_H__
