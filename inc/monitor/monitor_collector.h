#ifndef __MONITOR_COLLECTOR_H__
#define __MONITOR_COLLECTOR_H__

#include "engine.h"
#include "monitor_wire.h"

namespace TLSSMON {

/**
 * @brief 将解码后的 UDP 记录写入 Engine。
 * @param engine 接收记录的 Engine，须处于允许写入的阶段。
 * @param record V2 记录保留线协议变化时间；V1 使用 Engine 当前时间。
 * @param force 传递给 Engine 的强制更新标志。
 * @return Engine 的写入结果。
 */
MonData::UpdateResult ingest_decoded_record(Engine &engine,
                                            Wire::DecodedRecord record,
                                            bool force = false);

} // namespace TLSSMON

#endif // __MONITOR_COLLECTOR_H__
