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

/** @brief 监控记录发布回调；参数是 Store 记录的独立副本。 */
using MonitorPublisher = std::function<void(MonData::StoredRecord)>;

/**
 * @brief 将采样值写入 Store，并按需调用监控和可靠告警发布器。
 * @note 发布回调在 Store 解锁后调用，可以重新进入 Engine。
 */
class MonitorReporter final {
public:
  using Publisher = MonitorPublisher;

  /**
   * @brief 单独使用Reporter 时不绑定模块注册表
   * 现主要用于Reporter部分功能测试的接口
   * @param store 用于保存监控记录的 Store；由调用方持有。
   * */
  explicit MonitorReporter(MonitorStore &store);
  /**
   * @brief 绑定 Store 与用于补齐监控元数据的模块注册表。
   * @param store 用于保存监控记录的 Store；由调用方持有。
   * @param modules 用于查询或补齐模块元数据的注册表；由调用方持有。
   */
  MonitorReporter(MonitorStore &store, const MonitorModuleRegistry &modules);
  ~MonitorReporter() = default;

  MonitorReporter(const MonitorReporter &) = delete;
  MonitorReporter &operator=(const MonitorReporter &) = delete;

  /**
   * @brief 设置监控数据发布回调；空回调表示注销。
   * @param Publisher 要注册的监控发布回调；空回调表示注销。
   */
  void set_publisher(Publisher Publisher);

  /**
   * @brief 按 Store 更新规则提交完整记录。
   * @param data 要写入的完整监控记录。
   * @param force 控制 Store 对初始零值等情况的处理。
   * @param timestamp 记录值发生变化时使用的时间。
   * @return 写入状态及可用的记录副本。
   */
  MonData::UpdateResult update(MonData::MonitorData data, bool force,
                               MonData::MonitorTimestamp timestamp);
  /**
   * @brief 上报计数值，成功更改时通知监控发布器。
   * @param key 监控记录的完整主键。
   * @param value 要上报的数值。
   * @param description 监控记录的可读描述。
   * @param timestamp 记录变化时使用的时间戳。
   * @return 监控记录的写入状态及记录副本。
   */
  MonData::UpdateResult report_count(MonData::MonitorKey key,
                                     std::uint32_t value,
                                     std::string description,
                                     MonData::MonitorTimestamp timestamp);
  /**
   * @brief 上报错误值，并按模块元数据补齐级别和描述。
   * @note 配置可靠告警发布器时，必须先完成持久化才能更新 Store；
   *       此路径不再调用普通监控发布器。
   * @param key 监控记录的完整主键。
   * @param value 要上报的数值。
   * @param description 监控记录的可读描述。
   * @param timestamp 记录变化时使用的时间戳。
   * @return 监控记录的写入状态及记录副本。
   */
  MonData::UpdateResult report_error(MonData::MonitorKey key,
                                     std::uint32_t value,
                                     std::string description,
                                     MonData::MonitorTimestamp timestamp);
  /**
   * @brief 上报字符串值，成功更改时通知监控发布器。
   * @param key 监控记录的完整主键。
   * @param value 要上报的字符串值。
   * @param description 监控记录的可读描述。
   * @param timestamp 记录变化时使用的时间戳。
   * @return 监控记录的写入状态及记录副本。
   */
  MonData::UpdateResult report_string(MonData::MonitorKey key,
                                      std::string value,
                                      std::string description,
                                      MonData::MonitorTimestamp timestamp);

  /**
   * @brief 注册或注销可靠告警 Publisher。
   *
   * 空 std::function 表示注销。
   * @param publisher 可靠告警入队回调；空回调表示注销。
   */
  void set_alarm_publisher(AlarmPublisher publisher);

private:
  /**
   * @brief 根据 key._mid 和 key._eid 查询模块注册表。
   *
   * 找到元数据时：
   *
   * - Registry 中的 level 始终覆盖 key._level。
   * - Registry description 非空时覆盖调用者 description。
   *
   * 找不到元数据时保持调用者数据不变。
   * @param key 监控记录的完整主键。
   * @param description 监控记录的可读描述。
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
