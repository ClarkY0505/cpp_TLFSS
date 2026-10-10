#include "common/base/base_logger.h"
#include "common/base/mysql_connection.h"
#include "common/base/mysql_connection_pool.h"
#include "common/base/tlss_thread.h"

#include <dirent.h>
#include <mysql/mysql_com.h>
#include <sched.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
namespace TLSS::BASE {

namespace {
unsigned short parse_port(const std::string& value) {
  std::size_t pos = 0;

  unsigned long number = std::stoul(value, &pos);

  if (pos != value.size() || number > std::numeric_limits<unsigned short>::max() ||
      (!value.empty() && value[0] == '-')) {
    mysql_logger()->error("Invalid MySQL port: {}", value);
    std::abort();
  }

  return static_cast<unsigned short>(number);
}
}  // namespace

DbConnectionPool* DbConnectionPool::get_connection_pool() {
  static DbConnectionPool pool;
  return &pool;
}

bool DbConnectionPool::load_config_file() {
  namespace fs = std::filesystem;

  std::error_code ec;
  const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  if (ec) {
    mysql_logger()->error("Unable to locate the executable file : {}", ec.message());
    return false;
  }

  const fs::path config_path = exe.parent_path().parent_path() / "sql" / "mysql.cnf";

  std::ifstream config(config_path);
  if (!config.is_open()) {
    mysql_logger()->error("Unable to open MySQL configuration : {}", config_path.string());
    return false;
  }

  std::string line;
  while (std::getline(config, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }

    const std::size_t pos = line.find('=');
    if (pos == std::string::npos) {
      continue;
    }

    std::string key = line.substr(0, pos);
    std::string value = line.substr(pos + 1);
    if (!set_variable(key, value)) {
      return false;
    }
  }
  if (config.bad()) {
    mysql_logger()->error("Unable to read MySQL configuration : {}", config_path.string());
    return false;
  }
  return true;
}

bool DbConnectionPool::set_variable(const std::string& key, const std::string& value) {
  if (key == "ip") {
    _ip = value;
  } else if (key == "port") {
    _port = parse_port(value);
  } else if (key == "username") {
    _username = value;
  } else if (key == "password") {
    _password = value;
  } else if (key == "initSize") {
    _init_size = std::stoi(value);
  } else if (key == "maxSize") {
    _max_size = std::stoi(value);
  } else if (key == "maxIdleTime") {
    _max_idle_time = std::stoi(value);
  } else if (key == "ConnectionTimeOut") {
    _connection_timeout = std::stoi(value);
  } else {
    mysql_logger()->error("set connection pool config failed");
    return false;
  }
  return true;
}

DbConnectionPool::DbConnectionPool()
    : _ip("127.0.0.1")
    , _port(3306)
    , _username("root")
    , _password("123456")
    , _init_size(10)
    , _max_size(1024)
    , _max_idle_time(60)
    , _connection_timeout(100)
    , _produce_thread(std::bind(&DbConnectionPool::produce_connection_task, this),
                      "ConnectionPoolProduceThread")
    , _scanner_thread(std::bind(&DbConnectionPool::scanner_connection, this),
                      "ConnectionPoolScannerThread")
    , _connection_cnt(0)
    , _stopping(false) {
  if (!load_config_file()) {
    mysql_logger()->error("DbConnectionPool init failed");
    std::abort();
  }
  //
  // 创建初始连接数量
  for (int i = 0; i < _init_size; ++i) {
    auto p = std::make_unique<DbConnection>();
    if (!p->connect(_ip, _port, _username, _password, "tlss_db")) {
      std::abort();
    }
    p->refresh_alive_time();
    _connection_queue.push(std::move(p));
    _connection_cnt.fetch_add(1);
  }

  try {
    // 启动生产和扫描线程；若第二个线程启动失败，先停止已经运行的线程。
    _produce_thread.start();
    _scanner_thread.start();
  } catch (...) {
    stop_background_threads();
    throw;
  }
}

void DbConnectionPool::scanner_connection() {
  for (;;) {
    // 扫描整个队列，释放多于的连接
    std::vector<std::unique_ptr<DbConnection>> expired;
    {
      std::unique_lock<std::mutex> lock(_mutex);
      // 析构时的通知可立即结束扫描，不必等满空闲扫描周期。
      if (_cond.wait_for(lock, std::chrono::seconds(_max_idle_time),
                         [this]() { return _stopping; })) {
        return;
      }
      const auto max_idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::seconds(_max_idle_time))
                                   .count();
      while (_connection_cnt > _init_size && !_connection_queue.empty()) {
        auto& p = _connection_queue.front();
        if (p->get_alive_time() < max_idle_ms) {
          // 队头时间如果没有超过，其他时间肯定没有超过
          break;
        }
        expired.push_back(std::move(p));
        _connection_queue.pop();
        --_connection_cnt;
      }
    }
    if (!expired.empty()) {
      _cond.notify_all();
    }
  }
}

