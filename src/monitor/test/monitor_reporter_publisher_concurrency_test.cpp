#include "monitor_reporter.h"
#include "monitor_store.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
using namespace TLSSMON;

MonData::MonitorTimestamp timestamp_at(std::int64_t seconds) {

  return MonData::MonitorTimestamp{std::chrono::seconds{seconds}};
}

/*
 * 案例 1：没有 Publisher 时仍然更新 Store。
 *
 * 测试目的：
 * 验证 Publisher 是可选的输出端，而不是写入 Store 的前置条件。
 * Reporter 在未注册 Publisher 时仍必须执行去重、保存记录并返回正常的
 * UpdateResult。
 *
 * 执行过程：
 * 1. 创建 Store 和 Reporter，但不调用 set_publisher()；
 * 2. 通过 report_count() 写入一个首次出现的非零值；
 * 3. 检查返回状态、返回记录和 Store 快照。
 *
 * 通过标准：
 * 返回 INSERTED，UpdateResult 携带记录，Store 中存在对应 Key，并且 Store
 * 只有一个节点。该案例可以发现“没有 Publisher 就拒绝保存”的错误实现。
 */
void test_update_without_publisher_still_updates_store() {
  MonitorStore store;
  MonitorReporter reporter(store);

  const MonData::MonitorKey key{1, 1, 1, 1};

  /*
   * 不调用 set_publisher()。
   */
  const MonData::UpdateResult result =
      reporter.report_count(key, 10, "without-publisher", timestamp_at(100));

  assert(result._status == MonData::UpdateStatus::INSERTED);

  assert(result.changed());
  assert(result._record.has_value());

  const auto stored = store.find(key);

  assert(stored.has_value());
  assert(stored->_data._key == key);
  assert(store.size() == 1);
}

/*
 * 案例 2：空 Publisher 注销当前 Publisher。
 *
 * 测试目的：
 * 验证 set_publisher({}) 的语义是安全注销，而不是无效参数或 Store 停写。
 * 注销只影响之后的发布行为，不能影响记录更新。
 *
 * 执行过程：
 * 1. 注册一个只增加原子计数器的 Publisher；
 * 2. 首次写入并确认 Publisher 被调用一次；
 * 3. 传入空 std::function 注销 Publisher；
 * 4. 修改同一 Key 的数值；
 * 5. 检查 Store 已更新，但发布次数没有增加。
 *
 * 通过标准：
 * 第二次写入返回 UPDATED，发布次数仍为 1，Store 中仍只有该 Key 的最新
 * 记录。
 */
void test_empty_publisher_unregisters_current_publisher() {
  MonitorStore store;
  MonitorReporter reporter(store);

  std::atomic<std::size_t> publish_count{0};

  reporter.set_publisher([&](MonData::StoredRecord) {
    publish_count.fetch_add(1, std::memory_order_relaxed);
  });

  const MonData::MonitorKey key{2, 1, 1, 1};

  const MonData::UpdateResult inserted =
      reporter.report_count(key, 1, "first", timestamp_at(200));

  assert(inserted._status == MonData::UpdateStatus::INSERTED);

  assert(publish_count.load(std::memory_order_relaxed) == 1);

  /*
   * 空 std::function 表示注销。
   */
  reporter.set_publisher({});

  const MonData::UpdateResult updated =
      reporter.report_count(key, 2, "updated", timestamp_at(300));

  assert(updated._status == MonData::UpdateStatus::UPDATED);

  /*
   * Store 已更新，但 Publisher 没有再次执行。
   */
  assert(publish_count.load(std::memory_order_relaxed) == 1);

  const auto stored = store.find(key);

  assert(stored.has_value());
  assert(store.size() == 1);
}

/*
 * 案例 3：替换 Publisher 只影响后续变化。
 *
 * 测试目的：
 * 验证 set_publisher() 是替换语义。Publisher B 注册后，已经完成的发布仍
 * 计入 A，之后发生的记录变化只交给 B。
 *
 * 执行过程：
 * 1. 注册 Publisher A 并插入记录；
 * 2. 将 Publisher 替换为 B；
 * 3. 连续两次修改同一 Key 的数值；
 * 4. 分别检查 A、B 的调用次数。
 *
 * 通过标准：
 * A 只处理首次 INSERTED，因此调用 1 次；B 处理后续两个 UPDATED，因此
 * 调用 2 次。该案例可以发现 Publisher 被错误追加、旧 Publisher 未释放或
 * 替换后仍被调用的问题。
 */
