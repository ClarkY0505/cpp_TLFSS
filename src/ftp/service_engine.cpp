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

StorageContext<std::shared_ptr<ConnectionSession>> StorageEngine::find_user_by_name(
    std::shared_ptr<ConnectionSession>& session) {
  auto db_conn = _db_conn_pool.get_connection();
  if (!db_conn) {
    BASE::mysql_logger()->error("storage engine get connection failed");
    return {false, session};
  }
  std::string username = session->name;
  std::string sql = "select id , salt , password_hash from users where username = '" + username + "' and is_deleted = 0;";

  std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> result(db_conn->query(sql),
                                                                  &mysql_free_result);
  if (!result) {
    BASE::mysql_logger()->error("storage engine query failed");
    return {false, session};
  }

  MYSQL_ROW row = mysql_fetch_row(result.get());
  if (row == nullptr) {
    return {false, session};
  }
  if (row[0] == nullptr) {
    return {false, session};
  }

  std::uint64_t id = 0;
  const char* const end = row[0] + std::strlen(row[0]);
  const auto parsed = std::from_chars(row[0], end, id);
  if (parsed.ec != std::errc{} || parsed.ptr != end || id == 0) {
    BASE::mysql_logger()->error("storage engine received invalid user id");
    return {false, session};
  }
  std::unique_lock<std::mutex> lock(_mutex);
  session->user_id = id;
  session->salt = row[1];
  session->pass = row[2];
  session->login_state = LoginState::k_await_password;
  lock.unlock();
  return {true, session};
}
}  // namespace TLSS::FileStorage
