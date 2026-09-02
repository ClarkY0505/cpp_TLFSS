#include "monitor_data.h"
#include "monitor_wire.h"
#include "monitor_wire_detail.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
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

static_assert(Wire::V2_HEADER_SIZE == 40U);
static_assert(Wire::V2_MAX_DATAGRAM_SIZE == 1200U);

const MonData::MonitorKey TEST_KEY{
    TEST_MID,
    TEST_LEVEL,
    TEST_FID,
    TEST_EID};

MonData::StoredRecord make_record(
    std::string description,
    MonData::MonitorTimestamp changed_at = MonData::MonitorTimestamp{}) {
  return MonData::StoredRecord{
      MonData::MonitorData{
          TEST_KEY,
          std::move(description),
          MonData::NumericValue{123U, 2U}},
      changed_at};
}

/*
 * 独立构造一个合法 V2 数据报，不经过 write_v2_header()。
 *
 * 读取测试使用它作为输入，避免写入端和读取端出现相同错误时，
 * 往返测试仍然错误地通过。
 */
std::vector<std::uint8_t> make_valid_datagram(
    std::uint16_t description_length = 3U,
    std::uint16_t value_length = 8U) {
  const std::size_t total_length =
      Wire::V2_HEADER_SIZE +
      static_cast<std::size_t>(description_length) +
      static_cast<std::size_t>(value_length);

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
      INT64_C(-2));
  Wire::Detail::put_u32(
      bytes.data() + NANOSECONDS_OFFSET,
      250'000'000U);
  Wire::Detail::put_u16(
      bytes.data() + DESCRIPTION_LENGTH_OFFSET,
      description_length);
  Wire::Detail::put_u16(
      bytes.data() + VALUE_LENGTH_OFFSET,
      value_length);

  return bytes;
}

void assert_read_failure(
    const std::vector<std::uint8_t> &bytes,
    Wire::WireStatus expected_status) {
  const Wire::Detail::V2HeaderResult result =
      Wire::Detail::read_v2_header(bytes.data(), bytes.size());

  assert(result._status == expected_status);
  assert(!result._header.has_value());
}

/*
 * 验证写入端产生精确的 40 字节头部。
 *
 * golden bytes 同时锁定：
 * - version = 2；
 * - numeric type = 0；
 * - flags = 0；
 * - header_length = 40；
 * - Key、时间戳和长度字段全部使用大端序。
 */
void test_writer_matches_v2_header_golden_bytes() {
  const MonData::MonitorTimestamp timestamp{
      std::chrono::seconds{0x01020304} +
      std::chrono::nanoseconds{0x11223344}};
  const MonData::StoredRecord record =
      make_record("abc", timestamp);
  std::vector<std::uint8_t> datagram(
      Wire::V2_HEADER_SIZE + 3U + 8U,
      0U);

  const Wire::WireStatus status =
      Wire::Detail::write_v2_header(
          datagram,
          record,
          Wire::WireValueType::NUMERIC,
          8U);

  const std::array<std::uint8_t, Wire::V2_HEADER_SIZE> expected{
      0x02, 0x00,                         // version, numeric type
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
      0x00, 0x08};                        // value_length

  assert(status == Wire::WireStatus::SUCCESS);
  assert(std::equal(expected.begin(), expected.end(), datagram.begin()));
}

/*
 * -0.5 秒必须正规化为 seconds = -1、nanoseconds = 500000000，
 * 而不是 seconds = 0、nanoseconds 为负数。
 */
