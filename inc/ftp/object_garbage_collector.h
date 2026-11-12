#ifndef __INC_FTP_OBJECT_GARBAGE_COLLECTOR_H__
#define __INC_FTP_OBJECT_GARBAGE_COLLECTOR_H__

#include <chrono>
#include <string>

#include "common/base/mysql_connection_pool.h"
#include "ftp/object_coordinator.h"
#include "ftp/object_storage_engine.h"

namespace TLSS::FileStorage {

enum class ObjectGcCollectResult {
  skipped,         // 本轮无需回收
  collected,       // 磁盘删除、目录同步及数据库收尾均已完成
  failed,          // 校验、连接获取或数据库操作失败
  outcome_unknown  // 事务提交结果未知，需要后续重新确认
};

class ObjectGarbageCollector {
 public:
  ObjectGarbageCollector(BASE::DbConnectionPool& pool, ObjectStorageEngine& objects,
                         ObjectCoordinator& coordinator);

  // 同步阻塞操作，应在后台工作线程执行
  // 内部获取哈希锁，覆盖准备、磁盘删除和数据库收尾
  // 异常继续向上传播
  ObjectGcCollectResult collect(const std::string& hash, std::chrono::seconds grace_period);

  // 启动前调用；查询连接归还后，再逐个恢复对象
  // true：清单为空，或所有候选都已回收/无需处理
  // false：参数、连接、查询、清单校验失败，或某个对象处理失败/提交结果未知
  // 异常继续向上传播；中途失败不撤销之前已完成的回收
  bool recover_deleting(std::chrono::seconds grace_period);

  // 每次最多处理一批，after_hash 由调用线程独占维护。
  // true：本批处理成功，或到达表尾并已将游标清空。
  // false：校验、连接、查询失败，或某个对象处理失败/提交结果未知。
  // 游标保留最后尝试的位置；部分成功结果不回滚。
  // 异常继续向上传播，异常发生时游标也可能已经前移。
  bool collect_batch(std::string& after_hash, std::chrono::seconds grace_period);

 private:
  BASE::DbConnectionPool& _pool;
  ObjectStorageEngine& _objects;
  ObjectCoordinator& _coordinator;
};
}  // namespace TLSS::FileStorage
#endif  // __INC_FTP_OBJECT_GARBAGE_COLLECTOR_H__
