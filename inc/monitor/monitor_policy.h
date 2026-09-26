#ifndef __MONITOR_POLICY_H__
#define __MONITOR_POLICY_H__

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace TLSSMON {
/**
 * @brief 调用方注入的策略逻辑时间，单位为毫秒。
 * @note 通常来自 steady_clock，不是 StoredRecord::_changed_at 使用的墙上时间。
 */
using PolicyTime = std::chrono::milliseconds;

/**
 * @brief 一次采样是否产生新输出，以及策略当前输出值。
 *
 * _noteworthy：
 *   true 表示本次产生了值得继续上报的新输出。
 *
 * _value：
 *   策略处理后的当前输出值。
 *
 * 对 CONT_FOR 和 TIMES_PER_PERIOD：
 *   _value 为 0 或 1。
 *
 * 对 SMOOTH_TRIM：
 *   _value 为当前滑动平均值。
 */
struct PolicyFeedResult final {
  bool _noteworthy{false};
  std::uint32_t _value{0U};
};
/**
 * @brief 监控采样策略的状态接口；每个实例持有独立可变状态。
 *
 * 使用约束：
 *
 * 1. 一个策略对象应由一个采样回调独占；
 * 2. 多线程共享时由调用方负责同步；
 * 3. 同一个时间型策略收到的 now 不应倒退；
 * 4. 策略不读取 system_clock 或 steady_clock；
 * 5. 策略不直接调用 Engine，也不拥有 Engine。
 */
class MonitorPolicy {
public:
  virtual ~MonitorPolicy();

  MonitorPolicy(const MonitorPolicy &) = delete;
  MonitorPolicy &operator=(const MonitorPolicy &) = delete;

  MonitorPolicy(MonitorPolicy &&) = delete;
  MonitorPolicy &operator=(MonitorPolicy &&) = delete;

  /**
   * @brief 输入一个原始采样值。
   *
   * @param value 本次采样值。
   * @param now 调用方注入的逻辑时间；SMOOTH_TRIM 忽略该参数。
   * @return 是否产生值得处理的新输出，以及策略当前输出值。
   */
  virtual PolicyFeedResult feed(std::uint32_t value,
                                PolicyTime now) noexcept = 0;

  /**
   * @brief 查询策略当前输出。
   *
   * CONT_FOR：
   *   ACTIVE 时为 1，其他状态为 0。
   *
   * TIMES_PER_PERIOD：
   *   告警条件成立时为 1，否则为 0。
   *
   * SMOOTH_TRIM：
   *   当前滑动平均值。
   * @return 当前输出值。
   */
  virtual std::uint32_t value() const noexcept = 0;

protected:
  MonitorPolicy() = default;
};
/**
 * @brief 创建 CONT_FOR 策略。
 *
 * value 必须严格大于 threshold，并连续保持 duration，
 * 才会产生一次 noteworthy 输出。
 *
 * duration < PolicyTime::zero() 时返回 nullptr。
 *
 * duration == PolicyTime::zero() 时仍按状态机执行：
 * 第一次越过阈值进入 PENDING，第二次越阈值输入进入 ACTIVE。
 * @param threshold 严格比较的数值阈值。
 * @param duration 连续满足条件所需的逻辑时间。
 * @return 策略对象；duration 为负时返回 nullptr。
 */
[[nodiscard]] std::unique_ptr<MonitorPolicy>
make_cont_for_policy(std::uint32_t threshold, PolicyTime duration);

/**
 * @brief 创建 TIMES_PER_PERIOD 策略。
 *
 * 在滚动时间窗口 period 内累计 count 次严格超阈值事件时，
 * 产生一次 noteworthy 输出。
 *
 * count == 0 时返回 nullptr。
 * period < PolicyTime::zero() 时返回 nullptr。
 *
 * period == PolicyTime::zero() 时，只有时间戳完全相同的
 * count 个有效事件才能满足条件。
 *
 * value <= threshold 的输入不计入事件窗口，也不会主动
 * 清除当前 alerting 状态。
 * @param threshold 严格比较的数值阈值。
 * @param count 时间窗口内所需的事件次数。
 * @param period 滚动窗口长度。
 * @return 策略对象；count 为零或 period 为负时返回 nullptr。
 */
[[nodiscard]] std::unique_ptr<MonitorPolicy>
make_times_per_period_policy(std::uint32_t threshold, std::size_t count,
                             PolicyTime period);

/**
 * @brief 创建 SMOOTH_TRIM 策略。
 *
 * 维护最近 window 个被接受样本的整数平均值。
 *
 * 第一个样本无条件接受。
 *
 * 后续样本与当前平均值的偏差严格大于 trim_threshold
 * 时被拒绝；偏差等于 trim_threshold 时仍然接受。
 *
 * 被拒绝样本不能修改窗口、累计和、平均值或环形位置。
 *
 * window == 0 时返回 nullptr。
 *
 * feed() 忽略 now 参数。
 * @param window 滑动窗口中的样本数。
 * @param trim_threshold 允许的最大偏差。
 * @return 策略对象；window 为零时返回 nullptr。
 */
[[nodiscard]] std::unique_ptr<MonitorPolicy>
make_smooth_trim_policy(std::size_t window, std::uint32_t trim_threshold);

} // namespace TLSSMON
#endif // __MONITOR_POLICY_H__
