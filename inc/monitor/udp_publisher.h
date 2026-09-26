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
/** @brief UDP 发布初始化、编码和发送状态。 */
enum class UdpPublishStatus {
  SUCCESS,
  INVALID_ENDPOINT,
  INVALID_VERSION,
  SOCKET_ERROR,
  ENCODE_ERROR,
  SEND_ERROR,
  PARTIAL_SEND
};

/** @brief 单次发送结果，包含线协议状态、发送字节数和系统错误。 */
struct UdpPublishResult final {
  UdpPublishStatus _status;
  WireStatus _wire_status;
  std::size_t _bytes_sent;
  int _system_error;

  /**
   * @brief 判断结果是否成功。
   * @return 状态为 SUCCESS 且结果有效时返回 true。
   */
  [[nodiscard]] bool success() const noexcept {
    return _status == UdpPublishStatus::SUCCESS;
  }
};

/** @brief 目标 IPv4 地址和 UDP 端口。 */
struct UdpEndpoint final {
  std::string _address;
  std::uint16_t _port;
};

/** @brief UDP 目标及选用的监控线协议版本。 */
struct UdpPublisherConfig final {
  UdpEndpoint _endpoint;
  WireVersion _version;

  /**
   * @brief 禁止默认构造，调用者必须显式提供 Endpoint 和版本。
   */
  UdpPublisherConfig() = delete;
  /**
   * @brief 使用目标地址和线协议版本创建发布配置。
   * @param endpoint UDP 目标地址和端口。
   * @param version 监控线协议版本。
   */
  UdpPublisherConfig(UdpEndpoint endpoint, WireVersion version)
      : _endpoint(std::move(endpoint)), _version(version) {}
};

/** @brief 将监控记录编码并发送到固定 UDP 目标。 */
class UdpPublisher final {
public:
  /**
   * @brief 按配置创建发送 socket；通过 ready() 查询初始化结果。
   * @param config 目标 IPv4 地址与非零端口，以及 V1/V2 协议版本。
   */
  explicit UdpPublisher(UdpPublisherConfig config);
  ~UdpPublisher();

  UdpPublisher(const UdpPublisher &) = delete;
  UdpPublisher &operator=(const UdpPublisher &) = delete;

  UdpPublisher(UdpPublisher &&) = delete;
  UdpPublisher &operator=(UdpPublisher &&) = delete;
  /**
   * @brief 判断目标和 socket 是否初始化成功。
   * @return 初始化完成并可执行操作时返回 true。
   */
  [[nodiscard]] bool ready() const noexcept;

  /**
   * @brief 返回初始化状态。
   * @return 对象初始化的状态码。
   */
  [[nodiscard]] UdpPublishStatus setup_status() const noexcept;

  /**
   * @brief 返回发送时使用的线协议版本。
   * @return 发送时使用的线协议版本。
   */
  [[nodiscard]] WireVersion version() const noexcept {
    return _config._version;
  }

  /**
   * @brief 返回目标地址和协议版本配置。
   * @return 构造时保存的配置引用。
   */
  [[nodiscard]] const UdpPublisherConfig &config() const noexcept {
    return _config;
  }

  /**
   * @brief 编码并发送一条记录；错误详情保存在返回值中。
   * @param record 要编码、发送或保存的监控记录。
   * @return 编码和发送状态、已发送字节数及系统错误。
   */
  [[nodiscard]] UdpPublishResult send(const MonData::StoredRecord &record) const;

private:
  struct Impl;
  const UdpPublisherConfig _config;
  /**
   * @brief 隐藏 sockaddr_in、socket fd 等平台相关类型。
   */
  std::unique_ptr<Impl> _impl;
};

} // namespace Wire
} // namespace TLSSMON

#endif // __UDP_PUBLISHER_H__
