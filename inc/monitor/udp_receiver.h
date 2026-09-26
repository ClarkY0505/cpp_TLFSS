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

/** @brief UDP 监听地址和端口；端口 0 由系统分配。 */
struct UdpReceiverConfig final {
  std::string _bind_address;
  std::uint16_t _bind_port;
};
/** @brief UDP 初始化、接收和解码状态。 */
enum class UdpReceiveStatus {
  SUCCESS,

  /**
   * @brief 配置或初始化错误。
   */
  INVALID_ENDPOINT,
  SOCKET_ERROR,

  /**
   * @brief 非阻塞 socket 当前已经没有数据。
   * 这是 drain 循环的正常结束条件，不是协议错误。
   */
  WOULD_BLOCK,

  /**
   * @brief recvfrom() 发生其他系统错误。
   */
  RECEIVE_ERROR,

  /**
   * @brief 收到超过 V2 1200 字节上限的数据报。
   */
  DATAGRAM_TOO_LARGE,

  /**
   * @brief 数据已经成功读取，但 Wire::decode() 拒绝。
   *
   * 具体原因保存在 _wire_status，例如：
   * WRONG_VERSION、SHORT_HEADER、INVALID_TOTAL_LENGTH。
   */
  DECODE_ERROR
};

/** @brief 单次数据报接收结果及底层错误信息。 */
struct UdpReceiveResult final {
  UdpReceiveStatus _status;

  /**
   * @brief 解码成功时为 SUCCESS。
   * DECODE_ERROR 时保存 Codec 的具体状态。
   */
  WireStatus _wire_status;

  /**
   * @brief 只有 SUCCESS 时包含解码后的记录。
   */
  std::optional<DecodedRecord> _record;

  /**
   * @brief recvfrom() 实际返回的字节数。
   */
  std::size_t _bytes_received;

  /**
   * @brief socket/recvfrom 失败时保存 errno。
   */
  int _system_error;

  /**
   * @brief 判断结果是否成功。
   * @return 状态为 SUCCESS 且结果有效时返回 true。
   */
  [[nodiscard]]
  bool success() const noexcept {
    return _status == UdpReceiveStatus::SUCCESS;
  }
};

/** @brief 在非阻塞 socket 上读取并解码监控数据报。 */
class UdpReceiver final {
public:
  /**
   * @brief 创建并绑定监听 socket；通过 ready() 查询结果。
   * @param config 绑定的 IPv4 地址和 UDP 端口；端口 0 由系统分配。
   */
  explicit UdpReceiver(UdpReceiverConfig config);
  ~UdpReceiver();

  UdpReceiver(const UdpReceiver &) = delete;
  UdpReceiver &operator=(const UdpReceiver &) = delete;

  UdpReceiver(UdpReceiver &&) = delete;
  UdpReceiver &operator=(UdpReceiver &&) = delete;

  /**
   * @brief 判断监听 socket 是否初始化成功。
   * @return 初始化完成并可执行操作时返回 true。
   */
  [[nodiscard]]
  bool ready() const noexcept;
  /**
   * @brief 返回监听 socket 的初始化状态。
   * @return 对象初始化的状态码。
   */
  [[nodiscard]]
  UdpReceiveStatus setup_status() const noexcept;

  /**
   * @brief Engine::add_aio() 监听这个 fd。
   *
   * UdpReceiver 仍然拥有 fd，Engine 不负责关闭。
   * @return 接收 socket 的文件描述符；不可用时返回 -1。
   */
  [[nodiscard]]
  int fd() const noexcept;

  /**
   * @brief 返回实际绑定端口。
   *
   * 配置端口为 0 时，这里返回系统分配的非零端口。
   * @return 实际绑定的端口；未就绪时返回 0。
   */
  [[nodiscard]]
  std::uint16_t bound_port() const noexcept;

  /**
   * @brief 返回监听地址和端口配置。
   * @return 构造时保存的配置引用。
   */
  [[nodiscard]]
  const UdpReceiverConfig &config() const noexcept {
    return _config;
  }

  /**
   * @brief 非阻塞读取一个数据报，并调用统一 Wire::decode()。
   *
   * 本函数不按版本写 switch。
   * @return 接收和解码状态，以及成功解析的单条记录。
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
