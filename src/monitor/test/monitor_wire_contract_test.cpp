#include "monitor_data.h"
#include "monitor_wire.h"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace TLSSMON;

namespace {

/*
 *
 * 这些 static_assert 不运行任何编码逻辑；它们用于阻止后续实现随意
 * 改动版本号、值类型、结果结构或函数签名。
 */
static_assert(std::is_enum_v<Wire::WireVersion>,
              "WireVersion must be an enum");
static_assert(
    std::is_same_v<std::underlying_type_t<Wire::WireVersion>, std::uint8_t>,
    "WireVersion must use uint8_t");
static_assert(static_cast<std::uint8_t>(Wire::WireVersion::V1) == 1,
              "V1 wire version must be 1");
static_assert(static_cast<std::uint8_t>(Wire::WireVersion::V2) == 2,
              "V2 wire version must be 2");

static_assert(std::is_enum_v<Wire::WireValueType>,
              "WireValueType must be an enum");
static_assert(
    std::is_same_v<std::underlying_type_t<Wire::WireValueType>, std::uint8_t>,
    "WireValueType must use uint8_t");
static_assert(static_cast<std::uint8_t>(Wire::WireValueType::NUMERIC) == 0,
              "numeric wire type must be 0");
static_assert(static_cast<std::uint8_t>(Wire::WireValueType::STRING) == 1,
              "string wire type must be 1");

constexpr Wire::WireStatus ALL_WIRE_STATUSES[]{
    Wire::WireStatus::SUCCESS,
    Wire::WireStatus::EMPTY_INPUT,
    Wire::WireStatus::WRONG_VERSION,
    Wire::WireStatus::INVALID_TYPE,
    Wire::WireStatus::INVALID_FLAGS,
    Wire::WireStatus::SHORT_HEADER,
    Wire::WireStatus::INVALID_HEADER_LENGTH,
    Wire::WireStatus::INVALID_TOTAL_LENGTH,
    Wire::WireStatus::INVALID_FIELD_LENGTH,
    Wire::WireStatus::INVALID_TIMESTAMP,
    Wire::WireStatus::STRING_TOO_LONG,
    Wire::WireStatus::DATAGRAM_TOO_LARGE};

static_assert(std::size(ALL_WIRE_STATUSES) == 12,
              "WireStatus contract changed");

static_assert(
    std::is_same_v<decltype(std::declval<Wire::DecodedRecord &>()._version),
                   Wire::WireVersion>,
    "DecodedRecord::_version type changed");
static_assert(
    std::is_same_v<decltype(std::declval<Wire::DecodedRecord &>()._data),
                   MonData::MonitorData>,
    "DecodedRecord::_data type changed");
static_assert(
    std::is_same_v<decltype(std::declval<Wire::DecodedRecord &>()._changed_at),
                   std::optional<MonData::MonitorTimestamp>>,
    "DecodedRecord::_changed_at type changed");

static_assert(
    std::is_same_v<decltype(std::declval<Wire::EncodeResult &>()._status),
                   Wire::WireStatus>,
    "EncodeResult::_status type changed");
static_assert(
    std::is_same_v<decltype(std::declval<Wire::EncodeResult &>()._bytes),
                   std::vector<std::uint8_t>>,
    "EncodeResult::_bytes type changed");
static_assert(
    std::is_same_v<decltype(std::declval<Wire::DecodeResult &>()._status),
                   Wire::WireStatus>,
    "DecodeResult::_status type changed");
static_assert(
    std::is_same_v<decltype(std::declval<Wire::DecodeResult &>()._record),
                   std::optional<Wire::DecodedRecord>>,
    "DecodeResult::_record type changed");

using ExpectedEncodeV1 = Wire::EncodeResult (*)(const MonData::StoredRecord &);
using ExpectedEncodeV2 = Wire::EncodeResult (*)(const MonData::StoredRecord &);
using ExpectedDecodeV1 = Wire::DecodeResult (*)(const std::uint8_t *,
                                                std::size_t);
using ExpectedDecodeV2 = Wire::DecodeResult (*)(const std::uint8_t *,
                                                std::size_t);
using ExpectedEncode = Wire::EncodeResult (*)(const MonData::StoredRecord &,
                                              Wire::WireVersion);
using ExpectedDecode = Wire::DecodeResult (*)(const std::uint8_t *,
                                              std::size_t);

static_assert(std::is_same_v<decltype(&Wire::encode_v1), ExpectedEncodeV1>,
              "encode_v1 signature changed");
static_assert(std::is_same_v<decltype(&Wire::encode_v2), ExpectedEncodeV2>,
              "encode_v2 signature changed");
static_assert(std::is_same_v<decltype(&Wire::decode_v1), ExpectedDecodeV1>,
              "decode_v1 signature changed");
static_assert(std::is_same_v<decltype(&Wire::decode_v2), ExpectedDecodeV2>,
              "decode_v2 signature changed");
static_assert(std::is_same_v<decltype(&Wire::encode), ExpectedEncode>,
              "encode signature changed");
static_assert(std::is_same_v<decltype(&Wire::decode), ExpectedDecode>,
              "decode signature changed");

static_assert(std::is_move_constructible_v<Wire::EncodeResult>,
              "EncodeResult must be move constructible");
static_assert(std::is_move_constructible_v<Wire::DecodeResult>,
              "DecodeResult must be move constructible");

/*
 * V1：18 字节头部；数值报文固定 26 字节；字符串报文最多 82 字节。
 * V2：固定头部 40 字节；完整 UDP 数据报最多 1200 字节。
 */
static_assert(std::is_same_v<std::remove_cv_t<decltype(Wire::V1_HEADER_SIZE)>,
                             std::size_t>,
              "V1_HEADER_SIZE must use std::size_t");
static_assert(
    std::is_same_v<std::remove_cv_t<decltype(Wire::V1_NUMERIC_DATAGRAM_SIZE)>,
                   std::size_t>,
    "V1_NUMERIC_DATAGRAM_SIZE must use std::size_t");
static_assert(
    std::is_same_v<std::remove_cv_t<decltype(Wire::V1_MAX_DATAGRAM_SIZE)>,
                   std::size_t>,
    "V1_MAX_DATAGRAM_SIZE must use std::size_t");
static_assert(std::is_same_v<std::remove_cv_t<decltype(Wire::V2_HEADER_SIZE)>,
                             std::size_t>,
              "V2_HEADER_SIZE must use std::size_t");
static_assert(
    std::is_same_v<std::remove_cv_t<decltype(Wire::V2_MAX_DATAGRAM_SIZE)>,
                   std::size_t>,
    "V2_MAX_DATAGRAM_SIZE must use std::size_t");

static_assert(Wire::V1_HEADER_SIZE == 18,
              "V1 header must be exactly 18 bytes");
static_assert(Wire::V1_NUMERIC_DATAGRAM_SIZE == 26,
              "V1 numeric datagram must be exactly 26 bytes");
static_assert(Wire::V1_NUMERIC_DATAGRAM_SIZE == Wire::V1_HEADER_SIZE + 8,
              "V1 numeric payload must contain value and state");
static_assert(Wire::V1_MAX_DATAGRAM_SIZE == 82,
              "V1 maximum datagram must be exactly 82 bytes");
static_assert(Wire::V1_MAX_DATAGRAM_SIZE == Wire::V1_HEADER_SIZE + 63 + 1,
              "V1 string payload must allow 63 bytes plus NUL");
static_assert(Wire::V1_NUMERIC_DATAGRAM_SIZE < Wire::V1_MAX_DATAGRAM_SIZE,
              "V1 numeric size and maximum size are different concepts");

static_assert(Wire::V2_HEADER_SIZE == 40,
              "V2 header must be exactly 40 bytes");
static_assert(Wire::V2_MAX_DATAGRAM_SIZE == 1200,
              "V2 maximum datagram must be exactly 1200 bytes");
static_assert(Wire::V2_HEADER_SIZE < Wire::V2_MAX_DATAGRAM_SIZE,
              "V2 header must fit in a V2 datagram");
static_assert(Wire::V2_MAX_DATAGRAM_SIZE - Wire::V2_HEADER_SIZE == 1160,
              "V2 description and value must share 1160 payload bytes");
static_assert(
    Wire::V2_MAX_DATAGRAM_SIZE <=
        static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()),
    "V2 total length must fit in uint16_t");
