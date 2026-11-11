#ifndef __INC_FTP_OBJECT_GC_TRANSACTION_H__
#define __INC_FTP_OBJECT_GC_TRANSACTION_H__

#include <chrono>
#include <string>

#include "common/base/mysql_connection.h"

namespace TLSS::FileStorage {

enum class ObjectGcPrepareResult {
  skipped,          // 本轮无需删除；需要保存的计时变更已提交
  ready_to_delete,  // 已确认无引用，且 DELETING 状态已提交
  failed,           // 校验、查询或执行失败，禁止删除磁盘对象
  outcome_unknown   // 提交结果未知，禁止删除，等待后续重新确认
};

// 调用者须持有对应哈希锁。
// connection 是调用者借用的连接，调用前不能已有活动事务。
// 本函数负责开启、提交事务，只操作数据库，不操作磁盘。
// 异常继续向上传播。
// 若返回 ready_to_delete，调用者须继续持锁，覆盖后续磁盘删除及数据库收尾。
ObjectGcPrepareResult prepare_object_gc_transaction(BASE::DbConnection& connection,
                                                    const std::string& hash,
                                                    std::chrono::seconds grace_period);

enum class ObjectGcFinalizeResult {
  completed,       // 对象记录已不存在，或本次删除已提交
  failed,          // 校验、查询或删除失败
  outcome_unknown  // 提交结果未知，后续重新确认
};

// 调用者须持续持有对应哈希锁，且磁盘删除、目录同步已正常完成。
// connection 调用前不能已有活动事务；异常继续向上传播。
ObjectGcFinalizeResult finalize_object_gc_transaction(BASE::DbConnection& connection,
                                                      const std::string& hash);
}  // namespace TLSS::FileStorage
#endif  // __INC_FTP_OBJECT_GC_TRANSACTION_H__
