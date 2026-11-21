#ifndef TLSS_FTP_TCP_STORAGE_SERVER_H
#define TLSS_FTP_TCP_STORAGE_SERVER_H

#include "common/base/tlss_thread_pool.h"
#include "common/net/tcp_server.h"
#include "ftp/context.h"
#include "ftp/disk_executor.h"
#include "ftp/server.h"
#include "ftp/tcp_transfer_tasks.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace TLSS::FileStorage {
// TCP adapter: protocol/session state stays on each connection's owning IO loop.
// Execution resources and business service are borrowed from StorageRuntime.
class TcpStorageServer {
 public:
  TcpStorageServer(NET::EventLoop* loop, const NET::InetAddress& address, const std::string& name,
                   int io_threads, BASE::ThreadPool& workers, Transfer::DiskExecutor& disk,
                   StorageService& service, const std::atomic<bool>& stopping,
                   std::shared_ptr<OBSERVABILITY::StorageMetrics> metrics = {});
  void start() {
    _tcp_server.start();
  }
  void stop_accepting(NET::TcpServer::Completion done) {
    _tcp_server.stop_accepting(std::move(done));
  }
  void shutdown_connections(NET::TcpServer::Completion done) {
    _tcp_server.shutdown_connections(std::move(done));
  }
  void force_close_connections() {
    _tcp_server.force_close_connections();
  }
  void stop_io_threads() {
    _tcp_server.stop_io_threads();
  }

 private:
  std::shared_ptr<UploadTask> create_upload_task(TransferContext context, UploadRequest request,
                                                 TransferTask::Notify notify);
  std::shared_ptr<DownloadTask> create_download_task(TransferContext context, std::string name,
                                                     TransferTask::Notify notify);
  void on_connection(const NET::TcpConnectionPtr& conn);
  void on_message(const NET::TcpConnectionPtr& conn, NET::Buffer* buf, TIME::Timestamp);
  bool dispatch_task(const NET::TcpConnectionPtr& conn,
                     const std::shared_ptr<ConnectionSession>& session, std::string_view command);

  bool handle_auth_command(const NET::TcpConnectionPtr& conn,
                           const std::shared_ptr<ConnectionSession>& session,
                           const std::string& command);

  void finish_authentication(std::weak_ptr<ConnectionSession> original,
                             std::weak_ptr<NET::TcpConnection> connection, std::uint64_t attempt,
                             AUTH::AuthResult result);

  void handle_signup_command(const NET::TcpConnectionPtr& conn,
                             const std::shared_ptr<ConnectionSession>& session,
                             const std::string& command);
  void finish_signup(std::weak_ptr<ConnectionSession> original,
                     std::weak_ptr<NET::TcpConnection> connection, std::uint64_t attempt,
                     AUTH::SignupResult result);

  UploadRequest parse_upload_request(const std::string& args);
  TransferContext transfer_context(const NET::TcpConnectionPtr& conn,
                                   const std::shared_ptr<ConnectionSession>& session);
  void on_transfer_event(const TransferContext& context, TransferEvent event);
  void handle_upload(const NET::TcpConnectionPtr& conn,
                     const std::shared_ptr<ConnectionSession>& session, const std::string& args);

  void handle_download(const NET::TcpConnectionPtr& conn,
                       const std::shared_ptr<ConnectionSession>& session, const std::string& args);
  void handle_transfer_control(const NET::TcpConnectionPtr& conn,
                               const std::shared_ptr<ConnectionSession>& session,
                               const std::string& line);
  void transfer_failed(const NET::TcpConnectionPtr& conn,
                       const std::shared_ptr<ConnectionSession>& session, const std::string& error);

