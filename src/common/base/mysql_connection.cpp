#include "common/base/base_logger.h"
#include "common/base/mysql_connection.h"

#include <mysql/mysql.h>
#include <mysql/errmsg.h>
#include <cstdlib>
#include <memory>
#include <utility>
namespace TLSS::BASE {
namespace {

// mysql 官方提供的一种思路
using StatementHandle = std::unique_ptr<MYSQL_STMT, decltype(&mysql_stmt_close)>;

// 通用的准备、绑定和执行逻辑。
MYSQL_STMT* prepare_and_execute(MYSQL* conn, const std::string& sql,
                                const std::vector<DbValue>& params, bool expect_result,
                                unsigned int* error_code = nullptr) {
  if (error_code != nullptr) {
    *error_code = 0;
  }
  auto fail = [error_code](unsigned int code) -> MYSQL_STMT* {
    if (code == 0) {
      code = CR_UNKNOWN_ERROR;
    }
    if (error_code != nullptr) {
      *error_code = code;
    }
    mysql_logger()->error("MySQL prepared statement failed, errno: {}", code);
    return nullptr;
  };
  if (conn == nullptr) {
    return fail(CR_SERVER_GONE_ERROR);
  }

  StatementHandle stmt(mysql_stmt_init(conn), &mysql_stmt_close);
  if (!stmt) {
    return fail(mysql_errno(conn));
  }

  if (mysql_stmt_prepare(stmt.get(), sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
    return fail(mysql_stmt_errno(stmt.get()));
  }

  // 参数必须与 SQL 中的占位符一一对应。
  if (mysql_stmt_param_count(stmt.get()) != params.size()) {
    mysql_logger()->error("MySQL parameter count mismatch");
    return fail(CR_UNKNOWN_ERROR);
  }

  // 执行前检查，避免误用 query_prepared() 执行 UPDATE 等语句。
  const bool has_result = mysql_stmt_field_count(stmt.get()) != 0;
  if (has_result != expect_result) {
    mysql_logger()->error("MySQL statement result type mismatch");
    return fail(CR_UNKNOWN_ERROR);
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
    return fail(mysql_stmt_errno(stmt.get()));
  }

  if (mysql_stmt_execute(stmt.get()) != 0) {
    return fail(mysql_stmt_errno(stmt.get()));
  }

  // 调用者立即用 StatementHandle 接管，负责关闭语句。
  return stmt.release();
}

}  // namespace
DbConnection::DbConnection()
    : _conn(mysql_init(nullptr))
    , _alive_time(std::chrono::steady_clock::now()) {}

DbConnection::~DbConnection() {
  invalidate();
}

void DbConnection::invalidate() noexcept {
  _connected = false;
  _transaction_active = false;
  if (_conn != nullptr) {
    mysql_close(_conn);
    _conn = nullptr;
  }
}

bool DbConnection::connect(const std::string& ip, unsigned short port, const std::string& username,
                           const std::string& password, const std::string& dbname,
                           unsigned int connect_timeout_sec, unsigned int read_timeout_sec,
                           unsigned int write_timeout_sec) {
  if (_conn == nullptr || _connected) {
    return false;
  }
  // mysql_init 默认禁用自动重连，本类不启用它，避免事务跨会话继续执行。
  // 不调用已弃用的 MYSQL_OPT_RECONNECT（即使设为 false 也会输出警告）。
  // 选项必须在建连前设置；同一条连接后续的 ping/query 也使用读写超时。
  // MySQL 使用秒作为选项单位；这些超时不是整个 SQL 操作的严格截止时间。
  if (mysql_options(_conn, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout_sec) != 0 ||
      mysql_options(_conn, MYSQL_OPT_READ_TIMEOUT, &read_timeout_sec) != 0 ||
      mysql_options(_conn, MYSQL_OPT_WRITE_TIMEOUT, &write_timeout_sec) != 0) {
    mysql_logger()->error("Failed to configure MySQL connection options");
    invalidate();
    return false;
  }
  // 网络建连在配置了客户端超时之后执行；失败交给连接池决定重试或停止。
  MYSQL* p = mysql_real_connect(_conn, ip.c_str(), username.c_str(), password.c_str(),
                                dbname.c_str(), port, nullptr, 0);
  if (p == nullptr) {
    mysql_logger()->error("mysql_real_connect failed : {}", mysql_errno(_conn));
    invalidate();
    return false;
  }
  // 固定连接池的基础会话状态；业务通过 START TRANSACTION 临时开启事务。
  if (mysql_autocommit(_conn, true) != 0) {
    mysql_logger()->error("Failed to enable MySQL autocommit, errno: {}", mysql_errno(_conn));
    invalidate();
    return false;
  }
  _connected = true;
  return true;
}

bool DbConnection::ping() {
  return _connected && _conn != nullptr && mysql_ping(_conn) == 0;
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
  return execute_prepared(sql, params).success;
}

DbExecuteResult DbConnection::execute_prepared(const std::string& sql,
                                              const std::vector<DbValue>& params) {
  DbExecuteResult result;
  if (!_connected || _conn == nullptr) {
    result.error_code = CR_SERVER_GONE_ERROR;
    return result;
  }
  StatementHandle stmt(prepare_and_execute(_conn, sql, params, false, &result.error_code),
                       &mysql_stmt_close);
  if (!stmt) {
    return result;
  }
  result.success = true;
  result.affected_rows = static_cast<std::uint64_t>(mysql_stmt_affected_rows(stmt.get()));
  result.insert_id = static_cast<std::uint64_t>(mysql_stmt_insert_id(stmt.get()));
  return result;
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

bool DbConnection::begin_transaction() noexcept {
  if (!_connected || _conn == nullptr || _transaction_active) {
    return false;
  }
  if (mysql_query(_conn, "START TRANSACTION") != 0) {
    // 服务端可能已经开启事务，但响应未到达客户端。
    invalidate();
    return false;
  }
  _transaction_active = true;
  return true;
}

DbCommitStatus DbConnection::commit_transaction() noexcept {
  if (!_transaction_active) {
    return DbCommitStatus::k_not_active;
  }
  if (!_connected || _conn == nullptr ||
      mysql_query(_conn, "COMMIT AND NO CHAIN NO RELEASE") != 0) {
    // 没有收到提交确认，不能认定事务一定没有提交。
    invalidate();
    return DbCommitStatus::k_outcome_unknown;
  }
  _transaction_active = false;
  return DbCommitStatus::k_committed;
}

bool DbConnection::rollback_transaction() noexcept {
  if (!_transaction_active) {
    return true;
  }
  if (!_connected || _conn == nullptr ||
      mysql_query(_conn, "ROLLBACK AND NO CHAIN NO RELEASE") != 0) {
    invalidate();
    return false;
  }
  _transaction_active = false;
  return true;
}

DbTransaction::DbTransaction(DbConnection& connection) noexcept
    : connection_(connection)
    , active_(connection_.begin_transaction()) {}

DbTransaction::~DbTransaction() noexcept {
  rollback();
}

DbCommitStatus DbTransaction::commit() noexcept {
  if (!active_) {
    return DbCommitStatus::k_not_active;
  }
  const auto result = connection_.commit_transaction();
  active_ = false;
  return result;
}

bool DbTransaction::rollback() noexcept {
  if (!active_) {
    return true;
  }
  const bool result = connection_.rollback_transaction();
  active_ = false;
  return result;
}

}  // namespace TLSS::BASE
