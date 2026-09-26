#include "monitor_data.h"
#include "monitor_store.h"

#include <cstddef>
#include <cstdio>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
namespace TLSSMON {
namespace {

/*
 * Store 提交阶段依赖无异常 swap。
 *
 * 如果以后 StoredRecord 新增了不能安全移动的成员，
 * 编译应直接失败，而不是在 prepare 成功后提交失败。
 */
static_assert(std::is_nothrow_swappable_v<MonData::StoredRecord>,
              "StoredRecord must be nothrow swappable");

static_assert(std::is_nothrow_move_constructible_v<MonData::UpdateResult>,
              "UpdateResult must be nothrow move constructible");
/*
 * - 数值只比较 NumericValue::_value；
 * - state、description、timestamp 不参与数值去重；
 * - 字符串比较完整内容；
 * - 数值和字符串类型不同，视为发生变化。
 */
bool same_monitor_value(const MonData::MonitorData &incoming,
                        const MonData::StoredRecord &stored) noexcept {
  const auto *incoming_numeric =
      std::get_if<MonData::NumericValue>(&incoming._value);

  if (incoming_numeric != nullptr) {
    const auto *stored_numeric =
        std::get_if<MonData::NumericValue>(&stored._data._value);

    return stored_numeric != nullptr &&
           stored_numeric->_value == incoming_numeric->_value;
  }

  const auto *incoming_string = std::get_if<std::string>(&incoming._value);
  const auto *stored_string = std::get_if<std::string>(&stored._data._value);

  return incoming_string != nullptr && stored_string != nullptr &&
         *incoming_string == *stored_string;
}

/*
 * 普通 update() 传入 nullptr，表示不需要提交前准备。
 *
 * update_prepared() 传入有效 std::function。
 * 空函数、返回 false 或抛出异常都视为持久化失败。
 */
bool run_prepare(const MonitorStore::PrepareUpdate *prepare,
                 const MonData::StoredRecord &candidate) noexcept {
  // prepare 是提交前屏障：异常与 false 等价，候选值不进入可见状态。
  if (prepare == nullptr) {
    return true;
  }

  if (!*prepare) {
    return false;
  }
  try {
    return (*prepare)(candidate);
  } catch (...) {
    /*
     * 不允许持久化异常穿透 Store，并确保候选记录不提交。
     */
    return false;
  }
}

} // namespace

std::optional<MonData::StoredRecord>
MonitorStore::find(const MonData::MonitorKey &key) const {
  std::lock_guard<std::mutex> lock(_mutex);
  const auto pos = _records.find(key);
  if (pos == _records.end()) {
    return std::nullopt;
  }

  return pos->second;
}

std::size_t MonitorStore::size() const {
  std::lock_guard<std::mutex> lock(_mutex);
  return _records.size();
}

void MonitorStore::clear() {
  std::lock_guard<std::mutex> lock(_mutex);
  _records.clear();
}

MonData::UpdateResult
MonitorStore::update_impl(MonData::MonitorData data, bool force,
                          MonData::MonitorTimestamp timestamp,
                          const PrepareUpdate *prepare) {
  if (data._value.valueless_by_exception()) {
    return MonData::UpdateResult{MonData::UpdateStatus::INVALID, std::nullopt};
  }

  const MonData::MonitorKey key = data._key;

  const auto *incoming_numeric =
      std::get_if<MonData::NumericValue>(&data._value);

  std::lock_guard<std::mutex> lock(_mutex);

  const auto existing = _records.find(key);

  /*
   * 第一种情况：Key 不存在。
   */
  if (existing == _records.end()) {
    /*
     * 首次数值零且 force == false：
     *
     * - 不插入；
     * - 不执行 prepare；
     * - 不创建空 Key 节点。
     */
    if (incoming_numeric != nullptr && incoming_numeric->_value == 0U &&
        !force) {
      return MonData::UpdateResult{MonData::UpdateStatus::IGNORED_INITIAL_ZERO,
                                   std::nullopt};
    }

    MonData::StoredRecord candidate{std::move(data), timestamp};

    /*
     * 在修改 map 之前先准备返回值副本。
     *
     * 这里可能发生 string/vector 类内存分配失败，
     * 但此时：
     *
     * - map 尚未变化；
     * - prepare 尚未执行；
     * - outbox 尚未写入。
     */
    MonData::UpdateResult committed_result{MonData::UpdateStatus::INSERTED,
                                           candidate};

    /*
     * 先让 map 完成节点分配。
     *
     * 插入期间如果 std::bad_alloc，prepare 尚未执行，
     * 因此不会发生“outbox 成功但 map 节点创建失败”。
     *
     * 当前线程仍持有 Store mutex，其他线程看不到这个
     * 尚未完成 prepare 的临时节点。
     */
    const auto inserted = _records.emplace(key, std::move(candidate));

    if (!inserted.second) {
      return MonData::UpdateResult{MonData::UpdateStatus::INVALID,
                                   std::nullopt};
    }

    /*
     * 节点内存已经准备完成，现在执行落盘。
     */
    if (!run_prepare(prepare, inserted.first->second)) {
      /*
       * 回滚临时节点。
       *
       * erase(iterator) 不需要重新分配内存。
       */
      _records.erase(inserted.first);

      return MonData::UpdateResult{MonData::UpdateStatus::DURABILITY_FAILED,
                                   std::nullopt};
    }

    /*
     * prepare 成功后不再执行可能分配内存的操作。
     */
    return committed_result;
  }

  /*
   * 第二种情况：Key 已存在且值没有变化。
   *
   * 不调用 prepare，不更新时间戳和元数据。
   */
  if (same_monitor_value(data, existing->second)) {
    return MonData::UpdateResult{MonData::UpdateStatus::UNCHANGED,
                                 existing->second};
  }

  /*
   * 第三种情况：已有 Key 的值发生变化。
   */
  MonData::StoredRecord candidate{std::move(data), timestamp};

  /*
   * 先准备返回副本，确保 prepare 成功后不再发生复制分配。
   */
  MonData::UpdateResult committed_result{MonData::UpdateStatus::UPDATED,
                                         candidate};

  /*
   * prepare 接收到完整候选记录：
   *
   * - 新数据；
   * - 新 description/state；
   * - 新 timestamp。
   *
   * 此时 Store 仍保存旧记录。
   */
  if (!run_prepare(prepare, candidate)) {
    return MonData::UpdateResult{MonData::UpdateStatus::DURABILITY_FAILED,
                                 std::nullopt};
  }

  /*
   * std::swap(StoredRecord) 已由 static_assert 保证不会抛异常。
   *
   * candidate 接收旧记录，existing->second 接收新记录。
   */
  using std::swap;
  swap(existing->second, candidate);

  return committed_result;
}

MonData::UpdateResult
MonitorStore::update(MonData::MonitorData data, bool force,
                     MonData::MonitorTimestamp timestamp) {
  return update_impl(std::move(data), force, timestamp, nullptr);
}

MonData::UpdateResult
MonitorStore::update_prepared(MonData::MonitorData data, bool force,
                              MonData::MonitorTimestamp timestamp,
                              const PrepareUpdate &prepare) {
  return update_impl(std::move(data), force, timestamp, &prepare);
}

std::vector<MonData::StoredRecord>
MonitorStore::query(const MonData::MonitorFilter &filter) const {

  std::vector<MonData::StoredRecord> result;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    // 空过滤器最多返回全部记录，提前分配容量，
    // 避免复制过程中多次扩容。
    result.reserve(_records.size());

    // _records 是 std::map，遍历顺序由
    // MonitorKey::operator< 保证：
    //
    // mid → level → fid → eid
    for (const auto &entry : _records) {
      const MonData::MonitorKey &key = entry.first;
      if (filter.module_id.has_value() &&
          key._mid != filter.module_id.value()) {
        continue;
      }

      if (filter.level.has_value() && key._level != filter.level.value()) {
        continue;
      }

      if (filter.function_id.has_value() &&
          key._fid != filter.function_id.value()) {
        continue;
      }

      if (filter.event_id.has_value() && key._eid != filter.event_id.value()) {
        continue;
      }

      // entry.first  是 MonitorKey；
      // entry.second 是 StoredRecord。
      // push_back 执行值复制，不保存内部引用。
      result.push_back(entry.second);
    }
  }

  return result;
}
} // namespace TLSSMON
