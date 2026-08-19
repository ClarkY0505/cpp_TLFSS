#include "monitor_reporter.h"
#include "monitor_store.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace TLSSMON;

/*
 * 按值传递保证 Publisher 得到独立副本：
 *
 * 1. Publisher 可以保存该记录；
 * 2. Publisher 修改参数不会修改 Store；
 * 3. Reporter 返回后记录仍然有效；
 * 4. 后续 M6 可以安全地异步保存或编码这个副本。
 */
using ExpectedMonitorPublisher = std::function<void(MonData::StoredRecord)>;

/*
 * 锁定 TLSSMON 命名空间中的公共 Publisher 类型。
 */
static_assert(std::is_same_v<MonitorPublisher, ExpectedMonitorPublisher>,
              "MonitorPublisher must be "
              "std::function<void(MonData::StoredRecord)>");

/*
 * MonitorReporter 内部也应暴露同一个 Publisher 类型，
 * 方便 Engine 使用 MonitorReporter::Publisher。
 */
static_assert(
    std::is_same_v<MonitorReporter::Publisher, ExpectedMonitorPublisher>,
    "MonitorReporter::Publisher must match MonitorPublisher");

/*
 * 额外验证 Publisher 可以使用 StoredRecord 按值调用，
 * 并且返回类型严格为 void。
 */
static_assert(
    std::is_invocable_r_v<void, MonitorPublisher &, MonData::StoredRecord>,
    "MonitorPublisher must accept StoredRecord by value");

/*
 * MonitorReporter 必须显式绑定一个已经存在的 MonitorStore。
 *
 * Reporter 不拥有 Store，因此构造参数必须是 MonitorStore&。
 */
static_assert(std::is_constructible_v<MonitorReporter, MonitorStore &>,
              "MonitorReporter must be constructible from MonitorStore&");

/*
 * 不允许创建没有 Store 的 Reporter。
 */
static_assert(!std::is_default_constructible_v<MonitorReporter>,
              "MonitorReporter must not be default constructible");

/*
 * 类保持 final，避免通过继承修改 Publisher 和 Store 的调用语义。
 */
static_assert(std::is_final_v<MonitorReporter>,
              "MonitorReporter must remain final");

/*
 * 锁定 set_publisher()：
 *
 * 返回值：void
 * 参数：MonitorPublisher，按值传递
 *
 * 按值进入接口后，Reporter 可以将 Publisher 移入自己的成员。
 * 传入空 std::function 表示注销 Publisher。
 */
using ExpectedSetPublisher = void (MonitorReporter::*)(MonitorPublisher);

static_assert(std::is_same_v<decltype(&MonitorReporter::set_publisher),
                             ExpectedSetPublisher>,
              "MonitorReporter::set_publisher signature changed");

/*
 * 锁定统一更新入口：
 *
 * 返回值：MonData::UpdateResult
 * 参数 1：MonitorData，按值传递
 * 参数 2：force
 * 参数 3：调用方提供的时间戳
 */
using ExpectedReporterUpdate = MonData::UpdateResult (MonitorReporter::*)(
    MonData::MonitorData, bool, MonData::MonitorTimestamp);

static_assert(
    std::is_same_v<decltype(&MonitorReporter::update), ExpectedReporterUpdate>,
    "MonitorReporter::update signature changed");

/*
 * Reporter 内部持有 MonitorStore 引用和 Publisher 状态，
 * 复制或移动都可能破坏 Store 生命周期关系，因此全部禁止。
 */
static_assert(!std::is_copy_constructible_v<MonitorReporter>,
              "MonitorReporter must not be copy constructible");

static_assert(!std::is_copy_assignable_v<MonitorReporter>,
              "MonitorReporter must not be copy assignable");

static_assert(!std::is_move_constructible_v<MonitorReporter>,
              "MonitorReporter must not be move constructible");

static_assert(!std::is_move_assignable_v<MonitorReporter>,
              "MonitorReporter must not be move assignable");

MonData::MonitorTimestamp timestamp_at(std::int64_t seconds) {
  return MonData::MonitorTimestamp{std::chrono::seconds{seconds}};
}

MonData::MonitorData make_numeric(MonData::MonitorKey key, std::uint32_t value,
                                  std::uint32_t state,
                                  std::string description) {
  return MonData::MonitorData{key, std::move(description),
                              MonData::NumericValue{value, state}};
}

const MonData::NumericValue &
numeric_value(const MonData::StoredRecord &record) {
  return std::get<MonData::NumericValue>(record._data._value);
}

