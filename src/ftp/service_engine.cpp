#include <openssl/asn1.h>
#include <charconv>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include "common/base/base_logger.h"
#include "common/base/mysql_connection.h"
#include "common/base/mysql_connection_pool.h"
#include "ftp/context.h"
#include "ftp/file_transfer.h"
#include "ftp/service_engine.h"

namespace TLSS::FileStorage {
namespace {

// username 必须来自已认证身份，directory 使用会话中数据库确认的路径。
// 用用户名隔离租户，用当前目录/参数区分同名节点；不要只按文件名缓存。
// 字段带长度前缀，即使名称含 ':'，不同字段组合也不会拼成相同的键。
std::string metadata_key(std::uint64_t version, const std::string& username,
                         const std::string& directory, const std::string& argument = {}) {
    std::string key = std::to_string(version);
    const auto append = [&key](const std::string& field) {
        key.push_back(':');
        key += std::to_string(field.size());
        key.push_back(':');
        key += field;
    };
    append(username);
    append(directory);
    append(argument);
    return key;
}

}  // 匿名命名空间结束

StorageEngine::StorageEngine(BASE::DbConnectionPool& pool)
    : _db_conn_pool(pool) {}

StorageContext<std::vector<FileInfo>> StorageEngine::find_file_by_current_dir_db(
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
      "select file_name , file_type , created_at , updated_at "
      "from virtual_files "
      "where username = ? and parent_id = ?;",
      {username, id});
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

StorageContext<std::string> StorageEngine::move_dir_by_current_dir_db(
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

bool StorageEngine::create_dir_db(const std::string& username, const std::string& current_directory,
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
      "VALUES(? , ? , ? , ? ,'directory',NULL);",
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

bool StorageEngine::commit_upload_db(const std::string& username, const std::string& parent_id,
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

StorageContext<std::string> StorageEngine::find_download_hash_db(const std::string& username,
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

StorageContext<std::vector<FileInfo>> StorageEngine::find_file_by_current_dir(
    const std::string& username, const std::string& current_directory) {
    // 必须在生成版本键前加锁，且一直持有到查询和回填结束。并发冷查询因此
    // 能复用前一次的填充，写操作也不能穿插到旧结果的查询与回填之间。
    std::lock_guard<std::mutex> lock(_metadata_mutex);
    const auto key = metadata_key(_metadata_version, username, current_directory);

    std::vector<FileInfo> files;
    // 使用 bool get 区分「没有条目」和「成功缓存了一个空目录」。命中时
    // 不借用数据库连接；返回独立副本，调用者修改结果不会污染缓存。
    if (_list_cache.get(key, files)) {
        return {true, std::move(files)};
    }

    auto result = find_file_by_current_dir_db(username, current_directory);
    if (result.success) {
        _list_cache.put(key, result.result);
    }
    // 不存在、连接失败、SQL 错误都不缓存，后续请求仍有机会查询成功。
    return result;
}

StorageContext<std::string> StorageEngine::move_dir_by_current_dir(
    const std::string& username, const std::string& current_directory,
    const std::string& dir_name) {
    std::lock_guard<std::mutex> lock(_metadata_mutex);
    // CD 固定用版本 0：新增目录/文件不改变已有的父子目录关系。
    // 键还包含原目录和参数，避免 /docs 与 /archive/docs、子目录与 '..'
    // 混淆。命中 get 会累计热度，由 ARC 自行决定是否从 LRU 晋升到 LFU。
    const auto key = metadata_key(0, username, current_directory, dir_name);

    std::string path;
    if (_cd_cache.get(key, path)) {
        return {true, std::move(path)};
    }

    // 首次访问仍通过原 SQL 确认目录归属、类型与父子关系，不靠拼接路径猜测
    // 目录存在；根目录 '..' 也继续沿用原方法中直接返回 '/' 的行为。
    auto result = move_dir_by_current_dir_db(username, current_directory, dir_name);
    if (result.success) {
        _cd_cache.put(key, result.result);
    }
    // 不缓存失败的 CD，否则「先 cd 失败，再 mkdir」会被旧的失败结果挡住。
    // 此处只返回解析结果，会话 current_directory 仍由原 IO 线程更新。
    return result;
}

StorageContext<std::string> StorageEngine::find_download_hash(
    const std::string& username, const std::string& directory, const std::string& file_name) {
    std::lock_guard<std::mutex> lock(_metadata_mutex);
    const auto key = metadata_key(_metadata_version, username, directory, file_name);

    std::string hash;
    if (_download_cache.get(key, hash)) {
        return {true, std::move(hash)};
    }

    // 只缓存数据库验证过的用户/父目录/文件名到合法对象哈希的映射。
    // 文件内容和文件句柄不进入缓存，实际对象的打开与发送仍走原磁盘流程。
    auto result = find_download_hash_db(username, directory, file_name);
    if (result.success) {
        _download_cache.put(key, result.result);
    }
    return result;
}

bool StorageEngine::create_dir(const std::string& username,
                               const std::string& current_directory,
                               const std::string& directory) {
    std::lock_guard<std::mutex> lock(_metadata_mutex);
    // SQL 前推进版本：即便提交结果不确定或抛异常，旧列表也不再被命中。
    // 锁会持有到 SQL 返回，其他读者无法在「新版本但尚未写完」时进入。
    // 明确失败时也会多失效一次，这是基础版为了简单采用的保守策略。
    ++_metadata_version;
    return create_dir_db(username, current_directory, directory);
}

bool StorageEngine::commit_upload(const std::string& username, const std::string& parent_id,
                                  const std::string& directory, const std::string& file_name,
                                  const std::string& hash) {
    std::lock_guard<std::mutex> lock(_metadata_mutex);
    // 上传准备/文件块接收不改可见元数据，只有最终登记走这里使缓存失效。
    // 数据库中的归属验证、唯一索引和影响行数检查继续由原 SQL 实现负责。
    // prepare_upload 刻意不接缓存，每次仍查询数据库检查上传条件。
    ++_metadata_version;
    return commit_upload_db(username, parent_id, directory, file_name, hash);
}

}  // namespace TLSS::FileStorage
