#include "ftp/object_garbage_collector.h"
#include "ftp/object_gc_transaction.h"

#include <chrono>
#include <initializer_list>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace TLSS::BASE;
using namespace TLSS::FileStorage;
using Seconds = std::chrono::seconds;

const std::string object_hash(64, 'a');
const std::string second_hash(64, 'b');
const std::string third_hash(64, 'c');

enum class Stage { none, borrow, listing, prepare, remove, finalize };

class InjectedFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Configuration {
    bool no_connection = false;
    int no_connection_at = 0;
    bool check_other_hash = false;
    Stage throw_at = Stage::none;
    std::string throw_hash;
    DbQueryResult deleting_result{true, {}};
    ObjectGcPrepareResult prepare_result = ObjectGcPrepareResult::ready_to_delete;
    ObjectGcFinalizeResult finalize_result = ObjectGcFinalizeResult::completed;
    std::map<std::string, ObjectGcPrepareResult> prepare_overrides;
    std::map<std::string, ObjectGcFinalizeResult> finalize_overrides;
};

// 只替换边界，运行真实 collector 和 coordinator。
// 事务内部与真实文件删除分别由已有事务、磁盘测试覆盖。
struct State {
    Configuration configuration;
    DbConnectionPool* pool = nullptr;
    ObjectStorageEngine* objects = nullptr;
    ObjectCoordinator* coordinator = nullptr;
    DbConnection* borrowed_connection = nullptr;
    bool recovering = false;
    bool listing_connection = false;
    bool list_returned = false;
    std::vector<std::string> expected_hashes;
    std::vector<std::string> removed_hashes;
    std::vector<std::string> finalized_hashes;
    std::size_t next_hash = 0;
    std::string expected_hash;
    Seconds expected_grace{600};
    std::vector<std::string> events;
    int created = 0;
    int destroyed = 0;
    int returned = 0;
    int borrow_attempts = 0;
    bool removed = false;
    bool cleanup_failed = false;
} state;

void check(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

// 必须从另一个线程探测，不能在持锁线程内对同一 std::mutex 再次 try_lock。
// join 提供同步，结果无需通过 sleep 或线程调度时机推断。
bool available_from_another_thread(std::mutex& mutex) {
    bool acquired = false;
    std::thread observer([&] {
        acquired = mutex.try_lock();
        if (acquired)
            mutex.unlock();
    });
    observer.join();
    return acquired;
}

void check_hash_locked() {
    check(state.coordinator != nullptr, "missing coordinator");
    check(!available_from_another_thread(
              state.coordinator->mutex_for(state.expected_hash)),
          "collector did not retain the shared hash lock");
}

void check_other_hash_available() {
    // 哈希分片允许碰撞，因此先找出一个确实落在不同互斥量上的合法哈希。
    auto& held = state.coordinator->mutex_for(state.expected_hash);
    const std::string digits = "0123456789abcdef";
    std::string other_hash(64, 'b');
    for (unsigned int value = 0; value < 4096; ++value) {
        other_hash[61] = digits[(value >> 8) & 15];
        other_hash[62] = digits[(value >> 4) & 15];
        other_hash[63] = digits[value & 15];
        auto& other = state.coordinator->mutex_for(other_hash);
        if (&other != &held) {
            check(available_from_another_thread(other), "unrelated hash lock was blocked");
            return;
        }
    }
    throw std::runtime_error("could not find a distinct hash-lock shard");
}

void enter_stage(Stage stage, const char* name) {
    state.events.emplace_back(name);
    check_hash_locked();
    if (stage == Stage::remove && state.configuration.check_other_hash)
        check_other_hash_available();
    if (state.configuration.throw_at == stage &&
        (state.configuration.throw_hash.empty() ||
         state.configuration.throw_hash == state.expected_hash))
        throw InjectedFailure(std::string("injected failure at ") + name);
}

// 枚举清单时不持有对象哈希锁，连接必须在进入单对象流程前归还。
void enter_listing_stage(Stage stage, const char* name) {
    state.events.emplace_back(name);
    check(available_from_another_thread(state.coordinator->mutex_for(state.expected_hash)),
          "recovery held a hash lock while enumerating candidates");
    if (state.configuration.throw_at == stage && state.configuration.throw_hash.empty())
        throw InjectedFailure(std::string("injected failure at ") + name);
}

ObjectGcPrepareResult configured_preparation() {
    const auto found = state.configuration.prepare_overrides.find(state.expected_hash);
    return found == state.configuration.prepare_overrides.end()
        ? state.configuration.prepare_result : found->second;
}

ObjectGcFinalizeResult configured_finalization() {
    const auto found = state.configuration.finalize_overrides.find(state.expected_hash);
    return found == state.configuration.finalize_overrides.end()
        ? state.configuration.finalize_result : found->second;
}

void check_connection(DbConnection& connection) {
    check(state.borrowed_connection == &connection,
          "transaction did not use the borrowed connection");
}

}  // namespace

