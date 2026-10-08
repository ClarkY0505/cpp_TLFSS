#ifndef __INC_COMMON_NET_BUFFER_H__
#define __INC_COMMON_NET_BUFFER_H__

/* #include <dirent.h> */
#include <sys/types.h>
#include <uchar.h>
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>
namespace TLSS::NET {
//
// 这种设计方式参照与muduo库的buffer,
// 其实muduo库的buffer类也是参照 Java中第三方库 org.jboss.netty.buffer实现的
// TODO 在本项目中存在监控模块，后续会有二进制协议传输，目前只有在监控模块内部存在自定义协议
//
// +-------------------+------------------+------------------+
// | prependable bytes |  readable bytes  |  writable bytes  |
// |                   |     (CONTENT)    |                  |
// +-------------------+------------------+------------------+
// |                   |                  |                  |
// 0      <=      readerIndex   <=   writerIndex    <=     size
class Buffer {
 public:
  static const size_t k_cheap_prepend = 8;
  static const size_t k_initial_size = 1024;

  explicit Buffer(size_t initial_size = k_initial_size)
      : _buffer(k_cheap_prepend + initial_size)
      , _reader_idx(k_cheap_prepend)
      , _writer_idx(k_cheap_prepend) {}

  size_t readable_bytes() const {
    return _writer_idx - _reader_idx;
  }

  size_t writeable_bytes() const {
    return _buffer.size() - _writer_idx;
  }

  size_t prependable_bytes() const {
    return _reader_idx;
  }

  // 返回数据缓冲区中可读数据的起始地址
  const char* peek() const {
    return begin() + _reader_idx;
  }

  // onMessage 中 Buffer类型转换成string类型
  void retrieve(size_t len);
  void retrieve_all();
  // 把onMessage函数上报的buffer数据，转成 string类型的数据返回
  std::string retrieve_all_as_string();
  std::string retrieve_as_string(size_t len);
  //
  // buffer.size - writeidx < len 需要考虑扩容
  void ensure_writeable_bytes(size_t len) {
    if (writeable_bytes() < len) {
      make_space(len);
    }
  }

  void append(const char* data, size_t len);
  char* begin_write() {
    return begin() + _writer_idx;
  }
  const char* begin_write() const {
    return begin() + _writer_idx;
  }
  void has_written(size_t len);

  ssize_t read_fd(int fd, int* save_err);
  ssize_t write_fd(int fd, int* save_err);

 private:
  char* begin() {
    // 这里 & 和 *不会抵消
    // 实际上在容器使用begin()获取类迭代器指针后
    // 迭代器底层重写了operator*()，此时*it只是获取了首元素
    // 等价于以下写法
    // auto it = _buffer.begin();  // 获取指向首元素的迭代器
    // char& first_char = *it;    // 解引用迭代器，得到首元素的引用
    // char* first_ptr = &first_char; // 取首元素的地址
    // return first_ptr;
    //
    // C++17提供了容器data()函数，可以直接返回底层连续存储区域的指针
    // return _buffer.data();
    return &*_buffer.begin();
  }

  const char* begin() const {
    return &*_buffer.begin();
  }

  void make_space(size_t len);
  std::vector<char> _buffer;
  size_t _reader_idx;
  size_t _writer_idx;
};
}  // namespace TLSS::NET
#endif  // __INC_COMMON_NET_BUFFER_H__
