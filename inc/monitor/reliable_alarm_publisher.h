#ifndef __RELIABLE_ALARM_PUBLISHER_H__
#define __RELIABLE_ALARM_PUBLISHER_H__

#include "alarm_publisher_types.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace TLSSMON {
struct ReliableAlarmPublisherStatus final {
  bool _active{false};
  bool _connected{false};

  std::string _collector_host;
  std::uint16_t _collector_port{0U};

  std::uint64_t _pending_files{0U};
  std::uint64_t _pending_bytes{0U};
  std::uint64_t _corrupt_files{0U};

  std::uint64_t _connect_failures{0U};
  std::uint64_t _send_failures{0U};
  std::uint64_t _ack_timeouts{0U};
  std::uint64_t _acks{0U};

  std::chrono::milliseconds _current_backoff{0};

  /*
   * false 表示本次读取 Spool 统计信息失败。
   * 此时 pending/corrupt 字段不能作为有效统计值使用。
   */
  bool _spool_stats_available{false};
  int _spool_error{0};
};

struct ReliableAlarmPublisherConfig final {
  std::string _collector_host;
  std::uint16_t _collector_port{0U};
  std::uint64_t _source_id{0U};
  std::filesystem::path _outbox_dir;
  std::uint64_t _max_outbox_bytes{0U};
  std::chrono::milliseconds _ack_timeout{2000};
  std::chrono::milliseconds _max_backoff{30000};
};

enum class ReliableAlarmPublisherSetupStatus : std::uint8_t {
  SUCCESS,
  INVALID_CONFIG,
  SPOOL_ERROR,
  THREAD_ERROR
};

class ReliableAlarmPublisher final {
public:
  explicit ReliableAlarmPublisher(ReliableAlarmPublisherConfig config);

  ~ReliableAlarmPublisher();

  ReliableAlarmPublisher(const ReliableAlarmPublisher &) = delete;

  ReliableAlarmPublisher &operator=(const ReliableAlarmPublisher &) = delete;

  ReliableAlarmPublisher(ReliableAlarmPublisher &&) = delete;

  ReliableAlarmPublisher &operator=(ReliableAlarmPublisher &&) = delete;

  /*
   * ready() 只表示 Publisher 可以接受新告警。
   * Collector 暂时断线不会令 ready() 变成 false。
   */
  [[nodiscard]]
  bool ready() const noexcept;

  [[nodiscard]]
  ReliableAlarmPublisherSetupStatus setup_status() const noexcept;

  [[nodiscard]]
  int setup_error() const noexcept;

  [[nodiscard]]
  const ReliableAlarmPublisherConfig &config() const noexcept;

  /*
   * 只执行：
   *
   * V2 编码 → 告警帧编码 → outbox 原子落盘 → 唤醒发送线程
   *
   * 不进行 connect/send/recv。
   */
  [[nodiscard]]
  AlarmEnqueueResult enqueue(MonData::StoredRecord record);

  [[nodiscard]]
  ReliableAlarmPublisherStatus status() const;

private:
  struct Impl;

  const ReliableAlarmPublisherConfig _config;
  std::unique_ptr<Impl> _impl;
};

} // namespace TLSSMON

#endif // __RELIABLE_ALARM_PUBLISHER_H__
