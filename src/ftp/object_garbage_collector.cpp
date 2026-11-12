#include "common/base/mysql_connection_pool.h"
#include "ftp/object_garbage_collector.h"
#include "ftp/object_gc_transaction.h"
#include "ftp/object_repository.h"
#include "ftp/transfer_validation.h"

#include <mutex>

namespace TLSS::FileStorage {
ObjectGarbageCollector::ObjectGarbageCollector(BASE::DbConnectionPool& pool,
                                               ObjectStorageEngine& objects,
                                               ObjectCoordinator& coordinator)
    : _pool(pool)
    , _objects(objects)
    , _coordinator(coordinator) {}

ObjectGcCollectResult ObjectGarbageCollector::collect(const std::string& hash,
                                                      std::chrono::seconds grace_period) {
  // 1. 校验哈希和 grace_period.count() > 0，不满足返回 failed
  if (!Transfer::valid_hash(hash) || grace_period.count() <= 0) {
    return ObjectGcCollectResult::failed;
  }
  // 2. 获取哈希锁
  std::lock_guard<std::mutex> lock(_coordinator.mutex_for(hash));

  // 3. 调用 _pool.get_connection()，空句柄返回 failed
  BASE::DbConnectionHandle db = _pool.get_connection();
  if (!db) {
    return ObjectGcCollectResult::failed;
  }

  // 4. 执行准备事务，只有明确允许删除才能继续。
  const ObjectGcPrepareResult prepare_result =
      prepare_object_gc_transaction(*db, hash, grace_period);

  switch (prepare_result) {
    case ObjectGcPrepareResult::skipped:
      return ObjectGcCollectResult::skipped;

    case ObjectGcPrepareResult::failed:
      return ObjectGcCollectResult::failed;

    case ObjectGcPrepareResult::outcome_unknown:
      return ObjectGcCollectResult::outcome_unknown;

    case ObjectGcPrepareResult::ready_to_delete:
      break;  // 继续执行后面的磁盘删除。

    default:
      return ObjectGcCollectResult::outcome_unknown;
  }

  // 5. 调用 _objects.remove_object(hash)
  _objects.remove_object(hash);

  // 6. 保存返回值
  const ObjectGcFinalizeResult finalize_res = finalize_object_gc_transaction(*db, hash);

  // 7.映射结果
  switch (finalize_res) {
    case ObjectGcFinalizeResult::completed:
      return ObjectGcCollectResult::collected;

    case ObjectGcFinalizeResult::failed:
      return ObjectGcCollectResult::failed;

    case ObjectGcFinalizeResult::outcome_unknown:

    default:
      return ObjectGcCollectResult::outcome_unknown;
  }
}

bool ObjectGarbageCollector::recover_deleting(std::chrono::seconds grace_period) {
  if (grace_period.count() <= 0) {
    return false;
  }

  // 1. 查询清单，用内部作用域限制连接的借用时间
  BASE::DbQueryResult deleting_result;
  {
    auto db = _pool.get_connection();
    if (!db) {
      return false;
    }

    ObjectRepository repository(*db);
    deleting_result = repository.list_deleting_hashes();
  }

  if (!deleting_result.success) {
    return false;
  }

  for (auto& row : deleting_result.rows) {
    // 先检查列数，再访问第 0 列。
    if (row.size() != 1 || !row[0].has_value()) {
      return false;
    }

    const std::string& hash = *row[0];

    if (!Transfer::valid_hash(hash)) {
      return false;
    }
    // 对每个合法哈希调用 collect(hash, grace_period)：
    // - collected 或 skipped：继续。
    // - 其他结果：立即返回 false。
    ObjectGcCollectResult gc_res = collect(hash, grace_period);
    if (gc_res != ObjectGcCollectResult::collected && gc_res != ObjectGcCollectResult::skipped) {
      return false;
    }
  }
  return true;
}

bool ObjectGarbageCollector::collect_batch(std::string& after_hash,
                                           std::chrono::seconds grace_period) {
  // 1. 校验宽限期为正数，以及游标为空或为合法哈希
  if (grace_period.count() <= 0 || (!after_hash.empty() && !Transfer::valid_hash(after_hash))) {
    return false;
  }
  BASE::DbQueryResult candidate_res;
  {
    auto db = _pool.get_connection();
    if (!db) {
      return false;
    }
    ObjectRepository repository(*db);
    candidate_res = repository.list_gc_candidates(after_hash);
  }

  if (!candidate_res.success || candidate_res.rows.size() > 128) {
    return false;
  }

  if (candidate_res.success && candidate_res.rows.empty()) {
    after_hash.clear();
    return true;
  }

  // 先校验整批结果，暂不调用 GC：
  //  - 每行恰好一列且不是 NULL。
  //  - 每个值是合法哈希。
  //  - 哈希严格递增，且第一条大于传入游标。
  std::string previous_hash = after_hash;

  for (const auto& row : candidate_res.rows) {
    if (row.size() != 1 || !row[0].has_value()) {
      return false;
    }

    const std::string& hash = *row[0];

    // 同时要求合法、严格递增，并大于查询时的游标
    if (!Transfer::valid_hash(hash) || hash <= previous_hash) {
      return false;
    }

    previous_hash = hash;
  }

  // 整批校验通过后，逐个处理对象
  for (const auto& row : candidate_res.rows) {
    const std::string& hash = *row[0];

    // 先记录本次尝试的位置，失败时也保留进度
    after_hash = hash;

    const ObjectGcCollectResult gc_res = collect(hash, grace_period);

    if (gc_res != ObjectGcCollectResult::collected && gc_res != ObjectGcCollectResult::skipped) {
      return false;
    }
  }

  // 所有对象均已回收或无需处理
  return true;
}

}  // namespace TLSS::FileStorage
