#include "monitor_data.h"
#include "monitor_wire.h"
#include "monitor_wire_detail.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace TLSSMON;

namespace {

inline constexpr std::size_t VERSION_OFFSET = 0U;
inline constexpr std::size_t TYPE_OFFSET = 1U;
inline constexpr std::size_t FLAGS_OFFSET = 2U;
inline constexpr std::size_t TOTAL_LENGTH_OFFSET = 4U;
inline constexpr std::size_t HEADER_LENGTH_OFFSET = 6U;
inline constexpr std::size_t MID_OFFSET = 8U;
inline constexpr std::size_t LEVEL_OFFSET = 12U;
inline constexpr std::size_t FID_OFFSET = 16U;
inline constexpr std::size_t EID_OFFSET = 20U;
inline constexpr std::size_t SECONDS_OFFSET = 24U;
inline constexpr std::size_t NANOSECONDS_OFFSET = 32U;
inline constexpr std::size_t DESCRIPTION_LENGTH_OFFSET = 36U;
inline constexpr std::size_t VALUE_LENGTH_OFFSET = 38U;
inline constexpr std::size_t NUMERIC_PAYLOAD_SIZE = 8U;

inline constexpr std::uint32_t TEST_MID = 0x11223344U;
inline constexpr std::uint32_t TEST_LEVEL = 0x55667788U;
inline constexpr std::uint32_t TEST_FID = 0x99aabbccU;
inline constexpr std::uint32_t TEST_EID = 0xddeeff00U;

const MonData::MonitorKey TEST_KEY{
    TEST_MID,
    TEST_LEVEL,
    TEST_FID,
    TEST_EID};

MonData::StoredRecord make_numeric_record(
    std::uint32_t value,
    std::uint32_t state,
    std::string description,
    MonData::MonitorTimestamp changed_at,
    MonData::MonitorKey key = TEST_KEY) {
  return MonData::StoredRecord{
      MonData::MonitorData{
          key,
          std::move(description),
          MonData::NumericValue{value, state}},
      changed_at};
}

const MonData::NumericValue &numeric_value(
    const Wire::DecodedRecord &record) {
  const auto *numeric =
      std::get_if<MonData::NumericValue>(
          &record._data._value);

  assert(numeric != nullptr);
  return *numeric;
}

void assert_decode_failure(
    const Wire::DecodeResult &result,
    Wire::WireStatus expected_status) {
  assert(result._status == expected_status);
  assert(!result._record.has_value());
}

/*
 * 独立构造 V2 NUMERIC 数据报，不调用 encode_v2()。
 *
 * 这样 decode_v2() 即使和 encode_v2() 犯了相同的偏移错误，
 * 解码测试也不会因为“错误互相抵消”而通过。
 */
std::vector<std::uint8_t> make_raw_numeric_datagram(
    const std::string &description,
    std::uint16_t value_length,
    std::uint32_t value = 0x12345678U,
    std::uint32_t state = 0x90abcdefU,
    std::int64_t seconds = INT64_C(123),
    std::uint32_t nanoseconds = 456'789'012U) {
  const std::size_t total_length =
      Wire::V2_HEADER_SIZE +
      description.size() +
      static_cast<std::size_t>(value_length);

  assert(description.size() <=
         std::numeric_limits<std::uint16_t>::max());
  assert(total_length <= Wire::V2_MAX_DATAGRAM_SIZE);

  std::vector<std::uint8_t> bytes(total_length, 0U);

  bytes[VERSION_OFFSET] =
      static_cast<std::uint8_t>(Wire::WireVersion::V2);
  bytes[TYPE_OFFSET] =
      static_cast<std::uint8_t>(Wire::WireValueType::NUMERIC);

  Wire::Detail::put_u16(bytes.data() + FLAGS_OFFSET, 0U);
  Wire::Detail::put_u16(
      bytes.data() + TOTAL_LENGTH_OFFSET,
      static_cast<std::uint16_t>(total_length));
  Wire::Detail::put_u16(
      bytes.data() + HEADER_LENGTH_OFFSET,
      static_cast<std::uint16_t>(Wire::V2_HEADER_SIZE));
  Wire::Detail::put_u32(bytes.data() + MID_OFFSET, TEST_MID);
  Wire::Detail::put_u32(bytes.data() + LEVEL_OFFSET, TEST_LEVEL);
  Wire::Detail::put_u32(bytes.data() + FID_OFFSET, TEST_FID);
  Wire::Detail::put_u32(bytes.data() + EID_OFFSET, TEST_EID);
  Wire::Detail::put_i64_twos_complement(
      bytes.data() + SECONDS_OFFSET,
      seconds);
  Wire::Detail::put_u32(
      bytes.data() + NANOSECONDS_OFFSET,
      nanoseconds);
  Wire::Detail::put_u16(
      bytes.data() + DESCRIPTION_LENGTH_OFFSET,
      static_cast<std::uint16_t>(description.size()));
  Wire::Detail::put_u16(
      bytes.data() + VALUE_LENGTH_OFFSET,
      value_length);

  if (!description.empty()) {
    std::copy_n(
        reinterpret_cast<const std::uint8_t *>(description.data()),
        description.size(),
        bytes.data() + Wire::V2_HEADER_SIZE);
  }

  std::uint8_t *const payload =
      bytes.data() + Wire::V2_HEADER_SIZE + description.size();

  if (value_length >= 4U) {
    Wire::Detail::put_u32(payload, value);
  }
  if (value_length >= NUMERIC_PAYLOAD_SIZE) {
    Wire::Detail::put_u32(payload + 4U, state);
  }

  return bytes;
}

/*
 * 使用完整 golden bytes 验证编码结果，而不是只做 encode/decode 往返。
 * 该案例锁定 description 位置以及 value/state 的大端序。
 */
void test_numeric_encoding_matches_golden_bytes() {
  const MonData::MonitorTimestamp timestamp{
      std::chrono::seconds{0x01020304} +
      std::chrono::nanoseconds{0x11223344}};
  const MonData::StoredRecord record =
      make_numeric_record(
          0xa1b2c3d4U,
          0x01020304U,
          "abc",
          timestamp);

  const std::vector<std::uint8_t> expected{
      0x02, 0x00,                         // version, NUMERIC
      0x00, 0x00,                         // flags
      0x00, 0x33,                         // total_length = 51
      0x00, 0x28,                         // header_length = 40
      0x11, 0x22, 0x33, 0x44,             // mid
      0x55, 0x66, 0x77, 0x88,             // level
      0x99, 0xaa, 0xbb, 0xcc,             // fid
      0xdd, 0xee, 0xff, 0x00,             // eid
      0x00, 0x00, 0x00, 0x00,             // seconds high
      0x01, 0x02, 0x03, 0x04,             // seconds low
      0x11, 0x22, 0x33, 0x44,             // nanoseconds
      0x00, 0x03,                         // description_length
      0x00, 0x08,                         // value_length
      0x61, 0x62, 0x63,                   // "abc"
      0xa1, 0xb2, 0xc3, 0xd4,             // value
      0x01, 0x02, 0x03, 0x04};            // state

  const Wire::EncodeResult result = Wire::encode_v2(record);

  assert(result._status == Wire::WireStatus::SUCCESS);
  assert(result._bytes == expected);
}

/*
 * 解码独立构造的原始数据报，验证全部 Key、description、timestamp、
 * value 和 state 都来自正确的字段。
 */
void test_numeric_decoding_parses_independent_datagram() {
  const std::vector<std::uint8_t> bytes =
      make_raw_numeric_datagram(
          "independent",
          8U,
          0xa1b2c3d4U,
          0x01020304U,
          INT64_C(-2),
          250'000'000U);

  const Wire::DecodeResult result =
      Wire::decode_v2(bytes.data(), bytes.size());

  assert(result._status == Wire::WireStatus::SUCCESS);
  assert(result._record.has_value());

  const Wire::DecodedRecord &record = *result._record;
  assert(record._version == Wire::WireVersion::V2);
  assert(record._data._key == TEST_KEY);
  assert(record._data._description == "independent");
  assert(record._changed_at.has_value());

  const MonData::MonitorTimestamp expected_timestamp{
      std::chrono::seconds{-2} +
      std::chrono::nanoseconds{250'000'000}};
  assert(*record._changed_at == expected_timestamp);

  const MonData::NumericValue &numeric = numeric_value(record);
  assert(numeric._value == 0xa1b2c3d4U);
  assert(numeric._state == 0x01020304U);
}

/*
 * 空 description 是合法字段，数值数据报长度应恰好为 40 + 8。
 */
void test_empty_description_round_trip() {
  const MonData::StoredRecord source =
      make_numeric_record(
          10U,
          2U,
          "",
          MonData::MonitorTimestamp{
              std::chrono::seconds{100}});

  const Wire::EncodeResult encoded = Wire::encode_v2(source);
  assert(encoded._status == Wire::WireStatus::SUCCESS);
  assert(encoded._bytes.size() ==
         Wire::V2_HEADER_SIZE + NUMERIC_PAYLOAD_SIZE);

  const Wire::DecodeResult decoded =
      Wire::decode_v2(
          encoded._bytes.data(),
          encoded._bytes.size());

  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());
  assert(decoded._record->_data._description.empty());
  assert(numeric_value(*decoded._record)._value == 10U);
  assert(numeric_value(*decoded._record)._state == 2U);
}

