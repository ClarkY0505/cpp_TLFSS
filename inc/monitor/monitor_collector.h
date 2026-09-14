#ifndef __MONITOR_COLLECTOR_H__
#define __MONITOR_COLLECTOR_H__

#include "engine.h"
#include "monitor_wire.h"

namespace TLSSMON {

MonData::UpdateResult ingest_decoded_record(Engine &engine,
                                            Wire::DecodedRecord record,
                                            bool force = false);

} // namespace TLSSMON

#endif // __MONITOR_COLLECTOR_H__
