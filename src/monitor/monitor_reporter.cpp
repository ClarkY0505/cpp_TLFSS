#include "monitor_data.h"
#include "monitor_module_registry.h"
#include "monitor_reporter.h"
#include "monitor_store.h"

#include <cstdint>
#include <ctime>
#include <mutex>
#include <optional>
#include <utility>
namespace TLSSMON {
MonitorReporter::MonitorReporter(MonitorStore &store)
    : _store(store), _modules(nullptr) {}
MonitorReporter::MonitorReporter(MonitorStore &store,
                                 const MonitorModuleRegistry &modules)
    : _store(store), _modules(&modules) {}

void MonitorReporter::set_publisher(Publisher publisher) {
  std::lock_guard<std::mutex> lock(_publisher_mutex);
  _publisher = std::move(publisher);
}

MonData::UpdateResult
MonitorReporter::update(MonData::MonitorData data, bool force,
                        MonData::MonitorTimestamp timestamp) {
  MonData::UpdateResult result =
      _store.update(std::move(data), force, timestamp);

  const bool should_publish =
      result._status == MonData::UpdateStatus::INSERTED ||
      result._status == MonData::UpdateStatus::UPDATED;

  if (!should_publish || !result._record.has_value()) {
    return result;
  }

  // 复制发布回调，避免执行用户代码期间持有发布器锁。
  Publisher publisher;

  {
    std::lock_guard<std::mutex> lock(_publisher_mutex);
    publisher = _publisher;
  }

  // Publisher 在 Store 解锁后执行，允许回调重新查询或上报数据。
  if (publisher) {
    try {
      publisher(*result._record);
    } catch (...) {
      /*
       * Publisher 理论上应自行处理异常。
       * 此处仅隔离遗漏的异常，不重复记录日志。
       * TODO：记录发布回调异常。
       */
    }
  }

  return result;
}

MonData::UpdateResult
MonitorReporter::report_count(MonData::MonitorKey key, std::uint32_t value,
                              std::string description,
                              MonData::MonitorTimestamp timestamp) {
  enrich(key, description);
  MonData::MonitorData data{std::move(key), std::move(description),
                            MonData::NumericValue{value, 0U}};
  return update(std::move(data), false, timestamp);
}

MonData::UpdateResult
MonitorReporter::report_error(MonData::MonitorKey key, std::uint32_t value,
                              std::string description,
                              MonData::MonitorTimestamp timestamp) {
  enrich(key, description);
  MonData::MonitorData data{std::move(key), std::move(description),
                            MonData::NumericValue{value, 2U}};
  /*
   * 先在 Publisher mutex 下复制快照。
   *
   * 离开作用域后立即释放锁，后续不能持锁进入 Store，
   * 更不能持锁执行用户提供的 AlarmPublisher。
   */
  AlarmPublisher alarm_publisher;

  try {
    std::lock_guard<std::mutex> lock(_publisher_mutex);
    alarm_publisher = _alarm_publisher;
  } catch (...) {
    /*
     * std::function 目标复制失败。
     *
     * 此时 Store 尚未更新，返回持久化失败允许相同值重试。
     */
    return {MonData::UpdateStatus::DURABILITY_FAILED, std::nullopt};
  }

  /*
   * 没有可靠告警 Publisher 时保持行为：
   *
   * Store 更新成功后，由普通 Publisher/UDP 发布。
   */
  if (!alarm_publisher) {
    return update(std::move(data), true, timestamp);
  }

  /*
   * 构造提交前持久化操作。
   *
   * 捕获引用是安全的，因为 update_prepared() 是同步调用，
   * prepare 不会在函数返回后保留。
   */
  MonitorStore::PrepareUpdate prepare;

  try {
    prepare =
        [&alarm_publisher](const MonData::StoredRecord &candidate) -> bool {
      /*
       * AlarmPublisher 参数是 StoredRecord 按值传递，
       * 这里会生成 candidate 的副本。
       *
       * Publisher 抛出的异常由 MonitorStore::run_prepare()
       * 捕获并转换成 DURABILITY_FAILED。
       */
      const AlarmEnqueueResult result = alarm_publisher(candidate);

      return result.durable();
    };
  } catch (...) {
    return {MonData::UpdateStatus::DURABILITY_FAILED, std::nullopt};
  }

  /*
   * 可靠通道只执行：
   *
   * Store::update_prepared() → AlarmPublisher/outbox → Store commit
   *
   * 不调用 update()，因此不会进入普通 UDP Publisher。
   */
  return _store.update_prepared(std::move(data), true, timestamp, prepare);
}

MonData::UpdateResult
MonitorReporter::report_string(MonData::MonitorKey key, std::string value,
                               std::string description,
                               MonData::MonitorTimestamp timestamp) {
  enrich(key, description);
  MonData::MonitorData data{std::move(key), std::move(description),
                            std::move(value)};
  return update(std::move(data), false, timestamp);
}

void MonitorReporter::set_alarm_publisher(AlarmPublisher publisher) {

  std::lock_guard<std::mutex> lock(_publisher_mutex);
  _alarm_publisher = std::move(publisher);
}

void MonitorReporter::enrich(MonData::MonitorKey &key,
                             std::string &description) const {
  /*
   * 兼容之前版本的一些测试方案，
   * 可能在个别情况会使用到单独创建Reporter,
   * 不注册模块
   * */
  if (_modules == nullptr) {
    return;
  }

  const std::optional<MonitorErrorInfo> error =
      _modules->find_error(key._mid, key._eid);

  if (!error.has_value()) {
    return;
  }

  key._level = static_cast<std::uint32_t>(error->_level);
  if (!error->_description.empty()) {
    description = error->_description;
  }
}

} // namespace TLSSMON
