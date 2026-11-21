#ifndef TLSS_FTP_STORAGE_RECOVERY_ENGINE_H
#define TLSS_FTP_STORAGE_RECOVERY_ENGINE_H

namespace TLSS::BASE {
class DbConnectionPool;
}
namespace TLSS::FileStorage {
class ObjectStorageEngine;
class ObjectCoordinator;
class ObjectGarbageCollector;

// Blocking startup orchestration. The caller holds StorageRootLock and has not
// started transfers or GC. Failure prevents opening the business listener.
class StorageRecoveryEngine {
 public:
  StorageRecoveryEngine(BASE::DbConnectionPool& pool, ObjectStorageEngine& objects,
                        ObjectCoordinator& coordinator, ObjectGarbageCollector& collector)
      : _pool(pool)
      , _objects(objects)
      , _coordinator(coordinator)
      , _collector(collector) {}
  bool prepare();

 private:
  BASE::DbConnectionPool& _pool;
  ObjectStorageEngine& _objects;
  ObjectCoordinator& _coordinator;
  ObjectGarbageCollector& _collector;
};
}  // namespace TLSS::FileStorage
#endif
