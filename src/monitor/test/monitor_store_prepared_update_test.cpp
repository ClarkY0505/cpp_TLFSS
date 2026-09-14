#include "monitor_data.h"
#include "monitor_store.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using namespace TLSSMON;

namespace {

const MonData::MonitorKey TEST_KEY{100U, 2U, 30U, 40U};

MonData::MonitorTimestamp at_seconds(std::int64_t seconds) {
  return MonData::MonitorTimestamp{std::chrono::seconds{seconds}};
}

MonData::MonitorData numeric_data(std::uint32_t value,
                                  std::uint32_t state,
                                  std::string description = "numeric") {
  return MonData::MonitorData{TEST_KEY, std::move(description),
                              MonData::NumericValue{value, state}};
}

MonData::MonitorData string_data(std::string value,
                                 std::string description = "string") {
  return MonData::MonitorData{TEST_KEY, std::move(description),
                              std::move(value)};
}

MonData::NumericValue numeric_value(const MonData::StoredRecord &record) {
  return std::get<MonData::NumericValue>(record._data._value);
}

/* 新 Key 持久化失败不能留下 map 节点；同值随后可以重试。 */
void test_failed_insert_rolls_back_and_can_retry() {
  MonitorStore store;
  std::size_t prepare_calls = 0U;
  const auto failed = store.update_prepared(
      numeric_data(7U, 2U), true, at_seconds(1),
      [&](const MonData::StoredRecord &) {
        ++prepare_calls;
        return false;
      });
  assert(failed._status == MonData::UpdateStatus::DURABILITY_FAILED);
  assert(!failed.changed());
  assert(!failed._record.has_value());
  assert(prepare_calls == 1U);
  assert(store.size() == 0U);
  assert(!store.find(TEST_KEY).has_value());

  const auto retried = store.update_prepared(
      numeric_data(7U, 2U), true, at_seconds(2),
      [&](const MonData::StoredRecord &) {
        ++prepare_calls;
        return true;
      });
  assert(retried._status == MonData::UpdateStatus::INSERTED);
  assert(retried.changed());
  assert(prepare_calls == 2U);
  assert(store.size() == 1U);
  assert(store.find(TEST_KEY)->_changed_at == at_seconds(2));
}

/* 已有记录更新落盘失败时，旧值、元数据和时间戳必须全部保留。 */
void test_failed_update_preserves_old_record() {
  MonitorStore store;
  const auto inserted =
      store.update(numeric_data(10U, 0U, "old"), true, at_seconds(10));
  assert(inserted._status == MonData::UpdateStatus::INSERTED);

  const auto failed = store.update_prepared(
      numeric_data(20U, 2U, "new"), true, at_seconds(20),
      [](const MonData::StoredRecord &) { return false; });
  assert(failed._status == MonData::UpdateStatus::DURABILITY_FAILED);
  assert(!failed._record.has_value());

  const MonData::StoredRecord stored = *store.find(TEST_KEY);
  assert(numeric_value(stored) == (MonData::NumericValue{10U, 0U}));
  assert(stored._data._description == "old");
  assert(stored._changed_at == at_seconds(10));
}

/* prepare 成功后，候选记录整体替换旧记录。 */
void test_successful_update_commits_candidate() {
  MonitorStore store;
  assert(store.update(numeric_data(10U, 0U, "old"), true, at_seconds(1))
             .changed());
  MonData::StoredRecord observed{};
  const auto updated = store.update_prepared(
      numeric_data(20U, 2U, "new"), true, at_seconds(2),
      [&](const MonData::StoredRecord &candidate) {
        observed = candidate;
        return true;
      });
  assert(updated._status == MonData::UpdateStatus::UPDATED);
  assert(updated._record.has_value());
  assert(numeric_value(observed) == (MonData::NumericValue{20U, 2U}));
  assert(observed._data._description == "new");
  assert(observed._changed_at == at_seconds(2));
  const MonData::StoredRecord stored = *store.find(TEST_KEY);
  assert(updated._record->_data._key == stored._data._key);
  assert(updated._record->_data._description == stored._data._description);
  assert(updated._record->_data._value == stored._data._value);
  assert(updated._record->_changed_at == stored._changed_at);
}

/* 数值相同只比较 value；state、description、时间变化不触发 prepare。 */
void test_unchanged_numeric_does_not_prepare_or_overwrite_metadata() {
  MonitorStore store;
  store.update(numeric_data(10U, 0U, "old"), true, at_seconds(1));
  std::size_t calls = 0U;
  const auto unchanged = store.update_prepared(
      numeric_data(10U, 2U, "new"), true, at_seconds(2),
      [&](const MonData::StoredRecord &) {
        ++calls;
        return true;
      });
  assert(unchanged._status == MonData::UpdateStatus::UNCHANGED);
  assert(calls == 0U);
  const MonData::StoredRecord stored = *store.find(TEST_KEY);
  assert(numeric_value(stored) == (MonData::NumericValue{10U, 0U}));
  assert(stored._data._description == "old");
  assert(stored._changed_at == at_seconds(1));
}

/* 首次普通零值继续走 M4 门禁，不调用持久化回调。 */
void test_initial_zero_does_not_prepare() {
  MonitorStore store;
  std::size_t calls = 0U;
  const auto result = store.update_prepared(
      numeric_data(0U, 0U), false, at_seconds(1),
      [&](const MonData::StoredRecord &) {
        ++calls;
        return true;
      });
  assert(result._status == MonData::UpdateStatus::IGNORED_INITIAL_ZERO);
  assert(calls == 0U);
  assert(store.size() == 0U);
}

/* 字符串按完整内容去重；空字符串首次出现仍需要持久化。 */
void test_string_prepare_and_dedup() {
  MonitorStore store;
  std::size_t calls = 0U;
  const auto inserted = store.update_prepared(
      string_data(""), false, at_seconds(1),
      [&](const MonData::StoredRecord &) {
        ++calls;
        return true;
      });
  assert(inserted._status == MonData::UpdateStatus::INSERTED);
  assert(calls == 1U);

  const auto unchanged = store.update_prepared(
      string_data("", "changed metadata"), false, at_seconds(2),
      [&](const MonData::StoredRecord &) {
        ++calls;
        return true;
      });
  assert(unchanged._status == MonData::UpdateStatus::UNCHANGED);
  assert(calls == 1U);

  const auto updated = store.update_prepared(
      string_data("next"), false, at_seconds(3),
      [&](const MonData::StoredRecord &) {
        ++calls;
        return true;
      });
  assert(updated._status == MonData::UpdateStatus::UPDATED);
  assert(calls == 2U);
}

/* 空 prepare 和抛异常都必须隔离为 DURABILITY_FAILED。 */
void test_empty_and_throwing_prepare_fail_without_commit() {
  MonitorStore store;
  const MonitorStore::PrepareUpdate empty;
  const auto empty_result = store.update_prepared(
      numeric_data(1U, 2U), true, at_seconds(1), empty);
  assert(empty_result._status == MonData::UpdateStatus::DURABILITY_FAILED);
  assert(store.size() == 0U);

  const auto throwing_result = store.update_prepared(
      numeric_data(1U, 2U), true, at_seconds(2),
      [](const MonData::StoredRecord &) -> bool {
        throw std::runtime_error("durability failure");
      });
  assert(throwing_result._status == MonData::UpdateStatus::DURABILITY_FAILED);
  assert(store.size() == 0U);
}

/* prepare 接收到完整候选记录，并且修改副本不会影响最终 Store。 */
void test_prepare_receives_exact_candidate() {
  MonitorStore store;
  const MonData::MonitorTimestamp timestamp = at_seconds(123);
  const auto result = store.update_prepared(
      numeric_data(99U, 2U, "disk full"), true, timestamp,
      [&](const MonData::StoredRecord &candidate) {
        assert(candidate._data._key == TEST_KEY);
        assert(candidate._data._description == "disk full");
        assert(numeric_value(candidate) == (MonData::NumericValue{99U, 2U}));
        assert(candidate._changed_at == timestamp);
        MonData::StoredRecord copy = candidate;
        copy._data._description = "local copy";
        return true;
      });
  assert(result._status == MonData::UpdateStatus::INSERTED);
  assert(store.find(TEST_KEY)->_data._description == "disk full");
}

/* 普通 update() 完全绕过准备门，保持已有调用方式。 */
void test_plain_update_regression() {
  MonitorStore store;
  assert(store.update(numeric_data(1U, 0U), false, at_seconds(1))._status ==
         MonData::UpdateStatus::INSERTED);
  assert(store.update(numeric_data(1U, 2U), false, at_seconds(2))._status ==
         MonData::UpdateStatus::UNCHANGED);
  assert(store.update(numeric_data(2U, 0U), false, at_seconds(3))._status ==
         MonData::UpdateStatus::UPDATED);
}

/* 同 Key 同值并发首次写入，只允许一个线程执行 prepare。 */
void test_concurrent_same_key_prepares_once() {
  MonitorStore store;
  constexpr std::size_t thread_count = 16U;
  std::array<MonData::UpdateStatus, thread_count> statuses{};
  std::atomic<std::size_t> prepare_calls{0U};
  std::atomic<bool> start{false};
  std::vector<std::thread> workers;
  workers.reserve(thread_count);

  for (std::size_t i = 0U; i < thread_count; ++i) {
    workers.emplace_back([&, i] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      const auto result = store.update_prepared(
          numeric_data(55U, 2U), true, at_seconds(5),
          [&](const MonData::StoredRecord &) {
            prepare_calls.fetch_add(1U, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
            return true;
          });
      statuses[i] = result._status;
    });
  }

  start.store(true, std::memory_order_release);
  for (std::thread &worker : workers) {
    worker.join();
  }

  assert(prepare_calls.load(std::memory_order_relaxed) == 1U);
  assert(std::count(statuses.begin(), statuses.end(),
                    MonData::UpdateStatus::INSERTED) == 1);
  assert(std::count(statuses.begin(), statuses.end(),
                    MonData::UpdateStatus::UNCHANGED) == thread_count - 1U);
  assert(store.size() == 1U);
}

} // namespace

int main() {
  test_failed_insert_rolls_back_and_can_retry();
  test_failed_update_preserves_old_record();
  test_successful_update_commits_candidate();
  test_unchanged_numeric_does_not_prepare_or_overwrite_metadata();
  test_initial_zero_does_not_prepare();
  test_string_prepare_and_dedup();
  test_empty_and_throwing_prepare_fail_without_commit();
  test_prepare_receives_exact_candidate();
  test_plain_update_regression();
  test_concurrent_same_key_prepares_once();
  std::cout << "M9_TRANSACTIONAL_DEDUP=PASS\n";
  return 0;
}
