#ifndef __MONITOR_WIRE_DETAIL_H__
#define __MONITOR_WIRE_DETAIL_H__

#include "monitor_wire.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace TLSSMON {
namespace Wire {
namespace Detail {

/** @brief 解析后的 V2 头部字段，整数采用主机字节序。 */
struct V2Header final {
  WireValueType _type;
  std::uint16_t _flags;
  std::uint16_t _total_length;
  std::uint16_t _header_length;
  MonData::MonitorKey _key;

  std::int64_t _seconds;
  std::uint32_t _nanoseconds;

  std::uint16_t _description_length;
  std::uint16_t _value_length;
};

/** @brief V2 头部解析结果；仅成功时 _header 有值。 */
struct V2HeaderResult final {
  WireStatus _status;
  std::optional<V2Header> _header;
};

/**
 * @brief 向数据报写入 V2 头部，并校验记录与长度。
 * @param datagram 要写入头部的完整数据报缓冲区。
 * @param record 要编码、发送或保存的监控记录。
 * @param type 协议帧或监控值类型。
 * @param value_length 编码后监控值的字节数。
 * @return 头部写入和字段校验状态。
 */
[[nodiscard]]
WireStatus write_v2_header(std::vector<std::uint8_t> &datagram,
                           const MonData::StoredRecord &record,
                           WireValueType type, std::size_t value_length);
/**
 * @brief 校验并解析 V2 固定头部。
 * @param data 输入字节缓冲区；长度由 size 指定。
 * @param size 输入缓冲区的字节数。
 * @return 头部校验状态以及解析出的字段。
 */
[[nodiscard]]
V2HeaderResult read_v2_header(const std::uint8_t *data, std::size_t size);

/**
 * @brief 以网络字节序写入 16 位无符号整数。
 * @pre output 非空，且至少有 2 字节可写。
 * @param output 已预留足够空间的目标字节缓冲区。
 * @param value 要写入的整数值。
 */
void put_u16(std::uint8_t *output, std::uint16_t value) noexcept;

/**
 * @brief 以网络字节序写入 32 位无符号整数。
 * @pre output 非空，且至少有 4 字节可写。
 * @param output 已预留足够空间的目标字节缓冲区。
 * @param value 要写入的整数值。
 */
void put_u32(std::uint8_t *output, std::uint32_t value) noexcept;

/**
 * @brief 以网络字节序写入 64 位无符号整数。
 * @pre output 非空，且至少有 8 字节可写。
 * @param output 已预留足够空间的目标字节缓冲区。
 * @param value 要写入的整数值。
 */
void put_u64(std::uint8_t *output, std::uint64_t value) noexcept;

/**
 * @brief 从网络字节序读取 16 位无符号整数。
 * @pre input 非空，且至少有 2 字节可读。
 * @param input 输入缓冲区。
 * @return 从输入缓冲区读取的整数值。
 */
[[nodiscard]]
std::uint16_t get_u16(const std::uint8_t *input) noexcept;

/**
 * @brief 从网络字节序读取 32 位无符号整数。
 * @pre input 非空，且至少有 4 字节可读。
 * @param input 输入缓冲区。
 * @return 从输入缓冲区读取的整数值。
 */
[[nodiscard]]
std::uint32_t get_u32(const std::uint8_t *input) noexcept;

/**
 * @brief 从网络字节序读取 64 位无符号整数。
 * @pre input 非空，且至少有 8 字节可读。
 * @param input 输入缓冲区。
 * @return 从输入缓冲区读取的整数值。
 */
[[nodiscard]]
std::uint64_t get_u64(const std::uint8_t *input) noexcept;

/**
 * @brief 以网络字节序写入 64 位补码时间戳秒数。
 * @param output 已预留足够空间的目标字节缓冲区。
 * @param value 要写入的整数值。
 */
void put_i64_twos_complement(std::uint8_t *output, std::int64_t value) noexcept;

/**
 * @brief 从网络字节序读取 64 位补码时间戳秒数。
 * @param input 输入缓冲区。
 * @return 从输入缓冲区读取的整数值。
 */
[[nodiscard]]
std::int64_t get_i64_twos_complement(const std::uint8_t *input) noexcept;

/**
 * @brief V2 timestamp nanoseconds 的合法范围。
 * @param nanoseconds 时间戳的纳秒部分。
 * @return 纳秒数位于合法范围内时返回 true。
 */
[[nodiscard]]
bool is_valid_nanoseconds(std::uint32_t nanoseconds) noexcept;

} // namespace Detail
} // namespace Wire
} // namespace TLSSMON

#endif // __MONITOR_WIRE_DETAIL_H__
