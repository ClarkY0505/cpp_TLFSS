#include "ftp/file_rename.h"
#include "ftp/transfer_validation.h"

namespace TLSS::FileStorage {
RenameFileResult rename_file_transaction(BASE::DbConnection& connection,
                                         const std::string& username, const std::string& directory,
                                         const std::string& old_name, const std::string& new_name) {
  if (username.empty() || directory.empty() || !Transfer::valid_name(old_name) ||
      !Transfer::valid_name(new_name)) {
    return RenameFileResult::failed;
  }
  const std::string old_path = directory == "/" ? directory + old_name : directory + "/" + old_name;

  const std::string new_path = directory == "/" ? directory + new_name : directory + "/" + new_name;
  if (old_path.size() > 1024 || new_path.size() > 1024) {
    return RenameFileResult::failed;
  }

  BASE::DbTransaction transaction(connection);
  if (!transaction.active()) {
    return RenameFileResult::failed;
  }
  // 1. 先锁定用户
  auto user_result = connection.query_prepared(
      "SELECT id FROM users "
      "WHERE username = ? AND is_deleted = 0 "
      "FOR UPDATE;",
      {username});

  if (!user_result.success || user_result.rows.size() != 1 || user_result.rows[0].size() != 1 ||
      !user_result.rows[0][0].has_value()) {
    return RenameFileResult::failed;
  }
  // 2. 再查询并锁定当前目录
  auto parent_result = connection.query_prepared(
      "SELECT id FROM virtual_files "
      "WHERE username = ? AND path = ? "
      "AND file_type = 'directory' "
      "FOR UPDATE;",
      {username, directory});
  if (!parent_result.success || parent_result.rows.size() != 1 ||
      parent_result.rows[0].size() != 1 || !parent_result.rows[0][0].has_value()) {
    return RenameFileResult::failed;
  }
  // 3. 保存父目录 ID
  const std::string parent_id = *parent_result.rows[0][0];

  // 查询并锁定当前用户、当前目录下的源文件。
  auto file_result = connection.query_prepared(
      "SELECT id FROM virtual_files "
      "WHERE username = ? AND parent_id = ? "
      "AND file_name = ? "
      "AND CAST(path AS BINARY) = CAST(? AS BINARY) "
      "AND file_type = 'file' "
      "FOR UPDATE;",
      {username, parent_id, old_name, old_path});

  if (!file_result.success || file_result.rows.size() != 1 || file_result.rows[0].size() != 1 ||
      !file_result.rows[0][0].has_value()) {
    return RenameFileResult::failed;
  }

  const std::string file_id = *file_result.rows[0][0];
  auto target_result = connection.query_prepared(
      "SELECT id FROM virtual_files "
      "WHERE username = ? AND parent_id = ? "
      "AND file_name = ? AND id <> ? "
      "LIMIT 1 FOR UPDATE;",
      {username, parent_id, new_name, file_id});

  if (!target_result.success || !target_result.rows.empty()) {
    return RenameFileResult::failed;
  }

  // 名称发生变化时才执行更新
  if (old_name != new_name) {
    auto update_result = connection.execute_prepared(
        "UPDATE virtual_files SET file_name = ?, path = ? "
        "WHERE id = ? AND username = ? AND parent_id = ? "
        "AND file_name = ? "
        "AND CAST(path AS BINARY) = CAST(? AS BINARY) "
        "AND file_type = 'file';",
        {new_name, new_path, file_id, username, parent_id, old_name, old_path});

    if (!update_result.success || update_result.affected_rows != 1) {
      return RenameFileResult::failed;
    }
  }

  const auto commit_status = transaction.commit();

  switch (commit_status) {
    case BASE::DbCommitStatus::k_committed:
      return RenameFileResult::committed;

    case BASE::DbCommitStatus::k_not_active:
      return RenameFileResult::failed;

    case BASE::DbCommitStatus::k_outcome_unknown:
      return RenameFileResult::outcome_unknown;
  }

  return RenameFileResult::outcome_unknown;
}
}  // namespace TLSS::FileStorage
