#include "alarm_protocol.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace TLSSMON::AlarmWire;

namespace {

void put_u16(std::uint8_t *output, std::uint16_t value) {
  output[0] = static_cast<std::uint8_t>(value >> 8U);
  output[1] = static_cast<std::uint8_t>(value);
}

void put_u32(std::uint8_t *output, std::uint32_t value) {
  output[0] = static_cast<std::uint8_t>(value >> 24U);
  output[1] = static_cast<std::uint8_t>(value >> 16U);
  output[2] = static_cast<std::uint8_t>(value >> 8U);
  output[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t reference_crc32(const std::uint8_t *data, std::size_t size) {
  std::uint32_t crc = UINT32_C(0xffffffff);
  for (std::size_t i = 0U; i < size; ++i) {
    crc ^= data[i];
    for (unsigned int bit = 0U; bit < 8U; ++bit) {
      const std::uint32_t mask = static_cast<std::uint32_t>(
          -static_cast<std::int32_t>(crc & UINT32_C(1)));
      crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & mask);
    }
  }
  return crc ^ UINT32_C(0xffffffff);
}

void refresh_frame_crc(std::vector<std::uint8_t> &bytes) {
  std::vector<std::uint8_t> covered;
  covered.insert(covered.end(), bytes.begin(),
                 bytes.begin() + static_cast<std::ptrdiff_t>(ALARM_CRC32_OFFSET));
  covered.insert(covered.end(),
                 bytes.begin() + static_cast<std::ptrdiff_t>(ALARM_HEADER_SIZE),
                 bytes.end());
  put_u32(bytes.data() + ALARM_CRC32_OFFSET,
          reference_crc32(covered.data(), covered.size()));
}

AlarmFrame make_alarm(std::size_t payload_size = 5U) {
  AlarmFrame frame;
  frame._type = AlarmFrameType::ALARM;
  frame._source_id = UINT64_C(0x0102030405060708);
  frame._timestamp_ms = UINT64_C(0x1112131415161718);
  frame._payload.resize(payload_size);
  for (std::size_t i = 0U; i < payload_size; ++i) {
    frame._payload[i] = static_cast<std::uint8_t>(0x20U + (i % 211U));
  }
  for (std::size_t i = 0U; i < frame._message_id.size(); ++i) {
    frame._message_id[i] = static_cast<std::uint8_t>(0xa0U + i);
  }
  return frame;
}

/* 标准测试向量锁定 CRC32 算法。 */
void test_crc32_standard_vector() {
  const std::string text = "123456789";
  assert(alarm_crc32(
             reinterpret_cast<const std::uint8_t *>(text.data()),
             text.size()) == UINT32_C(0xcbf43926));
  assert(alarm_crc32(nullptr, 0U) == 0U);
  assert(alarm_crc32(nullptr, 1U) == 0U);
}

/* 验证 ALARM 的固定字节布局、大端序和完整往返。 */
void test_alarm_round_trip_and_layout() {
  const AlarmFrame original = make_alarm();
  const AlarmEncodeResult encoded = encode_alarm_frame(original);
  assert(encoded.success());
  assert(encoded._bytes.size() == ALARM_HEADER_SIZE + original._payload.size());

  const std::array<std::uint8_t, 44U> expected_prefix{
      'S',  'M',  'A',  '1',  0x01, 0x01, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x05,
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
      0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
      0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
      0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18};
  assert(std::equal(expected_prefix.begin(), expected_prefix.end(),
                    encoded._bytes.begin()));

  const AlarmDecodeResult decoded =
      decode_alarm_frame(encoded._bytes.data(), encoded._bytes.size());
  assert(decoded.success());
  assert(*decoded._frame == original);
}

/* ACK、PING、PONG 都是没有 payload 的 48 字节控制帧。 */
void test_control_frames_round_trip() {
  for (const AlarmFrameType type :
       {AlarmFrameType::ACK, AlarmFrameType::PING, AlarmFrameType::PONG}) {
    AlarmFrame frame{type, 9U, {}, 10U, {}};
    frame._message_id[0] = static_cast<std::uint8_t>(type);
    const AlarmEncodeResult encoded = encode_alarm_frame(frame);
    assert(encoded.success());
    assert(encoded._bytes.size() == ALARM_HEADER_SIZE);
    const AlarmDecodeResult decoded =
        decode_alarm_frame(encoded._bytes.data(), encoded._bytes.size());
    assert(decoded.success());
    assert(*decoded._frame == frame);
  }
}

/* 只要已有完整头部，就能取得整个帧的声明长度。 */
void test_frame_size_from_header() {
  const AlarmEncodeResult encoded = encode_alarm_frame(make_alarm(123U));
  assert(encoded.success());
  const AlarmFrameSizeResult result =
      alarm_frame_size(encoded._bytes.data(), ALARM_HEADER_SIZE);
  assert(result.success());
  assert(*result._frame_size == ALARM_HEADER_SIZE + 123U);

  assert(alarm_frame_size(nullptr, 0U)._status ==
         AlarmProtocolStatus::EMPTY_INPUT);
  assert(alarm_frame_size(encoded._bytes.data(), ALARM_HEADER_SIZE - 1U)
             ._status == AlarmProtocolStatus::SHORT_HEADER);
}

/* 每个固定头字段的错误都必须得到确定的状态。 */
void test_rejects_invalid_header_fields() {
  const AlarmEncodeResult valid = encode_alarm_frame(make_alarm());
  assert(valid.success());

  auto bytes = valid._bytes;
  bytes[ALARM_MAGIC_OFFSET] = 'X';
  assert(decode_alarm_frame(bytes.data(), bytes.size())._status ==
         AlarmProtocolStatus::WRONG_MAGIC);

  bytes = valid._bytes;
  bytes[ALARM_VERSION_OFFSET] = 2U;
  assert(decode_alarm_frame(bytes.data(), bytes.size())._status ==
         AlarmProtocolStatus::WRONG_VERSION);

  bytes = valid._bytes;
  bytes[ALARM_TYPE_OFFSET] = 99U;
  assert(decode_alarm_frame(bytes.data(), bytes.size())._status ==
         AlarmProtocolStatus::INVALID_TYPE);

  bytes = valid._bytes;
  put_u16(bytes.data() + ALARM_FLAGS_OFFSET, 1U);
  assert(decode_alarm_frame(bytes.data(), bytes.size())._status ==
         AlarmProtocolStatus::INVALID_FLAGS);
}

/* ALARM 必须有 payload，控制帧则禁止携带 payload。 */
void test_payload_shape_rules() {
  const AlarmEncodeResult empty_alarm =
      encode_alarm_frame(AlarmFrame{AlarmFrameType::ALARM, 0U, {}, 0U, {}});
  assert(empty_alarm._status == AlarmProtocolStatus::INVALID_PAYLOAD_LENGTH);
  assert(empty_alarm._bytes.empty());

  const AlarmEncodeResult ack_with_payload = encode_alarm_frame(
      AlarmFrame{AlarmFrameType::ACK, 0U, {}, 0U, {1U}});
  assert(ack_with_payload._status ==
         AlarmProtocolStatus::INVALID_PAYLOAD_LENGTH);

  const AlarmEncodeResult invalid_type = encode_alarm_frame(
      AlarmFrame{static_cast<AlarmFrameType>(99U), 0U, {}, 0U, {1U}});
  assert(invalid_type._status == AlarmProtocolStatus::INVALID_TYPE);
}

/* 截断、额外尾部和 CRC 损坏不能被接受。 */
void test_rejects_length_and_crc_errors() {
  const AlarmEncodeResult valid = encode_alarm_frame(make_alarm());
  assert(valid.success());

  auto bytes = valid._bytes;
  bytes.pop_back();
  assert(decode_alarm_frame(bytes.data(), bytes.size())._status ==
         AlarmProtocolStatus::INVALID_TOTAL_LENGTH);

  bytes = valid._bytes;
  bytes.push_back(0U);
  assert(decode_alarm_frame(bytes.data(), bytes.size())._status ==
         AlarmProtocolStatus::INVALID_TOTAL_LENGTH);

  bytes = valid._bytes;
  bytes[ALARM_HEADER_SIZE] ^= 0x01U;
  assert(decode_alarm_frame(bytes.data(), bytes.size())._status ==
         AlarmProtocolStatus::CRC_MISMATCH);

  bytes = valid._bytes;
  bytes[ALARM_CRC32_OFFSET] ^= 0x01U;
  assert(decode_alarm_frame(bytes.data(), bytes.size())._status ==
         AlarmProtocolStatus::CRC_MISMATCH);
}

/* 即使重新计算 CRC，非法的 type/payload 组合仍必须被拒绝。 */
void test_rejects_control_frame_with_declared_payload() {
  const AlarmEncodeResult valid = encode_alarm_frame(make_alarm());
  assert(valid.success());
  auto bytes = valid._bytes;
  bytes[ALARM_TYPE_OFFSET] = static_cast<std::uint8_t>(AlarmFrameType::ACK);
  refresh_frame_crc(bytes);
  assert(decode_alarm_frame(bytes.data(), bytes.size())._status ==
         AlarmProtocolStatus::INVALID_PAYLOAD_LENGTH);
}

/* 4096 是合法边界，4097 必须在编码或分配前拒绝。 */
void test_payload_size_boundary() {
  const AlarmFrame maximum = make_alarm(ALARM_MAX_PAYLOAD_SIZE);
  const AlarmEncodeResult encoded = encode_alarm_frame(maximum);
  assert(encoded.success());
  assert(encoded._bytes.size() == ALARM_MAX_FRAME_SIZE);
  const AlarmDecodeResult decoded =
      decode_alarm_frame(encoded._bytes.data(), encoded._bytes.size());
  assert(decoded.success());
  assert(*decoded._frame == maximum);

  AlarmFrame oversized = make_alarm(ALARM_MAX_PAYLOAD_SIZE + 1U);
  const AlarmEncodeResult rejected = encode_alarm_frame(oversized);
  assert(rejected._status == AlarmProtocolStatus::PAYLOAD_TOO_LARGE);
  assert(rejected._bytes.empty());

  auto forged = encoded._bytes;
  put_u32(forged.data() + ALARM_PAYLOAD_LENGTH_OFFSET,
          static_cast<std::uint32_t>(ALARM_MAX_PAYLOAD_SIZE + 1U));
  assert(alarm_frame_size(forged.data(), ALARM_HEADER_SIZE)._status ==
         AlarmProtocolStatus::PAYLOAD_TOO_LARGE);
}

} // namespace

int main() {
  test_crc32_standard_vector();
  test_alarm_round_trip_and_layout();
  test_control_frames_round_trip();
  test_frame_size_from_header();
  test_rejects_invalid_header_fields();
  test_payload_shape_rules();
  test_rejects_length_and_crc_errors();
  test_rejects_control_frame_with_declared_payload();
  test_payload_size_boundary();
  std::cout << "M9_ALARM_PROTOCOL=PASS\n";
  return 0;
}
