#include "common/base/tlss_thread_pool.h"
#include "common/net/callbacks.h"
#include "common/net/event_loop.h"
#include "common/net/tcp_connection.h"
#include "ftp/context.h"
#include "ftp/metadata_types.h"
#include "ftp/object_backfill.h"
#include "ftp/object_repository.h"
#include "ftp/server.h"
#include "ftp/service_logger.h"
#include "ftp/transfer_protocol.h"
#include "ftp/transfer_validation.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <future>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
/* #include <stdexcept> */
namespace TLSS::FileStorage {

namespace {
enum HandleCommand {
  login,
  signup,
  cd,
  ls,
  pwd,
  mkdir,
  rmdir,
  rm,
  rename,
  move,
  download,
  upload,
  unknown
};

void transfer_prompt(const NET::TcpConnectionPtr& conn,
                     const std::shared_ptr<ConnectionSession>& session) {
  conn->send("tlss@" + session->name + ":" + session->current_directory + ">");
}

std::optional<HandleCommand> string_to_command(std::string_view command) {
  if (command == "login") {
    return HandleCommand::login;
  }
  if (command == "signup") {
    return HandleCommand::signup;
  }
  if (command == "cd") {
    return HandleCommand::cd;
  }
  if (command == "ls") {
    return HandleCommand::ls;
  }
  if (command == "pwd") {
    return HandleCommand::pwd;
  }
  if (command == "mkdir") {
    return HandleCommand::mkdir;
  }
  if (command == "rmdir") {
    return HandleCommand::rmdir;
  }
  if (command == "rm") {
    return HandleCommand::rm;
  }
  if (command == "rename") {
    return HandleCommand::rename;
  }
  if (command == "move") {
    return HandleCommand::move;
  }
  if (command == "download") {
    return HandleCommand::download;
  }
  if (command == "upload") {
    return HandleCommand::upload;
  }

  return HandleCommand::unknown;
}

}  // namespace

StorageService::StorageService(NET::EventLoop* loop, const NET::InetAddress& addr,
                               const std::string& name, int subloop_size,
                               int thread_pool_threads_num, int thread_pool_queue_max_size,
                               std::shared_ptr<OBSERVABILITY::StorageMetrics> metrics,
                               std::shared_ptr<MetadataCache> cache)
    : _metrics(std::move(metrics))
    , _thread_pool_threads_num(thread_pool_threads_num)
    , _loop(loop)
    , _tcp_server(_loop, addr, name)
    , _db_conn_pool(BASE::DbConnectionPool::get_connection_pool())
    , _th_pool("tlss_worker")
    , _user_repository(*_db_conn_pool)
    , _auth_service(_user_repository)
    , _metadata_service(*_db_conn_pool, std::move(cache))
    , _object_gc(*_db_conn_pool, _object_storage, _object_coordinator)
    , _object_gc_worker(_object_gc)
    , _upload_engine(_th_pool, _disk_executor, _metadata_service, _object_storage,
                     _object_coordinator, _stopping)
    , _download_engine(_disk_executor, _metadata_service, _object_storage, _stopping) {
  _metadata_service.set_metrics(_metrics);
  _object_storage.set_metrics(_metrics);
  _db_conn_pool->set_metrics(_metrics);
  if (_metrics) {
    _metrics->set(OBSERVABILITY::Metric::service_state, 1);
  }
  _tcp_server.set_connection_cb(
      std::bind(&StorageService::on_connection, this, std::placeholders::_1));

  _tcp_server.set_message_cb(std::bind(&StorageService::on_message, this, std::placeholders::_1,
                                       std::placeholders::_2, std::placeholders::_3));

  _tcp_server.set_thread_num(subloop_size);
  _th_pool.set_max_queue_size(thread_pool_queue_max_size);
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
    _disk_executor.stop();
    _object_gc_worker.stop();
    _db_conn_pool->shutdown();
  }
}

void StorageService::sample_metrics() {
  if (!_metrics)
    return;
  const auto workers = _th_pool.snapshot();
  const auto database = _db_conn_pool->snapshot();
  const auto disk = _disk_executor.snapshot();
  const auto cache = _metadata_service.metadata_cache_stats();
  using OBSERVABILITY::Metric;
  _metrics->set(Metric::worker_queue, workers.queued);
  _metrics->set(Metric::worker_waiting, workers.waiting);
  _metrics->set(Metric::worker_rejected, workers.rejected);
  _metrics->set(Metric::worker_timeouts, workers.timeouts);
  _metrics->set(Metric::db_total, database.total);
  _metrics->set(Metric::db_idle, database.idle);
  _metrics->set(Metric::db_borrowed, database.borrowed);
  _metrics->set(Metric::db_waiters, database.waiters);
  _metrics->set(Metric::disk_active, disk.active);
  _metrics->set(Metric::disk_rejected, disk.rejected);
  _metrics->set(Metric::cache_requests, cache.directory_list.requests +
                                            cache.download_hash.requests + cache.cd_path.requests);
  _metrics->set(Metric::cache_hits,
                cache.directory_list.hits + cache.download_hash.hits + cache.cd_path.hits);
  _metrics->set(Metric::cache_loads,
                cache.directory_list.loads + cache.download_hash.loads + cache.cd_path.loads);
}

void StorageService::start() {
  // 先准备业务线程，再开放监听；停机时按相反顺序回收资源。
  _th_pool.start(_thread_pool_threads_num);
  // 这里可以设置异步磁盘任务线程池数量
  // _disk_executor.start(4);
  // 默认为2
  _disk_executor.start();
  if (!_object_gc_worker.start()) {
    throw std::runtime_error("Failed to start object GC worker");
  }
  _tcp_server.start();
  _started = true;
  if (_metrics)
    _metrics->set(OBSERVABILITY::Metric::service_state, 2);
}

bool StorageService::stop(std::chrono::milliseconds grace_period) {
  // 停机过程等待主循环回调，因此不能在主循环线程内阻塞调用
  if (_loop->is_in_loop_thread()) {
    service_logger()->error("StorageService::stop must run outside the base EventLoop thread");
    std::abort();
  }
  if (grace_period < std::chrono::milliseconds::zero()) {
    service_logger()->error("grace period must be nonnegative");
    std::abort();
  }

  std::unique_lock<std::mutex> lock(_stop_mutex);
  // 多个调用者只执行一次停机，其余调用者等待并共享最终结果
  if (_stop_started) {
    _stop_done.wait(lock, [this] {
      return _stopped;
    });
    return _stop_succeeded;
  }
  _stop_started = true;
  _stopping.store(true);
  if (_metrics)
    _metrics->set(OBSERVABILITY::Metric::service_state, 3);
  lock.unlock();

  // 关闭监听并暂停现有连接读取；屏障保证各 IO loop 已处理暂停请求
  auto paused = std::make_shared<std::promise<void>>();
  auto paused_future = paused->get_future();
  _tcp_server.stop_accepting([paused] {
    paused->set_value();
  });
  paused_future.wait();

  // 所有 IO 线程已越过停机屏障，现有任务仍能使用网络及数据库资源
  _th_pool.stop();
  // 磁盘任务仍可能访问数据库和向 IO loop 投递结果，必须先取消并等待收尾
  _disk_executor.stop();
  // 等待 GC 当前批次结束，后续不再访问数据库或对象目录
  _object_gc_worker.stop();

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
  if (_metrics)
    _metrics->set(OBSERVABILITY::Metric::service_state, 4);
  lock.unlock();
  _stop_done.notify_all();
  return db_stopped;
}

