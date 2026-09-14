#include "alarm_protocol.h"
#include "alarm_spool.h"
#include "cli_commands.h"
#include "cli_registry.h"
#include "engine.h"
#include "monitor_wire.h"
#include "reliable_alarm_collector.h"
#include "reliable_alarm_publisher.h"
#include "reliable_alarm_test_support.h"

#include <arpa/inet.h>
#include <sys/socket.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace TLSSMON;
using namespace ReliableAlarmTest;
using namespace std::chrono_literals;

namespace {

constexpr std::uint64_t SOURCE_ID = UINT64_C(0x1414141414141414);
constexpr const char *SECRET_DESCRIPTION = "secret alarm description";
constexpr const char *SECRET_PAYLOAD = "secret alarm payload";

ReliableAlarmPublisherConfig publisher_config(
    const std::filesystem::path &outbox, std::uint16_t port,
    std::chrono::milliseconds ack_timeout = 200ms) {
  return ReliableAlarmPublisherConfig{
      "127.0.0.1", port, SOURCE_ID, outbox, 0U, ack_timeout, 200ms};
}

ReliableAlarmCollectorConfig collector_config(
    const std::filesystem::path &inbox, std::size_t max_clients = 8U) {
  return ReliableAlarmCollectorConfig{
      "127.0.0.1", 0U, inbox, max_clients, 1s};
}

void initialize_engine(Engine &engine) {
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);
  assert(engine.get_phase() == EnginePhase::READY);
}

std::uint16_t reserve_unused_loopback_port() {
  UniqueSocket socket(::socket(AF_INET, SOCK_STREAM, 0));
  assert(socket.get() >= 0);

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(0U);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(::bind(socket.get(), reinterpret_cast<const sockaddr *>(&address),
                static_cast<socklen_t>(sizeof(address))) == 0);

  socklen_t size = static_cast<socklen_t>(sizeof(address));
  assert(::getsockname(socket.get(), reinterpret_cast<sockaddr *>(&address),
                       &size) == 0);
  return ntohs(address.sin_port);
}

