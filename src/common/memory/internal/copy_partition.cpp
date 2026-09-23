#include "copy_partition.h"

#include <cstdint>

namespace TLSS::MEMORY::INTERNAL {

CopyPartition make_copy_partition(const void* dst, std::size_t size,
                                  std::size_t worker_count) noexcept {
  constexpr std::size_t k_nt_alignment = 32;
  constexpr std::size_t k_nt_block_size = 8192;

  if (dst == nullptr || size == 0 || worker_count == 0) {
    return {};
  }

  const auto dst_address = reinterpret_cast<std::uintptr_t>(dst);
  const std::size_t misalignment = dst_address & (k_nt_alignment - 1);
  std::size_t prefix_size = 0;

  if (misalignment != 0) {
    prefix_size = k_nt_alignment - misalignment;
  }

  if (prefix_size > size) {
    prefix_size = size;
  }

  const std::size_t remaining_size = size - prefix_size;

  // 有多少个完整的 8192B NT block（非临时块）。
  const std::size_t block_count = remaining_size / k_nt_block_size;

  // 当前 ParallelCopyPool（并行复制线程池）
  // 仍然要求每个 worker 至少获得一个 block。
  //
  // 例如：
  // 7 blocks + 8 workers
  // 不能调用当前固定 8W pool。
  if (block_count < worker_count) {
    return {prefix_size, 0, remaining_size};
  }

  // 不再按照：
  // worker_count * 8192
  // 向下取整。
  const std::size_t body_size = block_count * k_nt_block_size;
  const std::size_t tail_size = remaining_size - body_size;

  return {prefix_size, body_size, tail_size};
}

}  // namespace TLSS::MEMORY::INTERNAL
