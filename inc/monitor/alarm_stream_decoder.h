#ifndef __ALARM_STREAM_DECODER_H__
#define __ALARM_STREAM_DECODER_H__

#include "alarm_protocol.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace TLSSMON {
namespace AlarmWire {

/**
 * @brief Decoder 内部最多缓存一个完整告警帧。
 *
 * 输入即使一次包含很多帧，也会逐帧处理，不会把整个输入块
 * 一次性保存到内部缓冲区。
 */
inline constexpr std::size_t ALARM_STREAM_BUFFER_LIMIT = ALARM_MAX_FRAME_SIZE;

/** @brief TCP 字节流输入和解码器生命周期状态。 */
enum class AlarmStreamStatus : std::uint8_t {
  /**
   * @brief 本次输入处理成功，并且没有残留的半帧。
   */
  SUCCESS,
  /**
   * @brief 输入处理成功，但内部仍保存着未完成的头部或 payload。
   */
  NEED_MORE_DATA,
  /**
   * @brief data == nullptr，但 size != 0。
   *
   * 这是调用参数错误，不污染 Decoder 状态。
   */
  INVALID_INPUT,
  /**
   * @brief 收到非法头部、错误 CRC、错误长度或 EOF 截断帧。
   */
  PROTOCOL_ERROR,
  /**
   * @brief 内部缓冲区超过硬上限。
   *
   * 在当前 4096 字节 payload 契约下正常不会发生，
   * 该状态用于防御未来协议变更。
   */
  BUFFER_LIMIT_EXCEEDED,
  /**
   * @brief vector 分配失败。
   */
  ALLOCATION_FAILED,
  /**
   * @brief Decoder 已经进入失败状态，调用方尚未 reset()。
   */
  ALREADY_FAILED
};

/** @brief 一次输入解析出的完整帧和错误详情。 */
struct AlarmStreamFeedResult final {
  AlarmStreamStatus _status{AlarmStreamStatus::SUCCESS};
  /**
   * @brief 本次输入中成功解析出的所有完整帧。
   *
   * 即使后续字节发生协议错误，错误之前已经成功解析的帧
   * 仍保存在这里。
   */
  std::vector<AlarmFrame> _frames;
  /**
   * @brief 当 _status == PROTOCOL_ERROR 时，保存具体协议错误。
   */
  std::optional<AlarmProtocolStatus> _protocol_status;
  /**
   * @brief SUCCESS 和 NEED_MORE_DATA 都表示输入被正常接受。
   * @return 状态为 SUCCESS 或 NEED_MORE_DATA 时返回 true。
   */
  [[nodiscard]] bool success() const noexcept;
};

/**
 * @brief 增量解析 TCP 字节流中的可靠告警信封。
 * @note 出错后需要调用 reset() 才能继续用于新连接。
 */
class AlarmStreamDecoder final {
public:
  AlarmStreamDecoder() = default;

  AlarmStreamDecoder(const AlarmStreamDecoder &) = delete;
  AlarmStreamDecoder &operator=(const AlarmStreamDecoder &) = delete;

  AlarmStreamDecoder(AlarmStreamDecoder &&) = delete;
  AlarmStreamDecoder &operator=(AlarmStreamDecoder &&) = delete;

  /**
   * @brief 输入一段 TCP 字节。
   *
   * 支持：
   *
   * - 一个帧分多次输入；
   * - 一次输入多个帧；
   * - 完整帧后跟半个帧；
   * - size == 0 的空输入。
   * @param data 输入字节缓冲区；长度由 size 指定。
   * @param size 输入缓冲区的字节数。
   * @return 本次解析状态、完整帧列表和可能的协议错误。
   */
  [[nodiscard]]
  AlarmStreamFeedResult feed(const std::uint8_t *data,
                             std::size_t size) noexcept;

  /**
   * @brief 告知 Decoder 对端已经关闭写方向或 recv() 返回 0。
   *
   * 如果此时还有残留字节，则判定为截断帧。
   * @return 输入结束后的解析状态；残留半帧会返回协议错误。
   */
  [[nodiscard]]
  AlarmStreamFeedResult finish() noexcept;

  /**
   * @brief 清除失败状态和残留数据，使对象可以用于一条新连接。
   */
  void reset() noexcept;

  /**
   * @brief 判断解码器是否处于失败状态。
   * @return 解码器处于失败状态时返回 true。
   */
  [[nodiscard]]
  bool failed() const noexcept;

  /**
   * @brief 返回当前缓存的未完成帧字节数。
   * @return 当前缓存的未完成帧字节数。
   */
  [[nodiscard]]
  std::size_t buffered_size() const noexcept;

  /**
   * @brief 头部完整后返回预期的完整帧长度。
   * @return 头部完整时返回预期帧长，否则返回 std::nullopt。
   */
  [[nodiscard]]
  std::optional<std::size_t> expected_frame_size() const noexcept;

  /**
   * @brief 返回最近一次协议错误；其他失败类型返回空值。
   * @return 最近一次协议错误；无协议错误时返回 std::nullopt。
   */
  [[nodiscard]]
  std::optional<AlarmProtocolStatus> protocol_error() const noexcept;

private:
  /**
   * @brief 记录协议错误并结束当前解码。
   * @param status 本次解码发现的协议错误。
   * @param completed 本次输入中已完成的帧。
   * @return 协议错误状态及此前已完成的帧。
   */
  AlarmStreamFeedResult
  fail_protocol(AlarmProtocolStatus status,
                std::vector<AlarmFrame> completed) noexcept;

  /**
   * @brief 记录流处理错误并结束当前解码。
   * @param status 失败状态或协议错误码。
   * @param completed 本次输入中已完成的帧。
   * @return 流处理失败状态及此前已完成的帧。
   */
  AlarmStreamFeedResult fail_stream(AlarmStreamStatus status,
                                    std::vector<AlarmFrame> completed) noexcept;

  std::vector<std::uint8_t> _buffer;

  /**
   * @brief 未读取完整头部时为空；读取完整头部后保存完整帧长度。
   */
  std::optional<std::size_t> _expected_frame_size;

  bool _failed{false};

  AlarmStreamStatus _failure_status{AlarmStreamStatus::SUCCESS};

  std::optional<AlarmProtocolStatus> _protocol_error;
};

} // namespace AlarmWire
} // namespace TLSSMON
#endif // __ALARM_STREAM_DECODER_H__