void test_replacing_publisher_affects_future_updates() {
  MonitorStore store;
  MonitorReporter reporter(store);

  std::atomic<std::size_t> publisher_a_count{0};
  std::atomic<std::size_t> publisher_b_count{0};

  reporter.set_publisher([&](MonData::StoredRecord) {
    publisher_a_count.fetch_add(1, std::memory_order_relaxed);
  });

  const MonData::MonitorKey key{3, 1, 1, 1};

  const auto first =
      reporter.report_count(key, 1, "publisher-a", timestamp_at(400));

  assert(first._status == MonData::UpdateStatus::INSERTED);

  reporter.set_publisher([&](MonData::StoredRecord) {
    publisher_b_count.fetch_add(1, std::memory_order_relaxed);
  });

  const auto second =
      reporter.report_count(key, 2, "publisher-b-first", timestamp_at(500));

  assert(second._status == MonData::UpdateStatus::UPDATED);

  const auto third =
      reporter.report_count(key, 3, "publisher-b-second", timestamp_at(600));

  assert(third._status == MonData::UpdateStatus::UPDATED);

  assert(publisher_a_count.load(std::memory_order_relaxed) == 1);

  assert(publisher_b_count.load(std::memory_order_relaxed) == 2);
}

/*
 * 案例 4：正在执行的更新使用 Publisher 快照。
 *
 * 测试目的：
 * 同时验证两个关键锁边界：
 * 1. update() 在锁内复制当前 Publisher，当前调用固定使用该副本；
 * 2. 复制完成后必须释放 _publisher_mutex，再执行用户 Publisher。
 *
 * 线程时序：
 * 1. updater 调用 report_count()，进入旧 Publisher 后设置
 *    old_publisher_entered=true，并等待 release_old_publisher；
 * 2. 主线程收到通知，确认旧 Publisher 正在执行；
 * 3. setter 在线程中调用 set_publisher() 替换成新 Publisher；
 * 4. 主线程等待 setter_completed。如果旧 Publisher 执行期间仍持有
 *    _publisher_mutex，setter 会被阻塞，这一步会超时；
 * 5. 主线程设置 release_old_publisher=true，唤醒旧 Publisher，确保失败
 *    路径也不会永久挂住；
 * 6. 再次更新同一 Key，验证后续变化由新 Publisher 处理。
 *
 * 通过标准：
 * setter 必须在旧 Publisher 被释放前完成；第一次发布只调用旧 Publisher，
 * 第二次发布只调用新 Publisher。该案例可以发现持锁调用用户代码和未使用
 * Publisher 快照两类问题。
 */