void StorageService::on_connection(const NET::TcpConnectionPtr& conn) {
  // 连接断开时，连接建立分支保留
  // 不会在 IO 线程中直接关闭临时文件
  // cancel() 会让工作线程负责收尾
  if (!conn->connected()) {
    service_logger()->info("conn down : {}", conn->peer_addr().to_ip_port());

    std::shared_ptr<ConnectionSession> session;

    {
      std::lock_guard<std::mutex> lock(sessions_mutex_);

      auto it = _sessions.find(conn.get());

      if (it != _sessions.end()) {
        session = std::move(it->second);
        _sessions.erase(it);
        if (_metrics)
          _metrics->subtract(OBSERVABILITY::Metric::connections);
      }
    }

    if (session && session->transfer)
      session->transfer->cancel();

    return;
  }

  // 设置连接背压
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
    const bool inserted = _sessions.emplace(conn.get(), session).second;
    if (inserted && _metrics)
      _metrics->add(OBSERVABILITY::Metric::connections);
  }
  service_logger()->info("conn up : {}", conn->peer_addr().to_ip_port());
  conn->send("Welcome to TLSS.\r\n");
  conn->send("Please enter login or signup:\r\ntlss>");
}

//
// 该方法实现时，没有引入传输状态
/* void StorageService::on_message(const NET::TcpConnectionPtr& conn, NET::Buffer* buf, */
/*                                 TIME::Timestamp timestamp) { */
/*   (void)timestamp; */
/*   // 停机开始后不再接受命令；清空缓存避免遗留数据再次被解析。 */
/*   if (_stopping.load()) { */
/*     buf->retrieve_all(); */
/*     return; */
/*   } */

/*   constexpr std::size_t k_max_command_bytes = 4096; */
/*   std::shared_ptr<ConnectionSession> session; */
/*   { */
/*     std::lock_guard<std::mutex> lock(sessions_mutex_); */
/*     auto it = _sessions.find(conn.get()); */
/*     if (it == _sessions.end()) { */
/*       return; */
/*     } */
/*     session = it->second; */
/*   } */

/*   while (conn->connected()) { */
/*     const char* begin = buf->peek(); */
/*     const char* end = begin + buf->readable_bytes(); */
/*     const char* delimiter = std::find(begin, end, '\n'); */

/*     if (delimiter == end) { */
/*       // 允许末尾的 '\r' 等待下次读取补上 '\n'。 */
/*       if (buf->readable_bytes() > k_max_command_bytes + 1) { */
/*         buf->retrieve_all(); */
/*         conn->force_close(); */
/*       } */
/*       return; */
/*     } */

/*     const std::size_t line_length = static_cast<std::size_t>(delimiter - begin); */
/*     const std::size_t length = */
/*         line_length > 0 && delimiter[-1] == '\r' ? line_length - 1 : line_length; */
/*     if (length > k_max_command_bytes) { */
/*       buf->retrieve_all(); */
/*       conn->force_close(); */
/*       return; */
/*     } */

/*     std::string command(begin, length); */
/*     buf->retrieve(line_length + 1); */

/*     if (dispatch_task(conn, session, command)) { */
/*       continue; */
/*     } */

/*     conn->send("\r\ntlss@" + session->name + ":" + session->current_directory + ">"); */
/*   } */
/* } */

void StorageService::on_message(const NET::TcpConnectionPtr& conn, NET::Buffer* buf,
                                TIME::Timestamp timestamp) {
  (void)timestamp;
  // 停机开始后不再接受命令；清空缓存避免遗留数据再次被解析。
  if (_stopping.load()) {
    buf->retrieve_all();
    return;
  }

  std::shared_ptr<ConnectionSession> session;
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = _sessions.find(conn.get());
    if (it == _sessions.end()) {
      return;
    }
    session = it->second;
  }

  try {
    while (conn->connected() && buf->readable_bytes() != 0) {
      const auto phase = session->transfer ? session->transfer->phase() : TransferPhase::idle;
      if (phase == TransferPhase::receiving_upload) {
        const auto task = session->transfer;
        task->consume(*buf);
        // One disk block per iteration; the adapter resumes buffered input.

        return;
      }

      if (phase == TransferPhase::finalizing_upload) {
        // 已接收完整正文，等待最后一块写完以及发布、数据库登记。
        // 保留可能紧跟正文到达的后续命令。
        return;
      }

      // 这些阶段不允许客户端继续提交内容。
      if (phase != TransferPhase::idle && phase != TransferPhase::waiting_download_ack) {
        buf->retrieve_all();
        conn->force_close();
        return;
      }

      const char* begin = buf->peek();
      const char* end = begin + buf->readable_bytes();
      const char* delimiter = std::find(begin, end, '\n');

      if (delimiter == end) {
        // 允许末尾的 '\r' 等待下次读取补上 '\n'。
        if (buf->readable_bytes() > Transfer::kMaxControlBytes + 1) {
          buf->retrieve_all();
          conn->force_close();
        }
        return;
      }

      const std::size_t raw_length = static_cast<std::size_t>(delimiter - begin);
      const std::size_t length =
          raw_length > 0 && delimiter[-1] == '\r' ? raw_length - 1 : raw_length;
      if (length > Transfer::kMaxControlBytes) {
        buf->retrieve_all();
        conn->force_close();
        return;
      }

      std::string command(begin, length);
      buf->retrieve(raw_length + 1);

      if (session->login_state == LoginState::k_authenticated && command.rfind("@TLSS ", 0) == 0) {
        handle_transfer_control(conn, session, command.substr(6));
        continue;
      }
      if (session->transfer != nullptr) {
        buf->retrieve_all();
        conn->force_close();
        return;
      }

      if (!dispatch_task(conn, session, command)) {
        conn->send("\r\ntlss@" + session->name + ":" + session->current_directory + ">");
      }
    }
  } catch (const std::exception& exception) {
    service_logger()->error("transfer receive failed: {}", exception.what());
    buf->retrieve_all();
    conn->force_close();
  }
}

// -------- private fun----------------------------------------------

