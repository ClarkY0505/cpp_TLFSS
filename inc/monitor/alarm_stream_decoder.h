#ifndef __ALARM_STREAM_DECODER_H__
#define __ALARM_STREAM_DECODER_H__

#include "alarm_protocol.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace TLSSMON {
namespace AlarmWire {

/*
 * Decoder 内部最多缓存一个完整告警帧。
 *
 * 输入即使一次包含很多帧，也会逐帧处理，不会把整个输入块
 * 一次性保存到内部缓冲区。
 */
inline constexpr std::size_t ALARM_STREAM_BUFFER_LIMIT = ALARM_MAX_FRAME_SIZE;

enum class AlarmStreamStatus : std::uint8_t {
  /*
   * 本次输入处理成功，并且没有残留的半帧。
   */
  SUCCESS,
  /*
   * 输入处理成功，但内部仍保存着未完成的头部或 payload。
   */
  NEED_MORE_DATA,
  /*
   * data == nullptr，但 size != 0。
   *
   * 这是调用参数错误，不污染 Decoder 状态。
   */
  INVALID_INPUT,
  /*
   * 收到非法头部、错误 CRC、错误长度或 EOF 截断帧。
   */
  PROTOCOL_ERROR,
  /*
   * 内部缓冲区超过硬上限。
   *
   * 在当前 4096 字节 payload 契约下正常不会发生，
   * 该状态用于防御未来协议变更。
   */
  BUFFER_LIMIT_EXCEEDED,
  /*
   * vector 分配失败。
   */
  ALLOCATION_FAILED,
  /*
   * Decoder 已经进入失败状态，调用方尚未 reset()。
   */
  ALREADY_FAILED
};

struct AlarmStreamFeedResult final {
  AlarmStreamStatus _status{AlarmStreamStatus::SUCCESS};
  /*
   * 本次输入中成功解析出的所有完整帧。
   *
   * 即使后续字节发生协议错误，错误之前已经成功解析的帧
   * 仍保存在这里。
   */
  std::vector<AlarmFrame> _frames;
  /*
   * 当 _status == PROTOCOL_ERROR 时，保存具体协议错误。
   */
  std::optional<AlarmProtocolStatus> _protocol_status;
  /*
   * SUCCESS 和 NEED_MORE_DATA 都表示输入被正常接受。
   */
  [[nodiscard]] bool success() const noexcept;
};

class AlarmStreamDecoder final {
public:
  AlarmStreamDecoder() = default;

  AlarmStreamDecoder(const AlarmStreamDecoder &) = delete;
  AlarmStreamDecoder &operator=(const AlarmStreamDecoder &) = delete;

  AlarmStreamDecoder(AlarmStreamDecoder &&) = delete;
  AlarmStreamDecoder &operator=(AlarmStreamDecoder &&) = delete;

  /*
   * 输入一段 TCP 字节。
   *
   * 支持：
   *
   * - 一个帧分多次输入；
   * - 一次输入多个帧；
   * - 完整帧后跟半个帧；
   * - size == 0 的空输入。
   */
  [[nodiscard]]
  AlarmStreamFeedResult feed(const std::uint8_t *data,
                             std::size_t size) noexcept;

  /*
   * 告知 Decoder 对端已经关闭写方向或 recv() 返回 0。
   *
   * 如果此时还有残留字节，则判定为截断帧。
   */
  [[nodiscard]]
  AlarmStreamFeedResult finish() noexcept;

  /*
   * 清除失败状态和残留数据，使对象可以用于一条新连接。
   */
  void reset() noexcept;

  [[nodiscard]]
  bool failed() const noexcept;

  [[nodiscard]]
  std::size_t buffered_size() const noexcept;

  [[nodiscard]]
  std::optional<std::size_t> expected_frame_size() const noexcept;

  [[nodiscard]]
  std::optional<AlarmProtocolStatus> protocol_error() const noexcept;

private:
  AlarmStreamFeedResult
  fail_protocol(AlarmProtocolStatus status,
                std::vector<AlarmFrame> completed) noexcept;

  AlarmStreamFeedResult fail_stream(AlarmStreamStatus status,
                                    std::vector<AlarmFrame> completed) noexcept;

  std::vector<std::uint8_t> _buffer;

  /*
   * 未读取完整头部时为空；读取完整头部后保存完整帧长度。
   */
  std::optional<std::size_t> _expected_frame_size;

  bool _failed{false};

  AlarmStreamStatus _failure_status{AlarmStreamStatus::SUCCESS};

  std::optional<AlarmProtocolStatus> _protocol_error;
};

} // namespace AlarmWire
} // namespace TLSSMON
#endif // __ALARM_STREAM_DECODER_H__
