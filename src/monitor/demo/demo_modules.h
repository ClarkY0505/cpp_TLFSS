#ifndef __DEMO_MODULES_H__
#define __DEMO_MODULES_H__

#include "monitor_module.h"

#include <cstdint>

namespace TLSSMON::DemoM9 {

inline constexpr std::uint32_t DISK_MODULE_ID = 0x300U;
inline constexpr std::uint32_t NETWORK_MODULE_ID = 0x400U;

inline constexpr std::uint32_t SAMPLE_FUNCTION_ID = 1U;

/*
 * vector 下标就是 eid，因此这些值必须从 0 连续排列。
 */
enum DiskEventId : std::uint32_t {
  DISK_TEMPERATURE = 0U,
  DISK_USED_PERCENT = 1U,
  DISK_ALMOST_FULL = 2U
};

enum NetworkEventId : std::uint32_t {
  NETWORK_RX_ERROR_COUNT = 0U,
  NETWORK_LINK_STATUS = 1U,
  NETWORK_RX_ERROR_BURST = 2U
};

const MonitorModuleInfo &disk_module();
const MonitorModuleInfo &network_module();

} // namespace TLSSMON::DemoM9

#endif // __DEMO_MODULES_H__
