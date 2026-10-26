#include "common/base/tlss_thread_pool.h"
#include "common/net/callbacks.h"
#include "common/net/event_loop.h"
#include "common/net/tcp_connection.h"
#include "ftp/context.h"
#include "ftp/file_transfer.h"
#include "ftp/server.h"
#include "ftp/service_logger.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <future>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
/* #include <stdexcept> */
namespace TLSS::FileStorage {

namespace {
enum HandleCommand { login, signup, cd, ls, pwd, mkdir, download, upload, unknown };

struct CommitUploadResult {
  bool success = false;
  std::string hash;
  std::string error;
};

struct PreparedUpload {
  bool success = false;
  std::string error;
  std::string parent_id;
  std::shared_ptr<Transfer::Receiver> receiver;
};

struct PreparedDownload {
  bool success = false;
  std::string error;
  std::string hash;
  std::shared_ptr<Transfer::SourceFile> source;
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
  if (command == "download") {
    return HandleCommand::download;
  }
  if (command == "upload") {
    return HandleCommand::upload;
  }

  return HandleCommand::unknown;
}

/* template <typename Work, typename Finish> */
/* bool submit_transfer_job(BASE::ThreadPool& pool, const std::atomic<bool>* stopping, */
/*                          const NET::TcpConnectionPtr& conn, */
/*                          const std::shared_ptr<ConnectionSession>& session, Work work, */
/*                          Finish finish) { */
/*   // 推导 Work 类型的对象以左值形式、不带参数调用时的返回类型 */
/*   using Result = std::invoke_result_t<Work&>; */
/*   auto weak_conn = std::weak_ptr<NET::TcpConnection>(conn); */
/*   auto weak_session = std::weak_ptr<ConnectionSession>(session); */
/*   NET::EventLoop* io_loop = conn->get_loop(); */

/*   const std::uint64_t generation = session->transfer_generation; */
/*   const std::uint64_t user_id = session->user_id; */
/*   const std::string username = session->name; */
/*   const std::string directory = session->current_directory; */

/*   return pool.try_run([weak_conn, weak_session, io_loop, stopping, generation, user_id, username,
 */
/*                        directory, work = std::move(work), finish = std::move(finish)]() mutable {
 */
/*     std::optional<Result> value; */
/*     std::string error; */
/*     if (stopping->load()) { */
/*       error = "server is stopping"; */
/*       service_logger()->info("transfer job skipped: {}", error); */
/*     } else { */
/*       try { */
/*         value.emplace(work()); */
/*       } catch (const std::exception& exception) { */
/*         error = exception.what(); */
/*         service_logger()->error("transfer job failed: {}", error); */
/*       } catch (...) { */
/*         error = "unexpected transfer failure"; */
/*         service_logger()->error("transfer job failed: {}", error); */
/*       } */
/*     } */

/*     io_loop->queue_in_loop([weak_conn, weak_session, stopping, generation, user_id, username, */
/*                             directory, value = std::move(value), error = std::move(error), */
/*                             finish = std::move(finish)]() mutable { */
/*       auto conn = weak_conn.lock(); */
/*       auto session = weak_session.lock(); */

/*       if (!conn || !session || !conn->connected()) { */
/*         return; */
/*       } */

/*       if (session->transfer_generation != generation || */
/*           session->login_state != LoginState::k_authenticated || session->user_id != user_id ||
 */
/*           session->name != username || session->current_directory != directory) { */
/*         return; */
/*       } */

/*       if (stopping->load()) { */
/*         conn->force_close(); */
/*         return; */
/*       } */

/*       finish(conn, session, std::move(value), error); */
/*     }); */
/*   }); */
/* } */

struct TransferJobContext {
  std::weak_ptr<NET::TcpConnection> weak_conn;
  std::weak_ptr<ConnectionSession> weak_session;

  NET::EventLoop* io_loop;
  const std::atomic<bool>* stopping;

