#ifndef __MONITOR_WIRE_H__
#define __MONITOR_WIRE_H__

#include "monitor_data.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
namespace TLSSMON {
namespace Wire {
/** @brief V1 固定头部的字节数。 */
inline constexpr std::size_t V1_HEADER_SIZE = 18U;

/** @brief V1 数值数据报的固定字节数。 */
inline constexpr std::size_t V1_NUMERIC_DATAGRAM_SIZE = 26U;

/** @brief V1 数据报的最大字节数。 */
inline constexpr std::size_t V1_MAX_DATAGRAM_SIZE = 82U;

/** @brief V2 固定头部的字节数。 */
inline constexpr std::size_t V2_HEADER_SIZE = 40U;

/** @brief V2 数据报的最大字节数。 */
inline constexpr std::size_t V2_MAX_DATAGRAM_SIZE = 1200U;

/** @brief 监控数据报的线协议版本。 */
enum class WireVersion : std::uint8_t { V1 = 1, V2 = 2 };
/**
 * @brief 数据报中监控值的类型标记。
 * @note NumericValue 对应 NUMERIC，std::string 对应 STRING。
 */
enum class WireValueType : std::uint8_t { NUMERIC = 0, STRING = 1 };

/** @brief 监控数据报编码或解码的状态码。 */
enum class WireStatus {
  SUCCESS,               ///< 编码或解码成功
  EMPTY_INPUT,           ///< 解码输入为空
  WRONG_VERSION,         ///< 未知版本，或专用解码器收到另一版本
  INVALID_TYPE,          ///< value type 既不是 NUMERIC 也不是 STRING
  INVALID_FLAGS,         ///< V2 flags 不为 0
  SHORT_HEADER,          ///< 数据报短于对应版本固定头部
  INVALID_HEADER_LENGTH, ///< V2 header_length 不等于 40
  INVALID_TOTAL_LENGTH,  ///< V2 total_length 与实际数据报长度不一致
  INVALID_FIELD_LENGTH, ///< description_length/value_length 与报文布局不一致
  INVALID_TIMESTAMP,    ///< V2 纳秒大于或等于 1,000,000,000
  STRING_TOO_LONG,      ///< V1 字符串内容超过 63 字节
  DATAGRAM_TOO_LARGE    ///< V2 完整数据报超过 1200 字节
};
/**
 * @brief 解码后的监控记录与原始协议版本。
 * @note V1 不携带描述或变化时间；V2 保留完整描述和线协议时间戳。
 */
struct DecodedRecord final {
  WireVersion _version;
  MonData::MonitorData _data;
  std::optional<MonData::MonitorTimestamp> _changed_at;
};
/**
 * @brief 数据报编码结果。
 * @note 成功时 _bytes 包含完整数据报；失败时为空。
 */
struct EncodeResult final {
  WireStatus _status;
  std::vector<std::uint8_t> _bytes;
};

/**
 * @brief 数据报解码结果。
 * @note 仅 _status 为 SUCCESS 时 _record 有值。
 */
struct DecodeResult final {
  WireStatus _status;
  std::optional<DecodedRecord> _record;
};

/**
 * @brief 将记录编码为 V1 数据报；V1 不传输描述和变化时间。
 * @param record 要编码、发送或保存的监控记录。
 * @return 编码状态及生成的数据报。
 */
EncodeResult encode_v1(const MonData::StoredRecord &record);

/**
 * @brief 将记录编码为 V2 数据报，包含描述和变化时间。
 * @param record 要编码、发送或保存的监控记录。
 * @return 编码状态及生成的数据报。
 */
EncodeResult encode_v2(const MonData::StoredRecord &record);

/**
 * @brief 只接受 V1 数据报；错误时返回具体 WireStatus。
 * @param data 输入字节缓冲区；长度由 size 指定。
 * @param size 输入缓冲区的字节数。
 * @return 解码状态及成功解析的记录。
 */
DecodeResult decode_v1(const std::uint8_t *data, std::size_t size);

/**
 * @brief 只接受 V2 数据报；错误时返回具体 WireStatus。
 * @param data 输入字节缓冲区；长度由 size 指定。
 * @param size 输入缓冲区的字节数。
 * @return 解码状态及成功解析的记录。
 */
DecodeResult decode_v2(const std::uint8_t *data, std::size_t size);

/**
 * @brief 按显式指定的协议版本编码记录。
 * @param record 要编码、发送或保存的监控记录。
 * @param version 监控线协议版本。
 * @return 编码状态及生成的数据报。
 */
EncodeResult encode(const MonData::StoredRecord &record, WireVersion version);

/**
 * @brief 根据版本字段自动选择解码器。
 * @param data 输入字节缓冲区；长度由 size 指定。
 * @param size 输入缓冲区的字节数。
 * @return 解码状态及成功解析的记录。
 */
DecodeResult decode(const std::uint8_t *data, std::size_t size);

} // namespace Wire
} // namespace TLSSMON

#endif // __MONITOR_WIRE_H__
