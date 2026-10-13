#include "common/base/base_logger.h"
#include "common/base/mysql_connection.h"

#include <mysql/mysql.h>
#include <cstdlib>
namespace TLSS::BASE {
namespace {

// mysql 官方提供的一种思路
using StatementHandle = std::unique_ptr<MYSQL_STMT, decltype(&mysql_stmt_close)>;

// 通用的准备、绑定和执行逻辑。
MYSQL_STMT* prepare_and_execute(MYSQL* conn, const std::string& sql,
                                const std::vector<DbValue>& params, bool expect_result) {
  if (conn == nullptr) {
    mysql_logger()->error("MySQL connection is null");
    return nullptr;
  }

  StatementHandle stmt(mysql_stmt_init(conn), &mysql_stmt_close);
  if (!stmt) {
    mysql_logger()->error("MySQL statement initialization failed: {}", mysql_error(conn));
    return nullptr;
  }

  if (mysql_stmt_prepare(stmt.get(), sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
    mysql_logger()->error("MySQL prepare failed: {}", mysql_stmt_error(stmt.get()));
    return nullptr;
  }

  // 参数必须与 SQL 中的占位符一一对应。
  if (mysql_stmt_param_count(stmt.get()) != params.size()) {
    mysql_logger()->error("MySQL parameter count mismatch");
    return nullptr;
  }

  // 执行前检查，避免误用 query_prepared() 执行 UPDATE 等语句。
  const bool has_result = mysql_stmt_field_count(stmt.get()) != 0;
  if (has_result != expect_result) {
    mysql_logger()->error("MySQL statement result type mismatch");
    return nullptr;
  }

  std::vector<MYSQL_BIND> bindings(params.size());
  std::vector<unsigned long> lengths(params.size());

  for (std::size_t i = 0; i < params.size(); ++i) {
    if (!params[i].has_value()) {
      bindings[i].buffer_type = MYSQL_TYPE_NULL;
      continue;
    }

    lengths[i] = static_cast<unsigned long>(params[i]->size());
    bindings[i].buffer_type = MYSQL_TYPE_STRING;
    bindings[i].buffer = const_cast<char*>(params[i]->data());
    bindings[i].buffer_length = lengths[i];
    bindings[i].length = &lengths[i];
  }

  if (!bindings.empty() && mysql_stmt_bind_param(stmt.get(), bindings.data()) != 0) {
    mysql_logger()->error("MySQL parameter binding failed: {}", mysql_stmt_error(stmt.get()));
    return nullptr;
  }

  if (mysql_stmt_execute(stmt.get()) != 0) {
    mysql_logger()->error("MySQL execute failed: {}", mysql_stmt_error(stmt.get()));
    return nullptr;
  }

  // 调用者立即用 StatementHandle 接管，负责关闭语句。
  return stmt.release();
}

}  // namespace
DbConnection::DbConnection()
    : _conn(mysql_init(nullptr))
    , _alive_time(std::chrono::steady_clock::now()) {}

DbConnection::~DbConnection() {
  if (_conn != nullptr) {
    mysql_close(_conn);
  }
}

bool DbConnection::connect(const std::string& ip, unsigned short port, const std::string& username,
                           const std::string& password, const std::string& dbname,
                           unsigned int connect_timeout_sec, unsigned int read_timeout_sec,
                           unsigned int write_timeout_sec) {
  // 选项必须在建连前设置；同一条连接后续的 ping/query 也使用读写超时。
  // MySQL 使用秒作为选项单位；这些超时不是整个 SQL 操作的严格截止时间。
  if (_conn == nullptr ||
      mysql_options(_conn, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout_sec) != 0 ||
      mysql_options(_conn, MYSQL_OPT_READ_TIMEOUT, &read_timeout_sec) != 0 ||
      mysql_options(_conn, MYSQL_OPT_WRITE_TIMEOUT, &write_timeout_sec) != 0) {
    mysql_logger()->error("Failed to configure MySQL connection timeouts");
    return false;
  }
  // 网络建连在配置了客户端超时之后执行；失败交给连接池决定重试或停止。
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

bool DbConnection::update_prepared(const std::string& sql, const std::vector<DbValue>& params) {
  StatementHandle stmt(prepare_and_execute(_conn, sql, params, false), &mysql_stmt_close);

  return stmt != nullptr;
}

DbQueryResult DbConnection::query_prepared(const std::string& sql,
                                           const std::vector<DbValue>& params) {
  StatementHandle stmt(prepare_and_execute(_conn, sql, params, true), &mysql_stmt_close);

  if (!stmt) {
    return {};
  }

  struct ColumnState {
    unsigned long length = 0;
    bool is_null = false;
  };

  const unsigned int column_count = mysql_stmt_field_count(stmt.get());
  std::vector<ColumnState> states(column_count);
  std::vector<MYSQL_BIND> bindings(column_count);

  // 首次读取只取得 NULL 标志和字段实际长度。
  for (unsigned int i = 0; i < column_count; ++i) {
    bindings[i].buffer_type = MYSQL_TYPE_STRING;
    bindings[i].buffer = nullptr;
    bindings[i].buffer_length = 0;
    bindings[i].length = &states[i].length;
    bindings[i].is_null = &states[i].is_null;
  }

  if (mysql_stmt_bind_result(stmt.get(), bindings.data()) != 0) {
    mysql_logger()->error("MySQL result binding failed: {}", mysql_stmt_error(stmt.get()));
    return {};
  }

  DbQueryResult result;

  while (true) {
    const int status = mysql_stmt_fetch(stmt.get());

    if (status == MYSQL_NO_DATA) {
      result.success = true;
      return result;
    }

    // 零长度缓冲区导致的截断是预期行为，下面再读完整内容。
    if (status != 0 && status != MYSQL_DATA_TRUNCATED) {
      mysql_logger()->error("MySQL result fetch failed: {}", mysql_stmt_error(stmt.get()));
      return {};
    }

    DbRow row(column_count);

    for (unsigned int i = 0; i < column_count; ++i) {
      if (states[i].is_null) {
        continue;  // 此单元格保留 std::nullopt。
      }

      std::string value(states[i].length, '\0');

      if (!value.empty()) {
        MYSQL_BIND column = bindings[i];
        bool truncated = false;

        column.buffer = value.data();
        column.buffer_length = static_cast<unsigned long>(value.size());
        column.error = &truncated;

        if (mysql_stmt_fetch_column(stmt.get(), &column, i, 0) != 0 || truncated ||
            states[i].length != value.size()) {
          mysql_logger()->error("MySQL column fetch failed: {}", mysql_stmt_error(stmt.get()));
          return {};
        }
      }

      row[i] = std::move(value);
    }

    result.rows.push_back(std::move(row));
  }
}

}  // namespace TLSS::BASE
