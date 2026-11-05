#ifndef TLSS_FTP_TRANSFER_PROTOCOL_H
#define TLSS_FTP_TRANSFER_PROTOCOL_H
#include "common/net/tcp_connection.h"
#include "ftp/transfer_validation.h"

namespace TLSS::FileStorage::Transfer {
inline constexpr const char* kResponsePrefix = "\n@TLSS ";
inline constexpr std::size_t kMaxControlBytes = 4096;
inline void send_control(const NET::TcpConnectionPtr& conn, const std::string& payload) {
  conn->send(std::string(kResponsePrefix) + payload + "\n");
}
}  // namespace TLSS::FileStorage::Transfer
#endif
