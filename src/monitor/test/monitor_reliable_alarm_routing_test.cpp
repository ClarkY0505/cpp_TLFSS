#include "alarm_publisher_types.h"
#include "engine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace TLSSMON;

namespace {

using namespace std::chrono_literals;

constexpr std::uint32_t MODULE_ID = 0x901U;

const MonData::NumericValue &
numeric_value(const MonData::StoredRecord &record) {
  assert(std::holds_alternative<MonData::NumericValue>(
      record._data._value));
  return std::get<MonData::NumericValue>(record._data._value);
}

AlarmEnqueueResult durable_result() {
  return {AlarmEnqueueStatus::SUCCESS, 0};
}

template <typename Predicate>
bool wait_until(Predicate predicate,
                std::chrono::milliseconds timeout = 2s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }

  return predicate();
}

static_assert(std::is_same_v<Engine::AlarmPublisher, AlarmPublisher>);
static_assert(std::is_invocable_r_v<AlarmEnqueueResult, AlarmPublisher,
                                    MonData::StoredRecord>);

/*
 * AlarmEnqueueResult 必须采用失败安全的默认值。只有 SUCCESS 表示候选记录
 * 已经可靠持久化，可以越过阶段 10 的提交门。
 */
void test_alarm_enqueue_result_contract() {
  assert(!AlarmEnqueueResult{}.durable());
  assert(durable_result().durable());

  for (const AlarmEnqueueStatus status :
       {AlarmEnqueueStatus::NOT_READY, AlarmEnqueueStatus::ENCODE_FAILED,
        AlarmEnqueueStatus::RANDOM_FAILED, AlarmEnqueueStatus::SPOOL_FULL,
        AlarmEnqueueStatus::IO_ERROR}) {
    assert((!AlarmEnqueueResult{status, 5}.durable()));
  }
}

/*
 * 没有安装可靠 Publisher 时必须保持 M5/M6 兼容行为：错误记录仍由普通
 * Publisher 发布。report_error() 使用 force=true，因此首次零值也必须保存。
 */
void test_error_falls_back_to_normal_publisher() {
  Engine engine{MonConfig{"m9-alarm-fallback", 0U, 1U}};
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);

  std::vector<MonData::StoredRecord> published;
  assert(engine.set_publisher([&](MonData::StoredRecord record) {
    published.push_back(std::move(record));
  }));

  const MonData::MonitorKey key{MODULE_ID, 1U, 10U, 1U};
  const MonData::UpdateResult result =
      engine.report_error(key, 0U, "initial zero error");

  assert(result._status == MonData::UpdateStatus::INSERTED);
  assert(result.changed());
  assert(published.size() == 1U);
  assert(published.front()._data._key == key);
  assert(numeric_value(published.front())._value == 0U);
  assert(numeric_value(published.front())._state == 2U);
  assert(engine.find_data(key).has_value());
}

/*
 * 安装可靠 Publisher 后，错误只能进入可靠通道。相同数值仍由 M4 去重，
 * 不重复写 outbox；数值发生变化后才再次调用可靠 Publisher。
 */
void test_reliable_route_deduplicates_and_bypasses_normal_publisher() {
  Engine engine{MonConfig{"m9-alarm-route", 0U, 1U}};
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);

  std::atomic<std::size_t> normal_calls{0U};
  std::vector<MonData::StoredRecord> alarms;

  assert(engine.set_publisher([&](MonData::StoredRecord) {
    normal_calls.fetch_add(1U, std::memory_order_relaxed);
  }));
  assert(engine.set_alarm_publisher([&](MonData::StoredRecord record) {
    alarms.push_back(std::move(record));
    return durable_result();
  }));

  const MonData::MonitorKey key{MODULE_ID, 1U, 10U, 2U};

  const auto inserted = engine.report_error(key, 7U, "first");
  const auto unchanged = engine.report_error(key, 7U, "metadata changed");
  const auto updated = engine.report_error(key, 8U, "second");

  assert(inserted._status == MonData::UpdateStatus::INSERTED);
  assert(unchanged._status == MonData::UpdateStatus::UNCHANGED);
  assert(updated._status == MonData::UpdateStatus::UPDATED);
  assert(alarms.size() == 2U);
  assert(numeric_value(alarms[0])._value == 7U);
  assert(numeric_value(alarms[1])._value == 8U);
  assert(normal_calls.load(std::memory_order_relaxed) == 0U);
}