void test_update_publishes_only_inserted_and_updated() {
  MonitorStore store;
  MonitorReporter reporter(store);

  std::vector<MonData::StoredRecord> published;

  reporter.set_publisher([&](MonData::StoredRecord record) {
    published.push_back(std::move(record));
  });

  const MonData::MonitorKey key{
      1,  // mid
      4,  // level
      23, // fid
      5   // eid
  };

  const auto first_time = timestamp_at(100);
  const auto duplicate_time = timestamp_at(200);
  const auto changed_time = timestamp_at(300);

  /*
   * 首次非零值：
   *
   * Store 返回 INSERTED；
   * Publisher 必须同步收到一条记录。
   */
  const MonData::UpdateResult inserted =
      reporter.update(make_numeric(key, 5, 0, "initial"), false, first_time);

  assert(inserted._status == MonData::UpdateStatus::INSERTED);

  assert(inserted.changed());
  assert(inserted._record.has_value());

  assert(published.size() == 1);
  assert(published[0]._data._key == key);
  assert(numeric_value(published[0])._value == 5);
  assert(numeric_value(published[0])._state == 0);
  assert(published[0]._data._description == "initial");
  assert(published[0]._changed_at == first_time);

  /*
   * 相同数值：
   *
   * 即使 state、description 和 timestamp 改变，
   * M4 仍然返回 UNCHANGED；
   * Publisher 调用次数不能增加。
   */
  const MonData::UpdateResult unchanged = reporter.update(
      make_numeric(key, 5, 2, "duplicate-metadata"), false, duplicate_time);

  assert(unchanged._status == MonData::UpdateStatus::UNCHANGED);

  assert(!unchanged.changed());
  assert(unchanged._record.has_value());

  assert(published.size() == 1);

  /*
   * 数值变化：
   *
   * Store 返回 UPDATED；
   * Publisher 必须收到第二条记录。
   */
  const MonData::UpdateResult updated =
      reporter.update(make_numeric(key, 8, 2, "changed"), false, changed_time);

  assert(updated._status == MonData::UpdateStatus::UPDATED);

  assert(updated.changed());
  assert(updated._record.has_value());

  assert(published.size() == 2);
  assert(published[1]._data._key == key);
  assert(numeric_value(published[1])._value == 8);
  assert(numeric_value(published[1])._state == 2);
  assert(published[1]._data._description == "changed");
  assert(published[1]._changed_at == changed_time);

  /*
   * 首次普通零值：
   *
   * 使用一个从未出现过的新 Key，
   * force=false 时必须被首次零值门禁忽略。
   */
  const MonData::MonitorKey zero_key{1, 4, 23, 6};

  const MonData::UpdateResult ignored = reporter.update(
      make_numeric(zero_key, 0, 0, "initial-zero"), false, timestamp_at(400));

  assert(ignored._status == MonData::UpdateStatus::IGNORED_INITIAL_ZERO);

  assert(!ignored.changed());
  assert(!ignored._record.has_value());

  /*
   * IGNORED_INITIAL_ZERO 不得进入 Publisher。
   */
  assert(published.size() == 2);

  /*
   * 首次零值不得产生 Store 节点。
   */
  assert(!store.find(zero_key).has_value());

  /*
   * 原 Key 经过一次插入和一次更新后仍然只有一条记录。
   */
  assert(store.size() == 1);
}