void test_writer_normalizes_negative_timestamp() {
  const MonData::MonitorTimestamp timestamp{
      std::chrono::nanoseconds{-500'000'000}};
  const MonData::StoredRecord record = make_record("", timestamp);
  std::vector<std::uint8_t> datagram(
      Wire::V2_HEADER_SIZE + 8U,
      0U);

  const Wire::WireStatus status =
      Wire::Detail::write_v2_header(
          datagram,
          record,
          Wire::WireValueType::NUMERIC,
          8U);

  assert(status == Wire::WireStatus::SUCCESS);
  assert(Wire::Detail::get_i64_twos_complement(
             datagram.data() + SECONDS_OFFSET) == INT64_C(-1));
  assert(Wire::Detail::get_u32(
             datagram.data() + NANOSECONDS_OFFSET) == 500'000'000U);
}

/*
 * STRING 类型只改变 type 字段；description_length 和 value_length
 * 仍分别记录各自的完整字节数。
 */
void test_writer_records_string_type_and_field_lengths() {
  const MonData::StoredRecord record = make_record("description");
  std::vector<std::uint8_t> datagram(
      Wire::V2_HEADER_SIZE + record._data._description.size() + 5U,
      0U);

  const Wire::WireStatus status =
      Wire::Detail::write_v2_header(
          datagram,
          record,
          Wire::WireValueType::STRING,
          5U);

  assert(status == Wire::WireStatus::SUCCESS);
  assert(datagram[TYPE_OFFSET] ==
         static_cast<std::uint8_t>(Wire::WireValueType::STRING));
  assert(Wire::Detail::get_u16(
             datagram.data() + DESCRIPTION_LENGTH_OFFSET) == 11U);
  assert(Wire::Detail::get_u16(
             datagram.data() + VALUE_LENGTH_OFFSET) == 5U);
}

/*
 * 调用方必须预先创建精确大小的数据报；少一个或多一个字节都拒绝。
 */
void test_writer_rejects_incorrect_buffer_size() {
  const MonData::StoredRecord record = make_record("abc");

  for (const std::size_t size : {
           Wire::V2_HEADER_SIZE + 3U + 8U - 1U,
           Wire::V2_HEADER_SIZE + 3U + 8U + 1U}) {
    std::vector<std::uint8_t> datagram(size, 0U);

    const Wire::WireStatus status =
        Wire::Detail::write_v2_header(
            datagram,
            record,
            Wire::WireValueType::NUMERIC,
            8U);

    assert(status == Wire::WireStatus::INVALID_TOTAL_LENGTH);
  }
}

/*
 * V2 最大数据报为 1200 字节：恰好 1200 成功，1201 必须拒绝。
 */
void test_writer_enforces_maximum_datagram_size() {
  const std::size_t value_length = 8U;
  const std::size_t maximum_description_length =
      Wire::V2_MAX_DATAGRAM_SIZE - Wire::V2_HEADER_SIZE - value_length;

  const MonData::StoredRecord maximum_record =
      make_record(std::string(maximum_description_length, 'A'));
  std::vector<std::uint8_t> maximum_datagram(
      Wire::V2_MAX_DATAGRAM_SIZE,
      0U);

  assert(Wire::Detail::write_v2_header(
             maximum_datagram,
             maximum_record,
             Wire::WireValueType::NUMERIC,
             value_length) == Wire::WireStatus::SUCCESS);

  const MonData::StoredRecord oversized_record =
      make_record(std::string(maximum_description_length + 1U, 'A'));
  std::vector<std::uint8_t> oversized_datagram(
      Wire::V2_MAX_DATAGRAM_SIZE + 1U,
      0U);

  assert(Wire::Detail::write_v2_header(
             oversized_datagram,
             oversized_record,
             Wire::WireValueType::NUMERIC,
             value_length) == Wire::WireStatus::DATAGRAM_TOO_LARGE);
}

/*
 * 未知类型和无法放入 uint16_t 的字段长度必须在写入前被拒绝。
 */
