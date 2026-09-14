#include "cli_commands.h"
#include "engine.h"
#include "monitor_data.h"
#include "monitor_error.h"
#include "monitor_module.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

using namespace TLSSMON;

constexpr std::uint32_t MODULE_ID = 0x100U;

void initialize_with_module(Engine &engine) {
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);
  assert(engine.get_phase() == EnginePhase::READY);

  assert(engine.register_module(
             {MODULE_ID,
              "module-a",
              "metadata service",
              {{MonitorLevel::INFO, "heartbeat"},
               {MonitorLevel::WARN, "metadata timeout"},
               {MonitorLevel::SOFT_STOP, ""},
               {MonitorLevel::EMERGENCY_STOP,
                "line\n\"quoted\"\\tail"}}}) ==
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

void assert_not_contains(const std::string &output,
                         const std::string &unexpected) {
  assert(output.find(unexpected) == std::string::npos);
}

/*
 * 本地 report_*() 已经由 Reporter 写入完整 description；CLI 应直接使用
 * Store 内容，不需要依赖显示阶段再次补全。
 */
void test_local_report_description_is_displayed() {
  Engine engine{MonConfig{"cli-local-description", 0U, 1U}};
  initialize_with_module(engine);

  const MonData::UpdateResult update = engine.report_count(
      {MODULE_ID, 99U, 1U, 1U}, 7U, "caller description");

  assert(update._status == MonData::UpdateStatus::INSERTED);
  assert(update._record.has_value());
  assert(update._record->_data._description == "metadata timeout");

  const std::string output = dump_records(engine);
  assert_contains(output,
                  "mid=256 lvl=warn fid=1 eid=1 num=7 state=0 ");
  assert_contains(output, "desc=\"metadata timeout\"");
  assert_not_contains(output, "desc=\"caller description\"");
}

/*
 * V1 风格记录没有 description。CLI 可以从 Registry 回填显示文本，但
 * 不能写回 Store、更新时间戳或因 db_dump 再次调用 Publisher。
 */
void test_v1_empty_description_is_filled_without_side_effects() {
  Engine engine{MonConfig{"cli-v1-description", 0U, 1U}};
  initialize_with_module(engine);

  std::size_t publish_count = 0U;
  assert(engine.set_publisher([&](MonData::StoredRecord) {
    ++publish_count;
  }));

  const MonData::MonitorKey key{
      MODULE_ID,
      static_cast<std::uint32_t>(MonitorLevel::WARN),
      2U,
      1U};

  const MonData::UpdateResult update = engine.update_data(
      MonData::MonitorData{key, "", MonData::NumericValue{9U, 0U}});

  assert(update._status == MonData::UpdateStatus::INSERTED);
  assert(update._record.has_value());
  assert(update._record->_data._description.empty());
  assert(publish_count == 1U);

  const auto before = engine.find_data(key);
  assert(before.has_value());
  assert(before->_data._description.empty());

  const std::size_t publishes_before_dump = publish_count;
  const std::string output = dump_records(engine);

  assert_contains(output,
                  "mid=256 lvl=warn fid=2 eid=1 num=9 state=0 ");
  assert_contains(output, "desc=\"metadata timeout\"");

  const auto after = engine.find_data(key);
  assert(after.has_value());
  assert(after->_data._description.empty());
  assert(after->_changed_at == before->_changed_at);
  assert(publish_count == publishes_before_dump);
}

/*
 * V2 非空 description 是发送端提供的数据，优先级高于接收端 Registry，
 * CLI 不能用本地错误描述覆盖它。
 */
void test_v2_nonempty_description_has_priority() {
  Engine engine{MonConfig{"cli-v2-description", 0U, 1U}};
  initialize_with_module(engine);

  const MonData::MonitorKey key{
      MODULE_ID,
      static_cast<std::uint32_t>(MonitorLevel::WARN),
      3U,
      1U};

  const MonData::UpdateResult update = engine.update_data(
      MonData::MonitorData{
          key, "remote node description", MonData::NumericValue{11U, 0U}});

  assert(update._status == MonData::UpdateStatus::INSERTED);

  const std::string output = dump_records(engine);
  assert_contains(output, "desc=\"remote node description\"");
  assert_not_contains(output, "desc=\"metadata timeout\"");

  const auto stored = engine.find_data(key);
  assert(stored.has_value());
  assert(stored->_data._description == "remote node description");
}

/*
 * V2 允许 description 长度为零。字符串记录也必须采用同一回填规则，
 * 不能只为 NumericValue 实现回填。
 */
void test_v2_empty_description_is_filled_for_string_record() {
  Engine engine{MonConfig{"cli-v2-empty-description", 0U, 1U}};
  initialize_with_module(engine);

  const MonData::MonitorKey key{
      MODULE_ID,
      static_cast<std::uint32_t>(MonitorLevel::WARN),
      4U,
      1U};

  const MonData::UpdateResult update = engine.update_data(
      MonData::MonitorData{key, "", std::string{"running"}});

  assert(update._status == MonData::UpdateStatus::INSERTED);

  const std::string output = dump_records(engine);
  assert_contains(output,
                  "mid=256 lvl=warn fid=4 eid=1 str=\"running\" ");
  assert_contains(output, "desc=\"metadata timeout\"");

  const auto stored = engine.find_data(key);
  assert(stored.has_value());
  assert(stored->_data._description.empty());
}

/*
 * 未知 MID 和越界 EID 都没有可用于回填的元数据；db_dump 必须成功并
 * 保持 desc=""，不能把一次查询失败当作命令错误。
 */
void test_unknown_mid_and_eid_keep_empty_description() {
  Engine engine{MonConfig{"cli-unknown-description", 0U, 1U}};
  initialize_with_module(engine);

  assert(engine.update_data(
                   MonData::MonitorData{
                       {0x999U, 99U, 5U, 0U},
                       "",
                       MonData::NumericValue{1U, 0U}})
             .changed());

  assert(engine.update_data(
                   MonData::MonitorData{
                       {MODULE_ID, 99U, 6U, 99U},
                       "",
                       MonData::NumericValue{2U, 0U}})
             .changed());

  const std::string output = dump_records(engine);

  assert_contains(output,
                  "mid=256 lvl=? fid=6 eid=99 num=2 state=0 desc=\"\"");
  assert_contains(output,
                  "mid=2457 lvl=? fid=5 eid=0 num=1 state=0 desc=\"\"");
  assert_contains(output, "2 entries\n");
}

/* Registry 回填后的 description 仍必须经过既有 CLI 转义规则。 */
void test_registry_description_is_escaped() {
  Engine engine{MonConfig{"cli-escaped-description", 0U, 1U}};
  initialize_with_module(engine);

  assert(engine.update_data(
                   MonData::MonitorData{
                       {MODULE_ID,
                        static_cast<std::uint32_t>(
                            MonitorLevel::EMERGENCY_STOP),
                        7U,
                        3U},
                       "",
                       MonData::NumericValue{3U, 0U}})
             .changed());

  const std::string output = dump_records(engine);
  assert_contains(output, "desc=\"line\\n\\\"quoted\\\"\\\\tail\"");
}

} // namespace

int main() {
  test_local_report_description_is_displayed();
  test_v1_empty_description_is_filled_without_side_effects();
  test_v2_nonempty_description_has_priority();
  test_v2_empty_description_is_filled_for_string_record();
  test_unknown_mid_and_eid_keep_empty_description();
  test_registry_description_is_escaped();

  std::cout << "M8_CLI_ERROR_METADATA=PASS\n";
  return 0;
}
