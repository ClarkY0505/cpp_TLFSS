#include "../timer_types.h"

  #include <cassert>
  #include <cstdint>

  using TLSSMON::TimerFlags;
  using TLSSMON::TimerHandle;
  using TLSSMON::has_flag;
  using TLSSMON::is_once;
  using TLSSMON::is_valid_timer_flags;

  int main()
  {
      // 保持与原 M3 标志值一致。
      static_assert(
          static_cast<std::uint8_t>(TimerFlags::ONCE) == 0x00);

      static_assert(
          static_cast<std::uint8_t>(TimerFlags::RECURRING) == 0x01);

      static_assert(
          static_cast<std::uint8_t>(TimerFlags::WORKER) == 0x02);

      // 标志组合。
      constexpr TimerFlags recurring_worker =
          TimerFlags::RECURRING | TimerFlags::WORKER;

      static_assert(
          has_flag(recurring_worker, TimerFlags::RECURRING));

      static_assert(
          has_flag(recurring_worker, TimerFlags::WORKER));

      static_assert(
          !is_once(recurring_worker));

      static_assert(
          is_once(TimerFlags::ONCE));

      static_assert(
          is_once(TimerFlags::WORKER));

      static_assert(
          is_valid_timer_flags(TimerFlags::ONCE));

      static_assert(
          is_valid_timer_flags(TimerFlags::RECURRING));

      static_assert(
          is_valid_timer_flags(TimerFlags::WORKER));

      static_assert(
          is_valid_timer_flags(recurring_worker));

      constexpr auto unknown =
          static_cast<TimerFlags>(1U << 7);

      static_assert(
          !is_valid_timer_flags(unknown));

      const TimerHandle invalid;
      const TimerHandle valid{42};

      assert(!invalid);
      assert(valid);

      return 0;
  }