void test_writer_rejects_invalid_type_and_field_length() {
  const MonData::StoredRecord normal_record = make_record("");
  std::vector<std::uint8_t> normal_datagram(
      Wire::V2_HEADER_SIZE,
      0U);

  assert(Wire::Detail::write_v2_header(
             normal_datagram,
             normal_record,
             static_cast<Wire::WireValueType>(0xffU),
             0U) == Wire::WireStatus::INVALID_TYPE);

  const MonData::StoredRecord long_description_record =
      make_record(std::string(
          static_cast<std::size_t>(
              std::numeric_limits<std::uint16_t>::max()) + 1U,
          'A'));

  assert(Wire::Detail::write_v2_header(
             normal_datagram,
             long_description_record,
             Wire::WireValueType::STRING,
             0U) == Wire::WireStatus::INVALID_FIELD_LENGTH);

  assert(Wire::Detail::write_v2_header(
             normal_datagram,
             normal_record,
             Wire::WireValueType::STRING,
             static_cast<std::size_t>(
                 std::numeric_limits<std::uint16_t>::max()) + 1U) ==
         Wire::WireStatus::INVALID_FIELD_LENGTH);
}

/*
 * 使用独立构造的报文验证读取端返回头部全部字段。
 */
void test_reader_parses_independent_valid_header() {
  const std::vector<std::uint8_t> datagram = make_valid_datagram();

  const Wire::Detail::V2HeaderResult result =
      Wire::Detail::read_v2_header(
          datagram.data(),
          datagram.size());

  assert(result._status == Wire::WireStatus::SUCCESS);
  assert(result._header.has_value());

  const Wire::Detail::V2Header &header = *result._header;
  assert(header._type == Wire::WireValueType::NUMERIC);
  assert(header._flags == 0U);
  assert(header._total_length == datagram.size());
  assert(header._header_length == Wire::V2_HEADER_SIZE);
  assert(header._key == TEST_KEY);
  assert(header._seconds == INT64_C(-2));
  assert(header._nanoseconds == 250'000'000U);
  assert(header._description_length == 3U);
  assert(header._value_length == 8U);
}

/*
 * 第 0 字节可以读取时先判断版本；只有 V2 才继续检查头部长度。
 */
void test_reader_rejects_empty_wrong_version_and_short_header() {
  const Wire::Detail::V2HeaderResult empty =
      Wire::Detail::read_v2_header(nullptr, 0U);
  assert(empty._status == Wire::WireStatus::EMPTY_INPUT);
  assert(!empty._header.has_value());

  const std::vector<std::uint8_t> wrong_version{
      static_cast<std::uint8_t>(Wire::WireVersion::V1)};
  assert_read_failure(wrong_version, Wire::WireStatus::WRONG_VERSION);

  std::vector<std::uint8_t> short_header(
      Wire::V2_HEADER_SIZE - 1U,
      0U);
  short_header[VERSION_OFFSET] =
      static_cast<std::uint8_t>(Wire::WireVersion::V2);
  assert_read_failure(short_header, Wire::WireStatus::SHORT_HEADER);
}

/*
 * type、flags 和 header_length 都是严格字段，不接受未知扩展值。
 */
void test_reader_rejects_invalid_type_flags_and_header_length() {
  std::vector<std::uint8_t> datagram = make_valid_datagram();
  datagram[TYPE_OFFSET] = 0xffU;
  assert_read_failure(datagram, Wire::WireStatus::INVALID_TYPE);

  datagram = make_valid_datagram();
  Wire::Detail::put_u16(datagram.data() + FLAGS_OFFSET, 1U);
  assert_read_failure(datagram, Wire::WireStatus::INVALID_FLAGS);

  datagram = make_valid_datagram();
  Wire::Detail::put_u16(
      datagram.data() + HEADER_LENGTH_OFFSET,
      static_cast<std::uint16_t>(Wire::V2_HEADER_SIZE - 1U));
  assert_read_failure(datagram, Wire::WireStatus::INVALID_HEADER_LENGTH);
}

/*
 * total_length 必须等于实际 UDP 数据报长度，且不能超过 1200。
 */
