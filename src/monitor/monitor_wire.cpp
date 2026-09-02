#include "monitor_data.h"
#include "monitor_wire.h"
#include "monitor_wire_detail.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace TLSSMON {
namespace Wire {

namespace {

inline constexpr std::size_t V1_VERSION_OFFSET = 0U;
inline constexpr std::size_t V1_TYPE_OFFSET = 1U;
inline constexpr std::size_t V1_MID_OFFSET = 2U;
inline constexpr std::size_t V1_FID_OFFSET = 6U;
inline constexpr std::size_t V1_EID_OFFSET = 10U;
inline constexpr std::size_t V1_LEVEL_OFFSET = 14U;
inline constexpr std::size_t V1_VALUE_OFFSET = 18U;
inline constexpr std::size_t V1_STATE_OFFSET = 22U;
inline constexpr std::size_t V1_NUMERIC_PAYLOAD_SIZE = 8U;
inline constexpr std::size_t V1_MAX_STRING_CONTENT_SIZE =
    V1_MAX_DATAGRAM_SIZE - V1_HEADER_SIZE - 1U;

inline constexpr std::size_t V2_VERSION_OFFSET = 0U;
inline constexpr std::size_t V2_TYPE_OFFSET = 1U;
inline constexpr std::size_t V2_FLAGS_OFFSET = 2U;
inline constexpr std::size_t V2_TOTAL_LENGTH_OFFSET = 4U;
inline constexpr std::size_t V2_HEADER_LENGTH_OFFSET = 6U;
inline constexpr std::size_t V2_MID_OFFSET = 8U;
inline constexpr std::size_t V2_LEVEL_OFFSET = 12U;
inline constexpr std::size_t V2_FID_OFFSET = 16U;
inline constexpr std::size_t V2_EID_OFFSET = 20U;
inline constexpr std::size_t V2_SECONDS_OFFSET = 24U;
inline constexpr std::size_t V2_NANOSECONDS_OFFSET = 32U;
inline constexpr std::size_t V2_DESCRIPTION_LENGTH_OFFSET = 36U;
inline constexpr std::size_t V2_VALUE_LENGTH_OFFSET = 38U;
inline constexpr std::uint16_t V2_CURRENT_FLAGS = 0U;
inline constexpr std::size_t V2_NUMERIC_PAYLOAD_SIZE = 8U;
inline constexpr std::size_t V2_NUMERIC_VALUE_OFFSET = 0U;
inline constexpr std::size_t V2_NUMERIC_STATE_OFFSET = 4U;

inline constexpr std::int64_t NANOSECONDS_PER_SECOND = INT64_C(1'000'000'000);

static_assert(V1_MAX_STRING_CONTENT_SIZE == 63U);
static_assert(V2_VALUE_LENGTH_OFFSET + 2U == V2_HEADER_SIZE);
static_assert(V2_NUMERIC_STATE_OFFSET + sizeof(std::uint32_t) ==
              V2_NUMERIC_PAYLOAD_SIZE);

/*
 * split_timestamp() 和 join_timestamp() 直接把
 * MonitorTimestamp::duration::count() 作为 int64 纳秒总数处理。
 *
 * 因此要求 system_clock::duration 必须等于
 * std::chrono::nanoseconds。若平台时钟使用其他精度，
 * 编译时直接拒绝，避免单位误判、截断或整数溢出。
 */
static_assert(std::is_same_v<MonData::MonitorTimestamp::duration,
                             std::chrono::nanoseconds>,
              "MonitorTimestamp must use int64 nanoseconds");

struct TimestampParts final {
  std::int64_t _seconds;
  std::uint32_t _nanoseconds;
};

EncodeResult make_encode_error(WireStatus status) {
  return EncodeResult{status, {}};
}
DecodeResult make_decode_error(WireStatus status) {
  return DecodeResult{status, std::nullopt};
}

void write_v1_header(std::vector<std::uint8_t> &bytes, WireValueType type,
                     const MonData::MonitorKey &key) {
  bytes[V1_VERSION_OFFSET] = static_cast<std::uint8_t>(WireVersion::V1);
  bytes[V1_TYPE_OFFSET] = static_cast<std::uint8_t>(type);
  Detail::put_u32(bytes.data() + V1_MID_OFFSET, key._mid);
  Detail::put_u32(bytes.data() + V1_FID_OFFSET, key._fid);
  Detail::put_u32(bytes.data() + V1_EID_OFFSET, key._eid);
  Detail::put_u32(bytes.data() + V1_LEVEL_OFFSET, key._level);
}

MonData::MonitorKey read_v1_key(const std::uint8_t *data) {
  return MonData::MonitorKey{Detail::get_u32(data + V1_MID_OFFSET),
                             Detail::get_u32(data + V1_LEVEL_OFFSET),
                             Detail::get_u32(data + V1_FID_OFFSET),
                             Detail::get_u32(data + V1_EID_OFFSET)};
}

TimestampParts split_timestamp(MonData::MonitorTimestamp timestamp) {
  const std::int64_t total_nanoseconds = timestamp.time_since_epoch().count();
  std::int64_t seconds = total_nanoseconds / NANOSECONDS_PER_SECOND;
  std::int64_t nanoseconds = total_nanoseconds % NANOSECONDS_PER_SECOND;

  /*
   * C++ 的负数取模结果可能为负。
   *
   * 例如 -0.5 秒初步得到：
   *
   * seconds     = 0
   * nanoseconds = -500000000
   *
   * 调整为线协议要求的正规形式：
   *
   * seconds     = -1
   * nanoseconds = 500000000
   */
  if (nanoseconds < 0) {
    --seconds;
    nanoseconds += NANOSECONDS_PER_SECOND;
  }

  return TimestampParts{seconds, static_cast<std::uint32_t>(nanoseconds)};
}

bool timestamp_parts_less(const TimestampParts &lhs,
                          const TimestampParts &rhs) noexcept {
  return lhs._seconds < rhs._seconds ||
         (lhs._seconds == rhs._seconds && lhs._nanoseconds < rhs._nanoseconds);
}

Detail::V2HeaderResult make_v2_header_error(WireStatus status) {
  return Detail::V2HeaderResult{status, std::nullopt};
}

// 用于时间戳恢复
std::optional<MonData::MonitorTimestamp>
join_timestamp(std::int64_t seconds, std::uint32_t nanoseconds) {
  if (!Detail::is_valid_nanoseconds(nanoseconds)) {
    return std::nullopt;
  }

  const TimestampParts input{seconds, nanoseconds};

  const TimestampParts minimum =
      split_timestamp(MonData::MonitorTimestamp::min());

  const TimestampParts maximum =
      split_timestamp(MonData::MonitorTimestamp::max());

  /*
   * 先比较边界，再进行任何乘法。
   */
  if (timestamp_parts_less(input, minimum) ||
      timestamp_parts_less(maximum, input)) {
    return std::nullopt;
  }

  std::int64_t total_nanoseconds = 0;

  /*
   * 最小时间戳的正规化 seconds 可能是：
   *
   * -9223372037
   *
   * 它单独乘以十亿会下溢，但加上 nanoseconds 后最终结果合法，
   * 所以使用 INT64_MIN 加安全差值恢复。
   */
  if (seconds == minimum._seconds) {
    const std::uint32_t delta = nanoseconds - minimum._nanoseconds;

    total_nanoseconds = std::numeric_limits<std::int64_t>::min() +
                        static_cast<std::int64_t>(delta);
  } else {
    /*
     * 经过范围检查后：
     *
     * seconds * 1,000,000,000
     *
     * 及后面的加法均不会溢出。
     */
    total_nanoseconds = seconds * NANOSECONDS_PER_SECOND;

    total_nanoseconds += static_cast<std::int64_t>(nanoseconds);
  }

  return MonData::MonitorTimestamp{std::chrono::nanoseconds{total_nanoseconds}};
}

} // namespace

EncodeResult encode_v1(const MonData::StoredRecord &record) {
  const auto *numeric =
      std::get_if<MonData::NumericValue>(&record._data._value);
  if (numeric != nullptr) {
    std::vector<std::uint8_t> bytes(V1_NUMERIC_DATAGRAM_SIZE, 0U);

    write_v1_header(bytes, WireValueType::NUMERIC, record._data._key);
    Detail::put_u32(bytes.data() + V1_VALUE_OFFSET, numeric->_value);
    Detail::put_u32(bytes.data() + V1_STATE_OFFSET, numeric->_state);

    return EncodeResult{WireStatus::SUCCESS, std::move(bytes)};
  }

  const auto *string_value = std::get_if<std::string>(&record._data._value);
  if (string_value == nullptr) {
    return make_encode_error(WireStatus::INVALID_TYPE);
  }
  /*
   * V1 是 NUL 终止协议。std::string 中存在内嵌 NUL 时，
   **/
  const std::size_t terminator = string_value->find('\0');
  const std::size_t content_size =
      terminator == std::string::npos ? string_value->size() : terminator;
  if (content_size > V1_MAX_STRING_CONTENT_SIZE) {
    return make_encode_error(WireStatus::STRING_TOO_LONG);
  }

  std::vector<std::uint8_t> bytes(V1_HEADER_SIZE + content_size + 1U, 0U);
  write_v1_header(bytes, WireValueType::STRING, record._data._key);
  /*
   * 想要得到一个大致这个形式的协议帧
   * ┌──────────────────────┬──────────────────────────────────────┐
   * │ 头部数据             │ 字符串内容                           │
   * │ bytes[0]～bytes[17]  │ bytes[18]～bytes[18 + content_size]  │
   * └──────────────────────┴──────────────────────────────────────┘
   * */
  std::copy_n(reinterpret_cast<const std::uint8_t *>(string_value->data()),
              content_size,
              bytes.begin() + static_cast<std::ptrdiff_t>(V1_HEADER_SIZE));
  /*
   * vector 初始化时已经填 0；这里显式赋值用于表达协议要求。
   */
  bytes[V1_HEADER_SIZE + content_size] = 0U;
  return EncodeResult{WireStatus::SUCCESS, std::move(bytes)};
}

DecodeResult decode_v1(const std::uint8_t *data, std::size_t size) {
  if (data == nullptr || size == 0U) {
    return make_decode_error(WireStatus::EMPTY_INPUT);
  }
  /*
   * 第 0 字节存在时，优先检查版本。
   *
   * 因此 decode_v1(V2 数据报) 返回 WRONG_VERSION，
   * 而不是其他长度错误。
   */
  if (data[V1_VERSION_OFFSET] != static_cast<std::uint8_t>(WireVersion::V1)) {
    return make_decode_error(WireStatus::WRONG_VERSION);
  }

  if (size < V1_HEADER_SIZE) {
    return make_decode_error(WireStatus::SHORT_HEADER);
  }

  if (size > V1_MAX_DATAGRAM_SIZE) {
    return make_decode_error(WireStatus::DATAGRAM_TOO_LARGE);
  }
  const std::uint8_t raw_type = data[V1_TYPE_OFFSET];

  if (raw_type != static_cast<std::uint8_t>(WireValueType::NUMERIC) &&
      raw_type != static_cast<std::uint8_t>(WireValueType::STRING)) {
    return make_decode_error(WireStatus::INVALID_TYPE);
  }

  const MonData::MonitorKey key = read_v1_key(data);

  if (raw_type == static_cast<std::uint8_t>(WireValueType::NUMERIC)) {
    if (size < V1_NUMERIC_DATAGRAM_SIZE) {
      return make_decode_error(WireStatus::INVALID_FIELD_LENGTH);
    }

    const MonData::NumericValue numeric{
        Detail::get_u32(data + V1_VALUE_OFFSET),
        Detail::get_u32(data + V1_STATE_OFFSET)};

    /*
     * size 大于 26 时不再读取尾部，兼容旧 V1 扩展。
     */
    return DecodeResult{WireStatus::SUCCESS,
                        DecodedRecord{WireVersion::V1,
                                      MonData::MonitorData{key, {}, numeric},
                                      std::nullopt}};
  }

  const std::uint8_t *const payload_begin = data + V1_HEADER_SIZE;

  const std::uint8_t *const payload_end = data + size;

  /*
   * 找到第一个 NUL。
   *
   * 找不到时，terminator == payload_end，
   * 此时使用全部有效载荷。
   */
  const std::uint8_t *const terminator =
      std::find(payload_begin, payload_end, static_cast<std::uint8_t>(0U));

  const std::size_t content_size =
      static_cast<std::size_t>(terminator - payload_begin);

  if (content_size > V1_MAX_STRING_CONTENT_SIZE) {
    return make_decode_error(WireStatus::STRING_TOO_LONG);
  }

  const std::string value{reinterpret_cast<const char *>(payload_begin),
                          content_size};

  return DecodeResult{WireStatus::SUCCESS,
                      DecodedRecord{WireVersion::V1,
                                    MonData::MonitorData{key, {}, value},
                                    std::nullopt}};
}

EncodeResult encode_v2(const MonData::StoredRecord &record) {
  const auto *numeric =
      std::get_if<MonData::NumericValue>(&record._data._value);
  const auto *string_value = std::get_if<std::string>(&record._data._value);

  WireValueType type = WireValueType::NUMERIC;
  std::size_t value_length = 0U;

  if (numeric != nullptr) {
    type = WireValueType::NUMERIC;
    value_length = V2_NUMERIC_PAYLOAD_SIZE;
  } else if (string_value != nullptr) {
    type = WireValueType::STRING;
    value_length = string_value->size();
  } else {
    return make_encode_error(WireStatus::INVALID_TYPE);
  }
  const std::size_t description_length = record._data._description.size();

  if (description_length > V2_MAX_DATAGRAM_SIZE - V2_HEADER_SIZE) {
    return make_encode_error(WireStatus::DATAGRAM_TOO_LARGE);
  }

  const std::size_t available_value_length =
      V2_MAX_DATAGRAM_SIZE - V2_HEADER_SIZE - description_length;
  if (value_length > available_value_length) {
    return make_encode_error(WireStatus::DATAGRAM_TOO_LARGE);
  }

  const std::size_t total_length =
      V2_HEADER_SIZE + description_length + value_length;

  std::vector<std::uint8_t> bytes(total_length, 0U);

  const WireStatus header_status =
      Detail::write_v2_header(bytes, record, type, value_length);

  if (header_status != WireStatus::SUCCESS) {
    return make_encode_error(header_status);
  }

  /*
   * description 紧跟在 40 字节头部之后。
   * 显式传递长度，因此内嵌 NUL 也会被完整编码。
   */
  if (description_length != 0U) {
    std::copy_n(reinterpret_cast<const std::uint8_t *>(
                    record._data._description.data()),
                description_length, bytes.data() + V2_HEADER_SIZE);
  }

  std::uint8_t *const payload =
      bytes.data() + V2_HEADER_SIZE + description_length;
  if (numeric != nullptr) {
    Detail::put_u32(payload + V2_NUMERIC_VALUE_OFFSET, numeric->_value);
    Detail::put_u32(payload + V2_NUMERIC_STATE_OFFSET, numeric->_state);
  } else if (value_length != 0U) {
    std::copy_n(reinterpret_cast<const std::uint8_t *>(string_value->data()),
                value_length, payload);
  }

  return EncodeResult{WireStatus::SUCCESS, std::move(bytes)};
}

// V2 解码
DecodeResult decode_v2(const std::uint8_t *data, std::size_t size) {
  const Detail::V2HeaderResult header_result =
      Detail::read_v2_header(data, size);

  if (header_result._status != WireStatus::SUCCESS) {
    return make_decode_error(header_result._status);
  }

  assert(header_result._header.has_value());

  const Detail::V2Header &header = *header_result._header;

  if (header._type == WireValueType::NUMERIC &&
      header._value_length != V2_NUMERIC_PAYLOAD_SIZE) {
    return make_decode_error(WireStatus::INVALID_FIELD_LENGTH);
  }

  const std::optional<MonData::MonitorTimestamp> timestamp =
      join_timestamp(header._seconds, header._nanoseconds);
  if (!timestamp.has_value()) {
    return make_decode_error(WireStatus::INVALID_TIMESTAMP);
  }

  const std::size_t description_length =
      static_cast<std::size_t>(header._description_length);
  const std::size_t value_length =
      static_cast<std::size_t>(header._value_length);
  const std::uint8_t *const description_begin = data + V2_HEADER_SIZE;
  const std::string description{
      reinterpret_cast<const char *>(description_begin), description_length};
  const std::uint8_t *const payload = description_begin + description_length;
  MonData::MonitorValue decoded_value{MonData::NumericValue{}};

  if (header._type == WireValueType::NUMERIC) {
    decoded_value = MonData::NumericValue{
        Detail::get_u32(payload + V2_NUMERIC_VALUE_OFFSET),
        Detail::get_u32(payload + V2_NUMERIC_STATE_OFFSET)};
  } else if (header._type == WireValueType::STRING) {
    decoded_value =
        std::string{reinterpret_cast<const char *>(payload), value_length};
  } else {
    return make_decode_error(WireStatus::INVALID_TYPE);
  }
  return DecodeResult{
      WireStatus::SUCCESS,
      DecodedRecord{WireVersion::V2,
                    MonData::MonitorData{header._key, std::move(description),
                                         std::move(decoded_value)},
                    *timestamp}};
}

EncodeResult encode(const MonData::StoredRecord &record, WireVersion version) {

  switch (version) {
  case WireVersion::V1:
    return encode_v1(record);

  case TLSSMON::Wire::WireVersion::V2:
    return encode_v2(record);

  default:
    return make_encode_error(WireStatus::WRONG_VERSION);
  }
}

DecodeResult decode(const std::uint8_t *data, std::size_t size) {
  if (data == nullptr || size == 0U) {
    return make_decode_error(WireStatus::EMPTY_INPUT);
  }

  switch (data[0]) {
  case static_cast<std::uint8_t>(WireVersion::V1):
    return decode_v1(data, size);

  case static_cast<std::uint8_t>(WireVersion::V2):
    return decode_v2(data, size);

  default:
    return make_decode_error(WireStatus::WRONG_VERSION);
  }
}

namespace Detail {
void put_u16(std::uint8_t *output, std::uint16_t value) noexcept {
  // value is 0x1234
  // output[0] = 0x0012 & 0xffU
  // 0001 0010 & 1111 1111 = 0001 0010 = 0x12
  output[0] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
  output[1] = static_cast<std::uint8_t>(value & 0xffU);
}

std::uint16_t get_u16(const std::uint8_t *input) noexcept {
  const std::uint32_t value = (static_cast<std::uint32_t>(input[0]) << 8U) |
                              static_cast<std::uint32_t>(input[1]);

  return static_cast<std::uint16_t>(value);
}

void put_u32(std::uint8_t *output, std::uint32_t value) noexcept {
  output[0] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
  output[1] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
  output[2] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
  output[3] = static_cast<std::uint8_t>(value & 0xffU);
}

std::uint32_t get_u32(const std::uint8_t *input) noexcept {
  return (static_cast<std::uint32_t>(input[0]) << 24U) |
         (static_cast<std::uint32_t>(input[1]) << 16U) |
         (static_cast<std::uint32_t>(input[2]) << 8U) |
         static_cast<std::uint32_t>(input[3]);
}

void put_u64(std::uint8_t *output, std::uint64_t value) noexcept {
  output[0] = static_cast<std::uint8_t>((value >> 56U) & UINT64_C(0xff));
  output[1] = static_cast<std::uint8_t>((value >> 48U) & UINT64_C(0xff));
  output[2] = static_cast<std::uint8_t>((value >> 40U) & UINT64_C(0xff));
  output[3] = static_cast<std::uint8_t>((value >> 32U) & UINT64_C(0xff));
  output[4] = static_cast<std::uint8_t>((value >> 24U) & UINT64_C(0xff));
  output[5] = static_cast<std::uint8_t>((value >> 16U) & UINT64_C(0xff));
  output[6] = static_cast<std::uint8_t>((value >> 8U) & UINT64_C(0xff));
  output[7] = static_cast<std::uint8_t>(value & UINT64_C(0xff));
}

std::uint64_t get_u64(const std::uint8_t *input) noexcept {
  return (static_cast<std::uint64_t>(input[0]) << 56U) |
         (static_cast<std::uint64_t>(input[1]) << 48U) |
         (static_cast<std::uint64_t>(input[2]) << 40U) |
         (static_cast<std::uint64_t>(input[3]) << 32U) |
         (static_cast<std::uint64_t>(input[4]) << 24U) |
         (static_cast<std::uint64_t>(input[5]) << 16U) |
         (static_cast<std::uint64_t>(input[6]) << 8U) |
         static_cast<std::uint64_t>(input[7]);
}

void put_i64_twos_complement(std::uint8_t *output,
                             std::int64_t value) noexcept {
  put_u64(output, static_cast<std::uint64_t>(value));
}

std::int64_t get_i64_twos_complement(const std::uint8_t *input) noexcept {
  const std::uint64_t bits = get_u64(input);

  constexpr std::uint64_t sign_bit = UINT64_C(1) << 63U;

  /*
   * 最高位为 0，是普通非负数。
   */
  if ((bits & sign_bit) == 0U) {
    return static_cast<std::int64_t>(bits);
  }

  /*
   * 负数补码的绝对值：
   *
   * magnitude = ~bits + 1
   */
  const std::uint64_t magnitude = (~bits) + UINT64_C(1);

  /*
   * 2^63 无法表示为正的 int64_t，单独映射到 INT64_MIN。
   */
  if (magnitude == sign_bit) {
    return std::numeric_limits<std::int64_t>::min();
  }

  return -static_cast<std::int64_t>(magnitude);
}

bool is_valid_nanoseconds(std::uint32_t nanoseconds) noexcept {
  constexpr std::uint32_t nanoseconds_per_second = 1'000'000'000U;

  return nanoseconds < nanoseconds_per_second;
}

/*
 * 调用方必须先创建完整数据报大小：
 * std::vector<std::uint8_t> datagram(
      V2_HEADER_SIZE + description_length + value_length,0U);
 * */
WireStatus write_v2_header(std::vector<std::uint8_t> &datagram,
                           const MonData::StoredRecord &record,
                           WireValueType type, std::size_t value_length) {
  const std::size_t description_length = record._data._description.size();
  if (description_length > std::numeric_limits<std::uint16_t>::max() ||
      value_length > std::numeric_limits<std::uint16_t>::max()) {
    return WireStatus::INVALID_FIELD_LENGTH;
  }

  const std::size_t total_length =
      V2_HEADER_SIZE + description_length + value_length;
  if (total_length > V2_MAX_DATAGRAM_SIZE) {
    return WireStatus::DATAGRAM_TOO_LARGE;
  }

  if (datagram.size() != total_length) {
    return WireStatus::INVALID_TOTAL_LENGTH;
  }

  const auto raw_type = static_cast<std::uint8_t>(type);
  if (raw_type != static_cast<std::uint8_t>(WireValueType::NUMERIC) &&
      raw_type != static_cast<std::uint8_t>(WireValueType::STRING)) {
    return WireStatus::INVALID_TYPE;
  }

  const TimestampParts timestamp = split_timestamp(record._changed_at);
  if (!is_valid_nanoseconds(timestamp._nanoseconds)) {
    return WireStatus::INVALID_TIMESTAMP;
  }

  datagram[V2_VERSION_OFFSET] = static_cast<std::uint8_t>(WireVersion::V2);
  datagram[V2_TYPE_OFFSET] = raw_type;
  put_u16(datagram.data() + V2_FLAGS_OFFSET, V2_CURRENT_FLAGS);
  put_u16(datagram.data() + V2_TOTAL_LENGTH_OFFSET,
          static_cast<std::uint16_t>(total_length));
  put_u16(datagram.data() + V2_HEADER_LENGTH_OFFSET,
          static_cast<std::uint16_t>(V2_HEADER_SIZE));
  put_u32(datagram.data() + V2_MID_OFFSET, record._data._key._mid);
  put_u32(datagram.data() + V2_LEVEL_OFFSET, record._data._key._level);
  put_u32(datagram.data() + V2_FID_OFFSET, record._data._key._fid);
  put_u32(datagram.data() + V2_EID_OFFSET, record._data._key._eid);
  put_i64_twos_complement(datagram.data() + V2_SECONDS_OFFSET,
                          timestamp._seconds);
  put_u32(datagram.data() + V2_NANOSECONDS_OFFSET, timestamp._nanoseconds);
  put_u16(datagram.data() + V2_DESCRIPTION_LENGTH_OFFSET,
          static_cast<std::uint16_t>(description_length));
  put_u16(datagram.data() + V2_VALUE_LENGTH_OFFSET,
          static_cast<std::uint16_t>(value_length));

  return WireStatus::SUCCESS;
}

V2HeaderResult read_v2_header(const std::uint8_t *data, std::size_t size) {
  if (data == nullptr || size == 0U) {
    return make_v2_header_error(WireStatus::EMPTY_INPUT);
  }

  if (data[V2_VERSION_OFFSET] != static_cast<std::uint8_t>(WireVersion::V2)) {
    return make_v2_header_error(WireStatus::WRONG_VERSION);
  }

  if (size < V2_HEADER_SIZE) {
    return make_v2_header_error(WireStatus::SHORT_HEADER);
  }

  if (size > V2_MAX_DATAGRAM_SIZE) {
    return make_v2_header_error(WireStatus::DATAGRAM_TOO_LARGE);
  }

  const std::uint8_t raw_type = data[V2_TYPE_OFFSET];
  if (raw_type != static_cast<std::uint8_t>(WireValueType::NUMERIC) &&
      raw_type != static_cast<std::uint8_t>(WireValueType::STRING)) {
    return make_v2_header_error(WireStatus::INVALID_TYPE);
  }

  const std::uint16_t flags = get_u16(data + V2_FLAGS_OFFSET);
  if (flags != V2_CURRENT_FLAGS) {
    return make_v2_header_error(WireStatus::INVALID_FLAGS);
  }

  const std::uint16_t total_length = get_u16(data + V2_TOTAL_LENGTH_OFFSET);

  const std::uint16_t header_length = get_u16(data + V2_HEADER_LENGTH_OFFSET);

  if (header_length != V2_HEADER_SIZE) {
    return make_v2_header_error(WireStatus::INVALID_HEADER_LENGTH);
  }

  if (total_length > V2_MAX_DATAGRAM_SIZE) {
    return make_v2_header_error(WireStatus::DATAGRAM_TOO_LARGE);
  }

  if (total_length < V2_HEADER_SIZE ||
      static_cast<std::size_t>(total_length) != size) {
    return make_v2_header_error(WireStatus::INVALID_TOTAL_LENGTH);
  }

  const std::uint32_t nanoseconds = get_u32(data + V2_NANOSECONDS_OFFSET);

  if (!is_valid_nanoseconds(nanoseconds)) {
    return make_v2_header_error(WireStatus::INVALID_TIMESTAMP);
  }

  const std::uint16_t description_length =
      get_u16(data + V2_DESCRIPTION_LENGTH_OFFSET);

  const std::uint16_t value_length = get_u16(data + V2_VALUE_LENGTH_OFFSET);

  const std::size_t declared_payload_length =
      static_cast<std::size_t>(description_length) +
      static_cast<std::size_t>(value_length);

  const std::size_t actual_payload_length = size - V2_HEADER_SIZE;

  if (declared_payload_length != actual_payload_length) {
    return make_v2_header_error(WireStatus::INVALID_FIELD_LENGTH);
  }

  const V2Header header{static_cast<WireValueType>(raw_type),
                        flags,
                        total_length,
                        header_length,
                        MonData::MonitorKey{get_u32(data + V2_MID_OFFSET),
                                            get_u32(data + V2_LEVEL_OFFSET),
                                            get_u32(data + V2_FID_OFFSET),
                                            get_u32(data + V2_EID_OFFSET)},
                        get_i64_twos_complement(data + V2_SECONDS_OFFSET),
                        nanoseconds,
                        description_length,
                        value_length};

  return V2HeaderResult{WireStatus::SUCCESS, header};
}

} // namespace Detail
} // namespace Wire
} // namespace TLSSMON
