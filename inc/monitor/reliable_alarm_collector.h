#ifndef __RELIABLE_ALARM_COLLECTOR_H__
#define __RELIABLE_ALARM_COLLECTOR_H__

#include "engine.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace TLSSMON {

/** @brief 接收端连接数、处理计数和 inbox 统计可用性快照。 */
struct ReliableAlarmCollectorStatus final {
  bool _active{false};
  std::size_t _clients{0U};

  std::uint64_t _rejected_clients{0U};
  std::uint64_t _accepted{0U};
  std::uint64_t _duplicates{0U};
  std::uint64_t _protocol_errors{0U};
  std::uint64_t _crc_errors{0U};
  std::uint64_t _persist_failures{0U};
  std::uint64_t _corrupt_files{0U};

  bool _spool_stats_available{false};
  int _spool_error{0};
};

/** @brief 接收端绑定地址、inbox 路径、客户端上限和超时。 */
struct ReliableAlarmCollectorConfig final {
  std::string _bind_host;
  std::uint16_t _listen_port{0U};
  std::filesystem::path _inbox_dir;
  std::size_t _max_clients{64U};
  std::chrono::milliseconds _client_timeout{5000};
};

/** @brief 接收端初始化及恢复阶段的状态。 */
enum class ReliableAlarmCollectorSetupStatus : std::uint8_t {
  SUCCESS,
  INVALID_CONFIG,
  ENGINE_NOT_READY,
  SPOOL_ERROR,
  RECOVERY_ERROR,
  WAKEUP_ERROR,
  SOCKET_ERROR,
  THREAD_ERROR
};

/**
 * @brief 接收可靠告警，持久化后 ACK，并将有效记录写入 Engine。
 * @note Collector 持有 Engine 的非拥有引用。
 */
class ReliableAlarmCollector final {
public:
  /**
   * @brief Engine 不归 Collector 所有。
   *
   * Engine 必须：
   * 1. 已完成 init()；
   * 2. 处于 READY 或 RUNNING；
   * 3. 生命周期长于 Collector。
   * @param engine 用于注册事件或读写监控数据的 Engine；由调用方持有。
   * @param config 绑定地址与端口、inbox 路径、客户端上限和空闲超时。
   */
  ReliableAlarmCollector(Engine &engine, ReliableAlarmCollectorConfig config);

  ~ReliableAlarmCollector();

  ReliableAlarmCollector(const ReliableAlarmCollector &) = delete;

  ReliableAlarmCollector &operator=(const ReliableAlarmCollector &) = delete;

  ReliableAlarmCollector(ReliableAlarmCollector &&) = delete;

  ReliableAlarmCollector &operator=(ReliableAlarmCollector &&) = delete;

  /**
   * @brief 表示恢复已经完成，并且监听线程正在工作。
   * @return 初始化完成并可执行操作时返回 true。
   */
  [[nodiscard]]
  bool ready() const noexcept;

  /**
   * @brief 返回初始化失败时的具体状态。
   * @return 对象初始化的状态码。
   */
  [[nodiscard]]
  ReliableAlarmCollectorSetupStatus setup_status() const noexcept;

  /**
   * @brief 返回初始化失败时的系统错误码。
   * @return 初始化失败的系统错误码；没有系统错误时为 0。
   */
  [[nodiscard]]
  int setup_error() const noexcept;

  /**
   * @brief 配置端口为 0 时，返回系统实际分配的端口。
   * @return 实际绑定的端口；未就绪时返回 0。
   */
  [[nodiscard]]
  std::uint16_t bound_port() const noexcept;

  /**
   * @brief 返回构造时提供的接收端配置。
   * @return 构造时保存的配置引用。
   */
  [[nodiscard]]
  const ReliableAlarmCollectorConfig &config() const noexcept;

  /**
   * @brief 幂等停止：
   *
   * 1. 唤醒 poll()；
   * 2. 停止接收；
   * 3. 关闭客户端；
   * 4. join 工作线程。
   */
  void stop() noexcept;

  /**
   * @brief 返回当前连接和告警处理统计的快照。
   * @return 当前运行状态和统计信息的独立快照。
   */
  [[nodiscard]]
  ReliableAlarmCollectorStatus status() const;

private:
  struct Impl;

  Engine &_engine;
  const ReliableAlarmCollectorConfig _config;
  std::unique_ptr<Impl> _impl;
};
} // namespace TLSSMON

#endif // __RELIABLE_ALARM_COLLECTOR_H__