void test_reader_strictly_validates_total_length() {
  std::vector<std::uint8_t> datagram = make_valid_datagram();
  Wire::Detail::put_u16(
      datagram.data() + TOTAL_LENGTH_OFFSET,
      static_cast<std::uint16_t>(datagram.size() - 1U));
  assert_read_failure(datagram, Wire::WireStatus::INVALID_TOTAL_LENGTH);

  datagram = make_valid_datagram();
  Wire::Detail::put_u16(
      datagram.data() + TOTAL_LENGTH_OFFSET,
      static_cast<std::uint16_t>(Wire::V2_MAX_DATAGRAM_SIZE + 1U));
  assert_read_failure(datagram, Wire::WireStatus::DATAGRAM_TOO_LARGE);

  datagram = make_valid_datagram();
  datagram.push_back(0U);
  assert_read_failure(datagram, Wire::WireStatus::INVALID_TOTAL_LENGTH);

  datagram = make_valid_datagram();
  datagram.resize(Wire::V2_MAX_DATAGRAM_SIZE + 1U, 0U);
  assert_read_failure(datagram, Wire::WireStatus::DATAGRAM_TOO_LARGE);
}

/*
 * description_length + value_length 必须精确等于 total_length - 40。
 */
void test_reader_rejects_inconsistent_field_lengths() {
  std::vector<std::uint8_t> datagram = make_valid_datagram();
  Wire::Detail::put_u16(
      datagram.data() + DESCRIPTION_LENGTH_OFFSET,
      4U);

  assert_read_failure(datagram, Wire::WireStatus::INVALID_FIELD_LENGTH);
}

/*
 * nanoseconds 的合法上界是 999999999；等于十亿时必须拒绝。
 */
void test_reader_validates_nanoseconds_range() {
  std::vector<std::uint8_t> datagram = make_valid_datagram();
  Wire::Detail::put_u32(
      datagram.data() + NANOSECONDS_OFFSET,
      999'999'999U);

  const Wire::Detail::V2HeaderResult maximum_valid =
      Wire::Detail::read_v2_header(
          datagram.data(),
          datagram.size());
  assert(maximum_valid._status == Wire::WireStatus::SUCCESS);
  assert(maximum_valid._header.has_value());
  assert(maximum_valid._header->_nanoseconds == 999'999'999U);

  Wire::Detail::put_u32(
      datagram.data() + NANOSECONDS_OFFSET,
      1'000'000'000U);
  assert_read_failure(datagram, Wire::WireStatus::INVALID_TIMESTAMP);
}

/*
 * 40 字节头部加 1160 字节载荷是合法 V2 数据报的最大边界。
 */
void test_reader_accepts_exactly_1200_bytes() {
  const std::vector<std::uint8_t> datagram =
      make_valid_datagram(1152U, 8U);

  const Wire::Detail::V2HeaderResult result =
      Wire::Detail::read_v2_header(
          datagram.data(),
          datagram.size());

  assert(datagram.size() == Wire::V2_MAX_DATAGRAM_SIZE);
  assert(result._status == Wire::WireStatus::SUCCESS);
  assert(result._header.has_value());
  assert(result._header->_description_length == 1152U);
  assert(result._header->_value_length == 8U);
}

} // namespace

int main() {
  test_writer_matches_v2_header_golden_bytes();
  test_writer_normalizes_negative_timestamp();
  test_writer_records_string_type_and_field_lengths();
  test_writer_rejects_incorrect_buffer_size();
  test_writer_enforces_maximum_datagram_size();
  test_writer_rejects_invalid_type_and_field_length();
  test_reader_parses_independent_valid_header();
  test_reader_rejects_empty_wrong_version_and_short_header();
  test_reader_rejects_invalid_type_flags_and_header_length();
  test_reader_strictly_validates_total_length();
  test_reader_rejects_inconsistent_field_lengths();
  test_reader_validates_nanoseconds_range();
  test_reader_accepts_exactly_1200_bytes();
  return 0;
}
