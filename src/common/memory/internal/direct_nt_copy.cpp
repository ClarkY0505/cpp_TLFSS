#include "direct_nt_copy.h"

#include <cstddef>
#include <cstdint>

#include "copy_partition.h"

extern "C" {

void* tlss_avx2_memcpy_5(void* dst, const void* src, std::size_t size);

void* tlss_avx2_nt_memcpy_2stream(void* dst, const void* src, std::size_t size);
}

namespace TLSS::MEMORY::INTERNAL {

void* direct_nt_copy(void* dst, const void* src, std::size_t size) {
  if (size == 0) {
    return dst;
  }

  auto* dst_bytes = static_cast<std::uint8_t*>(dst);

  const auto* src_bytes = static_cast<const std::uint8_t*>(src);

  //
  // worker_count = 1
  //
  // 对 Direct NT（直接非临时复制）来说，
  // body 只需要满足单个 8192B block 粒度。
  //
  const CopyPartition partition = make_copy_partition(dst, size, 1);

  //
  // 如果连一个完整的 8192B NT block 都没有，
  // 整体直接使用 cached AVX2（缓存型 AVX2）。
  //
  if (partition.body_size == 0) {
    return tlss_avx2_memcpy_5(dst, src, size);
  }

  // Prefix（前缀）
  if (partition.prefix_size != 0) {
    tlss_avx2_memcpy_5(dst_bytes, src_bytes, partition.prefix_size);
  }

  // NT body（非临时主体）
  tlss_avx2_nt_memcpy_2stream(dst_bytes + partition.prefix_size, src_bytes + partition.prefix_size,
                              partition.body_size);

  // Tail
  if (partition.tail_size != 0) {
    const std::size_t tail_offset = partition.prefix_size + partition.body_size;

    tlss_avx2_memcpy_5(dst_bytes + tail_offset, src_bytes + tail_offset, partition.tail_size);
  }

  return dst;
}

}  // namespace TLSS::MEMORY::INTERNAL
