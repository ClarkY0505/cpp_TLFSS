#include "cli_commands.h"
#include "cli_registry.h"
#include "engine.h"
#include "monitor_error.h"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

namespace {

using namespace TLSSMON;
using namespace std::chrono_literals;

constexpr std::uint32_t MODULE_ID = 0x100U;

bool contains(const std::string &text, const std::string &expected) {
  return text.find(expected) != std::string::npos;
}

class TestPipe final {
public:
  TestPipe() {
    int descriptors[2]{-1, -1};
    assert(::pipe(descriptors) == 0);
    _read_fd = descriptors[0];
    _write_fd = descriptors[1];
  }

  ~TestPipe() {
    assert(::close(_read_fd) == 0);
    assert(::close(_write_fd) == 0);
  }

  TestPipe(const TestPipe &) = delete;
  TestPipe &operator=(const TestPipe &) = delete;

  int read_fd() const noexcept { return _read_fd; }

  void signal() const {
    const char byte = '*';
    assert(::write(_write_fd, &byte, sizeof(byte)) ==
           static_cast<ssize_t>(sizeof(byte)));
  }

  void drain_one() const {
    char byte{};
    assert(::read(_read_fd, &byte, sizeof(byte)) ==
           static_cast<ssize_t>(sizeof(byte)));
  }

private:
  int _read_fd{-1};
  int _write_fd{-1};
};

class Rendezvous final {
public:
  explicit Rendezvous(std::size_t target) : _target(target) {
    assert(_target > 0U);
  }

  bool arrive_and_wait_for(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(_mutex);
    ++_arrived;
    _condition.notify_all();

    return _condition.wait_for(lock, timeout,
                               [&] { return _arrived >= _target; });
  }

private:
  const std::size_t _target;
  std::mutex _mutex;
  std::condition_variable _condition;
  std::size_t _arrived{0U};
};

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

void initialize_engine(Engine &engine) {
  assert(engine.init() == ENGINESTATE::SUCCESSFUL);
  assert(engine.register_module(
             {MODULE_ID,
              "module-a",
              "metadata service",
              {{MonitorLevel::INFO, "heartbeat"},
               {MonitorLevel::WARN, "metadata timeout"},
               {MonitorLevel::SOFT_STOP, "worker state"},
               {MonitorLevel::EMERGENCY_STOP, "reentrant error"}}}) ==
         ModuleRegisterStatus::SUCCESS);
}

/* 多线程 find_error() 必须始终返回独立、完整且可修改的副本。 */
void test_concurrent_find_error_returns_stable_copies() {
  Engine engine{MonConfig{"m8-find-error-concurrency", 0U, 1U}};
  initialize_engine(engine);

  constexpr std::size_t thread_count = 8U;
  constexpr std::size_t queries_per_thread = 2000U;
  std::atomic<bool> start{false};
  std::atomic<std::size_t> failures{0U};
  std::vector<std::thread> readers;
  readers.reserve(thread_count);

  for (std::size_t thread = 0U; thread < thread_count; ++thread) {
    readers.emplace_back([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }

      for (std::size_t index = 0U; index < queries_per_thread; ++index) {
        auto error = engine.find_error(MODULE_ID, 1U);
        const auto missing = engine.find_error(MODULE_ID, 99U);

        if (!error.has_value() || error->_level != MonitorLevel::WARN ||
            error->_description != "metadata timeout" || missing.has_value()) {
          failures.fetch_add(1U, std::memory_order_relaxed);
          continue;
        }

        error->_description = "changed snapshot";
      }
    });
  }

  start.store(true, std::memory_order_release);
  for (std::thread &reader : readers) {
    reader.join();
  }

  assert(failures.load(std::memory_order_relaxed) == 0U);
  const auto stored = engine.find_error(MODULE_ID, 1U);
  assert(stored.has_value());
  assert(stored->_description == "metadata timeout");
}