/*
 * V2 description 使用显式长度，不是 C 字符串；内嵌 NUL 后的内容
 * 不能被截断。
 */
void test_description_with_embedded_nul_round_trip() {
  const std::string description{"abc\0def", 7U};
  const MonData::StoredRecord source =
      make_numeric_record(
          11U,
          1U,
          description,
          MonData::MonitorTimestamp{});

  const Wire::EncodeResult encoded = Wire::encode_v2(source);
  assert(encoded._status == Wire::WireStatus::SUCCESS);

  const Wire::DecodeResult decoded =
      Wire::decode_v2(
          encoded._bytes.data(),
          encoded._bytes.size());

  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());
  assert(decoded._record->_data._description == description);
  assert(decoded._record->_data._description.size() == 7U);
}

/*
 * 使用互不相同的四个字段，防止 mid/level/fid/eid 顺序写错后
 * 仍因测试数据相同而无法发现。
 */
void test_all_key_fields_round_trip() {
  const MonData::MonitorKey key{
      0x01020304U,
      0x11121314U,
      0x21222324U,
      0x31323334U};
  const MonData::StoredRecord source =
      make_numeric_record(
          12U,
          0U,
          "key",
          MonData::MonitorTimestamp{},
          key);

  const Wire::EncodeResult encoded = Wire::encode_v2(source);
  assert(encoded._status == Wire::WireStatus::SUCCESS);

  const Wire::DecodeResult decoded =
      Wire::decode_v2(
          encoded._bytes.data(),
          encoded._bytes.size());

  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());
  assert(decoded._record->_data._key == key);
}

