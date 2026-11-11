#include "ftp/object_gc_transaction.h"

#include "ftp/object_repository.h"
#include "ftp/transfer_validation.h"
namespace TLSS::FileStorage {
namespace {

// 有对象记录时，要求结果结构完整且状态合法；无引用时间允许为 NULL。
bool valid_object_result(const BASE::DbQueryResult& result) {
  if (!result.success || result.rows.size() != 1) {
    return false;
  }

  const auto& row = result.rows[0];
  if (row.size() != 2 || !row[0].has_value()) {
    return false;
  }

  const std::string& state = *row[0];
  return state == "LIVE" || state == "UNAVAILABLE" || state == "DELETING";
}

// 引用查询允许零行或一行；有行时要求一列非 NULL 的 ID。
bool valid_reference_result(const BASE::DbQueryResult& result) {
  if (!result.success || result.rows.size() > 1) {
    return false;
  }

  if (result.rows.empty()) {
    return true;
  }

  const auto& row = result.rows[0];
  return row.size() == 1 && row[0].has_value();
}

}  // namespace

ObjectGcPrepareResult prepare_object_gc_transaction(BASE::DbConnection& connection,
                                                    const std::string& hash,
                                                    std::chrono::seconds grace_period) {
  // 1. 校验哈希，以及 grace_period.count() > 0；任一不满足，返回 ObjectGcPrepareResult::failed。
  if (!Transfer::valid_hash(hash) || grace_period.count() <= 0) {
    return ObjectGcPrepareResult::failed;
  }
  // 2. 使用传入的 connection 创建 BASE::DbTransaction；active() 为假时返回 failed。
  BASE::DbTransaction transaction(connection);

  if (!transaction.active()) {
    return ObjectGcPrepareResult::failed;
  }
  // 3. 使用同一个连接创建 ObjectRepository。
  ObjectRepository object_rep(connection);
  // 4. 调用 find_object_for_update(hash)：
  //   - 查询失败：返回 failed。
  //   - 查询成功但没有记录：返回 skipped。
  BASE::DbQueryResult qu_res;
  qu_res = object_rep.find_object_for_update(hash);
  if (!qu_res.success) {
    return ObjectGcPrepareResult::failed;
  }

  if (qu_res.rows.empty()) {
    return ObjectGcPrepareResult::skipped;
  }
  // 5. 有记录时，校验结构及对象状态。
  if (!valid_object_result(qu_res)) {
    return ObjectGcPrepareResult::failed;
  }

  // 6. 保存状态，供下一步分支使用
  const std::string& state = *qu_res.rows[0][0];

  BASE::DbQueryResult reference_res = object_rep.find_reference_for_update(hash);
  if (!valid_reference_result(reference_res)) {
    return ObjectGcPrepareResult::failed;
  }

  ObjectGcPrepareResult result_after_commit = ObjectGcPrepareResult::skipped;
  if (!reference_res.rows.empty()) {
    // 1. 如果 state == "DELETING"，返回failed
    // 这是“删除中却仍有引用”的异常状态，不能继续删除，也不能直接恢复为 LIVE。
    if (state == "DELETING") {
      return ObjectGcPrepareResult::failed;
    }
    // 2. 调用 object_rep.clear_unreferenced(hash)
    BASE::DbExecuteResult ex_tmp_res;
    ex_tmp_res = object_rep.clear_unreferenced(hash);
    // 3. 执行失败，或者 affected_rows > 1，返回 failed
    if (!ex_tmp_res.success || ex_tmp_res.affected_rows > 1) {
      return ObjectGcPrepareResult::failed;
    }
    // 4. 允许零行更新，因为计时可能原本就是 NULL
    // 5. 保持 result_after_commit 为 skipped，暂时不返回成功
  } else {
    if (state == "DELETING") {
      // 之前已经进入删除状态，本次确认仍然没有引用。
      result_after_commit = ObjectGcPrepareResult::ready_to_delete;

    } else if (!qu_res.rows[0][1].has_value()) {
      // 首次发现无引用：登记时间，本轮不删除。
      BASE::DbExecuteResult ex_tmp_res = object_rep.mark_unreferenced(hash);

      if (!ex_tmp_res.success || ex_tmp_res.affected_rows != 1) {
        return ObjectGcPrepareResult::failed;
      }

      // result_after_commit 保持 skipped。

    } else {
      // 已有无引用时间，由数据库判断宽限期是否到期。
      BASE::DbExecuteResult ex_tmp_res = object_rep.mark_deleting_if_expired(hash, grace_period);

      if (!ex_tmp_res.success || ex_tmp_res.affected_rows > 1) {
        return ObjectGcPrepareResult::failed;
      }

      if (ex_tmp_res.affected_rows == 1) {
        result_after_commit = ObjectGcPrepareResult::ready_to_delete;
      }
      // 零行表示没有发生状态转换，保持 skipped。
    }
  }
  const BASE::DbCommitStatus commit_status = transaction.commit();

  switch (commit_status) {
    case BASE::DbCommitStatus::k_committed:
      return result_after_commit;

    case BASE::DbCommitStatus::k_outcome_unknown:
      return ObjectGcPrepareResult::outcome_unknown;

    case BASE::DbCommitStatus::k_not_active:
      return ObjectGcPrepareResult::failed;
  }

  // 对未来可能新增的提交状态，保守地按未知处理。
  return ObjectGcPrepareResult::outcome_unknown;
}

ObjectGcFinalizeResult finalize_object_gc_transaction(BASE::DbConnection& connection,
                                                      const std::string& hash) {
  // 1. 校验哈希，非法返回 failed 
  if (!Transfer::valid_hash(hash)) {
    return ObjectGcFinalizeResult::failed;
  }
  // 2. 创建 BASE::DbTransaction，未激活则返回 failed
  BASE::DbTransaction transaction(connection);
  if (!transaction.active()) {
    return ObjectGcFinalizeResult::failed;
  }
  // 3. 使用同一个连接创建 ObjectRepository 
  ObjectRepository object_rep(connection);
  // 4. 调用 find_object_for_update(hash)：
  //   - 查询失败：返回 failed 
  //   - 记录不存在：返回 completed，支持重复收尾
  //   - 有记录：先用 valid_object_result() 校验，再要求状态为 DELETING；不满足返回 failed 
  BASE::DbQueryResult qu_res = object_rep.find_object_for_update(hash);

  if (!qu_res.success) {
    return ObjectGcFinalizeResult::failed;
  }

  // 记录已不存在，说明无需再次删除，支持重复收尾
  if (qu_res.rows.empty()) {
    return ObjectGcFinalizeResult::completed;
  }

  if (!valid_object_result(qu_res)) {
    return ObjectGcFinalizeResult::failed;
  }

  const std::string& state = *qu_res.rows[0][0];
  if (state != "DELETING") {
    return ObjectGcFinalizeResult::failed;
  }
  // 5. 调用 find_reference_for_update(hash)：
  //   - 用 valid_reference_result() 校验
  //   - 校验失败或仍有引用，都返回 failed 
  BASE::DbQueryResult reference_res = object_rep.find_reference_for_update(hash);

  if (!valid_reference_result(reference_res) || !reference_res.rows.empty()) {
    return ObjectGcFinalizeResult::failed;
  }

  // 6. 调用 delete_deleting_object(hash)
  BASE::DbExecuteResult ex_res = object_rep.delete_deleting_object(hash);

  if (!ex_res.success || ex_res.affected_rows != 1) {
    return ObjectGcFinalizeResult::failed;
  }

  // 7. 提交删除，并映射提交结果
  const BASE::DbCommitStatus commit_status = transaction.commit();

  switch (commit_status) {
    case BASE::DbCommitStatus::k_committed:
      return ObjectGcFinalizeResult::completed;

    case BASE::DbCommitStatus::k_outcome_unknown:
      return ObjectGcFinalizeResult::outcome_unknown;

    case BASE::DbCommitStatus::k_not_active:
      return ObjectGcFinalizeResult::failed;
  }

  // 对未来可能新增的状态，保守地按结果未知处理
  return ObjectGcFinalizeResult::outcome_unknown;
}
}  // namespace TLSS::FileStorage
