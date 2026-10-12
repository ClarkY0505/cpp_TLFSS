/* #include "common/base/mysql_connection_pool.h" */
/* #include "common/base/tlss_thread_pool.h" */
#include "ftp/context.h"
#include "ftp/server.h"
#include "ftp/service_logger.h"

#include <cstdlib>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
namespace TLSS::FileStorage {

namespace {
enum HandleCommand { cd, ls, pwd, mkdir, download, uoload };
}

StorageService::StorageService(NET::EventLoop* loop, const NET::InetAddress& addr,
                               const std::string& name, int subloop_size,
                               int thread_pool_threads_num, int thrad_pool_queue_max_size)
    : _thread_pool_threads_num(thread_pool_threads_num)
    , _loop(loop)
    , _tcp_server(_loop, addr, name)
    , _db_conn_pool(BASE::DbConnectionPool::get_connection_pool())
    , _th_pool("tlss_worker")
    , _storage_engine(*_db_conn_pool) {
  _tcp_server.set_connection_cb(
      std::bind(&StorageService::on_connection, this, std::placeholders::_1));
  _tcp_server.set_message_cb(std::bind(&StorageService::on_message, this, std::placeholders::_1,
                                       std::placeholders::_2, std::placeholders::_3));
  _tcp_server.set_thread_num(subloop_size);
  _th_pool.set_max_queue_size(thrad_pool_queue_max_size);
  _th_pool.set_max_wait_queue_size(128);
}

StorageService::~StorageService() {
  // 主循环停止后无法再执行停机回调；运行过的服务必须在析构前显式 stop。
  if (_started && !_stopped) {
    std::terminate();
  }
  if (!_started) {
    // 构造完成但未启动时，仍需释放已初始化的线程池和数据库资源。
    _th_pool.stop();
    _db_conn_pool->shutdown();
  }
}

void StorageService::start() {
  // 先准备业务线程，再开放监听；停机时按相反顺序回收资源。
  _th_pool.start(_thread_pool_threads_num);
  _tcp_server.start();
  _started = true;
}

bool StorageService::stop(std::chrono::milliseconds grace_period) {
  // 停机过程等待主循环回调，因此不能在主循环线程内阻塞调用。
  if (_loop->is_in_loop_thread()) {
    service_logger()->error("StorageService::stop must run outside the base EventLoop thread");
    std::abort();
  }
  if (grace_period < std::chrono::milliseconds::zero()) {
    service_logger()->error("grace period must be nonnegative");
    std::abort();
  }

  std::unique_lock<std::mutex> lock(_stop_mutex);
  // 多个调用者只执行一次停机，其余调用者等待并共享最终结果。
  if (_stop_started) {
    _stop_done.wait(lock, [this] {
      return _stopped;
    });
    return _stop_succeeded;
  }
  _stop_started = true;
  _stopping.store(true);
  lock.unlock();

  // 关闭监听并暂停现有连接读取；屏障保证各 IO loop 已处理暂停请求。
  auto paused = std::make_shared<std::promise<void>>();
  auto paused_future = paused->get_future();
  _tcp_server.stop_accepting([paused] {
    paused->set_value();
  });
  paused_future.wait();

  // 所有 IO 线程已越过停机屏障，现有任务仍能使用网络及数据库资源。
  _th_pool.stop();

  // 任务结束后半关闭连接写端，等待对端关闭；超时则强制清理剩余连接。
  auto drained = std::make_shared<std::promise<void>>();
  auto drained_future = drained->get_future();
  _tcp_server.shutdown_connections([drained] {
    drained->set_value();
  });
  if (drained_future.wait_for(grace_period) == std::future_status::timeout) {
    _tcp_server.force_close_connections();
  }
  drained_future.wait();

  // 连接清理完成后再回收 IO 线程和数据库，最后退出主循环。
  _tcp_server.stop_io_threads();
  const bool db_stopped = _db_conn_pool->shutdown();
  _loop->quit();

  // 唤醒同时调用 stop 的线程，并返回数据库连接池的关闭结果。
  lock.lock();
  _stop_succeeded = db_stopped;
  _stopped = true;
  lock.unlock();
  _stop_done.notify_all();
  return db_stopped;
}

void StorageService::on_connection(const NET::TcpConnectionPtr& conn) {
  if (!conn->connected()) {
    service_logger()->info("conn down : {}", conn->peer_addr().to_ip_port());
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    _sessions.erase(conn.get());
    return;
  }

  // 连接背压
  conn->set_high_water_mark(256ULL * 1024);
  conn->set_high_water_mark_cb([](const NET::TcpConnectionPtr& c, std::size_t) {
    c->stop_read();
  });
  conn->set_write_complete_cb([](const NET::TcpConnectionPtr& c) {
    c->start_read();
  });

  auto session = std::make_shared<ConnectionSession>();
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    _sessions.emplace(conn.get(), session);
  }
  service_logger()->info("conn up : {}", conn->peer_addr().to_ip_port());
  conn->send("Please enter your username.\r\n");
  conn->send("Username:");
}

