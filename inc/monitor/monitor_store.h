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
class MonitorStore final {
public:
  /*
   * prepare 返回 true：
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

  MonData::UpdateResult update(MonData::MonitorData data, bool force,
                               MonData::MonitorTimestamp timestamp);
  /*
   * 仅当候选记录实际发生变化时调用 prepare。
   *
   * prepare 成功后才提交候选记录。
   */
  MonData::UpdateResult update_prepared(MonData::MonitorData data, bool force,
                                        MonData::MonitorTimestamp timestamp,
                                        const PrepareUpdate &prepare);

  std::optional<MonData::StoredRecord>
  find(const MonData::MonitorKey &key) const;

  std::vector<MonData::StoredRecord>
  query(const MonData::MonitorFilter &filter = {}) const;

  std::size_t size() const;

  void clear();

private:
  MonData::UpdateResult update_impl(MonData::MonitorData data, bool force,
                                    MonData::MonitorTimestamp timestamp,
                                    const PrepareUpdate *prepare);

  mutable std::mutex _mutex;
  std::map<MonData::MonitorKey, MonData::StoredRecord> _records;
};
} // namespace TLSSMON

#endif // __MONITOR_STORE_H__
