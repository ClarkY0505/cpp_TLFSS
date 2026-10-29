#ifndef __INC_FTP_SERVICE_ENGINE_H__
#define __INC_FTP_SERVICE_ENGINE_H__

#include "common/base/mysql_connection_pool.h"
#include "common/cache/arc_cache.h"
#include "ftp/context.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace TLSS::FileStorage {
class StorageEngine {
 public:
  explicit StorageEngine(BASE::DbConnectionPool& pool);

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

 private:
    // 保留原有参数化 SQL：公开方法负责缓存，以下方法只负责数据库访问。
    // 它们不能回调公开缓存接口，否则会再次申请 _metadata_mutex 而死锁。
    StorageContext<std::vector<FileInfo>> find_file_by_current_dir_db(
        const std::string& username, const std::string& current_directory);
    StorageContext<std::string> move_dir_by_current_dir_db(
        const std::string& username, const std::string& current_directory,
        const std::string& dir_name);
    StorageContext<std::string> find_download_hash_db(
        const std::string& username, const std::string& directory,
        const std::string& file_name);
    bool create_dir_db(const std::string& username, const std::string& current_directory,
                       const std::string& directory);
    bool commit_upload_db(const std::string& username, const std::string& parent_id,
                          const std::string& directory, const std::string& file_name,
                          const std::string& hash);

    BASE::DbConnectionPool& _db_conn_pool;

    // 基础版用同一把锁串行化「查缓存 -> 查数据库 -> 填充」和元数据写入。
    // ARC 自己的锁只能保护单次 get/put，不能阻止旧 SQL 结果在写入后回填。
    // 这里的保护范围包含 SQL，因此慢查询也会阻塞本引擎的其他缓存操作。
    std::mutex _metadata_mutex;

    // 列表和下载哈希的键携带此版本。写入前递增后，旧键便不会再被访问，
    // 旧条目由 ARC 按容量淘汰，无需新增 erase/clear 接口。
    // 所有用户共享版本：一次写入会保守地使全部列表和下载哈希失效。
    std::uint64_t _metadata_version = 0;

    // 按条目数限制容量，不是按字节数。缓存由同一引擎的业务/磁盘线程共享，
    // 不感知其他引擎实例或进程直接写入数据库；本版不提供 TTL。
    CACHE::ArcCache<std::string, std::vector<FileInfo>> _list_cache{1024};
    CACHE::ArcCache<std::string, std::string> _download_cache{4096};

    // CD 只缓存数据库确认过的成功路径。当前只新增、不删除/移动目录，
    // 所以 mkdir、上传不使已有路径关系失效；目录删除后的处理留待后续。
    CACHE::ArcCache<std::string, std::string> _cd_cache{4096};
};

}  // namespace TLSS::FileStorage

#endif  // __INC_FTP_SERVICE_ENGINE_H__