/*
 * 所有非成功 enqueue 状态和异常都必须转换为 DURABILITY_FAILED。失败时
 * Store 不能产生记录；随后对同一个值重试仍应执行可靠 Publisher 并提交。
 */
void test_enqueue_failures_and_exceptions_roll_back_and_can_retry() {
  Engine engine{MonConfig{"m9-alarm-failure", 0U, 1U}};
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);

  const std::array<AlarmEnqueueStatus, 5U> failures{
      AlarmEnqueueStatus::NOT_READY, AlarmEnqueueStatus::ENCODE_FAILED,
      AlarmEnqueueStatus::RANDOM_FAILED, AlarmEnqueueStatus::SPOOL_FULL,
      AlarmEnqueueStatus::IO_ERROR};

  for (std::size_t index = 0U; index < failures.size(); ++index) {
    const AlarmEnqueueStatus status = failures[index];
    assert(engine.set_alarm_publisher(
        [status](MonData::StoredRecord) {
          return AlarmEnqueueResult{status, 28};
        }));

    const MonData::MonitorKey key{
        MODULE_ID, 2U, 20U, static_cast<std::uint32_t>(index)};
    const auto result = engine.report_error(key, 11U, "must roll back");

    assert(result._status == MonData::UpdateStatus::DURABILITY_FAILED);
    assert(!result.changed());
    assert(!result._record.has_value());
    assert(!engine.find_data(key).has_value());
  }

  const MonData::MonitorKey retry_key{MODULE_ID, 2U, 20U, 100U};
  assert(engine.set_alarm_publisher(
      [](MonData::StoredRecord) -> AlarmEnqueueResult {
        throw std::runtime_error("simulated spool exception");
      }));

  const auto thrown = engine.report_error(retry_key, 17U, "throw");
  assert(thrown._status == MonData::UpdateStatus::DURABILITY_FAILED);
  assert(!engine.find_data(retry_key).has_value());

  assert(engine.set_alarm_publisher(
      [](MonData::StoredRecord) { return durable_result(); }));
  const auto retried = engine.report_error(retry_key, 17U, "retry");
  assert(retried._status == MonData::UpdateStatus::INSERTED);
  assert(engine.find_data(retry_key).has_value());
}

/*
 * 已存在记录的下一次可靠持久化失败时，Store 必须完整保留旧值、旧描述和
 * 旧时间戳；恢复后同一个新值可以再次尝试并成为 UPDATED。
 */
void test_failed_reliable_update_preserves_existing_record() {
  Engine engine{MonConfig{"m9-alarm-existing", 0U, 1U}};
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);

  assert(engine.set_alarm_publisher(
      [](MonData::StoredRecord) { return durable_result(); }));

  const MonData::MonitorKey key{MODULE_ID, 2U, 21U, 1U};
  assert(engine.report_error(key, 20U, "old description")._status ==
         MonData::UpdateStatus::INSERTED);
  const auto before = engine.find_data(key);
  assert(before.has_value());

  assert(engine.set_alarm_publisher([](MonData::StoredRecord) {
    return AlarmEnqueueResult{AlarmEnqueueStatus::IO_ERROR, 5};
  }));
  const auto failed = engine.report_error(key, 21U, "new description");
  assert(failed._status == MonData::UpdateStatus::DURABILITY_FAILED);

  const auto after_failure = engine.find_data(key);
  assert(after_failure.has_value());
  assert(numeric_value(*after_failure) == numeric_value(*before));
  assert(after_failure->_data._description == before->_data._description);
  assert(after_failure->_changed_at == before->_changed_at);

  assert(engine.set_alarm_publisher(
      [](MonData::StoredRecord) { return durable_result(); }));
  const auto retried = engine.report_error(key, 21U, "new description");
  assert(retried._status == MonData::UpdateStatus::UPDATED);
  assert(numeric_value(*engine.find_data(key))._value == 21U);
}

