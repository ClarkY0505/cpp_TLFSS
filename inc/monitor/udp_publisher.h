#ifndef __UDP_PUBLISHER_H__
#define __UDP_PUBLISHER_H__

#include "monitor_wire.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace TLSSMON {
namespace Wire {
enum class UdpPublishStatus {
  SUCCESS,
  INVALID_ENDPOINT,
  INVALID_VERSION,
  SOCKET_ERROR,
  ENCODE_ERROR,
  SEND_ERROR,
  PARTIAL_SEND
};

struct UdpPublishResult final {
  UdpPublishStatus _status;
  WireStatus _wire_status;
  std::size_t _bytes_sent;
  int _system_error;

  [[nodiscard]] bool success() const noexcept {
    return _status == UdpPublishStatus::SUCCESS;
  }
};

struct UdpEndpoint final {
  std::string _address;
  std::uint16_t _port;
};

struct UdpPublisherConfig final {
  UdpEndpoint _endpoint;
  WireVersion _version;

  /*
   * 禁止默认构造，调用者必须显式提供 Endpoint 和版本。
   */
  UdpPublisherConfig() = delete;
  UdpPublisherConfig(UdpEndpoint endpoint, WireVersion version)
      : _endpoint(std::move(endpoint)), _version(version) {}
};

class UdpPublisher final {
public:
  explicit UdpPublisher(UdpPublisherConfig config);
  ~UdpPublisher();

  UdpPublisher(const UdpPublisher &) = delete;
  UdpPublisher &operator=(const UdpPublisher &) = delete;

  UdpPublisher(UdpPublisher &&) = delete;
  UdpPublisher &operator=(UdpPublisher &&) = delete;
  [[nodiscard]] bool ready() const noexcept;

  [[nodiscard]] UdpPublishStatus setup_status() const noexcept;

  [[nodiscard]] WireVersion version() const noexcept {
    return _config._version;
  }

  [[nodiscard]] const UdpPublisherConfig &config() const noexcept {
    return _config;
  }

  [[nodiscard]] UdpPublishResult send(const MonData::StoredRecord &record) const;

private:
  struct Impl;
  const UdpPublisherConfig _config;
  /*
   * 隐藏 sockaddr_in、socket fd 等平台相关类型。
   */
  std::unique_ptr<Impl> _impl;
};

} // namespace Wire
} // namespace TLSSMON

#endif // __UDP_PUBLISHER_H__
