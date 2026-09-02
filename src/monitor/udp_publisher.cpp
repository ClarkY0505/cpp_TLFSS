#include "monitor_wire.h"
#include "udp_publisher.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <memory>
#include <netinet/in.h>
#include <new>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace TLSSMON {
namespace Wire {
struct UdpPublisher::Impl {
  int _socket{-1};
  sockaddr_in _destination{};
  UdpPublishStatus _setup_status{UdpPublishStatus::SOCKET_ERROR};
  int _setup_error{0};
};

UdpPublisher::UdpPublisher(UdpPublisherConfig config)
    : _config(std::move(config)), _impl(std::make_unique<Impl>()) {
  if (_config._version != WireVersion::V1 &&
      _config._version != WireVersion::V2) {
    _impl->_setup_status = UdpPublishStatus::INVALID_VERSION;
    return;
  }

  if (_config._endpoint._address.empty() || _config._endpoint._port == 0) {
    _impl->_setup_status = UdpPublishStatus::INVALID_ENDPOINT;
    return;
  }

  sockaddr_in destination{};
  destination.sin_family = AF_INET;
  destination.sin_port = htons(_config._endpoint._port);
  const int address_result = ::inet_pton(
      AF_INET, _config._endpoint._address.c_str(), &destination.sin_addr);
  if (address_result != 1) {
    _impl->_setup_status = UdpPublishStatus::INVALID_ENDPOINT;
    if (address_result < 0) {
      _impl->_setup_error = errno;
    }

    return;
  }

  const int socket_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd < 0) {
    _impl->_setup_status = UdpPublishStatus::SOCKET_ERROR;
    _impl->_setup_error = errno;
    return;
  }

  _impl->_socket = socket_fd;
  _impl->_destination = destination;
  _impl->_setup_status = UdpPublishStatus::SUCCESS;
}

UdpPublisher::~UdpPublisher() {
  if (_impl && _impl->_socket >= 0) {
    ::close(_impl->_socket);
    _impl->_socket = -1;
  }
}

bool UdpPublisher::ready() const noexcept {
  return _impl && _impl->_socket >= 0 &&
         _impl->_setup_status == UdpPublishStatus::SUCCESS;
}

UdpPublishStatus UdpPublisher::setup_status() const noexcept {
  if (!_impl) {
    return UdpPublishStatus::SOCKET_ERROR;
  }

  return _impl->_setup_status;
}

UdpPublishResult UdpPublisher::send(const MonData::StoredRecord &record) const {
  if (!ready()) {
    return {_impl->_setup_status, WireStatus::SUCCESS, 0, _impl->_setup_error};
  }

  const EncodeResult encoded = encode(record, _config._version);
  if (encoded._status != WireStatus::SUCCESS) {
    return {UdpPublishStatus::ENCODE_ERROR, encoded._status, 0, 0};
  }

  /*
   * 一个 StoredRecord 只调用一次 sendto()。
   *
   * 不能按 TCP 的处理方式循环发送剩余字节，因为 UDP
   * 每次 sendto() 都代表一个独立数据报。
   */
  const ssize_t sent =
      ::sendto(_impl->_socket, encoded._bytes.data(), encoded._bytes.size(), 0,
               reinterpret_cast<const sockaddr *>(&_impl->_destination),
               static_cast<socklen_t>(sizeof(_impl->_destination)));

  if(sent < 0){
      const int send_error = errno;
      return {UdpPublishStatus::SEND_ERROR, WireStatus::SUCCESS, 0, send_error};
  }

  const std::size_t sent_size = static_cast<std::size_t>(sent);
  if(sent_size != encoded._bytes.size()){
      return {UdpPublishStatus::PARTIAL_SEND, WireStatus::SUCCESS, sent_size, 0};
  }

  return {UdpPublishStatus::SUCCESS, WireStatus::SUCCESS, sent_size, 0};
}

} // namespace Wire
} // namespace TLSSMON
