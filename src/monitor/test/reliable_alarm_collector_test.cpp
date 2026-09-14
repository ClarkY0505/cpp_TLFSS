#include "alarm_spool.h"
#include "engine.h"
#include "reliable_alarm_collector.h"
#include "reliable_alarm_test_support.h"

#include <sys/socket.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace TLSSMON;
using namespace ReliableAlarmTest;
using namespace std::chrono_literals;

namespace {

constexpr std::uint64_t SOURCE_ID = UINT64_C(0x1111222233334444);

ReliableAlarmCollectorConfig make_config(
    const std::filesystem::path &root,
    std::uint16_t port = 0U,
    std::size_t max_clients = 8U,
    std::chrono::milliseconds timeout = 1s) {
  return ReliableAlarmCollectorConfig{
      "127.0.0.1", port, root, max_clients, timeout};
}

void initialize_engine(Engine &engine) {
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);
  assert(engine.get_phase() == EnginePhase::READY);
}

const MonData::NumericValue &numeric_value(
    const MonData::StoredRecord &record) {
  assert(std::holds_alternative<MonData::NumericValue>(
      record._data._value));
  return std::get<MonData::NumericValue>(record._data._value);
}

void assert_matching_control(
    const AlarmWire::AlarmFrame &response,
    AlarmWire::AlarmFrameType type,
    const AlarmWire::AlarmFrame &request) {
  assert(response._type == type);
  assert(response._source_id == request._source_id);
  assert(response._message_id == request._message_id);
  assert(response._payload.empty());
}

/*
 * 配置、Engine 生命周期、inbox 初始化和端口占用错误必须在创建网络线程前
 * 转换为明确的 setup_status，不能得到一个半初始化 Collector。
 */
void test_setup_validation_and_failure_status() {
  TemporaryDirectory root("collector-config");
  Engine uninitialized{MonConfig{"collector-created", 0U, 1U}};
  ReliableAlarmCollector not_ready(
      uninitialized, make_config(root.path()));
  assert(!not_ready.ready());
  assert(not_ready.setup_status() ==
         ReliableAlarmCollectorSetupStatus::ENGINE_NOT_READY);

  Engine engine{MonConfig{"collector-config", 0U, 1U}};
  initialize_engine(engine);

  const auto assert_invalid = [&](ReliableAlarmCollectorConfig config) {
    ReliableAlarmCollector collector(engine, std::move(config));
    assert(!collector.ready());
    assert(collector.setup_status() ==
           ReliableAlarmCollectorSetupStatus::INVALID_CONFIG);
    assert(collector.setup_error() == EINVAL);
  };

  auto invalid = make_config(root.path());
  invalid._bind_host.clear();
  assert_invalid(invalid);
  invalid = make_config(root.path());
  invalid._inbox_dir.clear();
  assert_invalid(invalid);
  invalid = make_config(root.path());
  invalid._max_clients = 0U;
  assert_invalid(invalid);
  invalid = make_config(root.path());
  invalid._client_timeout = 0ms;
  assert_invalid(invalid);

  TemporaryDirectory file_root("collector-spool-error");
  {
    std::ofstream output(file_root.path());
    output << "not a directory";
  }
  ReliableAlarmCollector spool_error(
      engine, make_config(file_root.path()));
  assert(!spool_error.ready());
  assert(spool_error.setup_status() ==
         ReliableAlarmCollectorSetupStatus::SPOOL_ERROR);

  BoundTcpListener occupied;
  ReliableAlarmCollector socket_error(
      engine, make_config(root.path() / "occupied", occupied.port()));
  assert(!socket_error.ready());
  assert(socket_error.setup_status() ==
         ReliableAlarmCollectorSetupStatus::SOCKET_ERROR);
}

/* 配置端口 0 必须返回实际端口；stop() 可重复调用并及时唤醒 poll()。 */
void test_lifecycle_and_dynamic_port() {
  TemporaryDirectory root("collector-lifecycle");
  Engine engine{MonConfig{"collector-lifecycle", 0U, 1U}};
  initialize_engine(engine);

  ReliableAlarmCollector collector(engine, make_config(root.path()));
  assert(collector.ready());
  assert(collector.setup_status() ==
         ReliableAlarmCollectorSetupStatus::SUCCESS);
  assert(collector.bound_port() != 0U);
  assert(collector.config()._inbox_dir == root.path());

  const auto start = std::chrono::steady_clock::now();
  collector.stop();
  collector.stop();
  assert(std::chrono::steady_clock::now() - start < 1s);
  assert(!collector.ready());
}

/*
 * 分片输入一条 V2 ALARM，验证先写 inbox、再保留完整数据和生产端时间戳、
 * 最后发送同时匹配 source ID 与 message ID 的 ACK。
 */
