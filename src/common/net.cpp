#include "common/net.h"
#include "logger/logger.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <utility>

/*
 * @brief Create an IPv4 TCP socket and take ownership of its file descriptor.
 * */
TLSSNET::Socket::Socket()
    : _fd{::socket(AF_INET, SOCK_STREAM, 0)}, _status{TLSSNET::STATUS::UNINITIALIZED} {
  // Set the path of log file
  auto net_log = Logger::get("net");
  if (_fd < 0) {
    _status = TLSSNET::STATUS::TCPINIT_SOCKET_ERR;
    int err = errno;
    std::string msg = std::string("[TCPINIT_SOCKET_ERR] socket failed, errno = ") +
                      std::to_string(err) + ", error = " + std::strerror(err);

    net_log->error(msg);
    return;
  }
}

/*
 * @brief Take ownership of an existing socket file descriptor.
 *
 * @param[in] fd The socket file descriptor to manage.
 * */
TLSSNET::Socket::Socket(int fd) : _fd(fd), _status{TLSSNET::STATUS::SUCCESS} {}

/*
 * @brief Move-construct a socket and transfer ownership of its descriptor.
 *
 * @param[in,out] other The socket whose descriptor is transferred.
 * */
TLSSNET::Socket::Socket(TLSSNET::Socket&& other) noexcept
    : _fd(std::exchange(other._fd, -1)), _status{TLSSNET::STATUS::SUCCESS} {}

/*
 * @brief Move-assign a socket and transfer ownership of its descriptor.
 *
 * @param[in,out] other The socket whose descriptor is transferred.
 *
 * @return A reference to this socket.
 * */
TLSSNET::Socket& TLSSNET::Socket::operator=(TLSSNET::Socket&& other) noexcept {
  if (this != &other) {
    if (_fd > 0) {
      ::close(_fd);
    }
    std::exchange(other._fd, -1);
  }

  return *this;
}

/*
 * @brief Close the managed socket file descriptor.
 * */
TLSSNET::Socket::~Socket() {
  close(_fd);
}

/*
 *  @brief Get an existing socket file descriptor
 *
 *  @return int     an exsting socket file descriptor
 * */
int TLSSNET::Socket::get_fd() const noexcept {
  return _fd;
}

/*
 * @brief Get the current socket status code.
 *
 * @return A status code from socket status
 * */
TLSSNET::STATUS TLSSNET::Socket::status() {
  return _status;
}

/*
 * @brief Build an IPv4 socket address from an IP address and port.
 *
 * @param[in] ip The IPv4 address in presentation format.
 * @param[in] port The host-order port number.
 * */
TLSSNET::InetAddress::InetAddress(const std::string& ip, uint16_t port)
    : _addr{}, _status{TLSSNET::STATUS::UNINITIALIZED} {
  auto net_log = Logger::get("net");
  memset(&_addr, 0, sizeof(_addr));
  _addr.sin_family = AF_INET;
  _addr.sin_port = htons(port);
  /* addr.sin_addr.s_addr = inet_addr(ip.c_str()); */
  if (inet_pton(AF_INET, ip.c_str(), &_addr.sin_addr) <= 0) {
    int err = errno;
    _status = TLSSNET::STATUS::TCPINIT_IP_ERR;
    std::string msg = std::string("[TCPINIT_IP_ERR] invalid ip, ip = ") + ip +
                      ", errno = " + std::to_string(err) + ", error = " + std::strerror(err);
    net_log->error(msg);
    return;
  }
}

/*
 * @brief Copy an existing IPv4 socket address.
 *
 * @param[in] addr The socket address to copy.
 * */
TLSSNET::InetAddress::InetAddress(const struct sockaddr_in& addr)
    : _addr(addr), _status{TLSSNET::STATUS::SUCCESS} {}

/*
 * @brief Get an existing ip from IPv4 socket address.
 *
 * @return string   A ip from IPv4 socket address.
 * */
std::string TLSSNET::InetAddress::get_ip() const {
  return std::string{inet_ntoa(_addr.sin_addr)};
}

/*
 * @brief Get an existing port from IPv4 socket address.
 *
 * @return uint16_t A port from IPv4 socket address.
 * */
uint16_t TLSSNET::InetAddress::get_port() const {
  return ntohs(_addr.sin_port);
}

/*
 * @brief Get a pointer to the underlying IPv4 socket address.
 *
 * The returned pointer remains valid only while this InetAddress object exists.
 *
 * @return A read-only pointer to the stored sockaddr_in structure.
 * */
const struct sockaddr_in* TLSSNET::InetAddress::get_int_address_ptr() {
  return &_addr;
}

/*
 * @brief               Init Tcp socket
 *
 * Create a socket via the input IP address and port
 *
 * @param[in]   ip      The IP address that needs to be bound, such "192.168.1.1"
 * @param[in]   port    The port number that needs to be bound, such "12345"
 * @param[out]  sockfd  The sockfd number is return value
 *
 * @return      STATUS  SUCCESS
 *                      TCPINIT_BIND_ERR or TCPINIT_LISTEN_ERR
 * */
TLSSNET::STATUS TLSSNET::tcp_init(const std::string& ip, uint16_t port, int& sockfd) {
  // Set the path of log file
  auto net_log = Logger::get("net");

  // Initiage TCP initialization
  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    int err = errno;
    std::string msg = std::string("[TCPINIT_SOCKET_ERR] socket failed, errno = ") +
                      std::to_string(err) + ", error = " + std::strerror(err);

    net_log->error(msg);
    return TLSSNET::STATUS::TCPINIT_SOCKET_ERR;
  }

  int flag = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0) {
    int err = errno;
    std::string msg = std::string("[TCPINIT_SETSOCKOPT_ERR] setsockopt failed, errno = ") +
                      std::to_string(err) + ", error = " + std::strerror(err);

    net_log->error(msg);
    close(sockfd);
    sockfd = -1;
    return TLSSNET::STATUS::TCPINIT_SETSOCKOPT_ERR;
  }

  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  /* addr.sin_addr.s_addr = inet_addr(ip.c_str()); */
  if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
    int err = errno;
    std::string msg = std::string("[TCPINIT_IP_ERR] invalid ip, ip = ") + ip +
                      ", errno = " + std::to_string(err) + ", error = " + std::strerror(err);

    net_log->error(msg);
    close(sockfd);
    sockfd = -1;
    return TLSSNET::STATUS::TCPINIT_IP_ERR;
  }

  if (bind(sockfd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
    int err = errno;
    std::string msg = std::string("[TCPINIT_BIND_ERR] bind failed, errno = ") +
                      std::to_string(err) + ", error = " + std::strerror(err);
    net_log->error(msg);
    close(sockfd);
    sockfd = -1;
    return TLSSNET::STATUS::TCPINIT_BIND_ERR;
  }

  if (listen(sockfd, 10) < 0) {
    int err = errno;
    std::string msg = std::string("[TCPINIT_LISTEN_ERR] listen failed, errno = ") +
                      std::to_string(err) + ", error = " + std::strerror(err);
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

TLSSNET::STATUS TLSSNET::epoll_add(int epfd, int fd) {
  struct epoll_event ev {};
  ev.events = EPOLLIN;
  ev.data.fd = fd;

  if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
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
TLSSNET::STATUS TLSSNET::epoll_del(int epfd, int fd) {
  if (epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr) == -1) {
    return TLSSNET::STATUS::EPOLL_DEL_ERR;
  }

  return TLSSNET::STATUS::SUCCESS;
}
