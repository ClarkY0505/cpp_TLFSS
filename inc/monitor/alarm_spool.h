#ifndef __ALARM_SPOOL_H__
#define __ALARM_SPOOL_H__

#include "alarm_protocol.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace TLSSMON {
namespace AlarmWire {

enum class AlarmSpoolKind : std::uint8_t { OUTBOX, INBOX };

enum class AlarmSpoolStatus : std::uint8_t {
  SUCCESS,
  /*
   * 相同 source_id + message_id 已经存在。
   */
  DUPLICATE,
  /*
   * 保存后会超过配置的最大容量。
   */
  FULL,
  /*
   * next() 没有找到待处理文件。
   */
  EMPTY,
  INVALID_ARGUMENT,
  NOT_READY,
  NOT_FOUND,
  /*
   * 调用方提供的路径不属于本 Spool 的 data 目录。
   */
  OUTSIDE_ROOT,
  /*
   * 输入或磁盘文件不是合法 ALARM 帧。
   */
  INVALID_FRAME,
  CORRUPT,
  IO_ERROR
};

struct AlarmSpoolConfig final {
  std::filesystem::path _root;
  AlarmSpoolKind _kind;

  /*
   * 只统计 pending/ 或 accepted/。
   *
   * 0 表示不限制容量。
   */
  std::uint64_t _max_bytes{0U};
};

struct AlarmSpoolStoreResult final {
  AlarmSpoolStatus _status{AlarmSpoolStatus::NOT_READY};
  std::filesystem::path _path;
  int _system_error{0};

  [[nodiscard]]
  bool success() const noexcept {
    return _status == AlarmSpoolStatus::SUCCESS;
  }

  /*
   * DUPLICATE 表示相同告警已经持久化，也能满足可靠性门禁。
   */
  [[nodiscard]]
  bool durable() const noexcept {
    return _status == AlarmSpoolStatus::SUCCESS ||
           _status == AlarmSpoolStatus::DUPLICATE;
  }
};

struct AlarmSpoolPathResult final {
  AlarmSpoolStatus _status{AlarmSpoolStatus::NOT_READY};
  std::filesystem::path _path;
  int _system_error{0};

  [[nodiscard]]
  bool success() const noexcept {
    return _status == AlarmSpoolStatus::SUCCESS;
  }
};

struct AlarmSpoolReadResult final {
  AlarmSpoolStatus _status{AlarmSpoolStatus::NOT_READY};
  std::filesystem::path _path;
  std::vector<std::uint8_t> _bytes;
  int _system_error{0};

  [[nodiscard]]
  bool success() const noexcept {
    return _status == AlarmSpoolStatus::SUCCESS;
  }
};

struct AlarmSpoolStats final {
  std::uint64_t _files{0U};
  std::uint64_t _bytes{0U};
  std::uint64_t _corrupt_files{0U};
  std::uint64_t _corrupt_bytes{0U};
};

struct AlarmSpoolStatsResult final {
  AlarmSpoolStatus _status{AlarmSpoolStatus::NOT_READY};
  AlarmSpoolStats _stats;
  int _system_error{0};

  [[nodiscard]]
  bool success() const noexcept {
    return _status == AlarmSpoolStatus::SUCCESS;
  }
};

class AlarmSpool final {
public:
  explicit AlarmSpool(AlarmSpoolConfig config);
  ~AlarmSpool();

  AlarmSpool(const AlarmSpool &) = delete;
  AlarmSpool &operator=(const AlarmSpool &) = delete;

  AlarmSpool(AlarmSpool &&) = delete;
  AlarmSpool &operator=(AlarmSpool &&) = delete;

  [[nodiscard]]
  bool ready() const noexcept;

  [[nodiscard]]
  AlarmSpoolStatus setup_status() const noexcept;

  [[nodiscard]]
  int setup_error() const noexcept;

  [[nodiscard]]
  const AlarmSpoolConfig &config() const noexcept;

  /*
   * 保存完整、已经编码的 ALARM 信封。
   *
   * source_id 和 message_id 必须与信封内部字段一致。
   */
  [[nodiscard]]
  AlarmSpoolStoreResult store(std::uint64_t source_id,
                              const AlarmMessageId &message_id,
                              const std::vector<std::uint8_t> &wire);

  /*
   * 返回字典序最小的待处理告警。
   */
  [[nodiscard]]
  AlarmSpoolPathResult next() const;

  /*
   * 读取并验证一个 Spool 文件。
   *
   * 文件损坏时自动移入 corrupt/。
   */
  [[nodiscard]]
  AlarmSpoolReadResult read(const std::filesystem::path &path);

  /*
   * OUTBOX 只能在收到 ACK 后调用 remove()。
   */
  [[nodiscard]]
  AlarmSpoolPathResult remove(const std::filesystem::path &path);

  [[nodiscard]]
  AlarmSpoolPathResult quarantine(const std::filesystem::path &path);

  [[nodiscard]]
  AlarmSpoolStatsResult stats() const;

private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

} // namespace AlarmWire
} // namespace TLSSMON

#endif // __ALARM_SPOOL_H__
