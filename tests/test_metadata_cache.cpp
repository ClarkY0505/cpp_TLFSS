#include "common/base/mysql_connection.h"
#include "ftp/service_engine.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace TLSS::BASE;
using namespace TLSS::FileStorage;
using namespace std::chrono_literals;

// 只替换数据库访问边界，每个测试仍调用实际的 StorageEngine 公开方法。
// 因此缓存命中时，数据库连接借用次数和 SQL 查询次数都不应增加。
struct Database {
    std::atomic<int> borrows{0};
    std::atomic<int> queries{0};
    std::atomic<int> revision{0};
    std::atomic<bool> fail_query{false};
    std::atomic<bool> fail_write{false};
    std::atomic<bool> throw_write{false};
    std::atomic<bool> created_missing{false};
    std::function<void(const std::string&)> list_ready;
} database;

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string parameter(const std::vector<DbValue>& params, std::size_t index) {
    return params.at(index).value();
}

DbQueryResult query(const std::string& sql, const std::vector<DbValue>& params) {
    ++database.queries;
    if (database.fail_query.exchange(false)) {
        return {};
    }

    if (sql.find("select file_name") != std::string::npos) {
        const auto name = parameter(params, 1) + "#" +
                          std::to_string(database.revision.load());
        DbQueryResult result{true, {}};
        if (parameter(params, 1).find("/empty") == std::string::npos) {
            result.rows.push_back({name, "file", "created", "updated"});
        }
        if (database.list_ready) {
            database.list_ready(parameter(params, 1));
        }
        return result;
    }

    if (sql.find("SELECT child.path") != std::string::npos) {
        const auto name = parameter(params, 1);
        if (name == "missing" && !database.created_missing.load()) {
            return {true, {}};
        }
        const auto parent = parameter(params, 2);
        return {true, {{(parent == "/" ? parent : parent + "/") + name}}};
    }

    if (sql.find("SELECT parent.path") != std::string::npos) {
        const auto current = parameter(params, 1);
        const auto separator = current.rfind('/');
        return {true, {{separator == 0 ? "/" : current.substr(0, separator)}}};
    }

    if (sql.find("SELECT f.object_hash") != std::string::npos) {
        if (parameter(params, 2) == "missing") {
            return {true, {}};
        }
        const char digit = parameter(params, 0) == "bob" ? 'b' :
                           parameter(params, 2) == "other.txt" ? 'c' : 'a';
        return {true, {{std::string(64, digit)}}};
    }

    if (sql.find("SELECT p.id") != std::string::npos) {
        return {true, {{"42"}}};
    }

    if (sql.find("SELECT id FROM virtual_files") != std::string::npos) {
        const bool user_first = sql.find("WHERE username") != std::string::npos;
        return {true, {{parameter(params, user_first ? 0 : 1) + "@" +
                        parameter(params, user_first ? 1 : 0)}}};
    }

    throw std::runtime_error("unexpected metadata SQL: " + sql);
}

bool write() {
    ++database.revision;
    database.created_missing.store(true);
    // 模拟写入确认丢失：调用者收到失败或异常时，元数据仍可能已经改变，
    // 因此这种情况下也必须使旧缓存失效。
    if (database.throw_write.exchange(false)) {
        throw std::runtime_error("write acknowledgement lost");
    }
    return !database.fail_write.exchange(false);
}
}  // 匿名命名空间结束

// 沿用文件传输回归测试中的本地数据库替代方式。
// 不建立实际的 MySQL 连接，也不启动连接生产线程和空闲扫描线程。
namespace TLSS::BASE {
DbConnectionPool::DbConnectionPool()
    : _produce_thread([] {}), _scanner_thread([] {}),
      _connection_cnt(0), _stopping(false), _threads_joined(true),
      _borrowed_count(0), _active_getters(0) {}
DbConnectionPool::~DbConnectionPool() = default;
DbConnectionPool* DbConnectionPool::get_connection_pool() {
    static DbConnectionPool pool;
    return &pool;
}
DbConnectionHandle DbConnectionPool::get_connection() {
    ++database.borrows;
    return DbConnectionHandle(new DbConnection, DbConnectionDeleter{});
}
void DbConnectionDeleter::operator()(DbConnection* connection) const noexcept {
    delete connection;
}
DbConnection::DbConnection() : _conn(nullptr) {}
DbConnection::~DbConnection() = default;
DbQueryResult DbConnection::query_prepared(
    const std::string& sql, const std::vector<DbValue>& params) {
    return ::query(sql, params);
}
bool DbConnection::update_prepared(const std::string&, const std::vector<DbValue>&) {
    return write();
}
DbExecuteResult DbConnection::execute_prepared(
    const std::string&, const std::vector<DbValue>&) {
    const bool success = write();
    return {success, success ? 0U : 1U, success ? 1U : 0U, 1};
}
}  // TLSS::BASE 命名空间结束