/*
 * M8 补全必须发生在可靠持久化前。AlarmPublisher 收到按值副本，修改副本
 * 中的描述和值不能污染最终提交到 Store 的候选记录。
 */
void test_alarm_publisher_receives_enriched_copy() {
  Engine engine{MonConfig{"m9-alarm-enrichment", 0U, 1U}};
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);

  assert(engine.register_module(MonitorModuleInfo{
             MODULE_ID,
             "reliable-module",
             "reliable alarm test module",
             {{MonitorLevel::INFO, "unused"},
              {MonitorLevel::EMERGENCY_STOP, "disk unavailable"}}}) ==
         ModuleRegisterStatus::SUCCESS);

  MonData::StoredRecord received;
  bool called = false;
  assert(engine.set_alarm_publisher([&](MonData::StoredRecord record) {
    called = true;
    received = record;

    record._data._description = "publisher mutation";
    std::get<MonData::NumericValue>(record._data._value)._value = 999U;
    return durable_result();
  }));

  const MonData::MonitorKey input_key{MODULE_ID, 99U, 30U, 1U};
  const auto result = engine.report_error(input_key, 31U, "caller text");
  assert(result._status == MonData::UpdateStatus::INSERTED);
  assert(called);

  const std::uint32_t expected_level =
      static_cast<std::uint32_t>(MonitorLevel::EMERGENCY_STOP);
  assert(received._data._key._level == expected_level);
  assert(received._data._description == "disk unavailable");
  assert(numeric_value(received)._value == 31U);
  assert(numeric_value(received)._state == 2U);

  const MonData::MonitorKey stored_key{MODULE_ID, expected_level, 30U, 1U};
  const auto stored = engine.find_data(stored_key);
  assert(stored.has_value());
  assert(stored->_data._description == "disk unavailable");
  assert(numeric_value(*stored)._value == 31U);
  assert(stored->_changed_at == received._changed_at);
}

/*
 * 可靠 Publisher 只接管 report_error()。count、string 和统一 update_data()
 * 必须继续经过普通 Publisher，确保 M5/M6 入口行为没有改变。
 */
void test_non_error_reporting_stays_on_normal_publisher() {
  Engine engine{MonConfig{"m9-alarm-non-error", 0U, 1U}};
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);

  std::atomic<std::size_t> normal_calls{0U};
  std::atomic<std::size_t> alarm_calls{0U};
  assert(engine.set_publisher([&](MonData::StoredRecord) {
    normal_calls.fetch_add(1U, std::memory_order_relaxed);
  }));
  assert(engine.set_alarm_publisher([&](MonData::StoredRecord) {
    alarm_calls.fetch_add(1U, std::memory_order_relaxed);
    return durable_result();
  }));

  assert(engine.report_count({MODULE_ID, 0U, 40U, 1U}, 1U)._status ==
         MonData::UpdateStatus::INSERTED);
  assert(engine.report_string({MODULE_ID, 0U, 40U, 2U}, "online")._status ==
         MonData::UpdateStatus::INSERTED);
  assert(engine
             .update_data(MonData::MonitorData{
                 {MODULE_ID, 0U, 40U, 3U}, "direct",
                 MonData::NumericValue{3U, 0U}})
             ._status == MonData::UpdateStatus::INSERTED);

  assert(normal_calls.load(std::memory_order_relaxed) == 3U);
  assert(alarm_calls.load(std::memory_order_relaxed) == 0U);

  assert(engine.report_error({MODULE_ID, 0U, 40U, 4U}, 4U)._status ==
         MonData::UpdateStatus::INSERTED);
  assert(normal_calls.load(std::memory_order_relaxed) == 3U);
  assert(alarm_calls.load(std::memory_order_relaxed) == 1U);
}