bool StorageService::dispatch_task(const NET::TcpConnectionPtr& conn,
                                   const std::shared_ptr<ConnectionSession>& session,
                                   std::string_view command) {
  if (handle_auth_command(conn, session, std::string(command))) {
    return true;
  }

  if (session->directory_change_pending) {
    conn->send("Please wait.\r\n");
    return true;
  }

  if (command.empty()) {
    conn->send("tlss@" + session->name + ":" + session->current_directory + ">");
    return true;
  }

  if (command == "q" || command == "quit") {
    conn->send("bye!\r\n");
    conn->shutdow();
    return true;
  }

  if (command == "help" || command == "h") {
    conn->send("Commands : ls pwd cd rm rename move mkdir rmdir upload or download\r\ntlss@" +
               session->name + ":" + session->current_directory + ">");
    return true;
  }

  std::istringstream input{std::string(command)};
  std::string cmd;
  std::string args;

  input >> cmd;
  std::getline(input >> std::ws, args);
  const auto command_type = string_to_command(cmd);
  switch (*command_type) {
    case HandleCommand::cd:
      handle_cd(conn, session, args);
      break;

    case HandleCommand::ls:
      handle_ls(conn, session);
      break;

    case HandleCommand::pwd:
      handle_pwd(conn, session);
      break;

    case HandleCommand::rm:
      handle_rm(conn, session, args);
      break;

    case HandleCommand::rename:
      handle_rename(conn, session, args);
      break;

    case HandleCommand::move:
      handle_move(conn, session, args);
      break;

    case HandleCommand::mkdir:
      handle_mkdir(conn, session, args);
      break;

    case HandleCommand::rmdir:
      handle_rmdir(conn, session, args);
      break;

    case HandleCommand::download:
      handle_download(conn, session, args);
      break;

    case HandleCommand::upload:
      handle_upload(conn, session, args);
      break;

    default:
      conn->send("Invalid command");
      return false;
  }

  return true;
}

void StorageService::handle_cd(const NET::TcpConnectionPtr& conn,
                               const std::shared_ptr<ConnectionSession>& session,
                               const std::string& args) {
  if (_stopping.load()) {
    conn->force_close();
    return;
  }
  if (session->login_state != LoginState::k_authenticated) {
    conn->send("Login required.\r\n");
    return;
  }
  if (session->transfer || session->directory_change_pending) {
    conn->send("Please wait.\r\n");
    return;
  }

  std::string directory;
  try {
    std::istringstream input(args);
    directory = Transfer::read_field(input);
    Transfer::require_end(input);
    if (directory.empty()) {
      conn->send("Usage: cd \"directory\"\r\n");
      transfer_prompt(conn, session);
      return;
    }
  } catch (const std::exception&) {
    conn->send("Usage: cd \"directory\"\r\n");
    transfer_prompt(conn, session);
    return;
  }

  auto original = std::weak_ptr<ConnectionSession>(session);
  auto connection = std::weak_ptr<NET::TcpConnection>(conn);
  auto task = std::bind(&StorageService::move_file_by_cur_dir, this, session->name,
                        session->current_directory, original, connection, directory,
                        conn->get_loop());
  session->directory_change_pending = true;
  bool accepted = false;
  try {
    accepted = _th_pool.try_run(std::move(task));
  } catch (...) {
    session->directory_change_pending = false;
    throw;
  }
  if (!accepted) {
    session->directory_change_pending = false;
    if (_metrics) {
      _metrics->add(OBSERVABILITY::Metric::commands_rejected);
    }
    conn->send("Server busy. Please retry.\r\n");
    transfer_prompt(conn, session);
    return;
  }
}

void StorageService::move_file_by_cur_dir(const std::string& username,
                                          const std::string& current_directory,
                                          const std::weak_ptr<ConnectionSession>& original,
                                          const std::weak_ptr<NET::TcpConnection>& connection,
                                          const std::string& directory, NET::EventLoop* io_loop) {
  StorageContext<std::string> cur_path{false, {}};
  try {
    cur_path = _metadata_service.move_dir_by_current_dir(username, current_directory, directory);
  } catch (...) {
    // 服务层已经记录失败指标；异常也要交回 IO 线程清理本次 pending。
    cur_path = {false, {}};
  }

  // 查询期间不读取或修改会话；回调持有参数副本和弱引用，不延长旧会话寿命。
  auto check_task = [this, original, connection, username, current_directory,
                     cur_path = std::move(cur_path)] {
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

    // 结果只能用于提交查询时的用户、工作目录和仍在等待完成的会话。
    if (session->login_state != LoginState::k_authenticated || session->name != username ||
        session->current_directory != current_directory || !session->directory_change_pending) {
      return;
    }

    session->directory_change_pending = false;
    if (_stopping.load()) {
      conn->force_close();
      return;
    }

    if (cur_path.success) {
      session->current_directory = cur_path.result;
    } else {
      conn->send("Cannot change directory.\r\n");
    }
    transfer_prompt(conn, session);
  };

  io_loop->queue_in_loop(std::move(check_task));
}

void StorageService::handle_ls(const NET::TcpConnectionPtr& conn,
                               const std::shared_ptr<ConnectionSession>& session) {
  if (session->login_state == LoginState::k_authenticated) {
    auto connection = std::weak_ptr<NET::TcpConnection>(conn);
    auto task = std::bind(&StorageService::lookup_file_by_cur_dir, this, session->name,
                          session->current_directory, connection);
    const bool accepted = _th_pool.try_run(std::move(task));
    if (!accepted) {
      if (_metrics)
        _metrics->add(OBSERVABILITY::Metric::commands_rejected);
      conn->send("Server busy. Please retry.\r\n");
      return;
    }
  }
}

void StorageService::lookup_file_by_cur_dir(const std::string& username,
                                            const std::string& current_directory,
                                            std::weak_ptr<NET::TcpConnection>& connection) {
  auto files_info = _metadata_service.find_file_by_current_dir(username, current_directory);
  auto conn = connection.lock();
  if (!conn || !conn->connected()) {
    return;
  }
  std::string msg;

  if (!files_info.success) {
    msg = "Query failed.\r\n";
  } else if (files_info.result.empty()) {
    conn->send("tlss@" + username + ":" + current_directory + ">");
    return;
  } else {
    std::ostringstream out;
    out << std::left;  // 左对齐

    for (const auto& file : files_info.result) {
      out << std::setw(8) << file.file_name << "  " << std::setw(9) << file.type << "  "
          << file.create_at << "  " << file.update_at << "\r\n";
    }

    msg = out.str();
  }
  conn->send(msg + "tlss@" + username + ":" + current_directory + ">");
}

void StorageService::handle_pwd(const NET::TcpConnectionPtr& conn,
                                const std::shared_ptr<ConnectionSession>& session) {
  if (session->login_state == LoginState::k_authenticated) {
    conn->send(session->current_directory + "\r\ntlss@" + session->name + ":" +
               session->current_directory + ">");
  }
}

void StorageService::handle_mkdir(const NET::TcpConnectionPtr& conn,
                                  const std::shared_ptr<ConnectionSession>& session,
                                  const std::string& directory) {
  if (session->login_state == LoginState::k_authenticated) {
    auto connection = std::weak_ptr<NET::TcpConnection>(conn);
    auto task = std::bind(&StorageService::create_dir, this, session->name,
                          session->current_directory, directory, connection);
    const bool accepted = _th_pool.try_run(std::move(task));
    if (!accepted) {
      if (_metrics) {
        _metrics->add(OBSERVABILITY::Metric::commands_rejected);
      }
      conn->send("Server busy. Please retry.\r\n");
      return;
    }
  }
}

