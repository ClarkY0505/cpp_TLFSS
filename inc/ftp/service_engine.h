#ifndef __INC_FTP_SERVICE_ENGINE_H__
#define __INC_FTP_SERVICE_ENGINE_H__

#include "common/base/mysql_connection_pool.h"
#include "ftp/context.h"

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

 private:
  BASE::DbConnectionPool& _db_conn_pool;
};

}  // namespace TLSS::FileStorage

#endif  // __INC_FTP_SERVICE_ENGINE_H__
