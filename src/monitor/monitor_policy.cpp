#include "monitor_policy.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace TLSSMON {

/*
 * 确保 MonitorPolicy 具备稳定的虚函数表定义位置；
 * 允许通过 std::unique_ptr<MonitorPolicy> 安全销毁派生策略；
 */
MonitorPolicy::~MonitorPolicy() = default;
namespace {
/*
 * CONT_FOR 内部状态。
 * IDLE：
 *   当前没有连续超阈值区间。
 * PENDING：
 *   已经超过阈值，正在等待持续时间满足。
 * ACTIVE：
 *   持续时间已经满足，并且上升沿已经产生。
 */
enum class ContForState : std::uint8_t { IDLE, PENDING, ACTIVE };

class ContForPolicy final : public MonitorPolicy {
public:
  ContForPolicy(std::uint32_t threshold, PolicyTime duration) noexcept
      : _threshold(threshold), _duration(duration) {}
  PolicyFeedResult feed(std::uint32_t value, PolicyTime now) noexcept override {
    /*
     * CONT_FOR 使用严格超阈值条件。
     *
     * 等于或低于阈值都会：
     *
     * PENDING -> IDLE
     * ACTIVE  -> IDLE
     * IDLE    -> IDLE
     */
    if (value <= _threshold) {
      _state = ContForState::IDLE;
      _crossed_at = PolicyTime::zero();

      return PolicyFeedResult{false, 0U};
    }

    switch (_state) {
    case ContForState::IDLE:
      /*
       * 第一次超过阈值：
       *
       * 只记录连续区间的起始时间，
       * 不在本次调用立即触发。
       */
      _state = ContForState::PENDING;
      _crossed_at = now;

      return PolicyFeedResult{false, 0U};

    case ContForState::PENDING:
      /*
       * 这里额外避免负 elapsed 被误认为已经经过很长时间：
       * 时间倒退时保持 PENDING，不触发，也不改变起点。
       */
      if (now < _crossed_at) {
        return PolicyFeedResult{false, 0U};
      }

      if (now - _crossed_at >= _duration) {
        /*
         * 持续时间达到要求：
         *
         * 本次调用产生一次上升沿，
         * 并进入 ACTIVE。
         */
        _state = ContForState::ACTIVE;

        return PolicyFeedResult{true, 1U};
      }

      return PolicyFeedResult{false, 0U};

    case ContForState::ACTIVE:
      /*
       * ACTIVE 表示当前告警仍成立，但上升沿已经报告过。
       * 因此：
       *
       * noteworthy = false
       * value      = 1
       */
      return PolicyFeedResult{false, 1U};
    }

    /*
     * 枚举值理论上不可能落到这里。
     * 保留安全返回，避免未来增加枚举值后函数没有返回值。
     */
    return PolicyFeedResult{false, 0U};
  }

  std::uint32_t value() const noexcept override {
    return _state == ContForState::ACTIVE ? 1U : 0U;
  }

private:
  const std::uint32_t _threshold;
  const PolicyTime _duration;

  ContForState _state{ContForState::IDLE};
  PolicyTime _crossed_at{PolicyTime::zero()};
};

/*
 * TIMES_PER_PERIOD 策略。
 *
 * _events：
 *   保存最近 target_count 个有效事件的时间戳。
 *
 * _head：
 *   指向下一次写入位置。
 *
 *   当缓冲区已经填满后，它同时指向当前最旧事件。
 *
 * _event_count：
 *   当前已经保存的有效事件数量，最大为 _events.size()。
 *
 * _alerting：
 *   当前滚动窗口是否满足频率条件。
 */
class TimesPerPeriodPolicy final : public MonitorPolicy {
public:
  TimesPerPeriodPolicy(std::uint32_t threshold, std::size_t count,
                       PolicyTime period)
      : _threshold(threshold), _period(period),
        _events(count, PolicyTime::zero()) {}
  PolicyFeedResult feed(std::uint32_t value, PolicyTime now) noexcept override {
    // 未超阈值的采样不算事件，也不主动清除已有告警状态。
    if (value <= _threshold) {
      return PolicyFeedResult{false, current_value()};
    }

    _events[_head] = now;
    _head = (_head + 1U) % _events.size();

    if (_event_count < _events.size()) {
      ++_event_count;
    }
    bool condition_met = false;

    if (_event_count == _events.size()) {
      /*
       * 写入完成并移动 _head 后：
       *
       * _head 指向下一次要覆盖的位置，
       * 也就是当前窗口中的最旧事件。
       */
      const PolicyTime oldest = _events[_head];

      /*
       * 调用方应保证时间单调。
       *
       * 显式检查可以避免时间倒退产生负时间差后误判。
       */
      if (now >= oldest) {
        condition_met = now - oldest <= _period;
      }
    }

    /*
     * 只有 false -> true 的转换产生 noteworthy。
     */
    const bool fired = condition_met && !_alerting;

    _alerting = condition_met;

    return PolicyFeedResult{fired, current_value()};
  }
  std::uint32_t value() const noexcept override { return current_value(); }

private:
  std::uint32_t current_value() const noexcept { return _alerting ? 1U : 0U; }

