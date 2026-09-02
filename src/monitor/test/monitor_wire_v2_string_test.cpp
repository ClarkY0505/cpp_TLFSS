#include "monitor_data.h"
#include "monitor_wire.h"
#include "monitor_wire_detail.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
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

inline constexpr std::uint32_t TEST_MID = 0x11223344U;
inline constexpr std::uint32_t TEST_LEVEL = 0x55667788U;
inline constexpr std::uint32_t TEST_FID = 0x99aabbccU;
inline constexpr std::uint32_t TEST_EID = 0xddeeff00U;

const MonData::MonitorKey TEST_KEY{
    TEST_MID,
    TEST_LEVEL,
    TEST_FID,
    TEST_EID};

MonData::StoredRecord make_string_record(
    std::string value,
    std::string description = {},
    MonData::MonitorTimestamp timestamp = MonData::MonitorTimestamp{},
    MonData::MonitorKey key = TEST_KEY) {
  return MonData::StoredRecord{
      MonData::MonitorData{
          key,
          std::move(description),
          std::move(value)},
      timestamp};
}

const std::string &string_value(
    const Wire::DecodedRecord &record) {
  const auto *value =
      std::get_if<std::string>(
          &record._data._value);

  assert(value != nullptr);
  return *value;
}

void assert_decode_failure(
    const Wire::DecodeResult &result,
    Wire::WireStatus expected_status) {
  assert(result._status == expected_status);
  assert(!result._record.has_value());
}

/*
 * 独立构造合法 V2 STRING 数据报，不调用 encode_v2()。
 *
 * description 和 value 都按显式长度复制，不增加 NUL。使用该报文测试
 * decode_v2()，可以避免编码器与解码器使用相同错误边界时伪造往返成功。
 */
std::vector<std::uint8_t> make_raw_string_datagram(
    const std::string &description,
    const std::string &value,
    std::int64_t seconds = INT64_C(123),
    std::uint32_t nanoseconds = 456'789'012U) {
  const std::size_t total_length =
      Wire::V2_HEADER_SIZE +
      description.size() +
      value.size();

  assert(total_length <= Wire::V2_MAX_DATAGRAM_SIZE);

  std::vector<std::uint8_t> bytes(total_length, 0U);

  bytes[VERSION_OFFSET] =
      static_cast<std::uint8_t>(Wire::WireVersion::V2);
  bytes[TYPE_OFFSET] =
      static_cast<std::uint8_t>(Wire::WireValueType::STRING);

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
      static_cast<std::uint16_t>(value.size()));

  if (!description.empty()) {
    std::copy_n(
        reinterpret_cast<const std::uint8_t *>(description.data()),
        description.size(),
        bytes.data() + Wire::V2_HEADER_SIZE);
  }

  if (!value.empty()) {
    std::copy_n(
        reinterpret_cast<const std::uint8_t *>(value.data()),
        value.size(),
        bytes.data() + Wire::V2_HEADER_SIZE + description.size());
  }

  return bytes;
}

/*
 * 空字符串是合法值。它使用 STRING 类型和 value_length=0，数据报中
 * 不存在用于表示空字符串的额外 NUL 字节。
 */
void test_empty_string_round_trip() {
  const MonData::StoredRecord source =
      make_string_record("", "empty-string");

  const Wire::EncodeResult encoded = Wire::encode_v2(source);

  assert(encoded._status == Wire::WireStatus::SUCCESS);
  assert(encoded._bytes.size() ==
         Wire::V2_HEADER_SIZE + source._data._description.size());
  assert(encoded._bytes[TYPE_OFFSET] ==
         static_cast<std::uint8_t>(Wire::WireValueType::STRING));
  assert(Wire::Detail::get_u16(
             encoded._bytes.data() + VALUE_LENGTH_OFFSET) == 0U);

  const Wire::DecodeResult decoded =
      Wire::decode_v2(
          encoded._bytes.data(),
          encoded._bytes.size());

  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());
  assert(decoded._record->_version == Wire::WireVersion::V2);
  assert(decoded._record->_data._description == "empty-string");
  assert(string_value(*decoded._record).empty());
}

/*
 * 普通 ASCII 编码后最后一个字节就是字符串最后一个字符，不得追加 V1
 * 使用的 NUL 终止符。
 */