static_assert(Wire::V1_MAX_DATAGRAM_SIZE <= Wire::V2_MAX_DATAGRAM_SIZE,
              "a V2-sized receive buffer must also hold V1 datagrams");

/*
 *
 * MonitorKey 的 C++ 字段顺序是 mid -> level -> fid -> eid；旧 M6 V1
 * 的线协议顺序则是 mid -> fid(hid) -> eid -> level。选用不同的数值，
 * 可以发现实现误把结构体内存直接复制到报文中的问题。
 */
inline constexpr std::uint32_t GOLDEN_MID = 0x11223344U;
inline constexpr std::uint32_t GOLDEN_LEVEL = 4U;
inline constexpr std::uint32_t GOLDEN_FID = 0x55667788U;
inline constexpr std::uint32_t GOLDEN_EID = 3U;
inline constexpr std::uint32_t GOLDEN_NUMERIC_VALUE = 12345U;
inline constexpr std::uint32_t GOLDEN_NUMERIC_STATE = 2U;
inline constexpr const char *GOLDEN_DESCRIPTION = "not-sent-by-v1";
const MonData::MonitorTimestamp GOLDEN_TIMESTAMP{};

MonData::StoredRecord make_numeric_golden_record() {
  return MonData::StoredRecord{
      MonData::MonitorData{
          MonData::MonitorKey{GOLDEN_MID, GOLDEN_LEVEL, GOLDEN_FID, GOLDEN_EID},
          GOLDEN_DESCRIPTION,
          MonData::NumericValue{GOLDEN_NUMERIC_VALUE, GOLDEN_NUMERIC_STATE}},
      GOLDEN_TIMESTAMP};
}