void StorageService::create_dir(const std::string& username, const std::string& current_directory,
                                const std::string& directory,
                                std::weak_ptr<NET::TcpConnection>& connection) {
  CreateDirectoryResult outcome = CreateDirectoryResult::failed;

  try {
    outcome = _metadata_service.create_dir(username, current_directory, directory);
  } catch (...) {
    // 元数据服务已统计失败；避免异常逃出业务任务。
    outcome = CreateDirectoryResult::failed;
  }

  auto conn = connection.lock();
  if (!conn || !conn->connected()) {
    return;
  }

  switch (outcome) {
    case CreateDirectoryResult::committed:
      conn->send("Directory created.\r\n");
      break;

    case CreateDirectoryResult::failed:
      conn->send("Cannot create directory.\r\n");
      break;

    case CreateDirectoryResult::outcome_unknown:
    default:
      conn->send(
          "Directory creation result is unknown. "
          "Check the directory state.\r\n");
      break;
  }

  conn->send("tlss@" + username + ":" + current_directory + ">");
}

bool StorageService::handle_auth_command(const NET::TcpConnectionPtr& conn,
                                         const std::shared_ptr<ConnectionSession>& session,
                                         const std::string& command) {
  switch (session->login_state) {
    case LoginState::k_authenticated:
      // 交给后续 ls、cd、pwd 等命令处理。
      return false;

    case LoginState::k_await_choice:
      if (command == "login") {
        session->pending_username.clear();
        session->name.clear();
        session->user_id = 0;
        session->login_state = LoginState::k_await_username;
        conn->send("Username:");
      } else if (command == "signup") {
        session->pending_username.clear();
        session->signup_password.clear();
        session->name.clear();
        session->user_id = 0;
        session->login_state = LoginState::k_signup_username;
        conn->send("New username (3-64 letters, digits or underscore):");
      } else if (command == "help" || command == "h") {
        conn->send("Commands : login or signup\r\ntlss>");
      } else {
        conn->send("Please enter login or signup:\r\ntlss>");
      }
      return true;

    case LoginState::k_await_username:
      if (command.empty() || command.size() > 64) {
        conn->send(
            "Username must contain 1 to 64 bytes.\r\n"
            "Username:");
        return true;
      }

      session->pending_username = command;
      session->login_state = LoginState::k_await_password;
      conn->send("Password:");
      return true;

    case LoginState::k_verifying:
    case LoginState::k_registering:
      conn->send("Please wait.\r\n");
      return true;

    case LoginState::k_signup_username:
    case LoginState::k_signup_password:
    case LoginState::k_signup_confirm:
      handle_signup_command(conn, session, command);
      return true;

    case LoginState::k_await_password:
      break;
  }

  if (command.empty() || command.size() > 128) {
    conn->send(
        "Password must contain 1 to 128 bytes.\r\n"
        "Password:");
    return true;
  }

  AUTH::LoginRequest request{
      session->pending_username,
      command,
  };

  session->login_state = LoginState::k_verifying;
  const std::uint64_t attempt = ++session->auth_attempt;

  const auto original = std::weak_ptr<ConnectionSession>(session);
  const auto connection = std::weak_ptr<NET::TcpConnection>(conn);
  auto* io_loop = conn->get_loop();

  auto task = [this, original, connection, io_loop, attempt, request = std::move(request)] {
    // 数据库访问和密码计算都发生在业务线程。
    auto result = _auth_service.authenticate(request);
    if (_metrics) {
      const auto metric = result.status == AUTH::AuthStatus::k_authenticated
                              ? OBSERVABILITY::Metric::login_success
                          : result.status == AUTH::AuthStatus::k_invalid_credentials
                              ? OBSERVABILITY::Metric::login_invalid
                              : OBSERVABILITY::Metric::auth_backend_failures;
      _metrics->add(metric);
    }

    io_loop->queue_in_loop(
        [this, original, connection, attempt, result = std::move(result)]() mutable {
          finish_authentication(original, connection, attempt, std::move(result));
        });
  };

  auto on_timeout = [this, original, connection, io_loop, attempt] {
    if (_metrics) {
      _metrics->add(OBSERVABILITY::Metric::auth_queue_timeouts);
    }
    // submit_async 的超时回调不在连接 IO 线程中执行。
    io_loop->queue_in_loop([this, original, connection, attempt] {
      finish_authentication(original, connection, attempt, AUTH::AuthResult{});
    });
  };

  // 这里只限制等待进入执行队列的时间。
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

  const bool accepted = _th_pool.submit_async(std::move(task), deadline, std::move(on_timeout));

  if (!accepted) {
    if (_metrics) {
      _metrics->add(OBSERVABILITY::Metric::commands_rejected);
    }
    // 当前已经在该连接的 IO 线程，可以直接处理失败结果。
    finish_authentication(original, connection, attempt, AUTH::AuthResult{});
  }

  return true;
}

void StorageService::handle_signup_command(const NET::TcpConnectionPtr& conn,
                                           const std::shared_ptr<ConnectionSession>& session,
                                           const std::string& command) {
  if (session->login_state == LoginState::k_signup_username) {
    if (!AUTH::AuthService::valid_signup_username(command)) {
      conn->send("Use 3-64 letters, digits or underscore.\r\nNew username:");
      return;
    }
    session->pending_username = command;
    session->login_state = LoginState::k_signup_password;
    conn->send("New password (8-128 bytes, no control characters):");
    return;
  }
  if (session->login_state == LoginState::k_signup_password) {
    if (!AUTH::AuthService::valid_signup_password(command)) {
      conn->send("Use 8-128 bytes without control characters.\r\nNew password:");
      return;
    }
    session->signup_password = command;
    session->login_state = LoginState::k_signup_confirm;
    conn->send("Confirm password:");
    return;
  }
  if (command != session->signup_password) {
    session->signup_password.clear();
    session->login_state = LoginState::k_signup_password;
    conn->send("Passwords do not match.\r\nNew password:");
    return;
  }

  AUTH::SignupRequest request{session->pending_username, std::move(session->signup_password)};
  session->signup_password.clear();
  session->login_state = LoginState::k_registering;
  const auto attempt = ++session->auth_attempt;
  const auto original = std::weak_ptr<ConnectionSession>(session);
  const auto connection = std::weak_ptr<NET::TcpConnection>(conn);
  auto* io_loop = conn->get_loop();

  auto task = [this, original, connection, io_loop, attempt, request = std::move(request)] {
    auto result = _auth_service.signup(request);
    if (_metrics) {
      _metrics->add(result.status == AUTH::SignupStatus::k_created
                        ? OBSERVABILITY::Metric::signup_success
                        : OBSERVABILITY::Metric::signup_failed);
      if (result.status == AUTH::SignupStatus::k_backend_unavailable ||
          result.status == AUTH::SignupStatus::k_outcome_unknown) {
        _metrics->add(OBSERVABILITY::Metric::auth_backend_failures);
      }
    }
    io_loop->queue_in_loop(
        [this, original, connection, attempt, result = std::move(result)]() mutable {
          finish_signup(original, connection, attempt, std::move(result));
        });
  };
  auto on_timeout = [this, original, connection, io_loop, attempt] {
    if (_metrics) {
      _metrics->add(OBSERVABILITY::Metric::auth_queue_timeouts);
    }
    io_loop->queue_in_loop([this, original, connection, attempt] {
      finish_signup(original, connection, attempt, {});
    });
  };
  // 只限制排队等待时间；已开始的注册不能通过超时回调宣告失败。
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  if (!_th_pool.submit_async(std::move(task), deadline, std::move(on_timeout))) {
    if (_metrics) {
      _metrics->add(OBSERVABILITY::Metric::commands_rejected);
    }
    finish_signup(original, connection, attempt, {});
  }
}

