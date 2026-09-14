#include "engine.h"
#include "monitor_policy.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

using namespace TLSSMON;

namespace {

constexpr std::uint32_t MODULE_ID = 0x900U;
constexpr std::uint32_t FUNCTION_ID = 7U;
constexpr std::uint32_t TEMPERATURE_EID = 1U;
constexpr std::uint32_t SMOOTH_VALUE_EID = 2U;
constexpr std::uint32_t ERROR_BURST_EID = 3U;

std::uint32_t level_value(MonitorLevel level) {
  return static_cast<std::uint32_t>(level);
}

const MonData::NumericValue &
numeric_value(const MonData::StoredRecord &record) {
  assert(std::holds_alternative<MonData::NumericValue>(
      record._data._value));
  return std::get<MonData::NumericValue>(record._data._value);
}

void register_policy_module(Engine &engine) {
  const ModuleRegisterStatus status = engine.register_module(MonitorModuleInfo{
      MODULE_ID,
      "policy-module",
      "M9 policy integration module",
      {{MonitorLevel::INFO, "unused"},
       {MonitorLevel::WARN, "sustained overheating"},
       {MonitorLevel::INFO, "smoothed sensor value"},
       {MonitorLevel::WARN, "error burst"}}});
  assert(status == ModuleRegisterStatus::SUCCESS);
}

/*
 * 同步 Timer 注入逻辑时间。短暂尖峰被 CONT_FOR 丢弃，只有持续过热
 * 进入 Reporter；M8 随后补全 level 和 description。
 */
void test_timer_cont_for_reports_only_sustained_error() {
  Engine engine{MonConfig{"m9-cont-for-timer", 0U, 1U}};
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);
  register_policy_module(engine);

  std::vector<MonData::StoredRecord> published;
  assert(engine.set_publisher([&](MonData::StoredRecord record) {
    published.push_back(std::move(record));
  }));

  struct Sample final {
    std::uint32_t _value;
    PolicyTime _time;
  };
  const std::array<Sample, 6> samples{{
      {70U, PolicyTime{0}},
      {95U, PolicyTime{1000}},
      {60U, PolicyTime{1500}},
      {90U, PolicyTime{2000}},
      {91U, PolicyTime{3000}},
      {92U, PolicyTime{5000}},
  }};

  std::shared_ptr<MonitorPolicy> policy{
      make_cont_for_policy(80U, PolicyTime{3000})};
  assert(policy);

  std::size_t index = 0U;
  std::optional<MonData::UpdateStatus> report_status;
  const auto timer = engine.set_timer(
      MonCallback{
          "m9-cont-for-sample",
          [&, policy]() -> int {
            assert(index < samples.size());
            const Sample sample = samples[index];
            const PolicyFeedResult decision =
                policy->feed(sample._value, sample._time);

            if (decision._noteworthy) {
              const MonData::UpdateResult result = engine.report_error(
                  {MODULE_ID, 99U, FUNCTION_ID, TEMPERATURE_EID},
                  sample._value);
              assert(result.changed());
              report_status = result._status;
            }

            ++index;
            if (index == samples.size()) {
              engine.stop();
            }
            return 0;
          },
          false},
      TimerFlags::RECURRING, std::chrono::milliseconds{1});
  assert(timer.has_value());
  assert(engine.run() == ENGINESTATE::SUCCESSFUL);

  assert(index == samples.size());
  assert(report_status == MonData::UpdateStatus::INSERTED);
  assert(published.size() == 1U);

  const MonData::StoredRecord &record = published.front();
  assert(record._data._key._level == level_value(MonitorLevel::WARN));
  assert(record._data._description == "sustained overheating");
  assert(numeric_value(record)._value == 92U);
  assert(numeric_value(record)._state == 2U);

  const auto stored = engine.find_data(
      {MODULE_ID, level_value(MonitorLevel::WARN), FUNCTION_ID,
       TEMPERATURE_EID});
  assert(stored.has_value());
  assert(numeric_value(*stored)._value == 92U);
}

/*
 * SMOOTH_TRIM 上报策略输出而非原始值。异常值 9000 不进入 Engine。
 */