void test_ascii_encoding_has_no_trailing_nul() {
  const MonData::StoredRecord source =
      make_string_record("hello");

  const Wire::EncodeResult encoded = Wire::encode_v2(source);

  const std::vector<std::uint8_t> expected_payload{
      static_cast<std::uint8_t>('h'),
      static_cast<std::uint8_t>('e'),
      static_cast<std::uint8_t>('l'),
      static_cast<std::uint8_t>('l'),
      static_cast<std::uint8_t>('o')};

  assert(encoded._status == Wire::WireStatus::SUCCESS);
  assert(encoded._bytes.size() == Wire::V2_HEADER_SIZE + 5U);
  assert(Wire::Detail::get_u16(
             encoded._bytes.data() + VALUE_LENGTH_OFFSET) == 5U);
  assert(std::equal(
      expected_payload.begin(),
      expected_payload.end(),
      encoded._bytes.begin() +
          static_cast<std::ptrdiff_t>(Wire::V2_HEADER_SIZE)));
  assert(encoded._bytes.back() == static_cast<std::uint8_t>('o'));
}

/*
 * 使用独立构造的报文验证 ASCII 解码，确保读取端不依赖尾部 NUL。
 */
void test_ascii_decoding_from_independent_datagram() {
  const std::vector<std::uint8_t> bytes =
      make_raw_string_datagram("status", "running");

  const Wire::DecodeResult decoded =
      Wire::decode_v2(bytes.data(), bytes.size());

  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());
  assert(decoded._record->_data._key == TEST_KEY);
  assert(decoded._record->_data._description == "status");
  assert(string_value(*decoded._record) == "running");
}

/*
 * V2 长度字段表示 UTF-8 字节数，而不是中文字符数量。编解码过程不解释
 * 字符编码，只完整保存原始字节。
 */
void test_utf8_chinese_round_trip() {
  const std::string source_value = u8"系统正常";
  const std::string description = u8"中文描述";
  const MonData::StoredRecord source =
      make_string_record(source_value, description);

  const Wire::EncodeResult encoded = Wire::encode_v2(source);

  assert(encoded._status == Wire::WireStatus::SUCCESS);
  assert(Wire::Detail::get_u16(
             encoded._bytes.data() + DESCRIPTION_LENGTH_OFFSET) ==
         description.size());
  assert(Wire::Detail::get_u16(
             encoded._bytes.data() + VALUE_LENGTH_OFFSET) ==
         source_value.size());

  const Wire::DecodeResult decoded =
      Wire::decode_v2(
          encoded._bytes.data(),
          encoded._bytes.size());

  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());
  assert(decoded._record->_data._description == description);
  assert(string_value(*decoded._record) == source_value);
}

/*
 * 显式长度保证内嵌 NUL 后的字节不会像 V1 一样被截断。
 */
void test_embedded_nul_round_trip() {
  const std::string source_value{"ab\0cd", 5U};
  const MonData::StoredRecord source =
      make_string_record(source_value, "binary-string");

  const Wire::EncodeResult encoded = Wire::encode_v2(source);
  assert(encoded._status == Wire::WireStatus::SUCCESS);
  assert(Wire::Detail::get_u16(
             encoded._bytes.data() + VALUE_LENGTH_OFFSET) == 5U);

  const Wire::DecodeResult decoded =
      Wire::decode_v2(
          encoded._bytes.data(),
          encoded._bytes.size());

  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());
  assert(string_value(*decoded._record) == source_value);
  assert(string_value(*decoded._record).size() == 5U);
  assert(string_value(*decoded._record)[2] == '\0');
}

/*
 * 非空 description、四个 Key 字段和包含纳秒的时间戳必须与字符串一起
 * 完整往返。
 */
void test_string_metadata_round_trip() {
  const MonData::MonitorKey key{
      0x01020304U,
      0x11121314U,
      0x21222324U,
      0x31323334U};
  const MonData::MonitorTimestamp timestamp{
      std::chrono::seconds{1'700'000'000} +
      std::chrono::nanoseconds{123'456'789}};
  const MonData::StoredRecord source =
      make_string_record(
          "running",
          "service status",
          timestamp,
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
  assert(decoded._record->_data._description == "service status");
  assert(string_value(*decoded._record) == "running");
  assert(decoded._record->_changed_at.has_value());
  assert(*decoded._record->_changed_at == timestamp);
}

/*
 * 64 字节字符串超过旧 V1 的 63 字节内容上限，但在 V2 中仍然合法。
 */
void test_v2_string_is_not_limited_to_63_bytes() {
  const std::string source_value(64U, 'A');
  const Wire::EncodeResult encoded =
      Wire::encode_v2(make_string_record(source_value));

  assert(encoded._status == Wire::WireStatus::SUCCESS);

  const Wire::DecodeResult decoded =
      Wire::decode_v2(
          encoded._bytes.data(),
          encoded._bytes.size());

  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());
  assert(string_value(*decoded._record) == source_value);
}

/*
 * description=400、value=760，加上 40 字节头部后恰好为 1200。
 */
void test_combined_payload_exactly_1200_bytes() {
  const std::string description(400U, 'D');
  const std::string source_value(760U, 'V');
  const MonData::StoredRecord source =
      make_string_record(source_value, description);

  const Wire::EncodeResult encoded = Wire::encode_v2(source);

  assert(encoded._status == Wire::WireStatus::SUCCESS);
  assert(encoded._bytes.size() == Wire::V2_MAX_DATAGRAM_SIZE);
  assert(Wire::Detail::get_u16(
             encoded._bytes.data() + DESCRIPTION_LENGTH_OFFSET) == 400U);
  assert(Wire::Detail::get_u16(
             encoded._bytes.data() + VALUE_LENGTH_OFFSET) == 760U);

  const Wire::DecodeResult decoded =
      Wire::decode_v2(
          encoded._bytes.data(),
          encoded._bytes.size());

  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());
  assert(decoded._record->_data._description == description);
  assert(string_value(*decoded._record) == source_value);
}

