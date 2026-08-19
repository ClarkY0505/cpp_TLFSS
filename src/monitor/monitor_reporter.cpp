#include "monitor_reporter.h"
#include "monitor_data.h"
#include "monitor_store.h"

#include <algorithm>
#include <ctime>
#include <map>
#include <mutex>
#include <utility>
namespace TLSSMON {
MonitorReporter::MonitorReporter(MonitorStore &store) : _store(store) {}

void MonitorReporter::set_publisher(Publisher publisher){
    std::lock_guard<std::mutex> lock(_publisher_mutex);
    _publisher = std::move(publisher);
}

MonData::UpdateResult MonitorReporter::update(MonData::MonitorData data, bool force, MonData::MonitorTimestamp timestamp){
    MonData::UpdateResult result = _store.update(std::move(data), force, timestamp);

    const bool should_publish = result._status == MonData::UpdateStatus::INSERTED || result._status == MonData::UpdateStatus::UPDATED;

    if(!should_publish || !result._record.has_value()){
        return result;
    }

    //  publisher is std::function<void(MonData::StoredRecord)>
    Publisher publisher;

    {
        std::lock_guard<std::mutex> lock(_publisher_mutex);
        publisher = _publisher;
    }

    if(publisher){
        try {
          publisher(*result._record);
      } catch (...) {
          /*
           * Publisher 理论上应自行处理异常。
           * 此处仅隔离遗漏的异常，不重复记录日志。
           * TODO Log
           */
      }

    }

    return result;
}

MonData::UpdateResult MonitorReporter::report_count(MonData::MonitorKey key, std::uint32_t value, std::string description, MonData::MonitorTimestamp timestamp){
    MonData::MonitorData data{std::move(key), std::move(description), MonData::NumericValue{value, 0}};
    return update(std::move(data), false, timestamp);
}

MonData::UpdateResult MonitorReporter::report_error(MonData::MonitorKey key, std::uint32_t value, std::string description, MonData::MonitorTimestamp timestamp){
    MonData::MonitorData data{std::move(key), std::move(description), MonData::NumericValue{value, 2}};
    return update(std::move(data), true, timestamp);
}

MonData::UpdateResult MonitorReporter::report_string(MonData::MonitorKey key, std::string value, std::string description, MonData::MonitorTimestamp timestamp){
    MonData::MonitorData data{std::move(key), std::move(description), std::move(value)};
    return update(std::move(data), false, timestamp);
}

} // namespace TLSSMON
