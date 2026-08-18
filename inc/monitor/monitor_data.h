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
 * 一条监控记录的完整主键。
 *
 * 排序顺序必须保持：
 * mid -> level -> fid -> eid
 *
 */
struct MonitorKey final{
    std::uint32_t _mid{0};      // module id
    std::uint32_t _level{0};
    std::uint32_t _fid{0};      // function id
    std::uint32_t _eid{0};  // event id
};
struct MonitorFilter final {
      // MonitorKey::_mid。
      std::optional<std::uint32_t> module_id;

      // MonitorKey::_level。
      std::optional<std::uint32_t> level;

      // MonitorKey::_fid。
      std::optional<std::uint32_t> function_id;

      // MonitorKey::_eid。
      std::optional<std::uint32_t> event_id;
  };


inline bool operator==(const MonitorKey& lhs, const MonitorKey& rhs) noexcept{
    return std::tie(lhs._mid, lhs._level, lhs._fid, lhs._eid) == std::tie(rhs._mid, rhs._level, rhs._fid, rhs._eid);
}

inline bool operator!=(const MonitorKey& lhs, const MonitorKey& rhs) noexcept{
    return !(lhs == rhs);
}

inline bool operator<(const MonitorKey& lhs, const MonitorKey& rhs) noexcept{
    return std::tie(lhs._mid, lhs._level, lhs._fid, lhs._eid) < std::tie(rhs._mid, rhs._level, rhs._fid, rhs._eid);
}


/**
 * 数值型监控值。
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
inline bool operator==(const NumericValue& lhs, const NumericValue& rhs) noexcept
{
    return lhs._value == rhs._value && lhs._state == rhs._state;
}

inline bool operator!=(const NumericValue& lhs, const NumericValue& rhs) noexcept
{
    return !(lhs == rhs);
}

/**
 * 监控值只允许是数值或字符串。
 */
using MonitorValue = std::variant<NumericValue, std::string>;

/**
 * 最后变化时间使用系统时钟。
 *
 * TimerManager 的 deadline 使用 steady_clock；
 * 监控数据时间戳表示实际发生时间，因此使用 system_clock。
 */
using MonitorTimestamp = std::chrono::system_clock::time_point;

/**
 * 调用方提交给 MonitorStore 的完整数据。
 */
struct MonitorData {
    MonitorKey _key;
    std::string _description;
    MonitorValue _value;
};

/**
 * MonitorStore 内部保存的记录。
 *
 * _changed_at 只在首次插入或值实际变化时更新。
 */
struct StoredRecord final {
    MonitorData _data;
    MonitorTimestamp _changed_at;
};

/**
 * 一次写入操作的结果。
 */
enum class UpdateStatus : std::uint8_t {
    INSERTED,
    UPDATED,
    UNCHANGED,
    IGNORED_INITIAL_ZERO,
    INVALID
};

/**
 * MonitorStore::update() 的返回值。
 *
 * _record 使用值语义，不能暴露 Store 内部对象的裸指针。
 */
struct UpdateResult final {
    UpdateStatus _status{UpdateStatus::INVALID};
    std::optional<StoredRecord> _record;

    constexpr bool changed() const noexcept
    {
        return _status == UpdateStatus::INSERTED || _status == UpdateStatus::UPDATED;
    }
};


}
}
#endif // __MONITOR_DATA_H__
