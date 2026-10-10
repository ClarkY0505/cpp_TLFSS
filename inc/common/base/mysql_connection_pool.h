#ifndef __INC_COMMON_BASE_MYSQL_CONNECTION_POOL_H__
#define __INC_COMMON_BASE_MYSQL_CONNECTION_POOL_H__
#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include "common/NoCopy.h"
#include "common/base/tlss_thread.h"

namespace TLSS::BASE {
class DbConnection;
class DbConnectionPool;

// unique_ptr 的自定义删除器：句柄销毁或 reset() 时执行连接归还逻辑。
struct DbConnectionDeleter {
  // 指向所属连接池；池的析构会等待已借出的连接归还。
  DbConnectionPool* pool = nullptr;
  // 有所属连接池时归还连接，否则直接销毁；空指针无需处理。
  void operator()(DbConnection* conn) const noexcept;
};

// 连接借用句柄只能移动，不能复制；借出期间由该句柄独占管理连接。
using DbConnectionHandle = std::unique_ptr<DbConnection, DbConnectionDeleter>;

class DbConnectionPool : public NoCopy {
 public:
  // 首次调用构造局部静态单例，后续调用返回同一个连接池。
  static DbConnectionPool* get_connection_pool();
  // 等待可用连接并检查有效性；超时或池正在关闭时返回空句柄。
  // 成功借出的连接在句柄销毁时自动归还，业务应在程序退出前释放句柄。
  DbConnectionHandle get_connection();
  // 停止新借用并等待后台线程与已借出的连接；超时返回 false，池继续存活。
  // 调用方须先停止业务线程；重复调用安全，成功后也不再重新开放借用。
  bool shutdown(std::chrono::milliseconds timeout = std::chrono::seconds(30));

  // 禁止复制，避免复制线程、队列和连接所有权。
  /* DbConnectionPool(const DbConnectionPool&) = delete; */
  /* DbConnectionPool& operator=(const DbConnectionPool&) = delete; */

 private:
  // 允许删除器调用私有的归还接口，业务通过句柄自动完成归还。
  friend struct DbConnectionDeleter;

  // 加载配置、创建初始连接，然后启动生产和空闲回收线程。
  DbConnectionPool();
  // 停止后台线程，等待已登记的获取调用和借出连接结束，再销毁成员。
  ~DbConnectionPool();

  // 从可执行文件所在目录的上一级读取 sql/mysql.cnf。
  bool load_config_file();
  // 解析完成后集中检查参数之间的约束，避免用错误配置启动线程。
  bool validate_config() const;
  // 将一个配置键值写入对应成员；遇到未知键返回 false。
  bool set_variable(const std::string& key, const std::string& value);

  // 空闲队列为空且总数未达上限时创建连接，失败后延迟重试。
  void produce_connection_task();
  // 定期从空闲队列头回收超时连接，保留至少 _init_size 的总容量。
  void scanner_connection();
  // 设置关闭标志，唤醒等待者，并 join 已启动的后台线程。
  void stop_background_threads();
  // 接管归还连接；正常运行时入队，关闭期间直接销毁。
  void release_connection(DbConnection* conn) noexcept;

  // 数据库地址和认证信息；构造时给默认值，配置文件可覆盖。
  std::string _ip;
  unsigned short _port;
  std::string _username;
  std::string _password;
  int _init_size;           // 初始连接数量，也是空闲回收保留的总容量下限。
  int _max_size;            // 连接总数上限，包含借出连接和正在创建的容量。
  int _max_idle_time;       // 空闲超时及扫描间隔，单位：秒。
  int _connection_timeout; // 获取连接的条件等待期限，单位：毫秒。
  int _connect_timeout;    // MySQL 建连超时，单位：秒。
  int _read_timeout;       // MySQL 单次读取超时，单位：秒。
  int _write_timeout;      // MySQL 单次写入超时，单位：秒。

  // 线程回调会访问 this，因此池销毁成员前必须等待它们结束。
  Thread _produce_thread;
  Thread _scanner_thread;

  std::queue<std::unique_ptr<DbConnection>> _connection_queue; // 独占持有空闲连接。
  std::mutex _mutex; // 保护队列、关闭标志和下方两个生命周期计数。
  std::condition_variable _cond; // 用于获取、生产、扫描、重试和析构等待。
  std::mutex _shutdown_mutex; // 串行化停止操作，防止重复 join 同一个线程。
  std::atomic<int> _connection_cnt; // 总容量计数，生产失败时也会在锁外修改。
  bool _stopping; // 在 _mutex 下访问；关闭后不再借出或归还入队。
  bool _threads_joined; // 在 _shutdown_mutex 下访问；标记后台线程已完成 join。
  std::size_t _borrowed_count; // 在 _mutex 下访问；包含取出后正在 ping 的连接。
  std::size_t _active_getters; // 在 _mutex 下访问；已登记且尚未退出的获取调用。
};
}  // namespace TLSS::BASE
#endif  // __INC_COMMON_BASE_MYSQL_CONNECTION_POOL_H__
