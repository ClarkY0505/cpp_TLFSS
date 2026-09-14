#include "alarm_spool.h"
#include "engine.h"
#include "reliable_alarm_collector.h"
#include "reliable_alarm_publisher.h"
#include "reliable_alarm_test_support.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace TLSSMON;
using namespace ReliableAlarmTest;
using namespace std::chrono_literals;

namespace {

ReliableAlarmCollectorConfig collector_config(
    const std::filesystem::path &inbox,
    std::uint16_t port = 0U,
    std::size_t max_clients = 16U) {
  return ReliableAlarmCollectorConfig{
      "127.0.0.1", port, inbox, max_clients, 2s};
}

ReliableAlarmPublisherConfig publisher_config(
    const std::filesystem::path &outbox,
    std::uint16_t port,
    std::uint64_t source_id) {
  return ReliableAlarmPublisherConfig{
      "127.0.0.1", port, source_id, outbox, 0U, 500ms, 250ms};
}

void initialize_engine(Engine &engine) {
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);
}

const MonData::NumericValue &numeric_value(
    const MonData::StoredRecord &record) {
  assert(std::holds_alternative<MonData::NumericValue>(
      record._data._value));
  return std::get<MonData::NumericValue>(record._data._value);
}

std::uint64_t outbox_files(const std::filesystem::path &root) {
  AlarmWire::AlarmSpool spool(
      {root, AlarmWire::AlarmSpoolKind::OUTBOX, 0U});
  const auto stats = spool.stats();
  return stats.success() ? stats._stats._files : UINT64_MAX;
}

/*
 * 完整生产链路：Producer Engine 的 report_error() 通过可靠 seam 落入 outbox，
 * Collector 持久化并保留生产端时间戳，ACK 后 Publisher 删除 pending。
 */
void test_engine_to_engine_reliable_delivery() {
  TemporaryDirectory outbox("transport-engine-outbox");
  TemporaryDirectory inbox("transport-engine-inbox");

  Engine collector_engine{MonConfig{"transport-collector", 0U, 1U}};
  initialize_engine(collector_engine);
  ReliableAlarmCollector collector(
      collector_engine, collector_config(inbox.path()));
  assert(collector.ready());

  Engine producer{MonConfig{"transport-producer", 0U, 2U}};
  initialize_engine(producer);
  auto publisher = std::make_shared<ReliableAlarmPublisher>(
      publisher_config(outbox.path(), collector.bound_port(), 101U));
  assert(publisher->ready());
  assert(producer.set_alarm_publisher(
      [publisher](MonData::StoredRecord record) {
        return publisher->enqueue(std::move(record));
      }));

  const MonData::MonitorKey key{0x500U, 2U, 7U, 1U};
  const auto produced =
      producer.report_error(key, 9001U, "peer timed out");
  assert(produced._status == MonData::UpdateStatus::INSERTED);
  assert(produced._record.has_value());

  assert(wait_until([&] {
    return collector_engine.find_data(key).has_value() &&
           outbox_files(outbox.path()) == 0U;
  }));

  const auto collected = collector_engine.find_data(key);
  assert(collected.has_value());
  assert(collected->_data._description == "peer timed out");
  assert(numeric_value(*collected)._value == 9001U);
  assert(numeric_value(*collected)._state == 2U);
  assert(collected->_changed_at == produced._record->_changed_at);

  AlarmWire::AlarmSpool persisted(
      {inbox.path(), AlarmWire::AlarmSpoolKind::INBOX, 0U});
  assert(persisted.stats()._stats._files == 1U);
}

/*
 * Publisher 先于 Collector 启动时，enqueue() 仍应成功并保留文件；Collector
 * 后续监听同一端口后，后台重连完成投递并删除 outbox。
 */
void test_publisher_reconnects_after_collector_starts_late() {
  TemporaryDirectory outbox("transport-late-outbox");
  TemporaryDirectory inbox("transport-late-inbox");
  std::uint16_t port;
  {
    BoundTcpListener reservation;
    port = reservation.port();
  }

  ReliableAlarmPublisher publisher(
      publisher_config(outbox.path(), port, 102U));
  const auto record = make_record(10U, 10U);
  assert(publisher.enqueue(record).durable());
  assert(outbox_files(outbox.path()) == 1U);

  Engine engine{MonConfig{"transport-late", 0U, 1U}};
  initialize_engine(engine);
  ReliableAlarmCollector collector(
      engine, collector_config(inbox.path(), port));
  assert(collector.ready());

  assert(wait_until([&] {
    return outbox_files(outbox.path()) == 0U &&
           engine.find_data(record._data._key).has_value();
  }, 5s));
  assert(engine.find_data(record._data._key)->_changed_at ==
         record._changed_at);
}

/*
 * Collector 重启时先从持久化 inbox 恢复旧记录，再开始监听；同一个 Publisher
 * 随后重连并投递停机期间产生的新记录。
 */
