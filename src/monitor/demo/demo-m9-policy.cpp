#include "monitor_policy.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>

    using namespace TLSSMON;

namespace {

bool demonstrate_cont_for() {
  auto policy = make_cont_for_policy(80U, PolicyTime{3000});

  if (!policy) {
    return false;
  }

  struct Sample {
    PolicyTime _time;
    std::uint32_t _value;
  };

  const std::array<Sample, 6U> samples{{
      {PolicyTime{0}, 70U},
      {PolicyTime{1000}, 95U},
      {PolicyTime{1500}, 60U},
      {PolicyTime{2000}, 90U},
      {PolicyTime{3000}, 91U},
      {PolicyTime{5000}, 92U},
  }};

  std::size_t alarms = 0U;

  std::cout << "== CONT_FOR: temperature > 80 for 3000ms ==\n";

  for (const Sample &sample : samples) {
    const PolicyFeedResult result = policy->feed(sample._value, sample._time);

    std::cout << "t=" << sample._time.count() << "ms value=" << sample._value
              << " output=" << result._value;

    if (result._noteworthy) {
      ++alarms;
      std::cout << " ALARM";
    }

    std::cout << '\n';
  }

  return alarms == 1U && policy->value() == 1U;
}

bool demonstrate_times_per_period() {
  auto policy = make_times_per_period_policy(0U, 3U, PolicyTime{1000});

  if (!policy) {
    return false;
  }

  const std::array<PolicyTime, 5U> event_times{
      PolicyTime{0},    PolicyTime{200},  PolicyTime{5000},
      PolicyTime{5100}, PolicyTime{5200},
  };

  std::size_t alarms = 0U;

  std::cout << "\n== TIMES_PER_PERIOD: 3 errors in 1000ms ==\n";

  for (const PolicyTime time : event_times) {
    const PolicyFeedResult result = policy->feed(1U, time);

    std::cout << "error at " << time.count() << "ms output=" << result._value;

    if (result._noteworthy) {
      ++alarms;
      std::cout << " ALARM";
    }

    std::cout << '\n';
  }

  return alarms == 1U && policy->value() == 1U;
}

bool demonstrate_smooth_trim() {
  auto policy = make_smooth_trim_policy(4U, 50U);

  if (!policy) {
    return false;
  }

  const std::array<std::uint32_t, 6U> samples{300U,  310U, 320U,
                                              9999U, 330U, 340U};

  std::cout << "\n== SMOOTH_TRIM: window=4 trim=50 ==\n";

  for (const std::uint32_t sample : samples) {
    const PolicyFeedResult result = policy->feed(sample, PolicyTime{0});

    std::cout << "raw=" << sample << " average=" << result._value
              << " changed=" << (result._noteworthy ? "true" : "false") << '\n';
  }

  /*
   * 9999 被丢弃，最终窗口为 310、320、330、340，
   * 平均值为 325。
   */
  return policy->value() == 325U;
}

} // namespace

int main() {
  const bool cont_for_ok = demonstrate_cont_for();
  const bool rate_ok = demonstrate_times_per_period();
  const bool smooth_ok = demonstrate_smooth_trim();

  const bool passed = cont_for_ok && rate_ok && smooth_ok;

  std::cout << "\nM9_POLICY_DEMO=" << (passed ? "PASS" : "FAIL") << '\n';

  return passed ? 0 : 1;
}
