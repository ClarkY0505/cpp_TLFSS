#include "alarm_protocol.h"
#include <algorithm>

namespace TLSSMON {
namespace AlarmWire {
namespace {
/*
 * CRC32 使用标准 IEEE 多项式的反射形式。
 */
inline constexpr std::uint32_t CRC32_INITIAL = UINT32_C(0xffffffff);
inline constexpr std::uint32_t CRC32_POLYNOMIAL = UINT32_C(0xedb88320);

/*
 * 第一版协议 flags 必须为 0。
 */
inline constexpr std::uint16_t ALARM_CURRENT_FLAGS = 0U;
AlarmEncodeResult make_encode_error(AlarmProtocolStatus status) {
  return AlarmEncodeResult{status, {}};
}

AlarmDecodeResult make_decode_error(AlarmProtocolStatus status) {
  return AlarmDecodeResult{status, std::nullopt};
}

AlarmFrameSizeResult make_size_error(AlarmProtocolStatus status) {
  return AlarmFrameSizeResult{status, std::nullopt};
}

/*
 * 所有整数逐字节写入，避免：
 *
 * 1. 未对齐整数访问；
 * 2. 依赖主机字节序；
 * 3. strict-aliasing 问题。
 */
void put_u16(std::uint8_t *output, std::uint16_t value) noexcept {
  output[0] = static_cast<std::uint8_t>(value >> 8U);
  output[1] = static_cast<std::uint8_t>(value);
}

void put_u32(std::uint8_t *output, std::uint32_t value) noexcept {
  output[0] = static_cast<std::uint8_t>(value >> 24U);
  output[1] = static_cast<std::uint8_t>(value >> 16U);
  output[2] = static_cast<std::uint8_t>(value >> 8U);
  output[3] = static_cast<std::uint8_t>(value);
}

void put_u64(std::uint8_t *output, std::uint64_t value) noexcept {
  output[0] = static_cast<std::uint8_t>(value >> 56U);
  output[1] = static_cast<std::uint8_t>(value >> 48U);
  output[2] = static_cast<std::uint8_t>(value >> 40U);
  output[3] = static_cast<std::uint8_t>(value >> 32U);
  output[4] = static_cast<std::uint8_t>(value >> 24U);
  output[5] = static_cast<std::uint8_t>(value >> 16U);
  output[6] = static_cast<std::uint8_t>(value >> 8U);
  output[7] = static_cast<std::uint8_t>(value);
}

std::uint16_t get_u16(const std::uint8_t *input) noexcept {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(input[0]) << 8U) |
      static_cast<std::uint16_t>(input[1]));
}

std::uint32_t get_u32(const std::uint8_t *input) noexcept {
  return (static_cast<std::uint32_t>(input[0]) << 24U) |
         (static_cast<std::uint32_t>(input[1]) << 16U) |
         (static_cast<std::uint32_t>(input[2]) << 8U) |
         static_cast<std::uint32_t>(input[3]);
}

std::uint64_t get_u64(const std::uint8_t *input) noexcept {
  std::uint64_t value = 0U;

  for (std::size_t i = 0U; i < sizeof(std::uint64_t); ++i) {
    value = (value << 8U) | static_cast<std::uint64_t>(input[i]);
  }

  return value;
}

bool has_valid_magic(const std::uint8_t *data) noexcept {
  return std::equal(ALARM_MAGIC.begin(), ALARM_MAGIC.end(),
                    data + ALARM_MAGIC_OFFSET);
}

bool is_valid_raw_type(std::uint8_t raw_type) noexcept {
  return raw_type >= static_cast<std::uint8_t>(AlarmFrameType::ALARM) &&
         raw_type <= static_cast<std::uint8_t>(AlarmFrameType::PONG);
}

std::uint32_t crc32_update(std::uint32_t crc, const std::uint8_t *data,
                           std::size_t size) noexcept {
  for (std::size_t i = 0U; i < size; ++i) {
    crc ^= static_cast<std::uint32_t>(data[i]);

    for (unsigned int bit = 0U; bit < 8U; ++bit) {
      const std::uint32_t mask = static_cast<std::uint32_t>(
          -static_cast<std::int32_t>(crc & UINT32_C(1)));

      crc = (crc >> 1U) ^ (CRC32_POLYNOMIAL & mask);
    }
  }

  return crc;
}

/*
 * 告警帧 CRC 覆盖：
 *
 * bytes[0, 44)  固定头部中 CRC 字段以前的内容
 * bytes[48, end) payload
 *
 * bytes[44, 48) CRC 字段本身不参与计算。
 */
std::uint32_t calculate_frame_crc(const std::uint8_t *data,
                                  std::size_t payload_size) noexcept {
  std::uint32_t crc = crc32_update(CRC32_INITIAL, data, ALARM_CRC32_OFFSET);

  crc = crc32_update(crc, data + ALARM_HEADER_SIZE, payload_size);

  return crc ^ CRC32_INITIAL;
}

AlarmProtocolStatus
validate_frame_description(AlarmFrameType type,
                           std::size_t payload_size) noexcept {
  if (!is_valid_alarm_frame_type(type)) {
    return AlarmProtocolStatus::INVALID_TYPE;
  }

  if (payload_size > ALARM_MAX_PAYLOAD_SIZE) {
    return AlarmProtocolStatus::PAYLOAD_TOO_LARGE;
  }

  if (alarm_frame_requires_payload(type) && payload_size == 0U) {
    return AlarmProtocolStatus::INVALID_PAYLOAD_LENGTH;
  }

  if (is_alarm_control_frame(type) && payload_size != 0U) {
    return AlarmProtocolStatus::INVALID_PAYLOAD_LENGTH;
  }

  return AlarmProtocolStatus::SUCCESS;
}

} // namespace

