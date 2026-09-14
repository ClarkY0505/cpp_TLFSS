#ifndef __ALARM_PUBLISHER_TYPES_H__
#define __ALARM_PUBLISHER_TYPES_H__

#include "monitor_data.h"

#include <cstdint>
#include <functional>

namespace TLSSMON {

/*
 * ReliableAlarmPublisher::enqueue() 的结果。
 *
 * 只有 SUCCESS 表示候选记录已经可靠写入 outbox，
 * MonitorStore 才允许提交内存状态。
 */
enum class AlarmEnqueueStatus : std::uint8_t {
  SUCCESS,

  /*
   * Publisher 尚未完成初始化或者已经停止。
   */
  NOT_READY,

  /*
   * StoredRecord 无法编码成可靠告警。
   */
  ENCODE_FAILED,

  /*
   * 无法从系统安全随机源生成 message ID。
   */
  RANDOM_FAILED,

  /*
   * outbox 已达到容量上限。
   */
  SPOOL_FULL,

  /*
   * outbox 文件写入、同步或重命名失败。
   */
  IO_ERROR
};

struct AlarmEnqueueResult final {
  AlarmEnqueueStatus _status{AlarmEnqueueStatus::NOT_READY};

  /*
   * errno 等平台错误。
   *
   * 没有底层系统错误时为 0。
   */
  int _system_error{0};

  [[nodiscard]]
  constexpr bool durable() const noexcept {
    return _status == AlarmEnqueueStatus::SUCCESS;
  }
};

/*
 * 参数使用值语义：
 *
 * - Publisher 收到 StoredRecord 副本；
 * - Publisher 修改副本不会影响 Store 候选记录；
 * - 返回 SUCCESS 前必须已经完成 outbox 持久化；
 * - 此回调禁止执行网络 I/O。
 */
using AlarmPublisher = std::function<AlarmEnqueueResult(MonData::StoredRecord)>;

} // namespace TLSSMON

#endif // __ALARM_PUBLISHER_TYPES_H__
