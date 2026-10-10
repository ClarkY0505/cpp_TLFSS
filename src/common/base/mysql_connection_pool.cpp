#include "common/base/base_logger.h"
#include "common/base/mysql_connection.h"
#include "common/base/mysql_connection_pool.h"
#include "common/base/tlss_thread.h"

#include <dirent.h>
#include <mysql/mysql_com.h>
#include <sched.h>
#include <unistd.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
namespace TLSS::BASE {

namespace {
// 作用域清理器：F 是可调用对象的类型，C++17 可从构造参数推导 lambda 类型。
// 保存清理动作，正常返回、提前返回或异常退栈时由析构函数统一执行。
template <typename F>
class ScopeExit {
 public:
  // 将传入的清理动作移入成员，此时仅保存它，不执行函数体。
  explicit ScopeExit(F fn)
      : _fn(std::move(fn)) {}
  // 禁止复制，避免同一份清理动作被多个对象重复执行。
  ScopeExit(const ScopeExit&) = delete;
  ScopeExit& operator=(const ScopeExit&) = delete;
  ~ScopeExit() {
    // 调用保存的 lambda；用于生命周期计数的清理动作应避免抛出异常。
    _fn();
  }

 private:
  F _fn;
};

bool parse_integer(const std::string& key, const std::string& value, int& target) {
  // from_chars 不跳过前导空白；要求整个值都是可表示为 int 的十进制整数。
  int parsed = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
    // 只报告配置键，避免把可能含敏感信息的配置值写进日志。
    mysql_logger()->error("Invalid MySQL configuration: {} must be an integer", key);
    return false;
  }
  target = parsed;
  return true;
}
}  // namespace

DbConnectionPool* DbConnectionPool::get_connection_pool() {
  // C++11 起局部静态对象的首次初始化是线程安全的；正常退出时自动析构。
  static DbConnectionPool pool;
  // 返回非拥有指针，调用方不能 delete 这个单例。
  return &pool;
}

void DbConnectionDeleter::operator()(DbConnection* conn) const noexcept {
  // 防御性处理空指针，无需归还或删除。
  if (conn == nullptr) {
    return;
  }
  if (pool != nullptr) {
    // 交给所属连接池决定重新入队，或在关闭期间销毁连接。
    pool->release_connection(conn);
  } else {
    // 没有关联连接池时直接释放，避免遗失连接所有权。
    delete conn;
  }
}

bool DbConnectionPool::load_config_file() {
  // 用别名简化后续路径操作。
  namespace fs = std::filesystem;

  // Linux 下通过 /proc/self/exe 定位程序，配置路径不依赖启动时的工作目录。
  std::error_code ec;
  const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  if (ec) {
    // 无法定位可执行文件时记录错误，由构造函数处理初始化失败。
    mysql_logger()->error("Unable to locate the executable file : {}", ec.message());
    return false;
  }

  // 按项目的 bin/程序 与 sql/mysql.cnf 布局寻找配置文件。
  const fs::path config_path = exe.parent_path().parent_path() / "config" / "mysql.cnf";

  // 文件流通过 RAII 关闭；打开失败时不继续解析。
  std::ifstream config(config_path);
  if (!config.is_open()) {
    mysql_logger()->error("Unable to open MySQL configuration : {}", config_path.string());
    return false;
  }

  std::string line;
  // 逐行解析 key=value；当前实现直接使用原始文本，不裁剪两侧空白。
  while (std::getline(config, line)) {
    // 跳过空行和首字符为 # 的注释行。
    if (line.empty() || line[0] == '#') {
      continue;
    }

    // 以第一个等号分隔键值，没有等号的行不作为配置项处理。
    const std::size_t pos = line.find('=');
    if (pos == std::string::npos) {
      continue;
    }

    // 等号之后的全部内容作为值，值中后续的等号会被保留。
    std::string key = line.substr(0, pos);
    std::string value = line.substr(pos + 1);
    if (!set_variable(key, value)) {
      // 遇到未知配置键时终止加载，避免静默忽略配置错误。
      return false;
    }
  }
  // 正常到达 EOF 不算错误；badbit 表示读取过程中出现了流错误。
  if (config.bad()) {
    mysql_logger()->error("Unable to read MySQL configuration : {}", config_path.string());
    return false;
  }
  // 所有键完成解析后再检查跨字段约束；未配置的键沿用构造默认值。
  return validate_config();
}

