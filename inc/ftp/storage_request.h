#ifndef TLSS_FTP_STORAGE_REQUEST_H
#define TLSS_FTP_STORAGE_REQUEST_H

#include <cstdint>
#include <string>

namespace TLSS::FileStorage {
// The caller takes this identity snapshot after successful authentication.
struct StorageRequestContext {
  std::uint64_t user_id = 0;
  std::string username;
  std::string directory = "/";

  bool valid() const;
};
}  // namespace TLSS::FileStorage

#endif  // TLSS_FTP_STORAGE_REQUEST_H