/*
 * 验证数值 golden fixture 本身的 Key、description、timestamp、value 和
 * state，避免测试夹具构造错误导致后续协议测试得出错误结论。
 */
void test_numeric_golden_fixture() {
  const MonData::StoredRecord record = make_numeric_golden_record();

  assert(record._data._key._mid == GOLDEN_MID);
  assert(record._data._key._level == GOLDEN_LEVEL);
  assert(record._data._key._fid == GOLDEN_FID);
  assert(record._data._key._eid == GOLDEN_EID);
  assert(record._data._description == GOLDEN_DESCRIPTION);
  assert(record._changed_at == GOLDEN_TIMESTAMP);

  const auto *numeric =
      std::get_if<MonData::NumericValue>(&record._data._value);
  assert(numeric != nullptr);
  assert(numeric->_value == GOLDEN_NUMERIC_VALUE);
  assert(numeric->_state == GOLDEN_NUMERIC_STATE);
}

/*
 *
 * 该案例同时验证版本、类型、字段顺序、所有 uint32_t 的大端序，
 * 以及 V1 不编码 description 和 timestamp。
 */
void test_v1_numeric_matches_legacy_bytes() {
  const std::vector<std::uint8_t> expected{
      0x01, 0x00,                         // version, numeric type
      0x11, 0x22, 0x33, 0x44,             // mid
      0x55, 0x66, 0x77, 0x88,             // fid (legacy hid)
      0x00, 0x00, 0x00, 0x03,             // eid
      0x00, 0x00, 0x00, 0x04,             // level
      0x00, 0x00, 0x30, 0x39,             // value = 12345
      0x00, 0x00, 0x00, 0x02};            // state = fresh

  const Wire::EncodeResult result =
      Wire::encode_v1(make_numeric_golden_record());

  assert(result._status == Wire::WireStatus::SUCCESS);
  assert(result._bytes.size() == Wire::V1_NUMERIC_DATAGRAM_SIZE);
  assert(result._bytes.size() == 26U);
  assert(result._bytes == expected);
}

/*
 *
 * V1 字符串由 18 字节头部、完整字符串内容和一个结尾 NUL 组成。
 */
inline constexpr std::uint32_t STRING_MID = 1U;
inline constexpr std::uint32_t STRING_LEVEL = 5U;
inline constexpr std::uint32_t STRING_FID = 2U;
inline constexpr std::uint32_t STRING_EID = 3U;
inline constexpr const char *STRING_VALUE = "hello world";
inline constexpr const char *STRING_DESCRIPTION =
    "v1-description-not-on-wire";

const MonData::MonitorTimestamp STRING_TIMESTAMP{
    std::chrono::seconds{123456789}};

MonData::StoredRecord make_string_golden_record(
    std::string value = STRING_VALUE) {
  return MonData::StoredRecord{
      MonData::MonitorData{
          MonData::MonitorKey{STRING_MID, STRING_LEVEL, STRING_FID, STRING_EID},
          STRING_DESCRIPTION,
          std::move(value)},
      STRING_TIMESTAMP};
}

/*
 * 验证字符串 golden fixture 本身保存了预期的 Key、description、
 * timestamp 和字符串值，确保后续字节比较建立在正确输入上。
 */
void test_string_golden_fixture() {
  const MonData::StoredRecord record = make_string_golden_record();

  assert(record._data._key._mid == STRING_MID);
  assert(record._data._key._level == STRING_LEVEL);
  assert(record._data._key._fid == STRING_FID);
  assert(record._data._key._eid == STRING_EID);
  assert(record._data._description == STRING_DESCRIPTION);
  assert(record._changed_at == STRING_TIMESTAMP);

  const auto *value = std::get_if<std::string>(&record._data._value);
  assert(value != nullptr);
  assert(*value == STRING_VALUE);
}

/*
 * 将 V1 字符串编码结果与固定字节序列比较，验证版本、类型、Key 字段
 * 顺序、字符串内容以及结尾 NUL 均与旧协议一致。
 */
void test_v1_string_matches_legacy_bytes() {
  const std::vector<std::uint8_t> expected{
      0x01, 0x01,                         // version, string type
      0x00, 0x00, 0x00, 0x01,             // mid
      0x00, 0x00, 0x00, 0x02,             // fid (legacy hid)
      0x00, 0x00, 0x00, 0x03,             // eid
      0x00, 0x00, 0x00, 0x05,             // level
      0x68, 0x65, 0x6c, 0x6c, 0x6f,       // "hello"
      0x20,                               // space
      0x77, 0x6f, 0x72, 0x6c, 0x64,       // "world"
      0x00};                              // trailing NUL

  const Wire::EncodeResult result =
      Wire::encode_v1(make_string_golden_record());

  assert(result._status == Wire::WireStatus::SUCCESS);
  assert(result._bytes.size() == Wire::V1_HEADER_SIZE + 11U + 1U);
  assert(result._bytes.size() == 30U);
  assert(result._bytes == expected);
}

