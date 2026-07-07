#ifndef __INC_COMMON_NET_H__
#define __INC_COMMON_NET_H__

#include <string>
#include <sys/socket.h> // socket(), bind(), listen(), accept(), connect()
#include <netinet/in.h> // sockaddr_in, htons(), htonl()
#include <arpa/inet.h>  // inet_addr(), inet_pton(), inet_ntop()
#include <unistd.h>     // close()
#include <sys/epoll.h>

namespace TLSSNET{
enum STATUS{
    SUCCESS,
    TCPINIT_SOCKET_ERR = 100,
    TCPINIT_SETSOCKOPT_ERR,
    TCPINIT_IP_ERR,
    TCPINIT_BIND_ERR,
    TCPINIT_LISTEN_ERR,
    EPOLL_ADD_ERR,
    EPOLL_DEL_ERR
};

STATUS tcp_init(const std::string & ip, uint16_t port, int & sockfd);
STATUS epoll_add(int epfd, int fd);
STATUS epoll_del(int epfd, int fd);
}

#endif // __INC_COMMON_NET_H__
