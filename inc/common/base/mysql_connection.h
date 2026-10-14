#ifndef __INC_COMMON_BASE_MYSQL_CONNECTION_H__
#define __INC_COMMON_BASE_MYSQL_CONNECTION_H__

#include "mysql/mysql.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace TLSS::BASE {
using DbValue = std::optional<std::string>;
using DbRow = std::vector<DbValue>;

struct DbQueryResult {
  bool success = false;
  std::vector<DbRow> rows;
};

struct DbExecuteResult {
    bool success = false;
    unsigned int error_code = 0;
    std::uint64_t affected_rows = 0;
    std::uint64_t insert_id = 0;
};

enum class DbCommitStatus {
    k_committed,
    k_not_active,
    k_outcome_unknown,
};

class DbTransaction;

class DbConnection {
 public:
  // 初始化数据库连接
  DbConnection();
  ~DbConnection();
  DbConnection(const DbConnection&) = delete;
  DbConnection& operator=(const DbConnection&) = delete;
  // 在建立连接前设置 MySQL 网络超时；三个可选参数的单位均为秒。
  // 未指定时使用 5 秒默认值，连接池可按配置显式传入其他值。
  bool connect(const std::string& ip, unsigned short port, const std::string& username,
               const std::string& password, const std::string& dbname,
               unsigned int connect_timeout_sec = 5, unsigned int read_timeout_sec = 5,
               unsigned int write_timeout_sec = 5);
  bool ping();
  // insert delete update
  bool update(const std::string& sql);
  //  select
  MYSQL_RES* query(const std::string& sql);

  // 参数化 SELECT：没有匹配记录时，success 为 true，rows 为空。
  DbQueryResult query_prepared(const std::string& sql, const std::vector<DbValue>& params = {});

  // 参数化 INSERT、UPDATE、DELETE，以及不返回结果集的语句。
  bool update_prepared(const std::string& sql, const std::vector<DbValue>& params = {});

  // 在关闭 statement 前保存错误码、自增 ID 和影响行数。
  DbExecuteResult execute_prepared(const std::string& sql,
                                  const std::vector<DbValue>& params = {});

  // 由持有连接的线程调用；不进行网络 IO，也不保证远端仍然存活。
  bool is_reusable() const noexcept {
    return _conn != nullptr && _connected && !_transaction_active;
  }

  // 刷新连接的起始空闲时间点
  void refresh_alive_time() {
    _alive_time = std::chrono::steady_clock::now();
  }
  // 返回存活时间
  std::chrono::milliseconds::rep get_alive_time() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 _alive_time)
        .count();
  }

 private:
  friend class DbTransaction;
  bool begin_transaction() noexcept;
  DbCommitStatus commit_transaction() noexcept;
  bool rollback_transaction() noexcept;
  void invalidate() noexcept;

  MYSQL* _conn;
  std::chrono::steady_clock::time_point _alive_time;
  bool _connected = false;
  bool _transaction_active = false;
};

// 必须比引用的连接更早析构，且与连接在同一线程使用。
// 事务内只使用事务表的 DML，不执行隐式提交的 DDL，也不混用原始事务 SQL。
class DbTransaction {
public:
    explicit DbTransaction(DbConnection& connection) noexcept;
    ~DbTransaction() noexcept;

    DbTransaction(const DbTransaction&) = delete;
    DbTransaction& operator=(const DbTransaction&) = delete;
    DbTransaction(DbTransaction&&) = delete;
    DbTransaction& operator=(DbTransaction&&) = delete;

    bool active() const noexcept { return active_; }
    DbCommitStatus commit() noexcept;
    bool rollback() noexcept;

private:
    DbConnection& connection_;
    bool active_ = false;
};
}  // namespace TLSS::BASE

#endif  // __INC_COMMON_BASE_MYSQL_CONNECTION_H__
