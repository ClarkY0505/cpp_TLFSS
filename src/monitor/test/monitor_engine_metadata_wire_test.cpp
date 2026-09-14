#include "cli_commands.h"
#include "engine.h"
#include "monitor_data.h"
#include "monitor_error.h"
#include "monitor_module.h"
#include "monitor_wire.h"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <variant>

namespace {

using namespace TLSSMON;

constexpr std::uint32_t MODULE_ID = 0x100U;
constexpr std::uint32_t UNKNOWN_MODULE_ID = 0x999U;

void initialize_with_module(Engine &engine,
                            MonitorLevel level,
                            std::string description) {
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);
  assert(engine.get_phase() == EnginePhase::READY);
  assert(engine.register_module(
             {MODULE_ID,
              "network",
              "network module",
              {{level, std::move(description)}}}) ==
         ModuleRegisterStatus::SUCCESS);
}

std::string dump_records(Engine &engine) {
  CliRegistry registry;
  assert(register_db_dump_command(registry, engine) ==
         CliRegisterStatus::SUCCESS);

  const CliDispatchResult result = registry.dispatch("db_dump");
  assert(result._status == CliDispatchStatus::SUCCESS);
  return result._output;
}

void assert_contains(const std::string &output, const std::string &expected) {
  if (output.find(expected) == std::string::npos) {
    std::cerr << "expected substring: " << expected << '\n'
              << "actual output: " << output;
  }
  assert(output.find(expected) != std::string::npos);
}

/*
 * V1 数值链路：Producer Reporter 先规范化元数据；V1 只传 Key/value，
 * Collector 通过 update_data() 保存原始线协议 level，CLI 再从自己的
 * Registry 回填空 description，且不能把回填内容写回 Store。
 */
void test_v1_numeric_producer_to_collector_metadata_flow() {
  Engine producer{MonConfig{"v1-producer", 0U, 1U}};
  Engine collector{MonConfig{"v1-collector", 0U, 2U}};

  initialize_with_module(producer, MonitorLevel::WARN, "producer timeout");
  initialize_with_module(collector, MonitorLevel::EMERGENCY_STOP,
                         "collector timeout");

  const MonData::UpdateResult produced = producer.report_count(
      {MODULE_ID, 99U, 1U, 0U}, 7U, {});

  assert(produced._status == MonData::UpdateStatus::INSERTED);
  assert(produced._record.has_value());
  assert(produced._record->_data._key._level ==
         static_cast<std::uint32_t>(MonitorLevel::WARN));
  assert(produced._record->_data._description == "producer timeout");

  const Wire::EncodeResult encoded =
      Wire::encode(*produced._record, Wire::WireVersion::V1);

  assert(encoded._status == Wire::WireStatus::SUCCESS);
  assert(encoded._bytes.size() == Wire::V1_NUMERIC_DATAGRAM_SIZE);
  assert(encoded._bytes.size() == 26U);

  Wire::DecodeResult decoded =
      Wire::decode(encoded._bytes.data(), encoded._bytes.size());

  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());
  assert(decoded._record->_version == Wire::WireVersion::V1);
  assert(decoded._record->_data._description.empty());
  assert(!decoded._record->_changed_at.has_value());
  assert(decoded._record->_data._key._level ==
         static_cast<std::uint32_t>(MonitorLevel::WARN));

  const MonData::MonitorKey wire_key = decoded._record->_data._key;
  const MonData::UpdateResult ingested =
      collector.update_data(std::move(decoded._record->_data));

  assert(ingested._status == MonData::UpdateStatus::INSERTED);
  assert(ingested._record.has_value());
  assert(ingested._record->_data._key == wire_key);
  assert(ingested._record->_data._key._level ==
         static_cast<std::uint32_t>(MonitorLevel::WARN));
  assert(ingested._record->_data._description.empty());

  const std::string output = dump_records(collector);
  assert_contains(output,
                  "mid=256 lvl=warn fid=1 eid=0 num=7 state=0 ");
  assert_contains(output, "desc=\"collector timeout\"");

  const auto after_dump = collector.find_data(wire_key);
  assert(after_dump.has_value());
  assert(after_dump->_data._description.empty());
  assert(after_dump->_data._key._level ==
         static_cast<std::uint32_t>(MonitorLevel::WARN));
}

