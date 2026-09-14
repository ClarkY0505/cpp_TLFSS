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

std::unique_ptr<MonitorPolicy> make_policy(std::size_t window,
                                           std::uint32_t trim_threshold) {
  auto policy = make_smooth_trim_policy(window, trim_threshold);
  assert(policy);
  return policy;
}

void test_factory_validation_and_initial_state() {
  auto policy = make_policy(4U, 50U);
  assert(policy->value() == 0U);
  assert(!make_smooth_trim_policy(0U, 50U));
}

void test_first_sample_and_first_zero_are_accepted() {
  auto nonzero = make_policy(4U, 0U);
  expect(nonzero->feed(100U, PolicyTime{0}), true, 100U);

  auto zero = make_policy(2U, 10U);
  expect(zero->feed(0U, PolicyTime{0}), false, 0U);
  expect(zero->feed(10U, PolicyTime{1}), true, 5U);
}

void test_tracks_integer_average() {
  auto policy = make_policy(4U, 100U);
  expect(policy->feed(100U, PolicyTime{0}), true, 100U);
  expect(policy->feed(102U, PolicyTime{1}), true, 101U);
  expect(policy->feed(104U, PolicyTime{2}), true, 102U);
  expect(policy->feed(106U, PolicyTime{3}), true, 103U);
}

void test_accepted_sample_may_leave_average_unchanged() {
  auto policy = make_policy(3U, 100U);
  expect(policy->feed(100U, PolicyTime{0}), true, 100U);
  expect(policy->feed(101U, PolicyTime{1}), false, 100U);
  expect(policy->feed(102U, PolicyTime{2}), true, 101U);
}

void test_outlier_does_not_modify_window() {
  auto policy = make_policy(3U, 50U);
  expect(policy->feed(100U, PolicyTime{0}), true, 100U);
  expect(policy->feed(104U, PolicyTime{1}), true, 102U);
  expect(policy->feed(9000U, PolicyTime{2}), false, 102U);
  expect(policy->feed(108U, PolicyTime{3}), true, 104U);
}

void test_trim_boundary_is_accepted() {
  auto policy = make_policy(2U, 50U);
  expect(policy->feed(100U, PolicyTime{0}), true, 100U);
  expect(policy->feed(150U, PolicyTime{1}), true, 125U);
}

void test_full_window_evicts_oldest() {
  auto policy = make_policy(2U, 1000U);
  expect(policy->feed(10U, PolicyTime{0}), true, 10U);
  expect(policy->feed(20U, PolicyTime{1}), true, 15U);
  expect(policy->feed(30U, PolicyTime{2}), true, 25U);
}

void test_single_window_and_zero_trim() {
  auto single = make_policy(1U, 100U);
  expect(single->feed(10U, PolicyTime{0}), true, 10U);
  expect(single->feed(20U, PolicyTime{1}), true, 20U);

  auto zero_trim = make_policy(3U, 0U);
  expect(zero_trim->feed(100U, PolicyTime{0}), true, 100U);
  expect(zero_trim->feed(101U, PolicyTime{1}), false, 100U);
  expect(zero_trim->feed(100U, PolicyTime{2}), false, 100U);
}

void test_uint32_extremes_and_ignored_time_are_safe() {
  const auto maximum = std::numeric_limits<std::uint32_t>::max();
  auto extremes = make_policy(2U, maximum);
  expect(extremes->feed(maximum, PolicyTime{0}), true, maximum);
  expect(extremes->feed(0U, PolicyTime{1}), true, maximum / 2U);

  auto time_independent = make_policy(3U, 100U);
  expect(time_independent->feed(100U, PolicyTime{5000}), true, 100U);
  expect(time_independent->feed(102U, PolicyTime{-1000}), true, 101U);
}

} // namespace

int main() {
  test_factory_validation_and_initial_state();
  test_first_sample_and_first_zero_are_accepted();
  test_tracks_integer_average();
  test_accepted_sample_may_leave_average_unchanged();
  test_outlier_does_not_modify_window();
  test_trim_boundary_is_accepted();
  test_full_window_evicts_oldest();
  test_single_window_and_zero_trim();
  test_uint32_extremes_and_ignored_time_are_safe();
  std::cout << "M9_POLICY_SMOOTH_TRIM=PASS\n";
  return 0;
}