/*
 * 正时间戳包含非零纳秒时必须精确往返。
 */
void test_positive_timestamp_round_trip() {
  const MonData::MonitorTimestamp timestamp{
      std::chrono::seconds{1'700'000'000} +
      std::chrono::nanoseconds{987'654'321}};
  const MonData::StoredRecord source =
      make_numeric_record(13U, 2U, "time", timestamp);

  const Wire::EncodeResult encoded = Wire::encode_v2(source);
  assert(encoded._status == Wire::WireStatus::SUCCESS);

  const Wire::DecodeResult decoded =
      Wire::decode_v2(
          encoded._bytes.data(),
          encoded._bytes.size());

  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());
  assert(decoded._record->_changed_at.has_value());
  assert(*decoded._record->_changed_at == timestamp);
}

/*
 * -0.5 秒在线协议中表示为 seconds=-1、nanoseconds=500000000，
 * 解码后必须恢复为原始负时间戳。
 */
void test_negative_timestamp_round_trip() {
  const MonData::MonitorTimestamp timestamp{
      std::chrono::nanoseconds{-500'000'000}};
  const MonData::StoredRecord source =
      make_numeric_record(14U, 2U, "negative", timestamp);

  const Wire::EncodeResult encoded = Wire::encode_v2(source);
  assert(encoded._status == Wire::WireStatus::SUCCESS);

  const Wire::DecodeResult decoded =
      Wire::decode_v2(
          encoded._bytes.data(),
          encoded._bytes.size());

  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());
  assert(decoded._record->_changed_at.has_value());
  assert(*decoded._record->_changed_at == timestamp);
}

