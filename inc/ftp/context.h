#ifndef __INC_FTP_CONTEXT_H__
#define __INC_FTP_CONTEXT_H__

#include "ftp/file_transfer.h"

#include <cstdint>
#include <string>

namespace TLSS::FileStorage {

template <typename T>
struct StorageContext {
  bool success = false;
  T result{};
};

enum class LoginState {
  k_await_choice,
  k_await_username,
  k_await_password,
  k_verifying,
  k_signup_username,
  k_signup_password,
  k_signup_confirm,
  k_registering,
  k_authenticated,
};

enum class TransferPhase {
  idle,
  preparing_upload,
  receiving_upload,
  finalizing_upload,
  preparing_download,
  sending_download,
  waiting_download_ack
};

struct SessionTransfer {
  TransferPhase phase = TransferPhase::idle;

  std::string file_name;
  std::string directory;
  std::string parent_id;

  std::shared_ptr<Transfer::Receiver> receiver;
};

struct ConnectionSession {
  // 只允许连接所属的 IO 线程读写。
  LoginState login_state = LoginState::k_await_choice;

  // 登录或注册交互阶段暂存的用户名，不作为已认证身份使用。
  std::string pending_username;

  // 只在注册等待确认密码期间保存，提交或确认失败后立即清空。
  std::string signup_password;

  // 每次提交认证任务时递增，用于识别过期回调。
  std::uint64_t auth_attempt = 0;

  // 仅认证成功后填写。
  std::string name;
  std::uint64_t user_id = 0;

  std::string current_directory = "/";
  bool directory_change_pending = false;

  std::uint64_t transfer_generation = 0;
  SessionTransfer transfer;
};

struct FileInfo {
  std::string file_name;
  std::string type;
  std::string create_at;
  std::string update_at;
};

}  // namespace TLSS::FileStorage

#endif  // __INC_FTP_CONTEXT_H__