namespace TLSS::BASE {

// 替换单例的构造与借用边界，不读配置、不建连、不启动连接池线程。
DbConnectionPool::DbConnectionPool()
    : _port(0),
      _init_size(0),
      _max_size(0),
      _max_idle_time(0),
      _connection_timeout(0),
      _connect_timeout(0),
      _read_timeout(0),
      _write_timeout(0),
      _produce_thread([] {}),
      _scanner_thread([] {}),
      _connection_cnt(0),
      _stopping(false),
      _threads_joined(true),
      _borrowed_count(0),
      _active_getters(0) {}

DbConnectionPool::~DbConnectionPool() = default;

DbConnectionPool* DbConnectionPool::get_connection_pool() {
    static DbConnectionPool pool;
    return &pool;
}

DbConnectionHandle DbConnectionPool::get_connection() {
    check(this == state.pool, "collector used a different pool");
    // 模拟仅有一条可借用连接，第二次借用前必须先归还上一条。
    check(state.borrowed_connection == nullptr, "collector borrowed multiple connections");
    ++state.borrow_attempts;
    state.listing_connection = state.recovering && !state.list_returned;
    if (state.listing_connection) {
        enter_listing_stage(Stage::borrow, "list_borrow");
    } else {
        if (state.recovering) {
            check(state.next_hash < state.expected_hashes.size(),
                  "recovery attempted an unexpected candidate");
            state.expected_hash = state.expected_hashes[state.next_hash++];
            state.removed = false;
        }
        // 在单对象借连接的入口检查锁，验证先加哈希锁、再借连接。
        enter_stage(Stage::borrow, "borrow");
    }
    if (state.configuration.no_connection ||
        state.configuration.no_connection_at == state.borrow_attempts)
        return DbConnectionHandle(nullptr, DbConnectionDeleter{this});

    auto* connection = new DbConnection;
    state.borrowed_connection = connection;
    return DbConnectionHandle(connection, DbConnectionDeleter{this});
}

void DbConnectionDeleter::operator()(DbConnection* connection) const noexcept {
    if (!connection)
        return;

    // noexcept 清理中记录检查失败，等 collect 返回后再由测试报告。
    try {
        check(pool == state.pool && connection == state.borrowed_connection,
              "connection returned to the wrong pool");
        if (state.listing_connection) {
            state.events.emplace_back("list_return");
            state.list_returned = true;
        } else {
            state.events.emplace_back("return");
            check_hash_locked();
        }
        ++state.returned;
    } catch (...) {
        state.cleanup_failed = true;
    }
    state.borrowed_connection = nullptr;
    delete connection;
}

DbConnection::DbConnection()
    : _conn(nullptr), _connected(false), _transaction_active(false) {
    ++state.created;
}

DbConnection::~DbConnection() {
    ++state.destroyed;
}

// 使用真实 ObjectRepository 构造候选查询，替代连接只接受恢复清单 SQL。
DbQueryResult DbConnection::query_prepared(
    const std::string& sql, const std::vector<DbValue>& params) {
    check_connection(*this);
    check(state.listing_connection, "candidate query used an object-processing connection");
    check(params.empty(), "candidate query unexpectedly bound parameters");
    check(sql.find("SELECT object_hash FROM storage_objects ") == 0 &&
              sql.find("WHERE state = 'DELETING' ") != std::string::npos &&
              sql.find("ORDER BY object_hash") != std::string::npos &&
              sql.find("FOR UPDATE") == std::string::npos,
          "invalid candidate query or unexpected locking read");
    enter_listing_stage(Stage::listing, "list");
    return state.configuration.deleting_result;
}

DbExecuteResult DbConnection::execute_prepared(
    const std::string&, const std::vector<DbValue>&) {
    throw std::runtime_error("candidate enumeration unexpectedly wrote to the database");
}

}  // namespace TLSS::BASE

