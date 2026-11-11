#include <string>
#include "ftp/object_repository.h"

namespace TLSS::FileStorage {
ObjectRepository::ObjectRepository(BASE::DbConnection& connection)
    : _connection(connection) {}

BASE::DbQueryResult ObjectRepository::list_referenced_hashes() {
  std::string sql =
      "SELECT DISTINCT object_hash "
      "FROM virtual_files "
      "WHERE file_type = 'file' "
      "AND object_hash IS NOT NULL;";
  BASE::DbQueryResult res;
  res = _connection.query_prepared(sql);
  return res;
}

BASE::DbQueryResult ObjectRepository::find_object(const std::string& hash) {
  std::string sql =
      "SELECT state, unreferenced_at "
      "FROM storage_objects "
      "WHERE object_hash = ?;";
  BASE::DbQueryResult res;
  res = _connection.query_prepared(sql, {hash});
  return res;
}

BASE::DbExecuteResult ObjectRepository::insert_object(const std::string& hash,
                                                      const std::string& state) {
  if (state != "LIVE" && state != "UNAVAILABLE") {
    return {};
  }
  std::string sql =
      "INSERT INTO storage_objects (object_hash, state, unreferenced_at)"
      "VALUES (?, ?, NULL);";
  BASE::DbExecuteResult ex_res;
  ex_res = _connection.execute_prepared(sql, {hash, state});
  return ex_res;
}

}  // namespace TLSS::FileStorage
