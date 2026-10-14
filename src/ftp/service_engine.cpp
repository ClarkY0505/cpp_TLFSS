#include <charconv>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include "common/base/base_logger.h"
#include "common/base/mysql_connection.h"
#include "common/base/mysql_connection_pool.h"
#include "ftp/context.h"
#include "ftp/service_engine.h"

namespace TLSS::FileStorage {
StorageEngine::StorageEngine(BASE::DbConnectionPool& pool)
    : _db_conn_pool(pool) {}

StorageContext<UserInfo> StorageEngine::find_user_by_name(const std::string& username) {
  if (username.empty()) {
    return {false, {}};
  }

  auto db_conn = _db_conn_pool.get_connection();
  if (!db_conn) {
    BASE::mysql_logger()->error("storage engine get connection failed");
    return {false, {}};
  }

  auto result = db_conn->query_prepared(
      "SELECT id, salt, password_hash FROM users "
      "WHERE username = ? AND is_deleted = 0 LIMIT 1",
      {username});

  if (!result.success) {
    BASE::mysql_logger()->error("storage engine query failed");
    return {false, {}};
  }
  if (result.rows.empty()) {
    return {false, {}};
  }

  const auto& row = result.rows.front();
  if (row.size() != 3 || !row[0] || !row[1] || !row[2]) {
    BASE::mysql_logger()->error("storage engine received invalid user data");
    return {false, {}};
  }

  std::uint64_t id = 0;
  const std::string& text = *row[0];
  const char* end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), end, id);

  if (parsed.ec != std::errc{} || parsed.ptr != end || id == 0) {
    BASE::mysql_logger()->error("storage engine received invalid user id");
    return {false, {}};
  }

  return {true, UserInfo{id, *row[1], *row[2]}};
}

StorageContext<std::vector<FileInfo>> StorageEngine::find_file_by_current_dir(
    const std::string& username, const std::string& current_directory) {
  if (username.empty() || current_directory.empty()) {
    return {false, {}};
  }

  auto db_conn = _db_conn_pool.get_connection();
  if (!db_conn) {
    BASE::mysql_logger()->error("storage engine get connection failed");
    return {false, {}};
  }

  auto result = db_conn->query_prepared(
      "SELECT id FROM virtual_files WHERE username = ? "
      "AND path = ? ",
      {username, current_directory});

  if (!result.success) {
    BASE::mysql_logger()->error("storage engine query failed");
    return {false, {}};
  }
  if (result.rows.empty()) {
    return {false, {}};
  }

  const auto& row = result.rows.front();
  if (row.size() != 1 || !row[0]) {
    BASE::mysql_logger()->error("storage engine received invalid user data");
    return {false, {}};
  }

  std::string id = *row[0];
  result = db_conn->query_prepared(
      "select file_name , file_type , created_at , updated_at  from virtual_files "
      "where parent_id = ?;",
      {id});
  if (!result.success) {
    BASE::mysql_logger()->error("storage engine query failed");
    return {false, {}};
  }

  std::vector<FileInfo> files_info;
  for (const auto& row : result.rows) {
    if (row.size() != 4 || !row[0] || !row[1] || !row[2] || !row[3]) {
      return {false, {}};
    }

    FileInfo file_info;
    file_info.file_name = row[0].value_or("NULL");
    file_info.type = row[1].value_or("NULL");
    file_info.create_at = row[2].value_or("NULL");
    file_info.update_at = row[3].value_or("NULL");
    files_info.push_back(file_info);
  }

  return {true, files_info};
}

StorageContext<std::string> StorageEngine::move_dir_by_current_dir(
    const std::string& username, const std::string& current_directory,
    const std::string& dir_name) {
  if (username.empty() || current_directory.empty() || dir_name.empty()) {
    return {false, {}};
  }

  // 根目录没有上级目录，保持在根目录。
  if (dir_name == ".." && current_directory == "/") {
    return {true, "/"};
  }

  auto db_conn = _db_conn_pool.get_connection();
  if (!db_conn) {
    BASE::mysql_logger()->error("storage engine get connection failed");
    return {false, {}};
  }

  BASE::DbQueryResult result;

  if (dir_name != "..") {
    // parent 是当前目录，child 是要进入的子目录。
    result = db_conn->query_prepared(
        "SELECT child.path FROM virtual_files AS child "
        "JOIN virtual_files AS parent ON parent.id = child.parent_id "
        "AND parent.username = child.username "
        "WHERE child.username = ? "
        "AND child.file_name = ? "
        "AND child.file_type = 'directory' "
        "AND parent.file_type = 'directory' "
        "AND parent.path = ? LIMIT 1",
        {username, dir_name, current_directory});
  } else {
    // child 是当前目录，parent 是它的父目录。
    result = db_conn->query_prepared(
        "SELECT parent.path FROM virtual_files AS child "
        "JOIN virtual_files AS parent ON parent.id = child.parent_id "
        "AND parent.username = child.username "
        "WHERE child.username = ? "
        "AND child.path = ? "
        "AND child.file_type = 'directory' "
        "AND parent.file_type = 'directory' LIMIT 1",
        {username, current_directory});
  }

  if (!result.success) {
    BASE::mysql_logger()->error("storage engine query failed");
    return {false, {}};
  }

  if (result.rows.empty()) {
    return {false, {}};
  }

  const auto& row = result.rows.front();
  if (row.size() != 1 || !row[0] || row[0]->empty()) {
    BASE::mysql_logger()->error("storage engine received invalid directory data");
    return {false, {}};
  }

  return {true, *row[0]};
}

}  // namespace TLSS::FileStorage
