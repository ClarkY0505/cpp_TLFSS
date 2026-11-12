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
  // true：所有磁盘候选均已登记或原本已有记录，空清单也成功。
  // false：某个对象查询或登记失败；之前成功登记的记录保留。
  // 扫描及文件系统异常继续向上传播。
  bool register_disk_objects();

 private:
  ObjectRepository& _repository;
  ObjectStorageEngine& _objects;
  ObjectCoordinator& _coordinator;
};
}  // namespace TLSS::FileStorage
#endif  // __INC_FTP_OBJECT_BACKFILL_H__
