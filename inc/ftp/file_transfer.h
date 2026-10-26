#ifndef __INC_FTP_FILE_TRANSFER_H__
#define __INC_FTP_FILE_TRANSFER_H__
/*
 * 该头文件是only-head
 * 只提供文件句柄、临时文件、SHA-256、精确长度接收和文件发布功能
 * 因此本头文件中所有函数会抛出异常，统一由调用者处理
 * 必须使用try catch 来捕获异常
 * FIXME 目前这种方式存在慢磁盘阻塞，因此当前可以采用这种抛出异常的解决方法
 *       但是如果让传入文件任务进入异步任务来解决慢磁盘问题
 *       就不能在异步任务失败时，直接抛回提交者
 *
 * TODO 让函数抛出异常并不是一个很好的解决方法
 *      增加结构体来记录当前状态
 *      让调用者通过结构体中保存的信息来处理
 * FIXME 后续让每个函数不再抛出异常
 * FIXME valid_name valid_hash 保存bool
 * FIXME parse_size bool＋数值输出参数＋错误参数
 * FIXME consume publish sync_file bool＋错误参数
 * FIXME open_source 返回 shared_ptr，失败返回空指针，同时输出错误
 * FIXME make_digest 返回 Digest，失败返回空对象，同时输出错误
 * FIXME hash_file std::optional<std::string>＋错误参数
 * FIXME Receiver构造 改用工厂函数报告初始化失败
 * */

#include "common/net/buffer.h"
#include "common/net/tcp_connection.h"
#include "ftp/transfer_logger.h"

#include <bits/types/idtype_t.h>
#include <openssl/evp.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace TLSS::FileStorage::Transfer {
namespace fs = std::filesystem;

inline constexpr const char* kResponsePrefix = "\n@TLSS ";
inline constexpr std::size_t kMaxControlBytes = 4096;

// 这个函数一旦被调用，就不会正常返回到调用者
[[noreturn]] inline void system_failure(const char* operation) {
  const int error = errno;
  transfer_logger()->error("{}", operation);
  throw std::system_error(error, std::generic_category(), operation);
}

struct File {
  int fd = -1;

  File() = default;
  File(const File&) = delete;
  File& operator=(const File&) = delete;

  ~File() {
    reset();
  }

  void reset(int value = -1) {
    if (fd >= 0) {
      ::close(fd);
    }
    fd = value;
  }
};

struct TemporaryPath {
  std::string path;

  TemporaryPath() = default;
  TemporaryPath(const TemporaryPath&) = delete;
  TemporaryPath& operator=(const TemporaryPath&) = delete;

  ~TemporaryPath() {
    if (!path.empty()) {
      unlink(path.c_str());
    }
  }

  void remove() {
    if (!path.empty()) {
      if (::unlink(path.c_str()) == -1 && errno != ENOENT) {
        system_failure("unlink temporary file");
      }
      path.clear();
    }
  }
};

inline std::string quote(const std::string& value) {
  std::ostringstream out;
  out << std::quoted(value);
  return out.str();
}

inline std::string safe_error(std::string value) {
  for (char& ch : value) {
    const auto byte = static_cast<unsigned char>(ch);
    if (byte < 32 || byte == 127) {
      ch = ' ';
    }
  }
  return value;
}

inline void send_control(const NET::TcpConnectionPtr& conn, const std::string& payload) {
  conn->send(std::string(kResponsePrefix) + payload + "\n");
}

inline void require_end(std::istream& input) {
  input >> std::ws;
  if (!input.eof()) {
    transfer_logger()->error("unexpected trailing arguments");
    throw std::runtime_error("unexpected trailing arguments");
  }
}

inline std::string read_field(std::istream& input) {
  std::string value;
  if (!(input >> std::quoted(value))) {
    transfer_logger()->error("missing arguments");
    throw std::runtime_error("missing argument");
  }
  return value;
}

inline bool valid_name(const std::string& name) {
  if (name.empty() || name.size() > 255 || name == "." || name == "..") {
    return false;
  }

  bool res = std::all_of(name.begin(), name.end(), [](unsigned char ch) {
    return ch >= 32 && ch != 127 && ch != '/' && ch != '\\';
  });

  /* for (unsigned char ch : name) { */
  /* if (ch < 32 || ch == 127 || ch == '/' || ch == '\\') { */
  /* return false; */
  /* } */
  /* } */
  return res;
}

inline bool valid_hash(const std::string& hash) {
  return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

inline uint64_t parse_size(const std::string& text) {
  uint64_t size = 0;

  const auto result = std::from_chars(text.data(), text.data() + text.size(), size);

  if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      size > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
    transfer_logger()->error("invalid file size");
    throw std::runtime_error("invalid file size");
  }

  return size;
}

inline fs::path object_root() {
  static const fs::path root = [] {
    const char* configured = std::getenv("TLSS_STORAGE_ROOT");
    return fs::absolute(configured && *configured ? fs::path(configured)
                                                  : fs::path("./data/objects"));
  }();

  fs::create_directories(root);
  return root;
}

struct SourceFile {
  File file;
  uint64_t size = 0;
};

inline std::shared_ptr<SourceFile> open_source(const fs::path& path, bool no_followd = false) {
  auto source = std::make_shared<SourceFile>();
  // Avoid blocking on a FIFO before fstat can reject special files.
  const int flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK | (no_followd ? O_NOFOLLOW : 0);
  int fd;

  do {
    fd = ::open(path.c_str(), flags);
  } while (fd == -1 && errno == EINTR);

  if (fd == -1) {
    system_failure("open source");
  }
  source->file.reset(fd);

  struct stat info {};
  int result;

  do {
    result = ::fstat(fd, &info);
  } while (result == -1 && errno == EINTR);

  if (result == -1) {
    system_failure("fstat source");
  }
  if (!S_ISREG(info.st_mode) || info.st_size < 0) {
    throw std::runtime_error("source is not a regular file");
  }
  source->size = static_cast<uint64_t>(info.st_size);
  return source;
}

using Digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

inline Digest make_digest() {
  Digest digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1) {
    transfer_logger()->error("SHA-256 initialization failed");
    throw std::runtime_error("SHA-256 initialization failed");
  }

  return digest;
}

inline void update_digest(EVP_MD_CTX* digest, const void* data, std::size_t size) {
  if (EVP_DigestUpdate(digest, data, size) != 1) {
    transfer_logger()->error("SHA-256 update failed");
    throw std::runtime_error("SHA-256 update failed");
  }
}

inline std::string finish_digest(EVP_MD_CTX* digest) {
  unsigned char bytes[EVP_MAX_MD_SIZE];
  unsigned int length = 0;

  if (EVP_DigestFinal_ex(digest, bytes, &length) != 1 || length != 32) {
    transfer_logger()->error("SHA-256 finalization failed");
    throw std::runtime_error("SHA-256 finalization failed");
  }

  constexpr char digits[] = "0123456789abcdef";
  std::string hash;
  hash.reserve(length * 2);

  for (unsigned int i = 0; i < length; ++i) {
    hash.push_back(digits[bytes[i] >> 4]);
    hash.push_back(digits[bytes[i] & 15]);
  }

  return hash;
}

inline std::string hash_file(int fd) {
  auto digest = make_digest();
  char buffer[65536];
  off_t offset = 0;

  for (;;) {
    const ssize_t n = ::pread(fd, buffer, sizeof buffer, offset);

    if (n > 0) {
      update_digest(digest.get(), buffer, static_cast<std::size_t>(n));
      offset += n;
    } else if (n == 0) {
      return finish_digest(digest.get());
    } else if (errno != EINTR) {
      system_failure("pread");
    }
  }
}

inline void sync_file(int fd) {
  int result;
  do {
    result = ::fsync(fd);
  } while (result == -1 && errno == EINTR);

  if (result == -1) {
    system_failure("fsync");
  }
}

inline void sync_directory(const fs::path& directory) {
  File file;

  file.fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);

  if (file.fd == -1) {
    system_failure("open directory");
  }

  sync_file(file.fd);
}

class Receiver {
 public:
  Receiver(const fs::path& directory, uint64_t size)
      : total_(size)
      , remaining_(size)
      , digest_(make_digest()) {
    if (size > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
      transfer_logger()->error("file too large");
      throw std::runtime_error("file too large");
    }

    if (!fs::is_directory(directory)) {
      transfer_logger()->error("destination directory does not exist");
      throw std::runtime_error("destination directory does not exist");
    }

    temporary_.path = (directory / ".tlss-part-XXXXXX").string();

    file_.fd = ::mkostemp(temporary_.path.data(), O_CLOEXEC);

    if (file_.fd == -1) {
      system_failure("mkostemp");
    }
  }

  uint64_t remaining() const {
    return remaining_;
  }

  void consume(NET::Buffer& buffer) {
    const auto count =
        static_cast<std::size_t>(std::min<uint64_t>(remaining_, buffer.readable_bytes()));

    if (count == 0) {
      return;
    }

    const char* data = buffer.peek();
    std::size_t written = 0;

    while (written < count) {
      const ssize_t n = ::write(file_.fd, data + written, count - written);

      if (n > 0) {
        written += static_cast<std::size_t>(n);
      } else if (n == -1 && errno == EINTR) {
        continue;
      } else if (n == 0) {
        transfer_logger()->error("file write made no progress");
        throw std::runtime_error("file write made no progress");
      } else {
        system_failure("write file");
      }
    }

    update_digest(digest_.get(), data, count);
    buffer.retrieve(count);
    remaining_ -= count;
  }

  const std::string& hash() {
    if (remaining_ != 0) {
      transfer_logger()->error("file is incomplete");
      throw std::runtime_error("file is incomplete");
    }

    if (hash_.empty()) {
      hash_ = finish_digest(digest_.get());
    }
    return hash_;
  }

  // destination 必须与临时文件处于同一文件系统。
  // reuse_existing 用于服务端按内容哈希去重。
  void publish(const fs::path& destination, bool reuse_existing, const std::string& expected_hash,
               mode_t mode) {
    const std::string actual_hash = hash();

    if (!expected_hash.empty() && actual_hash != expected_hash) {
      transfer_logger()->error("SHA-256 mismatch");
      throw std::runtime_error("SHA-256 mismatch");
    }

    if (::fchmod(file_.fd, mode) == -1) {
      system_failure("fchmod");
    }

    sync_file(file_.fd);

    if (::link(temporary_.path.c_str(), destination.c_str()) == -1) {
      if (errno != EEXIST || !reuse_existing) {
        system_failure("publish file");
      }

      // 已有对象不能盲目接受，更不能覆盖。
      auto existing = open_source(destination, true);

      if (existing->size != total_ || hash_file(existing->file.fd) != actual_hash) {
        transfer_logger()->error("existing object is inconsistent");
        throw std::runtime_error("existing object is inconsistent");
      }
    }

    temporary_.remove();
    file_.reset();

    auto parent = destination.parent_path();
    sync_directory(parent.empty() ? fs::path(".") : parent);
  }

 private:
  uint64_t total_;
  uint64_t remaining_;

  File file_;
  TemporaryPath temporary_;
  Digest digest_;
  std::string hash_;
};

}  // namespace TLSS::FileStorage::Transfer
#endif  // __INC_FTP_FILE_TRANSFER_H__
