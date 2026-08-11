#ifndef __TIMER_TYPES_H__
#define __TIMER_TYPES_H__

#include <cstdint>
#include <memory>
#include <type_traits>
namespace TLSSMON{
enum class TimerFlags : std::uint8_t {
    ONCE        = 0,
    RECURRING   = 1U << 0,
    WORKER      = 1U << 1
};

struct TimerHandle{
    std::uint64_t _id{0};
    explicit operator bool() const noexcept{
        return _id != 0;
    }
};

constexpr TimerFlags operator|(TimerFlags lhs, TimerFlags rhs) noexcept{
    using ValueType = std::underlying_type_t<TimerFlags>;
    return static_cast<TimerFlags>(static_cast<ValueType>(lhs) | static_cast<ValueType>(rhs));
}

constexpr bool has_flag(TimerFlags value, TimerFlags flag) noexcept{
    using ValueType = std::underlying_type_t<TimerFlags>;
    const ValueType value_bits = static_cast<ValueType>(value);
    const ValueType flag_bits = static_cast<ValueType>(flag);

    return flag_bits != 0 && (value_bits & flag_bits) == flag_bits;
}

constexpr bool is_once(TimerFlags flags) noexcept{
    return !has_flag(flags, TimerFlags::RECURRING);
}

/**
 * 判断是否只包含支持的标志位。
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