void test_inflight_update_uses_publisher_snapshot() {
  MonitorStore store;
  MonitorReporter reporter(store);

  std::mutex state_mutex;
  std::condition_variable state_condition;

  bool old_publisher_entered = false;
  bool release_old_publisher = false;
  bool setter_completed = false;

  std::atomic<std::size_t> old_count{0};
  std::atomic<std::size_t> new_count{0};

  reporter.set_publisher([&](MonData::StoredRecord) {
    old_count.fetch_add(1, std::memory_order_relaxed);

    std::unique_lock<std::mutex> lock(state_mutex);

    old_publisher_entered = true;
    state_condition.notify_all();

    state_condition.wait(lock, [&] { return release_old_publisher; });
  });

  const MonData::MonitorKey key{4, 1, 1, 1};

  std::thread updater([&] {
    const auto result =
        reporter.report_count(key, 1, "old-publisher", timestamp_at(700));

    assert(result._status == MonData::UpdateStatus::INSERTED);
  });

  /*
   * 等待旧 Publisher 确实开始执行。
   */
  {
    std::unique_lock<std::mutex> lock(state_mutex);

    const bool entered = state_condition.wait_for(
        lock, std::chrono::seconds{2}, [&] { return old_publisher_entered; });

    assert(entered);
  }

  /*
   * 旧 Publisher 还没有返回时替换 Publisher。
   */
  std::thread setter([&] {
    reporter.set_publisher([&](MonData::StoredRecord) {
      new_count.fetch_add(1, std::memory_order_relaxed);
    });

    {
      std::lock_guard<std::mutex> lock(state_mutex);

      setter_completed = true;
    }

    state_condition.notify_all();
  });

  bool setter_completed_before_release = false;

  {
    std::unique_lock<std::mutex> lock(state_mutex);

    setter_completed_before_release = state_condition.wait_for(
        lock, std::chrono::seconds{2}, [&] { return setter_completed; });

    /*
     * 无论测试是否成功，都释放旧 Publisher，
     * 避免失败路径永久死锁。
     */
    release_old_publisher = true;
  }

  state_condition.notify_all();

  updater.join();
  setter.join();

  /*
   * set_publisher() 必须能在旧 Publisher
   * 尚未返回时完成。
   */
  assert(setter_completed_before_release);

  assert(old_count.load(std::memory_order_relaxed) == 1);

  assert(new_count.load(std::memory_order_relaxed) == 0);

  /*
   * 后续变化应使用新 Publisher。
   */
  const auto updated =
      reporter.report_count(key, 2, "new-publisher", timestamp_at(800));

  assert(updated._status == MonData::UpdateStatus::UPDATED);

  assert(old_count.load(std::memory_order_relaxed) == 1);

  assert(new_count.load(std::memory_order_relaxed) == 1);
}

/*
 * 案例 5：Publisher 可以注销自身。
 *
 * 测试目的：
 * 验证 Publisher 执行期间可以重新进入 set_publisher()，证明 Reporter 没有
 * 持有 _publisher_mutex 调用用户回调。
 *
 * 执行过程：
 * 1. 注册一个 Publisher，它首次执行时增加计数并调用
 *    set_publisher({}) 注销自身；
 * 2. 第一次写入触发该 Publisher；
 * 3. 第二次修改同一 Key；
 * 4. 检查第二次更新没有再次调用 Publisher。
 *
 * 通过标准：
 * 两次 Store 写入分别返回 INSERTED、UPDATED，但 Publisher 总调用次数为 1。
 * 如果持锁执行 Publisher，本案例会发生自死锁，应由测试程序外层 timeout
 * 捕获。
 */
void test_publisher_can_unregister_itself() {
  MonitorStore store;
  MonitorReporter reporter(store);

  std::atomic<std::size_t> publish_count{0};

  reporter.set_publisher([&](MonData::StoredRecord) {
    publish_count.fetch_add(1, std::memory_order_relaxed);

    /*
     * Publisher 内重新进入 set_publisher()。
     */
    reporter.set_publisher({});
  });

  const MonData::MonitorKey key{5, 1, 1, 1};

  const auto inserted =
      reporter.report_count(key, 1, "self-unregister", timestamp_at(900));

  assert(inserted._status == MonData::UpdateStatus::INSERTED);

  const auto updated =
      reporter.report_count(key, 2, "after-unregister", timestamp_at(1000));

  assert(updated._status == MonData::UpdateStatus::UPDATED);

  assert(publish_count.load(std::memory_order_relaxed) == 1);

  assert(store.size() == 1);
}

/*
 * 案例 6：Publisher 可以重新进入 Reporter 上报接口。
 *
 * 测试目的：
 * 验证用户 Publisher 可以在执行期间再次调用 report_count()。这模拟 Timer
 * 或 AIO 回调在一次发布中派生另一条监控记录的场景。
 *
 * 执行过程：
 * 1. 准备 outer_key 和 nested_key；
 * 2. 注册 Publisher，首次进入时对 nested_key 再次调用 report_count()；
 * 3. 使用 nested_started 防止嵌套发布无限递归；
 * 4. 对 outer_key 发起外层写入；
 * 5. 检查两次发布和两个 Store 节点。
 *
 * 通过标准：
 * 外层和嵌套写入都返回 INSERTED，Publisher 共调用 2 次，Store 中存在两个
 * Key。若调用 Publisher 时持有 _publisher_mutex，嵌套写入复制 Publisher
 * 时会死锁。
 */
