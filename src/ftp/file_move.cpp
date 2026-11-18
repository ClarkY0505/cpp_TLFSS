#include "ftp/file_move.h"
#include "ftp/transfer_validation.h"

namespace TLSS::FileStorage {

MoveFileResult move_file_transaction(BASE::DbConnection& connection, const std::string& username,
                                     const std::string& source_directory,
                                     const std::string& target_directory,
                                     const std::string& file_name) {
  // 在进入事务之前拒绝空用户、空目录及非法的单个文件名
  if (username.empty() || source_directory.empty() || target_directory.empty() ||
      !Transfer::valid_name(file_name)) {
    return MoveFileResult::failed;
  }

  // 根目录已经包含 '/'，其他目录在拼接文件名时补充分隔符
  const std::string source_path =
      source_directory == "/" ? source_directory + file_name : source_directory + "/" + file_name;
  const std::string target_path =
      target_directory == "/" ? target_directory + file_name : target_directory + "/" + file_name;

  if (source_path.size() > 1024 || target_path.size() > 1024) {
    return MoveFileResult::failed;
  }

  // 开启事务，后续查询与更新都使用同一个 connection
  BASE::DbTransaction transaction(connection);
  if (!transaction.active()) {
    return MoveFileResult::failed;
  }

  // 先锁定当前用户
  auto user_result = connection.query_prepared(
      "SELECT id FROM users "
      "WHERE username = ? AND is_deleted = 0 "
      "FOR UPDATE;",
      {username});

  if (!user_result.success || user_result.rows.size() != 1 || user_result.rows[0].size() != 1 ||
      !user_result.rows[0][0].has_value()) {
    return MoveFileResult::failed;
  }

  const std::string directory_sql =
      "SELECT id FROM virtual_files "
      "WHERE username = ? "
      "AND CAST(path AS BINARY) = CAST(? AS BINARY) "
      "AND file_type = 'directory' "
      "FOR UPDATE;";

  // 查询源目录并保存 ID
  auto source_result = connection.query_prepared(directory_sql, {username, source_directory});

  if (!source_result.success || source_result.rows.size() != 1 ||
      source_result.rows[0].size() != 1 || !source_result.rows[0][0].has_value()) {
    return MoveFileResult::failed;
  }

  const std::string source_parent_id = *source_result.rows[0][0];

  // 查询目标目录
  auto target_result = connection.query_prepared(directory_sql, {username, target_directory});

  if (!target_result.success || target_result.rows.size() != 1 ||
      target_result.rows[0].size() != 1 || !target_result.rows[0][0].has_value()) {
    return MoveFileResult::failed;
  }

  const std::string target_parent_id = *target_result.rows[0][0];

  // 查询并锁定源文件
  auto file_result = connection.query_prepared(
      "SELECT id FROM virtual_files "
      "WHERE username = ? AND parent_id = ? "
      "AND file_name = ? "
      "AND CAST(path AS BINARY) = CAST(? AS BINARY) "
      "AND file_type = 'file' "
      "FOR UPDATE;",
      {username, source_parent_id, file_name, source_path});

  if (!file_result.success || file_result.rows.size() != 1 || file_result.rows[0].size() != 1 ||
      !file_result.rows[0][0].has_value()) {
    return MoveFileResult::failed;
  }

  const std::string file_id = *file_result.rows[0][0];

  // 检查目标目录是否被同名记录占用
  auto conflict_result = connection.query_prepared(
      "SELECT id FROM virtual_files "
      "WHERE username = ? AND parent_id = ? "
      "AND file_name = ? AND id <> ? "
      "LIMIT 1 FOR UPDATE;",
      {username, target_parent_id, file_name, file_id});

  if (!conflict_result.success || !conflict_result.rows.empty()) {
    return MoveFileResult::failed;
  }

  // 源、目标目录不同时才更新
  if (source_parent_id != target_parent_id) {
    auto update_result = connection.execute_prepared(
        "UPDATE virtual_files SET parent_id = ?, path = ? "
        "WHERE id = ? AND username = ? AND parent_id = ? "
        "AND file_name = ? "
        "AND CAST(path AS BINARY) = CAST(? AS BINARY) "
        "AND file_type = 'file';",
        {target_parent_id, target_path, file_id, username, source_parent_id, file_name,
         source_path});

    if (!update_result.success || update_result.affected_rows != 1) {
      return MoveFileResult::failed;
    }
  }

  // 提交并映射结果
  const auto commit_status = transaction.commit();

  switch (commit_status) {
    case BASE::DbCommitStatus::k_committed:
      return MoveFileResult::committed;

    case BASE::DbCommitStatus::k_not_active:
      return MoveFileResult::failed;

    case BASE::DbCommitStatus::k_outcome_unknown:
      return MoveFileResult::outcome_unknown;
  }

  return MoveFileResult::outcome_unknown;
}

}  // namespace TLSS::FileStorage