void StorageService::finish_signup(std::weak_ptr<ConnectionSession> original,
                                   std::weak_ptr<NET::TcpConnection> connection,
                                   std::uint64_t attempt, AUTH::SignupResult result) {
  auto session = original.lock();
  auto conn = connection.lock();
  if (!session || !conn || !conn->connected()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    const auto it = _sessions.find(conn.get());
    if (it == _sessions.end() || it->second != session) {
      return;
    }
  }
  if (session->login_state != LoginState::k_registering || session->auth_attempt != attempt) {
    return;
  }
  session->signup_password.clear();
  // 注册成功也不会自动获得登录身份。
  session->name.clear();
  session->user_id = 0;
  session->current_directory = "/";
  session->directory_change_pending = false;

  if (result.status == AUTH::SignupStatus::k_invalid_password) {
    session->login_state = LoginState::k_signup_password;
    conn->send("Use 8-128 bytes without control characters.\r\nNew password:");
    return;
  }
  session->pending_username.clear();
  if (result.status == AUTH::SignupStatus::k_username_taken ||
      result.status == AUTH::SignupStatus::k_invalid_username) {
    session->login_state = LoginState::k_signup_username;
    conn->send(result.status == AUTH::SignupStatus::k_username_taken
                   ? "Username is already taken.\r\nNew username:"
                   : "Use 3-64 letters, digits or underscore.\r\nNew username:");
    return;
  }
  session->login_state = LoginState::k_await_choice;
  if (result.status == AUTH::SignupStatus::k_created) {
    conn->send("Signup successful. Please log in.\r\n");
  } else if (result.status == AUTH::SignupStatus::k_outcome_unknown) {
    conn->send("Signup result is unknown. Try logging in before signing up again.\r\n");
  } else {
    conn->send("Service unavailable. Please retry.\r\n");
  }
  conn->send("Please enter login or signup:\r\ntlss>");
}

void StorageService::finish_authentication(std::weak_ptr<ConnectionSession> original,
                                           std::weak_ptr<NET::TcpConnection> connection,
                                           std::uint64_t attempt, AUTH::AuthResult result) {
  auto session = original.lock();
  auto conn = connection.lock();

  if (!session || !conn || !conn->connected()) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    const auto it = _sessions.find(conn.get());

    if (it == _sessions.end() || it->second != session) {
      return;
    }
  }

  if (session->login_state != LoginState::k_verifying || session->auth_attempt != attempt) {
    return;
  }

  session->pending_username.clear();

  if (result.status == AUTH::AuthStatus::k_authenticated) {
    session->user_id = result.user_id;
    session->name = std::move(result.username);
    session->current_directory = "/";
    session->directory_change_pending = false;
    session->login_state = LoginState::k_authenticated;

    Transfer::send_control(conn, "AUTH_OK");

    conn->send("Login successful.\r\ntlss@" + session->name + ":" + session->current_directory +
               ">");

    return;
  }

  session->user_id = 0;
  session->name.clear();
  session->current_directory = "/";
  session->login_state = LoginState::k_await_username;

  if (result.status == AUTH::AuthStatus::k_invalid_credentials) {
    conn->send(
        "Invalid username or password.\r\n"
        "Username:");
  } else {
    conn->send(
        "Service unavailable. Please retry.\r\n"
        "Username:");
  }
}

void StorageService::transfer_failed(const NET::TcpConnectionPtr& conn,
                                     const std::shared_ptr<ConnectionSession>& session,
                                     const std::string& error) {
  if (session->transfer) {
    session->transfer->cancel();
  }
  ++session->transfer_generation;
  session->transfer.reset();
  Transfer::send_control(conn, "ERROR " + Transfer::quote(Transfer::safe_error(error)));
  transfer_prompt(conn, session);
}

TransferContext StorageService::transfer_context(
    const NET::TcpConnectionPtr& conn, const std::shared_ptr<ConnectionSession>& session) {
  return {conn,
          session,
          ++session->transfer_generation,
          session->user_id,
          session->name,
          session->current_directory,
          _metrics};
}

void StorageService::on_transfer_event(const TransferContext& context, TransferEvent event) {
  auto [conn, session] = context.current();
  if (!conn) {
    return;
  }
  if (_stopping.load() || event.close_connection) {
    conn->force_close();
    return;
  }
  switch (event.kind) {
    case TransferEvent::Kind::upload_ready:
      Transfer::send_control(conn, "UREADY");
      break;
    case TransferEvent::Kind::upload_complete:
      session->transfer.reset();
      Transfer::send_control(conn, "UOK " + event.value);
      transfer_prompt(conn, session);
      break;
    case TransferEvent::Kind::download_ready:
      Transfer::send_control(conn, "DATA " + std::to_string(event.size) + " " + event.value);
      break;
    case TransferEvent::Kind::download_sent:
      Transfer::send_control(conn, "DONE");
      break;
    case TransferEvent::Kind::download_complete:
      session->transfer.reset();
      Transfer::send_control(conn, "OK");
      transfer_prompt(conn, session);
      break;
    case TransferEvent::Kind::failed:
      transfer_failed(conn, session, event.value);
      break;
  }
}

void StorageService::handle_download(const NET::TcpConnectionPtr& conn,
                                     const std::shared_ptr<ConnectionSession>& session,
                                     const std::string& args) {
  try {
    if (_stopping.load()) {
      conn->force_close();
      return;
    }
    if (session->login_state != LoginState::k_authenticated) {
      transfer_failed(conn, session, "login required");
      return;
    }
    if (session->transfer) {
      conn->force_close();
      return;
    }
    if (session->directory_change_pending) {
      transfer_failed(conn, session, "session is busy");
      return;
    }
    std::istringstream input(args);
    const auto name = Transfer::read_field(input);
    Transfer::require_end(input);
    if (!Transfer::valid_name(name)) {
      transfer_failed(conn, session, "invalid remote filename");
      return;
    }
    const auto context = transfer_context(conn, session);
    const auto task = _download_engine.create(context, name, [this, context](TransferEvent event) {
      on_transfer_event(context, std::move(event));
    });
    session->transfer = task;
    task->start();
  } catch (const std::exception& exception) {
    transfer_failed(conn, session, exception.what());
  }
}