void test_publisher_can_reenter_reporter() {
  MonitorStore store;
  MonitorReporter reporter(store);

  const MonData::MonitorKey outer_key{6, 1, 1, 1};

  const MonData::MonitorKey nested_key{6, 1, 1, 2};

  bool nested_started = false;
  std::size_t publish_count = 0;

  reporter.set_publisher([&](MonData::StoredRecord) {
    ++publish_count;

    /*
     * 防止嵌套发布再次无限递归。
     */
    if (!nested_started) {
      nested_started = true;

      const auto nested =
          reporter.report_count(nested_key, 20, "nested", timestamp_at(1100));

      assert(nested._status == MonData::UpdateStatus::INSERTED);
    }
  });

  const auto outer =
      reporter.report_count(outer_key, 10, "outer", timestamp_at(1200));

  assert(outer._status == MonData::UpdateStatus::INSERTED);

  assert(nested_started);
  assert(publish_count == 2);

  assert(store.find(outer_key).has_value());
  assert(store.find(nested_key).has_value());

  assert(store.size() == 2);
}

/*
 * 案例 7：并发替换 Publisher 与并发写入。
 *
 * 测试目的：
 * 验证一个线程持续写 _publisher、多个线程持续复制并调用 _publisher 时，
 * std::function 对象不会被并发破坏，也不会丢失应发生的发布。
 *
 * 执行过程：
 * 1. 准备两个始终有效的 Publisher A、B，并先注册 A；
 * 2. setter 线程在 A、B 之间高频切换；
 * 3. 四个 writer 线程各写入 1000 个互不重复的 Key；
 * 4. start 原子变量让 setter 和 writers 尽量同时开始；
 * 5. 汇总 Store 大小以及 A、B 的发布次数。
 *
 * 通过标准：
 * 每次写入都是 INSERTED，所以 Store 大小以及 A、B 调用次数之和都必须等于
 * 4000。普通运行只能发现明显破坏；是否存在 _publisher 数据竞争必须结合
 * TSan 判断。
 */
void test_concurrent_publisher_replacement_and_updates() {
  MonitorStore store;
  MonitorReporter reporter(store);

  std::atomic<std::size_t> publisher_a_count{0};
  std::atomic<std::size_t> publisher_b_count{0};

  MonitorReporter::Publisher publisher_a = [&](MonData::StoredRecord) {
    publisher_a_count.fetch_add(1, std::memory_order_relaxed);
  };

  MonitorReporter::Publisher publisher_b = [&](MonData::StoredRecord) {
    publisher_b_count.fetch_add(1, std::memory_order_relaxed);
  };

  reporter.set_publisher(publisher_a);

  constexpr std::size_t writer_count = 4;
  constexpr std::size_t writes_per_writer = 1000;

  std::atomic<bool> start{false};

  std::thread setter([&] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    for (std::size_t index = 0; index < 10000; ++index) {

      if ((index % 2) == 0) {
        reporter.set_publisher(publisher_a);
      } else {
        reporter.set_publisher(publisher_b);
      }
    }
  });

  std::vector<std::thread> writers;
  writers.reserve(writer_count);

  for (std::size_t writer = 0; writer < writer_count; ++writer) {

    writers.emplace_back([&, writer] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }

      for (std::size_t index = 0; index < writes_per_writer; ++index) {

        const MonData::MonitorKey key{7, 1, static_cast<std::uint32_t>(writer),
                                      static_cast<std::uint32_t>(index)};

        const auto result =
            reporter.report_count(key, 1, "concurrent", timestamp_at(1300));

        assert(result._status == MonData::UpdateStatus::INSERTED);
      }
    });
  }

  start.store(true, std::memory_order_release);

  setter.join();

  for (std::thread &writer : writers) {
    writer.join();
  }

  const std::size_t expected = writer_count * writes_per_writer;

  assert(store.size() == expected);

  const std::size_t total_published =
      publisher_a_count.load(std::memory_order_relaxed) +
      publisher_b_count.load(std::memory_order_relaxed);

  assert(total_published == expected);
}

