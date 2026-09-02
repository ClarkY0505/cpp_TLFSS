#include "monitor_data.h"
#include "monitor_wire.h"
#include "monitor_wire_detail.h"

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

inline constexpr std::uint32_t TEST_MID = 0x11223344U;
inline constexpr std::uint32_t TEST_LEVEL = 4U;
inline constexpr std::uint32_t TEST_FID = 0x55667788U;
inline constexpr std::uint32_t TEST_EID = 3U;
inline constexpr std::uint32_t TEST_NUMERIC_VALUE = 12345U;
inline constexpr std::uint32_t TEST_NUMERIC_STATE = 2U;

const MonData::MonitorKey TEST_KEY{
    TEST_MID,
    TEST_LEVEL,
    TEST_FID,
    TEST_EID};

const MonData::MonitorTimestamp TEST_TIMESTAMP{
    std::chrono::seconds{123456789}};

MonData::StoredRecord make_numeric_record(
    std::uint32_t value = TEST_NUMERIC_VALUE,
    std::uint32_t state = TEST_NUMERIC_STATE,
    std::string description = "v1-numeric-description") {
  return MonData::StoredRecord{
      MonData::MonitorData{
          TEST_KEY,
          std::move(description),
          MonData::NumericValue{value, state}},
      TEST_TIMESTAMP};
}

MonData::StoredRecord make_string_record(
    std::string value,
    std::string description = "v1-string-description") {
  return MonData::StoredRecord{
      MonData::MonitorData{
          TEST_KEY,
          std::move(description),
          std::move(value)},
      TEST_TIMESTAMP};
}

std::vector<std::uint8_t> numeric_golden_bytes() {
  return std::vector<std::uint8_t>{
      0x01, 0x00,                         // version, numeric type
      0x11, 0x22, 0x33, 0x44,             // mid
      0x55, 0x66, 0x77, 0x88,             // fid (legacy hid)
      0x00, 0x00, 0x00, 0x03,             // eid
      0x00, 0x00, 0x00, 0x04,             // level
      0x00, 0x00, 0x30, 0x39,             // value = 12345
      0x00, 0x00, 0x00, 0x02};            // state = fresh
}

std::vector<std::uint8_t> make_raw_v1_header(
    std::uint8_t type) {
  std::vector<std::uint8_t> bytes(
      Wire::V1_HEADER_SIZE,
      0U);

  bytes[0] =
      static_cast<std::uint8_t>(Wire::WireVersion::V1);
  bytes[1] = type;

  Wire::Detail::put_u32(bytes.data() + 2U, TEST_MID);
  Wire::Detail::put_u32(bytes.data() + 6U, TEST_FID);
  Wire::Detail::put_u32(bytes.data() + 10U, TEST_EID);
  Wire::Detail::put_u32(bytes.data() + 14U, TEST_LEVEL);

  return bytes;
}

void assert_key_equals_test_key(
    const MonData::MonitorKey &key) {
  assert(key._mid == TEST_MID);
  assert(key._level == TEST_LEVEL);
  assert(key._fid == TEST_FID);
  assert(key._eid == TEST_EID);
}

const MonData::NumericValue &numeric_value(
    const Wire::DecodedRecord &record) {
  const auto *value =
      std::get_if<MonData::NumericValue>(
          &record._data._value);

  assert(value != nullptr);
  return *value;
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
 * 固定 26 字节数值报文，避免错误编码器和错误解码器通过
 * 相互抵消得到伪造的往返成功。
 */
void test_numeric_encoding_matches_legacy_bytes() {
  const Wire::EncodeResult result =
      Wire::encode_v1(make_numeric_record());

  assert(result._status == Wire::WireStatus::SUCCESS);
  assert(result._bytes.size() == Wire::V1_NUMERIC_DATAGRAM_SIZE);
  assert(result._bytes == numeric_golden_bytes());
}

/*
 * 直接解码独立构造的 golden bytes，验证 Key、value、state 以及 V1
 * 不产生 description 和 timestamp。
 */
void test_numeric_decoding_matches_legacy_bytes() {
  const std::vector<std::uint8_t> bytes =
      numeric_golden_bytes();

  const Wire::DecodeResult result =
      Wire::decode_v1(bytes.data(), bytes.size());

  assert(result._status == Wire::WireStatus::SUCCESS);
  assert(result._record.has_value());

  const Wire::DecodedRecord &record = *result._record;
  assert(record._version == Wire::WireVersion::V1);
  assert_key_equals_test_key(record._data._key);
  assert(record._data._description.empty());
  assert(!record._changed_at.has_value());
  assert(numeric_value(record)._value == TEST_NUMERIC_VALUE);
  assert(numeric_value(record)._state == TEST_NUMERIC_STATE);
}

void test_numeric_round_trip() {
  const MonData::StoredRecord source =
      make_numeric_record(0xa1b2c3d4U, 1U);

  const Wire::EncodeResult encoded =
      Wire::encode_v1(source);

  assert(encoded._status == Wire::WireStatus::SUCCESS);

  const Wire::DecodeResult decoded =
      Wire::decode_v1(
          encoded._bytes.data(),
          encoded._bytes.size());

  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());
  assert_key_equals_test_key(decoded._record->_data._key);
  assert(numeric_value(*decoded._record)._value == 0xa1b2c3d4U);
  assert(numeric_value(*decoded._record)._state == 1U);
  assert(decoded._record->_data._description.empty());
  assert(!decoded._record->_changed_at.has_value());
}