/*
 * V1 不携带 description 和 timestamp；仅改变这两个字段，不得改变字节。
 */
void test_v1_string_ignores_metadata() {
  MonData::StoredRecord record = make_string_golden_record();
  const Wire::EncodeResult original = Wire::encode_v1(record);

  record._data._description = "another description";
  record._changed_at += std::chrono::hours{24};
  const Wire::EncodeResult changed_metadata = Wire::encode_v1(record);

  assert(original._status == Wire::WireStatus::SUCCESS);
  assert(changed_metadata._status == Wire::WireStatus::SUCCESS);
  assert(changed_metadata._bytes == original._bytes);
}

/*
 *
 * 空字符串是合法值，报文仍需保留结尾 NUL，因此长度为 18 + 1。
 */
void test_v1_empty_string_is_valid() {
  const Wire::EncodeResult result =
      Wire::encode_v1(make_string_golden_record(""));

  assert(result._status == Wire::WireStatus::SUCCESS);
  assert(result._bytes.size() == Wire::V1_HEADER_SIZE + 1U);
  assert(result._bytes.size() == 19U);
  assert(result._bytes.at(0) ==
         static_cast<std::uint8_t>(Wire::WireVersion::V1));
  assert(result._bytes.at(1) ==
         static_cast<std::uint8_t>(Wire::WireValueType::STRING));
  assert(result._bytes.back() == 0U);
}

/*
 * 63 字节是 V1 可保存的最大字符串内容；加上 NUL 后恰好达到 82 字节。
 */
void test_v1_accepts_63_byte_string() {
  const std::string value(63U, 'A');
  const Wire::EncodeResult result =
      Wire::encode_v1(make_string_golden_record(value));

  assert(result._status == Wire::WireStatus::SUCCESS);
  assert(result._bytes.size() == Wire::V1_MAX_DATAGRAM_SIZE);
  for (std::size_t index = Wire::V1_HEADER_SIZE;
       index < Wire::V1_HEADER_SIZE + value.size(); ++index) {
    assert(result._bytes.at(index) == static_cast<std::uint8_t>('A'));
  }
  assert(result._bytes.back() == 0U);
}

/*
 * 64 字节内容需要 18 + 64 + 1 = 83 字节，超过旧协议上限。
 * 编码器必须返回明确错误，且不能返回部分数据报。
 */
void test_v1_rejects_64_byte_string() {
  const std::string value(64U, 'B');
  const MonData::StoredRecord record = make_string_golden_record(value);
  const Wire::EncodeResult result = Wire::encode_v1(record);

  assert(result._status == Wire::WireStatus::STRING_TOO_LONG);
  assert(result._bytes.empty());

  // 编码失败也不能改写调用方保存的字符串。
  const auto *stored_value =
      std::get_if<std::string>(&record._data._value);
  assert(stored_value != nullptr);
  assert(*stored_value == value);
}

/*
 * 超长输入与刚越界输入采用相同错误，不允许截断后伪装成成功报文。
 */
void test_v1_rejects_much_longer_string() {
  const std::string value(1024U, 'C');
  const MonData::StoredRecord record = make_string_golden_record(value);
  const Wire::EncodeResult result = Wire::encode_v1(record);

  assert(result._status == Wire::WireStatus::STRING_TOO_LONG);
  assert(result._bytes.empty());

  const auto *stored_value =
      std::get_if<std::string>(&record._data._value);
  assert(stored_value != nullptr);
  assert(stored_value->size() == 1024U);
  assert(*stored_value == value);
}

/*
 * 同时检查临界点附近的 62/63 字节，防止实现出现少一位的边界错误。
 */
void test_v1_near_maximum_string_sizes() {
  const Wire::EncodeResult length_62 =
      Wire::encode_v1(make_string_golden_record(std::string(62U, 'X')));
  const Wire::EncodeResult length_63 =
      Wire::encode_v1(make_string_golden_record(std::string(63U, 'Y')));

  assert(length_62._status == Wire::WireStatus::SUCCESS);
  assert(length_62._bytes.size() == 81U);
  assert(length_62._bytes.back() == 0U);

  assert(length_63._status == Wire::WireStatus::SUCCESS);
  assert(length_63._bytes.size() == Wire::V1_MAX_DATAGRAM_SIZE);
  assert(length_63._bytes.back() == 0U);
}

/*
 * 固定 V2 数值报文格式，防止后续修改意外改变公共线协议布局。
 *
 * V2 报文布局：
 *
 * 0～39   ：固定头部
 * 40～41  ：description，即 "v2"
 * 42～49  ：数值 value 和 state
 *
 * 总长度为 40 + 2 + 8 = 50 字节。
 */