void test_collector_restart_recovers_history_before_new_delivery() {
  TemporaryDirectory outbox("transport-restart-outbox");
  TemporaryDirectory inbox("transport-restart-inbox");

  Engine first_engine{MonConfig{"transport-first", 0U, 1U}};
  initialize_engine(first_engine);
  ReliableAlarmCollector first(
      first_engine, collector_config(inbox.path()));
  assert(first.ready());
  const std::uint16_t port = first.bound_port();

  ReliableAlarmPublisher publisher(
      publisher_config(outbox.path(), port, 103U));
  const auto old_record = make_record(20U, 20U, 1'700'000'020);
  assert(publisher.enqueue(old_record).durable());
  assert(wait_until([&] {
    return first_engine.find_data(old_record._data._key).has_value() &&
           outbox_files(outbox.path()) == 0U;
  }));

  first.stop();

  const auto new_record = make_record(21U, 21U, 1'700'000'021);
  assert(publisher.enqueue(new_record).durable());
  assert(outbox_files(outbox.path()) == 1U);

  Engine second_engine{MonConfig{"transport-second", 0U, 2U}};
  initialize_engine(second_engine);
  ReliableAlarmCollector second(
      second_engine, collector_config(inbox.path(), port));
  assert(second.ready());

  /* ready() 为真时，旧 inbox 已经恢复完毕。 */
  const auto restored = second_engine.find_data(old_record._data._key);
  assert(restored.has_value());
  assert(restored->_changed_at == old_record._changed_at);

  assert(wait_until([&] {
    return second_engine.find_data(new_record._data._key).has_value() &&
           outbox_files(outbox.path()) == 0U;
  }, 5s));
}

/*
 * 多个 source 的 Publisher 可以并发连接同一 Collector。所有记录必须形成独立
 * inbox 文件，全部 outbox 最终清空，Collector Store 不丢 Key。
 */
void test_multiple_publishers_deliver_concurrently() {
  TemporaryDirectory inbox("transport-concurrent-inbox");
  constexpr std::size_t publisher_count = 4U;
  constexpr std::size_t records_per_publisher = 8U;
  constexpr std::size_t expected =
      publisher_count * records_per_publisher;

  Engine engine{MonConfig{"transport-concurrent", 0U, 1U}};
  initialize_engine(engine);
  ReliableAlarmCollector collector(
      engine, collector_config(inbox.path(), 0U, publisher_count));
  assert(collector.ready());

  std::array<std::unique_ptr<TemporaryDirectory>, publisher_count> roots;
  std::array<std::shared_ptr<ReliableAlarmPublisher>, publisher_count>
      publishers;

  for (std::size_t index = 0U; index < publisher_count; ++index) {
    roots[index] = std::make_unique<TemporaryDirectory>(
        "transport-concurrent-outbox-" + std::to_string(index));
    publishers[index] = std::make_shared<ReliableAlarmPublisher>(
        publisher_config(roots[index]->path(), collector.bound_port(),
                         1000U + index));
    assert(publishers[index]->ready());
  }

  std::atomic<bool> start{false};
  std::atomic<std::size_t> failures{0U};
  std::vector<std::thread> workers;

  for (std::size_t producer = 0U; producer < publisher_count; ++producer) {
    workers.emplace_back([&, producer] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (std::size_t sequence = 0U;
           sequence < records_per_publisher; ++sequence) {
        const std::uint32_t event_id = static_cast<std::uint32_t>(
            producer * records_per_publisher + sequence + 100U);
        if (!publishers[producer]
                 ->enqueue(make_record(event_id, event_id))
                 .durable()) {
          failures.fetch_add(1U, std::memory_order_relaxed);
        }
      }
    });
  }

  start.store(true, std::memory_order_release);
  for (std::thread &worker : workers) {
    worker.join();
  }
  assert(failures.load(std::memory_order_relaxed) == 0U);

  assert(wait_until([&] {
    if (engine.query_data().size() != expected) {
      return false;
    }
    return std::all_of(roots.begin(), roots.end(), [](const auto &root) {
      return outbox_files(root->path()) == 0U;
    });
  }, 10s));

  AlarmWire::AlarmSpool persisted(
      {inbox.path(), AlarmWire::AlarmSpoolKind::INBOX, 0U});
  assert(persisted.stats()._stats._files == expected);
}

/*
 * Collector 正在处理 Publisher 连接时 stop() 必须唤醒 poll、关闭客户端并
 * join；停止后产生的告警只能留在 Publisher outbox，不能被已停止实例 ACK。
 */
void test_collector_stop_does_not_ack_later_alarm() {
  TemporaryDirectory outbox("transport-stop-outbox");
  TemporaryDirectory inbox("transport-stop-inbox");
  Engine engine{MonConfig{"transport-stop", 0U, 1U}};
  initialize_engine(engine);
  ReliableAlarmCollector collector(
      engine, collector_config(inbox.path()));

  ReliableAlarmPublisher publisher(
      publisher_config(outbox.path(), collector.bound_port(), 104U));
  const auto first = make_record(30U, 30U);
  assert(publisher.enqueue(first).durable());
  assert(wait_until([&] { return outbox_files(outbox.path()) == 0U; }));

  const auto start = std::chrono::steady_clock::now();
  collector.stop();
  assert(std::chrono::steady_clock::now() - start < 1s);

  const auto second = make_record(31U, 31U);
  assert(publisher.enqueue(second).durable());
  std::this_thread::sleep_for(400ms);
  assert(outbox_files(outbox.path()) == 1U);
  assert(!engine.find_data(second._data._key).has_value());
}

} // namespace

int main() {
  test_engine_to_engine_reliable_delivery();
  test_publisher_reconnects_after_collector_starts_late();
  test_collector_restart_recovers_history_before_new_delivery();
  test_multiple_publishers_deliver_concurrently();
  test_collector_stop_does_not_ack_later_alarm();

  std::cout << "M9_RELIABLE_ALARM_TRANSPORT=PASS\n";
  return 0;
}