/*
 * 验证 MonitorTimestamp 的完整可表示范围。
 *
 * 除了检查往返结果，还直接检查线协议中的 seconds/nanoseconds，防止
 * 编码器和解码器使用相同的错误算法后让普通往返测试错误地通过。
 *
 * int64 纳秒的两个边界正规化后分别是：
 *
 * min: seconds=-9223372037, nanoseconds=145224192
 * max: seconds= 9223372036, nanoseconds=854775807
 */
void test_timestamp_extremes_round_trip() {
  struct ExtremeTimestampCase final {
    MonData::MonitorTimestamp _timestamp;
    std::int64_t _wire_seconds;
    std::uint32_t _wire_nanoseconds;
  };

  const ExtremeTimestampCase cases[]{
      {MonData::MonitorTimestamp::min(),
       INT64_C(-9223372037),
       145'224'192U},
      {MonData::MonitorTimestamp::max(),
       INT64_C(9223372036),
       854'775'807U}};

  for (const ExtremeTimestampCase &test_case : cases) {
    const MonData::StoredRecord source =
        make_numeric_record(
            17U,
            2U,
            "timestamp-extreme",
            test_case._timestamp);

    const Wire::EncodeResult encoded = Wire::encode_v2(source);

    assert(encoded._status == Wire::WireStatus::SUCCESS);
    assert(encoded._bytes.size() >= Wire::V2_HEADER_SIZE);
    assert(Wire::Detail::get_i64_twos_complement(
               encoded._bytes.data() + SECONDS_OFFSET) ==
           test_case._wire_seconds);
    assert(Wire::Detail::get_u32(
               encoded._bytes.data() + NANOSECONDS_OFFSET) ==
           test_case._wire_nanoseconds);

    const Wire::DecodeResult decoded =
        Wire::decode_v2(
            encoded._bytes.data(),
            encoded._bytes.size());

    assert(decoded._status == Wire::WireStatus::SUCCESS);
    assert(decoded._record.has_value());
    assert(decoded._record->_changed_at.has_value());
    assert(*decoded._record->_changed_at == test_case._timestamp);
  }
}

/*
 * 公共 V2 头部允许保存完整 int64 seconds，但 MonitorTimestamp 在当前
 * 平台使用 int64 纳秒，因此可表示范围更窄。
 *
 * 边界之外一个纳秒的报文必须返回 INVALID_TIMESTAMP，不能溢出、回绕
 * 或被饱和到 min/max。
 */
void test_timestamp_outside_clock_range_is_rejected() {
  const std::vector<std::uint8_t> below_minimum =
      make_raw_numeric_datagram(
          "below-minimum",
          8U,
          1U,
          2U,
          INT64_C(-9223372037),
          145'224'191U);

  assert_decode_failure(
      Wire::decode_v2(
          below_minimum.data(),
          below_minimum.size()),
      Wire::WireStatus::INVALID_TIMESTAMP);

  const std::vector<std::uint8_t> above_maximum =
      make_raw_numeric_datagram(
          "above-maximum",
          8U,
          1U,
          2U,
          INT64_C(9223372036),
          854'775'808U);

  assert_decode_failure(
      Wire::decode_v2(
          above_maximum.data(),
          above_maximum.size()),
      Wire::WireStatus::INVALID_TIMESTAMP);
}

/*
 * value 和 state 均是无符号 32 位字段，最大值不能在转换时变成负数
 * 或被截断。
 */
void test_uint32_max_value_and_state_round_trip() {
  constexpr std::uint32_t maximum =
      std::numeric_limits<std::uint32_t>::max();
  const MonData::StoredRecord source =
      make_numeric_record(
          maximum,
          maximum,
          "maximum",
          MonData::MonitorTimestamp{});

  const Wire::EncodeResult encoded = Wire::encode_v2(source);
  assert(encoded._status == Wire::WireStatus::SUCCESS);

  const Wire::DecodeResult decoded =
      Wire::decode_v2(
          encoded._bytes.data(),
          encoded._bytes.size());

  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());
  assert(numeric_value(*decoded._record)._value == maximum);
  assert(numeric_value(*decoded._record)._state == maximum);
}