inline constexpr const char *V2_GOLDEN_DESCRIPTION = "v2";

/*
 * 使用非零时间戳验证 64 位 seconds 和 32 位 nanoseconds 的大端序。
 * 0x05060708 等于 84281096，处于合法纳秒范围内。
 */
inline constexpr std::int64_t V2_GOLDEN_SECONDS = 0x01020304LL;
inline constexpr std::int64_t V2_GOLDEN_NANOSECONDS = 0x05060708LL;

MonData::StoredRecord make_v2_numeric_golden_record() {
  MonData::StoredRecord record = make_numeric_golden_record();

  record._data._description = V2_GOLDEN_DESCRIPTION;

  const auto elapsed =
      std::chrono::seconds{V2_GOLDEN_SECONDS} +
      std::chrono::nanoseconds{V2_GOLDEN_NANOSECONDS};

  /*
   * 不假设 system_clock::duration 的具体精度，显式转换到项目使用的
   * MonitorTimestamp::duration。
   */
  record._changed_at = MonData::MonitorTimestamp{
      std::chrono::duration_cast<MonData::MonitorTimestamp::duration>(
          elapsed)};

  return record;
}

/*
 * 单独验证测试夹具，避免夹具字段构造错误掩盖 Codec 的真实问题。
 */
void test_v2_numeric_golden_fixture() {
  const MonData::StoredRecord record =
      make_v2_numeric_golden_record();

  assert(record._data._key._mid == GOLDEN_MID);
  assert(record._data._key._level == GOLDEN_LEVEL);
  assert(record._data._key._fid == GOLDEN_FID);
  assert(record._data._key._eid == GOLDEN_EID);

  assert(record._data._description == V2_GOLDEN_DESCRIPTION);
  assert(record._data._description.size() == 2U);

  const auto *numeric =
      std::get_if<MonData::NumericValue>(&record._data._value);

  assert(numeric != nullptr);
  assert(numeric->_value == GOLDEN_NUMERIC_VALUE);
  assert(numeric->_state == GOLDEN_NUMERIC_STATE);

  const auto elapsed = record._changed_at.time_since_epoch();
  const auto seconds =
      std::chrono::duration_cast<std::chrono::seconds>(elapsed);
  const auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          elapsed - seconds);

  assert(seconds.count() == V2_GOLDEN_SECONDS);
  assert(nanoseconds.count() == V2_GOLDEN_NANOSECONDS);
}

/*
 * 固定 V2 数值报文的完整字节序列。
 *
 * 该测试同时验证：
 *
 * - version、type 和 flags；
 * - total_length 和 40 字节 header_length；
 * - Key 顺序为 mid -> level -> fid -> eid；
 * - uint16、uint32 和 uint64 使用大端序；
 * - description 使用显式长度且不带 NUL；
 * - NumericValue 固定占 8 字节。
 */
void test_v2_numeric_matches_golden_bytes() {
  const std::vector<std::uint8_t> expected{
      0x02,                               // version = V2
      0x00,                               // type = NUMERIC
      0x00, 0x00,                         // flags = 0
      0x00, 0x32,                         // total_length = 50
      0x00, 0x28,                         // header_length = 40
      0x11, 0x22, 0x33, 0x44,             // mid
      0x00, 0x00, 0x00, 0x04,             // level
      0x55, 0x66, 0x77, 0x88,             // fid
      0x00, 0x00, 0x00, 0x03,             // eid
      0x00, 0x00, 0x00, 0x00,             // timestamp seconds high bytes
      0x01, 0x02, 0x03, 0x04,             // timestamp seconds low bytes
      0x05, 0x06, 0x07, 0x08,             // timestamp nanoseconds
      0x00, 0x02,                         // description_length = 2
      0x00, 0x08,                         // value_length = 8
      0x76, 0x32,                         // description = "v2"
      0x00, 0x00, 0x30, 0x39,             // value = 12345
      0x00, 0x00, 0x00, 0x02};            // state = fresh

  const Wire::EncodeResult result =
      Wire::encode_v2(make_v2_numeric_golden_record());

  assert(result._status == Wire::WireStatus::SUCCESS);
  assert(result._bytes.size() == Wire::V2_HEADER_SIZE + 2U + 8U);
  assert(result._bytes.size() == 50U);
  assert(result._bytes == expected);
}

/*
 * 验证统一编码入口选择 V1 时与直接调用 encode_v1() 完全等价，包含
 * 相同状态、相同字节和正确的版本字节。
 *
 * 发送端必须显式选择版本：
 *
 * encode(record, V1) -> encode_v1(record)
 * encode(record, V2) -> encode_v2(record)
 *
 * 接收端根据数据报第 0 字节选择解码器：
 *
 * data[0] == 1 -> decode_v1()
 * data[0] == 2 -> decode_v2()
 */
