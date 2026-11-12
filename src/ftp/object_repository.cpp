#include <string>
#include "common/base/mysql_connection.h"
#include "ftp/object_repository.h"
#include "ftp/transfer_validation.h"

namespace TLSS::FileStorage {
ObjectRepository::ObjectRepository(BASE::DbConnection& connection)
    : _connection(connection) {}

BASE::DbQueryResult ObjectRepository::list_referenced_hashes() {
  std::string sql =
      "SELECT DISTINCT object_hash "
      "FROM virtual_files "
      "WHERE file_type = 'file' "
      "AND object_hash IS NOT NULL;";
  BASE::DbQueryResult res;
  res = _connection.query_prepared(sql);
  return res;
}

BASE::DbQueryResult ObjectRepository::find_object(const std::string& hash) {
  std::string sql =
      "SELECT state, unreferenced_at "
      "FROM storage_objects "
      "WHERE object_hash = ?;";
  BASE::DbQueryResult res;
  res = _connection.query_prepared(sql, {hash});
  return res;
}

BASE::DbExecuteResult ObjectRepository::insert_object(const std::string& hash,
                                                      const std::string& state) {
  if (state != "LIVE" && state != "UNAVAILABLE") {
    return {};
  }
  std::string sql =
      "INSERT INTO storage_objects (object_hash, state, unreferenced_at)"
      "VALUES (?, ?, NULL);";
  BASE::DbExecuteResult ex_res;
  ex_res = _connection.execute_prepared(sql, {hash, state});
  return ex_res;
}

bool ObjectRepository::check_object_table() {
  std::string sql =
      "SELECT object_hash, state, unreferenced_at "
      "FROM storage_objects "
      "LIMIT 0;";
  BASE::DbQueryResult res;
  res = _connection.query_prepared(sql);

  return res.success;
}

BASE::DbQueryResult ObjectRepository::find_object_for_update(const std::string& hash) {
  std::string sql =
      "SELECT state, unreferenced_at "
      "FROM storage_objects "
      "WHERE object_hash = ? "
      "FOR UPDATE;";

  BASE::DbQueryResult res;
  res = _connection.query_prepared(sql, {hash});
  return res;
}

BASE::DbExecuteResult ObjectRepository::mark_live(const std::string& hash) {
  std::string sql =
      "UPDATE storage_objects "
      "SET state = 'LIVE', unreferenced_at = NULL "
      "WHERE object_hash = ? "
      "AND state IN ('LIVE', 'UNAVAILABLE');";
  BASE::DbExecuteResult ex_res;
  ex_res = _connection.execute_prepared(sql, {hash});
  return ex_res;
}

bool ObjectRepository::prepare_reference_in_transaction(const std::string& hash) {
  // 1. 用 Transfer::valid_hash(hash) 校验哈希，非法时返回 false。
  if (!Transfer::valid_hash(hash)) {
    return false;
  }
  // 2. 调用 find_object_for_update(hash)，查询失败返回 false。
  BASE::DbQueryResult qu_res;
  qu_res = find_object_for_update(hash);
  if (!qu_res.success) {
    return false;
  }
  // 3. 没有记录时，调用 insert_object(hash, "LIVE")，要求执行成功且 affected_rows == 1。
  if (qu_res.rows.empty()) {
    BASE::DbExecuteResult ex_res;
    ex_res = insert_object(hash, "LIVE");
    return ex_res.success && ex_res.affected_rows == 1;
  }
  // 4. 有记录时，先校验查询结果：
  //   - 恰好一行。
  //   - 该行恰好两列。
  //   - 第一列 state 不是 NULL。
  if (qu_res.rows.size() != 1 || qu_res.rows[0].size() != 2 || !qu_res.rows[0][0].has_value()) {
    return false;
  }
  // 5. 检查状态：
  //   - "LIVE" 或 "UNAVAILABLE"：允许继续。
  //   - "DELETING" 或其他未知值：返回 false。
  const std::string& state = *qu_res.rows[0][0];
  if (state != "LIVE" && state != "UNAVAILABLE") {
    return false;
  }
  // 6. 调用 mark_live(hash)，返回其 success。这里允许 affected_rows == 0
  BASE::DbExecuteResult ex_res;
  ex_res = mark_live(hash);
  return ex_res.success;
}

BASE::DbQueryResult ObjectRepository::find_reference_for_update(const std::string& hash) {
  if (!Transfer::valid_hash(hash)) {
    return {false, {}};
  }

  std::string sql =
      "SELECT id "
      "FROM virtual_files "
      "WHERE object_hash = ? "
      "AND file_type = 'file' "
      "LIMIT 1 "
      "FOR UPDATE;";

  BASE::DbQueryResult qu_res;
  qu_res = _connection.query_prepared(sql, {hash});
  return qu_res;
}

BASE::DbExecuteResult ObjectRepository::mark_unreferenced(const std::string& hash) {
  if (!Transfer::valid_hash(hash)) {
    return {false, {}};
  }

  std::string sql =
      "UPDATE storage_objects "
      "SET unreferenced_at = CURRENT_TIMESTAMP(6) "
      "WHERE object_hash = ? "
      "AND state IN ('LIVE', 'UNAVAILABLE') "
      "AND unreferenced_at IS NULL;";
  BASE::DbExecuteResult ex_res;
  ex_res = _connection.execute_prepared(sql, {hash});
  return ex_res;
}

BASE::DbExecuteResult ObjectRepository::clear_unreferenced(const std::string& hash) {
  if (!Transfer::valid_hash(hash)) {
    return {false, {}};
  }

  std::string sql =
      "UPDATE storage_objects "
      "SET unreferenced_at = NULL "
      "WHERE object_hash = ? "
      "AND state IN ('LIVE', 'UNAVAILABLE') "
      "AND unreferenced_at IS NOT NULL;";
  BASE::DbExecuteResult ex_res;
  ex_res = _connection.execute_prepared(sql, {hash});
  return ex_res;
}

BASE::DbExecuteResult ObjectRepository::mark_deleting_if_expired(
    const std::string& hash, std::chrono::seconds grace_period) {
  if (!Transfer::valid_hash(hash) || grace_period.count() <= 0) {
    return {false, {}};
  }

  std::string sql =
      "UPDATE storage_objects "
      "SET state = 'DELETING' "
      "WHERE object_hash = ? "
      "AND state IN ('LIVE', 'UNAVAILABLE') "
      "AND unreferenced_at IS NOT NULL "
      "AND unreferenced_at <= DATE_SUB(CURRENT_TIMESTAMP(6), INTERVAL ? SECOND);";
  BASE::DbExecuteResult ex_res;
  ex_res = _connection.execute_prepared(sql, {hash, std::to_string(grace_period.count())});
  // - success == false：执行失败。
  // - success == true && affected_rows == 1：本事务已将对象改成 DELETING，尚需提交。
  // - success == true && affected_rows == 0：没有发生状态转换，不能据此删除文件。
  return ex_res;
}

BASE::DbExecuteResult ObjectRepository::delete_deleting_object(const std::string& hash) {
  if (!Transfer::valid_hash(hash)) {
    return {false, {}};
  }

  std::string sql =
      "DELETE FROM storage_objects "
      "WHERE object_hash = ? "
      "AND state = 'DELETING';";
  BASE::DbExecuteResult ex_res;
  ex_res = _connection.execute_prepared(sql, {hash});
  return ex_res;
}

BASE::DbQueryResult ObjectRepository::list_deleting_hashes() {
  std::string sql =
      "SELECT object_hash "
      "FROM storage_objects "
      "WHERE state = 'DELETING' "
      "ORDER BY object_hash;";

  BASE::DbQueryResult res;
  res = _connection.query_prepared(sql);
  return res;
}

BASE::DbQueryResult ObjectRepository::list_gc_candidates(const std::string& after_hash) {
  // 1. 校验游标
  if (!after_hash.empty() && !Transfer::valid_hash(after_hash)) {
    return {false, {}};
  }

  std::string sql =
      "SELECT object_hash "
      "FROM storage_objects "
      "WHERE object_hash > ? "
      "ORDER BY object_hash "
      "LIMIT 128;";
  BASE::DbQueryResult res;
  res = _connection.query_prepared(sql, {after_hash});
  return res;
}

}  // namespace TLSS::FileStorage
