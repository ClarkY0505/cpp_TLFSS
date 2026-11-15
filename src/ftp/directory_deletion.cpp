#include "ftp/directory_deletion.h"
#include "ftp/transfer_validation.h"

#include <unordered_set>
#include <vector>

namespace TLSS::FileStorage {

namespace {

struct DirectoryTreeNode {
  std::string id;
  std::string parent_id;
  std::string file_name;
  std::string path;
  std::string file_type;
};

}  // namespace

DeleteDirectoryResult delete_empty_directory_transaction(BASE::DbConnection& connection,
                                                         const std::string& username,
                                                         const std::string& current_directory,
                                                         const std::string& directory_name) {
  if (username.empty() || current_directory.empty() || !Transfer::valid_name(directory_name)) {
    return DeleteDirectoryResult::failed;
  }

  const std::string target_path = current_directory == "/"
                                      ? current_directory + directory_name
                                      : current_directory + "/" + directory_name;

  if (target_path.size() > 1024) {
    return DeleteDirectoryResult::failed;
  }

  BASE::DbTransaction transaction(connection);
  if (!transaction.active()) {
    return DeleteDirectoryResult::failed;
  }

  std::string sql =
      "SELECT id FROM users "
      "WHERE username = ? AND is_deleted = 0 "
      "FOR UPDATE;";

  auto user_result = connection.query_prepared(sql, {username});

  if (!user_result.success || user_result.rows.size() != 1 || user_result.rows[0].size() != 1 ||
      !user_result.rows[0][0].has_value()) {
    return DeleteDirectoryResult::failed;
  }

  // 定位当前目录下的目标子目录，并明确排除根目录
  sql =
      "SELECT id FROM virtual_files "
      "WHERE username = ? AND path = ? "
      "AND file_type = 'directory' "
      "FOR UPDATE;";

  auto parent_result = connection.query_prepared(sql, {username, current_directory});

  if (!parent_result.success || parent_result.rows.size() != 1 ||
      parent_result.rows[0].size() != 1 || !parent_result.rows[0][0].has_value()) {
    return DeleteDirectoryResult::failed;
  }

  const std::string parent_id = *parent_result.rows[0][0];

  // 定位当前目录下的目标子目录，并明确排除根目录
  sql =
      "SELECT id FROM virtual_files "
      "WHERE username = ? AND parent_id = ? "
      "AND file_name = ? "
      "AND CAST(path AS BINARY) = CAST(? AS BINARY) "
      "AND file_type = 'directory' "
      "AND parent_id > 0 AND path <> '/' "
      "FOR UPDATE;";

  auto target_result =
      connection.query_prepared(sql, {username, parent_id, directory_name, target_path});

  if (!target_result.success || target_result.rows.size() != 1 ||
      target_result.rows[0].size() != 1 || !target_result.rows[0][0].has_value()) {
    return DeleteDirectoryResult::failed;
  }

  const std::string target_id = *target_result.rows[0][0];
  // 目标目录是否包含任何文件或子目录
  sql =
      "SELECT id FROM virtual_files "
      "WHERE username = ? AND parent_id = ? "
      "LIMIT 1 FOR UPDATE;";

  auto children_result = connection.query_prepared(sql, {username, target_id});

  if (!children_result.success) {
    return DeleteDirectoryResult::failed;
  }

  if (!children_result.rows.empty()) {
    if (children_result.rows.size() != 1 || children_result.rows[0].size() != 1 ||
        !children_result.rows[0][0].has_value()) {
      return DeleteDirectoryResult::failed;
    }

    return DeleteDirectoryResult::not_empty;
  }

  // 只删除刚才确认为空、已经锁定的目标目录，并要求恰好影响一行
  sql =
      "DELETE FROM virtual_files "
      "WHERE id = ? AND username = ? AND parent_id = ? "
      "AND file_name = ? "
      "AND CAST(path AS BINARY) = CAST(? AS BINARY) "
      "AND file_type = 'directory' "
      "AND parent_id > 0 AND path <> '/';";

  auto delete_result = connection.execute_prepared(
      sql, {target_id, username, parent_id, directory_name, target_path});

  if (!delete_result.success || delete_result.affected_rows != 1) {
    return DeleteDirectoryResult::failed;
  }

  // 提交删除后，保留数据库返回的真实确认状态
  const auto commit_status = transaction.commit();
  switch (commit_status) {
    case BASE::DbCommitStatus::k_committed:
      return DeleteDirectoryResult::committed;

    case BASE::DbCommitStatus::k_not_active:
      return DeleteDirectoryResult::failed;

    case BASE::DbCommitStatus::k_outcome_unknown:
      return DeleteDirectoryResult::outcome_unknown;
  }

  // 对未来可能增加的状态，保守地按结果未知处理。
  return DeleteDirectoryResult::outcome_unknown;
}

DeleteDirectoryTreeResult delete_directory_tree_transaction(BASE::DbConnection& connection,
                                                            const std::string& username,
                                                            const std::string& current_directory,
                                                            const std::string& directory_name) {
  if (username.empty() || current_directory.empty() || !Transfer::valid_name(directory_name)) {
    return DeleteDirectoryTreeResult::failed;
  }

  const std::string target_path = current_directory == "/"
                                      ? current_directory + directory_name
                                      : current_directory + "/" + directory_name;

  if (target_path.size() > 1024) {
    return DeleteDirectoryTreeResult::failed;
  }

  BASE::DbTransaction transaction(connection);
  if (!transaction.active()) {
    return DeleteDirectoryTreeResult::failed;
  }

  // 在同一连接上锁定用户
  auto user_result = connection.query_prepared(
      "SELECT id FROM users "
      "WHERE username = ? AND is_deleted = 0 "
      "FOR UPDATE;",
      {username});
  if (!user_result.success || user_result.rows.size() != 1 || user_result.rows[0].size() != 1 ||
      !user_result.rows[0][0].has_value()) {
    return DeleteDirectoryTreeResult::failed;
  }
  std::string sql =
      "SELECT id FROM virtual_files "
      "WHERE username = ? AND path = ? "
      "AND file_type = 'directory' "
      "FOR UPDATE;";

  auto parent_result = connection.query_prepared(sql, {username, current_directory});

  if (!parent_result.success || parent_result.rows.size() != 1 ||
      parent_result.rows[0].size() != 1 || !parent_result.rows[0][0].has_value()) {
    return DeleteDirectoryTreeResult::failed;
  }

  const std::string parent_id = *parent_result.rows[0][0];

  // 定位当前目录下的目标子目录，并明确排除根目录
  sql =
      "SELECT id FROM virtual_files "
      "WHERE username = ? AND parent_id = ? "
      "AND file_name = ? "
      "AND CAST(path AS BINARY) = CAST(? AS BINARY) "
      "AND file_type = 'directory' "
      "AND parent_id > 0 AND path <> '/' "
      "FOR UPDATE;";

  auto target_result =
      connection.query_prepared(sql, {username, parent_id, directory_name, target_path});

  if (!target_result.success || target_result.rows.size() != 1 ||
      target_result.rows[0].size() != 1 || !target_result.rows[0][0].has_value()) {
    return DeleteDirectoryTreeResult::failed;
  }

  const std::string target_id = *target_result.rows[0][0];
  std::vector<DirectoryTreeNode> nodes{
      {target_id, parent_id, directory_name, target_path, "directory"}};

  std::unordered_set<std::string> visited{target_id};
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    const auto node = nodes[index];

    // 文件需要保留在删除清单中，但不需要展开子项。
    if (node.file_type != "directory") {
      continue;
    }

    auto children_result = connection.query_prepared(
        "SELECT id, file_name, path, file_type FROM virtual_files "
        "WHERE username = ? AND parent_id = ? "
        "FOR UPDATE;",
        {username, node.id});

    if (!children_result.success) {
      return DeleteDirectoryTreeResult::failed;
    }

    // 校验每行结果，并追加到 nodes。
    for (const auto& row : children_result.rows) {
      if (row.size() != 4 || !row[0].has_value() || !row[1].has_value() || !row[2].has_value() ||
          !row[3].has_value()) {
        return DeleteDirectoryTreeResult::failed;
      }
      const std::string id = *row[0];
      const std::string file_name = *row[1];
      const std::string path = *row[2];
      const std::string file_type = *row[3];
      if (id.empty() || !Transfer::valid_name(file_name) ||
          (file_type != "file" && file_type != "directory") || path.size() > 1024 ||
          path != node.path + "/" + file_name) {
        return DeleteDirectoryTreeResult::failed;
      }

      if (!visited.insert(id).second) {
        return DeleteDirectoryTreeResult::failed;
      }
      nodes.push_back({id, node.id, file_name, path, file_type});
    }
  }
  // 反向遍历树 ，删除子目录 循环
  for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
    const auto& node = *it;
    sql =
        "DELETE FROM virtual_files "
        "WHERE id = ? AND username = ? AND parent_id = ? "
        "AND file_name = ? "
        "AND CAST(path AS BINARY) = CAST(? AS BINARY) "
        "AND file_type = ? "
        "AND parent_id > 0 AND path <> '/';";

    auto res = connection.execute_prepared(
        sql, {node.id, username, node.parent_id, node.file_name, node.path, node.file_type});
    // 此时检查执行成功，并且 affected_rows == 1
    if (!res.success || res.affected_rows != 1) {
     return DeleteDirectoryTreeResult::failed;
    }
  }
  // 所有节点均已删除成功，在这里统一提交一次
  const auto commit_status = transaction.commit();

  switch (commit_status) {
    case BASE::DbCommitStatus::k_committed:
      // 已确认整棵子树的元数据删除提交成功
      return DeleteDirectoryTreeResult::committed;

    case BASE::DbCommitStatus::k_not_active:
      // 没有可提交的活动事务，不能报告成功
      return DeleteDirectoryTreeResult::failed;

    case BASE::DbCommitStatus::k_outcome_unknown:
      // 可能已经提交，只是没有收到确认
      // 不重试删除，也不能认定已经回滚
      return DeleteDirectoryTreeResult::outcome_unknown;
  }

  // 对未来可能新增的状态，保守返回结果未知
  return DeleteDirectoryTreeResult::outcome_unknown;
}

}  // namespace TLSS::FileStorage
