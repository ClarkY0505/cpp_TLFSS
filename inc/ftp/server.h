#ifndef __INC_FTP_SERVER_H__
#define __INC_FTP_SERVER_H__

#include "auth/auth_service.h"
#include "auth/user_repository.h"
#include "common/base/mysql_connection_pool.h"
#include "common/base/tlss_thread_pool.h"
#include "common/net/tcp_server.h"
#include "ftp/service_engine.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
namespace TLSS::FileStorage {
class StorageService {
 public:
  explicit StorageService(NET::EventLoop* loop, const NET::InetAddress& addr,
                          const std::string& name = "tlss_service", int subloop_size = 4,
                          int thread_pool_threads_num = 8, int thread_pool_queue_max_size = 256);
  ~StorageService();
  void start();
  // 从主 EventLoop 线程以外调用；先排空任务和连接，再释放资源。
  // 重复调用等待同一次停机完成；返回数据库连接池是否在期限内关闭。
  bool stop(std::chrono::milliseconds grace_period = std::chrono::seconds(5));

 private:
  struct PreparedUpload {
    bool success = false;
    std::string error;
    std::string parent_id;
    // 第一版使用的成员变量
    // 该方法存在慢磁盘阻塞
    /* std::shared_ptr<Transfer::Receiver> receiver; */
  };
  // 作为第一版上传文件数据结构的保留
  /* struct CommitUploadResult { */
  /* bool success = false; */
  /* std::string hash; */
  /* std::string error; */
  /* }; */

  struct PreparedDownload {
    bool success = false;
    std::string error;
    std::string hash;
    std::shared_ptr<Transfer::SourceFile> source;
  };
  struct UploadRequest {
    std::uint64_t size = 0;
    std::string name;
  };

  struct UploadContext {
    std::weak_ptr<NET::TcpConnection> weak_conn;
    std::weak_ptr<ConnectionSession> weak_session;

    std::uint64_t generation = 0;
    std::uint64_t user_id = 0;

    std::string username;
    std::string directory;
    std::string name;
    std::uint64_t size = 0;

    using Current = std::pair<NET::TcpConnectionPtr, std::shared_ptr<ConnectionSession>>;

    Current current() const;
  };
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

  void handle_file_query();

  UploadRequest parse_upload_request(const std::string& args);
  void submit_upload_prepare(const NET::TcpConnectionPtr& conn,
                             const std::shared_ptr<ConnectionSession>& session,
                             UploadContext context);
  void on_upload_prepared(const UploadContext& context, std::optional<PreparedUpload> value,
                          const std::string& error);
  void start_upload_receiver(const UploadContext& context, const NET::TcpConnectionPtr& conn,
                             const std::shared_ptr<ConnectionSession>& session,
                             const std::string& parent_id);
  void on_upload_ready(const UploadContext& context);
  void on_upload_done(const UploadContext& context, std::string hash);
  void on_upload_failed(const UploadContext& context, std::string error);
  void handle_upload(const NET::TcpConnectionPtr& conn,
                     const std::shared_ptr<ConnectionSession>& session, const std::string& args);

  void handle_download(const NET::TcpConnectionPtr& conn,
                       const std::shared_ptr<ConnectionSession>& session, const std::string& args);
  // 该方法为第一版实现上传方式
  // 现在版本为第二版，通过 AsyncReceiver::finish()来完成
  /* void finish_upload(const NET::TcpConnectionPtr& conn, */
  /* const std::shared_ptr<ConnectionSession>& session); */
  void handle_transfer_control(const NET::TcpConnectionPtr& conn,
                               const std::shared_ptr<ConnectionSession>& session,
                               const std::string& line);
  void transfer_failed(const NET::TcpConnectionPtr& conn,
                       const std::shared_ptr<ConnectionSession>& session, const std::string& error);

  void handle_cd(const NET::TcpConnectionPtr& conn,
                 const std::shared_ptr<ConnectionSession>& session, const std::string& directory);
  void move_file_by_cur_dir(const std::string& username, const std::string& current_directory,
                            const std::weak_ptr<ConnectionSession>& original,
                            const std::weak_ptr<NET::TcpConnection>& connection,
                            const std::string& directory, NET::EventLoop* io_loop);
  void handle_ls(const NET::TcpConnectionPtr& conn,
                 const std::shared_ptr<ConnectionSession>& session);
  void lookup_file_by_cur_dir(const std::string& username, const std::string& current_directory,
                              std::weak_ptr<NET::TcpConnection>& connection);
  void handle_pwd(const NET::TcpConnectionPtr& conn,
                  const std::shared_ptr<ConnectionSession>& session);
  void handle_mkdir(const NET::TcpConnectionPtr& conn,
                    const std::shared_ptr<ConnectionSession>& session,
                    const std::string& directory);
  void create_dir(const std::string& username, const std::string& current_directory,
                  const std::string& directory, std::weak_ptr<NET::TcpConnection>& connection);
  // 异步线程超时处理
  void finish_username_wait_timeout(std::weak_ptr<ConnectionSession> original,
                                    std::weak_ptr<NET::TcpConnection> connection);
  int _thread_pool_threads_num;
  NET::EventLoop* _loop;
  NET::TcpServer _tcp_server;
  BASE::DbConnectionPool* _db_conn_pool;
  BASE::ThreadPool _th_pool;
  Transfer::DiskExecutor _disk_executor{"tlss_disk"};

  std::atomic<bool> _stopping{false};
  std::mutex _stop_mutex;
  std::condition_variable _stop_done;
  bool _started = false;
  bool _stop_started = false;
  bool _stopped = false;
  bool _stop_succeeded = false;

  AUTH::MySqlUserRepository _user_repository;
  AUTH::AuthService _auth_service;
  StorageEngine _storage_engine;

  std::mutex sessions_mutex_;
  std::unordered_map<const NET::TcpConnection*, std::shared_ptr<ConnectionSession>> _sessions;
};
}  // namespace TLSS::FileStorage

#endif  // __INC_FTP_SERVER_H__
