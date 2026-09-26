#include "monitor_collector.h"

#include <optional>
#include <utility>

namespace TLSSMON {

MonData::UpdateResult
ingest_decoded_record(Engine &engine, Wire::DecodedRecord record, bool force) {
  // 两版报文共用 Store 入口；仅 V2 能保留生产端的变化时间。
  const std::optional<MonData::MonitorTimestamp> changed_at =
      record._changed_at;

  if (changed_at.has_value()) {
    /*
     * V2 数据报：
     * 使用生产端携带的 changed_at。
     */
    return engine.update_data_at(std::move(record._data), *changed_at, force);
  }

  /*
   * V1 数据报：
   * 没有生产端时间戳，使用 Collector 当前时间。
   */
  return engine.update_data(std::move(record._data), force);
}

} // namespace TLSSMON