/*
 * V1 字符串报文由 18 字节头部、完整内容和一个 NUL 组成。
 */
void test_string_encoding_matches_legacy_bytes() {
  const std::vector<std::uint8_t> expected{
      0x01, 0x01,                         // version, string type
      0x11, 0x22, 0x33, 0x44,             // mid
      0x55, 0x66, 0x77, 0x88,             // fid
      0x00, 0x00, 0x00, 0x03,             // eid
      0x00, 0x00, 0x00, 0x04,             // level
      0x68, 0x65, 0x6c, 0x6c, 0x6f,       // "hello"
      0x20,                               // space
      0x77, 0x6f, 0x72, 0x6c, 0x64,       // "world"
      0x00};                              // trailing NUL

  const Wire::EncodeResult result =
      Wire::encode_v1(make_string_record("hello world"));

  assert(result._status == Wire::WireStatus::SUCCESS);
  assert(result._bytes == expected);
}

void test_string_round_trip() {
  const Wire::EncodeResult encoded =
      Wire::encode_v1(make_string_record("ready"));

  assert(encoded._status == Wire::WireStatus::SUCCESS);

  const Wire::DecodeResult decoded =
      Wire::decode_v1(
          encoded._bytes.data(),
          encoded._bytes.size());

  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());
  assert(decoded._record->_version == Wire::WireVersion::V1);
  assert_key_equals_test_key(decoded._record->_data._key);
  assert(string_value(*decoded._record) == "ready");
  assert(decoded._record->_data._description.empty());
  assert(!decoded._record->_changed_at.has_value());
}

void test_empty_string_round_trip() {
  const Wire::EncodeResult encoded =
      Wire::encode_v1(make_string_record(""));

  assert(encoded._status == Wire::WireStatus::SUCCESS);
  assert(encoded._bytes.size() == Wire::V1_HEADER_SIZE + 1U);
  assert(encoded._bytes.back() == 0U);

  const Wire::DecodeResult decoded =
      Wire::decode_v1(
          encoded._bytes.data(),
          encoded._bytes.size());

  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());
  assert(string_value(*decoded._record).empty());
}

void test_maximum_63_byte_string_round_trip() {
  const std::string source(63U, 'A');
  const Wire::EncodeResult encoded =
      Wire::encode_v1(make_string_record(source));

  assert(encoded._status == Wire::WireStatus::SUCCESS);
  assert(encoded._bytes.size() == Wire::V1_MAX_DATAGRAM_SIZE);
  assert(encoded._bytes.back() == 0U);

  const Wire::DecodeResult decoded =
      Wire::decode_v1(
          encoded._bytes.data(),
          encoded._bytes.size());

  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());
  assert(string_value(*decoded._record) == source);
}

void test_string_encoder_rejects_more_than_63_bytes() {
  for (const std::size_t length : {64U, 1024U}) {
    const Wire::EncodeResult result =
        Wire::encode_v1(
            make_string_record(std::string(length, 'B')));

    assert(result._status == Wire::WireStatus::STRING_TOO_LONG);
    assert(result._bytes.empty());
  }
}

/*
 * 内嵌 NUL 字符串由 V2 支持。
 */
