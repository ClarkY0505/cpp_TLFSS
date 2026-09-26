#ifndef __CPU_AFFINITY_H__
#define __CPU_AFFINITY_H__

#include <vector>

namespace TLSS::MEMORY::INTERNAL {

std::vector<int> detect_available_cpu_ids();

}  // namespace TLSS::MEMORY::INTERNAL
#endif // __CPU_AFFINITY_H__
