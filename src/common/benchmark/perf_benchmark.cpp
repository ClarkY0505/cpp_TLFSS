#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

extern "C" void* avx2_memcpy(void* dst, const void* src, std::size_t n);

using CopyFunction = void* (*)(void*, const void*, std::size_t);

__attribute__((noinline)) void* libc_memcpy(void* dst, const void* src, std::size_t n) {
  return std::memcpy(dst, src, n);
}

volatile std::uint8_t sink = 0;

int main(int argc, char* argv[]) {
  if (argc != 4) {
    std::cerr << "Usage:\n"
              << "  ./perf_benchmark " << "<libc|avx2> " << "<size> " << "<offset>\n";

    return 1;
  }

  const std::string type = argv[1];

  const std::size_t size = std::stoull(argv[2]);

  const std::size_t offset = std::stoull(argv[3]);

  CopyFunction copy_func = nullptr;

  if (type == "libc") {
    copy_func = libc_memcpy;
  } else if (type == "avx2") {
    copy_func = avx2_memcpy;
  } else {
    std::cerr << "Unknown memcpy type\n";
    return 1;
  }

  constexpr std::size_t alignment = 64;

  std::uint8_t* src_base = nullptr;
  std::uint8_t* dst_base = nullptr;

  const std::size_t buffer_size = size + offset + 64;

  if (posix_memalign(reinterpret_cast<void**>(&src_base), alignment, buffer_size) != 0) {
    return 1;
  }

  if (posix_memalign(reinterpret_cast<void**>(&dst_base), alignment, buffer_size) != 0) {
    std::free(src_base);
    return 1;
  }

  auto* src = src_base + offset;
  auto* dst = dst_base + offset;

  for (std::size_t i = 0; i < size; ++i) {
    src[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xff);
  }

  std::memset(dst, 0, size);

  // ---------------------------------------------
  // 根据 size 控制总工作量
  // 大概让每个测试复制若干 GiB
  // ---------------------------------------------

  constexpr std::size_t target = 4ULL * 1024ULL * 1024ULL * 1024ULL;

  std::size_t iterations = target / size;

  if (iterations < 100) {
    iterations = 100;
  }

  // warm-up
  for (int i = 0; i < 1000; ++i) {
    copy_func(dst, src, size);
  }

  for (std::size_t i = 0; i < iterations; ++i) {
    copy_func(dst, src, size);
  }

  sink = dst[size - 1];

  std::cout << "type=" << type << " size=" << size << " offset=" << offset
            << " iterations=" << iterations << '\n';

  std::free(src_base);
  std::free(dst_base);

  return 0;
}
