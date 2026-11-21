#include "ftp/storage_recovery_engine.h"

#include "common/base/mysql_connection_pool.h"
#include "ftp/object_backfill.h"
#include "ftp/object_garbage_collector.h"
#include "ftp/object_repository.h"

#include <chrono>

namespace TLSS::FileStorage {
bool StorageRecoveryEngine::prepare() {
  _objects.cleanup_temporary_files();
  {
    auto connection = _pool.get_connection();
    if (!connection)
      return false;
    ObjectRepository repository(*connection);
    if (!repository.check_object_table())
      return false;
    ObjectBackfill backfill(repository, _objects, _coordinator);
    if (!backfill.register_referenced_objects() || !backfill.register_disk_objects())
      return false;
  }
  // Return the listing connection before recovery borrows its own connections.
  return _collector.recover_deleting(std::chrono::minutes{10});
}
}  // namespace TLSS::FileStorage
