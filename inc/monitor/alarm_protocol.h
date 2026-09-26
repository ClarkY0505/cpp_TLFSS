#ifndef __ALARM_PROTOCOL_H__
#define __ALARM_PROTOCOL_H__

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace TLSSMON {
namespace AlarmWire {

/**
 * @brief 可靠告警信封固定 magic：
 *
 * ASCII "SMA1"
 */
inline constexpr std::array<std::uint8_t, 4> ALARM_MAGIC{
    static_cast<std::uint8_t>('S'), static_cast<std::uint8_t>('M'),
    static_cast<std::uint8_t>('A'), static_cast<std::uint8_t>('1')};

/**
 * @brief 这里的版本是可靠告警信封版本。
 *
 * 它与 TLSSMON::Wire::WireVersion::V1/V2 无关。
 */
inline constexpr std::uint8_t ALARM_PROTOCOL_VERSION = 1U;

/**
 * @brief message ID 使用 128 位随机值。
 */
inline constexpr std::size_t ALARM_MESSAGE_ID_SIZE = 16U;

/**
 * @brief 告警 payload 上限。
 *
 * 当前 Monitor Wire V2 最多为 1200 字节，
 * 4096 字节为可靠信封未来扩展保留空间。
 */
inline constexpr std::size_t ALARM_MAX_PAYLOAD_SIZE = 4096U;

/**
 * @brief 固定头部字段偏移。
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

/**
 * @brief 可靠告警帧类型。
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

/**
 * @brief 判断帧类型是否属于当前协议定义。
 * @param type 协议帧或监控值类型。
 * @return 帧类型已在当前协议中定义时返回 true。
 */
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

/**
 * @brief 判断帧类型是否要求携带 payload。
 * @param type 协议帧或监控值类型。
 * @return ALARM 帧返回 true，控制帧返回 false。
 */
constexpr bool alarm_frame_requires_payload(AlarmFrameType type) noexcept {
  return type == AlarmFrameType::ALARM;
}

/**
 * @brief 判断是否为 ACK、PING 或 PONG 控制帧。
 * @param type 协议帧或监控值类型。
 * @return ACK、PING 或 PONG 帧返回 true。
 */
constexpr bool is_alarm_control_frame(AlarmFrameType type) noexcept {
  return type == AlarmFrameType::ACK || type == AlarmFrameType::PING ||
         type == AlarmFrameType::PONG;
}

/** @brief 用于去重和 ACK 关联的 128 位告警消息 ID。 */
using AlarmMessageId = std::array<std::uint8_t, ALARM_MESSAGE_ID_SIZE>;

/**
 * @brief 解码后的告警帧。
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

/**
 * @brief 比较两个对象是否相等。
 * @param lhs 左操作数。
 * @param rhs 右操作数。
 * @return 比较条件成立时返回 true，否则返回 false。
 */
inline bool operator==(const AlarmFrame &lhs, const AlarmFrame &rhs) {
  return lhs._type == rhs._type && lhs._source_id == rhs._source_id &&
         lhs._message_id == rhs._message_id &&
         lhs._timestamp_ms == rhs._timestamp_ms && lhs._payload == rhs._payload;
}

/**
 * @brief 比较两个对象是否不同。
 * @param lhs 左操作数。
 * @param rhs 右操作数。
 * @return 比较条件成立时返回 true，否则返回 false。
 */
inline bool operator!=(const AlarmFrame &lhs, const AlarmFrame &rhs) {
  return !(lhs == rhs);
}

/**
 * @brief 纯协议层状态。
 *
 * 文件、socket、超时和 DNS 错误不属于该枚举，
 * 后续由 Spool、Publisher、Collector 各自状态表示。
 */
enum class AlarmProtocolStatus : std::uint8_t {
  SUCCESS,
  /**
   * @brief decode 输入指针为空或长度为零。
   */
  EMPTY_INPUT,
  /**
   * @brief 输入尚不足一个 48 字节头部。
   */
  SHORT_HEADER,
  /**
   * @brief magic 不是 "SMA1"。
   */
  WRONG_MAGIC,
  /**
   * @brief 信封版本不是 1。
   */
  WRONG_VERSION,
  /**
   * @brief type 不属于 ALARM/ACK/PING/PONG。
   */
  INVALID_TYPE,
  /**
   * @brief 首版 flags 必须等于零。
   */
  INVALID_FLAGS,
  /**
   * @brief ALARM payload 为空，或者控制帧带有 payload。
   */
  INVALID_PAYLOAD_LENGTH,
  /**
   * @brief payload_length 超过 4096。
   */
  PAYLOAD_TOO_LARGE,
  /**
   * @brief 头部声明长度与实际完整帧长度不同。
   */
  INVALID_TOTAL_LENGTH,
  /**
   * @brief CRC32 校验失败。
   */
  CRC_MISMATCH,
  /**
   * @brief ALARM payload 无法由 Monitor Wire 解码。
   */
  INVALID_PAYLOAD,
  /**
   * @brief vector 或其他协议对象分配内存失败。
   */
  ALLOCATION_FAILED
};

/** @brief 信封编码结果；成功时 _bytes 保存完整帧。 */
struct AlarmEncodeResult final {
  AlarmProtocolStatus _status{AlarmProtocolStatus::EMPTY_INPUT};

  std::vector<std::uint8_t> _bytes;

  /**
   * @brief 判断结果是否成功。
   * @return 状态为 SUCCESS 且结果有效时返回 true。
   */
  [[nodiscard]] bool success() const noexcept;
};

/** @brief 信封解码结果；仅成功时 _frame 有值。 */
struct AlarmDecodeResult final {
  AlarmProtocolStatus _status{AlarmProtocolStatus::EMPTY_INPUT};

  std::optional<AlarmFrame> _frame;

  /**
   * @brief 判断结果是否成功。
   * @return 状态为 SUCCESS 且结果有效时返回 true。
   */
  [[nodiscard]] bool success() const noexcept;
};

/** @brief 根据固定头部计算的完整帧长度。 */
struct AlarmFrameSizeResult final {
  AlarmProtocolStatus _status{AlarmProtocolStatus::EMPTY_INPUT};

  std::optional<std::size_t> _frame_size;

  /**
   * @brief 判断结果是否成功。
   * @return 状态为 SUCCESS 且结果有效时返回 true。
   */
  [[nodiscard]] bool success() const noexcept;
};

/**
 * @brief 计算可靠告警协议使用的 CRC32。
 * @note 纯计算函数，不执行文件或网络 I/O。
 * @param data 输入字节缓冲区；长度由 size 指定。
 * @param size 输入缓冲区的字节数。
 * @return 计算得到的 CRC32；非零长度且 data 为空时返回 0。
 */
std::uint32_t alarm_crc32(const std::uint8_t *data, std::size_t size) noexcept;

/**
 * @brief 从完整头部读取 payload 长度并计算预期帧大小。
 * @param prefix 至少包含固定头部的输入缓冲区。
 * @param prefix_size 输入缓冲区的字节数。
 * @return 头部校验状态以及完整帧的预期字节数。
 */
AlarmFrameSizeResult alarm_frame_size(const std::uint8_t *prefix,
                                      std::size_t prefix_size) noexcept;

/**
 * @brief 校验字段并编码完整可靠告警信封。
 * @param frame 要编码的告警帧。
 * @return 编码状态以及完整信封字节。
 */
AlarmEncodeResult encode_alarm_frame(const AlarmFrame &frame) noexcept;

/**
 * @brief 校验完整信封、CRC 和 ALARM payload 后解码。
 * @param data 输入字节缓冲区；长度由 size 指定。
 * @param size 输入缓冲区的字节数。
 * @return 解码状态以及成功解析的告警帧。
 */
AlarmDecodeResult decode_alarm_frame(const std::uint8_t *data,
                                     std::size_t size) noexcept;

} // namespace AlarmWire
} // namespace TLSSMON
#endif // __ALARM_PROTOCOL_H__