void test_unified_encode_dispatches_v1() {
  const MonData::StoredRecord record =
      make_numeric_golden_record();

  const Wire::EncodeResult direct = Wire::encode_v1(record);
  const Wire::EncodeResult unified =
      Wire::encode(record, Wire::WireVersion::V1);

  assert(direct._status == Wire::WireStatus::SUCCESS);
  assert(unified._status == Wire::WireStatus::SUCCESS);
  assert(unified._status == direct._status);
  assert(unified._bytes == direct._bytes);
  assert(unified._bytes.size() == Wire::V1_NUMERIC_DATAGRAM_SIZE);
  assert(!unified._bytes.empty());
  assert(unified._bytes.at(0) ==
         static_cast<std::uint8_t>(Wire::WireVersion::V1));
}

/*
 * 验证统一编码入口选择 V2 时与直接调用 encode_v2() 完全等价，统一
 * 入口只能负责分派，不能重新构造或修改 V2 数据报。
 */
void test_unified_encode_dispatches_v2() {
  const MonData::StoredRecord record =
      make_v2_numeric_golden_record();

  const Wire::EncodeResult direct = Wire::encode_v2(record);
  const Wire::EncodeResult unified =
      Wire::encode(record, Wire::WireVersion::V2);

  assert(direct._status == Wire::WireStatus::SUCCESS);
  assert(unified._status == Wire::WireStatus::SUCCESS);
  assert(unified._status == direct._status);
  assert(unified._bytes == direct._bytes);
  assert(unified._bytes.size() == 50U);
  assert(!unified._bytes.empty());
  assert(unified._bytes.at(0) ==
         static_cast<std::uint8_t>(Wire::WireVersion::V2));
}

/*
 * 统一编码入口只根据 WireVersion 选择编码器，不能因为 value 是字符串
 * 就绕过 V1 编码器或改变旧 V1 的 NUL 终止行为。
 */
void test_unified_encode_dispatches_v1_string() {
  const MonData::StoredRecord record =
      make_string_golden_record("legacy-string");

  const Wire::EncodeResult direct = Wire::encode_v1(record);
  const Wire::EncodeResult unified =
      Wire::encode(record, Wire::WireVersion::V1);

  assert(direct._status == Wire::WireStatus::SUCCESS);
  assert(unified._status == direct._status);
  assert(unified._bytes == direct._bytes);
  assert(!unified._bytes.empty());
  assert(unified._bytes.front() ==
         static_cast<std::uint8_t>(Wire::WireVersion::V1));
  assert(unified._bytes.back() == 0U);
}

/*
 * WireVersion 的底层类型允许调用方构造未声明的枚举值。统一编码入口
 * 必须显式拒绝，且失败结果不能携带任何部分数据报。
 */
void test_unified_encode_rejects_unknown_version() {
  const Wire::EncodeResult result =
      Wire::encode(
          make_numeric_golden_record(),
          static_cast<Wire::WireVersion>(0xffU));

  assert(result._status == Wire::WireStatus::WRONG_VERSION);
  assert(result._bytes.empty());
}

/*
 * V1 不携带 description 和 timestamp。统一入口识别 V1 后，必须恢复
 * Key 和数值，同时将缺失的元数据表达为空。
 */
void test_unified_decode_dispatches_v1() {
  const Wire::EncodeResult encoded =
      Wire::encode_v1(make_numeric_golden_record());

  assert(encoded._status == Wire::WireStatus::SUCCESS);
  assert(!encoded._bytes.empty());
  assert(encoded._bytes.at(0) ==
         static_cast<std::uint8_t>(Wire::WireVersion::V1));

  const Wire::DecodeResult decoded =
      Wire::decode(encoded._bytes.data(), encoded._bytes.size());

  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());

  const Wire::DecodedRecord &record = *decoded._record;

  assert(record._version == Wire::WireVersion::V1);
  assert(record._data._key._mid == GOLDEN_MID);
  assert(record._data._key._level == GOLDEN_LEVEL);
  assert(record._data._key._fid == GOLDEN_FID);
  assert(record._data._key._eid == GOLDEN_EID);
  assert(record._data._description.empty());
  assert(!record._changed_at.has_value());

  const auto *numeric =
      std::get_if<MonData::NumericValue>(&record._data._value);

  assert(numeric != nullptr);
  assert(numeric->_value == GOLDEN_NUMERIC_VALUE);
  assert(numeric->_state == GOLDEN_NUMERIC_STATE);
}

/*
 * V2 携带完整元数据。统一入口识别 V2 后，必须恢复 Key、description、
 * timestamp、value 和 state。
 */
