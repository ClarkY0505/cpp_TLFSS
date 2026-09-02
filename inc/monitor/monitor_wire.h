#ifndef __MONITOR_WIRE_H__
#define __MONITOR_WIRE_H__

#include "monitor_data.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
namespace TLSSMON {
namespace Wire {
inline constexpr std::size_t V1_HEADER_SIZE = 18U;

inline constexpr std::size_t V1_NUMERIC_DATAGRAM_SIZE = 26U;

inline constexpr std::size_t V1_MAX_DATAGRAM_SIZE = 82U;

inline constexpr std::size_t V2_HEADER_SIZE = 40U;

inline constexpr std::size_t V2_MAX_DATAGRAM_SIZE = 1200U;

enum class WireVersion : std::uint8_t { V1 = 1, V2 = 2 };
/*
 * MonData::NumericValue → WireValueType::NUMERIC
 * std::string           → WireValueType::STRING
 * */
enum class WireValueType : std::uint8_t { NUMERIC = 0, STRING = 1 };

enum class WireStatus {
  SUCCESS,               // 编码或解码成功
  EMPTY_INPUT,           // 解码输入为空
  WRONG_VERSION,         // 未知版本，或专用解码器收到另一版本
  INVALID_TYPE,          // value type 既不是 NUMERIC 也不是 STRING
  INVALID_FLAGS,         // V2 flags 不为 0
  SHORT_HEADER,          // 数据报短于对应版本固定头部
  INVALID_HEADER_LENGTH, // V2 header_length 不等于 40
  INVALID_TOTAL_LENGTH,  // V2 total_length 与实际数据报长度不一致
  INVALID_FIELD_LENGTH, // description_length/value_length 与报文布局不一致
  INVALID_TIMESTAMP,    // V2 纳秒大于或等于 1,000,000,000
  STRING_TOO_LONG,      // V1 字符串内容超过 63 字节
  DATAGRAM_TOO_LARGE    // V2 完整数据报超过 1200 字节
};
/*
 * V1:
 *     _version == WireVersion::V1
 *     _data._description.empty()
 *     _changed_at == std::nullopt_t
 * V2:
 *     _version == WireVersion::V2
 *     _data._description 保存完整 description
 *     _changed_at 保存完整线协议时间戳
 * */
struct DecodedRecord final {
  WireVersion _version;
  MonData::MonitorData _data;
  std::optional<MonData::MonitorTimestamp> _changed_at;
};
/*
 * 编码成功：
 *     _status == SUCCESS
 *     _bytes 包含完整数据报
 * 编码失败：
 *     _status != SUCCESS
 *     _bytes.empty()
 * 解码成功：
 *     _status == SUCCESS
 *     _record.has_value()
 * 解码失败：
 *     _status != SUCCESS
 *     !_record.has_value()
 * */
struct EncodeResult final {
  WireStatus _status;
  std::vector<std::uint8_t> _bytes;
};

struct DecodeResult final {
  WireStatus _status;
  std::optional<DecodedRecord> _record;
};

EncodeResult encode_v1(const MonData::StoredRecord &record);

EncodeResult encode_v2(const MonData::StoredRecord &record);

DecodeResult decode_v1(const std::uint8_t *data, std::size_t size);

DecodeResult decode_v2(const std::uint8_t *data, std::size_t size);

EncodeResult encode(const MonData::StoredRecord &record, WireVersion version);

DecodeResult decode(const std::uint8_t *data, std::size_t size);

} // namespace Wire
} // namespace TLSSMON

#endif // __MONITOR_WIRE_H__
