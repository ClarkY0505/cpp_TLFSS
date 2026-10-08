#include "common/base/current_thread.h"
#include "common/net/buffer.h"
#include "common/net/net_logger.h"
#include "common/utile/utile.h"

#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdlib>

namespace TLSS::NET {
void Buffer::retrieve(size_t len) {
  if (len < readable_bytes()) {
    // 应用只读取类缓冲区数据的一部分，长度为len,
    // 还剩下 _reader_idx += len 数据未读”应改成“读索引前进 len，
    // 剩余可读字节数减少 len
    _reader_idx += len;
  } else {
    retrieve_all();
  }
}

void Buffer::retrieve_all() {
  _reader_idx = k_cheap_prepend;
  _writer_idx = k_cheap_prepend;
}

std::string Buffer::retrieve_all_as_string() {
  return retrieve_as_string(readable_bytes());
}

std::string Buffer::retrieve_as_string(size_t len) {
  if (len > readable_bytes()) {
    net_logger()->critical("Read length exceeds readable bytes.");
    std::abort();
  }
  std::string res(peek(), len);
  retrieve(len);
  return res;
}

void Buffer::make_space(size_t len) {
  /**
   * k_cheap_prepend |  reader  |  writer  |
   * k_cheap_prepend |        len             |
   */
  // 在reader区域，在应用读取的时候，可能只读取一部分
  // 获取reader的索引加上可写区域 如果小于 要写入的总长度 加 k_cheap_prepend的长度
  // 直接扩容
  if (writeable_bytes() + prependable_bytes() < len + k_cheap_prepend) {
    _buffer.resize(_writer_idx + len);
  } else {
    // 如果说buffer整体空闲区域大于要写入的长度
    // 挪动reader数据，整合缓冲区
    size_t readable = readable_bytes();
    std::copy(begin() + _reader_idx, begin() + _writer_idx, begin() + k_cheap_prepend);
    _reader_idx = k_cheap_prepend;
    _writer_idx = _reader_idx + readable;
  }
}

void Buffer::append(const char* data, size_t len) {
  ensure_writeable_bytes(len);
  std::copy(data, data + len, begin_write());
  has_written(len);
}

void Buffer::has_written(size_t len) {
  if (len > writeable_bytes()) {
    net_logger()->critical("Out of writable range");
    std::abort();
  }
  _writer_idx += len;
}

ssize_t Buffer::read_fd(int fd, int* save_err) {
  // 栈上内存空间
  char extrabuf[65536];
  UTIL::memzero(extrabuf, sizeof extrabuf);
  iovec vec[2];
  // 这里获取Buffer底层缓冲区剩余可写的空间大小
  const size_t writable = writeable_bytes();
  // 第一块缓冲区
  vec[0].iov_base = begin() + _writer_idx;
  vec[0].iov_len = writable;
  // 第二快缓冲区
  vec[1].iov_base = extrabuf;
  vec[1].iov_len = sizeof extrabuf;

  // 如果内部缓冲区小于64k数据选择两块缓冲区来存储
  // wrritable + extrabuf
  // 如果内部缓冲区大于64k选择内部缓冲区来存储
  const int iovcnt = (writable < sizeof extrabuf) ? 2 : 1;
  const ssize_t n = ::readv(fd, vec, iovcnt);
  if (n < 0) {
    *save_err = errno;
    net_logger()->error("{}:readv failed", BASE::CurrentThread::tid());
  } else if (UTIL::implicit_cast<size_t>(n) <= writable) {
    // 此时说明内部缓冲区足够存取空间
    _writer_idx += n;
  } else {
    // 此时说明当前占用了临时缓冲区extrabuf
    _writer_idx = _buffer.size();
    append(extrabuf, n - writable);
  }
  return n;
}

ssize_t Buffer::write_fd(int fd, int* save_err) {
  ssize_t n = ::write(fd, peek(), readable_bytes());
  if (n < 0) {
    *save_err = errno;
    net_logger()->error("{}:write failed", BASE::CurrentThread::tid());
  }
  return n;
}
}  // namespace TLSS::NET