void test_unified_decode_dispatches_v2() {
  const MonData::StoredRecord source =
      make_v2_numeric_golden_record();

  const Wire::EncodeResult encoded = Wire::encode_v2(source);

  assert(encoded._status == Wire::WireStatus::SUCCESS);
  assert(!encoded._bytes.empty());
  assert(encoded._bytes.at(0) ==
         static_cast<std::uint8_t>(Wire::WireVersion::V2));

  const Wire::DecodeResult decoded =
      Wire::decode(encoded._bytes.data(), encoded._bytes.size());

  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());

  const Wire::DecodedRecord &record = *decoded._record;

  assert(record._version == Wire::WireVersion::V2);
  assert(record._data._key._mid == source._data._key._mid);
  assert(record._data._key._level == source._data._key._level);
  assert(record._data._key._fid == source._data._key._fid);
  assert(record._data._key._eid == source._data._key._eid);
  assert(record._data._description == source._data._description);
  assert(record._changed_at.has_value());
  assert(*record._changed_at == source._changed_at);

  const auto *decoded_numeric =
      std::get_if<MonData::NumericValue>(&record._data._value);
  const auto *source_numeric =
      std::get_if<MonData::NumericValue>(&source._data._value);

  assert(decoded_numeric != nullptr);
  assert(source_numeric != nullptr);
  assert(decoded_numeric->_value == source_numeric->_value);
  assert(decoded_numeric->_state == source_numeric->_state);
}

/*
 * V2 字符串也必须经过同一个统一入口。内嵌 NUL 用于确认统一分派没有
 * 误入 V1，description、timestamp 和完整字符串都必须保留。
 */
void test_unified_v2_string_round_trip() {
  const std::string source_value{"ab\0cd", 5U};
  const MonData::MonitorTimestamp timestamp{
      std::chrono::seconds{123} +
      std::chrono::nanoseconds{456}};
  const MonData::StoredRecord source{
      MonData::MonitorData{
          MonData::MonitorKey{1U, 2U, 3U, 4U},
          "string-description",
          source_value},
      timestamp};

  const Wire::EncodeResult direct = Wire::encode_v2(source);
  const Wire::EncodeResult unified =
      Wire::encode(source, Wire::WireVersion::V2);

  assert(direct._status == Wire::WireStatus::SUCCESS);
  assert(unified._status == direct._status);
  assert(unified._bytes == direct._bytes);
  assert(!unified._bytes.empty());
  assert(unified._bytes.front() ==
         static_cast<std::uint8_t>(Wire::WireVersion::V2));

  const Wire::DecodeResult decoded =
      Wire::decode(
          unified._bytes.data(),
          unified._bytes.size());

  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());
  assert(decoded._record->_version == Wire::WireVersion::V2);
  assert(decoded._record->_data._key == source._data._key);
  assert(decoded._record->_data._description ==
         source._data._description);
  assert(decoded._record->_changed_at.has_value());
  assert(*decoded._record->_changed_at == timestamp);

  const auto *decoded_value =
      std::get_if<std::string>(
          &decoded._record->_data._value);

  assert(decoded_value != nullptr);
  assert(*decoded_value == source_value);
  assert(decoded_value->size() == 5U);
}

/*
 * 验证统一解码入口对空输入的处理，不允许在没有第 0 字节时尝试判断
 * 版本，同时失败结果不能携带记录。
 *
 * size == 0    -> EMPTY_INPUT
 * data[0] == 1 -> decode_v1()
 * data[0] == 2 -> decode_v2()
 * 其他版本     -> WRONG_VERSION
 */
void test_unified_decode_rejects_empty_input() {
  const Wire::DecodeResult result = Wire::decode(nullptr, 0U);

  assert(result._status == Wire::WireStatus::EMPTY_INPUT);
  assert(!result._record.has_value());
}

/*
 * 即使 size 非零，只要 data 是 nullptr 就不能访问 data[0]，统一入口
 * 必须安全返回 EMPTY_INPUT。
 */
void test_unified_decode_rejects_null_pointer_with_nonzero_size() {
  const Wire::DecodeResult result = Wire::decode(nullptr, 10U);

  assert(result._status == Wire::WireStatus::EMPTY_INPUT);
  assert(!result._record.has_value());
}

/*
 * 统一入口只选择版本，不负责重新解释各版本的头部长度。识别第 0 字节
 * 后，应由对应专用解码器返回 SHORT_HEADER。
 */
void test_unified_decode_delegates_short_header_errors() {
  const std::vector<std::uint8_t> short_v1{
      static_cast<std::uint8_t>(Wire::WireVersion::V1)};

  const Wire::DecodeResult v1_result =
      Wire::decode(short_v1.data(), short_v1.size());

  assert(v1_result._status == Wire::WireStatus::SHORT_HEADER);
  assert(!v1_result._record.has_value());

  const std::vector<std::uint8_t> short_v2{
      static_cast<std::uint8_t>(Wire::WireVersion::V2)};

  const Wire::DecodeResult v2_result =
      Wire::decode(short_v2.data(), short_v2.size());

  assert(v2_result._status == Wire::WireStatus::SHORT_HEADER);
  assert(!v2_result._record.has_value());
}

