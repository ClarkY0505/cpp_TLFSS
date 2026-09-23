#ifndef __INC_COMMON_INET_ADDRESS_H__
#define __INC_COMMON_INET_ADDRESS_H__

#include <netinet/in.h>
#include <sys/socket.h>
#include <cstdint>
#include <string>
namespace TLSS::NET {
class InetAddress {
 public:
  explicit InetAddress(uint16_t port, std::string ip = "127.0.0.1");
  explicit InetAddress(const sockaddr_in& addr) : _addr(addr) {}

  [[nodiscard]] std::string to_ip() const;
  [[nodiscard]] std::string to_ip_port() const;
  [[nodiscard]] uint16_t to_port() const;
  [[nodiscard]] const sockaddr* get_sock_addr() const {
    return reinterpret_cast<const sockaddr*>(&_addr);
  };

 private:
  sockaddr_in _addr;
};
}  // namespace TLSSNET
#endif  // __INC_COMMON_INET_ADDRESS_H__