void DbConnectionPool::produce_connection_task() {
  auto retry_delay = std::chrono::milliseconds(100);
  const auto max_retry_delay = std::chrono::milliseconds(5000);
  for (;;) {
    std::unique_lock<std::mutex> lock(_mutex);
    _cond.wait(lock, [this]() {
      return _stopping || (_connection_queue.empty() &&
                           _connection_cnt.load(std::memory_order_relaxed) < _max_size);
    });
    if (_stopping) {
      return;
    }
    // 预留容量，连接过程不占用队列互斥锁。
    _connection_cnt.fetch_add(1);
    lock.unlock();

    auto p = std::make_unique<DbConnection>();
    if (!p->connect(_ip, _port, _username, _password, "tlss_db")) {
      _connection_cnt.fetch_sub(1);
      mysql_logger()->warn("MySQL connection failed; retrying in {} ms", retry_delay.count());
      p.reset();
      _cond.notify_all();
      // 重试等待也能响应析构通知。
      std::unique_lock<std::mutex> retry_lock(_mutex);
      if (_cond.wait_for(retry_lock, retry_delay, [this]() { return _stopping; })) {
        return;
      }
      retry_delay = std::min(retry_delay * 2, max_retry_delay);
      continue;
    }
    retry_delay = std::chrono::milliseconds(100);
    p->refresh_alive_time();

    lock.lock();
    // 连接期间可能已开始析构；释放预留容量并丢弃这条连接。
    if (_stopping) {
      _connection_cnt.fetch_sub(1);
      return;
    }
    _connection_queue.push(std::move(p));
    lock.unlock();
    _cond.notify_all();
  }
}

void DbConnectionPool::stop_background_threads() {
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _stopping = true;
  }
  _cond.notify_all();

  if (_produce_thread.started()) {
    _produce_thread.join();
  }
  if (_scanner_thread.started()) {
    _scanner_thread.join();
  }
}

DbConnectionPool::~DbConnectionPool() {
  stop_background_threads();
}

std::shared_ptr<DbConnection> DbConnectionPool::get_connection() {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(_connection_timeout);
  std::unique_lock<std::mutex> lock(_mutex);
  for (;;) {
    if (!_cond.wait_until(lock, deadline, [this]() {
          return !_connection_queue.empty();
        })) {
      mysql_logger()->warn("get connection timeout");
      return nullptr;
    }

    auto connection = std::move(_connection_queue.front());
    _connection_queue.pop();
    const bool need_create =
        _connection_queue.empty() && _connection_cnt.load(std::memory_order_relaxed) < _max_size;
    lock.unlock();

    if (need_create) {
      _cond.notify_all();
    }

    if (connection->ping()) {
      return std::shared_ptr<DbConnection>(connection.release(), [this](DbConnection* conn) {
        {
          std::lock_guard<std::mutex> guard(_mutex);
          conn->refresh_alive_time();
          _connection_queue.push(std::unique_ptr<DbConnection>(conn));
        }
        _cond.notify_all();
      });
    }

    mysql_logger()->warn("Discarding invalid MySQL connection");
    connection.reset();
    _connection_cnt.fetch_sub(1);
    _cond.notify_all();
    lock.lock();
    if (std::chrono::steady_clock::now() >= deadline) {
      mysql_logger()->warn("get connection timeout");
      return nullptr;
    }
  }
}

}  // namespace TLSS::BASE
