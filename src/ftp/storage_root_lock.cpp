#include "ftp/storage_root_lock.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <cerrno>
#include <stdexcept>
#include <system_error>

namespace TLSS::FileStorage {
StorageRootLock::StorageRootLock(const std::filesystem::path& root) {
  // 目录创建失败时，异常直接向上传播。
  std::filesystem::create_directories(root);

  const auto lock_path = root / ".tlss.lock";

  // 打开或创建锁文件；系统调用被信号中断时重试。
  int fd;
  do {
    fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  } while (fd == -1 && errno == EINTR);

  if (fd == -1) {
    const int error_code = errno;
    throw std::system_error(error_code, std::generic_category(), "open storage root lock");
  }

  // 立即移交所有权。
  // 后续即使构造函数抛异常，这个成员也会析构并关闭描述符。
  _lock_file.reset(fd);

  struct stat info {};
  int result;
  do {
    result = ::fstat(_lock_file.fd, &info);
  } while (result == -1 && errno == EINTR);

  if (result == -1) {
    const int error_code = errno;
    throw std::system_error(error_code, std::generic_category(), "stat storage root lock");
  }

  if (!S_ISREG(info.st_mode)) {
    throw std::runtime_error("storage root lock must be a regular file");
  }

  // 独占、非阻塞：已有实例持锁时，立即失败。
  do {
    result = ::flock(_lock_file.fd, LOCK_EX | LOCK_NB);
  } while (result == -1 && errno == EINTR);

  if (result == -1) {
    const int error_code = errno;
    throw std::system_error(error_code, std::generic_category(), "acquire storage root lock");
  }
}
}  // namespace TLSS::FileStorage
