#ifndef __ALARM_PROTOCOL_H__
#define __ALARM_PROTOCOL_H__

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace TLSSMON {
namespace AlarmWire {

/*
 * 可靠告警信封固定 magic：
 *
 * ASCII "SMA1"
 */
inline constexpr std::array<std::uint8_t, 4> ALARM_MAGIC{
    static_cast<std::uint8_t>('S'), static_cast<std::uint8_t>('M'),
    static_cast<std::uint8_t>('A'), static_cast<std::uint8_t>('1')};

/*
 * 这里的版本是可靠告警信封版本。
 *
 * 它与 TLSSMON::Wire::WireVersion::V1/V2 无关。
 */
inline constexpr std::uint8_t ALARM_PROTOCOL_VERSION = 1U;

/*
 * message ID 使用 128 位随机值。
 */
inline constexpr std::size_t ALARM_MESSAGE_ID_SIZE = 16U;

/*
 * 告警 payload 上限。
 *
 * 当前 Monitor Wire V2 最多为 1200 字节，
 * 4096 字节为可靠信封未来扩展保留空间。
 */
inline constexpr std::size_t ALARM_MAX_PAYLOAD_SIZE = 4096U;

/*
 * 固定头部字段偏移。
 */
inline constexpr std::size_t ALARM_MAGIC_OFFSET = 0U;
inline constexpr std::size_t ALARM_VERSION_OFFSET = 4U;
inline constexpr std::size_t ALARM_TYPE_OFFSET = 5U;
inline constexpr std::size_t ALARM_FLAGS_OFFSET = 6U;
inline constexpr std::size_t ALARM_PAYLOAD_LENGTH_OFFSET = 8U;
inline constexpr std::size_t ALARM_SOURCE_ID_OFFSET = 12U;
inline constexpr std::size_t ALARM_MESSAGE_ID_OFFSET = 20U;
inline constexpr std::size_t ALARM_TIMESTAMP_OFFSET = 36U;
inline constexpr std::size_t ALARM_CRC32_OFFSET = 44U;

inline constexpr std::size_t ALARM_HEADER_SIZE = 48U;

inline constexpr std::size_t ALARM_MAX_FRAME_SIZE =
    ALARM_HEADER_SIZE + ALARM_MAX_PAYLOAD_SIZE;

/*
 * 可靠告警帧类型。
 *
 * ALARM：
 *   携带一条实际监控错误记录。
 *
 * ACK：
 *   Collector 已经可靠持久化对应告警。
 *
 * PING/PONG：
 *   空闲连接存活检测。
 */
enum class AlarmFrameType : std::uint8_t {
  ALARM = 1U,
  ACK = 2U,
  PING = 3U,
  PONG = 4U
};

constexpr bool is_valid_alarm_frame_type(AlarmFrameType type) noexcept {
  switch (type) {
  case AlarmFrameType::ALARM:
  case AlarmFrameType::ACK:
  case AlarmFrameType::PING:
  case AlarmFrameType::PONG:
    return true;
  }

  return false;
}

constexpr bool alarm_frame_requires_payload(AlarmFrameType type) noexcept {
  return type == AlarmFrameType::ALARM;
}

constexpr bool is_alarm_control_frame(AlarmFrameType type) noexcept {
  return type == AlarmFrameType::ACK || type == AlarmFrameType::PING ||
         type == AlarmFrameType::PONG;
}

using AlarmMessageId = std::array<std::uint8_t, ALARM_MESSAGE_ID_SIZE>;

/*
 * 解码后的告警帧。
 *
 * 这里保存的是主机字节序数值。
 * 不能使用 sizeof(AlarmFrame) 作为线协议头部长度。
 */
struct AlarmFrame final {
  AlarmFrameType _type{AlarmFrameType::ALARM};
  std::uint64_t _source_id{0U};
  AlarmMessageId _message_id{};
  std::uint64_t _timestamp_ms{0U};
  std::vector<std::uint8_t> _payload;
};

inline bool operator==(const AlarmFrame &lhs, const AlarmFrame &rhs) {
  return lhs._type == rhs._type && lhs._source_id == rhs._source_id &&
         lhs._message_id == rhs._message_id &&
         lhs._timestamp_ms == rhs._timestamp_ms && lhs._payload == rhs._payload;
}

inline bool operator!=(const AlarmFrame &lhs, const AlarmFrame &rhs) {
  return !(lhs == rhs);
}

/*
 * 纯协议层状态。
 *
 * 文件、socket、超时和 DNS 错误不属于该枚举，
 * 后续由 Spool、Publisher、Collector 各自状态表示。
 */
enum class AlarmProtocolStatus : std::uint8_t {
  SUCCESS,
  /*
   * decode 输入指针为空或长度为零。
   */
  EMPTY_INPUT,
  /*
   * 输入尚不足一个 48 字节头部。
   */
  SHORT_HEADER,
  /*
   * magic 不是 "SMA1"。
   */
  WRONG_MAGIC,
  /*
   * 信封版本不是 1。
   */
  WRONG_VERSION,
  /*
   * type 不属于 ALARM/ACK/PING/PONG。
   */
  INVALID_TYPE,
  /*
   * 首版 flags 必须等于零。
   */
  INVALID_FLAGS,
  /*
   * ALARM payload 为空，或者控制帧带有 payload。
   */
  INVALID_PAYLOAD_LENGTH,
  /*
   * payload_length 超过 4096。
   */
  PAYLOAD_TOO_LARGE,
  /*
   * 头部声明长度与实际完整帧长度不同。
   */
  INVALID_TOTAL_LENGTH,
  /*
   * CRC32 校验失败。
   */
  CRC_MISMATCH,
  /*
   * ALARM payload 无法由 Monitor Wire 解码。
   */
  INVALID_PAYLOAD,
  /*
   * vector 或其他协议对象分配内存失败。
   */
  ALLOCATION_FAILED
};

struct AlarmEncodeResult final {
  AlarmProtocolStatus _status{AlarmProtocolStatus::EMPTY_INPUT};

  std::vector<std::uint8_t> _bytes;

  [[nodiscard]] bool success() const noexcept;
};

struct AlarmDecodeResult final {
  AlarmProtocolStatus _status{AlarmProtocolStatus::EMPTY_INPUT};

  std::optional<AlarmFrame> _frame;

  [[nodiscard]] bool success() const noexcept;
};

struct AlarmFrameSizeResult final {
  AlarmProtocolStatus _status{AlarmProtocolStatus::EMPTY_INPUT};

  std::optional<std::size_t> _frame_size;

  [[nodiscard]] bool success() const noexcept;
};

/*
 * 函数不执行文件或网络 I/O。
 */
std::uint32_t alarm_crc32(const std::uint8_t *data, std::size_t size) noexcept;

AlarmFrameSizeResult alarm_frame_size(const std::uint8_t *prefix,
                                      std::size_t prefix_size) noexcept;

AlarmEncodeResult encode_alarm_frame(const AlarmFrame &frame) noexcept;

AlarmDecodeResult decode_alarm_frame(const std::uint8_t *data,
                                     std::size_t size) noexcept;

} // namespace AlarmWire
} // namespace TLSSMON
#endif // __ALARM_PROTOCOL_H__