bool AlarmEncodeResult::success() const noexcept {
  return _status == AlarmProtocolStatus::SUCCESS &&
         _bytes.size() >= ALARM_HEADER_SIZE &&
         _bytes.size() <= ALARM_MAX_FRAME_SIZE;
}

bool AlarmDecodeResult::success() const noexcept {
  return _status == AlarmProtocolStatus::SUCCESS && _frame.has_value();
}

bool AlarmFrameSizeResult::success() const noexcept {
  return _status == AlarmProtocolStatus::SUCCESS && _frame_size.has_value() &&
         *_frame_size >= ALARM_HEADER_SIZE &&
         *_frame_size <= ALARM_MAX_FRAME_SIZE;
}

std::uint32_t alarm_crc32(const std::uint8_t *data, std::size_t size) noexcept {
  /*
   * 非零长度却没有输入地址属于调用错误。
   *
   * 接口没有错误状态可返回，因此与参考 M9 一样返回 0。
   */
  if (data == nullptr && size != 0U) {
    return 0U;
  }

  return crc32_update(CRC32_INITIAL, data, size) ^ CRC32_INITIAL;
}

AlarmFrameSizeResult alarm_frame_size(const std::uint8_t *prefix,
                                      std::size_t prefix_size) noexcept {
  if (prefix == nullptr || prefix_size == 0U) {
    return make_size_error(AlarmProtocolStatus::EMPTY_INPUT);
  }

  /*
   * TCP 流解析器至少要收到完整固定头部，才能读取 payload_length。
   */
  if (prefix_size < ALARM_HEADER_SIZE) {
    return make_size_error(AlarmProtocolStatus::SHORT_HEADER);
  }

  if (!has_valid_magic(prefix)) {
    return make_size_error(AlarmProtocolStatus::WRONG_MAGIC);
  }

  if (prefix[ALARM_VERSION_OFFSET] != ALARM_PROTOCOL_VERSION) {
    return make_size_error(AlarmProtocolStatus::WRONG_VERSION);
  }

  const std::uint8_t raw_type = prefix[ALARM_TYPE_OFFSET];

  if (!is_valid_raw_type(raw_type)) {
    return make_size_error(AlarmProtocolStatus::INVALID_TYPE);
  }

  if (get_u16(prefix + ALARM_FLAGS_OFFSET) != ALARM_CURRENT_FLAGS) {
    return make_size_error(AlarmProtocolStatus::INVALID_FLAGS);
  }

  const std::uint32_t payload_length =
      get_u32(prefix + ALARM_PAYLOAD_LENGTH_OFFSET);

  const AlarmFrameType type = static_cast<AlarmFrameType>(raw_type);

  const AlarmProtocolStatus validation =
      validate_frame_description(type, payload_length);

  if (validation != AlarmProtocolStatus::SUCCESS) {
    return make_size_error(validation);
  }

  /*
   * payload_length 已经限制为不超过 4096，因此加法不会溢出。
   */
  const std::size_t total_size =
      ALARM_HEADER_SIZE + static_cast<std::size_t>(payload_length);

  /*
   * prefix 只需要包含完整头部。
   *
   * 即使 payload 尚未全部到达，也能把完整帧长度返回给后续
   * TCP 流式解析器。
   */
  return AlarmFrameSizeResult{AlarmProtocolStatus::SUCCESS, total_size};
}

