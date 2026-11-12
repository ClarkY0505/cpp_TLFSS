#ifndef __INC_FTP_OBJECT_REPOSITORY_H__
#define __INC_FTP_OBJECT_REPOSITORY_H__

#include "common/base/mysql_connection.h"

#include <chrono>
#include <string>

namespace TLSS::FileStorage {

class ObjectRepository {
 public:
  // 借用连接，不接管其所有权；connection 的生命周期须覆盖本仓储的使用
  explicit ObjectRepository(BASE::DbConnection& connection);

  // 返回值通用约定：
  // - DbQueryResult.success 为 false 表示失败，不能把 rows 为空解释为无记录
  //   success 为 true 时才解释 rows；查询结果的列数、NULL 和内容由调用者校验
  // - DbExecuteResult.success 为 true 只表示语句执行成功，不表示业务目标已达成
  //   affected_rows 是影响行数；零行可能是正常的无操作，具体含义见各方法
  //   单对象写入预期最多影响一行，行数大于 1 时调用者应按异常结果处理
  //   error_code 保留数据库报告的错误码；insert_id 为返回的自增 ID，本表无自增键，
  //   不能用 insert_id 是否非零判断登记成功
  //   本地参数校验失败时返回默认失败结果，error_code 可能仍为 0，须先看 success
  // - 返回上述结果类型的方法在实际执行 SQL 后保留数据库返回结果
  //   本类不创建、提交或回滚事务；事务内写入成功仍须由调用者确认提交成功
  // - 本类不统一捕获异常；底层抛出的异常继续向上传播

  // 查询所有用户文件引用的去重哈希，每行一列 object_hash
  // success == false：查询失败；success == true 且 rows 为空：没有匹配的文件引用
  // success == true 且 rows 非空：返回被引用的哈希清单，不保证对应磁盘对象存在
  BASE::DbQueryResult list_referenced_hashes();

  // 普通查询，每行两列：state、unreferenced_at，后者允许为 NULL
  // success == false：查询失败；success == true 且 rows 为空：对象记录不存在
  // success == true 且 rows 非空：找到对象记录，不代表有引用或磁盘内容完整
  // 本方法不加数据库行锁，不单独构成后续修改或回收对象的许可
  BASE::DbQueryResult find_object(const std::string& hash);

  // 调用者须持有对应哈希锁，并使用当前连接的活动事务
  // 返回结构及空结果含义同 find_object；查询失败不能视为对象不存在
  // 使用 FOR UPDATE 锁定读；数据库锁随调用者事务提交或回滚释放
  BASE::DbQueryResult find_object_for_update(const std::string& hash);

  // 插入对象记录，初始 unreferenced_at 为 NULL；state 仅接受 LIVE、UNAVAILABLE。
  // success == false：状态参数非法或 INSERT 失败；重复主键也返回失败，不覆盖原记录。
  // success == true 且 affected_rows == 1：插入了一条记录，事务内仍待提交
  // success == true 但行数不为 1：不能据此确认对象登记成功
  BASE::DbExecuteResult insert_object(const std::string& hash, const std::string& state);

  // 调用者须持有哈希锁，在同一连接的活动事务中锁定对象，并已成功发布及验证磁盘对象
  // 将 LIVE、UNAVAILABLE 对象置为 LIVE 并清空计时，不修改 DELETING 对象
  // success == false：UPDATE 失败；success == true 且 affected_rows == 1：记录已更新
  // success == true 且 affected_rows == 0：可能原值已相同、记录不存在或状态不匹配，
  // 不能单凭零行判断原因；prepare_reference_in_transaction 会先锁定并校验原记录
  BASE::DbExecuteResult mark_live(const std::string& hash);

  // 调用者须持有对应哈希锁，并在同一连接的活动事务中先锁定对象记录
  // 使用锁定读查询所有用户的文件引用，最多一行一列，内容为 virtual_files
  // success == false：哈希非法或查询失败，不能认定无引用
  // success == true 且 rows 为空：本次查询未发现引用；非空：仍有引用，禁止回收
  BASE::DbQueryResult find_reference_for_update(const std::string& hash);