/* V1 字符串仍保持 63 字节内容和 82 字节数据报的旧协议上限。 */
void test_v1_string_limits_remain_compatible() {
  const MonData::MonitorTimestamp changed_at =
      MonData::MonitorTimestamp{std::chrono::seconds{100}};

  MonData::StoredRecord record{
      MonData::MonitorData{
          {MODULE_ID, 1U, 2U, 0U},
          "V1 does not encode this description",
          std::string(63U, 'x')},
      changed_at};

  const Wire::EncodeResult maximum =
      Wire::encode(record, Wire::WireVersion::V1);

  assert(maximum._status == Wire::WireStatus::SUCCESS);
  assert(maximum._bytes.size() == Wire::V1_MAX_DATAGRAM_SIZE);
  assert(maximum._bytes.size() == 82U);

  const Wire::DecodeResult decoded =
      Wire::decode(maximum._bytes.data(), maximum._bytes.size());

  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());
  assert(decoded._record->_version == Wire::WireVersion::V1);
  assert(decoded._record->_data._description.empty());
  assert(!decoded._record->_changed_at.has_value());
  assert(std::get<std::string>(decoded._record->_data._value) ==
         std::string(63U, 'x'));

  record._data._value = std::string(64U, 'y');

  const Wire::EncodeResult too_long =
      Wire::encode(record, Wire::WireVersion::V1);

  assert(too_long._status == Wire::WireStatus::STRING_TOO_LONG);
  assert(too_long._bytes.empty());
}

/*
 * V2 携带完整 Key、description 和 timestamp。Collector Registry 即使配置
 * 了不同 level/description，也不能覆盖数据报内容，CLI 优先显示 V2 描述。
 */
void test_v2_metadata_has_priority_at_collector() {
  Engine producer{MonConfig{"v2-producer", 0U, 1U}};
  Engine collector{MonConfig{"v2-collector", 0U, 2U}};

  initialize_with_module(producer, MonitorLevel::WARN, "producer timeout");
  initialize_with_module(collector, MonitorLevel::EMERGENCY_STOP,
                         "collector timeout");

  const MonData::UpdateResult produced = producer.report_string(
      {MODULE_ID, 99U, 3U, 0U}, std::string(900U, 'v'), {});

  assert(produced._status == MonData::UpdateStatus::INSERTED);
  assert(produced._record.has_value());
  assert(produced._record->_data._key._level ==
         static_cast<std::uint32_t>(MonitorLevel::WARN));
  assert(produced._record->_data._description == "producer timeout");

  const Wire::EncodeResult encoded =
      Wire::encode(*produced._record, Wire::WireVersion::V2);

  assert(encoded._status == Wire::WireStatus::SUCCESS);
  assert(encoded._bytes.size() <= Wire::V2_MAX_DATAGRAM_SIZE);

  Wire::DecodeResult decoded =
      Wire::decode(encoded._bytes.data(), encoded._bytes.size());

  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());
  assert(decoded._record->_version == Wire::WireVersion::V2);
  assert(decoded._record->_data._key == produced._record->_data._key);
  assert(decoded._record->_data._description == "producer timeout");
  assert(decoded._record->_data._value == produced._record->_data._value);
  assert(decoded._record->_changed_at.has_value());
  assert(*decoded._record->_changed_at == produced._record->_changed_at);

  const MonData::MonitorKey wire_key = decoded._record->_data._key;
  const MonData::UpdateResult ingested =
      collector.update_data(std::move(decoded._record->_data));

  assert(ingested._status == MonData::UpdateStatus::INSERTED);
  assert(ingested._record.has_value());
  assert(ingested._record->_data._key._level ==
         static_cast<std::uint32_t>(MonitorLevel::WARN));
  assert(ingested._record->_data._description == "producer timeout");

  const std::string output = dump_records(collector);
  assert_contains(output,
                  "mid=256 lvl=warn fid=3 eid=0 str=\"" +
                      std::string(900U, 'v') + "\" ");
  assert_contains(output, "desc=\"producer timeout\"");
  assert(output.find("desc=\"collector timeout\"") == std::string::npos);

  const auto stored = collector.find_data(wire_key);
  assert(stored.has_value());
  assert(stored->_data._description == "producer timeout");
  assert(stored->_data._key._level ==
         static_cast<std::uint32_t>(MonitorLevel::WARN));
}