void test_embedded_nul_uses_legacy_prefix() {
  const std::string source{"ab\0cd", 5U};
  const Wire::EncodeResult encoded =
      Wire::encode_v1(make_string_record(source));

  assert(encoded._status == Wire::WireStatus::SUCCESS);
  assert(encoded._bytes.size() == Wire::V1_HEADER_SIZE + 3U);
  assert(encoded._bytes[Wire::V1_HEADER_SIZE] ==
         static_cast<std::uint8_t>('a'));
  assert(encoded._bytes[Wire::V1_HEADER_SIZE + 1U] ==
         static_cast<std::uint8_t>('b'));
  assert(encoded._bytes.back() == 0U);

  const Wire::DecodeResult decoded =
      Wire::decode_v1(
          encoded._bytes.data(),
          encoded._bytes.size());

  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());
  assert(string_value(*decoded._record) == "ab");
}

/*
 * V1 不发送 description 和 timestamp；只改变元数据不能改变报文。
 */
void test_metadata_does_not_change_v1_bytes() {
  MonData::StoredRecord numeric_a = make_numeric_record();
  MonData::StoredRecord numeric_b = numeric_a;
  numeric_b._data._description = "changed";
  numeric_b._changed_at += std::chrono::hours{24};

  const Wire::EncodeResult numeric_a_encoded =
      Wire::encode_v1(numeric_a);
  const Wire::EncodeResult numeric_b_encoded =
      Wire::encode_v1(numeric_b);

  assert(numeric_a_encoded._status == Wire::WireStatus::SUCCESS);
  assert(numeric_b_encoded._status == Wire::WireStatus::SUCCESS);
  assert(numeric_a_encoded._bytes == numeric_b_encoded._bytes);

  MonData::StoredRecord string_a = make_string_record("same");
  MonData::StoredRecord string_b = string_a;
  string_b._data._description = "changed";
  string_b._changed_at += std::chrono::hours{24};

  const Wire::EncodeResult string_a_encoded =
      Wire::encode_v1(string_a);
  const Wire::EncodeResult string_b_encoded =
      Wire::encode_v1(string_b);

  assert(string_a_encoded._status == Wire::WireStatus::SUCCESS);
  assert(string_b_encoded._status == Wire::WireStatus::SUCCESS);
  assert(string_a_encoded._bytes == string_b_encoded._bytes);
}

/*
 * 26～82 字节范围内的数值尾部扩展必须忽略。
 */
void test_numeric_extension_is_ignored() {
  std::vector<std::uint8_t> bytes = numeric_golden_bytes();
  bytes.resize(Wire::V1_MAX_DATAGRAM_SIZE, 0xa5U);

  const Wire::DecodeResult result =
      Wire::decode_v1(bytes.data(), bytes.size());

  assert(result._status == Wire::WireStatus::SUCCESS);
  assert(result._record.has_value());
  assert(numeric_value(*result._record)._value == TEST_NUMERIC_VALUE);
  assert(numeric_value(*result._record)._state == TEST_NUMERIC_STATE);
}

void test_datagram_over_82_bytes_is_rejected() {
  std::vector<std::uint8_t> bytes = numeric_golden_bytes();
  bytes.resize(Wire::V1_MAX_DATAGRAM_SIZE + 1U, 0U);

  const Wire::DecodeResult result =
      Wire::decode_v1(bytes.data(), bytes.size());

  assert_decode_failure(
      result,
      Wire::WireStatus::DATAGRAM_TOO_LARGE);
}

void test_unknown_type_is_rejected() {
  const std::vector<std::uint8_t> bytes =
      make_raw_v1_header(0xffU);

  const Wire::DecodeResult result =
      Wire::decode_v1(bytes.data(), bytes.size());

  assert_decode_failure(
      result,
      Wire::WireStatus::INVALID_TYPE);
}

/*
 * 字符串载荷没有 NUL 时，解码器必须保存全部有效载荷。
 */
void test_string_without_nul_uses_entire_payload() {
  std::vector<std::uint8_t> bytes = make_raw_v1_header(
      static_cast<std::uint8_t>(Wire::WireValueType::STRING));

  bytes.push_back(static_cast<std::uint8_t>('a'));
  bytes.push_back(static_cast<std::uint8_t>('b'));
  bytes.push_back(static_cast<std::uint8_t>('c'));

  const Wire::DecodeResult result =
      Wire::decode_v1(bytes.data(), bytes.size());

  assert(result._status == Wire::WireStatus::SUCCESS);
  assert(result._record.has_value());
  assert(string_value(*result._record) == "abc");
}

void test_maximum_string_without_nul_is_valid() {
  std::vector<std::uint8_t> bytes = make_raw_v1_header(
      static_cast<std::uint8_t>(Wire::WireValueType::STRING));

  bytes.insert(bytes.end(), 63U, static_cast<std::uint8_t>('C'));

  assert(bytes.size() == 81U);

  const Wire::DecodeResult result =
      Wire::decode_v1(bytes.data(), bytes.size());

  assert(result._status == Wire::WireStatus::SUCCESS);
  assert(result._record.has_value());
  assert(string_value(*result._record) == std::string(63U, 'C'));
}

