#ifndef TLSS_FTP_STORAGE_REQUEST_H
#define TLSS_FTP_STORAGE_REQUEST_H

#include "auth/authenticated_principal.h"

#include <string>

namespace TLSS::AUTH {
class AuthService;
}

namespace TLSS::FileStorage {
// Identity is issued by authentication; callers only choose the request directory.
struct StorageRequestContext {
  AUTH::PrincipalHandle principal;
  std::string directory = "/";

  bool valid(const AUTH::AuthService& authority) const;
};
}  // namespace TLSS::FileStorage

#endif  // TLSS_FTP_STORAGE_REQUEST_H