void test_publisher_runs_synchronously_and_receives_copy() {
  MonitorStore store;
  MonitorReporter reporter(store);

  const MonData::MonitorKey key{
      2,  // mid
      7,  // level
      31, // fid
      9   // eid
  };

  const auto original_time = timestamp_at(500);
  const auto publisher_only_time = timestamp_at(999);

  /*
   * 保存 update() 调用线程。
   *
   * 如果 Publisher 被放到其他线程执行，
   * publisher_thread 将与 caller_thread 不同。
   */
  const std::thread::id caller_thread = std::this_thread::get_id();

  std::thread::id publisher_thread;

  bool publisher_started = false;
  bool publisher_finished = false;
  std::size_t publish_count = 0;

  /*
   * 保留 Publisher 收到并修改后的副本。
   *
   * 在 update() 返回之后继续检查它，
   * 证明 Publisher 参数拥有独立生命周期。
   */
  std::optional<MonData::StoredRecord> retained_publisher_copy;

  reporter.set_publisher([&](MonData::StoredRecord record) {
    publisher_started = true;
    publisher_thread = std::this_thread::get_id();

    ++publish_count;

    /*
     * 只修改 Publisher 自己收到的参数。
     *
     * 如果 Reporter 错误地传递 Store 内部引用，
     * 这些修改会污染 Store。
     */
    record._data._description = "publisher-only-description";

    MonData::NumericValue &numeric =
        std::get<MonData::NumericValue>(record._data._value);

    numeric._value = 999;
    numeric._state = 1;

    record._changed_at = publisher_only_time;

    retained_publisher_copy = std::move(record);

    publisher_finished = true;
  });

  const MonData::UpdateResult result = reporter.update(
      make_numeric(key, 42, 2, "store-original"), false, original_time);

  /*
   * 首次非零值必须成功插入。
   */
  assert(result._status == MonData::UpdateStatus::INSERTED);

  assert(result.changed());
  assert(result._record.has_value());

  /*
   * update() 返回前 Publisher 必须已经开始并结束。
   *
   * 如果 Publisher 是异步执行，
   * 这里可能仍然是 false。
   */
  assert(publisher_started);
  assert(publisher_finished);
  assert(publish_count == 1);

  /*
   * Publisher 必须在调用 update() 的同一个线程中执行。
   */
  assert(publisher_thread == caller_thread);

  /*
   * Publisher 修改后的副本必须可以在回调返回后继续使用。
   */
  assert(retained_publisher_copy.has_value());

  assert(retained_publisher_copy->_data._description ==
         "publisher-only-description");

  assert(numeric_value(*retained_publisher_copy)._value == 999);

  assert(numeric_value(*retained_publisher_copy)._state == 1);

  assert(retained_publisher_copy->_changed_at == publisher_only_time);

  /*
   * UpdateResult 中的记录也必须保持 Reporter
   * 从 Store 获得的原始内容。
   */
  assert(result._record->_data._description == "store-original");

  assert(numeric_value(*result._record)._value == 42);

  assert(numeric_value(*result._record)._state == 2);

  assert(result._record->_changed_at == original_time);

  /*
   * Store 内部记录不能受到 Publisher 参数修改的影响。
   */
  const auto stored = store.find(key);

  assert(stored.has_value());

  assert(stored->_data._description == "store-original");

  assert(numeric_value(*stored)._value == 42);

  assert(numeric_value(*stored)._state == 2);

  assert(stored->_changed_at == original_time);

  assert(store.size() == 1);
}

void test_publisher_runs_after_store_unlock() {
  MonitorStore store;
  MonitorReporter reporter(store);

  const MonData::MonitorKey published_key{
      3,  // mid
      2,  // level
      40, // fid
      50  // eid
  };

  const MonData::MonitorKey nested_key{3, 2, 40, 51};

  const auto published_time = timestamp_at(600);
  const auto nested_time = timestamp_at(700);

  bool publisher_entered = false;
  bool find_completed = false;
  bool query_completed = false;
  bool nested_update_completed = false;
  bool publisher_finished = false;

  reporter.set_publisher([&](MonData::StoredRecord record) {
    publisher_entered = true;

    /*
     * Publisher 收到当前已经保存的记录。
     */
    assert(record._data._key == published_key);

    assert(numeric_value(record)._value == 10);

    assert(record._changed_at == published_time);

    /*
     * 重新进入 Store::find()。
     *
     * 如果外层 MonitorStore::update() 尚未释放锁，
     * 这里会发生自死锁。
     */
    const auto found = store.find(published_key);

    assert(found.has_value());

    assert(numeric_value(*found)._value == 10);

    assert(found->_changed_at == published_time);

    find_completed = true;

    /*
     * 重新进入 Store::query()。
     *
     * 此时外层插入必须已经提交，所以查询结果中
     * 必须包含 published_key。
     */
    const auto snapshot = store.query();

    assert(snapshot.size() == 1);

    assert(snapshot[0]._data._key == published_key);

    query_completed = true;

    /*
     * 直接调用 Store::update() 写入另一个 Key。
     *
     * 使用 Store 而不是 Reporter，避免再次触发
     * 当前 Publisher 形成递归调用。
     */
    const MonData::UpdateResult nested =
        store.update(make_numeric(nested_key, 20, 2, "nested-store-update"),
                     false, nested_time);

    assert(nested._status == MonData::UpdateStatus::INSERTED);

    assert(nested.changed());
    assert(nested._record.has_value());

    nested_update_completed = true;
    publisher_finished = true;
  });

  /*
   * 外层 update() 插入 published_key，并同步进入 Publisher。
   */
  const MonData::UpdateResult result = reporter.update(
      make_numeric(published_key, 10, 2, "published"), false, published_time);

  assert(result._status == MonData::UpdateStatus::INSERTED);

  assert(result.changed());
  assert(result._record.has_value());

  /*
   * Publisher 是同步执行的，因此 update() 返回时，
   * Publisher 内所有 Store 重入操作都必须已经完成。
   */
  assert(publisher_entered);
  assert(find_completed);
  assert(query_completed);
  assert(nested_update_completed);
  assert(publisher_finished);

  /*
   * 外层插入和 Publisher 内的直接插入都应保留。
   */
  assert(store.size() == 2);

  const auto published_record = store.find(published_key);

  const auto nested_record = store.find(nested_key);

  assert(published_record.has_value());
  assert(nested_record.has_value());

  assert(numeric_value(*published_record)._value == 10);

  assert(numeric_value(*nested_record)._value == 20);

  assert(nested_record->_changed_at == nested_time);
}