void test_fragmented_alarm_is_persisted_ingested_and_acked() {
  TemporaryDirectory root("collector-ingest");
  Engine engine{MonConfig{"collector-ingest", 0U, 1U}};
  initialize_engine(engine);
  ReliableAlarmCollector collector(engine, make_config(root.path()));
  assert(collector.ready());

  const MonData::StoredRecord original =
      make_record(1U, 700U, 1'700'000'010, "accept failed");
  const AlarmWire::AlarmFrame alarm =
      make_alarm(original, SOURCE_ID, 1U);
  const auto wire = encode_frame(alarm);

  UniqueSocket client = connect_loopback(collector.bound_port());
  assert(client.get() >= 0);
  assert(send_fragmented(client.get(), wire));
  const auto ack = receive_frame(client.get());
  assert(ack.has_value());
  assert_matching_control(*ack, AlarmWire::AlarmFrameType::ACK, alarm);

  const auto stored = engine.find_data(original._data._key);
  assert(stored.has_value());
  assert(stored->_data._description == original._data._description);
  assert(numeric_value(*stored) == numeric_value(original));
  assert(stored->_changed_at == original._changed_at);

  AlarmWire::AlarmSpool inbox(
      {root.path(), AlarmWire::AlarmSpoolKind::INBOX, 0U});
  const auto stats = inbox.stats();
  assert(stats.success());
  assert(stats._stats._files == 1U);
}

/* 相同身份重复发送必须得到第二个 ACK，但 inbox 中仍然只有一个文件。 */
void test_duplicate_alarm_is_idempotent_and_acked_again() {
  TemporaryDirectory root("collector-duplicate");
  Engine engine{MonConfig{"collector-duplicate", 0U, 1U}};
  initialize_engine(engine);
  ReliableAlarmCollector collector(engine, make_config(root.path()));

  const auto original = make_record(2U, 88U);
  const auto alarm = make_alarm(original, SOURCE_ID, 2U);
  const auto wire = encode_frame(alarm);
  UniqueSocket client = connect_loopback(collector.bound_port());
  assert(client.get() >= 0);

  assert(send_all(client.get(), wire.data(), wire.size()));
  const auto first_ack = receive_frame(client.get());
  assert(first_ack.has_value());
  assert_matching_control(*first_ack, AlarmWire::AlarmFrameType::ACK, alarm);

  assert(send_all(client.get(), wire.data(), wire.size()));
  const auto second_ack = receive_frame(client.get());
  assert(second_ack.has_value());
  assert_matching_control(*second_ack, AlarmWire::AlarmFrameType::ACK, alarm);

  AlarmWire::AlarmSpool inbox(
      {root.path(), AlarmWire::AlarmSpoolKind::INBOX, 0U});
  assert(inbox.stats()._stats._files == 1U);
  assert(numeric_value(*engine.find_data(original._data._key))._value == 88U);
}

/* 空闲连接发送 PING 时，Collector 必须返回相同身份的 PONG。 */
void test_ping_receives_matching_pong() {
  TemporaryDirectory root("collector-ping");
  Engine engine{MonConfig{"collector-ping", 0U, 1U}};
  initialize_engine(engine);
  ReliableAlarmCollector collector(engine, make_config(root.path()));

  AlarmWire::AlarmFrame ping;
  ping._type = AlarmWire::AlarmFrameType::PING;
  ping._source_id = SOURCE_ID;
  ping._message_id = make_message_id(9U);
  ping._timestamp_ms = UINT64_C(1700000000000);
  const auto wire = encode_frame(ping);

  UniqueSocket client = connect_loopback(collector.bound_port());
  assert(send_all(client.get(), wire.data(), wire.size()));
  const auto pong = receive_frame(client.get());
  assert(pong.has_value());
  assert_matching_control(*pong, AlarmWire::AlarmFrameType::PONG, ping);
}

/*
 * Publisher 半关闭写方向后，Collector 仍必须把已经排队的 ACK 发完，然后
 * 关闭连接，不能在看到 EOF 时丢弃确认。
 */
void test_half_close_flushes_ack_before_disconnect() {
  TemporaryDirectory root("collector-half-close");
  Engine engine{MonConfig{"collector-half-close", 0U, 1U}};
  initialize_engine(engine);
  ReliableAlarmCollector collector(engine, make_config(root.path()));

  const auto record = make_record(3U, 99U);
  const auto alarm = make_alarm(record, SOURCE_ID, 3U);
  const auto wire = encode_frame(alarm);
  UniqueSocket client = connect_loopback(collector.bound_port());
  assert(send_all(client.get(), wire.data(), wire.size()));
  assert(::shutdown(client.get(), SHUT_WR) == 0);

  const auto ack = receive_frame(client.get());
  assert(ack.has_value());
  assert_matching_control(*ack, AlarmWire::AlarmFrameType::ACK, alarm);
  assert(wait_for_close(client.get()));
  assert(engine.find_data(record._data._key).has_value());
}

/*
 * CRC 错误和信封中的 V1 payload 都必须只关闭对应客户端；随后新客户端发送
 * 的合法 V2 ALARM 仍然可以保存并得到 ACK。
 */
void test_protocol_errors_are_isolated_between_clients() {
  TemporaryDirectory root("collector-protocol");
  Engine engine{MonConfig{"collector-protocol", 0U, 1U}};
  initialize_engine(engine);
  ReliableAlarmCollector collector(engine, make_config(root.path()));

  const auto valid_record = make_record(4U, 123U);
  auto corrupt = encode_frame(make_alarm(valid_record, SOURCE_ID, 4U));
  corrupt.back() ^= 0x01U;
  UniqueSocket bad_crc = connect_loopback(collector.bound_port());
  assert(send_all(bad_crc.get(), corrupt.data(), corrupt.size()));
  assert(wait_for_close(bad_crc.get()));

  const auto v1_payload = Wire::encode_v1(valid_record);
  assert(v1_payload._status == Wire::WireStatus::SUCCESS);
  AlarmWire::AlarmFrame v1_alarm;
  v1_alarm._type = AlarmWire::AlarmFrameType::ALARM;
  v1_alarm._source_id = SOURCE_ID;
  v1_alarm._message_id = make_message_id(5U);
  v1_alarm._timestamp_ms = UINT64_C(1700000000000);
  v1_alarm._payload = v1_payload._bytes;
  const auto v1_wire = encode_frame(v1_alarm);
  UniqueSocket bad_version = connect_loopback(collector.bound_port());
  assert(send_all(bad_version.get(), v1_wire.data(), v1_wire.size()));
  assert(wait_for_close(bad_version.get()));

  const auto valid_alarm = make_alarm(valid_record, SOURCE_ID, 6U);
  const auto valid_wire = encode_frame(valid_alarm);
  UniqueSocket good = connect_loopback(collector.bound_port());
  assert(send_all(good.get(), valid_wire.data(), valid_wire.size()));
  const auto ack = receive_frame(good.get());
  assert(ack.has_value());
  assert_matching_control(*ack, AlarmWire::AlarmFrameType::ACK, valid_alarm);
  assert(engine.find_data(valid_record._data._key).has_value());
}

/*
 * inbox 持久化路径在运行中失效时不得发送 ACK，也不能提前更新 Engine。
 */
void test_persist_failure_does_not_ack_or_update_engine() {
  TemporaryDirectory root("collector-persist-failure");
  Engine engine{MonConfig{"collector-persist-failure", 0U, 1U}};
  initialize_engine(engine);
  ReliableAlarmCollector collector(engine, make_config(root.path()));
  assert(collector.ready());

  std::error_code error;
  std::filesystem::remove_all(root.path() / "accepted", error);
  assert(!error);
  {
    std::ofstream blocker(root.path() / "accepted");
    blocker << "not a directory";
  }

  const auto record = make_record(5U, 500U);
  const auto wire = encode_frame(make_alarm(record, SOURCE_ID, 7U));
  UniqueSocket client = connect_loopback(collector.bound_port());
  assert(send_all(client.get(), wire.data(), wire.size()));
  assert(wait_for_close(client.get()));
  assert(!engine.find_data(record._data._key).has_value());
}

/* 发送半个头部后保持沉默，必须在绝对 client_timeout 后断开。 */
void test_slow_partial_client_is_timed_out() {
  TemporaryDirectory root("collector-timeout");
  Engine engine{MonConfig{"collector-timeout", 0U, 1U}};
  initialize_engine(engine);
  ReliableAlarmCollector collector(
      engine, make_config(root.path(), 0U, 8U, 120ms));

  const auto wire = encode_frame(
      make_alarm(make_record(6U, 600U), SOURCE_ID, 8U));
  UniqueSocket client = connect_loopback(collector.bound_port());
  assert(send_all(client.get(), wire.data(), 10U));
  assert(wait_for_close(client.get(), 1s));
  assert(!engine.find_data(make_record(6U, 600U)._data._key).has_value());
}

/*
 * max_clients=1 时，已经接纳的慢客户端占用唯一名额；第二条连接必须被关闭，
 * 而第一条连接补全剩余帧后仍能正常得到 ACK。
 */
void test_maximum_client_limit_rejects_extra_connection() {
  TemporaryDirectory root("collector-client-limit");
  Engine engine{MonConfig{"collector-client-limit", 0U, 1U}};
  initialize_engine(engine);
  ReliableAlarmCollector collector(
      engine, make_config(root.path(), 0U, 1U, 2s));

  const auto record = make_record(9U, 900U);
  const auto alarm = make_alarm(record, SOURCE_ID, 11U);
  const auto wire = encode_frame(alarm);

  UniqueSocket first = connect_loopback(collector.bound_port());
  assert(first.get() >= 0);
  assert(send_all(first.get(), wire.data(), 1U));

  /* 给 poll 线程一次接纳并保存半帧的机会。 */
  std::this_thread::sleep_for(50ms);

  UniqueSocket extra = connect_loopback(collector.bound_port());
  assert(extra.get() >= 0);
  AlarmWire::AlarmFrame ping;
  ping._type = AlarmWire::AlarmFrameType::PING;
  ping._source_id = SOURCE_ID;
  ping._message_id = make_message_id(12U);
  const auto ping_wire = encode_frame(ping);
  (void)send_all(extra.get(), ping_wire.data(), ping_wire.size());
  assert(wait_for_close(extra.get()));

  assert(send_all(first.get(), wire.data() + 1U, wire.size() - 1U));
  const auto ack = receive_frame(first.get());
  assert(ack.has_value());
  assert_matching_control(*ack, AlarmWire::AlarmFrameType::ACK, alarm);
  assert(engine.find_data(record._data._key).has_value());
}

/*
 * 启动恢复必须使用 timestamp_ms → source_id → message_id 排序，而不是随机
 * 文件遍历顺序；损坏文件被隔离，恢复后的身份可以直接响应重复 ACK。
 */
void test_startup_recovery_is_sorted_and_quarantines_corrupt_files() {
  TemporaryDirectory root("collector-recovery");
  const MonData::MonitorKey key = make_record(7U, 1U)._data._key;
  AlarmWire::AlarmFrame expected_last;

  {
    AlarmWire::AlarmSpool inbox(
        {root.path(), AlarmWire::AlarmSpoolKind::INBOX, 0U});
    assert(inbox.ready());

    const std::vector<AlarmWire::AlarmFrame> frames{
        make_alarm(make_record(7U, 10U, 101), 2U, 20U, 1000U),
        make_alarm(make_record(7U, 20U, 102), 1U, 30U, 1000U),
        make_alarm(make_record(7U, 30U, 103), 2U, 10U, 1000U)};

    expected_last = frames[0];
    for (const auto &frame : frames) {
      const auto wire = encode_frame(frame);
      assert(inbox.store(frame._source_id, frame._message_id, wire).success());
    }

    auto damaged = make_alarm(make_record(8U, 999U), 3U, 40U, 2000U);
    auto damaged_wire = encode_frame(damaged);
    const auto stored =
        inbox.store(damaged._source_id, damaged._message_id, damaged_wire);
    assert(stored.success());
    {
      std::fstream file(stored._path,
                        std::ios::binary | std::ios::in | std::ios::out);
      assert(file.good());
      file.seekp(static_cast<std::streamoff>(AlarmWire::ALARM_HEADER_SIZE));
      const char changed = static_cast<char>(0xff);
      file.write(&changed, 1);
    }
  }

  Engine engine{MonConfig{"collector-recovery", 0U, 1U}};
  initialize_engine(engine);
  ReliableAlarmCollector collector(engine, make_config(root.path()));
  assert(collector.ready());

  const auto recovered = engine.find_data(key);
  assert(recovered.has_value());
  assert(numeric_value(*recovered)._value == 10U);
  assert(recovered->_changed_at ==
         make_record(7U, 10U, 101)._changed_at);

  AlarmWire::AlarmSpool inbox(
      {root.path(), AlarmWire::AlarmSpoolKind::INBOX, 0U});
  const auto stats = inbox.stats();
  assert(stats._stats._files == 3U);
  assert(stats._stats._corrupt_files == 1U);

  const auto duplicate_wire = encode_frame(expected_last);
  UniqueSocket client = connect_loopback(collector.bound_port());
  assert(send_all(client.get(), duplicate_wire.data(), duplicate_wire.size()));
  const auto ack = receive_frame(client.get());
  assert(ack.has_value());
  assert_matching_control(
      *ack, AlarmWire::AlarmFrameType::ACK, expected_last);
  assert(inbox.stats()._stats._files == 3U);
}

} // namespace

int main() {
  test_setup_validation_and_failure_status();
  test_lifecycle_and_dynamic_port();
  test_fragmented_alarm_is_persisted_ingested_and_acked();
  test_duplicate_alarm_is_idempotent_and_acked_again();
  test_ping_receives_matching_pong();
  test_half_close_flushes_ack_before_disconnect();
  test_protocol_errors_are_isolated_between_clients();
  test_persist_failure_does_not_ack_or_update_engine();
  test_slow_partial_client_is_timed_out();
  test_maximum_client_limit_rejects_extra_connection();
  test_startup_recovery_is_sorted_and_quarantines_corrupt_files();

  std::cout << "M9_RELIABLE_ALARM_COLLECTOR=PASS\n";
  return 0;
}
