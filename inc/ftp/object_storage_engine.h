#ifndef TLSS_FTP_OBJECT_STORAGE_ENGINE_H
#define TLSS_FTP_OBJECT_STORAGE_ENGINE_H

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ftp/verify_engine.h"

#include "observability/storage_metrics.h"

namespace TLSS::NET {
class Buffer;
}
namespace TLSS::FileStorage::Transfer {
namespace fs = std::filesystem;

struct File {
  int fd = -1;
  File() = default;
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  ~File();
  void reset(int value = -1);
};
struct TemporaryPath {
  std::string path;
  TemporaryPath() = default;
  TemporaryPath(const TemporaryPath&) = delete;
  TemporaryPath& operator=(const TemporaryPath&) = delete;
  ~TemporaryPath();
  void remove();
};
struct SourceFile {
  File file;
  std::uint64_t size = 0;
};
[[noreturn]] void system_failure(const char* operation);
fs::path object_root();
std::shared_ptr<SourceFile> open_source(const fs::path& path, bool no_followd = false);
std::string hash_file(int fd);
void sync_file(int fd);
void sync_directory(const fs::path& directory);

// Blocking local-file implementation, also used for client download destinations.
// Construction, mutation and destruction belong to the disk executor.
class Receiver {
 public:
  Receiver(const fs::path& directory, std::uint64_t size);
  void set_metrics(std::shared_ptr<OBSERVABILITY::StorageMetrics> metrics) {
    metrics_ = metrics;
    verifier_.set_metrics(std::move(metrics));
  }
  std::uint64_t remaining() const {
    return remaining_;
  }
  std::uint64_t size() const {
    return total_;
  }
  void append(const char* data, std::size_t count);
  void consume(NET::Buffer& buffer);
  const std::string& hash();
  void publish(const fs::path& destination, bool reuse_existing, const std::string& expected_hash,
               mode_t mode);

 private:
  std::shared_ptr<OBSERVABILITY::StorageMetrics> metrics_;
  std::uint64_t total_;
  std::uint64_t remaining_;
  File file_;
  TemporaryPath temporary_;
  VerifyEngine verifier_;
};
}  // namespace TLSS::FileStorage::Transfer

namespace TLSS::FileStorage {
struct ObjectInfo {
  std::string hash;
  std::uint64_t size = 0;
};
// Synchronous storage primitives: callers choose the execution resource.
// This component has no database, connection or protocol state.
class ObjectStorageEngine {
 public:
  // Configure before starting workers; metrics ownership outlives asynchronous operations.
  void set_metrics(std::shared_ptr<OBSERVABILITY::StorageMetrics> metrics) {
    _metrics = std::move(metrics);
  }

  using WriteHandle = std::unique_ptr<Transfer::Receiver>;
  using ReadHandle = std::shared_ptr<Transfer::SourceFile>;
  WriteHandle create_temporary(std::uint64_t size) const;
  ObjectInfo publish(Transfer::Receiver& writer) const;
  ReadHandle open_read(const std::string& hash) const;
  static std::string read(const ReadHandle& source, off_t offset, std::size_t count,
                          const std::function<bool()>& cancelled = {});

  bool verify_object(const std::string& hash) const;
  // 调用者须持有对应哈希锁，并已确认 DELETING 状态提交成功。
  // 对象不存在时仍同步目录；其他删除或同步错误继续向上传播。
  void remove_object(const std::string& hash) const;
  // 仅在持有对象目录独占锁、且没有活动传输的启动阶段调用。
  // 返回本次实际删除的临时文件数量；零表示没有文件被删除。
  // 删除结束后同步目录；扫描、删除或同步失败时抛出异常。
  // 中途失败不撤销之前已完成的删除，可在下次启动时重试。
  std::size_t cleanup_temporary_files() const;
  // 启动阶段持有目录独占锁、没有活动传输时调用。
  // 返回按哈希排序的普通文件名；空清单是正常结果。
  // 不跟随符号链接、不递归、不验证文件内容。
  // 扫描失败时抛出异常，不把失败转换为空清单。
  std::vector<std::string> list_object_hashes() const;

 private:
  std::shared_ptr<OBSERVABILITY::StorageMetrics> _metrics;
};
}  // namespace TLSS::FileStorage
#endif