/*
 * Publisher 在 Store 和 Publisher mutex 之外运行，因此可以查询 Engine 的
 * 模块/错误表，并重新进入 Reporter 上报另一个 EID。
 */
void test_publisher_can_query_metadata_and_reenter_reporter() {
  Engine engine{MonConfig{"m8-publisher-reentry", 0U, 1U}};
  initialize_engine(engine);

  std::atomic<std::size_t> published{0U};
  std::atomic<std::size_t> reentered{0U};

  assert(engine.set_publisher([&](MonData::StoredRecord record) {
    const auto module = engine.find_module_by_id(record._data._key._mid);
    const auto error = engine.find_error(record._data._key._mid,
                                         record._data._key._eid);
    assert(module.has_value());
    assert(error.has_value());
    assert(record._data._key._level ==
           static_cast<std::uint32_t>(error->_level));
    assert(record._data._description == error->_description);
    published.fetch_add(1U, std::memory_order_relaxed);

    if (record._data._key._eid == 0U) {
      const auto nested =
          engine.report_error({MODULE_ID, 99U, 9U, 3U}, 0U);
      assert(nested._status == MonData::UpdateStatus::INSERTED);
      reentered.fetch_add(1U, std::memory_order_relaxed);
    }
  }));

  const auto outer = engine.report_count({MODULE_ID, 99U, 9U, 0U}, 1U);
  assert(outer._status == MonData::UpdateStatus::INSERTED);
  assert(published.load(std::memory_order_relaxed) == 2U);
  assert(reentered.load(std::memory_order_relaxed) == 1U);
  assert(engine.query_data().size() == 2U);
}

/*
 * 一个异步 Timer 和两个异步 AIO worker 同时调用 report_count/error/string。
 * 三者在 Rendezvous 汇合，证明回调实际并发存活；最后完成者负责停止 Engine。
 */
void test_timer_and_aio_concurrently_report_all_value_types() {
  Engine engine{MonConfig{"m8-timer-aio-concurrency", 0U, 1U}};
  initialize_engine(engine);

  std::array<TestPipe, 2U> inputs;
  Rendezvous rendezvous{3U};
  std::atomic<bool> timer_claimed{false};
  std::array<std::atomic<bool>, 2U> aio_claimed{};
  std::atomic<std::size_t> rendezvous_success{0U};
  std::atomic<std::size_t> completed{0U};
  std::atomic<std::size_t> changed{0U};
  std::atomic<std::size_t> published{0U};

  assert(engine.set_publisher([&](MonData::StoredRecord record) {
    const auto error = engine.find_error(record._data._key._mid,
                                         record._data._key._eid);
    assert(error.has_value());
    assert(record._data._description == error->_description);
    published.fetch_add(1U, std::memory_order_relaxed);
  }));

  const auto finish = [&](MonData::UpdateResult result) {
    if (result.changed()) {
      changed.fetch_add(1U, std::memory_order_relaxed);
    }

    if (completed.fetch_add(1U, std::memory_order_acq_rel) + 1U == 3U) {
      engine.stop();
    }
  };

  const auto timer = engine.set_timer(
      MonCallback{
          "m8-concurrent-timer",
          [&]() -> int {
            if (timer_claimed.exchange(true, std::memory_order_acq_rel)) {
              return 0;
            }

            if (rendezvous.arrive_and_wait_for(2s)) {
              rendezvous_success.fetch_add(1U, std::memory_order_relaxed);
            }
            finish(engine.report_count({MODULE_ID, 99U, 10U, 0U}, 1U));
            return 0;
          },
          true},
      TimerFlags::RECURRING | TimerFlags::WORKER, 10ms);
  assert(timer.has_value());

  for (std::size_t index = 0U; index < inputs.size(); ++index) {
    const auto aio = engine.add_aio(
        inputs[index].read_fd(),
        MonCallback{
            "m8-concurrent-aio",
            [&, index]() -> int {
              if (aio_claimed[index].exchange(true,
                                               std::memory_order_acq_rel)) {
                return 0;
              }

              inputs[index].drain_one();
              if (rendezvous.arrive_and_wait_for(2s)) {
                rendezvous_success.fetch_add(1U, std::memory_order_relaxed);
              }

              if (index == 0U) {
                finish(engine.report_error({MODULE_ID, 99U, 11U, 1U}, 0U));
              } else {
                finish(engine.report_string({MODULE_ID, 99U, 12U, 2U},
                                            "aio-running"));
              }
              return 0;
            },
            true});
    assert(aio.has_value());
  }

  std::promise<ENGINESTATE> result_promise;
  std::future<ENGINESTATE> result = result_promise.get_future();
  std::thread runner([&] { result_promise.set_value(engine.run()); });

  assert(wait_for_phase(engine, EnginePhase::RUNNING, 2s));
  for (const TestPipe &input : inputs) {
    input.signal();
  }

  const bool finished = result.wait_for(5s) == std::future_status::ready;
  if (!finished) {
    engine.stop();
  }
  runner.join();

  assert(finished);
  assert(result.get() == ENGINESTATE::SUCCESSFUL);
  assert(rendezvous_success.load(std::memory_order_relaxed) == 3U);
  assert(completed.load(std::memory_order_relaxed) == 3U);
  assert(changed.load(std::memory_order_relaxed) == 3U);
  assert(published.load(std::memory_order_relaxed) == 3U);

  const auto records = engine.query_data();
  assert(records.size() == 3U);
  assert(records[0]._data._key._level ==
         static_cast<std::uint32_t>(MonitorLevel::INFO));
  assert(records[1]._data._key._level ==
         static_cast<std::uint32_t>(MonitorLevel::WARN));
  assert(records[2]._data._key._level ==
         static_cast<std::uint32_t>(MonitorLevel::SOFT_STOP));
  assert(std::get<std::string>(records[2]._data._value) == "aio-running");
}

