#ifndef __UDP_RECEIVER_H__
#define __UDP_RECEIVER_H__

#include "monitor_wire.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace TLSSMON {
namespace Wire {

struct UdpReceiverConfig final {
  std::string _bind_address;
  std::uint16_t _bind_port;
};
enum class UdpReceiveStatus {
  SUCCESS,

  /*
   * 配置或初始化错误。
   */
  INVALID_ENDPOINT,
  SOCKET_ERROR,

  /*
   * 非阻塞 socket 当前已经没有数据。
   * 这是 drain 循环的正常结束条件，不是协议错误。
   */
  WOULD_BLOCK,

  /*
   * recvfrom() 发生其他系统错误。
   */
  RECEIVE_ERROR,

  /*
   * 收到超过 V2 1200 字节上限的数据报。
   */
  DATAGRAM_TOO_LARGE,

  /*
   * 数据已经成功读取，但 Wire::decode() 拒绝。
   *
   * 具体原因保存在 _wire_status，例如：
   * WRONG_VERSION、SHORT_HEADER、INVALID_TOTAL_LENGTH。
   */
  DECODE_ERROR
};

struct UdpReceiveResult final {
  UdpReceiveStatus _status;

  /*
   * 解码成功时为 SUCCESS。
   * DECODE_ERROR 时保存 Codec 的具体状态。
   */
  WireStatus _wire_status;

  /*
   * 只有 SUCCESS 时包含解码后的记录。
   */
  std::optional<DecodedRecord> _record;

  /*
   * recvfrom() 实际返回的字节数。
   */
  std::size_t _bytes_received;

  /*
   * socket/recvfrom 失败时保存 errno。
   */
  int _system_error;

  [[nodiscard]]
  bool success() const noexcept {
    return _status == UdpReceiveStatus::SUCCESS;
  }
};

class UdpReceiver final {
public:
  explicit UdpReceiver(UdpReceiverConfig config);
  ~UdpReceiver();

  UdpReceiver(const UdpReceiver &) = delete;
  UdpReceiver &operator=(const UdpReceiver &) = delete;

  UdpReceiver(UdpReceiver &&) = delete;
  UdpReceiver &operator=(UdpReceiver &&) = delete;

  [[nodiscard]]
  bool ready() const noexcept;
  [[nodiscard]]
  UdpReceiveStatus setup_status() const noexcept;

  /*
   * Engine::add_aio() 监听这个 fd。
   *
   * UdpReceiver 仍然拥有 fd，Engine 不负责关闭。
   */
  [[nodiscard]]
  int fd() const noexcept;

  /*
   * 返回实际绑定端口。
   *
   * 配置端口为 0 时，这里返回系统分配的非零端口。
   */
  [[nodiscard]]
  std::uint16_t bound_port() const noexcept;

  [[nodiscard]]
  const UdpReceiverConfig &config() const noexcept {
    return _config;
  }

  /*
   * 非阻塞读取一个数据报，并调用统一 Wire::decode()。
   *
   * 本函数不按版本写 switch。
   */
  [[nodiscard]]
  UdpReceiveResult receive_one() const;

private:
  struct Impl;

  const UdpReceiverConfig _config;
  std::unique_ptr<Impl> _impl;
};

} // namespace Wire
} // namespace TLSSMON
#endif // __UDP_RECEIVER_H__
