#ifndef __INC_FTP_TRANSFER_LOGGER_H__
#define __INC_FTP_TRANSFER_LOGGER_H__

#include "logger/logger.h"
namespace TLSS::FileStorage::Transfer {
inline const TLSSLOG::Logger::LoggerPtr& transfer_logger() {
  // 第一次调用时获取，之后复用
  static const auto logger = TLSSLOG::Logger::get("transfer");
  return logger;
}
}  // namespace TLSS::FileStorage::Transfer

#endif  // __INC_FTP_TRANSFER_LOGGER_H__