/*
 * report_error() 必须在 Publisher 锁内只复制回调。旧回调正在执行时注销操作
 * 仍能完成；已经取得的快照允许结束，注销后的新告警恢复到普通通道。
 */
void test_unregister_affects_only_future_alarm_reports() {
  Engine engine{MonConfig{"m9-alarm-unregister", 0U, 1U}};
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);

  std::mutex gate_mutex;
  std::condition_variable gate_condition;
  bool entered = false;
  bool release = false;
  std::atomic<std::size_t> alarm_calls{0U};
  std::atomic<std::size_t> normal_calls{0U};

  assert(engine.set_publisher([&](MonData::StoredRecord) {
    normal_calls.fetch_add(1U, std::memory_order_relaxed);
  }));
  assert(engine.set_alarm_publisher([&](MonData::StoredRecord) {
    alarm_calls.fetch_add(1U, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(gate_mutex);
    entered = true;
    gate_condition.notify_all();
    gate_condition.wait(lock, [&] { return release; });
    return durable_result();
  }));

  MonData::UpdateResult first_result;
  std::thread reporter([&] {
    first_result =
        engine.report_error({MODULE_ID, 1U, 50U, 1U}, 1U, "in flight");
  });

  {
    std::unique_lock<std::mutex> lock(gate_mutex);
    assert(gate_condition.wait_for(lock, 2s, [&] { return entered; }));
  }

  std::atomic<bool> unregistered{false};
  std::thread unregister_thread([&] {
    const bool accepted = engine.set_alarm_publisher({});
    assert(accepted);
    unregistered.store(true, std::memory_order_release);
  });

  const bool completed_while_callback_blocked = wait_until([&] {
    return unregistered.load(std::memory_order_acquire);
  });

  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    release = true;
  }
  gate_condition.notify_all();

  unregister_thread.join();
  reporter.join();

  assert(completed_while_callback_blocked);
  assert(first_result._status == MonData::UpdateStatus::INSERTED);
  assert(alarm_calls.load(std::memory_order_relaxed) == 1U);

  const auto second =
      engine.report_error({MODULE_ID, 1U, 50U, 2U}, 2U, "after unregister");
  assert(second._status == MonData::UpdateStatus::INSERTED);
  assert(alarm_calls.load(std::memory_order_relaxed) == 1U);
  assert(normal_calls.load(std::memory_order_relaxed) == 1U);
}

/*
 * 多线程同时上报同一个首次错误时，Store 的事务锁必须让可靠 Publisher 只
 * 执行一次：一个调用 INSERTED，其余全部 UNCHANGED。
 */
void test_concurrent_same_error_enqueues_once() {
  Engine engine{MonConfig{"m9-alarm-concurrent", 0U, 1U}};
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);

  std::atomic<std::size_t> alarm_calls{0U};
  assert(engine.set_alarm_publisher([&](MonData::StoredRecord) {
    alarm_calls.fetch_add(1U, std::memory_order_relaxed);
    return durable_result();
  }));

  constexpr std::size_t thread_count = 16U;
  std::array<MonData::UpdateStatus, thread_count> statuses{};
  std::atomic<bool> start{false};
  const MonData::MonitorKey key{MODULE_ID, 2U, 60U, 1U};
  std::vector<std::thread> workers;
  workers.reserve(thread_count);

  for (std::size_t index = 0U; index < thread_count; ++index) {
    workers.emplace_back([&, index] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      statuses[index] = engine.report_error(key, 88U, "same error")._status;
    });
  }

  start.store(true, std::memory_order_release);
  for (std::thread &worker : workers) {
    worker.join();
  }

  assert(std::count(statuses.begin(), statuses.end(),
                    MonData::UpdateStatus::INSERTED) == 1);
  assert(std::count(statuses.begin(), statuses.end(),
                    MonData::UpdateStatus::UNCHANGED) ==
         static_cast<std::ptrdiff_t>(thread_count - 1U));
  assert(alarm_calls.load(std::memory_order_relaxed) == 1U);
  assert(engine.query_data().size() == 1U);
}

