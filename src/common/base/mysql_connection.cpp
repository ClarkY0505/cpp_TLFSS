#include "common/base/base_logger.h"
#include "common/base/mysql_connection.h"

#include <mysql/mysql.h>
#include <cstdlib>
namespace TLSS::BASE {
DbConnection::DbConnection()
    : _conn(mysql_init(nullptr))
    , _alive_time(std::chrono::steady_clock::now()) {}

DbConnection::~DbConnection() {
  if (_conn != nullptr) {
    mysql_close(_conn);
  }
}

bool DbConnection::connect(const std::string& ip, unsigned short port, const std::string& username,
                           const std::string& password, const std::string& dbname) {
  MYSQL* p = mysql_real_connect(_conn, ip.c_str(), username.c_str(), password.c_str(),
                                dbname.c_str(), port, nullptr, 0);
  if (p == nullptr) {
    mysql_logger()->error("mysql_real_connect failed : {}", mysql_errno(_conn));
    return false;
  }
  return true;
}

bool DbConnection::ping() {
  return _conn != nullptr && mysql_ping(_conn) == 0;
}

bool DbConnection::update(const std::string& sql) {
  if (_conn == nullptr) {
    return false;
  }
  if (mysql_query(_conn, sql.c_str()) != 0) {
    mysql_logger()->error("MySQL update failed, errno: {}, error: {}", ::mysql_errno(_conn),
                          ::mysql_error(_conn));
    return false;
  }

  return true;
}

MYSQL_RES* DbConnection::query(const std::string& sql) {
  if (_conn == nullptr) {
    return nullptr;
  }
  if (mysql_query(_conn, sql.c_str()) != 0) {
    mysql_logger()->error("MySQL query failed: {}", ::mysql_error(_conn));
    return nullptr;
  }

  MYSQL_RES* result = ::mysql_store_result(_conn);

  if (result == nullptr) {
    mysql_logger()->error("MySQL store result failed: {}", ::mysql_error(_conn));
    return nullptr;
  }

  return result;
}
}  // namespace TLSS::BASE
