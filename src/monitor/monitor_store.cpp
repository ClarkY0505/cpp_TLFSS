#include "monitor_store.h"
#include "monitor_data.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace TLSSMON{
std::optional<MonData::StoredRecord> MonitorStore::find(const MonData::MonitorKey& key) const{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto pos = _records.find(key);
    if(pos == _records.end()){
        return std::nullopt;
    }

    return pos->second;
}

std::size_t MonitorStore::size() const{
    std::lock_guard<std::mutex> lock(_mutex);
    return _records.size();
}

void MonitorStore::clear(){
    std::lock_guard<std::mutex> lock(_mutex);
    _records.clear();
}

MonData::UpdateResult MonitorStore::update(MonData::MonitorData data, bool force, MonData::MonitorTimestamp timestamp){


    const MonData::MonitorKey key = data._key;
    const MonData::NumericValue* const incoming_numeric = std::get_if<MonData::NumericValue>(&data._value);

    std::lock_guard<std::mutex> lock(_mutex);
    const auto existing = _records.find(key);
    /*
     * 必须先判断记录是否存在。
     *
     * 首次零值门禁只针对不存在的 Key。
     * 不能在这里提前忽略。
     */
    if(existing == _records.end()){
        /*
         * 新 Key + 数值零 + 未强制：
         *
         * 不创建 map 节点；
         * 不保存时间戳；
         * 不返回 StoredRecord。
         */
        if(incoming_numeric != nullptr && incoming_numeric->_value == 0 && !force){
            return MonData::UpdateResult{MonData::UpdateStatus::IGNORED_INITIAL_ZERO, std::nullopt};
        }

        /*
         * 以下情况允许首次插入：
         *
         * 1. 非零数值；
         * 2. force=true 的零值；
         * 3. 字符串值。
         */
        MonData::StoredRecord record{std::move(data), timestamp};

        // auto is pair<iterator,bool>, 
        // iterator is iterator of _record that point to insert current element 
        const auto inserted = _records.emplace(key, std::move(record));
        if(!inserted.second){
            return MonData::UpdateResult{MonData::UpdateStatus::INVALID, std::nullopt};
        }
        return MonData::UpdateResult{MonData::UpdateStatus::INSERTED, inserted.first->second};
    }

    /*
     * 已存在记录的去重判断。
     *
     * 数值和字符串分别比较，不能直接比较整个 variant：
     *
     * 数值只比较 NumericValue::_value；
     * 字符串比较完整 std::string 内容。
     */
    if (incoming_numeric != nullptr) {

        // 本次提交的是数值。
        const MonData::NumericValue* const stored_numeric =
            std::get_if<MonData::NumericValue>(
                                               &existing->second._data._value);

        // 原记录也是数值，并且数值主体相同。
        //
        // 不比较 state 和 description；
        // 不覆盖原记录和 changed_at。
        if (stored_numeric != nullptr
            && stored_numeric->_value
            == incoming_numeric->_value) {

            return MonData::UpdateResult{
                MonData::UpdateStatus::UNCHANGED,
                    existing->second
            };
        }

    } else {

        // 本次提交的是字符串。
        const std::string* const incoming_string =
            std::get_if<std::string>(
                                     &data._value);

        // 尝试取得原记录中的字符串。
        const std::string* const stored_string =
            std::get_if<std::string>(
                                     &existing->second._data._value);

        // 新旧记录都是字符串，并且完整内容相同。
        //
        // 不覆盖 description；
        // 不更新 changed_at；
        // force 不影响这里的比较结果。
        if (incoming_string != nullptr
            && stored_string != nullptr
            && *stored_string == *incoming_string) {

            return MonData::UpdateResult{
                MonData::UpdateStatus::UNCHANGED,
                    existing->second
            };
        }
    }

    /*
     * 执行到这里表示值发生了变化：
     *
     * 1. 数值内容不同；
     * 2. 字符串内容不同；
     * 3. 原记录和新记录的值类型不同。
     *
     * 整体替换数据，并更新时间。
     */
    existing->second = MonData::StoredRecord{std::move(data), timestamp};

    return MonData::UpdateResult{MonData::UpdateStatus::UPDATED, existing->second};
}

std::vector<MonData::StoredRecord> MonitorStore::query(const MonData::MonitorFilter& filter) const{

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
          for (const auto& entry : _records) {
              const MonData::MonitorKey& key = entry.first;
              if(filter.module_id.has_value() && key._mid != filter.module_id.value()){
                  continue;
              }

              if(filter.level.has_value() && key._level != filter.level.value()){
                  continue;
              }

              if (filter.function_id.has_value()
                  && key._fid != filter.function_id.value()) {
                  continue;
              }

              if (filter.event_id.has_value()
                  && key._eid != filter.event_id.value()) {
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
}
