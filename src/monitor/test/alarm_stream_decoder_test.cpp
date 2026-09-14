#include "alarm_protocol.h"
#include "alarm_stream_decoder.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <vector>

using namespace TLSSMON::AlarmWire;

namespace {

void put_u32(std::uint8_t *output, std::uint32_t value) {
  output[0] = static_cast<std::uint8_t>(value >> 24U);
  output[1] = static_cast<std::uint8_t>(value >> 16U);
  output[2] = static_cast<std::uint8_t>(value >> 8U);
  output[3] = static_cast<std::uint8_t>(value);
}

AlarmFrame make_alarm(std::uint8_t seed, std::size_t payload_size = 16U) {
  AlarmFrame frame;
  frame._type = AlarmFrameType::ALARM;
  frame._source_id = UINT64_C(0x0102030405060708) + seed;
  frame._timestamp_ms = UINT64_C(1700000000000) + seed;
  frame._payload.resize(payload_size);
  for (std::size_t i = 0U; i < frame._message_id.size(); ++i) {
    frame._message_id[i] = static_cast<std::uint8_t>(seed + i);
  }
  for (std::size_t i = 0U; i < payload_size; ++i) {
    frame._payload[i] = static_cast<std::uint8_t>(seed + (i % 223U));
  }
  return frame;
}

AlarmFrame make_control(AlarmFrameType type, std::uint8_t seed) {
  AlarmFrame frame;
  frame._type = type;
  frame._source_id = seed;
  frame._timestamp_ms = 1000U + seed;
  frame._message_id[0] = seed;
  return frame;
}

std::vector<std::uint8_t> encode_checked(const AlarmFrame &frame) {
  const AlarmEncodeResult result = encode_alarm_frame(frame);
  assert(result.success());
  return result._bytes;
}

/* 一个完整帧一次输入，应立即输出且不残留缓存。 */
void test_complete_frame() {
  const AlarmFrame expected = make_alarm(1U);
  const auto bytes = encode_checked(expected);
  AlarmStreamDecoder decoder;
  const AlarmStreamFeedResult result = decoder.feed(bytes.data(), bytes.size());
  assert(result.success());
  assert(result._status == AlarmStreamStatus::SUCCESS);
  assert(result._frames.size() == 1U);
  assert(result._frames[0] == expected);
  assert(decoder.buffered_size() == 0U);
}

/* 每次只输入一个字节，验证最极端的 TCP 拆包。 */
void test_single_byte_fragmentation() {
  const AlarmFrame expected = make_alarm(2U, 127U);
  const auto bytes = encode_checked(expected);
  AlarmStreamDecoder decoder;
  std::vector<AlarmFrame> received;
  for (std::size_t i = 0U; i < bytes.size(); ++i) {
    AlarmStreamFeedResult result = decoder.feed(&bytes[i], 1U);
    assert(result.success());
    received.insert(received.end(),
                    std::make_move_iterator(result._frames.begin()),
                    std::make_move_iterator(result._frames.end()));
    if (i + 1U < bytes.size()) {
      assert(result._status == AlarmStreamStatus::NEED_MORE_DATA);
    }
  }
  assert(received.size() == 1U);
  assert(received[0] == expected);
}

/* 不规则分片覆盖头部边界和 payload 边界。 */
void test_irregular_fragmentation() {
  const AlarmFrame expected = make_alarm(3U, 333U);
  const auto bytes = encode_checked(expected);
  const std::array<std::size_t, 7U> chunks{1U, 3U, 7U, 19U, 2U, 64U, 11U};
  AlarmStreamDecoder decoder;
  std::vector<AlarmFrame> received;
  std::size_t offset = 0U;
  std::size_t index = 0U;
  while (offset < bytes.size()) {
    const std::size_t amount =
        std::min(chunks[index % chunks.size()], bytes.size() - offset);
    AlarmStreamFeedResult result = decoder.feed(bytes.data() + offset, amount);
    assert(result.success());
    received.insert(received.end(),
                    std::make_move_iterator(result._frames.begin()),
                    std::make_move_iterator(result._frames.end()));
    offset += amount;
    ++index;
  }
  assert(received.size() == 1U);
  assert(received[0] == expected);
}

/* 一次输入多帧，验证 TCP 粘包以及输出顺序。 */
void test_multiple_frames_in_one_feed() {
  const std::array<AlarmFrame, 4U> expected{
      make_alarm(4U), make_control(AlarmFrameType::ACK, 5U),
      make_control(AlarmFrameType::PING, 6U), make_alarm(7U, 200U)};
  std::vector<std::uint8_t> input;
  for (const AlarmFrame &frame : expected) {
    const auto bytes = encode_checked(frame);
    input.insert(input.end(), bytes.begin(), bytes.end());
  }
  AlarmStreamDecoder decoder;
  const AlarmStreamFeedResult result = decoder.feed(input.data(), input.size());
  assert(result.success());
  assert(result._status == AlarmStreamStatus::SUCCESS);
  assert(result._frames.size() == expected.size());
  assert(std::equal(expected.begin(), expected.end(), result._frames.begin()));
}

/* 完整帧后的半帧应被保留，并在下一次输入后完成。 */
void test_complete_frame_with_partial_tail() {
  const AlarmFrame first = make_alarm(8U);
  const AlarmFrame second = make_alarm(9U, 512U);
  const auto first_bytes = encode_checked(first);
  const auto second_bytes = encode_checked(second);
  constexpr std::size_t partial_size = 23U;
  std::vector<std::uint8_t> input = first_bytes;
  input.insert(input.end(), second_bytes.begin(),
               second_bytes.begin() + static_cast<std::ptrdiff_t>(partial_size));

  AlarmStreamDecoder decoder;
  const AlarmStreamFeedResult first_result =
      decoder.feed(input.data(), input.size());
  assert(first_result.success());
  assert(first_result._status == AlarmStreamStatus::NEED_MORE_DATA);
  assert(first_result._frames.size() == 1U);
  assert(first_result._frames[0] == first);
  assert(decoder.buffered_size() == partial_size);
  assert(!decoder.expected_frame_size().has_value());

  const AlarmStreamFeedResult second_result =
      decoder.feed(second_bytes.data() + partial_size,
                   second_bytes.size() - partial_size);
  assert(second_result.success());
  assert(second_result._frames.size() == 1U);
  assert(second_result._frames[0] == second);
  assert(decoder.buffered_size() == 0U);
}

/* 收到完整头部后，应记录完整帧长度但不提前输出。 */
void test_header_determines_expected_size() {
  const auto bytes = encode_checked(make_alarm(10U, 700U));
  AlarmStreamDecoder decoder;
  const AlarmStreamFeedResult result =
      decoder.feed(bytes.data(), ALARM_HEADER_SIZE);
  assert(result.success());
  assert(result._status == AlarmStreamStatus::NEED_MORE_DATA);
  assert(result._frames.empty());
  assert(decoder.expected_frame_size().has_value());
  assert(*decoder.expected_frame_size() == bytes.size());
}

/* EOF 时不足 48 字节属于截断头部。 */
void test_eof_rejects_truncated_header() {
  const auto bytes = encode_checked(make_alarm(11U));
  AlarmStreamDecoder decoder;
  assert(decoder.feed(bytes.data(), 17U).success());
  const AlarmStreamFeedResult result = decoder.finish();
  assert(!result.success());
  assert(result._status == AlarmStreamStatus::PROTOCOL_ERROR);
  assert(result._protocol_status == AlarmProtocolStatus::SHORT_HEADER);
  assert(decoder.failed());
}

/* EOF 时头部完整但 payload 不完整，属于总长度错误。 */
void test_eof_rejects_truncated_payload() {
  const auto bytes = encode_checked(make_alarm(12U, 300U));
  AlarmStreamDecoder decoder;
  assert(decoder.feed(bytes.data(), bytes.size() - 1U).success());
  const AlarmStreamFeedResult result = decoder.finish();
  assert(!result.success());
  assert(result._status == AlarmStreamStatus::PROTOCOL_ERROR);
  assert(result._protocol_status == AlarmProtocolStatus::INVALID_TOTAL_LENGTH);
}

/* 超大声明长度必须在只收到头部时被拒绝，避免按攻击长度分配。 */
void test_rejects_oversized_payload_before_allocation() {
  auto bytes = encode_checked(make_alarm(13U));
  put_u32(bytes.data() + ALARM_PAYLOAD_LENGTH_OFFSET,
          static_cast<std::uint32_t>(ALARM_MAX_PAYLOAD_SIZE + 1U));
  AlarmStreamDecoder decoder;
  const AlarmStreamFeedResult result =
      decoder.feed(bytes.data(), ALARM_HEADER_SIZE);
  assert(!result.success());
  assert(result._status == AlarmStreamStatus::PROTOCOL_ERROR);
  assert(result._protocol_status == AlarmProtocolStatus::PAYLOAD_TOO_LARGE);
  assert(decoder.failed());
  assert(decoder.buffered_size() == 0U);
}

/* CRC 错误使当前连接进入终止状态，reset 前不能继续解析。 */
void test_crc_error_is_terminal_and_reset_recovers() {
  const AlarmFrame expected = make_alarm(14U);
  auto corrupt = encode_checked(expected);
  const auto valid = encode_checked(expected);
  corrupt[ALARM_HEADER_SIZE] ^= 0x01U;

  AlarmStreamDecoder decoder;
  const AlarmStreamFeedResult bad = decoder.feed(corrupt.data(), corrupt.size());
  assert(!bad.success());
  assert(bad._protocol_status == AlarmProtocolStatus::CRC_MISMATCH);
  assert(decoder.failed());

  const AlarmStreamFeedResult blocked = decoder.feed(valid.data(), valid.size());
  assert(blocked._status == AlarmStreamStatus::ALREADY_FAILED);
  assert(blocked._frames.empty());

  decoder.reset();
  assert(!decoder.failed());
  assert(!decoder.protocol_error().has_value());
  const AlarmStreamFeedResult recovered = decoder.feed(valid.data(), valid.size());
  assert(recovered.success());
  assert(recovered._frames.size() == 1U);
  assert(recovered._frames[0] == expected);
}

/* 后一个坏帧不能吞掉同批输入中已解析完成的前一个好帧。 */
void test_valid_frame_before_bad_frame_is_preserved() {
  const AlarmFrame expected = make_alarm(15U);
  const auto valid = encode_checked(expected);
  auto corrupt = encode_checked(make_alarm(16U));
  corrupt[ALARM_HEADER_SIZE] ^= 0x01U;
  std::vector<std::uint8_t> input = valid;
  input.insert(input.end(), corrupt.begin(), corrupt.end());

  AlarmStreamDecoder decoder;
  const AlarmStreamFeedResult result = decoder.feed(input.data(), input.size());
  assert(!result.success());
  assert(result._protocol_status == AlarmProtocolStatus::CRC_MISMATCH);
  assert(result._frames.size() == 1U);
  assert(result._frames[0] == expected);
}

/* 最大合法帧可以处理，且不会留下超过硬上限的缓存。 */
void test_maximum_frame_size() {
  const AlarmFrame expected = make_alarm(17U, ALARM_MAX_PAYLOAD_SIZE);
  const auto bytes = encode_checked(expected);
  static_assert(ALARM_STREAM_BUFFER_LIMIT == ALARM_MAX_FRAME_SIZE);
  assert(bytes.size() == ALARM_STREAM_BUFFER_LIMIT);
  AlarmStreamDecoder decoder;
  const AlarmStreamFeedResult result = decoder.feed(bytes.data(), bytes.size());
  assert(result.success());
  assert(result._frames.size() == 1U);
  assert(result._frames[0] == expected);
  assert(decoder.buffered_size() == 0U);
}

/* 空输入无副作用；空指针配非零长度也不能污染 Decoder。 */
void test_empty_and_invalid_input() {
  AlarmStreamDecoder decoder;
  const AlarmStreamFeedResult empty = decoder.feed(nullptr, 0U);
  assert(empty.success());
  assert(empty._status == AlarmStreamStatus::SUCCESS);

  const AlarmStreamFeedResult invalid = decoder.feed(nullptr, 1U);
  assert(!invalid.success());
  assert(invalid._status == AlarmStreamStatus::INVALID_INPUT);
  assert(!decoder.failed());

  const AlarmFrame expected = make_alarm(18U);
  const auto bytes = encode_checked(expected);
  const AlarmStreamFeedResult valid = decoder.feed(bytes.data(), bytes.size());
  assert(valid.success());
  assert(valid._frames.size() == 1U);
}

/* 无残留数据时收到 EOF 是正常结束。 */
void test_clean_eof() {
  AlarmStreamDecoder decoder;
  const AlarmStreamFeedResult result = decoder.finish();
  assert(result.success());
  assert(result._status == AlarmStreamStatus::SUCCESS);
  assert(!decoder.failed());
}

} // namespace

int main() {
  test_complete_frame();
  test_single_byte_fragmentation();
  test_irregular_fragmentation();
  test_multiple_frames_in_one_feed();
  test_complete_frame_with_partial_tail();
  test_header_determines_expected_size();
  test_eof_rejects_truncated_header();
  test_eof_rejects_truncated_payload();
  test_rejects_oversized_payload_before_allocation();
  test_crc_error_is_terminal_and_reset_recovers();
  test_valid_frame_before_bad_frame_is_preserved();
  test_maximum_frame_size();
  test_empty_and_invalid_input();
  test_clean_eof();
  std::cout << "M9_ALARM_STREAM=PASS\n";
  return 0;
}
