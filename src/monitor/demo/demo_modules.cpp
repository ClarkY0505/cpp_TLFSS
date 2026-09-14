#include "demo_modules.h"

namespace TLSSMON::DemoM9 {

const MonitorModuleInfo &disk_module() {
  static const MonitorModuleInfo module{
      DISK_MODULE_ID,
      "disk",
      "storage disk service",
      {
          {MonitorLevel::INFO, "disk temperature"},
          {MonitorLevel::INFO, "disk used percent"},
          {MonitorLevel::WARN, "disk almost full"},
      }};

  return module;
}

const MonitorModuleInfo &network_module() {
  static const MonitorModuleInfo module{
      NETWORK_MODULE_ID,
      "network",
      "storage network service",
      {
          {MonitorLevel::INFO, "receive error count"},
          {MonitorLevel::INFO, "network link status"},
          {MonitorLevel::WARN, "receive error burst"},
      }};

  return module;
}

} // namespace TLSSMON::DemoM9
