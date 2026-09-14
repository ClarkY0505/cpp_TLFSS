#include "monitor_policy.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <type_traits>
#include <utility>

using namespace TLSSMON;

static_assert(std::is_same_v<PolicyTime, std::chrono::milliseconds>);
static_assert(std::is_aggregate_v<PolicyFeedResult>);
static_assert(std::is_copy_constructible_v<PolicyFeedResult>);
static_assert(std::is_move_constructible_v<PolicyFeedResult>);

static_assert(std::is_abstract_v<MonitorPolicy>);
static_assert(std::has_virtual_destructor_v<MonitorPolicy>);
static_assert(!std::is_copy_constructible_v<MonitorPolicy>);
static_assert(!std::is_copy_assignable_v<MonitorPolicy>);
static_assert(!std::is_move_constructible_v<MonitorPolicy>);
static_assert(!std::is_move_assignable_v<MonitorPolicy>);

using ExpectedPolicyFeed = PolicyFeedResult (MonitorPolicy::*)(
    std::uint32_t, PolicyTime) noexcept;
using ExpectedPolicyValue =
    std::uint32_t (MonitorPolicy::*)() const noexcept;

static_assert(
    std::is_same_v<decltype(&MonitorPolicy::feed), ExpectedPolicyFeed>);
static_assert(
    std::is_same_v<decltype(&MonitorPolicy::value), ExpectedPolicyValue>);

using ExpectedContForFactory =
    std::unique_ptr<MonitorPolicy> (*)(std::uint32_t, PolicyTime);
using ExpectedTimesFactory = std::unique_ptr<MonitorPolicy> (*)(
    std::uint32_t, std::size_t, PolicyTime);
using ExpectedSmoothFactory =
    std::unique_ptr<MonitorPolicy> (*)(std::size_t, std::uint32_t);

static_assert(std::is_same_v<decltype(&make_cont_for_policy),
                             ExpectedContForFactory>);
static_assert(std::is_same_v<decltype(&make_times_per_period_policy),
                             ExpectedTimesFactory>);
static_assert(std::is_same_v<decltype(&make_smooth_trim_policy),
                             ExpectedSmoothFactory>);

namespace {

/*
 * 测试专用策略只验证公共接口、按值返回和虚析构，不实现任何 M9 算法。
 */
class ProbePolicy final : public MonitorPolicy {
public:
  explicit ProbePolicy(bool &destroyed) noexcept : _destroyed(destroyed) {}

  ~ProbePolicy() override { _destroyed = true; }

  PolicyFeedResult feed(std::uint32_t value,
                        PolicyTime now) noexcept override {
    _value = value;
    _last_time = now;
    return {true, value};
  }

  std::uint32_t value() const noexcept override { return _value; }
  PolicyTime last_time() const noexcept { return _last_time; }

private:
  bool &_destroyed;
  std::uint32_t _value{0U};
  PolicyTime _last_time{0};
};

void test_feed_uses_injected_time_and_returns_a_copy() {
  bool destroyed = false;
  auto concrete = std::make_unique<ProbePolicy>(destroyed);
  ProbePolicy *const probe = concrete.get();
  std::unique_ptr<MonitorPolicy> policy = std::move(concrete);

  PolicyFeedResult result = policy->feed(42U, PolicyTime{1500});
  assert(result._noteworthy);
  assert(result._value == 42U);
  assert(probe->last_time() == PolicyTime{1500});

  result._value = 999U;
  assert(policy->value() == 42U);
  assert(!destroyed);
}

void test_virtual_destructor_releases_derived_policy() {
  bool destroyed = false;
  {
    std::unique_ptr<MonitorPolicy> policy =
        std::make_unique<ProbePolicy>(destroyed);
    assert(!destroyed);
  }
  assert(destroyed);
}

void test_default_result_means_no_output() {
  const PolicyFeedResult result{};
  assert(!result._noteworthy);
  assert(result._value == 0U);
}

} // namespace

int main() {
  test_feed_uses_injected_time_and_returns_a_copy();
  test_virtual_destructor_releases_derived_policy();
  test_default_result_means_no_output();
  std::cout << "M9_POLICY_CONTRACT=PASS\n";
  return 0;
}
