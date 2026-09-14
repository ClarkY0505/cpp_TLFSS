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
                                           PolicyTime duration) {
  auto policy = make_cont_for_policy(threshold, duration);
  assert(policy);
  return policy;
}

void test_factory_validation_and_initial_state() {
  auto policy = make_policy(80U, PolicyTime{3000});
  assert(policy->value() == 0U);
  assert(!make_cont_for_policy(80U, PolicyTime{-1}));
}

void test_threshold_is_strict_and_exact_duration_fires() {
  auto policy = make_policy(80U, PolicyTime{1000});
  expect(policy->feed(80U, PolicyTime{0}), false, 0U);
  expect(policy->feed(81U, PolicyTime{100}), false, 0U);
  expect(policy->feed(81U, PolicyTime{1099}), false, 0U);
  expect(policy->feed(81U, PolicyTime{1100}), true, 1U);
}

void test_active_fires_once_and_drop_rearms() {
  auto policy = make_policy(10U, PolicyTime{100});
  expect(policy->feed(20U, PolicyTime{0}), false, 0U);
  expect(policy->feed(20U, PolicyTime{100}), true, 1U);
  expect(policy->feed(30U, PolicyTime{200}), false, 1U);

  expect(policy->feed(10U, PolicyTime{250}), false, 0U);
  expect(policy->feed(20U, PolicyTime{300}), false, 0U);
  expect(policy->feed(20U, PolicyTime{400}), true, 1U);
}

void test_pending_drop_restarts_elapsed_time() {
  auto policy = make_policy(10U, PolicyTime{1000});
  expect(policy->feed(20U, PolicyTime{0}), false, 0U);
  expect(policy->feed(5U, PolicyTime{500}), false, 0U);
  expect(policy->feed(20U, PolicyTime{700}), false, 0U);
  expect(policy->feed(20U, PolicyTime{1699}), false, 0U);
  expect(policy->feed(20U, PolicyTime{1700}), true, 1U);
}

void test_zero_duration_and_maximum_threshold() {
  auto zero = make_policy(10U, PolicyTime{0});
  expect(zero->feed(20U, PolicyTime{100}), false, 0U);
  expect(zero->feed(20U, PolicyTime{100}), true, 1U);

  const auto maximum = std::numeric_limits<std::uint32_t>::max();
  auto unreachable = make_policy(maximum, PolicyTime{1});
  expect(unreachable->feed(maximum, PolicyTime{0}), false, 0U);
  expect(unreachable->feed(maximum, PolicyTime{100}), false, 0U);
}

void test_time_regression_cannot_trigger() {
  auto policy = make_policy(10U, PolicyTime{100});
  expect(policy->feed(20U, PolicyTime{1000}), false, 0U);
  expect(policy->feed(20U, PolicyTime{900}), false, 0U);
  expect(policy->feed(20U, PolicyTime{1100}), true, 1U);
}

} // namespace

int main() {
  test_factory_validation_and_initial_state();
  test_threshold_is_strict_and_exact_duration_fires();
  test_active_fires_once_and_drop_rearms();
  test_pending_drop_restarts_elapsed_time();
  test_zero_duration_and_maximum_threshold();
  test_time_regression_cannot_trigger();
  std::cout << "M9_POLICY_CONT_FOR=PASS\n";
  return 0;
}