bool DbConnectionPool::validate_config() const {
  // 空主机或用户名会让初始化时的建连参数不完整。
  if (_ip.empty() || _username.empty()) {
    mysql_logger()->error("Invalid MySQL configuration: ip and username must not be empty");
    return false;
  }
  // 保证初始建连数有效，且不会在启动时超过最大容量。
  if (_init_size < 1 || _max_size < _init_size) {
    mysql_logger()->error("Invalid MySQL configuration: require 1 <= initSize <= maxSize");
    return false;
  }
  // 零或负的扫描间隔会让扫描线程忙循环；获取连接的等待期限也必须为正。
  if (_max_idle_time <= 0 || _connection_timeout <= 0) {
    mysql_logger()->error(
        "Invalid MySQL configuration: maxIdleTime and ConnectionTimeOut must be positive");
    return false;
  }
  // 三种客户端超时都以秒计，必须为正，之后才可安全转换为 unsigned int。
  if (_connect_timeout <= 0 || _read_timeout <= 0 || _write_timeout <= 0) {
    mysql_logger()->error("Invalid MySQL configuration: network timeouts must be positive");
    return false;
  }
  return true;
}

bool DbConnectionPool::set_variable(const std::string& key, const std::string& value) {
  // 数字先解析到局部变量，只有完整解析并通过必要的范围检查才覆盖默认值。
  int parsed = 0;
  if (key == "ip") {
    // 数据库主机地址。
    _ip = value;
  } else if (key == "port") {
    // MySQL 端口只能取 1 到 65535；0 在客户端 API 中有默认端口含义。
    if (!parse_integer(key, value, parsed)) {
      return false;
    }
    if (parsed < 1 || parsed > std::numeric_limits<unsigned short>::max()) {
      mysql_logger()->error("Invalid MySQL configuration: port out of range");
      return false;
    }
    _port = static_cast<unsigned short>(parsed);
  } else if (key == "username") {
    // 数据库登录用户名。
    _username = value;
  } else if (key == "password") {
    // 数据库登录密码，不将其写入错误日志。
    _password = value;
  } else if (key == "initSize") {
    // 初始连接数，以及扫描回收时保留的总容量下限。
    return parse_integer(key, value, _init_size);
  } else if (key == "maxSize") {
    // 允许创建的连接总数上限。
    return parse_integer(key, value, _max_size);
  } else if (key == "maxIdleTime") {
    // 空闲超时及扫描周期，单位为秒。
    return parse_integer(key, value, _max_idle_time);
  } else if (key == "ConnectionTimeOut") {
    // 获取连接的条件等待期限，单位为毫秒。
    return parse_integer(key, value, _connection_timeout);
  } else if (key == "connectTimeout") {
    // 建连超时，单位为秒；交给 mysql_options() 设置。
    return parse_integer(key, value, _connect_timeout);
  } else if (key == "readTimeout") {
    // 网络读取超时，单位为秒，适用于 ping 与查询的读取阶段。
    return parse_integer(key, value, _read_timeout);
  } else if (key == "writeTimeout") {
    // 网络写入超时，单位为秒，适用于客户端向服务器发送请求。
    return parse_integer(key, value, _write_timeout);
  } else {
    // 未知键通常表示拼写或配置格式错误，交给加载函数返回失败。
    mysql_logger()->error("set connection pool config failed");
    return false;
  }
  // 已识别并写入该配置项。
  return true;
}