  const std::uint32_t _threshold;
  const PolicyTime _period;

  std::vector<PolicyTime> _events;
  std::size_t _event_count{0U};
  std::size_t _head{0U};
  bool _alerting{false};
};

/*
 * SMOOTH_TRIM 策略。
 *
 * 只将被接受的样本写入滑动窗口。
 */
class SmoothTrimPolicy final : public MonitorPolicy {
public:
  SmoothTrimPolicy(std::size_t window, std::uint32_t trim_threshold)
      : _trim_threshold(trim_threshold), _samples(window, 0U) {}

  PolicyFeedResult feed(std::uint32_t value,
                        PolicyTime /* now */) noexcept override {
    /*
     * 第一个样本没有历史平均值，直接接受。
     *
     * 从第二个样本开始执行异常值检查。
     */
    if (_sample_count > 0U) {
      const std::uint32_t difference =
          value >= _average ? value - _average : _average - value;

      /*
       * 严格大于阈值才拒绝。
       *
       * 等于阈值时仍然接受。
       */
      if (difference > _trim_threshold) {
        return PolicyFeedResult{false, _average};
      }
    }

    if (_sample_count < _samples.size()) {
      /*
       * 窗口还没有填满：
       *
       * 直接在 head 位置写入新样本。
       */
      _samples[_head] = value;
      _sum += static_cast<std::uint64_t>(value);
      ++_sample_count;
    } else {
      /*
       * 窗口已经填满：
       *
       * head 指向当前最旧样本。
       * 先从累计和中移除旧样本，再写入新样本。
       */
      _sum -= static_cast<std::uint64_t>(_samples[_head]);

      _samples[_head] = value;

      _sum += static_cast<std::uint64_t>(value);
    }

    /*
     * 移动到下一次写入位置。
     *
     * 工厂函数已保证 samples 不为空，因此不会除零。
     */
    _head = (_head + 1U) % _samples.size();

    /*
     * sample_count 至少为 1，因此这里不会除零。
     *
     * 平均值按 M9 契约使用整数除法。
     */
    const std::uint32_t new_average = static_cast<std::uint32_t>(
        _sum / static_cast<std::uint64_t>(_sample_count));

    const bool changed = new_average != _average;

    _average = new_average;

    return PolicyFeedResult{changed, _average};
  }

  std::uint32_t value() const noexcept override { return _average; }

private:
  const std::uint32_t _trim_threshold;

  std::vector<std::uint32_t> _samples;
  std::size_t _sample_count{0U};
  std::size_t _head{0U};

  std::uint64_t _sum{0U};
  std::uint32_t _average{0U};
};

} // namespace

std::unique_ptr<MonitorPolicy> make_cont_for_policy(std::uint32_t threshold,
                                                    PolicyTime duration) {
  if (duration < PolicyTime::zero()) {
    return {};
  }

  return std::make_unique<ContForPolicy>(threshold, duration);
}

std::unique_ptr<MonitorPolicy>
make_times_per_period_policy(std::uint32_t threshold, std::size_t count,
                             PolicyTime period) {
  /*
   * count == 0 会产生空环形缓冲区，并导致后续取模除零。
   *
   */
  if (count == 0U || period < PolicyTime::zero()) {
    return {};
  }

  return std::make_unique<TimesPerPeriodPolicy>(threshold, count, period);
}

std::unique_ptr<MonitorPolicy>
make_smooth_trim_policy(std::size_t window, std::uint32_t trim_threshold) {
  /*
   * 空窗口无法保存样本，并会导致环形下标取模除零。
   */
  if (window == 0U) {
    return {};
  }

  return std::make_unique<SmoothTrimPolicy>(window, trim_threshold);
}

} // namespace TLSSMON
