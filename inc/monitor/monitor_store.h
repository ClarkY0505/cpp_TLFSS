#ifndef __MONITOR_STORE_H__
#define __MONITOR_STORE_H__

#include "monitor_data.h"
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace TLSSMON {
/**
 * @brief 按完整 MonitorKey 保存记录，提供线程安全的写入和快照查询。
 * @note 查询返回值副本；不暴露受互斥锁保护的内部记录。
 */
class MonitorStore final {
public:
  /**
   * @brief prepare 返回 true：
   *   候选记录已经完成持久化，可以提交 Store。
   *
   * prepare 返回 false 或抛出异常：
   *   Store 不提交候选记录。
   *
   * prepare 在 Store 锁内执行，禁止：
   *
   * - 调用 find()/query()/size()/clear()/update()；
   * - 网络发送；
   * - 等待依赖 Store 的其他线程。
   *
   * 它只允许执行有界的本地持久化操作。
   */
  using PrepareUpdate = std::function<bool(const MonData::StoredRecord &)>;
  MonitorStore() = default;
  ~MonitorStore() = default;

  MonitorStore(const MonitorStore &) = delete;
  MonitorStore &operator=(const MonitorStore &) = delete;

  MonitorStore(MonitorStore &&) = delete;
  MonitorStore &operator=(MonitorStore &&) = delete;

  /**
   * @brief 插入或更新记录，并在值变化时写入 timestamp。
   * @param data 要写入的完整监控记录。
   * @param force 允许提交原本会被忽略的初始零值。
   * @param timestamp 记录变化时使用的时间戳。
   * @return 写入状态及记录副本。
   */
  MonData::UpdateResult update(MonData::MonitorData data, bool force,
                               MonData::MonitorTimestamp timestamp);
  /**
   * @brief 仅当候选记录实际发生变化时调用 prepare。
   *
   * prepare 成功后才提交候选记录。
   * @param data 要写入的完整监控记录。
   * @param force 是否强制写入原本会被忽略的初始数值零。
   * @param timestamp 记录变化时使用的时间戳。
   * @param prepare 在 Store 锁内执行的提交前持久化回调；失败时不提交。
   * @return 监控记录的写入状态及记录副本。
   */
  MonData::UpdateResult update_prepared(MonData::MonitorData data, bool force,
                                        MonData::MonitorTimestamp timestamp,
                                        const PrepareUpdate &prepare);

  /**
   * @brief 按完整主键查询记录；未找到时返回 std::nullopt。
   * @param key 监控记录的完整主键。
   * @return 成功时返回结果；失败或未找到时返回 std::nullopt。
   */
  std::optional<MonData::StoredRecord>
  find(const MonData::MonitorKey &key) const;

  /**
   * @brief 返回匹配过滤条件的独立快照，按主键顺序排列。
   * @param filter 用于匹配主键字段的过滤条件。
   * @return 独立的结果快照。
   */
  std::vector<MonData::StoredRecord>
  query(const MonData::MonitorFilter &filter = {}) const;

  /**
   * @brief 返回当前记录数。
   * @return 当前保存的记录数。
   */
  std::size_t size() const;

  /** @brief 清空 Store 中的全部记录。 */
  void clear();

private:
  /**
   * @brief 执行普通更新或带持久化门禁的更新。
   * @param data 要写入的完整监控记录。
   * @param force 是否强制写入原本会被忽略的初始数值零。
   * @param timestamp 记录变化时使用的时间戳。
   * @param prepare 可选的提交前持久化回调；空指针表示普通更新。
   * @return 监控记录的写入状态及记录副本。
   */
  MonData::UpdateResult update_impl(MonData::MonitorData data, bool force,
                                    MonData::MonitorTimestamp timestamp,
                                    const PrepareUpdate *prepare);

  mutable std::mutex _mutex;
  std::map<MonData::MonitorKey, MonData::StoredRecord> _records;
};
} // namespace TLSSMON

#endif // __MONITOR_STORE_H__