/*
 * CREATED、STOPPING 和 STOPPED 不允许注册可靠 Publisher或上报新告警；
 * READY 和 RUNNING 允许注册。阻塞的异步 Timer 保证 STOPPING 状态可观察。
 */
void test_engine_lifecycle_rules_for_alarm_publisher() {
  Engine engine{MonConfig{"m9-alarm-lifecycle", 0U, 1U}};

  const auto publisher =
      [](MonData::StoredRecord) { return durable_result(); };
  assert(!engine.set_alarm_publisher(publisher));
  assert(engine.report_error({MODULE_ID, 1U, 70U, 1U}, 1U)._status ==
         MonData::UpdateStatus::INVALID);

  assert(engine.init() == ENGINESTATE::SUCCESSFUL);
  assert(engine.set_alarm_publisher(publisher));

  std::mutex gate_mutex;
  std::condition_variable gate_condition;
  bool entered = false;
  bool release = false;

  const auto timer = engine.set_timer(
      MonCallback{"m9-blocking-worker",
                  [&]() -> int {
                    std::unique_lock<std::mutex> lock(gate_mutex);
                    entered = true;
                    gate_condition.notify_all();
                    gate_condition.wait(lock, [&] { return release; });
                    return 0;
                  },
                  false},
      TimerFlags::RECURRING | TimerFlags::WORKER, 1ms);
  assert(timer.has_value());

  ENGINESTATE run_result = ENGINESTATE::INITFAILED;
  std::thread runner([&] { run_result = engine.run(); });

  {
    std::unique_lock<std::mutex> lock(gate_mutex);
    assert(gate_condition.wait_for(lock, 2s, [&] { return entered; }));
  }
  assert(engine.get_phase() == EnginePhase::RUNNING);
  assert(engine.set_alarm_publisher(publisher));

  engine.stop();
  assert(engine.get_phase() == EnginePhase::STOPPING);
  assert(!engine.set_alarm_publisher(publisher));
  assert(engine.report_error({MODULE_ID, 1U, 70U, 2U}, 2U)._status ==
         MonData::UpdateStatus::INVALID);

  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    release = true;
  }
  gate_condition.notify_all();
  runner.join();

  assert(run_result == ENGINESTATE::SUCCESSFUL);
  assert(engine.get_phase() == EnginePhase::STOPPED);
  assert(!engine.set_alarm_publisher(publisher));
  assert(engine.report_error({MODULE_ID, 1U, 70U, 3U}, 3U)._status ==
         MonData::UpdateStatus::INVALID);
}

} // namespace

int main() {
  test_alarm_enqueue_result_contract();
  test_error_falls_back_to_normal_publisher();
  test_reliable_route_deduplicates_and_bypasses_normal_publisher();
  test_enqueue_failures_and_exceptions_roll_back_and_can_retry();
  test_failed_reliable_update_preserves_existing_record();
  test_alarm_publisher_receives_enriched_copy();
  test_non_error_reporting_stays_on_normal_publisher();
  test_unregister_affects_only_future_alarm_reports();
  test_concurrent_same_error_enqueues_once();
  test_engine_lifecycle_rules_for_alarm_publisher();
  std::cout << "M9_RELIABLE_ALARM_ROUTING=PASS\n";
  return 0;
}
