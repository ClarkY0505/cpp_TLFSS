#ifndef __TIMER_TYPES_H__
#define __TIMER_TYPES_H__

#include <cstdint>
#include <memory>
#include <type_traits>
namespace TLSSMON{
/** @brief 定时器行为标志；WORKER 仅适用于 RECURRING 定时器。 */
enum class TimerFlags : std::uint8_t {
    ONCE        = 0,
    RECURRING   = 1U << 0,
    WORKER      = 1U << 1
};

/** @brief 定时器注册句柄；ID 为 0 表示无效。 */
struct TimerHandle{
    std::uint64_t _id{0};
    /**
     * @brief 判断定时器句柄是否有效。
     * @return ID 非零时返回 true，否则返回 false。
     */
    explicit operator bool() const noexcept{
        return _id != 0;
    }
};

/**
 * @brief 组合定时器标志。
 * @param lhs 左操作数。
 * @param rhs 右操作数。
 * @return 组合后的标志值。
 */
constexpr TimerFlags operator|(TimerFlags lhs, TimerFlags rhs) noexcept{
    using ValueType = std::underlying_type_t<TimerFlags>;
    return static_cast<TimerFlags>(static_cast<ValueType>(lhs) | static_cast<ValueType>(rhs));
}

/**
 * @brief 判断 value 是否包含指定的非零标志。
 * @param value 待检查的标志组合。
 * @param flag 要检查的非零标志。
 * @return value 包含指定的非零标志时返回 true。
 */
constexpr bool has_flag(TimerFlags value, TimerFlags flag) noexcept{
    using ValueType = std::underlying_type_t<TimerFlags>;
    const ValueType value_bits = static_cast<ValueType>(value);
    const ValueType flag_bits = static_cast<ValueType>(flag);

    return flag_bits != 0 && (value_bits & flag_bits) == flag_bits;
}

/**
 * @brief 判断是否为单次定时器。
 * @param flags 定时器行为标志。
 * @return 未设置 RECURRING 标志时返回 true。
 */
constexpr bool is_once(TimerFlags flags) noexcept{
    return !has_flag(flags, TimerFlags::RECURRING);
}

/**
 * @brief 判断是否只包含支持的标志位。
 * @param flags 定时器行为标志。
 * @return 仅包含支持的标志位时返回 true。
 */
constexpr bool is_valid_timer_flags(TimerFlags flags) noexcept{
    using ValueType = std::underlying_type_t<TimerFlags>;
    constexpr ValueType known_bits  = 
        static_cast<ValueType>(TimerFlags::RECURRING) 
        | static_cast<ValueType>(TimerFlags::WORKER);

    const ValueType supplied_bits = static_cast<ValueType>(flags);

    const ValueType unknown_bits = 
        static_cast<ValueType>(supplied_bits
                               & static_cast<ValueType>(~known_bits));
    return unknown_bits == 0;
}
}


#endif // __TIMER_TYPES_H__
