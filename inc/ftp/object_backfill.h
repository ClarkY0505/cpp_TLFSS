#ifndef __INC_FTP_OBJECT_BACKFILL_H__
#define __INC_FTP_OBJECT_BACKFILL_H__

#include "ftp/object_coordinator.h"
#include "ftp/object_repository.h"
#include "ftp/object_storage_engine.h"
namespace TLSS::FileStorage {
class ObjectBackfill {
 public:
  ObjectBackfill(ObjectRepository& repository, ObjectStorageEngine& objects,
                 ObjectCoordinator& coordinator);

  bool register_if_absent(const std::string& hash);
  bool register_referenced_objects();

 private:
  ObjectRepository& _repository;
  ObjectStorageEngine& _objects;
  ObjectCoordinator& _coordinator;
};
}  // namespace TLSS::FileStorage
#endif  // __INC_FTP_OBJECT_BACKFILL_H__