void test_smooth_trim_reports_smoothed_value() {
  Engine engine{MonConfig{"m9-smooth-integration", 0U, 1U}};
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);
  register_policy_module(engine);

  std::vector<MonData::StoredRecord> published;
  assert(engine.set_publisher([&](MonData::StoredRecord record) {
    published.push_back(std::move(record));
  }));

  auto policy = make_smooth_trim_policy(3U, 50U);
  assert(policy);
  const MonData::MonitorKey key{MODULE_ID, 99U, FUNCTION_ID,
                                SMOOTH_VALUE_EID};

  const auto feed_and_report = [&](std::uint32_t raw, PolicyTime now) {
    const PolicyFeedResult decision = policy->feed(raw, now);
    if (decision._noteworthy) {
      assert(engine.report_count(key, decision._value).changed());
    }
  };

  feed_and_report(100U, PolicyTime{0});
  feed_and_report(104U, PolicyTime{1});
  feed_and_report(9000U, PolicyTime{2});
  feed_and_report(108U, PolicyTime{3});

  assert(published.size() == 3U);
  assert(numeric_value(published[0])._value == 100U);
  assert(numeric_value(published[1])._value == 102U);
  assert(numeric_value(published[2])._value == 104U);
  assert(published[2]._data._key._level == level_value(MonitorLevel::INFO));
  assert(published[2]._data._description == "smoothed sensor value");
}

/*
 * 策略上升沿与 M4 去重职责不同。第二轮频率告警仍调用 report_error，
 * 但最终数值仍为 20，所以 Store 返回 UNCHANGED，Publisher 不重复发送。
 */
void test_policy_output_still_passes_through_store_dedup() {
  Engine engine{MonConfig{"m9-policy-store-dedup", 0U, 1U}};
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);
  register_policy_module(engine);

  std::vector<MonData::StoredRecord> published;
  assert(engine.set_publisher([&](MonData::StoredRecord record) {
    published.push_back(std::move(record));
  }));

  auto policy =
      make_times_per_period_policy(10U, 3U, PolicyTime{1000});
  assert(policy);
  std::vector<MonData::UpdateStatus> statuses;

  const auto feed_event = [&](PolicyTime now) {
    const PolicyFeedResult decision = policy->feed(20U, now);
    if (decision._noteworthy) {
      statuses.push_back(
          engine
              .report_error(
                  {MODULE_ID, 99U, FUNCTION_ID, ERROR_BURST_EID}, 20U)
              ._status);
    }
  };

  feed_event(PolicyTime{0});
  feed_event(PolicyTime{100});
  feed_event(PolicyTime{200});
  feed_event(PolicyTime{2000});
  feed_event(PolicyTime{2100});
  feed_event(PolicyTime{2200});

  assert(statuses.size() == 2U);
  assert(statuses[0] == MonData::UpdateStatus::INSERTED);
  assert(statuses[1] == MonData::UpdateStatus::UNCHANGED);
  assert(published.size() == 1U);
}

/* AIO 回调可以按值捕获 shared_ptr 策略并安全完成一次频率上报。 */
void test_aio_callback_can_own_and_feed_policy() {
  Engine engine{MonConfig{"m9-policy-aio", 0U, 1U}};
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);
  register_policy_module(engine);

  std::vector<MonData::StoredRecord> published;
  assert(engine.set_publisher([&](MonData::StoredRecord record) {
    published.push_back(std::move(record));
  }));

  std::shared_ptr<MonitorPolicy> policy{
      make_times_per_period_policy(0U, 1U, PolicyTime{1000})};
  assert(policy);

  int pipe_fds[2]{-1, -1};
  assert(::pipe(pipe_fds) == 0);
  bool callback_executed = false;

  const auto aio = engine.add_aio(
      pipe_fds[0],
      MonCallback{
          "m9-policy-aio-event",
          [&, policy]() -> int {
            char signal = '\0';
            assert(::read(pipe_fds[0], &signal, sizeof(signal)) ==
                   static_cast<ssize_t>(sizeof(signal)));

            const PolicyFeedResult decision =
                policy->feed(1U, PolicyTime{100});
            assert(decision._noteworthy);
            assert(engine
                       .report_error(
                           {MODULE_ID, 99U, FUNCTION_ID, ERROR_BURST_EID},
                           1U)
                       ._status == MonData::UpdateStatus::INSERTED);

            callback_executed = true;
            engine.stop();
            return 0;
          },
          false});
  assert(aio.has_value());

  const char signal = 'x';
  assert(::write(pipe_fds[1], &signal, sizeof(signal)) ==
         static_cast<ssize_t>(sizeof(signal)));
  assert(engine.run() == ENGINESTATE::SUCCESSFUL);

  assert(callback_executed);
  assert(published.size() == 1U);
  assert(numeric_value(published.front())._state == 2U);
  assert(::close(pipe_fds[0]) == 0);
  assert(::close(pipe_fds[1]) == 0);
}

} // namespace

int main() {
  test_timer_cont_for_reports_only_sustained_error();
  test_smooth_trim_reports_smoothed_value();
  test_policy_output_still_passes_through_store_dedup();
  test_aio_callback_can_own_and_feed_policy();
  std::cout << "M9_POLICY_ENGINE_PIPELINE=PASS\n";
  return 0;
}