AlarmEncodeResult encode_alarm_frame(const AlarmFrame &frame) noexcept {
  const AlarmProtocolStatus validation =
      validate_frame_description(frame._type, frame._payload.size());

  if (validation != AlarmProtocolStatus::SUCCESS) {
    return make_encode_error(validation);
  }

  try {
    const std::size_t total_size = ALARM_HEADER_SIZE + frame._payload.size();

    std::vector<std::uint8_t> bytes(total_size, 0U);

    std::copy(ALARM_MAGIC.begin(), ALARM_MAGIC.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(ALARM_MAGIC_OFFSET));

    bytes[ALARM_VERSION_OFFSET] = ALARM_PROTOCOL_VERSION;
    bytes[ALARM_TYPE_OFFSET] = static_cast<std::uint8_t>(frame._type);

    put_u16(bytes.data() + ALARM_FLAGS_OFFSET, ALARM_CURRENT_FLAGS);

    put_u32(bytes.data() + ALARM_PAYLOAD_LENGTH_OFFSET,
            static_cast<std::uint32_t>(frame._payload.size()));

    put_u64(bytes.data() + ALARM_SOURCE_ID_OFFSET, frame._source_id);

    std::copy(frame._message_id.begin(), frame._message_id.end(),
              bytes.begin() +
                  static_cast<std::ptrdiff_t>(ALARM_MESSAGE_ID_OFFSET));

    put_u64(bytes.data() + ALARM_TIMESTAMP_OFFSET, frame._timestamp_ms);

    if (!frame._payload.empty()) {
      std::copy(frame._payload.begin(), frame._payload.end(),
                bytes.begin() + static_cast<std::ptrdiff_t>(ALARM_HEADER_SIZE));
    }

    const std::uint32_t crc =
        calculate_frame_crc(bytes.data(), frame._payload.size());

    put_u32(bytes.data() + ALARM_CRC32_OFFSET, crc);

    return AlarmEncodeResult{AlarmProtocolStatus::SUCCESS, std::move(bytes)};
  } catch (const std::bad_alloc &) {
    return make_encode_error(AlarmProtocolStatus::ALLOCATION_FAILED);
  }
}

AlarmDecodeResult decode_alarm_frame(const std::uint8_t *data,
                                     std::size_t size) noexcept {
  const AlarmFrameSizeResult size_result = alarm_frame_size(data, size);

  if (!size_result.success()) {
    return make_decode_error(size_result._status);
  }

  if (size != *size_result._frame_size) {
    return make_decode_error(AlarmProtocolStatus::INVALID_TOTAL_LENGTH);
  }

  const std::size_t payload_size = size - ALARM_HEADER_SIZE;

  const std::uint32_t expected_crc = get_u32(data + ALARM_CRC32_OFFSET);

  const std::uint32_t actual_crc = calculate_frame_crc(data, payload_size);

  if (expected_crc != actual_crc) {
    return make_decode_error(AlarmProtocolStatus::CRC_MISMATCH);
  }

  try {
    AlarmFrame frame;

    frame._type = static_cast<AlarmFrameType>(data[ALARM_TYPE_OFFSET]);

    frame._source_id = get_u64(data + ALARM_SOURCE_ID_OFFSET);

    std::copy_n(data + ALARM_MESSAGE_ID_OFFSET, ALARM_MESSAGE_ID_SIZE,
                frame._message_id.begin());

    frame._timestamp_ms = get_u64(data + ALARM_TIMESTAMP_OFFSET);

    if (payload_size != 0U) {
      frame._payload.assign(data + ALARM_HEADER_SIZE, data + size);
    }

    return AlarmDecodeResult{AlarmProtocolStatus::SUCCESS, std::move(frame)};
  } catch (const std::bad_alloc &) {
    return make_decode_error(AlarmProtocolStatus::ALLOCATION_FAILED);
  }
}

} // namespace AlarmWire
} // namespace TLSSMON