/*
 * 未知版本必须由统一入口直接拒绝。输入已经包含第 0 字节，因此不能
 * 将它归类为 EMPTY_INPUT 或根据后续长度猜测协议。
 */
void test_unified_decode_rejects_unknown_versions() {
  const std::vector<std::uint8_t> unknown_versions{
      0x00,
      0x03,
      0x7f,
      0xff};

  for (const std::uint8_t version : unknown_versions) {
    const std::vector<std::uint8_t> datagram{version};

    const Wire::DecodeResult result =
        Wire::decode(datagram.data(), datagram.size());

    assert(result._status == Wire::WireStatus::WRONG_VERSION);
    assert(!result._record.has_value());
  }
}

/*
 * 40 字节虽然等于 V2 固定头部长度，但未知版本仍必须返回
 * WRONG_VERSION，不能仅根据数据报长度选择 V2 解码器。
 */
void test_unified_decode_does_not_guess_version_from_size() {
  std::vector<std::uint8_t> datagram(Wire::V2_HEADER_SIZE, 0U);
  datagram.at(0) = 0x03;

  const Wire::DecodeResult result =
      Wire::decode(datagram.data(), datagram.size());

  assert(result._status == Wire::WireStatus::WRONG_VERSION);
  assert(!result._record.has_value());
}

/*
 * 专用 V1 解码器必须检查 version，不能接受合法的 V2 数据报。
 */
void test_decode_v1_rejects_v2_datagram() {
  const Wire::EncodeResult encoded =
      Wire::encode_v2(make_v2_numeric_golden_record());

  assert(encoded._status == Wire::WireStatus::SUCCESS);
  assert(!encoded._bytes.empty());
  assert(encoded._bytes.at(0) ==
         static_cast<std::uint8_t>(Wire::WireVersion::V2));

  const Wire::DecodeResult decoded =
      Wire::decode_v1(encoded._bytes.data(), encoded._bytes.size());

  assert(decoded._status == Wire::WireStatus::WRONG_VERSION);
  assert(!decoded._record.has_value());
}

/*
 * 专用 V2 解码器必须检查 version，不能将 26 字节 V1 数值数据报
 * 仅仅归类为损坏或过短的 V2 数据报。
 */
void test_decode_v2_rejects_v1_datagram() {
  const Wire::EncodeResult encoded =
      Wire::encode_v1(make_numeric_golden_record());

  assert(encoded._status == Wire::WireStatus::SUCCESS);
  assert(!encoded._bytes.empty());
  assert(encoded._bytes.at(0) ==
         static_cast<std::uint8_t>(Wire::WireVersion::V1));

  const Wire::DecodeResult decoded =
      Wire::decode_v2(encoded._bytes.data(), encoded._bytes.size());

  assert(decoded._status == Wire::WireStatus::WRONG_VERSION);
  assert(!decoded._record.has_value());
}

/*
 * 所有版本拒绝结果都必须清空 optional，避免调用方误用未完整构造或
 * 上一次解码残留的记录。
 */
void test_wrong_version_never_returns_record() {
  std::vector<std::uint8_t> datagram(Wire::V2_HEADER_SIZE, 0U);
  datagram.at(0) = 0xff;

  const Wire::DecodeResult v1_result =
      Wire::decode_v1(datagram.data(), datagram.size());
  const Wire::DecodeResult v2_result =
      Wire::decode_v2(datagram.data(), datagram.size());

  assert(v1_result._status == Wire::WireStatus::WRONG_VERSION);
  assert(v2_result._status == Wire::WireStatus::WRONG_VERSION);
  assert(!v1_result._record.has_value());
  assert(!v2_result._record.has_value());
}

} // namespace

int main() {
  test_numeric_golden_fixture();
  test_v1_numeric_matches_legacy_bytes();
  test_string_golden_fixture();
  test_v1_string_matches_legacy_bytes();
  test_v1_string_ignores_metadata();
  test_v1_empty_string_is_valid();
  test_v1_accepts_63_byte_string();
  test_v1_rejects_64_byte_string();
  test_v1_rejects_much_longer_string();
  test_v1_near_maximum_string_sizes();
  test_v2_numeric_golden_fixture();
  test_v2_numeric_matches_golden_bytes();
  test_unified_encode_dispatches_v1();
  test_unified_encode_dispatches_v2();
  test_unified_encode_dispatches_v1_string();
  test_unified_encode_rejects_unknown_version();
  test_unified_decode_dispatches_v1();
  test_unified_decode_dispatches_v2();
  test_unified_v2_string_round_trip();
  test_unified_decode_rejects_empty_input();
  test_unified_decode_rejects_null_pointer_with_nonzero_size();
  test_unified_decode_delegates_short_header_errors();
  test_unified_decode_rejects_unknown_versions();
  test_unified_decode_does_not_guess_version_from_size();
  test_decode_v1_rejects_v2_datagram();
  test_decode_v2_rejects_v1_datagram();
  test_wrong_version_never_returns_record();
  return 0;
}
