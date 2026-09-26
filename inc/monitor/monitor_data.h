#ifndef __MONITOR_DATA_H__
#define __MONITOR_DATA_H__

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <variant>
namespace TLSSMON{
namespace MonData{
/**
 * @brief 一条监控记录的完整主键。
 *
 * 排序顺序必须保持：
 * mid -> level -> fid -> eid
 *
 */
struct MonitorKey final{
    std::uint32_t _mid{0};      ///< module id
    std::uint32_t _level{0};
    std::uint32_t _fid{0};      ///< function id
    std::uint32_t _eid{0};  ///< event id
};
/** @brief 按主键字段筛选记录；未设置的字段不参与过滤。 */
struct MonitorFilter final {
      /** @brief MonitorKey::_mid。 */
      std::optional<std::uint32_t> module_id;

      /** @brief MonitorKey::_level。 */
      std::optional<std::uint32_t> level;

      /** @brief MonitorKey::_fid。 */
      std::optional<std::uint32_t> function_id;

      /** @brief MonitorKey::_eid。 */
      std::optional<std::uint32_t> event_id;
  };


/**
 * @brief 比较完整主键的四个字段。
 * @param lhs 左操作数。
 * @param rhs 右操作数。
 * @return 比较条件成立时返回 true，否则返回 false。
 */
inline bool operator==(const MonitorKey& lhs, const MonitorKey& rhs) noexcept{
    return std::tie(lhs._mid, lhs._level, lhs._fid, lhs._eid) == std::tie(rhs._mid, rhs._level, rhs._fid, rhs._eid);
}

/**
 * @brief 判断完整主键是否不同。
 * @param lhs 左操作数。
 * @param rhs 右操作数。
 * @return 比较条件成立时返回 true，否则返回 false。
 */
inline bool operator!=(const MonitorKey& lhs, const MonitorKey& rhs) noexcept{
    return !(lhs == rhs);
}

/**
 * @brief 按 mid、level、fid、eid 顺序比较主键。
 * @param lhs 左操作数。
 * @param rhs 右操作数。
 * @return 比较条件成立时返回 true，否则返回 false。
 */
inline bool operator<(const MonitorKey& lhs, const MonitorKey& rhs) noexcept{
    return std::tie(lhs._mid, lhs._level, lhs._fid, lhs._eid) < std::tie(rhs._mid, rhs._level, rhs._fid, rhs._eid);
}


/**
 * @brief 数值型监控值。
 *
 * _value 对应旧 M4 的 sv_num。
 * _state 对应旧 M4 的 sv_state：
 *
 * 0：off
 * 1：stale
 * 2：fresh
 *
 */
struct NumericValue {
    std::uint32_t _value{0};
    std::uint32_t _state{0};
};
/**
 * @brief 同时比较数值和状态。
 * @param lhs 左操作数。
 * @param rhs 右操作数。
 * @return 比较条件成立时返回 true，否则返回 false。
 */
inline bool operator==(const NumericValue& lhs, const NumericValue& rhs) noexcept
{
    return lhs._value == rhs._value && lhs._state == rhs._state;
}

/**
 * @brief 判断数值或状态是否不同。
 * @param lhs 左操作数。
 * @param rhs 右操作数。
 * @return 比较条件成立时返回 true，否则返回 false。
 */
inline bool operator!=(const NumericValue& lhs, const NumericValue& rhs) noexcept
{
    return !(lhs == rhs);
}

/**
 * @brief 监控值只允许是数值或字符串。
 */
using MonitorValue = std::variant<NumericValue, std::string>;

/**
 * @brief 最后变化时间使用系统时钟。
 *
 * TimerManager 的 deadline 使用 steady_clock；
 * 监控数据时间戳表示实际发生时间，因此使用 system_clock。
 */
using MonitorTimestamp = std::chrono::system_clock::time_point;

/**
 * @brief 调用方提交给 MonitorStore 的完整数据。
 */
struct MonitorData {
    MonitorKey _key;
    std::string _description;
    MonitorValue _value;
};

/**
 * @brief MonitorStore 内部保存的记录。
 *
 * _changed_at 只在首次插入或值实际变化时更新。
 */
struct StoredRecord final {
    MonitorData _data;
    MonitorTimestamp _changed_at;
};

/**
 * @brief 一次写入操作的结果。
 */
enum class UpdateStatus : std::uint8_t {
    INSERTED,             ///< 首次插入记录。
    UPDATED,              ///< 已有记录的值发生变化。
    UNCHANGED,            ///< 值未变化。
    IGNORED_INITIAL_ZERO, ///< 按 Store 规则忽略初始零值。
    INVALID,              ///< 输入或当前 Engine 阶段不允许写入。
    /**
     * @brief 数据原本需要插入或更新，但提交前的持久化操作失败。
     *
     * Store 中的数据没有发生变化。
     */
    DURABILITY_FAILED
};

/**
 * @brief MonitorStore::update() 的返回值。
 *
 * _record 使用值语义，不能暴露 Store 内部对象的裸指针。
 */
struct UpdateResult final {
    UpdateStatus _status{UpdateStatus::INVALID};
    std::optional<StoredRecord> _record;

    /**
     * @brief 判断本次是否插入或更新了记录。
     * @return 本次写入插入或更新记录时返回 true。
     */
    constexpr bool changed() const noexcept
    {
        return _status == UpdateStatus::INSERTED || _status == UpdateStatus::UPDATED;
    }
};


}
}
#endif // __MONITOR_DATA_H__
