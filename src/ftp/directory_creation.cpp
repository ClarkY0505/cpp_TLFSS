#include "ftp/directory_creation.h"
#include "ftp/transfer_validation.h"

namespace TLSS::FileStorage {

CreateDirectoryResult create_directory_transaction(BASE::DbConnection& connection,
                                                   const std::string& username,
                                                   const std::string& current_directory,
                                                   const std::string& directory_name) {
  if (username.empty() || current_directory.empty() || !Transfer::valid_name(directory_name)) {
    return CreateDirectoryResult::failed;
  }

  const std::string new_path = current_directory == "/" ? current_directory + directory_name
                                                        : current_directory + "/" + directory_name;

  if (new_path.size() > 1024) {
    return CreateDirectoryResult::failed;
  }

  BASE::DbTransaction transaction(connection);
  if (!transaction.active()) {
    return CreateDirectoryResult::failed;
  }

  // 先锁定当前有效用户，再查询父目录，避免使用等待锁之前读到的旧目录状态
  std::string sql =
      "SELECT id FROM users "
      "WHERE username = ? AND is_deleted = 0 "
      "FOR UPDATE;";

  auto user_result = connection.query_prepared(sql, {username});

  if (!user_result.success || user_result.rows.size() != 1 || user_result.rows[0].size() != 1 ||
      !user_result.rows[0][0].has_value()) {
    return CreateDirectoryResult::failed;
  }

  // 确认父目录仍然存在、属于当前用户，并取得它的 ID
  sql =
      "SELECT id FROM virtual_files "
      "WHERE username = ? AND path = ? "
      "AND file_type = 'directory' "
      "FOR UPDATE;";

  auto parent_result = connection.query_prepared(sql, {username, current_directory});

  if (!parent_result.success || parent_result.rows.size() != 1 ||
      parent_result.rows[0].size() != 1 || !parent_result.rows[0][0].has_value()) {
    return CreateDirectoryResult::failed;
  }

  const std::string parent_id = *parent_result.rows[0][0];
  // 插入新目录，并确认恰好新增了一条记录
  sql =
      "INSERT INTO virtual_files "
      "(file_name, username, parent_id, path, file_type, object_hash) "
      "VALUES (?, ?, ?, ?, 'directory', NULL);";

  auto insert_result =
      connection.execute_prepared(sql, {directory_name, username, parent_id, new_path});

  if (!insert_result.success || insert_result.affected_rows != 1) {
    return CreateDirectoryResult::failed;
  }

  // 只有收到提交成功确认，才报告目录创建成功
  const auto commit_status = transaction.commit();

  switch (commit_status) {
    case BASE::DbCommitStatus::k_committed:
      return CreateDirectoryResult::committed;

    case BASE::DbCommitStatus::k_not_active:
      return CreateDirectoryResult::failed;

    case BASE::DbCommitStatus::k_outcome_unknown:
      return CreateDirectoryResult::outcome_unknown;
  }

  // 对未来可能增加的状态，保守地按结果未知处理。
  return CreateDirectoryResult::outcome_unknown;
}

}  // namespace TLSS::FileStorage
