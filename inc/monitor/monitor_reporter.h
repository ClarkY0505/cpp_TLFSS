#ifndef __MONITOR_REPORTER_H__
#define __MONITOR_REPORTER_H__

#include "alarm_publisher_types.h"
#include "monitor_data.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace TLSSMON {
class MonitorModuleRegistry;
class MonitorStore;

using MonitorPublisher = std::function<void(MonData::StoredRecord)>;

class MonitorReporter final {
public:
  using Publisher = MonitorPublisher;

  /*
   * 单独使用Reporter 时不绑定模块注册表
   * 现主要用于Reporter部分功能测试的接口
   * */
  explicit MonitorReporter(MonitorStore &store);
  MonitorReporter(MonitorStore &store, const MonitorModuleRegistry &modules);
  ~MonitorReporter() = default;

  MonitorReporter(const MonitorReporter &) = delete;
  MonitorReporter &operator=(const MonitorReporter &) = delete;

  void set_publisher(Publisher Publisher);

  MonData::UpdateResult update(MonData::MonitorData data, bool force,
                               MonData::MonitorTimestamp timestamp);
  MonData::UpdateResult report_count(MonData::MonitorKey key,
                                     std::uint32_t value,
                                     std::string description,
                                     MonData::MonitorTimestamp timestamp);
  MonData::UpdateResult report_error(MonData::MonitorKey key,
                                     std::uint32_t value,
                                     std::string description,
                                     MonData::MonitorTimestamp timestamp);
  MonData::UpdateResult report_string(MonData::MonitorKey key,
                                      std::string value,
                                      std::string description,
                                      MonData::MonitorTimestamp timestamp);

  /*
   * 注册或注销可靠告警 Publisher。
   *
   * 空 std::function 表示注销。
   */
  void set_alarm_publisher(AlarmPublisher publisher);

private:
  /*
   * 根据 key._mid 和 key._eid 查询模块注册表。
   *
   * 找到元数据时：
   *
   * - Registry 中的 level 始终覆盖 key._level。
   * - Registry description 非空时覆盖调用者 description。
   *
   * 找不到元数据时保持调用者数据不变。
   */
  void enrich(MonData::MonitorKey &key, std::string &description) const;

  MonitorStore &_store;

  const MonitorModuleRegistry *_modules{nullptr};
  std::mutex _publisher_mutex;
  Publisher _publisher{};
  AlarmPublisher _alarm_publisher{};
};
} // namespace TLSSMON

#endif // __MONITOR_REPORTER_H__