void StorageService::handle_transfer_control(const NET::TcpConnectionPtr& conn,
                                             const std::shared_ptr<ConnectionSession>& session,
                                             const std::string& line) {
  std::istringstream input(line);
  std::string verb;
  input >> verb;
  if (session->transfer && session->transfer->phase() == TransferPhase::waiting_download_ack) {
    Transfer::require_end(input);
    if (verb != "ACK") {
      conn->force_close();
      return;
    }
    const auto task = session->transfer;
    task->acknowledge();
    return;
  }
  if (session->transfer) {
    conn->force_close();
    return;
  }
  std::string args;
  std::getline(input >> std::ws, args);
  if (verb == "UPLOAD") {
    handle_upload(conn, session, args);
  } else if (verb == "DOWNLOAD") {
    handle_download(conn, session, args);
  } else {
    transfer_failed(conn, session, "unknown transfer command");
  }
}

UploadRequest StorageService::parse_upload_request(const std::string& args) {
  std::istringstream input(args);
  std::string size_text;
  if (!(input >> size_text)) {
    throw std::runtime_error("missing file size");
  }
  UploadRequest request;
  request.size = Transfer::parse_size(size_text);
  request.name = Transfer::read_field(input);
  Transfer::require_end(input);
  if (!Transfer::valid_name(request.name)) {
    throw std::runtime_error("invalid remote filename");
  }
  return request;
}

void StorageService::handle_upload(const NET::TcpConnectionPtr& conn,
                                   const std::shared_ptr<ConnectionSession>& session,
                                   const std::string& args) {
  try {
    if (_stopping.load()) {
      conn->force_close();
      return;
    }
    if (session->login_state != LoginState::k_authenticated) {
      transfer_failed(conn, session, "login required");
      return;
    }
    if (session->transfer) {
      conn->force_close();
      return;
    }
    if (session->directory_change_pending) {
      transfer_failed(conn, session, "session is busy");
      return;
    }
    auto request = parse_upload_request(args);
    const auto context = transfer_context(conn, session);
    const auto task =
        _upload_engine.create(context, std::move(request), [this, context](TransferEvent event) {
          on_transfer_event(context, std::move(event));
        });
    session->transfer = task;
    task->start();
  } catch (const std::exception& exception) {
    transfer_failed(conn, session, exception.what());
  }
}

/*
 * 清理临时文件
 * -检查对象表
 * -回填历史引用
 * -登记磁盘对象
 * -归还连接
 * -恢复 DELETING 对象
 * */
bool StorageService::prepare_storage() {
  if (_started) {
    return false;
  }

  // 清理临时文件
  // 清理崩溃遗留的临时文件，并同步对象目录。
  // 失败时异常向上传播，停止后续启动准备。
  _object_storage.cleanup_temporary_files();

  // 这里借用连接池的连接之后，短暂持有连接
  // 用作用域提前归还
  {
    // 1. 借用连接、创建 ObjectRepository
    auto connection = _db_conn_pool->get_connection();
    if (!connection) {
      return false;
    }
    ObjectRepository repository(*connection);

    // 检查对象表
    // 2. 调用 repository.check_object_table()
    if (!repository.check_object_table()) {
      return false;
    }
    ObjectBackfill backfill(repository, _object_storage, _object_coordinator);

    // 回填历史引用
    if (!backfill.register_referenced_objects()) {
      return false;
    }

    // 登记磁盘上尚未进入对象表的对象。
    if (!backfill.register_disk_objects()) {
      return false;
    }
  }
  // 归还连接
  // 恢复 DELETING 对象
  return _object_gc.recover_deleting(std::chrono::minutes{10});
}

void StorageService::handle_rm(const NET::TcpConnectionPtr& conn,
                               const std::shared_ptr<ConnectionSession>& session,
                               const std::string& args) {
  // 1. _stopping 为真：关闭连接并返回
  if (_stopping.load()) {
    conn->force_close();
    return;
  }
  // 2. 用户未认证：发送 "Login required.\r\n" 并返回
  if (session->login_state != LoginState::k_authenticated) {
    conn->send("Login required.\r\n");
    return;
  }
  // 3. 存在 session->transfer，或 directory_change_pending 为真：发送 "Please wait.\r\n" 并返回
  if (session->transfer || session->directory_change_pending) {
    conn->send("Please wait.\r\n");
    return;
  }

  std::string file_name;
  // 4. 解析恰好一个文件名
  try {
    std::istringstream input(args);
    file_name = Transfer::read_field(input);
    Transfer::require_end(input);
    if (!Transfer::valid_name(file_name)) {
      conn->send("Invalid filename.\r\n");
      transfer_prompt(conn, session);
      return;
    }

  } catch (const std::exception& exception) {
    conn->send("Usage: rm \"filename\"\r\n");
    transfer_prompt(conn, session);
    return;
  }
  auto original = std::weak_ptr<ConnectionSession>(session);
  auto connection = std::weak_ptr<NET::TcpConnection>(conn);

  auto task =
      std::bind(&StorageService::remove_file_by_current_dir, this, session->name,
                session->current_directory, file_name, original, connection, conn->get_loop());

  // 暂时复用此标志，阻止删除期间继续提交普通命令。
  session->directory_change_pending = true;

  bool accepted = false;
  try {
    accepted = _th_pool.try_run(std::move(task));
  } catch (...) {
    session->directory_change_pending = false;
    throw;
  }

  if (!accepted) {
    session->directory_change_pending = false;
    if (_metrics) {
      _metrics->add(OBSERVABILITY::Metric::commands_rejected);
    }
    conn->send("Server busy. Please retry.\r\n");
    transfer_prompt(conn, session);
    return;
  }
}

void StorageService::remove_file_by_current_dir(const std::string& username,
                                                const std::string& directory,
                                                const std::string& file_name,
                                                const std::weak_ptr<ConnectionSession>& original,
                                                const std::weak_ptr<NET::TcpConnection>& connection,
                                                NET::EventLoop* io_loop) {
  DeleteFileResult outcome = DeleteFileResult::failed;
  // 业务线程：执行数据库删除，不访问会话字段
  try {
    outcome = _metadata_service.delete_file(username, directory, file_name);
  } catch (...) {
    // MetadataService 已记录失败指标。
    // 在任务入口处理异常，避免异常逃出业务线程。
    outcome = DeleteFileResult::failed;
  }

  // IO 线程：检查会话、恢复状态并发送结果
  auto check_task = [this, original, connection, username, directory, outcome] {
    auto session = original.lock();
    auto conn = connection.lock();

    if (!session || !conn || !conn->connected()) {
      return;
    }

    // 确认连接仍然对应提交任务时的会话。
    {
      std::lock_guard<std::mutex> lock(sessions_mutex_);
      auto it = _sessions.find(conn.get());

      if (it == _sessions.end() || it->second != session) {
        return;
      }
    }

    // 丢弃已经不适用于当前会话状态的结果。
    if (session->login_state != LoginState::k_authenticated || session->name != username ||
        session->current_directory != directory || !session->directory_change_pending) {
      return;
    }

    session->directory_change_pending = false;

    if (_stopping.load()) {
      conn->force_close();
      return;
    }

    switch (outcome) {
      case DeleteFileResult::committed:
        conn->send("File removed.\r\n");
        break;

      case DeleteFileResult::failed:
        conn->send("Cannot remove file.\r\n");
        break;

      case DeleteFileResult::outcome_unknown:
      default:
        conn->send(
            "Delete result is unknown. "
            "Check the directory state.\r\n");
        break;
    }

    transfer_prompt(conn, session);
  };

  // 将检查会话，恢复结果的任务交给io线程去做
  io_loop->queue_in_loop(std::move(check_task));
}