/* V2 的 40 字节头部和 1200 字节完整数据报边界保持不变。 */
void test_v2_maximum_datagram_boundary() {
  const MonData::MonitorTimestamp changed_at =
      MonData::MonitorTimestamp{std::chrono::seconds{200} +
                                std::chrono::nanoseconds{123456789}};

  MonData::StoredRecord record{
      MonData::MonitorData{
          {MODULE_ID, 1U, 4U, 0U},
          std::string(100U, 'd'),
          std::string(1060U, 's')},
      changed_at};

  const Wire::EncodeResult maximum =
      Wire::encode(record, Wire::WireVersion::V2);

  assert(Wire::V2_HEADER_SIZE == 40U);
  assert(Wire::V2_MAX_DATAGRAM_SIZE == 1200U);
  assert(maximum._status == Wire::WireStatus::SUCCESS);
  assert(maximum._bytes.size() == Wire::V2_MAX_DATAGRAM_SIZE);

  const Wire::DecodeResult decoded =
      Wire::decode(maximum._bytes.data(), maximum._bytes.size());

  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());
  assert(decoded._record->_data._description == std::string(100U, 'd'));
  assert(std::get<std::string>(decoded._record->_data._value) ==
         std::string(1060U, 's'));
  assert(decoded._record->_changed_at.has_value());
  assert(*decoded._record->_changed_at == changed_at);

  record._data._value = std::string(1061U, 's');

  const Wire::EncodeResult too_large =
      Wire::encode(record, Wire::WireVersion::V2);

  assert(too_large._status == Wire::WireStatus::DATAGRAM_TOO_LARGE);
  assert(too_large._bytes.empty());
}

/*
 * 未知模块的数据仍可接收和保存；一次未知版本解码失败也不能污染统一
 * decoder 的后续调用。
 */
void test_unknown_module_and_version_do_not_break_following_records() {
  Engine collector{MonConfig{"unknown-collector", 0U, 2U}};
  initialize_with_module(collector, MonitorLevel::WARN, "known event");

  const MonData::StoredRecord unknown{
      MonData::MonitorData{
          {UNKNOWN_MODULE_ID, 99U, 5U, 88U},
          "",
          MonData::NumericValue{5U, 0U}},
      MonData::MonitorTimestamp{std::chrono::seconds{300}}};

  const Wire::EncodeResult encoded =
      Wire::encode(unknown, Wire::WireVersion::V1);
  assert(encoded._status == Wire::WireStatus::SUCCESS);

  const std::uint8_t unknown_version[]{99U};
  const Wire::DecodeResult rejected =
      Wire::decode(unknown_version, sizeof(unknown_version));
  assert(rejected._status == Wire::WireStatus::WRONG_VERSION);
  assert(!rejected._record.has_value());

  Wire::DecodeResult decoded =
      Wire::decode(encoded._bytes.data(), encoded._bytes.size());
  assert(decoded._status == Wire::WireStatus::SUCCESS);
  assert(decoded._record.has_value());
  assert(decoded._record->_data._key._level == 99U);

  const MonData::MonitorKey wire_key = decoded._record->_data._key;
  const MonData::UpdateResult ingested =
      collector.update_data(std::move(decoded._record->_data));

  assert(ingested._status == MonData::UpdateStatus::INSERTED);
  assert(ingested._record.has_value());
  assert(ingested._record->_data._key == wire_key);
  assert(ingested._record->_data._description.empty());

  const std::string output = dump_records(collector);
  assert_contains(output,
                  "mid=2457 lvl=? fid=5 eid=88 num=5 state=0 desc=\"\"");
}

} // namespace

int main() {
  test_v1_numeric_producer_to_collector_metadata_flow();
  test_v1_string_limits_remain_compatible();
  test_v2_metadata_has_priority_at_collector();
  test_v2_maximum_datagram_boundary();
  test_unknown_module_and_version_do_not_break_following_records();

  std::cout << "M8_ENGINE_METADATA_WIRE=PASS\n";
  return 0;
}
