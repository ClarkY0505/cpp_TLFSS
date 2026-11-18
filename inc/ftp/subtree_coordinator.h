#ifndef __INC_FTP_SUBTREE_COORDINATOR_H__
#define __INC_FTP_SUBTREE_COORDINATOR_H__

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace TLSS::FileStorage {
enum class PathScopeKind {
  exact,   // 只包含指定目录
  subtree  // 包含指定目录及全部后代
};

struct PathScope {
  std::string path;
  PathScopeKind kind = PathScopeKind::exact;
};

// 参数必须是规范绝对路径，除根目录外没有末尾 '/。
// 判断 path 是否等于 root，或位于 root 的子树内
bool path_is_within(const std::string& path, const std::string& root);

// 参数中的路径均为规范绝对路径。
// 只判断范围是否重叠，不判断用户和读写方式。
bool scopes_overlap(const PathScope& first, const PathScope& second);

enum class AccessMode { read, write };

struct CoordinationRequest {
  std::string username;
  AccessMode access = AccessMode::read;
  std::vector<PathScope> scopes;
};

// 同一用户、至少一方写入、任意一对范围重叠时，返回 true。
bool requests_conflict(const CoordinationRequest& first, const CoordinationRequest& second);

class SubtreeCoordinator {
 private:
  struct RequestState {
    CoordinationRequest request;
  };

 public:
  class Guard;

  SubtreeCoordinator() = default;
  SubtreeCoordinator(const SubtreeCoordinator&) = delete;
  SubtreeCoordinator& operator=(const SubtreeCoordinator&) = delete;

  // 一次申请整个请求；存在冲突时等待。
  // 协调器必须比返回的 Guard 活得更久。
  Guard acquire(CoordinationRequest request);

 private:
  void release(const std::shared_ptr<RequestState>& state) noexcept;
  // 调用者必须持有 _mutex，state 已加入等待队列。
  // 本函数不重复加锁。
  bool can_enter_locked(const std::shared_ptr<RequestState>& state) const;

  std::mutex _mutex;
  std::condition_variable _changed;

  // 保存已获准进入的请求
  std::vector<std::shared_ptr<RequestState>> _active_requests;
  // 保存等待请求，后续用来避免后来者不断抢先
  std::vector<std::shared_ptr<RequestState>> _waiting_requests;
};

class SubtreeCoordinator::Guard {
 public:
  Guard() noexcept = default;
  ~Guard() noexcept;

  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;

  Guard(Guard&& other) noexcept;
  Guard& operator=(Guard&& other) noexcept;

  void reset() noexcept;

 private:
  friend class SubtreeCoordinator;

  Guard(SubtreeCoordinator* owner, std::shared_ptr<RequestState> state) noexcept;

  SubtreeCoordinator* _owner = nullptr;
  std::shared_ptr<RequestState> _state;
};

}  // namespace TLSS::FileStorage
#endif  // __INC_FTP_SUBTREE_COORDINATOR_H__