namespace TLSS::FileStorage {

ObjectGcPrepareResult prepare_object_gc_transaction(
    BASE::DbConnection& connection, const std::string& hash, Seconds grace_period) {
    check_connection(connection);
    check(hash == state.expected_hash && grace_period == state.expected_grace,
          "collector changed preparation arguments");
    enter_stage(Stage::prepare, "prepare");
    return configured_preparation();
}

void ObjectStorageEngine::remove_object(const std::string& hash) const {
    check(this == state.objects, "collector used a different object storage engine");
    check(hash == state.expected_hash, "collector removed the wrong hash");
    check(state.borrowed_connection != nullptr, "connection released before disk removal");
    check(configured_preparation() == ObjectGcPrepareResult::ready_to_delete,
          "disk removal happened without confirmed deletion permission");
    enter_stage(Stage::remove, "remove");
    state.removed = true;
    state.removed_hashes.push_back(hash);
}

ObjectGcFinalizeResult finalize_object_gc_transaction(
    BASE::DbConnection& connection, const std::string& hash) {
    check_connection(connection);
    check(hash == state.expected_hash, "collector finalized the wrong hash");
    check(state.removed, "finalization happened before successful disk removal");
    enter_stage(Stage::finalize, "finalize");
    const auto result = configured_finalization();
    if (result == ObjectGcFinalizeResult::completed)
        state.finalized_hashes.push_back(hash);
    return result;
}

}  // namespace TLSS::FileStorage

