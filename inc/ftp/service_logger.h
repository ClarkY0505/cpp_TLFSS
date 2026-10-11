#ifndef __INC_FTP_SERVICE_LOGGER_H__
#define __INC_FTP_SERVICE_LOGGER_H__

#include "logger/logger.h"
namespace TLSS::FileStorage {
inline const TLSSLOG::Logger::LoggerPtr& service_logger() {
  static const auto logger = TLSSLOG::Logger::get("storage_service");
  return logger;
}
}       // namespace TLSS::FileStorage
#endif  // __INC_FTP_SERCVICE_LOGGER_H__