namespace {
void test_list(StorageEngine& engine) {
    auto first = engine.find_file_by_current_dir("alice", "/");
    check(first.success && first.result.size() == 1, "initial listing failed");
    check(database.queries.load() == 2, "listing must initially execute two SELECTs");
    first.result.front().file_name = "local change";
    const auto second = engine.find_file_by_current_dir("alice", "/");
    check(second.success && second.result.front().file_name == "alice@/#0",
          "caller mutation changed the cached listing");
    check(database.queries.load() == 2 && database.borrows.load() == 1,
          "repeated listing still queried or borrowed a connection");

    const auto bob = engine.find_file_by_current_dir("bob", "/");
    const auto nested = engine.find_file_by_current_dir("alice", "/docs");
    check(bob.result.front().file_name == "bob@/#0", "listing users were mixed");
    check(nested.result.front().file_name == "alice@/docs#0", "listing paths were mixed");

    const auto empty = engine.find_file_by_current_dir("alice", "/empty");
    const int before = database.queries.load();
    const auto empty_again = engine.find_file_by_current_dir("alice", "/empty");
    check(empty.success && empty.result.empty() && empty_again.success,
          "empty directory was treated as a failure");
    check(database.queries.load() == before, "empty directory was not cached");
}

void test_cd(StorageEngine& engine) {
    check(engine.move_dir_by_current_dir("alice", "/", "docs").result == "/docs",
          "initial child lookup failed");
    const int before = database.queries.load();
    engine.move_dir_by_current_dir("alice", "/", "docs");
    check(database.queries.load() == before && database.borrows.load() == 1,
          "repeated cd still accessed the database");
    check(engine.move_dir_by_current_dir("alice", "/archive", "docs").result ==
              "/archive/docs", "cd parents were mixed");
    engine.move_dir_by_current_dir("bob", "/", "docs");
    check(database.queries.load() == before + 2, "cd users were not isolated");

    check(engine.move_dir_by_current_dir("alice", "/docs/sub", "..").result == "/docs",
          "parent lookup failed");
    const int parent_queries = database.queries.load();
    engine.move_dir_by_current_dir("alice", "/docs/sub", "..");
    engine.move_dir_by_current_dir("alice", "/", "..");
    check(database.queries.load() == parent_queries,
          "cached parent or root lookup queried the database");

    // 名称中的分隔符不能让不同字段组合发生键冲突。
    // 如果仅用 ':' 拼接字段，下面两次访问就会生成相同的键。
    check(engine.move_dir_by_current_dir("alice", "/b", "c:d").result == "/b/c:d",
          "colon-containing directory lookup failed");
    check(engine.move_dir_by_current_dir("alice", "/b:c", "d").result == "/b:c/d",
          "cache key fields were ambiguously concatenated");
}

void test_download(StorageEngine& engine) {
    check(engine.find_download_hash("alice", "/", "a.txt").result == std::string(64, 'a'),
          "initial hash lookup failed");
    engine.find_download_hash("alice", "/", "a.txt");
    check(database.queries.load() == 1 && database.borrows.load() == 1,
          "repeated download hash still accessed the database");
    check(engine.find_download_hash("bob", "/", "a.txt").result == std::string(64, 'b'),
          "download users were mixed");
    check(engine.find_download_hash("alice", "/", "other.txt").result == std::string(64, 'c'),
          "download filenames were mixed");
    engine.find_download_hash("alice", "/docs", "a.txt");
    check(database.queries.load() == 4, "download directory was omitted from the key");
}

void test_failures(StorageEngine& engine) {
    database.fail_query.store(true);
    check(!engine.find_file_by_current_dir("alice", "/").success, "SQL error was hidden");
    check(engine.find_file_by_current_dir("alice", "/").success, "SQL error was cached");
    const int before = database.queries.load();
    engine.move_dir_by_current_dir("alice", "/", "missing");
    engine.move_dir_by_current_dir("alice", "/", "missing");
    engine.find_download_hash("alice", "/", "missing");
    engine.find_download_hash("alice", "/", "missing");
    check(database.queries.load() == before + 4, "missing metadata was cached");
    check(engine.create_dir("alice", "/", "missing"), "mkdir failed");
    check(engine.move_dir_by_current_dir("alice", "/", "missing").success,
          "negative cd result hid the new directory");
}

void test_writes(StorageEngine& engine) {
    engine.find_file_by_current_dir("alice", "/");
    engine.find_download_hash("alice", "/", "a.txt");
    engine.move_dir_by_current_dir("alice", "/", "docs");
    check(engine.create_dir("alice", "/", "new"), "mkdir failed");
    check(engine.find_file_by_current_dir("alice", "/").result.front().file_name ==
              "alice@/#1", "mkdir did not invalidate listing");
    int before = database.queries.load();
    engine.find_download_hash("alice", "/", "a.txt");
    check(database.queries.load() == before + 1, "mkdir did not invalidate download hash");
    before = database.queries.load();
    engine.move_dir_by_current_dir("alice", "/", "docs");
    check(database.queries.load() == before, "mkdir invalidated hot cd paths");

    check(engine.commit_upload("alice", "42", "/", "new.txt", std::string(64, 'a')),
          "upload commit failed");
    check(engine.find_file_by_current_dir("alice", "/").result.front().file_name ==
              "alice@/#2", "upload did not invalidate listing");
    before = database.queries.load();
    engine.find_download_hash("alice", "/", "a.txt");
    check(database.queries.load() == before + 1, "upload did not invalidate download hash");
    before = database.queries.load();
    engine.move_dir_by_current_dir("alice", "/", "docs");
    check(database.queries.load() == before, "upload invalidated hot cd paths");

    before = database.queries.load();
    engine.prepare_upload("alice", "/", "another.txt");
    engine.prepare_upload("alice", "/", "another.txt");
    check(database.queries.load() == before + 2, "upload preflight must still query MySQL");
}

void test_failed_write(StorageEngine& engine) {
    engine.find_file_by_current_dir("alice", "/");
    database.fail_write.store(true);
    check(!engine.create_dir("alice", "/", "uncertain"), "write failure was hidden");
    check(engine.find_file_by_current_dir("alice", "/").result.front().file_name ==
              "alice@/#1", "uncertain write left a stale listing");
    database.throw_write.store(true);
    bool threw = false;
    try {
        engine.create_dir("alice", "/", "uncertain2");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "write exception was hidden");
    check(engine.find_file_by_current_dir("alice", "/").result.front().file_name ==
              "alice@/#2", "write exception left a stale listing");
}

void test_concurrent(StorageEngine& engine) {
    std::promise<void> start;
    auto ready = start.get_future().share();
    std::vector<std::future<StorageContext<std::vector<FileInfo>>>> readers;
    for (int i = 0; i < 8; ++i) {
        readers.push_back(std::async(std::launch::async, [&engine, ready] {
            ready.wait();
            return engine.find_file_by_current_dir("alice", "/");
        }));
    }
    start.set_value();
    for (auto& reader : readers) {
        check(reader.get().success, "concurrent read failed");
    }
    check(database.queries.load() == 2 && database.borrows.load() == 1,
          "concurrent cold reads issued duplicate database queries");
}

void test_stale_fill(StorageEngine& engine) {
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future().share();
    database.list_ready = [&](const std::string&) {
        entered.set_value();
        released.wait();
    };
    auto reader = std::async(std::launch::async, [&] {
        return engine.find_file_by_current_dir("alice", "/");
    });
    entered.get_future().wait();
    std::promise<void> writer_started;
    auto writer = std::async(std::launch::async, [&] {
        writer_started.set_value();
        return engine.create_dir("alice", "/", "new");
    });
    writer_started.get_future().wait();
    const bool writer_waited = writer.wait_for(20ms) == std::future_status::timeout;
    // 先释放读取线程再断言，避免断言失败后异步任务清理一直等待。
    release.set_value();
    check(reader.get().success && writer.get(), "concurrent operations failed");
    database.list_ready = {};
    check(writer_waited, "write interleaved with an unfinished cache fill");
    check(engine.find_file_by_current_dir("alice", "/").result.front().file_name ==
              "alice@/#1", "old query repopulated stale data after write");
}

std::string independent_directory() {
    // 只用于选择测试输入：按设计约定的长度前缀和六十四个协调分片，
    // 找到不与根目录冲突的目录，避免把合法的哈希碰撞误判为全局锁。
    const auto shard = [](const std::string& directory) {
        const std::string key = "5:alice" + std::to_string(directory.size()) + ":" + directory;
        return std::hash<std::string>{}(key) % 64;
    };
    for (int i = 0; i < 1024; ++i) {
        const auto directory = "/independent-" + std::to_string(i);
        if (shard(directory) != shard("/")) {
            return directory;
        }
    }
    throw std::runtime_error("无法找到独立协调分片的测试目录");
}

void test_local_invalidation(StorageEngine& engine) {
    const auto other = independent_directory();
    engine.find_file_by_current_dir("alice", "/");
    engine.find_file_by_current_dir("alice", other);
    engine.find_download_hash("alice", other, "a.txt");

    check(engine.create_dir("alice", "/", "new"), "建目录失败");
    const int before = database.queries.load();
    engine.find_file_by_current_dir("alice", other);
    engine.find_download_hash("alice", other, "a.txt");
    check(database.queries.load() == before, "写入使独立分片的缓存也失效了");
    check(engine.find_file_by_current_dir("alice", "/").result.front().file_name ==
              "alice@/#1", "写入目录自身的缓存没有失效");
}

void test_partition_concurrency(StorageEngine& engine) {
    const auto other = independent_directory();
    engine.move_dir_by_current_dir("alice", other, "docs");

    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future().share();
    database.list_ready = [&](const std::string& parent) {
        if (parent == "alice@/") {
            entered.set_value();
            released.wait();
        }
    };
    auto blocked = std::async(std::launch::async, [&] {
        return engine.find_file_by_current_dir("alice", "/");
    });
    entered.get_future().wait();
    auto independent = std::async(std::launch::async, [&] {
        const auto cd = engine.move_dir_by_current_dir("alice", other, "docs");
        const auto list = engine.find_file_by_current_dir("alice", other);
        return cd.success && list.success;
    });
    const bool progressed = independent.wait_for(1s) == std::future_status::ready;
    // 无论是否通过，都先释放阻塞任务，保证异步析构不会永久等待。
    release.set_value();
    const bool succeeded = blocked.get().success && independent.get();
    database.list_ready = {};
    check(succeeded, "分片并发操作失败");
    check(progressed, "一个目录的慢查询阻塞了独立分片的缓存命中和冷查询");
}

void test_ttl(StorageEngine& engine) {
    engine.find_file_by_current_dir("alice", "/");
    engine.find_download_hash("alice", "/", "a.txt");
    engine.move_dir_by_current_dir("alice", "/", "docs");
    const int before = database.queries.load();
    // 此场景的三类 TTL 均为一毫秒，等待后应重新执行两条列表 SQL 和两条标量 SQL。
    std::this_thread::sleep_for(20ms);
    engine.find_file_by_current_dir("alice", "/");
    engine.find_download_hash("alice", "/", "a.txt");
    engine.move_dir_by_current_dir("alice", "/", "docs");
    check(database.queries.load() == before + 4, "缓存过期后没有重新查询数据库");
}

void test_large_listing(StorageEngine& engine) {
    const auto first = engine.find_file_by_current_dir("alice", "/");
    const auto second = engine.find_file_by_current_dir("alice", "/");
    check(first.success && second.success, "大目录旁路不应导致业务失败");
    check(database.queries.load() == 4, "超过大小限制的目录仍被缓存");
    check(engine.metadata_cache_stats().directory_list.admission_skips == 2,
          "大目录跳过缓存的统计不正确");
}

void test_statistics(StorageEngine& engine) {
    engine.move_dir_by_current_dir("alice", "/", "docs");
    engine.move_dir_by_current_dir("alice", "/", "docs");
    const auto stats = engine.metadata_cache_stats();
    const auto& cd = stats.cd_path;
    check(cd.requests == 2 && cd.hits == 1 && cd.misses == 1 && cd.loads == 1 &&
              cd.coalesced == 0 && cd.admission_skips == 0, "缓存统计不正确");
    check(stats.directory_list.requests == 0 && stats.download_hash.requests == 0,
          "不同类型的缓存统计混在一起");
}

void test_disabled(StorageEngine& engine) {
    for (int i = 0; i < 2; ++i) {
        engine.find_file_by_current_dir("alice", "/");
        engine.find_download_hash("alice", "/", "a.txt");
        engine.move_dir_by_current_dir("alice", "/", "docs");
    }
    check(database.queries.load() == 8 && database.borrows.load() == 6,
          "零容量或零 TTL 没有关闭对应缓存");
}

void test_arc_invalidation() {
    TLSS::CACHE::ArcCache<std::string, int> cache(4, 2);
    cache.put("hot", 42);
    int value = 0;
    check(cache.get("hot", value) && value == 42, "ARC 晋升前读取失败");
    check(cache.get("hot", value), "LFU 读取失败");
    check(cache.erase("hot") && !cache.get("hot", value), "未清除已晋升条目");
    check(!cache.erase("hot"), "重复删除应返回未找到");
    cache.put("hot", 99);
    check(cache.get("hot", value) && value == 99, "删除后无法重新插入");

    TLSS::CACHE::ArcLru<std::string, int> lru(1, 2, 1);
    lru.put("old", 1);
    lru.put("new", 2);
    check(lru.invalidate("old") && !lru.check_ghost("old"), "未清除 LRU 历史节点");
    check(lru.invalidate("new"), "未清除 LRU 主节点");

    TLSS::CACHE::ArcLfu<std::string, int> lfu(2, 2, 2);
    lfu.put("a", 1);
    lfu.put("b", 2);
    lfu.get("a", value);
    check(lfu.invalidate("a") && lfu.get("b", value), "删除破坏了 LFU 频率桶");
    check(lfu.invalidate("b"), "LFU 最后一个条目删除失败");
    lfu.put("c", 3);
    lfu.put("d", 4);
    lfu.get("c", value);
    lfu.put("e", 5);
    check(lfu.invalidate("d") && !lfu.check_ghost("d"), "未清除 LFU 历史节点");

    TLSS::CACHE::HashArcCache<std::string, int> sharded(32, 4);
    sharded.put("key", 7);
    check(sharded.erase("key") && !sharded.get("key", value), "分片删除没有生效");
    TLSS::CACHE::HashArcCache<std::string, int> disabled(0, 4);
    check(!disabled.erase("key"), "零容量删除不应命中");
}

void test_shared(StorageEngine& engine, const std::shared_ptr<MetadataCache>& cache) {
    StorageEngine second(*DbConnectionPool::get_connection_pool(), cache);
    engine.find_file_by_current_dir("alice", "/");
    second.find_file_by_current_dir("alice", "/");
    check(database.queries.load() == 2, "显式共享缓存的引擎未复用查询结果");
    check(engine.create_dir("alice", "/", "new"), "共享缓存写入失败");
    check(second.find_file_by_current_dir("alice", "/").result.front().file_name ==
              "alice@/#1", "共享缓存的写后失效没有传播");
}

void test_config() {
    for (const auto ttl : {-1ms, std::chrono::milliseconds(25h)}) {
        MetadataCacheConfig config;
        config.cd_ttl = ttl;
        bool rejected = false;
        try {
            MetadataCache cache(config);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "非法缓存有效期没有被拒绝");
    }
}
}  // 匿名命名空间结束