namespace {

void run_case(const std::string& name, Configuration configuration,
              ObjectGcCollectResult expected_result, const std::vector<std::string>& expected_events,
              bool expect_exception = false, const std::string& hash = object_hash,
              Seconds grace_period = Seconds{600}) {
    ObjectStorageEngine objects;
    ObjectCoordinator coordinator;
    auto* pool = DbConnectionPool::get_connection_pool();

    state = {};
    state.configuration = configuration;
    state.pool = pool;
    state.objects = &objects;
    state.coordinator = &coordinator;
    state.expected_hash = hash;
    state.expected_grace = grace_period;

    ObjectGarbageCollector collector(*pool, objects, coordinator);
    ObjectGcCollectResult result = ObjectGcCollectResult::failed;
    bool caught = false;
    try {
        result = collector.collect(hash, grace_period);
    } catch (const InjectedFailure&) {
        caught = true;
    }

    check(caught == expect_exception, name + ": exception was swallowed or unexpected");
    if (!expect_exception)
        check(result == expected_result, name + ": unexpected collection result");
    check(state.events == expected_events, name + ": wrong stage order, extra call or retry");
    check(!state.cleanup_failed, name + ": connection cleanup or lock ordering failed");
    check(state.created == state.returned && state.created == state.destroyed &&
              state.borrowed_connection == nullptr,
          name + ": borrowed connection was leaked or returned more than once");
    // 成功、提前返回和异常路径都必须释放锁，供后续上传或恢复使用。
    check(available_from_another_thread(coordinator.mutex_for(hash)),
          name + ": collector retained the hash lock after returning");
}

void test_invalid_arguments() {
    for (const auto& hash : {std::string{}, std::string("../escape"), std::string(63, 'a'),
                             std::string(65, 'a'), std::string(64, 'A'), std::string(64, 'g')}) {
        run_case("invalid hash", {}, ObjectGcCollectResult::failed, {}, false, hash);
    }
    for (const auto grace : {Seconds{0}, Seconds{-1}}) {
        run_case("invalid grace period", {}, ObjectGcCollectResult::failed,
                 {}, false, object_hash, grace);
    }
}

void test_connection_failure() {
    Configuration configuration;
    configuration.no_connection = true;
    run_case("empty connection", configuration, ObjectGcCollectResult::failed, {"borrow"});
}

void test_prepare_results() {
    struct Case {
        ObjectGcPrepareResult prepared;
        ObjectGcCollectResult expected;
    };
    const std::vector<Case> cases{
        {ObjectGcPrepareResult::skipped, ObjectGcCollectResult::skipped},
        {ObjectGcPrepareResult::failed, ObjectGcCollectResult::failed},
        {ObjectGcPrepareResult::outcome_unknown, ObjectGcCollectResult::outcome_unknown},
        {static_cast<ObjectGcPrepareResult>(99), ObjectGcCollectResult::outcome_unknown},
    };
    for (const auto& item : cases) {
        Configuration configuration;
        configuration.prepare_result = item.prepared;
        run_case("preparation stopped collection", configuration, item.expected,
                 {"borrow", "prepare", "return"});
    }
}

void test_finalize_results() {
    struct Case {
        ObjectGcFinalizeResult finalized;
        ObjectGcCollectResult expected;
    };
    const std::vector<Case> cases{
        {ObjectGcFinalizeResult::completed, ObjectGcCollectResult::collected},
        {ObjectGcFinalizeResult::failed, ObjectGcCollectResult::failed},
        {ObjectGcFinalizeResult::outcome_unknown, ObjectGcCollectResult::outcome_unknown},
        {static_cast<ObjectGcFinalizeResult>(99), ObjectGcCollectResult::outcome_unknown},
    };
    for (const auto& item : cases) {
        Configuration configuration;
        configuration.finalize_result = item.finalized;
        run_case("finalization result", configuration, item.expected,
                 {"borrow", "prepare", "remove", "finalize", "return"});
    }
}

void test_exceptions() {
    struct Case {
        Stage stage;
        std::vector<std::string> expected_events;
    };
    const std::vector<Case> cases{
        {Stage::borrow, {"borrow"}},
        {Stage::prepare, {"borrow", "prepare", "return"}},
        {Stage::remove, {"borrow", "prepare", "remove", "return"}},
        {Stage::finalize, {"borrow", "prepare", "remove", "finalize", "return"}},
    };
    for (const auto& item : cases) {
        Configuration configuration;
        configuration.throw_at = item.stage;
        run_case("exception propagation", configuration, ObjectGcCollectResult::failed,
                 item.expected_events, true);
    }
}

void test_hash_lock() {
    Configuration configuration;
    configuration.check_other_hash = true;
    run_case("shared hash lock lifetime", configuration, ObjectGcCollectResult::collected,
             {"borrow", "prepare", "remove", "finalize", "return"});
}

const std::vector<std::string> collected_events{
    "borrow", "prepare", "remove", "finalize", "return"};
const std::vector<std::string> preparation_stop_events{"borrow", "prepare", "return"};

std::vector<std::string> recovery_events(
    std::initializer_list<std::vector<std::string>> objects) {
    std::vector<std::string> events{"list_borrow", "list", "list_return"};
    for (const auto& object_events : objects)
        events.insert(events.end(), object_events.begin(), object_events.end());
    return events;
}

void run_recovery(const std::string& name, Configuration configuration, bool expected_result,
                  const std::vector<std::string>& expected_events,
                  const std::vector<std::string>& expected_hashes = {},
                  bool expect_exception = false, Seconds grace_period = Seconds{600}) {
    ObjectStorageEngine objects;
    ObjectCoordinator coordinator;
    auto* pool = DbConnectionPool::get_connection_pool();
    state = {};
    state.configuration = configuration;
    state.pool = pool;
    state.objects = &objects;
    state.coordinator = &coordinator;
    state.recovering = true;
    state.expected_hash = object_hash;
    state.expected_hashes = expected_hashes;
    state.expected_grace = grace_period;

    ObjectGarbageCollector collector(*pool, objects, coordinator);
    bool result = false;
    bool caught = false;
    try {
        result = collector.recover_deleting(grace_period);
    } catch (const InjectedFailure&) {
        caught = true;
    }
    check(caught == expect_exception, name + ": recovery exception was swallowed or unexpected");
    if (!expect_exception)
        check(result == expected_result, name + ": unexpected recovery result");
    check(state.events == expected_events, name + ": recovery stage order or stop point changed");
    check(!state.cleanup_failed && state.created == state.returned &&
              state.created == state.destroyed && state.borrowed_connection == nullptr,
          name + ": recovery leaked a connection or released it in the wrong order");
    for (const auto& hash : expected_hashes) {
        check(available_from_another_thread(coordinator.mutex_for(hash)),
              name + ": recovery retained a candidate hash lock");
    }
}

void test_recovery_invalid_arguments() {
    for (const auto grace : {Seconds{0}, Seconds{-1}})
        run_recovery("invalid recovery grace period", {}, false, {}, {}, false, grace);
}

void test_recovery_listing() {
    // 空清单也必须实际查询成功，查询失败不能被当成空清单成功。
    run_recovery("empty recovery list", {}, true, recovery_events({}));

    Configuration configuration;
    configuration.deleting_result.success = false;
    run_recovery("failed recovery list", configuration, false, recovery_events({}));

    configuration = {};
    configuration.no_connection = true;
    run_recovery("missing list connection", configuration, false, {"list_borrow"});
}

void test_recovery_malformed() {
    const std::vector<DbRow> rows{
        DbRow{}, DbRow{std::nullopt}, DbRow{"invalid"}, DbRow{std::string(63, 'a')},
        DbRow{std::string(64, 'A')}, DbRow{object_hash, "extra"},
    };
    for (const auto& row : rows) {
        Configuration configuration;
        configuration.deleting_result = {true, {row}};
        run_recovery("malformed recovery row", configuration, false, recovery_events({}));
    }

    // 逐对象恢复中途遇到坏数据时，前面已经完成的回收保留。
    Configuration configuration;
    configuration.deleting_result = {true, {{object_hash}, {"invalid"}, {third_hash}}};
    run_recovery("malformed later recovery row", configuration, false,
                 recovery_events({collected_events}), {object_hash});
    check(state.finalized_hashes == std::vector<std::string>{object_hash},
          "completed recovery was lost or later candidates were processed");
}

void test_recovery_multiple() {
    Configuration configuration;
    configuration.deleting_result = {true, {{object_hash}, {second_hash}, {third_hash}}};
    configuration.prepare_overrides[second_hash] = ObjectGcPrepareResult::skipped;
    run_recovery("recover multiple candidates", configuration, true,
                 recovery_events({collected_events, preparation_stop_events, collected_events}),
                 {object_hash, second_hash, third_hash});
    check(state.removed_hashes == std::vector<std::string>{object_hash, third_hash} &&
              state.finalized_hashes == state.removed_hashes,
          "recovery removed a skipped object or missed a valid candidate");
}

void test_recovery_collection_failure() {
    Configuration configuration;
    configuration.deleting_result = {true, {{object_hash}}};
    configuration.no_connection_at = 2;
    run_recovery("missing object connection", configuration, false,
                 recovery_events({{"borrow"}}), {object_hash});

    configuration = {};
    configuration.deleting_result = {true, {{object_hash}, {second_hash}, {third_hash}}};
    configuration.prepare_overrides[second_hash] = ObjectGcPrepareResult::failed;
    run_recovery("preparation failure stops recovery", configuration, false,
                 recovery_events({collected_events, preparation_stop_events}),
                 {object_hash, second_hash, third_hash});
    check(state.finalized_hashes == std::vector<std::string>{object_hash},
          "recovery continued after a failed preparation");

    configuration.prepare_overrides.clear();
    configuration.finalize_overrides[second_hash] = ObjectGcFinalizeResult::failed;
    run_recovery("finalization failure stops recovery", configuration, false,
                 recovery_events({collected_events, collected_events}),
                 {object_hash, second_hash, third_hash});
    check(state.finalized_hashes == std::vector<std::string>{object_hash},
          "recovery reported failed finalization as completed");
}

void test_recovery_unknown() {
    // 准备或收尾提交未知都必须停止；未识别枚举值同样保守处理。
    for (const bool preparation : {true, false}) {
        for (const bool unrecognized : {false, true}) {
            Configuration configuration;
            configuration.deleting_result = {true, {{object_hash}, {second_hash}, {third_hash}}};
            if (preparation) {
                configuration.prepare_overrides[second_hash] = unrecognized
                    ? static_cast<ObjectGcPrepareResult>(99)
                    : ObjectGcPrepareResult::outcome_unknown;
            } else {
                configuration.finalize_overrides[second_hash] = unrecognized
                    ? static_cast<ObjectGcFinalizeResult>(99)
                    : ObjectGcFinalizeResult::outcome_unknown;
            }
            run_recovery("unknown outcome stops recovery", configuration, false,
                         recovery_events({collected_events,
                             preparation ? preparation_stop_events : collected_events}),
                         {object_hash, second_hash, third_hash});
            check(state.finalized_hashes == std::vector<std::string>{object_hash},
                  "unknown outcome was reported as completed or recovery continued");
            const std::vector<std::string> expected_removed = preparation
                ? std::vector<std::string>{object_hash}
                : std::vector<std::string>{object_hash, second_hash};
            check(state.removed_hashes == expected_removed,
                  "recovery removed an object after unknown preparation");
        }
    }
}

void test_recovery_exceptions() {
    Configuration configuration;
    configuration.throw_at = Stage::borrow;
    run_recovery("list borrow exception", configuration, false, {"list_borrow"}, {}, true);

    configuration.throw_at = Stage::listing;
    run_recovery("list query exception", configuration, false, recovery_events({}), {}, true);

    for (const auto stage : {Stage::borrow, Stage::prepare, Stage::remove, Stage::finalize}) {
        configuration = {};
        configuration.deleting_result = {true, {{object_hash}, {second_hash}, {third_hash}}};
        configuration.throw_at = stage;
        configuration.throw_hash = second_hash;
        std::vector<std::string> second_events{"borrow"};
        if (stage != Stage::borrow)
            second_events.push_back("prepare");
        if (stage == Stage::remove || stage == Stage::finalize)
            second_events.push_back("remove");
        if (stage == Stage::finalize)
            second_events.push_back("finalize");
        if (stage != Stage::borrow)
            second_events.push_back("return");
        run_recovery("object exception stops recovery", configuration, false,
                     recovery_events({collected_events, second_events}),
                     {object_hash, second_hash, third_hash}, true);
        check(state.finalized_hashes == std::vector<std::string>{object_hash},
              "exception discarded completed work or allowed later candidates");
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 2, "expected one scenario argument");
        const std::string scenario(argv[1]);
        if (scenario == "invalid_arguments")
            test_invalid_arguments();
        else if (scenario == "connection_failure")
            test_connection_failure();
        else if (scenario == "prepare_results")
            test_prepare_results();
        else if (scenario == "finalize_results")
            test_finalize_results();
        else if (scenario == "exceptions")
            test_exceptions();
        else if (scenario == "hash_lock")
            test_hash_lock();
        else if (scenario == "recovery_invalid_arguments")
            test_recovery_invalid_arguments();
        else if (scenario == "recovery_listing")
            test_recovery_listing();
        else if (scenario == "recovery_malformed")
            test_recovery_malformed();
        else if (scenario == "recovery_multiple")
            test_recovery_multiple();
        else if (scenario == "recovery_collection_failure")
            test_recovery_collection_failure();
        else if (scenario == "recovery_unknown")
            test_recovery_unknown();
        else if (scenario == "recovery_exceptions")
            test_recovery_exceptions();
        else
            throw std::invalid_argument("unknown scenario: " + scenario);
        std::cout << "PASS object garbage collector " << scenario << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
