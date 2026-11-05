#ifndef __INC_FTP_SERVICE_ENGINE_H__
#define __INC_FTP_SERVICE_ENGINE_H__
#include "ftp/metadata_service.h"
namespace TLSS::FileStorage {
// Preserve source compatibility for existing users and cache regressions.
using StorageEngine = MetadataService;
}  // namespace TLSS::FileStorage
#endif
