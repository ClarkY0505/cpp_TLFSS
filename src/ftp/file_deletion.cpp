#include "ftp/file_deletion.h"
#include "ftp/metadata_types.h"
#include "ftp/transfer_validation.h"

namespace TLSS::FileStorage {

DeleteFileResult delete_file_transaction(BASE::DbConnection& connection,
                                         const std::string& username, const std::string& directory,
                                         const std::string& file_name) {
  // 1. 检查 username、directory 非空，
  // 以及 Transfer::valid_name(file_name) 为真；
  // 不满足就返回 failed
  if (username.empty() || !Transfer::valid_name(file_name) || directory.empty()) {
    return DeleteFileResult::failed;
  }

  // 2. 拼接文件完整路径
  const std::string path = directory == "/" ? directory + file_name : directory + "/" + file_name;
  if (path.size() > 1024) {
    return DeleteFileResult::failed;
  }

  // 3. 使用传入连接创建事务，并检查是否成功开启
  BASE::DbTransaction transaction(connection);

  if (!transaction.active()) {
    return DeleteFileResult::failed;
  }

  std::string sql =
      "SELECT id FROM users "
      "WHERE username = ? AND is_deleted = 0 "
      "FOR UPDATE;";

  auto user_result = connection.query_prepared(sql, {username});
  if (!user_result.success || user_result.rows.size() != 1 || user_result.rows[0].size() != 1 ||
      !user_result.rows[0][0].has_value()) {
    return DeleteFileResult::failed;
  }

  // 4. 根据用户名 和 当前路径来查找 parent_id
  sql =
      "SELECT id FROM virtual_files "
      "WHERE username = ? AND path = ? "
      "AND file_type = 'directory' "
      "FOR UPDATE;";

  auto parent_result = connection.query_prepared(sql, {username, directory});

  if (!parent_result.success || parent_result.rows.size() != 1 ||
      parent_result.rows[0].size() != 1 || !parent_result.rows[0][0].has_value()) {
    return DeleteFileResult::failed;
  }

  const std::string parent_id = *parent_result.rows[0][0];

  // 5. 查询并锁定待删除文件。
  sql =
      "SELECT id, object_hash FROM virtual_files "
      "WHERE username = ? AND parent_id = ? "
      "AND file_name = ? AND file_type = 'file' "
      "FOR UPDATE;";

  auto file_result = connection.query_prepared(sql, {username, parent_id, file_name});

  if (!file_result.success || file_result.rows.size() != 1 || file_result.rows[0].size() != 2 ||
      !file_result.rows[0][0].has_value() || !file_result.rows[0][1].has_value() ||
      !Transfer::valid_hash(*file_result.rows[0][1])) {
    return DeleteFileResult::failed;
  }

  const std::string file_id = *file_result.rows[0][0];
  const std::string object_hash = *file_result.rows[0][1];
  // 6. 删除刚才查询并锁定的文件元数据。
  sql =
      "DELETE FROM virtual_files "
      "WHERE id = ? AND username = ? AND parent_id = ? "
      "AND file_name = ? AND file_type = 'file' "
      "AND object_hash = ?;";

  auto delete_result =
      connection.execute_prepared(sql, {file_id, username, parent_id, file_name, object_hash});

  if (!delete_result.success || delete_result.affected_rows != 1) {
    return DeleteFileResult::failed;
  }

  // 7. 提交事务，并保留提交结果未知的状态。
  const auto commit_status = transaction.commit();

  switch (commit_status) {
    case BASE::DbCommitStatus::k_committed:
      return DeleteFileResult::committed;

    case BASE::DbCommitStatus::k_not_active:
      return DeleteFileResult::failed;

    case BASE::DbCommitStatus::k_outcome_unknown:
      return DeleteFileResult::outcome_unknown;
  }

  // 对未来可能增加的状态，保守地按结果未知处理。
  return DeleteFileResult::outcome_unknown;
}

}  // namespace TLSS::FileStorage
