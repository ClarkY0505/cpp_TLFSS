#include "common/net.h"
#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <sys/socket.h>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "common/common.h"
#include "logger/logger.h"
#include "spdlog/common.h"
/*
 * @brief               Init Tcp socket
 *
 * Create a socket via the input IP address and port 
 *
 * @param[in]   ip      The IP address that needs to be bound, such "192.168.1.1"
 * @param[in]   port    The port number that needs to be bound, such "12345"
 * @param[out]  sockfd  The sockfd number is return value
 *
 * @return      success SUCCESS     
 *              fail    TCPINIT_BIND_ERR or TCPINIT_LISTEN_ERR
 * */
TLSSNET::STATUS TLSSNET::tcp_init(const std::string & ip, uint16_t port, int & sockfd){
    // Set the path of log file
    auto net_log= Logger::get("net");

    // Initiage TCP initialization
    sockfd = socket(AF_INET, SOCK_STREAM,0);
    if (sockfd < 0){
        int err = errno;
        std::string msg = std::string("[TCPINIT_SOCKET_ERR] socket failed, errno = ") +
            std::to_string(err) + ", error = " +
            std::strerror(err);

        net_log->error(msg);
        return TLSSNET::STATUS::TCPINIT_SOCKET_ERR;
    }

    int flag = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0){
        int err = errno;
        std::string msg = std::string("[TCPINIT_SETSOCKOPT_ERR] setsockopt failed, errno = ") +
            std::to_string(err) + ", error = " +
            std::strerror(err);

        net_log->error(msg);
        close(sockfd);
        sockfd = -1;
        return TLSSNET::STATUS::TCPINIT_SETSOCKOPT_ERR;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    /* addr.sin_addr.s_addr = inet_addr(ip.c_str()); */
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0){
        int err = errno;
        std::string msg = std::string("[TCPINIT_IP_ERR] invalid ip, ip = ") + ip +
            ", errno = " + std::to_string(err) +
            ", error = " + std::strerror(err);

        net_log->error(msg);
        close(sockfd);
        sockfd = -1;
        return TLSSNET::STATUS::TCPINIT_IP_ERR;
    }

    if (bind(sockfd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0){
        int err = errno;
        std::string msg = std::string("[TCPINIT_BIND_ERR] bind failed, errno = ") +
            std::to_string(err) + ", error = " +
            std::strerror(err);
        net_log->error(msg);
        close(sockfd);
        sockfd = -1;
        return TLSSNET::STATUS::TCPINIT_BIND_ERR;
    }

    if (listen(sockfd, 10) < 0){
        int err = errno;
        std::string msg = std::string("[TCPINIT_LISTEN_ERR] listen failed, errno = ") +
            std::to_string(err) + ", error = " +
            std::strerror(err);
        net_log->error(msg);
        close(sockfd);
        sockfd = -1;
        return TLSSNET::STATUS::TCPINIT_LISTEN_ERR;
    }

    return TLSSNET::STATUS::SUCCESS;
}

/*
 * @brief               Add a fd to epoll listen
 *
 * @param[in]   epfd    The epoll fd
 * @param[in]   fd      The fd that needs to be listen by epoll
 *
 * @return      success SUCCESS 
 *              fail    EPOLL_ADD_ERR 
 * */

TLSSNET::STATUS TLSSNET::epoll_add(int epfd, int fd){
    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1){
        return TLSSNET::STATUS::EPOLL_ADD_ERR;
    }

    return TLSSNET::STATUS::SUCCESS;
}

/*
 * @brief               Add a fd to epoll listen
 *
 * @param[in]   epfd    The epoll fd
 * @param[in]   fd      The fd that needs to be listen by epoll
 *
 * @return      success SUCCESS 
 *              fail    EPOLL_DEL_ERR 
 * */
TLSSNET::STATUS TLSSNET::epoll_del(int epfd, int fd){
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr) == -1){
        return TLSSNET::STATUS::EPOLL_DEL_ERR;
    }

    return TLSSNET::STATUS::SUCCESS;
}