/*
 * 案例 8：多个线程可以同时执行 Publisher。
 *
 * 测试目的：
 * 验证 Reporter 的互斥锁只保护 Publisher 快照，不会把不同更新对应的用户
 * Publisher 串行化。单次 Publisher 是同步调用，但多个调用可以分属不同
 * Timer/AIO worker 并发执行。
 *
 * 执行过程：
 * 1. Publisher 进入后增加 entered，并在条件变量上等待 release；
 * 2. 两个线程分别写入不同 Key；
 * 3. 主线程等待 entered==2，确认两个 Publisher 已同时进入；
 * 4. 主线程设置 release=true 并唤醒两个 Publisher；
 * 5. join 两个写线程并检查 Store。
 *
 * 通过标准：
 * 2 秒内必须观察到 entered==2，两个写入均为 INSERTED，Store 中有两个
 * 节点。如果 Reporter 持锁调用 Publisher，第二个线程无法进入，本案例会
 * 超时失败。
 */
void test_multiple_publishers_can_execute_concurrently() {
  MonitorStore store;
  MonitorReporter reporter(store);

  std::mutex gate_mutex;
  std::condition_variable gate_condition;

  std::size_t entered = 0;
  bool release = false;

  reporter.set_publisher([&](MonData::StoredRecord) {
    std::unique_lock<std::mutex> lock(gate_mutex);

    ++entered;
    gate_condition.notify_all();

    /*
     * condition_variable::wait()
     * 等待时会释放 gate_mutex，
     * 因此第二个 Publisher 可以进入。
     */
    gate_condition.wait(lock, [&] { return release; });
  });

  std::thread first([&] {
    const auto result =
        reporter.report_count({8, 1, 1, 1}, 1, "first", timestamp_at(1400));

    assert(result._status == MonData::UpdateStatus::INSERTED);
  });

  std::thread second([&] {
    const auto result =
        reporter.report_count({8, 1, 1, 2}, 1, "second", timestamp_at(1500));

    assert(result._status == MonData::UpdateStatus::INSERTED);
  });

  bool both_entered = false;

  {
    std::unique_lock<std::mutex> lock(gate_mutex);

    both_entered = gate_condition.wait_for(lock, std::chrono::seconds{2},
                                           [&] { return entered == 2; });

    /*
     * 即使失败也释放已经进入的回调，
     * 防止测试永久阻塞。
     */
    release = true;
  }

  gate_condition.notify_all();

  first.join();
  second.join();

  assert(both_entered);
  assert(entered == 2);
  assert(store.size() == 2);
}

/*
 * 案例 9：Publisher 自行保护捕获的共享状态。
 *
 * 测试目的：
 * 明确 Reporter 和 Publisher 的并发责任边界。Reporter 只保护 Publisher
 * 对象本身，不保护 Publisher 捕获的外部 vector、map 或其他业务状态。
 *
 * 执行过程：
 * 1. Publisher 捕获共享 vector；
 * 2. Publisher 每次 push_back() 前自行锁住 published_mutex；
 * 3. 四个 writer 线程各写入 500 个唯一 Key；
 * 4. join 后在相同 mutex 保护下检查 vector 大小；
 * 5. 检查 Store 大小。
 *
 * 通过标准：
 * published 和 Store 都必须包含 2000 条记录，TSan 不应报告共享 vector 的
 * 数据竞争。删除 Publisher 内部的 published_mutex 后，测试代码自身将产生
 * 数据竞争，这不是 Reporter 应负责修复的问题。
 */
