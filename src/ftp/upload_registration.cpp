#include "common/base/mysql_connection.h"
#include "ftp/object_backfill.h"
#include "ftp/object_repository.h"
#include "ftp/transfer_validation.h"
#include "ftp/upload_registration.h"

namespace TLSS::FileStorage {
UploadCommitResult commit_upload_transaction(
    BASE::DbConnection& connection, const std::string& username, const std::string& parent_id,
    const std::string& directory, const std::string& file_name, const std::string& hash) {
  // 1. 校验用户名、父目录 ID、目录非空，以及文件名、哈希合法性；失败返回 failed。
  if (username.empty() || parent_id.empty() || directory.empty() ||
      !Transfer::valid_name(file_name) || !Transfer::valid_hash(hash)) {
    return UploadCommitResult::failed;
  }
  // 2. 路径拼接，路径超过 1024 字节返回 failed。
  const std::string path = directory == "/" ? directory + file_name : directory + "/" + file_name;

  if (path.size() > 1024) {
    return UploadCommitResult::failed;
  }
  // 3. 使用传入连接创建 BASE::DbTransaction；active() 为 false 时返回 failed。
  BASE::DbTransaction transaction(connection);

  if (!transaction.active()) {
    return UploadCommitResult::failed;
  }
  // 4. 在事务内锁定当前用户
  std::string sql =
      "SELECT id FROM users "
      "WHERE username = ? AND is_deleted = 0 "
      "FOR UPDATE;";
  BASE::DbQueryResult qu_res;
  qu_res = connection.query_prepared(sql, {username});
  // - 查询成功。
  // - 找到恰好一个未删除的用户。
  // - 返回一列非空的用户 ID。
  if (!qu_res.success || qu_res.rows.size() != 1 || qu_res.rows[0].size() != 1 ||
      !qu_res.rows[0][0].has_value()) {
    return UploadCommitResult::failed;
  }
  // 5. 使用同一个 connection 创建 ObjectRepository。
  ObjectRepository object_rep(connection);
  // 6. 调用 prepare_reference_in_transaction(hash)，失败返回 failed。
  if (!object_rep.prepare_reference_in_transaction(hash)) {
    return UploadCommitResult::failed;
  }
  // 7. 准备插入sql
  sql =
      "INSERT INTO virtual_files "
      "(file_name, username, parent_id, path, file_type, object_hash) "
      "SELECT ?, p.username, p.id, ?, 'file', ? "
      "FROM virtual_files p "
      "WHERE p.id = ? AND p.username = ? "
      "AND p.path = ? AND p.file_type = 'directory';";
  BASE::DbExecuteResult ex_res;
  ex_res =
      connection.execute_prepared(sql, {file_name, path, hash, parent_id, username, directory});
  // 8. 文件插入要求 success 为真且 affected_rows == 1，否则返回 failed。
  if (!ex_res.success || ex_res.affected_rows != 1) {
    return UploadCommitResult::failed;
  }
  // 9. 调用事务的 commit()，映射结果：
  const BASE::DbCommitStatus commit_status = transaction.commit();
  switch (commit_status) {
    case BASE::DbCommitStatus::k_committed:
      return UploadCommitResult::committed;

    case BASE::DbCommitStatus::k_outcome_unknown:
      return UploadCommitResult::outcome_unknown;

    case BASE::DbCommitStatus::k_not_active:
      return UploadCommitResult::failed;
  }
  // 对未来可能增加的状态，保守地按结果未知处理。
  return UploadCommitResult::outcome_unknown;
}
}  // namespace TLSS::FileStorage
