#ifndef __ALARM_SPOOL_H__
#define __ALARM_SPOOL_H__

#include "alarm_protocol.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace TLSSMON {
namespace AlarmWire {

/** @brief 持久化目录用于发送端 outbox 或接收端 inbox。 */
enum class AlarmSpoolKind : std::uint8_t { OUTBOX, INBOX };

/** @brief Spool 的持久化、查询和文件校验状态。 */
enum class AlarmSpoolStatus : std::uint8_t {
  SUCCESS,
  /**
   * @brief 相同 source_id + message_id 已经存在。
   */
  DUPLICATE,
  /**
   * @brief 保存后会超过配置的最大容量。
   */
  FULL,
  /**
   * @brief next() 没有找到待处理文件。
   */
  EMPTY,
  INVALID_ARGUMENT,
  NOT_READY,
  NOT_FOUND,
  /**
   * @brief 调用方提供的路径不属于本 Spool 的 data 目录。
   */
  OUTSIDE_ROOT,
  /**
   * @brief 输入或磁盘文件不是合法 ALARM 帧。
   */
  INVALID_FRAME,
  CORRUPT,
  IO_ERROR
};

/** @brief Spool 根目录、用途和容量上限。 */
struct AlarmSpoolConfig final {
  std::filesystem::path _root;
  AlarmSpoolKind _kind;

  /**
   * @brief 只统计 pending/ 或 accepted/。
   *
   * 0 表示不限制容量。
   */
  std::uint64_t _max_bytes{0U};
};

/** @brief 保存结果；DUPLICATE 同样满足已持久化条件。 */
struct AlarmSpoolStoreResult final {
  AlarmSpoolStatus _status{AlarmSpoolStatus::NOT_READY};
  std::filesystem::path _path;
  int _system_error{0};

  /**
   * @brief 判断结果是否成功。
   * @return 状态为 SUCCESS 且结果有效时返回 true。
   */
  [[nodiscard]]
  bool success() const noexcept {
    return _status == AlarmSpoolStatus::SUCCESS;
  }

  /**
   * @brief DUPLICATE 表示相同告警已经持久化，也能满足可靠性门禁。
   * @return 记录已完成可靠持久化时返回 true。
   */
  [[nodiscard]]
  bool durable() const noexcept {
    return _status == AlarmSpoolStatus::SUCCESS ||
           _status == AlarmSpoolStatus::DUPLICATE;
  }
};

/** @brief 文件路径操作的状态、路径和系统错误。 */
struct AlarmSpoolPathResult final {
  AlarmSpoolStatus _status{AlarmSpoolStatus::NOT_READY};
  std::filesystem::path _path;
  int _system_error{0};

  /**
   * @brief 判断结果是否成功。
   * @return 状态为 SUCCESS 且结果有效时返回 true。
   */
  [[nodiscard]]
  bool success() const noexcept {
    return _status == AlarmSpoolStatus::SUCCESS;
  }
};

/** @brief 已校验文件的路径、完整字节和读取状态。 */
struct AlarmSpoolReadResult final {
  AlarmSpoolStatus _status{AlarmSpoolStatus::NOT_READY};
  std::filesystem::path _path;
  std::vector<std::uint8_t> _bytes;
  int _system_error{0};

  /**
   * @brief 判断结果是否成功。
   * @return 状态为 SUCCESS 且结果有效时返回 true。
   */
  [[nodiscard]]
  bool success() const noexcept {
    return _status == AlarmSpoolStatus::SUCCESS;
  }
};

/** @brief 数据目录和隔离目录中的文件及字节计数。 */
struct AlarmSpoolStats final {
  std::uint64_t _files{0U};
  std::uint64_t _bytes{0U};
  std::uint64_t _corrupt_files{0U};
  std::uint64_t _corrupt_bytes{0U};
};

/** @brief Spool 统计读取结果。 */
struct AlarmSpoolStatsResult final {
  AlarmSpoolStatus _status{AlarmSpoolStatus::NOT_READY};
  AlarmSpoolStats _stats;
  int _system_error{0};

