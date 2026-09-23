#include "cpu_affinity.h"

#if defined(__linux__)

#include <sched.h>

#endif

#include <stdexcept>
#include <vector>

namespace TLSS::MEMORY::INTERNAL {

std::vector<int> detect_available_cpu_ids() {
  std::vector<int> cpu_ids;

#if defined(__linux__)

  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);
  const int result = sched_getaffinity(0, sizeof(cpu_set_t), &cpu_set);

  if (result != 0) {
    throw std::runtime_error("sched_getaffinity failed");
  }

  for (int cpu_id = 0; cpu_id < CPU_SETSIZE; ++cpu_id) {
    if (CPU_ISSET(cpu_id, &cpu_set)) {
      cpu_ids.push_back(cpu_id);
    }
  }

#else

  //
  // 当前先只实现 Linux。
  // 其他平台后续提供 platform-specific implementation
  // 至少不是现在
  throw std::runtime_error(
      "CPU affinity detection is not supported "
      "on this platform");

#endif

  if (cpu_ids.empty()) {
    throw std::runtime_error("no available CPUs detected");
  }

  return cpu_ids;
}

}  // namespace TLSS::MEMORY::INTERNAL
