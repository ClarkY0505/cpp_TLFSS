#ifndef __MONITOR_STORE_H__
#define __MONITOR_STORE_H__

#include "monitor_data.h"
#include <cstddef>
#include <map>
#include <mutex>
#include <vector>
#include <optional>

namespace TLSSMON{
class MonitorStore final{
public:
    MonitorStore() = default;
    ~MonitorStore() = default;

    MonitorStore(const MonitorStore&) = delete;
    MonitorStore& operator=(const MonitorStore&) = delete;

    MonitorStore(MonitorStore&&) = delete;
    MonitorStore& operator=(MonitorStore&&) = delete;

    MonData::UpdateResult update(MonData::MonitorData data, bool force, MonData::MonitorTimestamp timestamp);
    std::optional<MonData::StoredRecord> find(const MonData::MonitorKey& key) const;
    std::vector<MonData::StoredRecord> query(const MonData::MonitorFilter& filter = {}) const;
    
    std::size_t size() const;

    void clear();

private:
    mutable std::mutex _mutex;
    std::map<MonData::MonitorKey, MonData::StoredRecord> _records;

};
}


#endif // __MONITOR_STORE_H__