  std::uint64_t generation;
  std::uint64_t user_id;
  std::string username;
  std::string directory;
};

template <typename Result, typename Finish>
void finish_transfer_job(const TransferJobContext& context, std::optional<Result>& value,
                         const std::string& error, Finish& finish) {
  auto conn = context.weak_conn.lock();
  auto session = context.weak_session.lock();

  if (!conn || !session || !conn->connected()) {
    return;
  }

  if (session->transfer_generation != context.generation ||
      session->login_state != LoginState::k_authenticated || session->user_id != context.user_id ||
      session->name != context.username || session->current_directory != context.directory) {
    return;
  }

  if (context.stopping->load()) {
    conn->force_close();
    return;
  }

  finish(conn, session, std::move(value), error);
}

template <typename Work, typename Finish>
void run_transfer_job(const TransferJobContext& context, Work& work, Finish& finish) {
  using Result = std::invoke_result_t<Work&>;

  std::optional<Result> value;
  std::string error;
  if (context.stopping->load()) {
    error = "server is stopping";
    service_logger()->info("transfer job skipped: {}", error);
  } else {
    //
    // TODO 如果想做，消除异常处理
    // 这里的异常大部分是由optional提供的
    // FIXME 少部分来自work,目前work用于传输文件
    //       在调用file_transfer中的函数会抛出异常
    //       在修改完数据结构后，这里不应该在出现来自work的异常
    // 如果不想要接收异常需重载std::error_code&
    // 然后明确函数状态
    try {
      value.emplace(work());
    } catch (const std::exception& exception) {
      error = exception.what();
      service_logger()->error("transfer job failed: {}", error);
    } catch (...) {
      error = "unexpected transfer failure";
      service_logger()->error("transfer job failed: {}", error);
    }
  }

  context.io_loop->queue_in_loop(std::bind(&finish_transfer_job<Result, Finish>, context,
                                           std::move(value), std::move(error), std::move(finish)));
}

template <typename Work, typename Finish>
bool submit_transfer_job(BASE::ThreadPool& pool, const std::atomic<bool>* stopping,
                         const NET::TcpConnectionPtr& conn,
                         const std::shared_ptr<ConnectionSession>& session, Work work,
                         Finish finish) {
  TransferJobContext context{std::weak_ptr<NET::TcpConnection>(conn),
                             std::weak_ptr<ConnectionSession>(session),
                             conn->get_loop(),
                             stopping,
                             session->transfer_generation,
                             session->user_id,
                             session->name,
                             session->current_directory};
  return pool.try_run(std::bind(&run_transfer_job<Work, Finish>, std::move(context),
                                std::move(work), std::move(finish)));
}

}  // namespace

StorageService::StorageService(NET::EventLoop* loop, const NET::InetAddress& addr,
                               const std::string& name, int subloop_size,
                               int thread_pool_threads_num, int thread_pool_queue_max_size)
    : _thread_pool_threads_num(thread_pool_threads_num)
    , _loop(loop)
    , _tcp_server(_loop, addr, name)
    , _db_conn_pool(BASE::DbConnectionPool::get_connection_pool())
    , _th_pool("tlss_worker")
    , _user_repository(*_db_conn_pool)
    , _auth_service(_user_repository)
    , _storage_engine(*_db_conn_pool) {
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
    _sessions.emplace(conn.get(), session);
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
      auto phase = session->transfer.phase;
      if (phase == TransferPhase::receiving_upload) {
        session->transfer.receiver->consume(*buf);
        if (session->transfer.receiver->remaining() == 0) {
          finish_upload(conn, session);
        }

        continue;
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
      if (session->transfer.phase != TransferPhase::idle) {
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
    conn->send("Commands : ls pwd cd mkdir upload or download\r\ntlss@" + session->name + ":" +
               session->current_directory + ">");
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
      if (args.empty()) {
        conn->send("Usage: cd <directory>");
        return false;
      }

      handle_cd(conn, session, args);
      break;

    case HandleCommand::ls:
      handle_ls(conn, session);
      break;

    case HandleCommand::pwd:
      handle_pwd(conn, session);
      break;

    case HandleCommand::mkdir:
      handle_mkdir(conn, session, args);
      break;

    case HandleCommand::download:
      handle_download(conn,session,args);
      break;

    case HandleCommand::upload:
      handle_upload(conn,session,args);
      break;

    default:
      conn->send("Invalid command");
      return false;
  }

  return true;
}

void StorageService::handle_cd(const NET::TcpConnectionPtr& conn,
                               const std::shared_ptr<ConnectionSession>& session,
                               const std::string& directory) {
  if (session->login_state == LoginState::k_authenticated) {
    auto original = std::weak_ptr<ConnectionSession>(session);
    auto connection = std::weak_ptr<NET::TcpConnection>(conn);
    auto task =
        std::bind(&StorageService::move_file_by_cur_dir, this, session->name,
                  session->current_directory, original, connection, directory, conn->get_loop());
    session->directory_change_pending = true;
    const bool accepted = _th_pool.try_run(std::move(task));
    if (!accepted) {
      session->directory_change_pending = false;
      conn->send("Server busy. Please retry.\r\ntlss@" + session->name + ":" +
                 session->current_directory + ">");
      return;
    }
  }
}

void StorageService::move_file_by_cur_dir(const std::string& username,
                                          const std::string& current_directory,
                                          const std::weak_ptr<ConnectionSession>& original,
                                          const std::weak_ptr<NET::TcpConnection>& connection,
                                          const std::string& directory, NET::EventLoop* io_loop) {
  auto cur_path = _storage_engine.move_dir_by_current_dir(username, current_directory, directory);
  io_loop->queue_in_loop([original, connection, cur_path = std::move(cur_path)] {
    auto session = original.lock();
    auto conn = connection.lock();
    if (!session || !conn || !conn->connected()) {
      return;
    }

    session->directory_change_pending = false;
    if (cur_path.success) {
      session->current_directory.clear();
      session->current_directory = cur_path.result;
    } else {
      conn->send("Cannot change directory.\r\n");
    }
    conn->send("tlss@" + session->name + ":" + session->current_directory + ">");
  });
}

void StorageService::handle_ls(const NET::TcpConnectionPtr& conn,
                               const std::shared_ptr<ConnectionSession>& session) {
  if (session->login_state == LoginState::k_authenticated) {
    auto connection = std::weak_ptr<NET::TcpConnection>(conn);
    auto task = std::bind(&StorageService::lookup_file_by_cur_dir, this, session->name,
                          session->current_directory, connection);
    const bool accepted = _th_pool.try_run(std::move(task));
    if (!accepted) {
      conn->send("Server busy. Please retry.\r\n");
      return;
    }
  }
}

void StorageService::lookup_file_by_cur_dir(const std::string& username,
                                            const std::string& current_directory,
                                            std::weak_ptr<NET::TcpConnection>& connection) {
  auto files_info = _storage_engine.find_file_by_current_dir(username, current_directory);
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
      conn->send("Server busy. Please retry.\r\n");
      return;
    }
  }
}