  /**
   * @brief 判断结果是否成功。
   * @return 状态为 SUCCESS 且结果有效时返回 true。
   */
  [[nodiscard]]
  bool success() const noexcept {
    return _status == AlarmSpoolStatus::SUCCESS;
  }
};

/**
 * @brief 持久化可靠告警信封，并隔离损坏文件。
 * @note 文件路径由本对象管理；调用方只能操作其所属目录内的文件。
 */
class AlarmSpool final {
public:
  /**
   * @brief 创建目录并恢复 Spool；通过 ready() 查询结果。
   * @param config Spool 根目录、OUTBOX/INBOX 类型及容量上限；容量为 0 表示不限制。
   */
  explicit AlarmSpool(AlarmSpoolConfig config);
  ~AlarmSpool();

  AlarmSpool(const AlarmSpool &) = delete;
  AlarmSpool &operator=(const AlarmSpool &) = delete;

  AlarmSpool(AlarmSpool &&) = delete;
  AlarmSpool &operator=(AlarmSpool &&) = delete;

  /**
   * @brief 判断目录是否已准备好接收操作。
   * @return 初始化完成并可执行操作时返回 true。
   */
  [[nodiscard]]
  bool ready() const noexcept;

  /**
   * @brief 返回初始化状态。
   * @return 对象初始化的状态码。
   */
  [[nodiscard]]
  AlarmSpoolStatus setup_status() const noexcept;

  /**
   * @brief 返回初始化失败时的系统错误码。
   * @return 初始化失败的系统错误码；没有系统错误时为 0。
   */
  [[nodiscard]]
  int setup_error() const noexcept;

  /**
   * @brief 返回构造时使用的 Spool 配置。
   * @return 构造时保存的配置引用。
   */
  [[nodiscard]]
  const AlarmSpoolConfig &config() const noexcept;

  /**
   * @brief 保存完整、已经编码的 ALARM 信封。
   *
   * source_id 和 message_id 必须与信封内部字段一致。
   * @param source_id 告警来源 ID，须与信封字段一致。
   * @param message_id 告警消息 ID，须与信封字段一致。
   * @param wire 已编码的完整 ALARM 信封字节。
   * @return 保存状态、持久化后的路径和系统错误；重复消息返回 DUPLICATE。
   */
  [[nodiscard]]
  AlarmSpoolStoreResult store(std::uint64_t source_id,
                              const AlarmMessageId &message_id,
                              const std::vector<std::uint8_t> &wire);

  /**
   * @brief 返回字典序最小的待处理告警。
   * @return 待处理文件的状态与路径；没有文件时状态为 EMPTY。
   */
  [[nodiscard]]
  AlarmSpoolPathResult next() const;

  /**
   * @brief 读取并验证一个 Spool 文件。
   *
   * 文件损坏时自动移入 corrupt/。
   * @param path Spool 管理目录内的目标文件路径。
   * @return 校验状态、文件路径与完整信封字节；损坏文件返回 CORRUPT。
   */
  [[nodiscard]]
  AlarmSpoolReadResult read(const std::filesystem::path &path);

  /**
   * @brief OUTBOX 只能在收到 ACK 后调用 remove()。
   * @param path Spool 管理目录内的目标文件路径。
   * @return 删除状态与路径；失败时包含系统错误码。
   */
  [[nodiscard]]
  AlarmSpoolPathResult remove(const std::filesystem::path &path);

  /**
   * @brief 将指定文件移入 corrupt 目录。
   * @param path Spool 管理目录内的目标文件路径。
   * @return 隔离状态与新路径；失败时包含系统错误码。
   */
  [[nodiscard]]
  AlarmSpoolPathResult quarantine(const std::filesystem::path &path);

  /**
   * @brief 读取待处理与损坏文件的统计信息。
   * @return 当前统计信息的独立快照。
   */
  [[nodiscard]]
  AlarmSpoolStatsResult stats() const;

private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

} // namespace AlarmWire
} // namespace TLSSMON

#endif // __ALARM_SPOOL_H__