  // 调用者须持有哈希锁，在同一连接的活动事务中锁定对象并通过锁定读确认无引用
  // 仅为 LIVE、UNAVAILABLE 且计时为 NULL 的对象登记数据库当前时间
  // success == false：哈希非法或 UPDATE 失败
  // success == true 且 affected_rows == 1：首次无引用时间已登记，事务内仍待提交
  // success == true 且 affected_rows == 0：已有计时、记录不存在或状态不匹配；
  // 不刷新原有计时，也不表示对象已经满足删除条件
  BASE::DbExecuteResult mark_unreferenced(const std::string& hash);

  // 调用者须持有哈希锁，在同一连接的活动事务中锁定对象并通过锁定读确认存在引用
  // 清空 LIVE、UNAVAILABLE 对象的计时，保留原状态；不处理 DELETING 对象
  // success == false：哈希非法或 UPDATE 失败
  // success == true 且 affected_rows == 1：计时已清空，事务内仍待提交
  // success == true 且 affected_rows == 0：计时已为 NULL、记录不存在或状态不匹配；
  // 调用者已锁定并确认对象状态时，原计时为 NULL 的零行更新允许作为成功的无操作
  BASE::DbExecuteResult clear_unreferenced(const std::string& hash);

  // 调用者须持有哈希锁，在同一连接的活动事务中锁定对象并通过锁定读确认无引用
  // 使用数据库时间判断宽限期，仅将到期的 LIVE、UNAVAILABLE 对象改为 DELETING
  // success == false：哈希非法、宽限期非正数或 UPDATE 失败
  // success == true 且 affected_rows == 1：本事务已改为 DELETING，确认提交后才能删磁盘。
  // success == true 且 affected_rows == 0：未发生状态转换，可能未到期、未登记计时、
  // 记录不存在或状态不匹配；不能据此允许删除磁盘对象
  BASE::DbExecuteResult mark_deleting_if_expired(const std::string& hash,
                                                 std::chrono::seconds grace_period);

  // 调用者须持续持有哈希锁，且磁盘删除、目录同步已正常完成
  // 在同一连接的活动事务中锁定对象，确认 DELETING 且无引用后调用本方法
  // success == false：哈希非法或 DELETE 失败
  // success == true 且 affected_rows == 1：对象记录已在事务内删除，尚需确认提交
  // success == true 且 affected_rows == 0：记录不存在或不是 DELETING，不能单凭零行
  // 认定收尾完成；收尾事务在删除前单独处理记录已不存在的情况
  BASE::DbExecuteResult delete_deleting_object(const std::string& hash);

  // 枚举 DELETING 对象，按哈希排序，每行一列 object_hash；此查询不使用锁定读
  // success == false：查询失败，启动恢复不能继续
  // success == true 且 rows 为空：没有遗留删除任务；非空：返回待恢复的候选清单
  // 清单不构成删除许可，处理每个哈希时仍须重新加锁、检查状态和引用
  BASE::DbQueryResult list_deleting_hashes();

  // after_hash 为空表示从头扫描，否则须为合法对象哈希。
  // success == false：游标非法或查询失败。
  // success == true：每行一列哈希，按升序返回，最多 128 行。
  // rows 为空表示当前游标之后没有记录。
  // 清单包含所有对象状态，不构成删除许可；实际处理时仍须调用 collect()。
  BASE::DbQueryResult list_gc_candidates(const std::string& after_hash);

  // true：对对象表所需三列的 LIMIT 0 查询成功，表为空也返回 true
  // false：查询失败，例如表或列缺失、权限不足或连接异常
  // 本方法不创建表，也不校验完整表结构、索引、约束或存储引擎
  bool check_object_table();

  // 调用者须持有哈希锁、已成功发布及验证磁盘对象，并已在当前连接开启事务
  // true：不存在的对象已插入 LIVE 记录；或已有 LIVE、UNAVAILABLE 记录已执行
  // 恢复 LIVE、清空计时的操作（原值相同时允许零行更新），可继续在同一事务建立引用
  // false：哈希非法、查询或写入失败、结果结构异常、状态为 DELETING 或未知状态，
  // 或新插入记录的影响行数不为 1；调用者须终止并回滚当前事务
  // 返回 true 不表示文件引用已经插入或事务已提交；本方法不操作磁盘
  bool prepare_reference_in_transaction(const std::string& hash);

 private:
  BASE::DbConnection& _connection;
};
}  // namespace TLSS::FileStorage

#endif  // __INC_FTP_OBJECT_REPOSITORY_H__
