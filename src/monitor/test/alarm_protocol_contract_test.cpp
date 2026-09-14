#include "alarm_protocol.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

using namespace TLSSMON::AlarmWire;

namespace {

static_assert(ALARM_MAGIC.size() == 4U);
static_assert(ALARM_MAGIC[0] == static_cast<std::uint8_t>('S'));
static_assert(ALARM_MAGIC[1] == static_cast<std::uint8_t>('M'));
static_assert(ALARM_MAGIC[2] == static_cast<std::uint8_t>('A'));
static_assert(ALARM_MAGIC[3] == static_cast<std::uint8_t>('1'));
static_assert(ALARM_PROTOCOL_VERSION == 1U);
static_assert(ALARM_MESSAGE_ID_SIZE == 16U);
static_assert(ALARM_HEADER_SIZE == 48U);
static_assert(ALARM_MAX_PAYLOAD_SIZE == 4096U);
static_assert(ALARM_MAX_FRAME_SIZE == 4144U);

/*
 * 锁定固定头部布局，避免后续添加字段时静默破坏已有协议。
 */
static_assert(ALARM_MAGIC_OFFSET == 0U);
static_assert(ALARM_VERSION_OFFSET == 4U);
static_assert(ALARM_TYPE_OFFSET == 5U);
static_assert(ALARM_FLAGS_OFFSET == 6U);
static_assert(ALARM_PAYLOAD_LENGTH_OFFSET == 8U);
static_assert(ALARM_SOURCE_ID_OFFSET == 12U);
static_assert(ALARM_MESSAGE_ID_OFFSET == 20U);
static_assert(ALARM_TIMESTAMP_OFFSET == 36U);
static_assert(ALARM_CRC32_OFFSET == 44U);
static_assert(ALARM_CRC32_OFFSET + sizeof(std::uint32_t) ==
              ALARM_HEADER_SIZE);

static_assert(std::is_enum_v<AlarmFrameType>);
static_assert(
    std::is_same_v<std::underlying_type_t<AlarmFrameType>, std::uint8_t>);
static_assert(static_cast<std::uint8_t>(AlarmFrameType::ALARM) == 1U);
static_assert(static_cast<std::uint8_t>(AlarmFrameType::ACK) == 2U);
static_assert(static_cast<std::uint8_t>(AlarmFrameType::PING) == 3U);
static_assert(static_cast<std::uint8_t>(AlarmFrameType::PONG) == 4U);

static_assert(std::is_same_v<AlarmMessageId,
                             std::array<std::uint8_t, 16U>>);
static_assert(std::is_copy_constructible_v<AlarmFrame>);
static_assert(std::is_move_constructible_v<AlarmFrame>);
static_assert(std::is_copy_assignable_v<AlarmFrame>);
static_assert(std::is_move_assignable_v<AlarmFrame>);

static_assert(
    std::is_same_v<decltype(std::declval<AlarmFrame &>()._type),
                   AlarmFrameType>);
static_assert(
    std::is_same_v<decltype(std::declval<AlarmFrame &>()._source_id),
                   std::uint64_t>);
static_assert(
    std::is_same_v<decltype(std::declval<AlarmFrame &>()._message_id),
                   AlarmMessageId>);
static_assert(
    std::is_same_v<decltype(std::declval<AlarmFrame &>()._timestamp_ms),
                   std::uint64_t>);
static_assert(
    std::is_same_v<decltype(std::declval<AlarmFrame &>()._payload),
                   std::vector<std::uint8_t>>);

using ExpectedCrc = std::uint32_t (*)(const std::uint8_t *,
                                      std::size_t) noexcept;
using ExpectedSize = AlarmFrameSizeResult (*)(const std::uint8_t *,
                                               std::size_t) noexcept;
using ExpectedEncode = AlarmEncodeResult (*)(const AlarmFrame &) noexcept;
using ExpectedDecode = AlarmDecodeResult (*)(const std::uint8_t *,
                                              std::size_t) noexcept;

static_assert(std::is_same_v<decltype(&alarm_crc32), ExpectedCrc>);
static_assert(std::is_same_v<decltype(&alarm_frame_size), ExpectedSize>);
static_assert(std::is_same_v<decltype(&encode_alarm_frame), ExpectedEncode>);
static_assert(std::is_same_v<decltype(&decode_alarm_frame), ExpectedDecode>);

/*
 * 确认帧类型辅助函数不会把未知枚举值误判为合法类型。
 */
void test_frame_type_classification() {
  assert(is_valid_alarm_frame_type(AlarmFrameType::ALARM));
  assert(is_valid_alarm_frame_type(AlarmFrameType::ACK));
  assert(is_valid_alarm_frame_type(AlarmFrameType::PING));
  assert(is_valid_alarm_frame_type(AlarmFrameType::PONG));
  assert(!is_valid_alarm_frame_type(static_cast<AlarmFrameType>(99U)));

  assert(alarm_frame_requires_payload(AlarmFrameType::ALARM));
  assert(!alarm_frame_requires_payload(AlarmFrameType::ACK));
  assert(is_alarm_control_frame(AlarmFrameType::ACK));
  assert(is_alarm_control_frame(AlarmFrameType::PING));
  assert(is_alarm_control_frame(AlarmFrameType::PONG));
  assert(!is_alarm_control_frame(AlarmFrameType::ALARM));
}

/*
 * success() 必须同时检查状态和结果对象，不能只检查其中一个字段。
 */
void test_result_invariants() {
  const AlarmEncodeResult empty_encode{};
  assert(!empty_encode.success());

  const AlarmEncodeResult encoded{
      AlarmProtocolStatus::SUCCESS,
      std::vector<std::uint8_t>(ALARM_HEADER_SIZE, 0U)};
  assert(encoded.success());

  const AlarmDecodeResult empty_decode{};
  assert(!empty_decode.success());

  const AlarmDecodeResult decoded{
      AlarmProtocolStatus::SUCCESS,
      AlarmFrame{AlarmFrameType::PING, 1U, {}, 2U, {}}};
  assert(decoded.success());

  const AlarmFrameSizeResult empty_size{};
  assert(!empty_size.success());

  const AlarmFrameSizeResult frame_size{
      AlarmProtocolStatus::SUCCESS, ALARM_HEADER_SIZE};
  assert(frame_size.success());
}

/*
 * AlarmFrame 使用值语义；修改副本不能影响原始帧。
 */
void test_alarm_frame_value_semantics() {
  AlarmFrame original{AlarmFrameType::ALARM,
                      7U,
                      {},
                      8U,
                      {1U, 2U, 3U}};
  AlarmFrame copy = original;
  copy._payload[0] = 9U;

  assert(original._payload[0] == 1U);
  assert(copy._payload[0] == 9U);
  assert(copy != original);
}

} // namespace

int main() {
  test_frame_type_classification();
  test_result_invariants();
  test_alarm_frame_value_semantics();
  std::cout << "M9_ALARM_PROTOCOL_CONTRACT=PASS\n";
  return 0;
}