/* CLI 快照查询和 report 更新并发时不得出现空引用、损坏输出或数据竞争。 */
void test_cli_db_dump_and_report_are_concurrent_safe() {
  Engine engine{MonConfig{"m8-cli-report-concurrency", 0U, 1U}};
  initialize_engine(engine);

  CliRegistry cli;
  assert(register_db_dump_command(cli, engine) == CliRegisterStatus::SUCCESS);

  constexpr std::uint32_t updates = 1000U;
  std::atomic<bool> start{false};
  std::atomic<std::size_t> failures{0U};

  std::thread writer([&] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    for (std::uint32_t value = 1U; value <= updates; ++value) {
      const auto result =
          engine.report_count({MODULE_ID, 99U, 20U, 0U}, value);
      if (!result.changed()) {
        failures.fetch_add(1U, std::memory_order_relaxed);
      }
    }
  });

  std::thread reader([&] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    for (std::uint32_t index = 0U; index < updates; ++index) {
      const CliDispatchResult result = cli.dispatch("db_dump");
      if (result._status != CliDispatchStatus::SUCCESS ||
          (result._output != "0 entry\n" &&
           (!contains(result._output, "lvl=info") ||
            !contains(result._output, "desc=\"heartbeat\"") ||
            !contains(result._output, "1 entry\n")))) {
        failures.fetch_add(1U, std::memory_order_relaxed);
      }
    }
  });

  start.store(true, std::memory_order_release);
  writer.join();
  reader.join();

  assert(failures.load(std::memory_order_relaxed) == 0U);
  const auto records = engine.query_data();
  assert(records.size() == 1U);
  assert(std::get<MonData::NumericValue>(records[0]._data._value)._value ==
         updates);
}

} // namespace

int main() {
  test_concurrent_find_error_returns_stable_copies();
  test_publisher_can_query_metadata_and_reenter_reporter();
  test_timer_and_aio_concurrently_report_all_value_types();
  test_cli_db_dump_and_report_are_concurrent_safe();

  std::cout << "M8_CONCURRENCY=PASS\n";
  return 0;
}
