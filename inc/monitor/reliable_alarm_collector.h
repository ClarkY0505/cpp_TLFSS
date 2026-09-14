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

struct ReliableAlarmCollectorConfig final {
  std::string _bind_host;
  std::uint16_t _listen_port{0U};
  std::filesystem::path _inbox_dir;
  std::size_t _max_clients{64U};
  std::chrono::milliseconds _client_timeout{5000};
};

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

class ReliableAlarmCollector final {
public:
  /*
   * Engine 不归 Collector 所有。
   *
   * Engine 必须：
   * 1. 已完成 init()；
   * 2. 处于 READY 或 RUNNING；
   * 3. 生命周期长于 Collector。
   */
  ReliableAlarmCollector(Engine &engine, ReliableAlarmCollectorConfig config);

  ~ReliableAlarmCollector();

  ReliableAlarmCollector(const ReliableAlarmCollector &) = delete;

  ReliableAlarmCollector &operator=(const ReliableAlarmCollector &) = delete;

  ReliableAlarmCollector(ReliableAlarmCollector &&) = delete;

  ReliableAlarmCollector &operator=(ReliableAlarmCollector &&) = delete;

  /*
   * 表示恢复已经完成，并且监听线程正在工作。
   */
  [[nodiscard]]
  bool ready() const noexcept;

  [[nodiscard]]
  ReliableAlarmCollectorSetupStatus setup_status() const noexcept;

  [[nodiscard]]
  int setup_error() const noexcept;

  /*
   * 配置端口为 0 时，返回系统实际分配的端口。
   */
  [[nodiscard]]
  std::uint16_t bound_port() const noexcept;

  [[nodiscard]]
  const ReliableAlarmCollectorConfig &config() const noexcept;

  /*
   * 幂等停止：
   *
   * 1. 唤醒 poll()；
   * 2. 停止接收；
   * 3. 关闭客户端；
   * 4. join 工作线程。
   */
  void stop() noexcept;

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
