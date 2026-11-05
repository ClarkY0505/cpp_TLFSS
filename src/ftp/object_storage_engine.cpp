#include "ftp/object_storage_engine.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <system_error>

#include "common/net/buffer.h"
#include "ftp/transfer_logger.h"
#include "ftp/transfer_validation.h"

namespace TLSS::FileStorage::Transfer {
[[noreturn]] void system_failure(const char* operation) {
  const int error = errno;
  transfer_logger()->error("{}", operation);
  throw std::system_error(error, std::generic_category(), operation);
}

File::~File() {
  reset();
}
void File::reset(int value) {
  if (fd >= 0)
    ::close(fd);
  fd = value;
}
TemporaryPath::~TemporaryPath() {
  if (!path.empty())
    ::unlink(path.c_str());
}
void TemporaryPath::remove() {
  if (!path.empty()) {
    if (::unlink(path.c_str()) == -1 && errno != ENOENT)
      system_failure("unlink temporary file");
    path.clear();
  }
}

fs::path object_root() {
  static const fs::path root = [] {
    const char* configured = std::getenv("TLSS_STORAGE_ROOT");
    return fs::absolute(configured && *configured ? fs::path(configured)
                                                  : fs::path("./data/objects"));
  }();

  fs::create_directories(root);
  return root;
}

std::shared_ptr<SourceFile> open_source(const fs::path& path, bool no_followd) {
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

std::string hash_file(int fd) {
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

void sync_file(int fd) {
  int result;
  do {
    result = ::fsync(fd);
  } while (result == -1 && errno == EINTR);

  if (result == -1) {
    system_failure("fsync");
  }
}

void sync_directory(const fs::path& directory) {
  File file;

  file.fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);

  if (file.fd == -1) {
    system_failure("open directory");
  }

  sync_file(file.fd);
}

Receiver::Receiver(const fs::path& directory, uint64_t size)
    : total_(size)
    , remaining_(size)
    , verifier_(size) {
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

void Receiver::append(const char* data, std::size_t count) {
  if (count > remaining_) {
    throw std::runtime_error("received more bytes than declared");
  }

  if (count == 0) {
    return;
  }

  std::size_t written = 0;

  while (written < count) {
    const ssize_t n = ::write(file_.fd, data + written, count - written);

    if (n > 0) {
      written += static_cast<std::size_t>(n);
    } else if (n == -1 && errno == EINTR) {
      continue;
    } else if (n == 0) {
      throw std::runtime_error("file write made no progress");
    } else {
      system_failure("write file");
    }
  }

  verifier_.append(data, count);
  remaining_ -= count;
}

void Receiver::consume(NET::Buffer& buffer) {
  const auto count =
      static_cast<std::size_t>(std::min<uint64_t>(remaining_, buffer.readable_bytes()));

  append(buffer.peek(), count);
  buffer.retrieve(count);
}

const std::string& Receiver::hash() {
  if (remaining_ != 0) {
    transfer_logger()->error("file is incomplete");
    throw std::runtime_error("file is incomplete");
  }

  return verifier_.finish();
}

// destination 必须与临时文件处于同一文件系统。
// reuse_existing 用于服务端按内容哈希去重。
void Receiver::publish(const fs::path& destination, bool reuse_existing,
                       const std::string& expected_hash, mode_t mode) {
  const std::string actual_hash = hash();

  verifier_.verify(expected_hash);

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

}  // namespace TLSS::FileStorage::Transfer

namespace TLSS::FileStorage {
ObjectStorageEngine::WriteHandle ObjectStorageEngine::create_temporary(std::uint64_t size) const {
  return std::make_unique<Transfer::Receiver>(Transfer::object_root(), size);
}
ObjectInfo ObjectStorageEngine::publish(Transfer::Receiver& writer) const {
  const std::string hash = writer.hash();
  writer.publish(Transfer::object_root() / hash, true, hash, 0444);
  return {hash, writer.size()};
}
ObjectStorageEngine::ReadHandle ObjectStorageEngine::open_read(const std::string& hash) const {
  if (!Transfer::valid_hash(hash))
    throw std::invalid_argument("invalid object hash");
  return Transfer::open_source(Transfer::object_root() / hash, true);
}
std::string ObjectStorageEngine::read(const ReadHandle& source, off_t offset, std::size_t count,
                                      const std::function<bool()>& cancelled) {
  if (!source || offset < 0 || static_cast<std::uint64_t>(offset) > source->size ||
      count > source->size - static_cast<std::uint64_t>(offset)) {
    throw std::system_error(EINVAL, std::generic_category(), "invalid file read");
  }
  std::string data(count, '\0');
  std::size_t done = 0;
  while (done < count) {
    if (cancelled && cancelled()) {
      throw std::system_error(ECANCELED, std::generic_category(), "file read cancelled");
    }
    const ssize_t n = ::pread(source->file.fd, data.data() + done, count - done, offset + done);
    if (n > 0)
      done += static_cast<std::size_t>(n);
    else if (n == -1 && errno == EINTR)
      continue;
    else
      throw std::system_error(n == 0 ? EIO : errno, std::generic_category(), "pread");
  }
  return data;
}
}  // namespace TLSS::FileStorage
