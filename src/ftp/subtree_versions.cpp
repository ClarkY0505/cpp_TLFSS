#include "ftp/subtree_versions.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
namespace TLSS::FileStorage {
std::size_t SubtreeVersions::bucket_for(std::string_view username, std::string_view path) noexcept {
  const auto user_hash = std::hash<std::string_view>{}(username);
  const auto path_hash = std::hash<std::string_view>{}(path);

  return (user_hash ^ path_hash) % kBucketCount;
}

void SubtreeVersions::advance(const std::string& username, const std::string& root) noexcept {
  // 保存分片下标
  std::size_t idx = bucket_for(username, root);
  std::lock_guard<std::mutex> lock(_mutex);
  if (!_enabled) {
    return;
  }

  // _enabled 设为 false，然后返回，防止版本回绕
  if (_sequence == std::numeric_limits<std::uint64_t>::max()) {
    _enabled = false;
    return;
  }

  ++_sequence;
  _versions[idx] = _sequence;
}

SubtreeVersions::Snapshot SubtreeVersions::snapshot(const std::string& username,
                                                    const std::string& directory) const {
  // 1. 准备分片标记和路径视图
  std::array<bool, kBucketCount> selected{};
  const std::string_view path(directory);

  selected[bucket_for(username, "/")] = true;
  // 2. 找到每个中间祖先，并标记其分片
  auto pos = path.find('/', 1);
  while (pos != std::string_view::npos) {
    selected[bucket_for(username, path.substr(0, pos))] = true;
    pos = path.find('/', pos + 1);
  }

  // 3. 标记目录自己

  selected[bucket_for(username, path)] = true;
  std::lock_guard<std::mutex> lock(_mutex);
  if (!_enabled) {
    return {0, false};
  }
  std::uint64_t generation = 0;

  for (std::size_t index = 0; index < selected.size(); ++index) {
    if (selected[index]) {
      generation = std::max(generation, _versions[index]);
    }
  }

  return {generation, true};
}
}  // namespace TLSS::FileStorage