// 先建立默认配置和线程回调；Thread 构造只保存回调，start() 才启动线程。
// 容量、关闭标志和生命周期计数从空池状态开始。
DbConnectionPool::DbConnectionPool()
    : _ip("127.0.0.1")
    , _port(3306)
    , _username("root")
    , _password("123456")
    , _init_size(10)
    , _max_size(1024)
    , _max_idle_time(60)
    , _connection_timeout(100)
    , _connect_timeout(5)
    , _read_timeout(5)
    , _write_timeout(5)
    , _produce_thread(std::bind(&DbConnectionPool::produce_connection_task, this),
                      "ConnectionPoolProduceThread")
    , _scanner_thread(std::bind(&DbConnectionPool::scanner_connection, this),
                      "ConnectionPoolScannerThread")
    , _connection_cnt(0)
    , _stopping(false)
    , _threads_joined(false)
    , _borrowed_count(0)
    , _active_getters(0) {
  // 用配置文件覆盖默认值；加载失败时终止程序，不暴露未初始化完成的池。
  if (!load_config_file()) {
    mysql_logger()->error("DbConnectionPool init failed");
    std::abort();
  }
  // 此时尚无后台线程或借用者，可直接创建并填充初始空闲队列。
  for (int i = 0; i < _init_size; ++i) {
    // unique_ptr 接管新连接对象，构造或连接过程中无需手工 delete。
    auto p = std::make_unique<DbConnection>();
    // 当前连接使用固定数据库 tlss_db；初始连接失败按现有策略终止程序。
    if (!p->connect(_ip, _port, _username, _password, "tlss_db",
                    static_cast<unsigned int>(_connect_timeout),
                    static_cast<unsigned int>(_read_timeout),
                    static_cast<unsigned int>(_write_timeout))) {
      std::abort();
    }
    // 从创建成功时开始计算空闲时间，再把所有权移交给空闲队列。
    p->refresh_alive_time();
    _connection_queue.push(std::move(p));
    // 计入总容量；正常借出和重新入队保留该计数，销毁连接时才扣减。
    _connection_cnt.fetch_add(1);
  }

  try {
    // 启动生产和扫描线程；若第二个线程启动失败，先停止已经运行的线程。
    _produce_thread.start();
    _scanner_thread.start();
  } catch (...) {
    // 构造失败时不会调用本类析构函数，因此必须在这里清理已启动的线程。
    stop_background_threads();
    throw;
  }
}

void DbConnectionPool::scanner_connection() {
  // 持续等待扫描周期；收到关闭请求时退出，由析构流程 join。
  for (;;) {
    // 暂存被回收的连接，等退出互斥区后再销毁，避免关闭连接长期占锁。
    std::vector<std::unique_ptr<DbConnection>> expired;
    {
      std::unique_lock<std::mutex> lock(_mutex);
      // wait_for 等待时释放锁，返回时重新持锁；谓词为真才提前结束等待。
      // 普通归还通知不终止扫描周期，关闭通知则可立即结束等待。
      if (_cond.wait_for(lock, std::chrono::seconds(_max_idle_time), [this]() {
            return _stopping;
          })) {
        return;
      }
      // 配置以秒计，连接的空闲时长以毫秒计，比较前统一单位。
      const auto max_idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::seconds(_max_idle_time))
                                   .count();
      // 只处理空闲队列，不触碰借出连接；总容量降至初始值后停止回收。
      while (_connection_cnt > _init_size && !_connection_queue.empty()) {
        auto& p = _connection_queue.front();
        if (p->get_alive_time() < max_idle_ms) {
          // 本轮只回收队头连续过期的连接，遇到未过期队头就结束扫描。
          break;
        }
        // 所有权转移到局部容器后出队，并同步减少总容量。
        expired.push_back(std::move(p));
        _connection_queue.pop();
        --_connection_cnt;
      }
    }
    if (!expired.empty()) {
      // 容量有变化时唤醒等待者，让生产线程重新检查是否需要补充连接。
      _cond.notify_all();
    }
    // expired 在本轮结束时析构，此时不持有队列互斥锁。
  }
}

