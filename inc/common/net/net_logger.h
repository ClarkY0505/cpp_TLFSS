#ifndef __INC_COMMON_NET_NET_LOGGER_H__
#define __INC_COMMON_NET_NET_LOGGER_H__

#include "logger/logger.h"

namespace TLSS::NET {
inline const TLSSLOG::Logger::LoggerPtr& net_logger() {
  // 第一次调用时获取，之后复用
  static const auto logger = TLSSLOG::Logger::get("net");
  return logger;
}

}  // namespace TLSS::NET
#endif  // __INC_COMMON_NET_NET_LOGGER_H__
