#include "monitor_error.h"
#include "monitor_module.h"
#include "monitor_module_registry.h"

#include <cstdint>
#include <ctime>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace TLSSMON {
ModuleRegisterStatus
MonitorModuleRegistry::register_module(MonitorModuleInfo module) {
  if (!is_valid_module_name(module._name)) {
    return ModuleRegisterStatus::INVALID_NAME;
  }

  for (const MonitorErrorInfo &error : module._errors) {
    const auto raw_level = static_cast<std::uint32_t>(error._level);
    if (!is_valid_monitor_level(raw_level)) {
      return ModuleRegisterStatus::INVALID_ERROR_LEVEL;
    }
  }

  /*
   * 在移动 module 之前保存索引所需的值。
   *
   * name 的复制如果抛出异常，此时 Registry 还没有被修改。
   */
  const std::uint32_t mid = module._mid;
  std::string name = module._name;

  std::lock_guard<std::mutex> lock{_mutex};

  /*
   * 固定冲突优先级：
   *
   * 如果 mid 和名称同时冲突，先返回 DUPLICATE_ID。
   */
  if (_by_id.find(mid) != _by_id.end()) {
    return ModuleRegisterStatus::DUPLICATE_ID;
  }

  if (_id_by_name.find(name) != _id_by_name.end()) {
    return ModuleRegisterStatus::DUPLICATE_NAME;
  }

  /*
   * 先写入完整记录。
   */
  const auto id_insertion = _by_id.emplace(mid, std::move(module));

  if (!id_insertion.second) {
    return ModuleRegisterStatus::DUPLICATE_ID;
  }

  try {
    /*
     * 再写入名称索引。
     *
     * 因为整个过程持有同一把锁，不会有其他线程在两次
     * emplace() 之间观察到半注册状态。
     */
    const auto name_insertion = _id_by_name.emplace(std::move(name), mid);

    if (!name_insertion.second) {
      /*
       * 正常情况下前面的重复检查已经排除了这种情况。
       * 仍然保留回滚，确保两张表始终一致。
       */
      _by_id.erase(id_insertion.first);
      return ModuleRegisterStatus::DUPLICATE_NAME;
    }
  } catch (...) {
    /*
     * 如果名称索引分配内存等操作抛出异常，
     * 回滚已经插入的完整记录，然后继续传播异常。
     *
     * 这样 register_module() 具备强异常安全保证：
     * 调用失败时 Registry 状态保持不变。
     */
    _by_id.erase(id_insertion.first);
    throw;
  }

  return ModuleRegisterStatus::SUCCESS;
}

std::optional<MonitorModuleInfo>
MonitorModuleRegistry::find_by_id(std::uint32_t mid) const {
  std::lock_guard<std::mutex> lock{_mutex};

  const auto module = _by_id.find(mid);

  if (module == _by_id.end()) {
    return std::nullopt;
  }

  /*
   * optional 内保存 MonitorModuleInfo 副本，
   * 返回后不再依赖 Registry 的锁和内部节点。
   */
  return module->second;
}

std::optional<MonitorErrorInfo> 
MonitorModuleRegistry::find_error(std::uint32_t mid, std::uint32_t eid) const {
    // EID 是模块错误表的下标；缺少模块或越界都表示没有可用元数据。
    std::lock_guard<std::mutex> lock{_mutex};
    const auto module = _by_id.find(mid);
    if(module == _by_id.end()){
        return std::nullopt;
    }
    const std::vector<MonitorErrorInfo> &errors = module->second._errors;
    if (eid >= errors.size()) {
      return std::nullopt;
    }

    return errors[eid];
}

std::optional<MonitorModuleInfo>
MonitorModuleRegistry::find_by_name(std::string_view name) const {
  std::lock_guard<std::mutex> lock{_mutex};

  /*
   * 当前项目使用 C++17。
   *
   * std::map<std::string, ...> 没有配置透明比较器，
   * TODO 之后添加透明比较器
   * 因此这里显式构造 std::string 进行查询。
   */
  const auto name_entry = _id_by_name.find(std::string{name});

  if (name_entry == _id_by_name.end()) {
    return std::nullopt;
  }

  const auto module = _by_id.find(name_entry->second);

  /*
   * 两张表只在 register_module() 的同一个临界区内修改，
   * 因此正常情况下名称索引一定能找到完整记录。
   */
  if (module == _by_id.end()) {
    return std::nullopt;
  }

  return module->second;
}

std::vector<MonitorModuleInfo> MonitorModuleRegistry::modules() const {
  std::lock_guard<std::mutex> lock{_mutex};

  std::vector<MonitorModuleInfo> snapshot;
  snapshot.reserve(_id_by_name.size());

  /*
   * _id_by_name 是 std::map<std::string, ...>，
   * 迭代顺序天然是模块名字典序。
   */
  for (const auto &name_entry : _id_by_name) {
    const auto module = _by_id.find(name_entry.second);

    if (module != _by_id.end()) {
      snapshot.push_back(module->second);
    }
  }

  /*
   * snapshot 拥有每条 MonitorModuleInfo 的完整副本。
   * 返回后 Registry 锁立即释放，调用者可以安全长期保存。
   */
  return snapshot;
}

} // namespace TLSSMON