void StorageService::handle_rename(const NET::TcpConnectionPtr& conn,
                                   const std::shared_ptr<ConnectionSession>& session,
                                   const std::string& args) {
  if (_stopping.load()) {
    conn->force_close();
    return;
  }

  if (session->login_state != LoginState::k_authenticated) {
    conn->send("Login required.\r\n");
    return;
  }

  if (session->transfer || session->directory_change_pending) {
    conn->send("Please wait.\r\n");
    return;
  }

  std::string old_name;
  std::string new_name;
  try {
    std::istringstream input(args);
    old_name = Transfer::read_field(input);
    new_name = Transfer::read_field(input);
    Transfer::require_end(input);

    if (!Transfer::valid_name(old_name) || !Transfer::valid_name(new_name)) {
      conn->send("Invalid file name.\r\n");
      transfer_prompt(conn, session);
      return;
    }
  } catch (const std::exception&) {
    conn->send("Usage: rename \"old_name\" \"new_name\"\r\n");
    transfer_prompt(conn, session);
    return;
  }

  auto original = std::weak_ptr<ConnectionSession>(session);
  auto connection = std::weak_ptr<NET::TcpConnection>(conn);
  auto task = std::bind(&StorageService::rename_file_by_current_dir, this, session->name,
                        session->current_directory, old_name, new_name, original, connection,
                        conn->get_loop());

  // 提交前设置，防止任务立即完成时 IO 回调看不到本次操作的 pending 状态
  session->directory_change_pending = true;
  bool accepted = false;
  try {
    accepted = _th_pool.try_run(std::move(task));
  } catch (...) {
    session->directory_change_pending = false;
    throw;
  }

  if (!accepted) {
    session->directory_change_pending = false;
    if (_metrics) {
      _metrics->add(OBSERVABILITY::Metric::commands_rejected);
    }
    conn->send("Server busy. Please retry.\r\n");
    transfer_prompt(conn, session);
    return;
  }
}

void StorageService::handle_move(const NET::TcpConnectionPtr& conn,
                                 const std::shared_ptr<ConnectionSession>& session,
                                 const std::string& args) {
  if (_stopping.load()) {
    conn->force_close();
    return;
  }

  if (session->login_state != LoginState::k_authenticated) {
    conn->send("Login required.\r\n");
    return;
  }

  if (session->transfer || session->directory_change_pending) {
    conn->send("Please wait.\r\n");
    return;
  }

  std::string file_name;
  std::string target_directory;
  try {
    std::istringstream input(args);
    file_name = Transfer::read_field(input);
    target_directory = Transfer::read_field(input);
    Transfer::require_end(input);

    if (!Transfer::valid_name(file_name)) {
      conn->send("Invalid file name.\r\n");
      transfer_prompt(conn, session);
      return;
    }

    auto resolved = Transfer::resolve_directory_path(session->current_directory, target_directory);
    if (!resolved) {
      conn->send("Invalid target directory.\r\n");
      transfer_prompt(conn, session);
      return;
    }
    target_directory = std::move(*resolved);
  } catch (const std::exception&) {
    conn->send("Usage: move \"file_name\" \"target_directory\"\r\n");
    transfer_prompt(conn, session);
    return;
  }

  auto original = std::weak_ptr<ConnectionSession>(session);
  auto connection = std::weak_ptr<NET::TcpConnection>(conn);
  auto task = std::bind(&StorageService::move_file_to_directory, this, session->name,
                        session->current_directory, target_directory, file_name, original,
                        connection, conn->get_loop());

  // 提交前设置 pending；成功后的清理由业务任务投递的 IO 回调负责。
  session->directory_change_pending = true;
  bool accepted = false;
  try {
    accepted = _th_pool.try_run(std::move(task));
  } catch (...) {
    session->directory_change_pending = false;
    throw;
  }

  if (!accepted) {
    // 没有任务就不会有完成回调，入口必须自行恢复 pending。
    session->directory_change_pending = false;
    if (_metrics) {
      _metrics->add(OBSERVABILITY::Metric::commands_rejected);
    }
    conn->send("Server busy. Please retry.\r\n");
    transfer_prompt(conn, session);
    return;
  }
}

void StorageService::rename_file_by_current_dir(
    const std::string& username, const std::string& directory, const std::string& old_name,
    const std::string& new_name, const std::weak_ptr<ConnectionSession>& original,
    const std::weak_ptr<NET::TcpConnection>& connection, NET::EventLoop* io_loop) {
  RenameFileResult outcome = RenameFileResult::failed;
  // 业务线程执行数据库事务；会话字段的检查和修改统一放到 IO 回调中。
  try {
    outcome = _metadata_service.rename_file(username, directory, old_name, new_name);
  } catch (...) {
    // 服务层已经记录失败指标，任务入口负责阻止异常逃出业务线程。
    outcome = RenameFileResult::failed;
  }

  // 业务方法返回后回调才可能执行，因此捕获参数副本，连接和会话保留弱引用。
  auto check_task = [this, original, connection, username, directory, outcome] {
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

    // 结果只适用于提交任务时的会话身份和当前目录。
    if (session->login_state != LoginState::k_authenticated || session->name != username ||
        session->current_directory != directory || !session->directory_change_pending) {
      return;
    }

    session->directory_change_pending = false;
    if (_stopping.load()) {
      conn->force_close();
      return;
    }

    switch (outcome) {
      case RenameFileResult::committed:
        conn->send("File renamed.\r\n");
        break;
      case RenameFileResult::failed:
        conn->send("Cannot rename file.\r\n");
        break;
      case RenameFileResult::outcome_unknown:
      default:
        conn->send("Rename result is unknown. Check the directory state.\r\n");
        break;
    }

    transfer_prompt(conn, session);
  };

  io_loop->queue_in_loop(std::move(check_task));
}

void StorageService::move_file_to_directory(const std::string& username,
                                            const std::string& source_directory,
                                            const std::string& target_directory,
                                            const std::string& file_name,
                                            const std::weak_ptr<ConnectionSession>& original,
                                            const std::weak_ptr<NET::TcpConnection>& connection,
                                            NET::EventLoop* io_loop) {
  MoveFileResult outcome = MoveFileResult::failed;
  try {
    outcome = _metadata_service.move_file(username, source_directory, target_directory, file_name);
  } catch (...) {
    // 服务层负责失败指标；任务入口捕获异常，保证结果仍能交回 IO 线程。
    outcome = MoveFileResult::failed;
  }

  // 回调晚于业务方法返回，必须捕获参数副本；连接和会话继续保持弱引用。
  auto check_task = [this, original, connection, username, source_directory, outcome] {
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

    // 只处理发起移动时的会话和源目录，过期结果不能影响后来的会话。
    if (session->login_state != LoginState::k_authenticated || session->name != username ||
        session->current_directory != source_directory || !session->directory_change_pending) {
      return;
    }

    session->directory_change_pending = false;
    if (_stopping.load()) {
      conn->force_close();
      return;
    }

    switch (outcome) {
      case MoveFileResult::committed:
        conn->send("File moved.\r\n");
        break;
      case MoveFileResult::failed:
        conn->send("Cannot move file.\r\n");
        break;
      case MoveFileResult::outcome_unknown:
      default:
        conn->send("Move result is unknown. Check both directories.\r\n");
        break;
    }

    transfer_prompt(conn, session);
  };

  io_loop->queue_in_loop(std::move(check_task));
}

