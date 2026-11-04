#ifndef __INC_FTP_SERVICE_ENGINE_H__
#define __INC_FTP_SERVICE_ENGINE_H__

#include "common/base/mysql_connection_pool.h"
#include "ftp/context.h"
#include "ftp/metadata_cache.h"

#include <memory>
#include <string>
#include <vector>

namespace TLSS::FileStorage {
class StorageEngine {
 public:
  explicit StorageEngine(BASE::DbConnectionPool& pool);
  StorageEngine(BASE::DbConnectionPool& pool, std::shared_ptr<MetadataCache> cache);

  StorageEngine(const StorageEngine&) = delete;
  StorageEngine& operator=(const StorageEngine&) = delete;

  StorageContext<std::vector<FileInfo>> find_file_by_current_dir(
      const std::string& username, const std::string& current_directory);

  StorageContext<std::string> move_dir_by_current_dir(const std::string& username,
                                                      const std::string& current_directory,
                                                      const std::string& dir_name);
  bool create_dir(const std::string& username, const std::string& current_directory,
                  const std::string& directory);

  StorageContext<std::string> prepare_upload(const std::string& username,
                                             const std::string& directory,
                                             const std::string& file_name);

  bool commit_upload(const std::string& username, const std::string& parent_id,
                     const std::string& directory, const std::string& file_name,
                     const std::string& hash);

  StorageContext<std::string> find_download_hash(const std::string& username,
                                                 const std::string& directory,
                                                 const std::string& file_name);

  MetadataCacheStats metadata_cache_stats() const;

 private:
  // 保留原有参数化 SQL：公开方法负责缓存，以下方法只负责数据库访问。
  // 它们不能回调公开缓存接口，否则会再次申请目录协调锁而死锁。
  StorageContext<std::vector<FileInfo>> find_file_by_current_dir_db(
      const std::string& username, const std::string& current_directory);
  StorageContext<std::string> move_dir_by_current_dir_db(const std::string& username,
                                                         const std::string& current_directory,
                                                         const std::string& dir_name);
  StorageContext<std::string> find_download_hash_db(const std::string& username,
                                                    const std::string& directory,
                                                    const std::string& file_name);
  bool create_dir_db(const std::string& username, const std::string& current_directory,
                     const std::string& directory);
  bool commit_upload_db(const std::string& username, const std::string& parent_id,
                        const std::string& directory, const std::string& file_name,
                        const std::string& hash);

  BASE::DbConnectionPool& _db_conn_pool;

  // 默认由本引擎创建；多个引擎需要协调时必须显式共享同一个实例。
  std::shared_ptr<MetadataCache> _metadata_cache;
};

}  // namespace TLSS::FileStorage

#endif  // __INC_FTP_SERVICE_ENGINE_H__