  void handle_cd(const NET::TcpConnectionPtr& conn,
                 const std::shared_ptr<ConnectionSession>& session, const std::string& args);
  void move_file_by_cur_dir(AUTH::PrincipalHandle principal, const std::string& current_directory,
                            const std::weak_ptr<ConnectionSession>& original,
                            const std::weak_ptr<NET::TcpConnection>& connection,
                            const std::string& directory, NET::EventLoop* io_loop);
  void handle_ls(const NET::TcpConnectionPtr& conn,
                 const std::shared_ptr<ConnectionSession>& session);
  void lookup_file_by_cur_dir(AUTH::PrincipalHandle principal, const std::string& current_directory,
                              const std::weak_ptr<ConnectionSession>& original,
                              const std::weak_ptr<NET::TcpConnection>& connection,
                              NET::EventLoop* io_loop);
  void handle_pwd(const NET::TcpConnectionPtr& conn,
                  const std::shared_ptr<ConnectionSession>& session);
  void handle_mkdir(const NET::TcpConnectionPtr& conn,
                    const std::shared_ptr<ConnectionSession>& session,
                    const std::string& directory);
  // 在连接所属的 IO 线程中执行：解析参数、检查状态、提交任务
  void handle_rm(const NET::TcpConnectionPtr& conn,
                 const std::shared_ptr<ConnectionSession>& session, const std::string& args);
  // IO 线程：校验参数和会话状态，提交业务任务
  void handle_rmdir(const NET::TcpConnectionPtr& conn,
                    const std::shared_ptr<ConnectionSession>& session, const std::string& args);
  // IO 线程：检查会话、解析新旧文件名并提交重命名业务任务。
  void handle_rename(const NET::TcpConnectionPtr& conn,
                     const std::shared_ptr<ConnectionSession>& session, const std::string& args);
  // IO 线程：检查会话状态，解析文件名和目标绝对目录。
  void handle_move(const NET::TcpConnectionPtr& conn,
                   const std::shared_ptr<ConnectionSession>& session, const std::string& args);

  // 业务线程：删除空目录，将结果投递回 IO 线程
  void remove_empty_directory_by_current_dir(AUTH::PrincipalHandle principal,
                                             const std::string& directory,
                                             const std::string& directory_name,
                                             const std::weak_ptr<ConnectionSession>& original,
                                             const std::weak_ptr<NET::TcpConnection>& connection,
                                             NET::EventLoop* io_loop);
  // 在业务线程中执行：删除元数据，再向 IO 线程投递结果
  void remove_file_by_current_dir(AUTH::PrincipalHandle principal, const std::string& directory,
                                  const std::string& file_name,
                                  const std::weak_ptr<ConnectionSession>& original,
                                  const std::weak_ptr<NET::TcpConnection>& connection,
                                  NET::EventLoop* io_loop);
  // 业务线程：重命名文件，将三态结果投递回连接所属的 IO 线程
  void rename_file_by_current_dir(AUTH::PrincipalHandle principal, const std::string& directory,
                                  const std::string& old_name, const std::string& new_name,
                                  const std::weak_ptr<ConnectionSession>& original,
                                  const std::weak_ptr<NET::TcpConnection>& connection,
                                  NET::EventLoop* io_loop);
  // 业务线程：移动文件，将三态结果投递回连接所属的 IO 线程。
  void move_file_to_directory(AUTH::PrincipalHandle principal, const std::string& source_directory,
                              const std::string& target_directory, const std::string& file_name,
                              const std::weak_ptr<ConnectionSession>& original,
                              const std::weak_ptr<NET::TcpConnection>& connection,
                              NET::EventLoop* io_loop);
  void create_dir(AUTH::PrincipalHandle principal, const std::string& current_directory,
                  const std::string& directory, const std::weak_ptr<ConnectionSession>& original,
                  const std::weak_ptr<NET::TcpConnection>& connection, NET::EventLoop* io_loop);
  // Worker callers supply snapshots; only the IO loop checks the live session and replies.
  void queue_command_reply(AUTH::PrincipalHandle principal,
                           std::weak_ptr<ConnectionSession> original,
                           std::weak_ptr<NET::TcpConnection> connection, NET::EventLoop* io_loop,
                           std::string message);
  // 异步线程超时处理
  void finish_username_wait_timeout(std::weak_ptr<ConnectionSession> original,
                                    std::weak_ptr<NET::TcpConnection> connection);

  // 业务线程：递归删除目录，将结果投递回 IO 线程
  void remove_directory_tree_by_current_dir(AUTH::PrincipalHandle principal,
                                            const std::string& directory,
                                            const std::string& directory_name,
                                            const std::weak_ptr<ConnectionSession>& original,
                                            const std::weak_ptr<NET::TcpConnection>& connection,
                                            NET::EventLoop* io_loop);
  std::shared_ptr<OBSERVABILITY::StorageMetrics> _metrics;
  NET::TcpServer _tcp_server;
  BASE::ThreadPool& _th_pool;
  Transfer::DiskExecutor& _disk_executor;
  StorageService& _service;
  const std::atomic<bool>& _stopping;
  std::mutex sessions_mutex_;
  std::unordered_map<const NET::TcpConnection*, std::shared_ptr<ConnectionSession>> _sessions;
};
}  // namespace TLSS::FileStorage
#endif