void test_update_without_publisher_still_updates_store() {
  MonitorStore store;

  /*
   * 不调用 set_publisher()。
   *
   * Reporter 的默认状态必须是没有 Publisher，
   * 但这不能阻止数据写入 Store。
   */
  MonitorReporter reporter(store);

  /*
   * 当前纯 C++ Key：
   *
   * mid   = 0x100
   * level = 7
   * fid   = 23，对应旧 M5 的 hid
   * eid   = 45
   */
  const MonData::MonitorKey key{0x100, 7, 23, 45};

  const auto first_time = timestamp_at(800);
  const auto duplicate_time = timestamp_at(900);
  const auto changed_time = timestamp_at(1000);

  /*
   * 没有 Publisher 时首次非零值仍然必须保存。
   */
  const MonData::UpdateResult inserted = reporter.update(
      make_numeric(key, 50, 2, "caller-description"), false, first_time);

  assert(inserted._status == MonData::UpdateStatus::INSERTED);

  assert(inserted.changed());
  assert(inserted._record.has_value());

  /*
   * UpdateResult 必须包含完整的存储记录。
   */
  assert(inserted._record->_data._key._mid == 0x100);

  assert(inserted._record->_data._key._level == 7);

  assert(inserted._record->_data._key._fid == 23);

  assert(inserted._record->_data._key._eid == 45);

  assert(inserted._record->_data._description == "caller-description");

  assert(numeric_value(*inserted._record)._value == 50);

  assert(numeric_value(*inserted._record)._state == 2);

  assert(inserted._record->_changed_at == first_time);

  /*
   * Store 中必须存在完全相同的记录。
   */
  const auto first_stored = store.find(key);

  assert(first_stored.has_value());

  assert(first_stored->_data._key == key);

  assert(first_stored->_data._description == "caller-description");

  assert(numeric_value(*first_stored)._value == 50);

  assert(numeric_value(*first_stored)._state == 2);

  assert(first_stored->_changed_at == first_time);

  assert(store.size() == 1);

  /*
   * 相同数值仍然必须按照 M4 去重。
   *
   * 没有 Publisher 不能导致每次 update() 都被当成变化。
   */
  const MonData::UpdateResult unchanged =
      reporter.update(make_numeric(key, 50, 1, "must-not-replace-original"),
                      false, duplicate_time);

  assert(unchanged._status == MonData::UpdateStatus::UNCHANGED);

  assert(!unchanged.changed());
  assert(unchanged._record.has_value());

  /*
   * UNCHANGED 必须保留首次保存的元数据和时间戳。
   */
  assert(unchanged._record->_data._description == "caller-description");

  assert(numeric_value(*unchanged._record)._state == 2);

  assert(unchanged._record->_changed_at == first_time);

  const auto after_unchanged = store.find(key);

  assert(after_unchanged.has_value());

  assert(after_unchanged->_data._description == "caller-description");

  assert(numeric_value(*after_unchanged)._value == 50);

  assert(numeric_value(*after_unchanged)._state == 2);

  assert(after_unchanged->_changed_at == first_time);

  /*
   * 数值变化时，即使没有 Publisher，
   * Store 仍然必须完成整体替换。
   */
  const MonData::UpdateResult updated =
      reporter.update(make_numeric(key, 60, 1, "caller-updated-description"),
                      false, changed_time);

  assert(updated._status == MonData::UpdateStatus::UPDATED);

  assert(updated.changed());
  assert(updated._record.has_value());

  assert(updated._record->_data._key == key);

  assert(updated._record->_data._description == "caller-updated-description");

  assert(numeric_value(*updated._record)._value == 60);

  assert(numeric_value(*updated._record)._state == 1);

  assert(updated._record->_changed_at == changed_time);

  const auto final_stored = store.find(key);

  assert(final_stored.has_value());

  assert(final_stored->_data._key == key);

  assert(final_stored->_data._description == "caller-updated-description");

  assert(numeric_value(*final_stored)._value == 60);

  assert(numeric_value(*final_stored)._state == 1);

  assert(final_stored->_changed_at == changed_time);

  /*
   * 对同一个 Key 的重复和更新不能创建额外节点。
   */
  assert(store.size() == 1);
}

int main() {
  test_update_publishes_only_inserted_and_updated();
  test_publisher_runs_synchronously_and_receives_copy();
  test_publisher_runs_after_store_unlock();

  test_update_without_publisher_still_updates_store();

  return 0;
}
