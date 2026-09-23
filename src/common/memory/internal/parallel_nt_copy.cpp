#include "parallel_nt_copy.h"

#include "copy_partition.h"
#include "parallel_copy_pool.h"

#include <cstddef>
#include <cstdint>

extern "C" {

void* tlss_avx2_memcpy_5(void* dst, const void* src, std::size_t size);
}

namespace TLSS::MEMORY::INTERNAL {

void* parallel_nt_copy(ParallelCopyPool& pool,
                       const std::vector<std::size_t>& active_worker_indices, void* dst,
                       const void* src, std::size_t size) {
  if (size == 0) {
    return dst;
  }

  if (active_worker_indices.empty()) {
    return tlss_avx2_memcpy_5(dst, src, size);
  }

  auto* dst_bytes = static_cast<std::uint8_t*>(dst);
  const auto* src_bytes = static_cast<const std::uint8_t*>(src);
  const std::size_t active_worker_count = active_worker_indices.size();
  const CopyPartition partition = make_copy_partition(dst, size, active_worker_count);

  //
  // Prefix（前缀）
  //
  if (partition.prefix_size != 0) {
    tlss_avx2_memcpy_5(dst_bytes, src_bytes, partition.prefix_size);
  }

  const std::size_t tail_offset = partition.prefix_size + partition.body_size;

  //
  // ------------------------------------------------------------
  // Parallel body（并行主体）
  // ------------------------------------------------------------
  //
  if (partition.body_size != 0) {
    //
    // 先发布 Parallel NT 工作。
    //
    pool.dispatch_copy(active_worker_indices, dst_bytes + partition.prefix_size,
                       src_bytes + partition.prefix_size, partition.body_size);

    //
    // P-core workers
    // 正在复制 body 的同时，
    // caller 处理 tail。
    //
    //
    // body 和 tail 是互不重叠的区域，
    // 所以 memcpy 语义下可以并行执行。
    //
    if (partition.tail_size != 0) {
      tlss_avx2_memcpy_5(dst_bytes + tail_offset, src_bytes + tail_offset, partition.tail_size);
    }

    //
    // 最后统一等待 body 完成。
    //
    pool.wait();

    return dst;
  }

  tlss_avx2_memcpy_5(dst, src, size);

  return dst;
}

}  // namespace TLSS::MEMORY::INTERNAL
