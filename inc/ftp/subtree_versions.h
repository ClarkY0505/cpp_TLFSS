#ifndef __INC_FTP_SUBTREE_VERSIONS_H__
#define __INC_FTP_SUBTREE_VERSIONS_H__

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace TLSS::FileStorage {

class SubtreeVersions {
 public:
  struct Snapshot {
    std::uint64_t generation = 0;
    bool enabled = true;
  };

  // 参数来自已校验的请求；目录为规范绝对路径。
  Snapshot snapshot(const std::string& username, const std::string& directory) const;

  // 推进目标前缀的子树版本；不分配内存。
  void advance(const std::string& username, const std::string& root) noexcept;

 private:
  static constexpr std::size_t kBucketCount = 64;

  static std::size_t bucket_for(std::string_view username, std::string_view path) noexcept;

  mutable std::mutex _mutex;
  std::uint64_t _sequence = 0;
  bool _enabled = true;
  std::array<std::uint64_t, kBucketCount> _versions{};
};

}  // namespace TLSS::FileStorage
#endif  // __INC_FTP_SUBTREE_VERSIONS_H__
