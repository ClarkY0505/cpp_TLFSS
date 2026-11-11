#ifndef __INC_FTP_STORAGE_ROOT_LOCK_H__
#define __INC_FTP_STORAGE_ROOT_LOCK_H__

#include <filesystem>
#include "ftp/object_storage_engine.h"

namespace TLSS::FileStorage {

class StorageRootLock {
 public:
  explicit StorageRootLock(const std::filesystem::path& root);

  StorageRootLock(const StorageRootLock&) = delete;
  StorageRootLock& operator=(const StorageRootLock&) = delete;

 private:
  // 复用已有 RAII 文件句柄，析构时自动关闭。
  Transfer::File _lock_file;
};
}  // namespace TLSS::FileStorage
#endif  // __INC_FTP_STORAGE_ROOT_LOCK_H__
