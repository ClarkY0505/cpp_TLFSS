#include "cpu_topology.h"

#include <fstream>
#include <string>

namespace TLSS::MEMORY::INTERNAL {

namespace {

bool read_integer_file(const std::string& path, int& value) {
  std::ifstream file(path);

  if (!file.is_open()) {
    return false;
  }

  file >> value;

  return !file.fail();
}

}  // namespace

CpuTopology detect_cpu_topology(const std::vector<int>& available_cpu_ids) {
  CpuTopology topology;

  topology.cpus.reserve(available_cpu_ids.size());

  for (int cpu_id : available_cpu_ids) {
    CpuInfo cpu_info{};

    cpu_info.cpu_id = cpu_id;

#if defined(__linux__)

    const std::string topology_path =
        "/sys/devices/system/cpu/cpu" + std::to_string(cpu_id) + "/topology/";
    const bool package_ok =
        read_integer_file(topology_path + "physical_package_id", cpu_info.package_id);
    const bool core_ok = read_integer_file(topology_path + "core_id", cpu_info.core_id);
    cpu_info.topology_known = package_ok && core_ok;

#endif

    topology.cpus.push_back(cpu_info);
  }

  for (const CpuInfo& cpu : topology.cpus) {
    if (!cpu.topology_known) {
      continue;
    }

    PhysicalCore* physical_core = nullptr;

    for (PhysicalCore& core : topology.physical_cores) {
      if (core.package_id == cpu.package_id && core.core_id == cpu.core_id) {
        physical_core = &core;

        break;
      }
    }

    //
    // 第一次遇到这个 physical core（物理核心）。
    //
    if (physical_core == nullptr) {
      topology.physical_cores.push_back(
          PhysicalCore{cpu.package_id, cpu.core_id, CpuCoreType::Unknown, {}});

      physical_core = &topology.physical_cores.back();
    }
    physical_core->logical_cpu_ids.push_back(cpu.cpu_id);
  }

  // 检测 physical core type（物理核心类型）
  for (PhysicalCore& core : topology.physical_cores) {
    if (core.logical_cpu_ids.empty()) {
      continue;
    }

    const int representative_cpu = core.logical_cpu_ids.front();

    core.core_type = detect_cpu_core_type(representative_cpu);
  }

  return topology;
}

const PhysicalCore* find_physical_core(const CpuTopology& topology, int cpu_id) noexcept {
  for (const PhysicalCore& core : topology.physical_cores) {
    for (int logical_cpu_id : core.logical_cpu_ids) {
      if (logical_cpu_id == cpu_id) {
        return &core;
      }
    }
  }

  return nullptr;
}

WorkerCandidates select_worker_candidates(const CpuTopology& topology, int caller_cpu_id) {
  WorkerCandidates candidates;
  const PhysicalCore* caller_core = find_physical_core(topology, caller_cpu_id);

  for (const PhysicalCore& core : topology.physical_cores) {
    //
    // 排除 caller 整个 physical core
    if (caller_core != nullptr && core.package_id == caller_core->package_id &&
        core.core_id == caller_core->core_id) {
      continue;
    }

    if (core.logical_cpu_ids.empty()) {
      continue;
    }

    // 只选择一个 logical CPU
    const int cpu_id = core.logical_cpu_ids.front();

    switch (core.core_type) {
      case CpuCoreType::IntelCore:
        candidates.intel_core_cpu_ids.push_back(cpu_id);
        break;

      case CpuCoreType::IntelAtom:
        candidates.intel_atom_cpu_ids.push_back(cpu_id);
        break;

      case CpuCoreType::Unknown:
        candidates.unknown_cpu_ids.push_back(cpu_id);
        break;
    }
  }

  return candidates;
}

}  // namespace TLSS::MEMORY::INTERNAL