int main(int argc, char** argv) {
    try {
        check(argc == 2, "expected one metadata cache scenario");
        const std::string scenario = argv[1];
        MetadataCacheConfig config;
        if (scenario == "ttl") {
            config.list_ttl = 1ms;
            config.download_ttl = 1ms;
            config.cd_ttl = 1ms;
        } else if (scenario == "large") {
            config.max_list_bytes = 1;
        } else if (scenario == "disabled") {
            config.list_capacity = 0;
            config.download_ttl = 0ms;
            config.cd_capacity = 0;
        }
        auto cache = std::make_shared<MetadataCache>(config);
        StorageEngine engine(*DbConnectionPool::get_connection_pool(), cache);
        if (scenario == "list") test_list(engine);
        else if (scenario == "cd") test_cd(engine);
        else if (scenario == "download") test_download(engine);
        else if (scenario == "failures") test_failures(engine);
        else if (scenario == "writes") test_writes(engine);
        else if (scenario == "failed_write") test_failed_write(engine);
        else if (scenario == "concurrent") test_concurrent(engine);
        else if (scenario == "stale_fill") test_stale_fill(engine);
        else if (scenario == "local_invalidation") test_local_invalidation(engine);
        else if (scenario == "partition_concurrency") test_partition_concurrency(engine);
        else if (scenario == "ttl") test_ttl(engine);
        else if (scenario == "large") test_large_listing(engine);
        else if (scenario == "statistics") test_statistics(engine);
        else if (scenario == "disabled") test_disabled(engine);
        else if (scenario == "arc") test_arc_invalidation();
        else if (scenario == "shared") test_shared(engine, cache);
        else if (scenario == "config") test_config();
        else throw std::runtime_error("unknown scenario: " + scenario);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