MonData::StoredRecord secret_record(std::uint32_t event_id = 1U) {
  return MonData::StoredRecord{
      MonData::MonitorData{
          MonData::MonitorKey{0x140U, 2U, 0x240U, event_id},
          SECRET_DESCRIPTION,
          std::string{SECRET_PAYLOAD}},
      MonData::MonitorTimestamp{
          std::chrono::seconds{1'700'000'014} +
          std::chrono::nanoseconds{140'000'000}}};
}

void assert_contains(const std::string &output, const std::string &text) {
  assert(output.find(text) != std::string::npos);
}

void assert_does_not_contain(const std::string &output,
                             const std::string &text) {
  assert(output.find(text) == std::string::npos);
}

/*
 * alarm_status 没有关联可靠通道时应明确报告 inactive；参数错误和重复注册
 * 也必须走 CliRegistry 的稳定结果，不能静默覆盖旧 Handler。
 */
void test_registration_inactive_and_usage() {
  CliRegistry registry;
  assert(register_alarm_status_command(registry, nullptr, nullptr) ==
         CliRegisterStatus::SUCCESS);
  assert(register_alarm_status_command(registry, nullptr, nullptr) ==
         CliRegisterStatus::DUPLICATE_COMMAND);

  const CliDispatchResult inactive = registry.dispatch("alarm_status");
  assert(inactive._status == CliDispatchStatus::SUCCESS);
  assert(inactive._output == "alarm channel inactive\n");

  const CliDispatchResult usage =
      registry.dispatch("alarm_status unexpected");
  assert(usage._status == CliDispatchStatus::SUCCESS);
  assert(usage._output == "usage: alarm_status\n");
}

/*
 * Collector 不在线时，Publisher 仍应保持 active、保留 pending 文件并累计
 * connect_failures。并发 CLI 查询覆盖 status() 与重连线程之间的同步边界。
 */
void test_publisher_reconnect_status_and_concurrent_cli_queries() {
  TemporaryDirectory outbox("alarm-status-reconnect");
  const std::uint16_t port = reserve_unused_loopback_port();
  ReliableAlarmPublisher publisher(publisher_config(outbox.path(), port));
  assert(publisher.ready());

  const ReliableAlarmPublisherStatus initial = publisher.status();
  assert(initial._active);
  assert(!initial._connected);
  assert(initial._collector_host == "127.0.0.1");
  assert(initial._collector_port == port);
  assert(initial._spool_stats_available);
  assert(initial._pending_files == 0U);
  assert(initial._pending_bytes == 0U);
  assert(initial._corrupt_files == 0U);

  assert(publisher.enqueue(secret_record()).durable());
  assert(wait_until([&publisher] {
    const auto status = publisher.status();
    return status._connect_failures >= 1U && status._pending_files == 1U &&
           status._pending_bytes != 0U;
  }));

  /* status() 返回副本；修改旧快照不能污染 Publisher 内部配置。 */
  ReliableAlarmPublisherStatus changed_copy = publisher.status();
  changed_copy._collector_host = "modified-by-caller";
  changed_copy._connect_failures = UINT64_MAX;
  const ReliableAlarmPublisherStatus fresh = publisher.status();
  assert(fresh._collector_host == "127.0.0.1");
  assert(fresh._connect_failures != UINT64_MAX);

  CliRegistry registry;
  assert(register_alarm_status_command(registry, &publisher, nullptr) ==
         CliRegisterStatus::SUCCESS);

  std::atomic<bool> query_failed{false};
  std::vector<std::thread> readers;
  for (std::size_t index = 0U; index < 4U; ++index) {
    readers.emplace_back([&registry, &query_failed] {
      for (std::size_t iteration = 0U; iteration < 200U; ++iteration) {
        const CliDispatchResult result = registry.dispatch("alarm_status");
        if (result._status != CliDispatchStatus::SUCCESS ||
            result._output.find("alarm publisher: active=true") ==
                std::string::npos ||
            result._output.find("connect_failures=") == std::string::npos ||
            result._output.find("backoff_ms=") == std::string::npos) {
          query_failed.store(true, std::memory_order_relaxed);
        }
      }
    });
  }
  for (std::thread &reader : readers) {
    reader.join();
  }
  assert(!query_failed.load(std::memory_order_relaxed));

  const std::string output = registry.dispatch("alarm_status")._output;
  assert_contains(output, "connected=false");
  assert_contains(output, "collector_host=127.0.0.1");
  assert_contains(output, "collector_port=" + std::to_string(port));
  assert_contains(output, "pending_files=1");
  assert_contains(output, "pending_bytes=");
  assert_contains(output, "corrupt_files=0");
  assert_contains(output, "connect_failures=");
  assert_contains(output, "send_failures=");
  assert_contains(output, "ack_timeouts=");
  assert_contains(output, "acks=");
  assert_contains(output, "backoff_ms=");
  assert_does_not_contain(output, SECRET_DESCRIPTION);
  assert_does_not_contain(output, SECRET_PAYLOAD);
}

/*
 * TCP 握手成功但对端不读取、不返回 ACK 时，应累计 ack_timeouts，且告警继续
 * 留在 outbox 中等待重试。监听 socket 的存在让该场景不依赖外部服务。
 */
void test_publisher_ack_timeout_status() {
  TemporaryDirectory outbox("alarm-status-ack-timeout");
  BoundTcpListener silent_collector;
  ReliableAlarmPublisher publisher(
      publisher_config(outbox.path(), silent_collector.port(), 100ms));
  assert(publisher.enqueue(secret_record(2U)).durable());

  assert(wait_until([&publisher] {
    const ReliableAlarmPublisherStatus status = publisher.status();
    return status._ack_timeouts >= 1U && status._pending_files == 1U;
  }));

  const ReliableAlarmPublisherStatus status = publisher.status();
  assert(status._active);
  assert(status._ack_timeouts >= 1U);
  assert(status._acks == 0U);
  assert(status._pending_files == 1U);
}

/*
 * 真实 Publisher → Collector → ACK 链路必须同步反映为 Publisher 的 ACK、
 * 清空 pending，以及 Collector 的 accepted。一个命令同时输出两端快照。
 */
void test_successful_delivery_is_visible_from_both_sides() {
  TemporaryDirectory outbox("alarm-status-delivery-outbox");
  TemporaryDirectory inbox("alarm-status-delivery-inbox");
  Engine engine{MonConfig{"alarm-status-delivery", 0U, 1U}};
  initialize_engine(engine);
  ReliableAlarmCollector collector(engine, collector_config(inbox.path()));
  assert(collector.ready());

  {
    ReliableAlarmPublisher publisher(
        publisher_config(outbox.path(), collector.bound_port()));
    assert(publisher.ready());
    assert(publisher.enqueue(secret_record(3U)).durable());

    assert(wait_until([&publisher, &collector] {
      const auto publisher_status = publisher.status();
      const auto collector_status = collector.status();
      return publisher_status._acks == 1U &&
             publisher_status._pending_files == 0U &&
             collector_status._accepted == 1U;
    }));

    const ReliableAlarmPublisherStatus publisher_status = publisher.status();
    const ReliableAlarmCollectorStatus collector_status = collector.status();
    assert(publisher_status._active);
    assert(publisher_status._connected);
    assert(publisher_status._acks == 1U);
    assert(publisher_status._pending_files == 0U);
    assert(collector_status._active);
    assert(collector_status._accepted == 1U);

    CliRegistry registry;
    assert(register_alarm_status_command(registry, &publisher, &collector) ==
           CliRegisterStatus::SUCCESS);
    const CliDispatchResult result = registry.dispatch("alarm_status");
    assert(result._status == CliDispatchStatus::SUCCESS);
    assert_contains(result._output, "alarm publisher: active=true");
    assert_contains(result._output, "acks=1");
    assert_contains(result._output, "alarm collector: active=true");
    assert_contains(result._output, "accepted=1");
    assert_does_not_contain(result._output, SECRET_DESCRIPTION);
    assert_does_not_contain(result._output, SECRET_PAYLOAD);
  }

  collector.stop();
  const ReliableAlarmCollectorStatus stopped = collector.status();
  assert(!stopped._active);
  assert(stopped._clients == 0U);
  assert(stopped._accepted == 1U);
}

/*
 * 依次制造客户端上限、成功接收、磁盘重复、CRC 错误和 V1 payload 协议错误，
 * 验证 Collector 对外快照中的运行计数与真实网络事件一致。
 */
void test_collector_runtime_counters_and_cli_output() {
  TemporaryDirectory inbox("alarm-status-collector-counters");
  Engine engine{MonConfig{"alarm-status-collector", 0U, 1U}};
  initialize_engine(engine);
  ReliableAlarmCollector collector(
      engine, collector_config(inbox.path(), 1U));
  assert(collector.ready());

  const MonData::StoredRecord record = secret_record(4U);
  const AlarmWire::AlarmFrame alarm = make_alarm(record, SOURCE_ID, 4U);
  const std::vector<std::uint8_t> wire = encode_frame(alarm);

  UniqueSocket first = connect_loopback(collector.bound_port());
  assert(first.get() >= 0);
  assert(send_all(first.get(), wire.data(), 1U));
  assert(wait_until([&collector] { return collector.status()._clients == 1U; }));

  UniqueSocket rejected = connect_loopback(collector.bound_port());
  assert(rejected.get() >= 0);
  assert(wait_for_close(rejected.get()));
  assert(wait_until([&collector] {
    return collector.status()._rejected_clients >= 1U;
  }));

  assert(send_all(first.get(), wire.data() + 1U, wire.size() - 1U));
  assert(receive_frame(first.get()).has_value());
  assert(send_all(first.get(), wire.data(), wire.size()));
  assert(receive_frame(first.get()).has_value());
  assert(wait_until([&collector] {
    const auto status = collector.status();
    return status._accepted == 1U && status._duplicates == 1U;
  }));

  first = UniqueSocket{};
  assert(wait_until([&collector] { return collector.status()._clients == 0U; }));

  std::vector<std::uint8_t> corrupt = wire;
  corrupt.back() ^= 0x01U;
  UniqueSocket bad_crc = connect_loopback(collector.bound_port());
  assert(bad_crc.get() >= 0);
  assert(send_all(bad_crc.get(), corrupt.data(), corrupt.size()));
  assert(wait_for_close(bad_crc.get()));
  assert(wait_until([&collector] { return collector.status()._crc_errors == 1U; }));

  const Wire::EncodeResult v1_payload = Wire::encode_v1(record);
  assert(v1_payload._status == Wire::WireStatus::SUCCESS);
  AlarmWire::AlarmFrame v1_alarm = alarm;
  v1_alarm._message_id = make_message_id(5U);
  v1_alarm._payload = v1_payload._bytes;
  const std::vector<std::uint8_t> v1_wire = encode_frame(v1_alarm);
  UniqueSocket bad_protocol = connect_loopback(collector.bound_port());
  assert(bad_protocol.get() >= 0);
  assert(send_all(bad_protocol.get(), v1_wire.data(), v1_wire.size()));
  assert(wait_for_close(bad_protocol.get()));
  assert(wait_until([&collector] {
    return collector.status()._protocol_errors == 1U;
  }));

  ReliableAlarmCollectorStatus modified_copy = collector.status();
  modified_copy._accepted = UINT64_MAX;
  assert(modified_copy._accepted == UINT64_MAX);
  assert(collector.status()._accepted == 1U);

  CliRegistry registry;
  assert(register_alarm_status_command(registry, nullptr, &collector) ==
         CliRegisterStatus::SUCCESS);
  const std::string output = registry.dispatch("alarm_status")._output;
  assert_contains(output, "alarm collector: active=true");
  assert_contains(output, "clients=0");
  assert_contains(output, "rejected_clients=1");
  assert_contains(output, "accepted=1");
  assert_contains(output, "duplicates=1");
  assert_contains(output, "protocol_errors=1");
  assert_contains(output, "crc_errors=1");
  assert_contains(output, "persist_failures=0");
  assert_contains(output, "corrupt_files=0");
  assert_does_not_contain(output, SECRET_DESCRIPTION);
  assert_does_not_contain(output, SECRET_PAYLOAD);
}

/*
 * accepted 目录在运行中变成普通文件时，Collector 不能发送 ACK，并应同时
 * 暴露 persist_failures 和不可用的 Spool 统计，而不是返回伪造的零值。
 */
void test_collector_persist_failure_and_unavailable_spool_status() {
  TemporaryDirectory inbox("alarm-status-persist-failure");
  Engine engine{MonConfig{"alarm-status-persist", 0U, 1U}};
  initialize_engine(engine);
  ReliableAlarmCollector collector(engine, collector_config(inbox.path()));
  assert(collector.ready());

  std::error_code error;
  std::filesystem::remove_all(inbox.path() / "accepted", error);
  assert(!error);
  {
    std::ofstream blocker(inbox.path() / "accepted");
    blocker << "not a directory";
  }

  const MonData::StoredRecord record = secret_record(6U);
  const std::vector<std::uint8_t> wire =
      encode_frame(make_alarm(record, SOURCE_ID, 6U));
  UniqueSocket client = connect_loopback(collector.bound_port());
  assert(client.get() >= 0);
  assert(send_all(client.get(), wire.data(), wire.size()));
  assert(wait_for_close(client.get()));
  assert(wait_until([&collector] {
    return collector.status()._persist_failures == 1U;
  }));

  const ReliableAlarmCollectorStatus status = collector.status();
  assert(status._persist_failures == 1U);
  assert(!status._spool_stats_available);
  assert(status._spool_error != 0);
  assert(!engine.find_data(record._data._key).has_value());

  CliRegistry registry;
  assert(register_alarm_status_command(registry, nullptr, &collector) ==
         CliRegisterStatus::SUCCESS);
  const std::string output = registry.dispatch("alarm_status")._output;
  assert_contains(output, "persist_failures=1");
  assert_contains(output, "corrupt_files=unavailable");
  assert_contains(output, "spool_error=");
}

/*
 * 启动恢复遇到损坏的 inbox 文件时，文件必须进入 corrupt 目录，并通过
 * Collector 状态与 alarm_status 暴露数量。
 */
void test_collector_corrupt_file_count() {
  TemporaryDirectory inbox("alarm-status-corrupt-count");
  const MonData::StoredRecord record = secret_record(7U);
  const AlarmWire::AlarmFrame alarm = make_alarm(record, SOURCE_ID, 7U);
  const std::vector<std::uint8_t> wire = encode_frame(alarm);

  {
    AlarmWire::AlarmSpool spool(
        {inbox.path(), AlarmWire::AlarmSpoolKind::INBOX, 0U});
    assert(spool.ready());
    const AlarmWire::AlarmSpoolStoreResult stored =
        spool.store(alarm._source_id, alarm._message_id, wire);
    assert(stored.success());

    std::fstream file(stored._path,
                      std::ios::binary | std::ios::in | std::ios::out);
    assert(file.good());
    file.seekp(static_cast<std::streamoff>(AlarmWire::ALARM_HEADER_SIZE));
    const char changed = static_cast<char>(0xff);
    file.write(&changed, 1);
  }

  Engine engine{MonConfig{"alarm-status-corrupt", 0U, 1U}};
  initialize_engine(engine);
  ReliableAlarmCollector collector(engine, collector_config(inbox.path()));
  assert(collector.ready());

  const ReliableAlarmCollectorStatus status = collector.status();
  assert(status._spool_stats_available);
  assert(status._corrupt_files == 1U);

  CliRegistry registry;
  assert(register_alarm_status_command(registry, nullptr, &collector) ==
         CliRegisterStatus::SUCCESS);
  assert_contains(registry.dispatch("alarm_status")._output,
                  "corrupt_files=1");
}

} // namespace

int main() {
  test_registration_inactive_and_usage();
  test_publisher_reconnect_status_and_concurrent_cli_queries();
  test_publisher_ack_timeout_status();
  test_successful_delivery_is_visible_from_both_sides();
  test_collector_runtime_counters_and_cli_output();
  test_collector_persist_failure_and_unavailable_spool_status();
  test_collector_corrupt_file_count();

  std::cout << "M9_ALARM_CLI_STATUS=PASS\n";
  return 0;
}
