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

struct V2HeaderResult final {
  WireStatus _status;
  std::optional<V2Header> _header;
};

[[nodiscard]]
WireStatus write_v2_header(std::vector<std::uint8_t> &datagram,
                           const MonData::StoredRecord &record,
                           WireValueType type, std::size_t value_length);
[[nodiscard]]
V2HeaderResult read_v2_header(const std::uint8_t *data, std::size_t size);

/*
 * 这些函数的调用前提：
 *
 * - output/input 不是 nullptr；
 * - 调用方已经检查缓冲区至少有对应字节数。
 */
void put_u16(std::uint8_t *output, std::uint16_t value) noexcept;

void put_u32(std::uint8_t *output, std::uint32_t value) noexcept;

void put_u64(std::uint8_t *output, std::uint64_t value) noexcept;

[[nodiscard]]
std::uint16_t get_u16(const std::uint8_t *input) noexcept;

[[nodiscard]]
std::uint32_t get_u32(const std::uint8_t *input) noexcept;

[[nodiscard]]
std::uint64_t get_u64(const std::uint8_t *input) noexcept;

/*
 * timestamp seconds 在线协议中使用 64 位补码。
 */
void put_i64_twos_complement(std::uint8_t *output, std::int64_t value) noexcept;

[[nodiscard]]
std::int64_t get_i64_twos_complement(const std::uint8_t *input) noexcept;

/*
 * V2 timestamp nanoseconds 的合法范围。
 */
[[nodiscard]]
bool is_valid_nanoseconds(std::uint32_t nanoseconds) noexcept;

} // namespace Detail
} // namespace Wire
} // namespace TLSSMON

#endif // __MONITOR_WIRE_DETAIL_H__
