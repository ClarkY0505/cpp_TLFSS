#include "monitor_wire.h"
#include "udp_receiver.h"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <iterator>
#include <netinet/in.h>
#include <optional>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace TLSSMON {
namespace Wire {

struct UdpReceiver::Impl final {
  int _socket{-1};
  UdpReceiveStatus _setup_status{UdpReceiveStatus::SOCKET_ERROR};
  int _setup_error{0};
  std::uint16_t _bound_port{0};
};

UdpReceiver::UdpReceiver(UdpReceiverConfig config)
    : _config(std::move(config)), _impl(std::make_unique<Impl>()) {
  if (_config._bind_address.empty()) {
    _impl->_setup_status = UdpReceiveStatus::INVALID_ENDPOINT;
    return;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(_config._bind_port);

  const int address_result =
      ::inet_pton(AF_INET, _config._bind_address.c_str(), &address.sin_addr);
  if (address_result != 1) {
    _impl->_setup_status = UdpReceiveStatus::INVALID_ENDPOINT;

    if (address_result < 0) {
      _impl->_setup_error = errno;
    }

    return;
  }
  const int socket_fd = ::socket(AF_INET, SOCK_DGRAM, 0);

  if (socket_fd < 0) {
    _impl->_setup_status = UdpReceiveStatus::SOCKET_ERROR;
    _impl->_setup_error = errno;
    return;
  }

  /*
   * 接收socket必须是非阻塞的
   * 在AioManager中使用的水平触发select()
   * 如果异步回调被重复激活
   * 最后一次回调执行时数据可能已经被前一次回调读完
   * 非阻塞recvfrom()返回EAGAIN, 而不是永久阻塞worker
   **/
  const int status_flags = ::fcntl(socket_fd, F_GETFL, 0);

  if (status_flags < 0 ||
      ::fcntl(socket_fd, F_SETFL, status_flags | O_NONBLOCK) < 0) {
    const int setup_error = errno;
    ::close(socket_fd);
    _impl->_setup_status = UdpReceiveStatus::SOCKET_ERROR;
    _impl->_setup_error = setup_error;
    return;
  }
  /*
   * 防止 exec 后子进程继承 socket。
   */
  const int descriptor_flags = ::fcntl(socket_fd, F_GETFD, 0);

  if (descriptor_flags < 0 ||
      ::fcntl(socket_fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
    const int setup_error = errno;
    (void)::close(socket_fd);

    _impl->_setup_status = UdpReceiveStatus::SOCKET_ERROR;
    _impl->_setup_error = setup_error;
    return;
  }

  const int bind_result =
      ::bind(socket_fd, reinterpret_cast<const sockaddr *>(&address),
             static_cast<socklen_t>(sizeof(address)));
  if (bind_result < 0) {
    const int setup_error = errno;
    ::close(socket_fd);

    _impl->_setup_status = UdpReceiveStatus::SOCKET_ERROR;
    _impl->_setup_error = setup_error;
    return;
  }
  /*
   * 如果配置端口是 0，通过 getsockname() 查询系统分配的端口。
   */
  socklen_t address_size = static_cast<socklen_t>(sizeof(address));
  const int name_result = ::getsockname(
      socket_fd, reinterpret_cast<sockaddr *>(&address), &address_size);

  if (name_result < 0) {
    const int setup_error = errno;
    ::close(socket_fd);
    _impl->_setup_status = UdpReceiveStatus::SOCKET_ERROR;
    _impl->_setup_error = setup_error;
    return;
  }
  _impl->_socket = socket_fd;
  _impl->_bound_port = ntohs(address.sin_port);
  _impl->_setup_status = UdpReceiveStatus::SUCCESS;
}

UdpReceiver::~UdpReceiver() {
  if (_impl && _impl->_socket >= 0) {
    ::close(_impl->_socket);
    _impl->_socket = -1;
  }
}

bool UdpReceiver::ready() const noexcept {
  return _impl && _impl->_socket >= 0 &&
         _impl->_setup_status == UdpReceiveStatus::SUCCESS;
}

UdpReceiveStatus UdpReceiver::setup_status() const noexcept {
  if (!_impl) {
    return UdpReceiveStatus::SOCKET_ERROR;
  }

  return _impl->_setup_status;
}

int UdpReceiver::fd() const noexcept {
  if (!_impl) {
    return -1;
  }

  return _impl->_socket;
}

std::uint16_t UdpReceiver::bound_port() const noexcept {
  if (!_impl) {
    return 0U;
  }
  return _impl->_bound_port;
}

UdpReceiveResult UdpReceiver::receive_one() const {
  if (!ready()) {
    return UdpReceiveResult{_impl->_setup_status, WireStatus::SUCCESS,
                            std::nullopt, 0U, _impl->_setup_error};
  }
  /*
   * 1201 字节缓冲区用于发现超出 V2 1200 字节上限的数据报。
   *
   * 即使实际数据报更大，recvfrom() 至少会返回 1201，
   * 足以判定它违反协议限制。
   */
  std::array<std::uint8_t, V2_MAX_DATAGRAM_SIZE + 1U> buffer{};
  const ssize_t received = ::recvfrom(_impl->_socket, buffer.data(),
                                      buffer.size(), 0, nullptr, nullptr);
  if (received < 0) {
    const int receive_error = errno;

    if (receive_error == EAGAIN || receive_error == EWOULDBLOCK) {
      return UdpReceiveResult{UdpReceiveStatus::WOULD_BLOCK,
                              WireStatus::SUCCESS, std::nullopt, 0U,
                              receive_error};
    }

    return UdpReceiveResult{UdpReceiveStatus::RECEIVE_ERROR,
                            WireStatus::SUCCESS, std::nullopt, 0U,
                            receive_error};
  }

  const std::size_t received_size = static_cast<std::size_t>(received);
  if (received_size > V2_MAX_DATAGRAM_SIZE) {
    return UdpReceiveResult{UdpReceiveStatus::DATAGRAM_TOO_LARGE,
                            WireStatus::DATAGRAM_TOO_LARGE, std::nullopt,
                            received_size, 0};
  }

  /*
   * 接收器不自行判断 data[0]。
   *
   * 版本分派完全交给统一 decode()：
   *
   * 1 → decode_v1()
   * 2 → decode_v2()
   * 其他 → WRONG_VERSION
   */
  DecodeResult decoded = decode(buffer.data(), received_size);
  if (decoded._status != WireStatus::SUCCESS) {
    return UdpReceiveResult{UdpReceiveStatus::DECODE_ERROR, decoded._status,
                            std::nullopt, received_size, 0};
  }

  return UdpReceiveResult{UdpReceiveStatus::SUCCESS, WireStatus::SUCCESS,
                          std::move(decoded._record), received_size, 0};
}

} // namespace Wire
} // namespace TLSSMON
