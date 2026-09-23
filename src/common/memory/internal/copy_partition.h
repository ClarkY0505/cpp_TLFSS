#ifndef __COPY_PARTITION_H__
#define __COPY_PARTITION_H__
#include <cstddef>


namespace TLSS::MEMORY::INTERNAL {

struct CopyPartition {
  std::size_t prefix_size{0};
  std::size_t body_size{0};
  std::size_t tail_size{0};
};

CopyPartition make_copy_partition(
    const void* dst,
    std::size_t size,
    std::size_t worker_count) noexcept;

}  // namespace TLSS::MEMORY::INTERNAL
#endif // __COPY_PARTITION_H__