void DbConnectionPool::produce_connection_task() {
  // 连接失败后从 100ms 开始退避，最多延迟 5s，避免频繁重连。
  auto retry_delay = std::chrono::milliseconds(100);
  const auto max_retry_delay = std::chrono::milliseconds(5000);
  for (;;) {
    std::unique_lock<std::mutex> lock(_mutex);
    // 没有补充需求时释放锁等待；关闭，或空闲队列为空且仍有容量时醒来。
    _cond.wait(lock, [this]() {
      return _stopping || (_connection_queue.empty() &&
                           _connection_cnt.load(std::memory_order_relaxed) < _max_size);
    });
    if (_stopping) {
      // 关闭优先于补充连接，退出后由停止流程 join。
      return;
    }
    // 预留容量，连接过程不占用队列互斥锁。
    _connection_cnt.fetch_add(1);
    lock.unlock();

    auto p = std::make_unique<DbConnection>();
    // 网络连接在锁外执行，让其他线程仍能获取、归还连接或发出关闭请求。
    if (!p->connect(_ip, _port, _username, _password, "tlss_db",
                    static_cast<unsigned int>(_connect_timeout),
                    static_cast<unsigned int>(_read_timeout),
                    static_cast<unsigned int>(_write_timeout))) {
      // 失败连接没有成为池中资源，撤销预留总容量并销毁临时对象。
      _connection_cnt.fetch_sub(1);
      mysql_logger()->warn("MySQL connection failed; retrying in {} ms", retry_delay.count());
      p.reset();
      // 通知其他等待者容量已更新，然后进入可被关闭请求中断的重试等待。
      _cond.notify_all();
      // 重试等待也能响应析构通知。
      std::unique_lock<std::mutex> retry_lock(_mutex);
      if (_cond.wait_for(retry_lock, retry_delay, [this]() {
            return _stopping;
          })) {
        return;
      }
      // 每次失败将下次等待时间翻倍，但不超过设定上限。
      retry_delay = std::min(retry_delay * 2, max_retry_delay);
      continue;
    }
    // 创建成功后恢复初始重试间隔，并记录新连接开始空闲的时间。
    retry_delay = std::chrono::milliseconds(100);
    p->refresh_alive_time();

    // 重新持锁后才能检查关闭状态并操作队列。
    lock.lock();
    // 连接期间可能已开始析构；释放预留容量并丢弃这条连接。
    if (_stopping) {
      _connection_cnt.fetch_sub(1);
      return;
    }
    // 将新连接所有权移交给队列；此前预留的总容量无需再次增加。
    _connection_queue.push(std::move(p));
    lock.unlock();
    // 解锁后通知获取连接的线程，避免它们醒来后立即竞争仍被占用的锁。
    _cond.notify_all();
  }
}

void DbConnectionPool::stop_background_threads() {
  // 关闭可能显式调用多次，也会从析构函数再次调用；只允许一次 join。
  std::lock_guard<std::mutex> shutdown_lock(_shutdown_mutex);
  if (_threads_joined) {
    return;
  }
  {
    // 关闭标志与等待谓词在同一个互斥锁下访问，避免状态读写竞争。
    std::lock_guard<std::mutex> lock(_mutex);
    _stopping = true;
  }
  // 唤醒获取、生产、扫描以及重试等待，使它们重新检查关闭标志。
  _cond.notify_all();

  // 不持队列锁进行 join，否则后台线程可能无法重新获取锁并退出。
  // 构造中途失败时部分线程可能未启动，因此只等待已启动的线程。
  if (_produce_thread.started()) {
    _produce_thread.join();
  }
  if (_scanner_thread.started()) {
    _scanner_thread.join();
  }
  // join 完成后不再有后台线程访问 this；后续关闭调用可以直接跳过。
  _threads_joined = true;
}

bool DbConnectionPool::shutdown(std::chrono::milliseconds timeout) {
  // 截止点从调用开始计时；join 花费的时间也计入等待预算。
  // std::thread::join() 本身无法中断，实际耗时仍取决于 MySQL 调用能否返回。
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  // 先阻止新借用，唤醒等待者，并完成后台线程的 join。
  stop_background_threads();
  std::unique_lock<std::mutex> lock(_mutex);
  // 若期限到达仍有句柄或获取调用，返回 false，保留所有池成员供后续归还使用。
  // wait_until 会释放锁，使获取调用的清理器和连接归还器能够更新计数。
  return _cond.wait_until(lock, deadline, [this]() {
    return _borrowed_count == 0 && _active_getters == 0;
  });
}

DbConnectionPool::~DbConnectionPool() {
  // 静态单例退出时再确认所有句柄归还；已显式关闭时 stop 可重复调用。
  if (!shutdown(std::chrono::seconds(30))) {
    // 超时后直接销毁池会使仍存活的句柄持有悬空 pool 指针，因此明确失败终止。
    mysql_logger()->error("DbConnectionPool shutdown timed out");
    std::fputs("DbConnectionPool shutdown timed out: outstanding connections or getters\n", stderr);
    std::abort();
  }
  // 成功后局部锁已释放，再由成员析构销毁空闲队列及同步对象。
}

