#include "monitor_error.h"

namespace TLSSMON {

std::string_view monitor_level_name(std::uint32_t level) noexcept {
  switch (level) {
  case static_cast<std::uint32_t>(MonitorLevel::INFO):
    return "info";

  case static_cast<std::uint32_t>(MonitorLevel::WARN):
    return "warn";

  case static_cast<std::uint32_t>(MonitorLevel::SOFT_STOP):
    return "sstop";

  case static_cast<std::uint32_t>(MonitorLevel::EMERGENCY_STOP):
    return "estop";

  default:
    return "?";
  }
}

} // namespace TLSSMON
