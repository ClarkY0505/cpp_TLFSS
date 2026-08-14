#include "monitor_store.h"
#include "monitor_data.h"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <utility>

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
    
    // un-used, now
    (void)force;
    const MonData::MonitorKey key = data._key;
    
    std::lock_guard<std::mutex> lock(_mutex);
    const auto existing = _records.find(key);
    if(existing != _records.end()){
        return MonData::UpdateResult{MonData::UpdateStatus::UNCHANGED, existing->second};
    }

    MonData::StoredRecord record{std::move(data), timestamp};

    // auto is pair<iterator,bool>, 
    // iterator is iterator of _record that point to insert current element 
    const auto result = _records.emplace(key, std::move(record));
    if(!result.second){
        return MonData::UpdateResult{MonData::UpdateStatus::INVALID, std::nullopt};
    }

    return MonData::UpdateResult{MonData::UpdateStatus::INSERTED, result.first->second};
    
    
}
}
