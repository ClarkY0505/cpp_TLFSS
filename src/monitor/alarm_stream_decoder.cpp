#include "alarm_protocol.h"
#include "alarm_stream_decoder.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace TLSSMON {
namespace AlarmWire {
bool AlarmStreamFeedResult::success() const noexcept {
  return _status == AlarmStreamStatus::SUCCESS ||
         _status == AlarmStreamStatus::NEED_MORE_DATA;
}

AlarmStreamFeedResult
AlarmStreamDecoder::fail_protocol(AlarmProtocolStatus status,
                                  std::vector<AlarmFrame> completed) noexcept {
  // 已完成的前序帧仍返回调用方；错误连接进入失败态，必须 reset 后复用。
  _failed = true;
  _failure_status = AlarmStreamStatus::PROTOCOL_ERROR;
  _protocol_error = status;

  _buffer.clear();
  _expected_frame_size.reset();
  return AlarmStreamFeedResult{AlarmStreamStatus::PROTOCOL_ERROR,
                               std::move(completed), status};
}

AlarmStreamFeedResult
AlarmStreamDecoder::fail_stream(AlarmStreamStatus status,
                                std::vector<AlarmFrame> completed) noexcept {
  _failed = true;
  _failure_status = status;
  _protocol_error.reset();

  _buffer.clear();
  _expected_frame_size.reset();

  return AlarmStreamFeedResult{status, std::move(completed), std::nullopt};
}

AlarmStreamFeedResult AlarmStreamDecoder::feed(const std::uint8_t *data,
                                               std::size_t size) noexcept {
  // 一次 feed 可以补齐半帧，也可以连续解析多个完整帧。
  std::vector<AlarmFrame> completed;

  if (_failed) {
    return AlarmStreamFeedResult{
        AlarmStreamStatus::ALREADY_FAILED, {}, _protocol_error};
  }

  if (data == nullptr && size != 0U) {
    /*
     * 调用参数错误不应污染连接解析状态。
     */
    return AlarmStreamFeedResult{
        AlarmStreamStatus::INVALID_INPUT, {}, std::nullopt};
  }

  try {
    std::size_t cursor = 0U;

    while (cursor < size) {
      /*
       * 第一步：补齐固定 48 字节头部。
       */
      if (!_expected_frame_size.has_value()) {
        const std::size_t header_needed = ALARM_HEADER_SIZE - _buffer.size();
        const std::size_t available = size - cursor;
        const std::size_t take = std::min(header_needed, available);

        _buffer.insert(_buffer.end(), data + cursor, data + cursor + take);
        cursor += take;

        if (_buffer.size() < ALARM_HEADER_SIZE) {
          /*
           * 头部尚未收完，等待下一次 feed()。
           */
          break;
        }

        /*
         * 只用固定头部计算完整帧长度。
         *
         * alarm_frame_size() 会在复制 payload 前验证：
         *
         * - magic；
         * - version；
         * - type；
         * - flags；
         * - payload_length；
         * - 最大 4096 字节限制。
         */
        const AlarmFrameSizeResult size_result =
            alarm_frame_size(_buffer.data(), _buffer.size());
        if (!size_result.success()) {
          return fail_protocol(size_result._status, std::move(completed));
        }

        if (*size_result._frame_size > ALARM_STREAM_BUFFER_LIMIT) {
          return fail_stream(AlarmStreamStatus::BUFFER_LIMIT_EXCEEDED,
                             std::move(completed));
        }

        _expected_frame_size = *size_result._frame_size;
      }

      /*
       * 第二步：补齐当前帧剩余部分。
       */
      const std::size_t frame_needed = *_expected_frame_size - _buffer.size();
      const std::size_t available = size - cursor;
      const std::size_t take = std::min(frame_needed, available);

      _buffer.insert(_buffer.end(), data + cursor, data + cursor + take);
      cursor += take;

      if (_buffer.size() < *_expected_frame_size) {
        /*
         * payload 尚未收完。
         */
        break;
      }

      /*
       * 理论上前面的长度约束保证不会超过上限。
       * 这里保留防御式检查。
       */
      if (_buffer.size() > ALARM_STREAM_BUFFER_LIMIT) {
        return fail_stream(AlarmStreamStatus::BUFFER_LIMIT_EXCEEDED,
                           std::move(completed));
      }
      /*
       * 第三步：对完整帧执行 CRC 和字段解码。
       */
      AlarmDecodeResult decoded =
          decode_alarm_frame(_buffer.data(), _buffer.size());

      if (!decoded.success()) {
        /*
         * 内存分配失败不是对端协议错误。
         */
        if (decoded._status == AlarmProtocolStatus::ALLOCATION_FAILED) {
          return fail_stream(AlarmStreamStatus::ALLOCATION_FAILED,
                             std::move(completed));
        }

        return fail_protocol(decoded._status, std::move(completed));
      }

      completed.push_back(std::move(*decoded._frame));

      /*
       * 当前帧已经完成，继续解析输入中的下一个帧。
       */
      _buffer.clear();
      _expected_frame_size.reset();
    }
  } catch (const std::bad_alloc &) {
    return fail_stream(AlarmStreamStatus::ALLOCATION_FAILED,
                       std::move(completed));
  }

  const AlarmStreamStatus status = _buffer.empty()
                                       ? AlarmStreamStatus::SUCCESS
                                       : AlarmStreamStatus::NEED_MORE_DATA;

  return AlarmStreamFeedResult{status, std::move(completed), std::nullopt};
}

AlarmStreamFeedResult AlarmStreamDecoder::finish() noexcept {
  if (_failed) {
    return AlarmStreamFeedResult{
        AlarmStreamStatus::ALREADY_FAILED, {}, _protocol_error};
  }

  /*
   * TCP EOF 时没有残留，说明所有帧都完整结束。
   */
  if (_buffer.empty()) {
    return AlarmStreamFeedResult{AlarmStreamStatus::SUCCESS, {}, std::nullopt};
  }

  /*
   * 连固定头部都没有收完整。
   */
  if (_buffer.size() < ALARM_HEADER_SIZE) {
    return fail_protocol(AlarmProtocolStatus::SHORT_HEADER, {});
  }
  /*
   * 已经有完整头部，但完整帧尚未到达。
   */
  return fail_protocol(AlarmProtocolStatus::INVALID_TOTAL_LENGTH, {});
}

void AlarmStreamDecoder::reset() noexcept {
  _buffer.clear();
  _expected_frame_size.reset();

  _failed = false;
  _failure_status = AlarmStreamStatus::SUCCESS;
  _protocol_error.reset();
}

bool AlarmStreamDecoder::failed() const noexcept { return _failed; }

std::size_t AlarmStreamDecoder::buffered_size() const noexcept {
  return _buffer.size();
}

std::optional<std::size_t>
AlarmStreamDecoder::expected_frame_size() const noexcept {
  return _expected_frame_size;
}

std::optional<AlarmProtocolStatus>
AlarmStreamDecoder::protocol_error() const noexcept {
  return _protocol_error;
}

} // namespace AlarmWire
} // namespace TLSSMON
