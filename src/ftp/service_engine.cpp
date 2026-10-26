#include <openssl/asn1.h>
#include <charconv>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include "common/base/base_logger.h"
#include "common/base/mysql_connection.h"
#include "common/base/mysql_connection_pool.h"
#include "ftp/context.h"
#include "ftp/file_transfer.h"
#include "ftp/service_engine.h"

namespace TLSS::FileStorage {
StorageEngine::StorageEngine(BASE::DbConnectionPool& pool)
    : _db_conn_pool(pool) {}

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

bool StorageEngine::create_dir(const std::string& username, const std::string& current_directory,
                               const std::string& directory) {
  if (username.empty() || current_directory.empty() || directory.empty()) {
    return false;
  }

  auto db_conn = _db_conn_pool.get_connection();
  if (!db_conn) {
    BASE::mysql_logger()->error("storage engine get connection failed");
    return false;
  }

  auto result = db_conn->query_prepared(
      "SELECT id FROM virtual_files "
      "WHERE path = ? AND username = ?;",
      {current_directory, username});

  if (!result.success) {
    BASE::mysql_logger()->error("storage engine query failed");
    return false;
  }
  if (result.rows.empty()) {
    return false;
  }

  const auto& row = result.rows.front();
  if (row.size() != 1 || !row[0]) {
    BASE::mysql_logger()->error("storage engine received invalid user data");
    return false;
  }

  std::string id = *row[0];
  // 根目录本身已包含分隔符；嵌套目录才需要追加一个 '/'.
  const std::string new_dir_path =
      (current_directory == "/" ? current_directory : current_directory + "/") + directory;
  bool res = db_conn->update_prepared(
      "INSERT INTO virtual_files"
      "(file_name, username, parent_id, path, file_type, object_hash)"
      "VALUES(? , ? , ? , ? ,'directory','NULL');",
      {directory, username, id, new_dir_path});

  return res;
}

StorageContext<std::string> StorageEngine::prepare_upload(const std::string& username,
                                                          const std::string& directory,
                                                          const std::string& file_name) {
  if (username.empty() || directory.empty() || !Transfer::valid_name(file_name)) {
    return {false, {}};
  }

  const std::string path = directory == "/" ? directory + file_name : directory + "/" + file_name;
  if (path.size() > 1024) {
    return {false, {}};
  }

  auto db = _db_conn_pool.get_connection();
  if (!db) {
    return {false, {}};
  }

  //
  // 找到指定用户的指定父目录
  // 并且确认该目录下还没有同名的文件或目录
  // 满足条件时，返回父目录的 id
  auto result = db->query_prepared(
      "SELECT p.id FROM virtual_files p "
      "WHERE p.username = ? AND p.path = ? "
      "AND p.file_type = 'directory' "
      "AND NOT EXISTS ("
      " SELECT 1 FROM virtual_files f "
      " WHERE f.username = p.username "
      " AND f.parent_id = p.id AND f.file_name = ?"
      ") LIMIT 1",
      {username, directory, file_name});

  if (!result.success || result.rows.size() != 1 || result.rows[0].size() != 1 ||
      !result.rows[0][0]) {
    return {false, {}};
  }
  return {true, *result.rows[0][0]};
}

bool StorageEngine::commit_upload(const std::string& username, const std::string& parent_id,
                                  const std::string& directory, const std::string& file_name,
                                  const std::string& hash) {
  if (!Transfer::valid_name(file_name) || !Transfer::valid_hash(hash)) {
    return false;
  }

  const std::string path = directory == "/" ? directory + file_name : directory + "/" + file_name;
  if (path.size() > 1024) {
    return false;
  }

  auto db = _db_conn_pool.get_connection();
  if (!db) {
    return false;
  }

  // 再次验证父目录属于当前用户
  // 唯一索引负责拦截并发同名上传
  // 先查询父目录，再利用查询结果插入一条文件记录
  // 父目录满足条件才会插入
  const auto result = db->execute_prepared(
      "INSERT INTO virtual_files "
      "(file_name, username, parent_id, path, "
      " file_type, object_hash) "
      "SELECT ?, p.username, p.id, ?, 'file', ? "
      "FROM virtual_files p "
      "WHERE p.id = ? AND p.username = ? "
      "AND p.path = ? AND p.file_type = 'directory'",
      {file_name, path, hash, parent_id, username, directory});

  return result.success && result.affected_rows == 1;
}

StorageContext<std::string> StorageEngine::find_download_hash(const std::string& username,
                                                              const std::string& directory,
                                                              const std::string& file_name) {
  if (!Transfer::valid_name(file_name)) {
    return {false, {}};
  }

  auto db = _db_conn_pool.get_connection();
  if (!db) {
    return {false, {}};
  }

  auto result = db->query_prepared(
      "SELECT f.object_hash "
      "FROM virtual_files f "
      "JOIN virtual_files p "
      "ON p.id = f.parent_id "
      "AND p.username = f.username "
      "WHERE f.username = ? AND p.path = ? "
      "AND p.file_type = 'directory' "
      "AND f.file_name = ? AND f.file_type = 'file' "
      "LIMIT 1",
      {username, directory, file_name});

  if (!result.success || result.rows.size() != 1 || result.rows[0].size() != 1 ||
      !result.rows[0][0] || !Transfer::valid_hash(*result.rows[0][0])) {
    return {false, {}};
  }

  return {true, *result.rows[0][0]};
}
}  // namespace TLSS::FileStorage
