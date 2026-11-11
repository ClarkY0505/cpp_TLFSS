#ifndef __INC_FTP_OBJECT_REPOSITORY_H__
#define __INC_FTP_OBJECT_REPOSITORY_H__

#include "common/base/mysql_connection.h"
#include <string>
namespace TLSS::FileStorage {

class ObjectRepository {
 public:
  explicit ObjectRepository(BASE::DbConnection& connection);

  BASE::DbQueryResult list_referenced_hashes();
  BASE::DbQueryResult find_object(const std::string& hash);
  BASE::DbExecuteResult insert_object(const std::string& hash, const std::string& state);

 private:
  BASE::DbConnection& _connection;
};
}  // namespace TLSS::FileStorage

#endif  // __INC_FTP_OBJECT_REPOSITORY_H__
