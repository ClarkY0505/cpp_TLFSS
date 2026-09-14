#include "engine.h"
#include "monitor_error.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <string>
#include <thread>

namespace {

using namespace TLSSMON;
using namespace std::chrono_literals;

constexpr std::uint32_t MODULE_ID = 0x100U;

bool wait_for_phase(Engine &engine, EnginePhase expected,
                    std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (std::chrono::steady_clock::now() < deadline) {
    if (engine.get_phase() == expected) {
      return true;
    }

    std::this_thread::yield();
  }

  return engine.get_phase() == expected;
}

MonitorModuleInfo standard_module() {
  return MonitorModuleInfo{
      MODULE_ID,
      "module-a",
      "metadata service",
      {{MonitorLevel::INFO, "heartbeat"},
       {MonitorLevel::WARN, "metadata timeout"}}};
}

/* 模块只能在 READY 注册；CREATED 阶段必须明确拒绝。 */
void test_module_registration_requires_ready_phase() {
  Engine engine{MonConfig{"m8-module-phase", 0U, 1U}};

  assert(engine.get_phase() == EnginePhase::CREATED);
  assert(engine.register_module(standard_module()) ==
         ModuleRegisterStatus::INVALID_PHASE);
  assert(engine.modules().empty());

  assert(engine.init() == ENGINESTATE::SUCCESSFUL);
  assert(engine.get_phase() == EnginePhase::READY);
  assert(engine.register_module(standard_module()) ==
         ModuleRegisterStatus::SUCCESS);
}

/* Engine 的所有模块查询都返回独立副本，并读取同一份错误表。 */
void test_engine_module_queries_return_value_snapshots() {
  Engine engine{MonConfig{"m8-module-snapshot", 0U, 1U}};
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);
  assert(engine.register_module(standard_module()) ==
         ModuleRegisterStatus::SUCCESS);

  auto by_id = engine.find_module_by_id(MODULE_ID);
  auto by_name = engine.find_module_by_name("module-a");
  auto error = engine.find_error(MODULE_ID, 1U);
  auto modules = engine.modules();

  assert(by_id.has_value());
  assert(by_name.has_value());
  assert(*by_id == *by_name);
  assert(error.has_value());
  assert(error->_level == MonitorLevel::WARN);
  assert(error->_description == "metadata timeout");
  assert(modules.size() == 1U);
  assert(modules[0] == *by_id);

  by_id->_name = "changed-copy";
  by_id->_errors[1U]._description = "changed-copy";
  modules[0]._errors.clear();

  const auto stored = engine.find_module_by_id(MODULE_ID);
  assert(stored.has_value());
  assert(stored->_name == "module-a");
  assert(stored->_errors.size() == 2U);
  assert(stored->_errors[1U]._description == "metadata timeout");
}

/* 非法错误级别必须拒绝整个模块，不能留下 mid 或 name 索引。 */
void test_invalid_error_level_does_not_partially_register_module() {
  Engine engine{MonConfig{"m8-invalid-level", 0U, 1U}};
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);

  MonitorModuleInfo invalid{
      0x200U,
      "invalid-module",
      "invalid metadata",
      {{static_cast<MonitorLevel>(99U), "invalid level"}}};

  assert(engine.register_module(std::move(invalid)) ==
         ModuleRegisterStatus::INVALID_ERROR_LEVEL);
  assert(!engine.find_module_by_id(0x200U).has_value());
  assert(!engine.find_module_by_name("invalid-module").has_value());
  assert(!engine.find_error(0x200U, 0U).has_value());
  assert(engine.modules().empty());
}

/*
 * run() 开始后模块表冻结；STOPPING/STOPPED 拒绝上报，但停止后模块、错误表
 * 和最后一条 Store 记录仍然可以读取。
 */
void test_running_freezes_modules_and_stopped_keeps_snapshots() {
  Engine engine{MonConfig{"m8-module-lifecycle", 0U, 1U}};
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);
  assert(engine.register_module(standard_module()) ==
         ModuleRegisterStatus::SUCCESS);

  const MonData::MonitorKey supplied{MODULE_ID, 99U, 7U, 0U};
  const MonData::MonitorKey normalized{
      MODULE_ID, static_cast<std::uint32_t>(MonitorLevel::INFO), 7U, 0U};

  const MonData::UpdateResult inserted =
      engine.report_count(supplied, 1U);
  assert(inserted._status == MonData::UpdateStatus::INSERTED);

  std::promise<ENGINESTATE> result_promise;
  std::future<ENGINESTATE> result = result_promise.get_future();
  std::thread runner([&] { result_promise.set_value(engine.run()); });

  assert(wait_for_phase(engine, EnginePhase::RUNNING, 2s));
  assert(engine.register_module(
             {0x300U, "late-module", "must be rejected", {}}) ==
         ModuleRegisterStatus::INVALID_PHASE);

  engine.stop();

  const MonData::UpdateResult rejected =
      engine.report_count(supplied, 2U);
  assert(rejected._status == MonData::UpdateStatus::INVALID);

  runner.join();
  assert(result.get() == ENGINESTATE::SUCCESSFUL);
  assert(engine.get_phase() == EnginePhase::STOPPED);
  assert(engine.register_module(
             {0x400U, "stopped-module", "must be rejected", {}}) ==
         ModuleRegisterStatus::INVALID_PHASE);

  assert(engine.find_module_by_id(MODULE_ID).has_value());
  assert(engine.find_module_by_name("module-a").has_value());
  assert(engine.find_error(MODULE_ID, 0U).has_value());
  assert(engine.modules().size() == 1U);

  const auto stored = engine.find_data(normalized);
  assert(stored.has_value());
  assert(stored->_data._description == "heartbeat");
}

/*
 * Reporter 持有 Registry 非拥有引用。反复构造、发布、查询和析构 Engine，
 * 由 ASan/UBSan 验证 MonContext 先于 Engine::_modules 销毁。
 */
void test_engine_destruction_keeps_reporter_registry_reference_valid() {
  constexpr std::size_t iterations = 100U;
  std::atomic<std::size_t> published{0U};

  for (std::size_t index = 0U; index < iterations; ++index) {
    Engine engine{MonConfig{"m8-destruction", 0U, 1U}};
    assert(engine.init() == ENGINESTATE::SUCCESSFUL);
    assert(engine.register_module(standard_module()) ==
           ModuleRegisterStatus::SUCCESS);

    assert(engine.set_publisher([&](MonData::StoredRecord record) {
      assert(record._data._description == "heartbeat");
      assert(engine.find_error(MODULE_ID, 0U).has_value());
      published.fetch_add(1U, std::memory_order_relaxed);
    }));

    const auto result = engine.report_count(
        {MODULE_ID, 99U, static_cast<std::uint32_t>(index), 0U}, 1U);
    assert(result._status == MonData::UpdateStatus::INSERTED);
  }

  assert(published.load(std::memory_order_relaxed) == iterations);
}

} // namespace

int main() {
  test_module_registration_requires_ready_phase();
  test_engine_module_queries_return_value_snapshots();
  test_invalid_error_level_does_not_partially_register_module();
  test_running_freezes_modules_and_stopped_keeps_snapshots();
  test_engine_destruction_keeps_reporter_registry_reference_valid();

  std::cout << "M8_ENGINE_MODULE_LIFECYCLE=PASS\n";
  return 0;
}
