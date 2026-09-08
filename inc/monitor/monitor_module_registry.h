#ifndef __MONITOR_MODULE_REGISTRY_H__
#define __MONITOR_MODULE_REGISTRY_H__

#include "monitor_module.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace TLSSMON {

/*
 * 受监控模块注册表。
 *
 * Registry 同时维护：
 *
 * 1. mid -> MonitorModuleInfo
 * 2. name -> mid
 *
 * 两张表由同一个互斥锁保护，保证注册操作是一个整体。
 */
class MonitorModuleRegistry final {
public:
  MonitorModuleRegistry() = default;
  ~MonitorModuleRegistry() = default;

  MonitorModuleRegistry(const MonitorModuleRegistry &) = delete;

  MonitorModuleRegistry &operator=(const MonitorModuleRegistry &) = delete;

  MonitorModuleRegistry(MonitorModuleRegistry &&) = delete;

  MonitorModuleRegistry &operator=(MonitorModuleRegistry &&) = delete;

  /*
   * 注册一个模块。
   *
   * 注册成功后，mid 和名称都不能再被其他模块使用。
   * 重复注册不会覆盖原有模块。
   */
  ModuleRegisterStatus register_module(MonitorModuleInfo module);

  /*
   * 根据 mid 查询模块。
   *
   * 返回独立副本；未找到时返回 std::nullopt。
   */
  std::optional<MonitorModuleInfo> find_by_id(std::uint32_t mid) const;

  /*
   * 根据区分大小写的模块名查询。
   *
   * 返回独立副本；未找到时返回 std::nullopt。
   */
  std::optional<MonitorModuleInfo> find_by_name(std::string_view name) const;

  /*
   * 返回所有模块的独立快照。
   *
   * 返回顺序稳定为模块名字典序。
   */
  std::vector<MonitorModuleInfo> modules() const;

private:
  mutable std::mutex _mutex;

  /*
   * 保存模块的完整信息，支持按 mid 查询。
   */
  std::map<std::uint32_t, MonitorModuleInfo> _by_id;

  /*
   * 按模块名排序，同时支持名称到 mid 的查询。
   */
  std::map<std::string, std::uint32_t> _id_by_name;
};

} // namespace TLSSMON

#endif // __MONITOR_MODULE_REGISTRY_H__