/*
 * NUMERIC 的 value_length 必须严格等于 8。
 *
 * 每个报文的 total_length 和字段长度之和保持一致，确保失败确实来自
 * NUMERIC 的固定长度规则，而不是阶段 4 的头部总长度校验。
 */
void test_numeric_value_length_must_equal_eight() {
  for (const std::uint16_t value_length : {
           std::uint16_t{0U},
           std::uint16_t{7U},
           std::uint16_t{9U}}) {
    const std::vector<std::uint8_t> bytes =
        make_raw_numeric_datagram(
            "abc",
            value_length);

    const Wire::DecodeResult result =
        Wire::decode_v2(bytes.data(), bytes.size());

    assert_decode_failure(
        result,
        Wire::WireStatus::INVALID_FIELD_LENGTH);
  }
}

/*
 * 数值报文最大 description 为 1200 - 40 - 8 = 1152 字节。
 * 恰好达到边界时仍必须成功编码并完整解码。
 */
void test_maximum_description_round_trip() {
  constexpr std::size_t maximum_description_length =
      Wire::V2_MAX_DATAGRAM_SIZE -
      Wire::V2_HEADER_SIZE -
      NUMERIC_PAYLOAD_SIZE;
  const std::string description(
      maximum_description_length,
      'A');
  const MonData::StoredRecord source =
      make_numeric_record(
          15U,
          2U,
          description,
          MonData::MonitorTimestamp{});

  const Wire::EncodeResult encoded = Wire::encode_v2(source);

  assert(encoded._status == Wire::WireStatus::SUCCESS);
  assert(encoded._bytes.size() == Wire::V2_MAX_DATAGRAM_SIZE);

  const Wire::DecodeResult decoded =
      Wire::decode_v2(
          encoded._bytes.data(),
          encoded._bytes.size());

  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());
  assert(decoded._record->_data._description == description);
}

/*
 * description 再增加一个字节会形成 1201 字节数据报，编码器必须拒绝，
 * 且失败结果不能携带部分编码数据。
 */
void test_oversized_description_is_rejected() {
  constexpr std::size_t oversized_description_length =
      Wire::V2_MAX_DATAGRAM_SIZE -
      Wire::V2_HEADER_SIZE -
      NUMERIC_PAYLOAD_SIZE + 1U;
  const MonData::StoredRecord source =
      make_numeric_record(
          16U,
          2U,
          std::string(oversized_description_length, 'B'),
          MonData::MonitorTimestamp{});

  const Wire::EncodeResult encoded = Wire::encode_v2(source);

  assert(encoded._status == Wire::WireStatus::DATAGRAM_TOO_LARGE);
  assert(encoded._bytes.empty());
}

/*
 * decode_v2() 必须把公共头部解析器的失败状态原样返回，不能继续读取
 * description 或数值载荷。
 */
void test_header_validation_error_is_propagated() {
  std::vector<std::uint8_t> bytes =
      make_raw_numeric_datagram("abc", 8U);
  Wire::Detail::put_u16(bytes.data() + FLAGS_OFFSET, 1U);

  assert_decode_failure(
      Wire::decode_v2(bytes.data(), bytes.size()),
      Wire::WireStatus::INVALID_FLAGS);

  bytes = make_raw_numeric_datagram("abc", 8U);
  Wire::Detail::put_u32(
      bytes.data() + NANOSECONDS_OFFSET,
      1'000'000'000U);

  assert_decode_failure(
      Wire::decode_v2(bytes.data(), bytes.size()),
      Wire::WireStatus::INVALID_TIMESTAMP);
}

} // namespace

int main() {
  test_numeric_encoding_matches_golden_bytes();
  test_numeric_decoding_parses_independent_datagram();
  test_empty_description_round_trip();
  test_description_with_embedded_nul_round_trip();
  test_all_key_fields_round_trip();
  test_positive_timestamp_round_trip();
  test_negative_timestamp_round_trip();
  test_timestamp_extremes_round_trip();
  test_timestamp_outside_clock_range_is_rejected();
  test_uint32_max_value_and_state_round_trip();
  test_numeric_value_length_must_equal_eight();
  test_maximum_description_round_trip();
  test_oversized_description_is_rejected();
  test_header_validation_error_is_propagated();
  return 0;
}
