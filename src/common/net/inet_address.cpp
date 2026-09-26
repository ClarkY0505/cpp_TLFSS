#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "common/net/inet_address.h"
namespace TLSS::NET {
InetAddress::InetAddress(uint16_t port, std::string ip) : _addr{} {
  memset(&_addr, 0, sizeof _addr);
  _addr.sin_family = AF_INET;
  _addr.sin_port = htons(port);
  _addr.sin_addr.s_addr = inet_addr(ip.c_str());
}

std::string InetAddress::to_ip() const{
    char buffer[64] = {0};
    ::inet_ntop(AF_INET, &_addr.sin_addr, buffer, sizeof buffer);
    return buffer;
}

std::string InetAddress::to_ip_port() const{
    char buffer[64] = {0};
    ::inet_ntop(AF_INET, &_addr.sin_addr, buffer, sizeof buffer);
    size_t end = strlen(buffer);
    uint16_t port = ntohs(_addr.sin_port);
    snprintf(buffer + end, sizeof(buffer) - end, ":%u", static_cast<unsigned int>(port));
    return buffer;
}

uint16_t InetAddress::to_port() const{
    return ntohs(_addr.sin_port);
}
}  // namespace TLSS::NET
