#ifndef __MONITOR_REPORTER_H__
#define __MONITOR_REPORTER_H__

#include "monitor_data.h"

#include <mutex>
#include <cstdint>
#include <functional>
#include <string>

namespace TLSSMON{
class MonitorStore;

using MonitorPublisher = std::function<void(MonData::StoredRecord)>;

class MonitorReporter final {
public:
    using Publisher = MonitorPublisher;

    explicit MonitorReporter(MonitorStore& store);
    ~MonitorReporter() = default;

    MonitorReporter(const MonitorReporter&) = delete;
    MonitorReporter& operator=(const MonitorReporter&) = delete;

    void set_publisher(Publisher Publisher);

    MonData::UpdateResult update(MonData::MonitorData data, bool force, MonData::MonitorTimestamp timestamp);
    MonData::UpdateResult report_count(MonData::MonitorKey key, std::uint32_t value, std::string description, MonData::MonitorTimestamp timestamp);
    MonData::UpdateResult report_error(MonData::MonitorKey key, std::uint32_t value, std::string description, MonData::MonitorTimestamp timestamp);
    MonData::UpdateResult report_string(MonData::MonitorKey key, std::string value, std::string description, MonData::MonitorTimestamp timestamp);

private:
    MonitorStore& _store;

    std::mutex _publisher_mutex;
    Publisher _publisher{};
};
}

#endif // __MONITOR_REPORTER_H__
