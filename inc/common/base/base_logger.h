#ifndef __INC_COMMON_BASE_BASE_LOGGER_H__
#define __INC_COMMON_BASE_BASE_LOGGER_H__

#include "logger/logger.h"

namespace TLSS::BASE {
inline const TLSSLOG::Logger::LoggerPtr& mysql_logger() {
  // 第一次调用时获取，之后复用
  static const auto logger = TLSSLOG::Logger::get("mysql");
  return logger;
}
}  // namespace TLSS::BASE

#endif  // __INC_COMMON_BASE_BASE_LOGGER_H__
