#ifndef __INC_FTP_SERVICE_ENGINE_H__
#define __INC_FTP_SERVICE_ENGINE_H__

#include <mutex>
#include <vector>
#include "common/base/mysql_connection_pool.h"
#include "ftp/context.h"

namespace TLSS::FileStorage {
class StorageEngine {
 public:
  StorageEngine(BASE::DbConnectionPool& pool);
  StorageContext<std::shared_ptr<ConnectionSession>> find_user_by_name(std::shared_ptr<ConnectionSession> &session);
  StorageContext<std::vector<FileInfo>> find_file_by_current_dir(std::shared_ptr<ConnectionSession> &session);
  StorageContext<std::string> move_dir_by_current_dir(std::shared_ptr<ConnectionSession> &session , const std::string &dir_name);

 private:
  BASE::DbConnectionPool& _db_conn_pool;
  std::mutex _mutex;
};

}  // namespace TLSS::FileStorage

#endif  // __INC_FTP_SERVICE_ENGINE_H__
