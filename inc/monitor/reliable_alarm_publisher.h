#ifndef __RELIABLE_ALARM_PUBLISHER_H__
#define __RELIABLE_ALARM_PUBLISHER_H__

#include "alarm_publisher_types.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace TLSSMON {
/** @brief 发送端连接、outbox 和重试统计快照。 */
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

  /**
   * @brief false 表示本次读取 Spool 统计信息失败。
   * 此时 pending/corrupt 字段不能作为有效统计值使用。
   */
  bool _spool_stats_available{false};
  int _spool_error{0};
};

/** @brief Collector 目标、发送源 ID、outbox 路径和重试参数。 */
struct ReliableAlarmPublisherConfig final {
  std::string _collector_host;
  std::uint16_t _collector_port{0U};
  std::uint64_t _source_id{0U};
  std::filesystem::path _outbox_dir;
  std::uint64_t _max_outbox_bytes{0U};
  std::chrono::milliseconds _ack_timeout{2000};
  std::chrono::milliseconds _max_backoff{30000};
};

/** @brief 发送端初始化状态。 */
enum class ReliableAlarmPublisherSetupStatus : std::uint8_t {
  SUCCESS,
  INVALID_CONFIG,
  SPOOL_ERROR,
  THREAD_ERROR
};

/**
 * @brief 将告警写入 outbox，并在后台向 Collector 重试发送直到确认。
 */
class ReliableAlarmPublisher final {
public:
  /**
   * @brief 初始化 outbox 和后台发送线程。
   * @param config Collector 地址与端口、来源 ID、outbox 路径、容量及重试超时。
   */
  explicit ReliableAlarmPublisher(ReliableAlarmPublisherConfig config);

  ~ReliableAlarmPublisher();

  ReliableAlarmPublisher(const ReliableAlarmPublisher &) = delete;

  ReliableAlarmPublisher &operator=(const ReliableAlarmPublisher &) = delete;

  ReliableAlarmPublisher(ReliableAlarmPublisher &&) = delete;

  ReliableAlarmPublisher &operator=(ReliableAlarmPublisher &&) = delete;

  /**
   * @brief ready() 只表示 Publisher 可以接受新告警。
   * Collector 暂时断线不会令 ready() 变成 false。
   * @return 初始化完成并可执行操作时返回 true。
   */
  [[nodiscard]]
  bool ready() const noexcept;

  /**
   * @brief 返回初始化失败时的具体状态。
   * @return 对象初始化的状态码。
   */
  [[nodiscard]]
  ReliableAlarmPublisherSetupStatus setup_status() const noexcept;

  /**
   * @brief 返回初始化失败时的系统错误码。
   * @return 初始化失败的系统错误码；没有系统错误时为 0。
   */
  [[nodiscard]]
  int setup_error() const noexcept;

  /**
   * @brief 返回构造时提供的发送端配置。
   * @return 构造时保存的配置引用。
   */
  [[nodiscard]]
  const ReliableAlarmPublisherConfig &config() const noexcept;

  /**
   * @brief 只执行：
   *
   * V2 编码 → 告警帧编码 → outbox 原子落盘 → 唤醒发送线程
   *
   * 不进行 connect/send/recv。
   * @param record 要编码、发送或保存的监控记录。
   * @return 告警是否完成 outbox 持久化，以及失败时的错误。
   */
  [[nodiscard]]
  AlarmEnqueueResult enqueue(MonData::StoredRecord record);

  /**
   * @brief 返回当前连接、待发送和重试统计的快照。
   * @return 当前运行状态和统计信息的独立快照。
   */
  [[nodiscard]]
  ReliableAlarmPublisherStatus status() const;

private:
  struct Impl;

  const ReliableAlarmPublisherConfig _config;
  std::unique_ptr<Impl> _impl;
};

} // namespace TLSSMON

#endif // __RELIABLE_ALARM_PUBLISHER_H__
