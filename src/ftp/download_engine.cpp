#include "ftp/download_engine.h"

#include "ftp/metadata_service.h"

#include <stdexcept>
#include <utility>

namespace TLSS::FileStorage {
DownloadObject DownloadEngine::open(const StorageRequestContext& context,
                                    const std::string& name) const {
  if (!context.valid())
    throw std::runtime_error("invalid storage request context");
  auto result = _metadata.find_download_hash(context.username, context.directory, name);
  if (!result.success)
    throw std::runtime_error("file not found or unavailable");
  return {_objects.open_read(result.result), std::move(result.result)};
}
}  // namespace TLSS::FileStorage