void StorageService::handle_rmdir(const NET::TcpConnectionPtr& conn,
                                  const std::shared_ptr<ConnectionSession>& session,
                                  const std::string& args) {
  if (_stopping.load()) {
    conn->force_close();
    return;
  }

  if (session->login_state != LoginState::k_authenticated) {
    conn->send("Login required.\r\n");
    return;
  }

  if (session->transfer || session->directory_change_pending) {
    conn->send("Please wait.\r\n");
    return;
  }

  std::string directory_name;
  bool recursive = false;
  try {
    std::istringstream input(args);
    input >> std::ws;
    const bool first_is_quoted = input.peek() == '"';
    const auto first = Transfer::read_field(input);
    if (!first_is_quoted && first == "-r") {
      recursive = true;
      directory_name = Transfer::read_field(input);
    } else {
      directory_name = first;
    }
    Transfer::require_end(input);
    if (!Transfer::valid_name(directory_name)) {
      conn->send("Invalid directory name.\r\n");
      transfer_prompt(conn, session);
      return;
    }
  } catch (const std::exception&) {
    conn->send("Usage: rmdir [-r] \"directory\"\r\n");
    transfer_prompt(conn, session);
    return;
  }

  auto original = std::weak_ptr<ConnectionSession>(session);
  auto connection = std::weak_ptr<NET::TcpConnection>(conn);

  auto operation = recursive ? &StorageService::remove_directory_tree_by_current_dir
                             : &StorageService::remove_empty_directory_by_current_dir;

  auto task = std::bind(operation, this, session->name, session->current_directory, directory_name,
                        original, connection, conn->get_loop());
  session->directory_change_pending = true;

  bool accepted = false;
  try {
    accepted = _th_pool.try_run(std::move(task));
  } catch (...) {
    session->directory_change_pending = false;
    throw;
  }

  if (!accepted) {
    session->directory_change_pending = false;
    if (_metrics) {
      _metrics->add(OBSERVABILITY::Metric::commands_rejected);
    }
    conn->send("Server busy. Please retry.\r\n");
    transfer_prompt(conn, session);
    return;
  }
}

void StorageService::remove_empty_directory_by_current_dir(
    const std::string& username, const std::string& directory, const std::string& directory_name,
    const std::weak_ptr<ConnectionSession>& original,
    const std::weak_ptr<NET::TcpConnection>& connection, NET::EventLoop* io_loop) {
  DeleteDirectoryResult outcome = DeleteDirectoryResult::failed;

  try {
    outcome = _metadata_service.delete_empty_directory(username, directory, directory_name);
  } catch (...) {
    // MetadataService 已记录失败指标。
    // 在任务入口捕获异常，避免异常逃出业务线程。
    outcome = DeleteDirectoryResult::failed;
  }
  auto check_task = [this, original, connection, username, directory, outcome] {
    auto session = original.lock();
    auto conn = connection.lock();

    if (!session || !conn || !conn->connected()) {
      return;
    }

    // 确认连接仍然对应提交任务时的会话。
    {
      std::lock_guard<std::mutex> lock(sessions_mutex_);
      auto it = _sessions.find(conn.get());

      if (it == _sessions.end() || it->second != session) {
        return;
      }
    }

    // 丢弃已经不适用于当前会话状态的结果。
    if (session->login_state != LoginState::k_authenticated || session->name != username ||
        session->current_directory != directory || !session->directory_change_pending) {
      return;
    }

    session->directory_change_pending = false;

    if (_stopping.load()) {
      conn->force_close();
      return;
    }

    switch (outcome) {
      case DeleteDirectoryResult::committed:
        conn->send("directory removed.\r\n");
        break;

      case DeleteDirectoryResult::failed:
        conn->send("Cannot remove directory.\r\n");
        break;

      case DeleteDirectoryResult::not_empty:
        conn->send("Directory is not empty.\r\n");
        break;

      case DeleteDirectoryResult::outcome_unknown:
      default:
        conn->send(
            "Delete result is unknown. "
            "Check the directory state.\r\n");
        break;
    }

    transfer_prompt(conn, session);
  };

  // 将检查会话，恢复结果的任务交给io线程去做
  io_loop->queue_in_loop(std::move(check_task));
}

void StorageService::remove_directory_tree_by_current_dir(
    const std::string& username, const std::string& directory, const std::string& directory_name,
    const std::weak_ptr<ConnectionSession>& original,
    const std::weak_ptr<NET::TcpConnection>& connection, NET::EventLoop* io_loop) {
  DeleteDirectoryTreeResult outcome = DeleteDirectoryTreeResult::failed;

  try {
    outcome = _metadata_service.delete_directory_tree(username, directory, directory_name);
  } catch (...) {
    // MetadataService 已记录失败指标。
    // 在任务入口捕获异常，避免异常逃出业务线程。
    outcome = DeleteDirectoryTreeResult::failed;
  }
  auto check_task = [this, original, connection, username, directory, outcome] {
    auto session = original.lock();
    auto conn = connection.lock();

    if (!session || !conn || !conn->connected()) {
      return;
    }

    // 确认连接仍然对应提交任务时的会话。
    {
      std::lock_guard<std::mutex> lock(sessions_mutex_);
      auto it = _sessions.find(conn.get());

      if (it == _sessions.end() || it->second != session) {
        return;
      }
    }

    // 丢弃已经不适用于当前会话状态的结果。
    if (session->login_state != LoginState::k_authenticated || session->name != username ||
        session->current_directory != directory || !session->directory_change_pending) {
      return;
    }

    session->directory_change_pending = false;

    if (_stopping.load()) {
      conn->force_close();
      return;
    }

    switch (outcome) {
      case DeleteDirectoryTreeResult::committed:
        conn->send("directory tree removed.\r\n");
        break;

      case DeleteDirectoryTreeResult::failed:
        conn->send("Cannot remove directory tree.\r\n");
        break;

      case DeleteDirectoryTreeResult::outcome_unknown:
      default:
        conn->send(
            "Delete result is unknown. "
            "Check the directory state.\r\n");
        break;
    }

    transfer_prompt(conn, session);
  };

  // 将检查会话，恢复结果的任务交给io线程去做
  io_loop->queue_in_loop(std::move(check_task));
}

}  // namespace TLSS::FileStorage
