#include "monitor_policy.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>

using namespace TLSSMON;

namespace {

void expect(const PolicyFeedResult &result, bool noteworthy,
            std::uint32_t value) {
  assert(result._noteworthy == noteworthy);
  assert(result._value == value);
}

std::unique_ptr<MonitorPolicy> make_policy(std::uint32_t threshold,
                                           std::size_t count,
                                           PolicyTime period) {
  auto policy = make_times_per_period_policy(threshold, count, period);
  assert(policy);
  return policy;
}

void test_factory_validation_and_initial_state() {
  auto policy = make_policy(10U, 3U, PolicyTime{1000});
  assert(policy->value() == 0U);
  assert(!make_times_per_period_policy(10U, 0U, PolicyTime{1000}));
  assert(!make_times_per_period_policy(10U, 3U, PolicyTime{-1}));
}

void test_only_strictly_greater_values_count() {
  auto policy = make_policy(10U, 2U, PolicyTime{1000});
  expect(policy->feed(10U, PolicyTime{0}), false, 0U);
  expect(policy->feed(11U, PolicyTime{100}), false, 0U);
  expect(policy->feed(10U, PolicyTime{150}), false, 0U);
  expect(policy->feed(11U, PolicyTime{200}), true, 1U);
}

void test_nth_event_fires_and_boundary_is_inclusive() {
  auto policy = make_policy(10U, 3U, PolicyTime{1000});
  expect(policy->feed(20U, PolicyTime{0}), false, 0U);
  expect(policy->feed(20U, PolicyTime{500}), false, 0U);
  expect(policy->feed(20U, PolicyTime{1000}), true, 1U);
}

void test_spread_out_events_do_not_fire() {
  auto policy = make_policy(10U, 3U, PolicyTime{1000});
  expect(policy->feed(20U, PolicyTime{0}), false, 0U);
  expect(policy->feed(20U, PolicyTime{600}), false, 0U);
  expect(policy->feed(20U, PolicyTime{1300}), false, 0U);
  assert(policy->value() == 0U);
}

void test_saturated_stream_fires_once() {
  auto policy = make_policy(10U, 3U, PolicyTime{1000});
  expect(policy->feed(20U, PolicyTime{0}), false, 0U);
  expect(policy->feed(20U, PolicyTime{100}), false, 0U);
  expect(policy->feed(20U, PolicyTime{200}), true, 1U);
  expect(policy->feed(20U, PolicyTime{300}), false, 1U);
  expect(policy->feed(20U, PolicyTime{400}), false, 1U);
}

void test_rate_drop_rearms_and_new_window_fires() {
  auto policy = make_policy(10U, 3U, PolicyTime{1000});
  expect(policy->feed(20U, PolicyTime{0}), false, 0U);
  expect(policy->feed(20U, PolicyTime{100}), false, 0U);
  expect(policy->feed(20U, PolicyTime{200}), true, 1U);

  expect(policy->feed(20U, PolicyTime{2000}), false, 0U);
  expect(policy->feed(20U, PolicyTime{2100}), false, 0U);
  expect(policy->feed(20U, PolicyTime{2200}), true, 1U);
}

void test_non_events_do_not_age_or_reset_alerting() {
  auto policy = make_policy(10U, 3U, PolicyTime{1000});
  expect(policy->feed(20U, PolicyTime{0}), false, 0U);
  expect(policy->feed(20U, PolicyTime{100}), false, 0U);
  expect(policy->feed(20U, PolicyTime{200}), true, 1U);
  expect(policy->feed(10U, PolicyTime{5000}), false, 1U);
  expect(policy->feed(0U, PolicyTime{6000}), false, 1U);
  expect(policy->feed(20U, PolicyTime{7000}), false, 0U);
}

void test_single_event_zero_period_and_maximum_threshold() {
  auto single = make_policy(10U, 1U, PolicyTime{1000});
  expect(single->feed(11U, PolicyTime{100}), true, 1U);
  expect(single->feed(11U, PolicyTime{200}), false, 1U);

  auto zero_period = make_policy(10U, 2U, PolicyTime{0});
  expect(zero_period->feed(20U, PolicyTime{100}), false, 0U);
  expect(zero_period->feed(20U, PolicyTime{100}), true, 1U);

  const auto maximum = std::numeric_limits<std::uint32_t>::max();
  auto unreachable = make_policy(maximum, 1U, PolicyTime{1000});
  expect(unreachable->feed(maximum, PolicyTime{0}), false, 0U);
}

} // namespace

int main() {
  test_factory_validation_and_initial_state();
  test_only_strictly_greater_values_count();
  test_nth_event_fires_and_boundary_is_inclusive();
  test_spread_out_events_do_not_fire();
  test_saturated_stream_fires_once();
  test_rate_drop_rearms_and_new_window_fires();
  test_non_events_do_not_age_or_reset_alerting();
  test_single_event_zero_period_and_maximum_threshold();
  std::cout << "M9_POLICY_TIMES_PER_PERIOD=PASS\n";
  return 0;
}