/*
 * 组合载荷增加一个字节后总长度为 1201，必须返回 DATAGRAM_TOO_LARGE，
 * 且错误结果不能携带部分数据报。
 */
void test_combined_payload_over_1200_is_rejected() {
  const MonData::StoredRecord source =
      make_string_record(
          std::string(761U, 'V'),
          std::string(400U, 'D'));

  const Wire::EncodeResult encoded = Wire::encode_v2(source);

  assert(encoded._status == Wire::WireStatus::DATAGRAM_TOO_LARGE);
  assert(encoded._bytes.empty());
}

/*
 * 删除最后一个 value 字节后，头部 total_length 与实际 UDP 数据报长度
 * 不一致，因此必须拒绝。
 */
void test_truncated_string_datagram_is_rejected() {
  const Wire::EncodeResult encoded =
      Wire::encode_v2(
          make_string_record("hello", "description"));

  assert(encoded._status == Wire::WireStatus::SUCCESS);

  std::vector<std::uint8_t> truncated = encoded._bytes;
  truncated.pop_back();

  assert_decode_failure(
      Wire::decode_v2(
          truncated.data(),
          truncated.size()),
      Wire::WireStatus::INVALID_TOTAL_LENGTH);
}

/*
 * 单独增加 description_length 会让字段长度之和大于实际载荷。
 */
void test_forged_description_length_is_rejected() {
  const Wire::EncodeResult encoded =
      Wire::encode_v2(
          make_string_record("hello", "description"));

  assert(encoded._status == Wire::WireStatus::SUCCESS);

  std::vector<std::uint8_t> forged = encoded._bytes;
  const std::uint16_t original_length =
      Wire::Detail::get_u16(
          forged.data() + DESCRIPTION_LENGTH_OFFSET);

  Wire::Detail::put_u16(
      forged.data() + DESCRIPTION_LENGTH_OFFSET,
      static_cast<std::uint16_t>(original_length + 1U));

  assert_decode_failure(
      Wire::decode_v2(forged.data(), forged.size()),
      Wire::WireStatus::INVALID_FIELD_LENGTH);
}

/*
 * 单独增加 value_length 同样会破坏字段长度总和。
 */
void test_forged_value_length_is_rejected() {
  const Wire::EncodeResult encoded =
      Wire::encode_v2(
          make_string_record("hello", "description"));

  assert(encoded._status == Wire::WireStatus::SUCCESS);

  std::vector<std::uint8_t> forged = encoded._bytes;
  const std::uint16_t original_length =
      Wire::Detail::get_u16(
          forged.data() + VALUE_LENGTH_OFFSET);

  Wire::Detail::put_u16(
      forged.data() + VALUE_LENGTH_OFFSET,
      static_cast<std::uint16_t>(original_length + 1U));

  assert_decode_failure(
      Wire::decode_v2(forged.data(), forged.size()),
      Wire::WireStatus::INVALID_FIELD_LENGTH);
}

} // namespace

int main() {
  test_empty_string_round_trip();
  test_ascii_encoding_has_no_trailing_nul();
  test_ascii_decoding_from_independent_datagram();
  test_utf8_chinese_round_trip();
  test_embedded_nul_round_trip();
  test_string_metadata_round_trip();
  test_v2_string_is_not_limited_to_63_bytes();
  test_combined_payload_exactly_1200_bytes();
  test_combined_payload_over_1200_is_rejected();
  test_truncated_string_datagram_is_rejected();
  test_forged_description_length_is_rejected();
  test_forged_value_length_is_rejected();
  return 0;
}