void test_64_byte_string_without_nul_is_rejected() {
  std::vector<std::uint8_t> bytes = make_raw_v1_header(
      static_cast<std::uint8_t>(Wire::WireValueType::STRING));

  bytes.insert(bytes.end(), 64U, static_cast<std::uint8_t>('D'));

  assert(bytes.size() == Wire::V1_MAX_DATAGRAM_SIZE);

  const Wire::DecodeResult result =
      Wire::decode_v1(bytes.data(), bytes.size());

  assert_decode_failure(
      result,
      Wire::WireStatus::STRING_TOO_LONG);
}

void test_string_stops_at_first_nul() {
  std::vector<std::uint8_t> bytes = make_raw_v1_header(
      static_cast<std::uint8_t>(Wire::WireValueType::STRING));

  const std::vector<std::uint8_t> payload{
      static_cast<std::uint8_t>('a'),
      static_cast<std::uint8_t>('b'),
      0U,
      static_cast<std::uint8_t>('c'),
      static_cast<std::uint8_t>('d')};

  bytes.insert(bytes.end(), payload.begin(), payload.end());

  const Wire::DecodeResult result =
      Wire::decode_v1(bytes.data(), bytes.size());

  assert(result._status == Wire::WireStatus::SUCCESS);
  assert(result._record.has_value());
  assert(string_value(*result._record) == "ab");
}

void test_empty_wrong_version_and_short_header_errors() {
  assert_decode_failure(
      Wire::decode_v1(nullptr, 0U),
      Wire::WireStatus::EMPTY_INPUT);

  const std::vector<std::uint8_t> wrong_version{
      static_cast<std::uint8_t>(Wire::WireVersion::V2)};

  assert_decode_failure(
      Wire::decode_v1(
          wrong_version.data(),
          wrong_version.size()),
      Wire::WireStatus::WRONG_VERSION);

  std::vector<std::uint8_t> short_header(
      Wire::V1_HEADER_SIZE - 1U,
      0U);
  short_header[0] =
      static_cast<std::uint8_t>(Wire::WireVersion::V1);

  assert_decode_failure(
      Wire::decode_v1(
          short_header.data(),
          short_header.size()),
      Wire::WireStatus::SHORT_HEADER);
}

void test_short_numeric_payload_is_rejected() {
  std::vector<std::uint8_t> bytes = numeric_golden_bytes();
  bytes.resize(Wire::V1_NUMERIC_DATAGRAM_SIZE - 1U);

  const Wire::DecodeResult result =
      Wire::decode_v1(bytes.data(), bytes.size());

  assert_decode_failure(
      result,
      Wire::WireStatus::INVALID_FIELD_LENGTH);
}

/*
 * 只有字符串头部、没有任何载荷时，“全部有效载荷”为空字符串。
 */
void test_header_only_string_decodes_as_empty() {
  const std::vector<std::uint8_t> bytes =
      make_raw_v1_header(
          static_cast<std::uint8_t>(
              Wire::WireValueType::STRING));

  const Wire::DecodeResult result =
      Wire::decode_v1(bytes.data(), bytes.size());

  assert(result._status == Wire::WireStatus::SUCCESS);
  assert(result._record.has_value());
  assert(string_value(*result._record).empty());
}

} // namespace

int main() {
  test_numeric_encoding_matches_legacy_bytes();
  test_numeric_decoding_matches_legacy_bytes();
  test_numeric_round_trip();
  test_string_encoding_matches_legacy_bytes();
  test_string_round_trip();
  test_empty_string_round_trip();
  test_maximum_63_byte_string_round_trip();
  test_string_encoder_rejects_more_than_63_bytes();
  test_embedded_nul_uses_legacy_prefix();
  test_metadata_does_not_change_v1_bytes();
  test_numeric_extension_is_ignored();
  test_datagram_over_82_bytes_is_rejected();
  test_unknown_type_is_rejected();
  test_string_without_nul_uses_entire_payload();
  test_maximum_string_without_nul_is_valid();
  test_64_byte_string_without_nul_is_rejected();
  test_string_stops_at_first_nul();
  test_empty_wrong_version_and_short_header_errors();
  test_short_numeric_payload_is_rejected();
  test_header_only_string_decodes_as_empty();
  return 0;
}