void StorageService::create_dir(const std::string& username, const std::string& current_directory,
                                const std::string& directory,
                                std::weak_ptr<NET::TcpConnection>& connection) {
  bool res = _storage_engine.create_dir(username, current_directory, directory);
  auto conn = connection.lock();
  if (!conn || !conn->connected()) {
    return;
  }
  if (!res) {
    conn->send("create dir failed\r\n");
    return;
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

    io_loop->queue_in_loop(
        [this, original, connection, attempt, result = std::move(result)]() mutable {
          finish_authentication(original, connection, attempt, std::move(result));
        });
  };

  auto on_timeout = [this, original, connection, io_loop, attempt] {
    // submit_async 的超时回调不在连接 IO 线程中执行。
    io_loop->queue_in_loop([this, original, connection, attempt] {
      finish_authentication(original, connection, attempt, AUTH::AuthResult{});
    });
  };

  // 这里只限制等待进入执行队列的时间。
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

  const bool accepted = _th_pool.submit_async(std::move(task), deadline, std::move(on_timeout));

  if (!accepted) {
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
    io_loop->queue_in_loop(
        [this, original, connection, attempt, result = std::move(result)]() mutable {
          finish_signup(original, connection, attempt, std::move(result));
        });
  };
  auto on_timeout = [this, original, connection, io_loop, attempt] {
    io_loop->queue_in_loop([this, original, connection, attempt] {
      finish_signup(original, connection, attempt, {});
    });
  };
  // 只限制排队等待时间；已开始的注册不能通过超时回调宣告失败。
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  if (!_th_pool.submit_async(std::move(task), deadline, std::move(on_timeout))) {
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
  ++session->transfer_generation;
  session->transfer = SessionTransfer{};

  Transfer::send_control(conn, "ERROR " + Transfer::quote(Transfer::safe_error(error)));
  transfer_prompt(conn, session);
}

void StorageService::handle_upload(const NET::TcpConnectionPtr& conn,
                                   const std::shared_ptr<ConnectionSession>& session,
                                   const std::string& args) {
  //
  // HACK
  // 在这里做try其实并不好
  // 写在这里为了后续修改这部分
  // 目前对于传输文件来说会抛出异常
  // TODO 修改file_transfer中的数据结构
  // FIXME 修改完数据结构后不再需要接收异常
  try {
    if (_stopping.load()) {
      conn->force_close();
      return;
    }
    if (session->login_state != LoginState::k_authenticated) {
      transfer_failed(conn, session, "login required");
      return;
    }
    if (session->transfer.phase != TransferPhase::idle) {
      // Do not reset a transfer whose body may still be in flight.
      conn->force_close();
      return;
    }
    if (session->directory_change_pending) {
      transfer_failed(conn, session, "session is busy");
      return;
    }

    std::istringstream input(args);
    std::string size_text;
    if (!(input >> size_text)) {
      transfer_failed(conn, session, "missing file size");
      return;
    }

    // FIXME 这里会抛出异常
    const uint64_t size = Transfer::parse_size(size_text);
    // FIXME 这里会抛出异常
    const std::string name = Transfer::read_field(input);
    // FIXME 这里会抛出异常
    Transfer::require_end(input);
    // FIXME 这里会抛出异常
    if (!Transfer::valid_name(name)) {
      transfer_failed(conn, session, "invalid remote filename");
      return;
    }

    ++session->transfer_generation;
    session->transfer.phase = TransferPhase::preparing_upload;
    session->transfer.file_name = name;
    session->transfer.directory = session->current_directory;

    const std::string username = session->name;
    const std::string directory = session->current_directory;

    auto work = [this, username, directory, name, size]() -> PreparedUpload {
      PreparedUpload result;
      StorageContext<std::string> parent =
          _storage_engine.prepare_upload(username, directory, name);
      if (!parent.success) {
        result.error = "directory unavailable or filename already exists";
        return result;
      }

      result.parent_id = parent.result;
      result.receiver = std::make_shared<Transfer::Receiver>(Transfer::object_root(), size);
      result.success = true;
      return result;
    };

    /* *
     * auto finish = [this, conn](const std::shared_ptr<NET::TcpConnection>& lamdba_conn,
     * const std::shared_ptr<FileStorage::ConnectionSession>& session,
     * std::optional<PreparedUpload> value, const std::string& error) {};
     * */
    auto finish = [this](const auto& conn, const auto& session, auto value,
                         const std::string& error) {
      // submit_transfer_job 本身报告的失败
      if (!value) {
        transfer_failed(conn, session, error);
        return;
      }

      // work 返回的业务失败
      if (!value->success) {
        transfer_failed(conn, session, value->error);
        return;
      }

      session->transfer.parent_id = value->parent_id;
      session->transfer.receiver = value->receiver;
      session->transfer.phase = TransferPhase::receiving_upload;
      Transfer::send_control(conn, "UREADY");

      if (value->receiver->remaining() == 0) {
        finish_upload(conn, session);
      }
    };
    const bool accepted = submit_transfer_job(_th_pool, &_stopping, conn, session, work, finish);
    if (!accepted) {
      transfer_failed(conn, session, "server is busy");
    }
  } catch (const std::exception& exception) {
    transfer_failed(conn, session, exception.what());
  }
}

void StorageService::finish_upload(const NET::TcpConnectionPtr& conn,
                                   const std::shared_ptr<ConnectionSession>& session) {
  if (session->transfer.phase != TransferPhase::receiving_upload || !session->transfer.receiver ||
      session->transfer.receiver->remaining() != 0) {
    return;
  }

  if (_stopping.load()) {
    conn->force_close();
    return;
  }

  session->transfer.phase = TransferPhase::finalizing_upload;
  const std::shared_ptr<Transfer::Receiver> receiver = session->transfer.receiver;
  const std::string username = session->name;
  const std::string parent_id = session->transfer.parent_id;
  const std::string directory = session->transfer.directory;
  const std::string name = session->transfer.file_name;

  auto work = [this, receiver, username, parent_id, directory, name]() -> CommitUploadResult {
    std::string hash = receiver->hash();
    receiver->publish(Transfer::object_root() / hash, true, hash, 0444);
    if (!_storage_engine.commit_upload(username, parent_id, directory, name, hash)) {
      // 不能删除可能被其他用户引用的共享对象
      return {false, {}, "file stored but metadata registration failed"};
    }

    return {true, std::move(hash), {}};
  };

  /* *
   * auto finish = [this, conn](const std::shared_ptr<NET::TcpConnection>& lamdba_conn,
   * const std::shared_ptr<FileStorage::ConnectionSession>& session,
   * std::optional<CommitUploadResult> value, const std::string& error) {};
   * */
  auto finish = [this](const auto& conn, const auto& session, auto value,
                       const std::string& error) {
    // 任务未执行，或执行过程中抛出了异常。
    if (!value) {
      transfer_failed(conn, session, error);
      return;
    }

    // 任务正常返回，但元数据登记失败。
    if (!value->success) {
      transfer_failed(conn, session, value->error);
      return;
    }

    session->transfer = SessionTransfer{};

    Transfer::send_control(conn, "UOK " + value->hash);
    transfer_prompt(conn, session);
  };

  const bool accepted = submit_transfer_job(_th_pool, &_stopping, conn, session, work, finish);
  if (!accepted) {
    transfer_failed(conn, session, "server is busy");
  }
}

void StorageService::handle_download(const NET::TcpConnectionPtr& conn,
                                     const std::shared_ptr<ConnectionSession>& session,
                                     const std::string& args) {
  //
  // HACK
  // 在这里做try其实并不好
  // 写在这里为了后续修改这部分
  // 目前对于传输文件来说会抛出异常
  // TODO 修改file_transfer中的数据结构
  // FIXME 修改完数据结构后不再需要接收异常
  try {
    if (_stopping.load()) {
      conn->force_close();
      return;
    }
    if (session->login_state != LoginState::k_authenticated) {
      transfer_failed(conn, session, "login required");
      return;
    }
    if (session->transfer.phase != TransferPhase::idle) {
      // Do not reset a transfer whose body may still be in flight.
      conn->force_close();
      return;
    }
    if (session->directory_change_pending) {
      transfer_failed(conn, session, "session is busy");
      return;
    }

    std::istringstream input(args);
    // FIXME 这里会抛出异常
    const std::string name = Transfer::read_field(input);
    // FIXME 这里会抛出异常
    Transfer::require_end(input);
    // FIXME 这里会抛出异常
    if (!Transfer::valid_name(name)) {
      transfer_failed(conn, session, "invalid remote filename");
      return;
    }

    ++session->transfer_generation;
    session->transfer.phase = TransferPhase::preparing_download;

    const std::string username = session->name;
    const std::string directory = session->current_directory;

    auto work = [this, username, directory, name]() -> PreparedDownload {
      const StorageContext<std::string> result =
          _storage_engine.find_download_hash(username, directory, name);
      PreparedDownload prepared;
      if (!result.success) {
        prepared.error = "file not found or unavailable";
        return prepared;
      }

      prepared.hash = result.result;
      // FIXME 这里会抛出异常
      prepared.source = Transfer::open_source(Transfer::object_root() / prepared.hash, true);
      prepared.success = true;
      return prepared;
    };

    /* auto finish = [this](const auto& conn, const auto& session, auto value, */
    /* const std::string& error) { */
    auto finish = [this](const std::shared_ptr<NET::TcpConnection>& conn,
                               const std::shared_ptr<FileStorage::ConnectionSession>& session,
                               std::optional<PreparedDownload> value, const std::string& error) {
      // 本身的错误
      if (!value) {
        transfer_failed(conn, session, error);
        return;
      }
      // work业务的错误
      if (!value->success) {
        transfer_failed(conn, session, value->error);
        return;
      }

      session->transfer.phase = TransferPhase::sending_download;
      const std::uint64_t generation = session->transfer_generation;
      auto weak_conn = std::weak_ptr<NET::TcpConnection>(conn);
      auto weak_session = std::weak_ptr<ConnectionSession>(session);

      // 同一个 IO 回调中连续提交，保证头部和正文相邻。
      Transfer::send_control(conn,
                             "DATA " + std::to_string(value->source->size) + " " + value->hash);
      // 在发送完文件后的一个回调函数
      NET::TcpConnection::FileSendCallback cb = [weak_conn, weak_session, generation](
                                                    std::error_code error, uint64_t) {
        auto conn = weak_conn.lock();
        auto session = weak_session.lock();

        if (!conn || !session || !conn->connected() || session->transfer_generation != generation) {
          return;
        }
        if (error) {
          conn->force_close();
          return;
        }

        // 先切换状态，再发送 DONE，避免 ACK 竞态。
        session->transfer.phase = TransferPhase::waiting_download_ack;
        Transfer::send_control(conn, "DONE");
      };
      // FIXME 存在慢磁盘阻塞
      conn->send_file(value->source->file.fd, 0, value->source->size, cb);
    };

    const bool accepted = submit_transfer_job(_th_pool, &_stopping, conn, session, work, finish);
    if (!accepted) {
      transfer_failed(conn, session, "server is busy");
    }
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
  if (session->transfer.phase == TransferPhase::waiting_download_ack) {
    Transfer::require_end(input);
    if (verb != "ACK") {
      conn->force_close();
      return;
    }

    session->transfer = SessionTransfer{};
    Transfer::send_control(conn, "OK");
    transfer_prompt(conn, session);
    return;
  }

  if (session->transfer.phase != TransferPhase::idle) {
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

}  // namespace TLSS::FileStorage
