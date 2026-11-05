#include "common/base/mysql_connection.h"
#include "ftp/metadata_service.h"

namespace TLSS::FileStorage {
bool MetadataService::create_root_in_transaction(BASE::DbConnection& connection,
                                                 const std::string& username) {
  const auto root = connection.execute_prepared(
      "INSERT INTO virtual_files(username, file_name, parent_id, path, file_type, object_hash) "
      "VALUES (?, '/', -1, '/', 'directory', NULL)",
      {username});
  return root.success && root.affected_rows == 1;
}
}  // namespace TLSS::FileStorage