void DbConnectionPool::release_connection(DbConnection* conn) noexcept {
  // 防御性处理空指针，不修改借出计数。
  if (conn == nullptr) {
    return;
  }

  // 立即接管原始指针：若没有成功入队，局部 unique_ptr 负责关闭连接。
  std::unique_ptr<DbConnection> owned(conn);
  {
    std::lock_guard<std::mutex> lock(_mutex);
    // 记录关闭期间是否跳过入队，以决定是否减少总容量。
    bool requeued = false;
    if (!_stopping) {
      // 重新计算空闲起点，然后将所有权从局部变量转交给空闲队列。
      owned->refresh_alive_time();
      _connection_queue.push(std::move(owned));
      requeued = true;
    }
    // 无论归还入队还是销毁，这条连接都不再属于借出状态。
    --_borrowed_count;
    if (!requeued) {
      // 关闭期间没有重新入队时，连接退出池并撤销总容量计数。
      _connection_cnt.fetch_sub(1);
    }
    // 在释放锁前通知，析构线程才能在本次归还不再访问池时继续。
    _cond.notify_all();
  }
  // 未入队连接在锁外销毁；此后不再访问池，析构等待可以继续。
}

DbConnectionHandle DbConnectionPool::get_connection() {
  // 使用单调时钟和固定截止点；丢弃坏连接后的重试不重新计算等待期限。
  // 该期限约束条件变量等待，不能中断后续 MySQL ping 网络调用。
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(_connection_timeout);
  // 只有成功持锁并增加活跃调用计数后才登记，避免计数尚未增加就被扣减。
  bool registered = false;
  // 清理器声明在 lock 前：离开作用域时 lock 先析构，再执行清理器。
  // 因此下面的清理动作可重新加锁，不会对同一互斥锁重复加锁而死锁。
  ScopeExit finish_getter([this, &registered]() {
    if (registered) {
      // 所有返回路径及异常退栈都会注销该获取调用，并唤醒析构等待者。
      std::lock_guard<std::mutex> guard(_mutex);
      --_active_getters;
      _cond.notify_all();
    }
  });
  std::unique_lock<std::mutex> lock(_mutex);
  // 在锁内登记本次调用，析构必须等待它完成清理后才能销毁同步对象。
  ++_active_getters;
  registered = true;
  // 取出的连接可能已失效，因此在同一等待期限内继续尝试其他连接。
  for (;;) {
    // 队列为空时释放锁等待；有空闲连接或关闭标志置位时重新持锁返回。
    // 谓词会重新检查状态，过滤虚假唤醒及其他用途的通知。
    if (!_cond.wait_until(lock, deadline, [this]() {
          return _stopping || !_connection_queue.empty();
        })) {
      // 截止点到达仍无可借用连接，返回空句柄；清理器负责注销调用。
      mysql_logger()->warn("get connection timeout");
      return {};
    }
    if (_stopping) {
      // 即使队列仍有连接，关闭后也不再借出。
      return {};
    }

    // 将队头连接的所有权移入局部 unique_ptr，再移除空的队列元素。
    auto connection = std::move(_connection_queue.front());
    _connection_queue.pop();
    // 在解锁和 ping 前计入借出状态，防止析构忽略这条正在检查的连接。
    ++_borrowed_count;
    // 若取走最后一条空闲连接且还有容量，通知生产线程补充连接。
    const bool need_create =
        _connection_queue.empty() && _connection_cnt.load(std::memory_order_relaxed) < _max_size;
    // ping 可能等待网络响应，必须在锁外执行以允许其他池操作继续。
    lock.unlock();

    if (need_create) {
      _cond.notify_all();
    }

    if (connection->ping()) {
      // release() 交出局部所有权，新句柄接管同一连接并绑定归还器。
      // 返回后 finish_getter 注销获取调用，借出计数保留至句柄归还时扣减。
      return DbConnectionHandle(connection.release(), DbConnectionDeleter{this});
    }

    mysql_logger()->warn("Discarding invalid MySQL connection");
    // 无效连接直接在锁外销毁，不通过归还器重新进入空闲队列。
    connection.reset();
    lock.lock();
    // 重新持锁后撤销借出状态与总容量，为生产线程腾出创建容量。
    --_borrowed_count;
    _connection_cnt.fetch_sub(1);
    // 通知生产及析构等待者，随后检查是否还有时间重试。
    _cond.notify_all();
    if (std::chrono::steady_clock::now() >= deadline) {
      mysql_logger()->warn("get connection timeout");
      return {};
    }
  }
}

}  // namespace TLSS::BASE
