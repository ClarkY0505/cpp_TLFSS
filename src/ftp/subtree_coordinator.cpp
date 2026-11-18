#include "ftp/subtree_coordinator.h"
#include "ftp/transfer_validation.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

namespace TLSS::FileStorage {

bool path_is_within(const std::string& path, const std::string& root) {
  if (path == root) {
    return true;
  }

  if (root == "/") {
    return true;
  }

  if (path.size() <= root.size()) {
    return false;
  }

  return path.compare(0, root.size(), root) == 0 && path[root.size()] == '/';
}

bool scopes_overlap(const PathScope& first, const PathScope& second) {
  // 1. 两个都是 exact
  if (first.path == second.path) {
    return true;
  }

  // 2. 前者是 subtree，后者是 exact
  // 判断后者是否位于前者的子树内
  if (first.kind == PathScopeKind::subtree && path_is_within(second.path, first.path)) {
    return true;
  }

  // 3. 前者是 exact，后者是 subtree
  // 判断后者是否位于前者的子树内
  if (second.kind == PathScopeKind::subtree && path_is_within(first.path, second.path)) {
    return true;
  }

  return false;
}

bool requests_conflict(const CoordinationRequest& first, const CoordinationRequest& second) {
  if (first.username != second.username ||
      (first.access == AccessMode::read && second.access == AccessMode::read)) {
    return false;
  }
  for (const auto& first_scope : first.scopes) {
    for (const auto& second_scope : second.scopes) {
      if (scopes_overlap(first_scope, second_scope)) {
        return true;
      }
    }
  }
  return false;
}

SubtreeCoordinator::Guard::Guard(SubtreeCoordinator* owner,
                                 std::shared_ptr<RequestState> state) noexcept
    : _owner(owner)
    , _state(std::move(state)) {}

SubtreeCoordinator::Guard::Guard(Guard&& other) noexcept
    : _owner(other._owner)
    , _state(std::move(other._state)) {
  other._owner = nullptr;
}

SubtreeCoordinator::Guard::~Guard() noexcept {
  reset();
}

void SubtreeCoordinator::release(const std::shared_ptr<RequestState>& state) noexcept {
  {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = std::find(_active_requests.begin(), _active_requests.end(), state);
    if (it == _active_requests.end()) {
      return;
    }

    _active_requests.erase(it);
  }
  _changed.notify_all();
}

void SubtreeCoordinator::Guard::reset() noexcept {
  if (_owner == nullptr) {
    return;
  }

  _owner->release(_state);
  _owner = nullptr;
  _state.reset();
}

SubtreeCoordinator::Guard& SubtreeCoordinator::Guard::operator=(Guard&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  reset();
  _owner = other._owner;
  _state = std::move(other._state);
  other._owner = nullptr;

  return *this;
}

bool SubtreeCoordinator::can_enter_locked(const std::shared_ptr<RequestState>& state) const {
  for (const auto& active_request : _active_requests) {
    if (requests_conflict(active_request->request, state->request)) {
      return false;
    }
  }

  for (const auto& waiting_request : _waiting_requests) {
    if (waiting_request == state) {
      return true;
    }
    if (requests_conflict(waiting_request->request, state->request)) {
      return false;
    }
  }

  return false;
}

SubtreeCoordinator::Guard SubtreeCoordinator::acquire(CoordinationRequest request) {
  // 1. 用户名为空或 scopes 为空时，抛出 std::invalid_argument
  if (request.username.empty() || request.scopes.empty()) {
    throw std::invalid_argument("");
  }

  if (request.access != AccessMode::read && request.access != AccessMode::write) {
    throw std::invalid_argument("invalid access mode");
  }

  for (const auto& scope : request.scopes) {
    if ((scope.kind != PathScopeKind::exact && scope.kind != PathScopeKind::subtree) ||
        !Transfer::valid_directory_path(scope.path)) {
      throw std::invalid_argument("invalid path scope");
    }
  }

  // 2. 创建独立的请求记录
  auto state = std::make_shared<RequestState>();
  state->request = std::move(request);
  // 3. 获取管理锁，并登记到等待队列
  std::unique_lock<std::mutex> lock(_mutex);
  _waiting_requests.push_back(state);
  try {
    // 4.  等待并加入活动列表
    _changed.wait(lock, [this, &state] {
      return can_enter_locked(state);
    });
    _active_requests.push_back(state);
  } catch (...) {
    auto it = std::find(_waiting_requests.begin(), _waiting_requests.end(), state);
    if (it != _waiting_requests.end()) {
      _waiting_requests.erase(it);
    }

    lock.unlock();
    _changed.notify_all();
    throw;
  }

  auto it = std::find(_waiting_requests.begin(), _waiting_requests.end(), state);
  _waiting_requests.erase(it);
  return Guard(this, std::move(state));
}
}  // namespace TLSS::FileStorage