void StorageService::on_message(const NET::TcpConnectionPtr& conn, NET::Buffer* buf,
                                TIME::Timestamp timestamp) {
  (void)timestamp;
  // 停机开始后不再接受命令；清空缓存避免遗留数据再次被解析。
  if (_stopping.load()) {
    buf->retrieve_all();
    return;
  }

  constexpr std::size_t k_max_command_bytes = 4096;

  while (conn->connected()) {
    const char* begin = buf->peek();
    const char* end = begin + buf->readable_bytes();
    const char* delimiter = std::find(begin, end, '\n');

    if (delimiter == end) {
      // 允许末尾的 '\r' 等待下次读取补上 '\n'。
      if (buf->readable_bytes() > k_max_command_bytes + 1) {
        buf->retrieve_all();
        conn->force_close();
      }
      return;
    }

    const std::size_t line_length = static_cast<std::size_t>(delimiter - begin);
    const std::size_t length =
        line_length > 0 && delimiter[-1] == '\r' ? line_length - 1 : line_length;
    if (length > k_max_command_bytes) {
      buf->retrieve_all();
      conn->force_close();
      return;
    }

    std::string command(begin, length);
    buf->retrieve(line_length + 1);

    std::shared_ptr<ConnectionSession> session;
    {
      std::lock_guard<std::mutex> lock(sessions_mutex_);
      auto it = _sessions.find(conn.get());
      if (it == _sessions.end()) {
        return;
      }
      session = it->second;
    }

    if (command == "q" || command == "quit") {
      conn->send("bye!\r\n");
      conn->shutdow();
      return;
    }

    if (handle_login_command(conn, session, command)) {
      continue;
    }

    conn->send(command + "\r\ntlss@" + session->name + ":" + session->current_directory + ">");
  }
}

// -------- private fun----------------------------------------------

bool StorageService::handle_login_command(const NET::TcpConnectionPtr& conn,
                                          const std::shared_ptr<ConnectionSession>& session,
                                          const std::string& command) {
  if (session->login_state == LoginState::k_verifying) {
    conn->send("Please wait.\r\n");
    return true;
  }

  if (session->login_state == LoginState::k_await_username) {
    session->name = command;
    session->login_state = LoginState::k_verifying;
    auto query_session = std::make_shared<ConnectionSession>(*session);

    // 原生线程
    /* bool accepted = */
    /*     _th_pool.run(std::bind(&StorageService::lookup_username, this, query_session, */
    /*                            std::weak_ptr<ConnectionSession>(session), */
    /*                            std::weak_ptr<NET::TcpConnection>(conn), conn->get_loop())); */

    // 异步任务线程 225-240
    auto original = std::weak_ptr<ConnectionSession>(session);
    auto connection = std::weak_ptr<NET::TcpConnection>(conn);
    auto* io_loop = conn->get_loop();

    auto task = std::bind(&StorageService::lookup_username, this, query_session, original,
                          connection, io_loop);

    // 超时回调由线程池的计时线程调用，因此把 session 操作投递回 IO 线程。
    auto on_timeout = [this, original, connection, io_loop] {
      io_loop->queue_in_loop(
          std::bind(&StorageService::finish_username_wait_timeout, this, original, connection));
    };

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

    const bool accepted = _th_pool.submit_async(std::move(task), deadline, std::move(on_timeout));

    if (!accepted) {
      session->name.clear();
      session->login_state = LoginState::k_await_username;
      conn->send("Service unavailable.\r\nUsername:");
    }
    return true;
  }

  if (session->login_state == LoginState::k_await_password) {
    if (command == session->pass) {
      session->login_state = LoginState::k_authenticated;
      session->pass.clear();
      conn->send("Login successful.\r\ntlss@" + session->name + ":" + session->current_directory +
                 ">");
    } else {
      conn->send("Invalid password.\r\nPassword:");
    }
    return true;
  }

  return false;  // 已认证，交给后续命令处理
}

void StorageService::lookup_username(std::shared_ptr<ConnectionSession> query_session,
                                     std::weak_ptr<ConnectionSession> original,
                                     std::weak_ptr<NET::TcpConnection> connection,
                                     NET::EventLoop* io_loop) {
  StorageContext<std::shared_ptr<ConnectionSession>> result;
  result = _storage_engine.find_user_by_name(query_session);

  io_loop->queue_in_loop(std::bind(&StorageService::finish_username_lookup, this,
                                   std::move(original), std::move(connection), std::move(result)));
}

void StorageService::finish_username_lookup(
    std::weak_ptr<ConnectionSession>& original, std::weak_ptr<NET::TcpConnection>& connection,
    StorageContext<std::shared_ptr<ConnectionSession>>& result) {
  auto session = original.lock();
  auto conn = connection.lock();
  if (!session || !conn || !conn->connected()) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = _sessions.find(conn.get());
    if (it == _sessions.end() || it->second != session) {
      return;
    }
  }
  if (session->login_state != LoginState::k_verifying) {
    return;
  }

  if (result.success && result.result) {
    session->user_id = result.result->user_id;
    session->salt = result.result->salt;
    session->pass = result.result->pass;
    session->login_state = LoginState::k_await_password;
    conn->send("Password:");
  } else {
    session->name.clear();
    session->user_id = 0;
    session->login_state = LoginState::k_await_username;
    conn->send("Username not found.\r\n");
    conn->send("Username:");
  }
}

// 异步线程超时处理
void StorageService::finish_username_wait_timeout(std::weak_ptr<ConnectionSession> original,
                                                  std::weak_ptr<NET::TcpConnection> connection) {
  auto session = original.lock();
  auto conn = connection.lock();
  if (!session || !conn || !conn->connected()) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = _sessions.find(conn.get());
    if (it == _sessions.end() || it->second != session) {
      return;
    }
  }

  if (session->login_state != LoginState::k_verifying) {
    return;
  }

  session->name.clear();
  session->user_id = 0;
  session->login_state = LoginState::k_await_username;
  conn->send("Server busy. Please retry.\r\nUsername:");
}

}  // namespace TLSS::FileStorage