void test_publisher_protects_its_shared_state() {
  MonitorStore store;
  MonitorReporter reporter(store);

  std::mutex published_mutex;
  std::vector<MonData::StoredRecord> published;

  reporter.set_publisher([&](MonData::StoredRecord record) {
    /*
     * Reporter 不保护 Publisher 捕获的外部对象。
     */
    std::lock_guard<std::mutex> lock(published_mutex);

    published.push_back(std::move(record));
  });

  constexpr std::size_t writer_count = 4;
  constexpr std::size_t writes_per_writer = 500;

  std::vector<std::thread> writers;
  writers.reserve(writer_count);

  for (std::size_t writer = 0; writer < writer_count; ++writer) {

    writers.emplace_back([&, writer] {
      for (std::size_t index = 0; index < writes_per_writer; ++index) {

        const MonData::MonitorKey key{9, 1, static_cast<std::uint32_t>(writer),
                                      static_cast<std::uint32_t>(index)};

        const auto result =
            reporter.report_count(key, 1, "shared-state", timestamp_at(1600));

        assert(result._status == MonData::UpdateStatus::INSERTED);
      }
    });
  }

  for (std::thread &writer : writers) {
    writer.join();
  }

  const std::size_t expected = writer_count * writes_per_writer;

  {
    std::lock_guard<std::mutex> lock(published_mutex);

    assert(published.size() == expected);
  }

  assert(store.size() == expected);
}

/*
 * 案例 10：Publisher 异常被 Reporter 隔离。
 *
 * 测试目的：
 * 验证用户 Publisher 抛出的任意异常不会穿透 report_count()，从而避免异常
 * 逃出 Timer/AIO 工作线程。Publisher 失败也不能回滚已经提交的 Store 更新。
 *
 * 执行过程：
 * 1. 注册一个始终抛 std::runtime_error 的 Publisher；
 * 2. 调用 report_count()，记录异常是否逃逸；
 * 3. 查询 Store，确认首次写入已经保存；
 * 4. 替换为正常 Publisher；
 * 5. 修改同一 Key，确认 Reporter 仍能继续发布；
 * 6. 检查首次调用正常返回 INSERTED。
 *
 * 通过标准：
 * exception_escaped 为 false，首次记录存在，第二次写入返回 UPDATED，正常
 * Publisher 调用一次，Store 中仍只有同一个 Key。当前未捕获 Publisher 异常
 * 的实现会在最后的断言中失败。
 */
void test_publisher_exception_is_contained() {
  MonitorStore store;
  MonitorReporter reporter(store);

  const MonData::MonitorKey key{10, 1, 1, 1};

  reporter.set_publisher([](MonData::StoredRecord) {
    throw std::runtime_error{"publisher failure"};
  });

  bool exception_escaped = false;

  MonData::UpdateResult inserted;

  try {
    inserted =
        reporter.report_count(key, 1, "throwing-publisher", timestamp_at(1700));
  } catch (...) {
    exception_escaped = true;
  }

  /*
   * 即使当前实现让异常逃逸，
   * Store 更新也已经发生。
   */
  const auto stored_after_exception = store.find(key);

  assert(stored_after_exception.has_value());

  /*
   * 替换成正常 Publisher，验证 Reporter
   * 后续仍然可以使用。
   */
  std::atomic<std::size_t> normal_count{0};

  reporter.set_publisher([&](MonData::StoredRecord) {
    normal_count.fetch_add(1, std::memory_order_relaxed);
  });

  const auto updated =
      reporter.report_count(key, 2, "normal-publisher", timestamp_at(1800));

  assert(updated._status == MonData::UpdateStatus::UPDATED);

  assert(normal_count.load(std::memory_order_relaxed) == 1);

  /*
   * 并且第一次调用必须正常返回 INSERTED。
   */
  assert(!exception_escaped);

  assert(inserted._status == MonData::UpdateStatus::INSERTED);

  assert(inserted._record.has_value());

  assert(store.size() == 1);
}
int main() {
  test_update_without_publisher_still_updates_store();
  test_empty_publisher_unregisters_current_publisher();
  test_replacing_publisher_affects_future_updates();
  test_inflight_update_uses_publisher_snapshot();
  test_publisher_can_unregister_itself();
  test_publisher_can_reenter_reporter();
  test_concurrent_publisher_replacement_and_updates();
  test_multiple_publishers_can_execute_concurrently();
  test_publisher_protects_its_shared_state();
  test_publisher_exception_is_contained();

  return 0;
}
