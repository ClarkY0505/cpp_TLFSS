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

 private:
  std::shared_ptr<OBSERVABILITY::StorageMetrics> _metrics;
};
}  // namespace TLSS::FileStorage
#endif
