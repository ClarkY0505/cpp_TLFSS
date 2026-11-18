#include "ftp/metadata_cache.h"

#include <array>
#include <chrono>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

using namespace TLSS::FileStorage;
using namespace std::chrono_literals;

void check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

enum class Kind { list, download, cd };
constexpr std::array<Kind, 3> kinds{Kind::list, Kind::download, Kind::cd};

MetadataCacheConfig config(bool disabled) {
  MetadataCacheConfig result;
  // 使用较长 TTL，保证测试中的失效来自版本变化，而非自然过期。
  result.list_ttl = result.download_ttl = result.cd_ttl = 1h;
  if (disabled)
    result.list_capacity = result.download_capacity = result.cd_capacity = 0;
  return result;
}

struct Fixture {
  MetadataCache cache;
  std::string payload = "old";
  int loads = 0;
  bool exists = true;
  int loader_exception = 0;

  explicit Fixture(bool disabled = false)
      : cache(config(disabled)) {}

  void begin_load() {
    ++loads;
    if (loader_exception == 1)
      throw std::runtime_error("loader exception");
    if (loader_exception == 2)
      throw 42;
  }

  // 使用真实缓存，替代的只有查询结果生产函数，不连接数据库。
  StorageContext<std::string> read(Kind kind, const std::string& directory = "/docs/a",
                                   const std::string& user = "alice") {
    if (kind == Kind::list) {
      auto result = cache.list(user, directory, [&] {
        begin_load();
        return StorageContext<MetadataCache::FileList>{
            exists, exists ? MetadataCache::FileList{{payload, "file", "c", "u"}}
                           : MetadataCache::FileList{}};
      });
      if (!result.success)
        return {false, {}};
      check(result.result.size() == 1, "unexpected directory listing");
      return {true, result.result.front().file_name};
    }
    auto loader = [&] {
      begin_load();
      return StorageContext<std::string>{exists, exists ? payload : std::string{}};
    };
    return kind == Kind::download ? cache.download(user, directory, "file.bin", loader)
                                  : cache.cd(user, directory, "child", loader);
  }

  MetadataCacheCounters counters(Kind kind) {
    const auto stats = cache.stats();
    if (kind == Kind::list)
      return stats.directory_list;
    return kind == Kind::download ? stats.download_hash : stats.cd_path;
  }

  // 暂由测试模拟子树写入的版本发布阶段。递归删除的公开缓存入口尚待接入。
  // 保持与生产设计相同的顺序：取得逻辑写权限，推进版本，再释放权限。
  void invalidate(const std::string& root, const std::string& user = "alice") {
    auto guard = cache._subtree_coordinator.acquire(
        {user, AccessMode::write, {{root, PathScopeKind::subtree}}});
    cache._subtree_versions.advance(user, root);
  }

  // 保存实际 ARC 条目的 shared_ptr，以证明禁用后没有偷偷替换缓存条目。
  // 仅此测试目标开放私有访问；生产头文件不增加测试接口。
  std::shared_ptr<const void> stored_value(Kind kind) {
    if (kind == Kind::list) {
      MetadataCacheDetail::Entry<MetadataCache::FileList> entry;
      check(cache._lists.arc.get(MetadataCache::make_key("alice", "/docs/a"), entry),
            "expected a stored directory entry");
      return entry.value;
    }
    MetadataCacheDetail::Entry<std::string> entry;
    auto& bank = kind == Kind::download ? cache._downloads : cache._cd_paths;
    const auto key =
        MetadataCache::make_key("alice", "/docs/a", kind == Kind::download ? "file.bin" : "child");
    check(bank.arc.get(key, entry), "expected a stored scalar entry");
    return entry.value;
  }
};

void expect(Fixture& fixture, Kind kind, const std::string& expected,
            const std::string& directory = "/docs/a", const std::string& user = "alice") {
  const auto result = fixture.read(kind, directory, user);
  check(result.success && result.result == expected, "unexpected cached or loaded value");
}

void test_nonzero(Kind kind) {
  Fixture fixture;
  fixture.invalidate("/docs");
  expect(fixture, kind, "old");
  fixture.payload = "must not load";
  expect(fixture, kind, "old");
  check(fixture.loads == 1 && fixture.counters(kind).hits == 1,
        "a nonzero-version entry was not reusable");
}

void test_invalidation(Kind kind, const std::string& root) {
  Fixture fixture;
  expect(fixture, kind, "old");
  fixture.payload = "fresh";
  fixture.invalidate(root);
  expect(fixture, kind, "fresh");
  expect(fixture, kind, "fresh");
  check(fixture.loads == 2, "invalidation did not reload exactly once");
}

void test_repeated(Kind kind) {
  Fixture fixture;
  expect(fixture, kind, "old");
  for (int revision = 1; revision <= 10; ++revision) {
    fixture.payload = "revision-" + std::to_string(revision);
    fixture.invalidate(revision % 2 == 0 ? "/docs" : "/docs/a");
    expect(fixture, kind, fixture.payload);
    expect(fixture, kind, fixture.payload);
  }
  check(fixture.loads == 11, "repeated invalidation lost a change or prevented cache hits");
}

void test_failed_load(Kind kind) {
  Fixture fixture;
  expect(fixture, kind, "old");
  fixture.exists = false;
  fixture.invalidate("/docs");
  check(!fixture.read(kind).success && !fixture.read(kind).success,
        "failed lookup reused the old cache entry");
  check(fixture.loads == 3, "failed results were cached");
  fixture.exists = true;
  fixture.payload = "recreated";
  expect(fixture, kind, "recreated");
  expect(fixture, kind, "recreated");
  check(fixture.loads == 4, "successful lookup after failure was not cached");
}

void test_directory_version(Kind kind) {
  Fixture fixture;
  fixture.invalidate("/docs");
  expect(fixture, kind, "old");
  // 子树版本相同，普通写入推进的精确目录版本仍必须使旧条目失效。
  check(fixture.cache.write("alice", "/docs/a",
                            [&] {
                              fixture.payload = "written";
                              return true;
                            }),
        "ordinary write failed");
  expect(fixture, kind, "written");
  expect(fixture, kind, "written");
  check(fixture.loads == 2, "subtree version check replaced the directory version check");
}

void test_disabled(Kind kind, bool overflow_boundary) {
  Fixture fixture;
  if (overflow_boundary) {
    fixture.cache._subtree_versions._sequence = std::numeric_limits<std::uint64_t>::max() - 1;
    fixture.invalidate("/docs");
    check(fixture.cache._subtree_versions.snapshot("alice", "/docs/a").enabled,
          "last representable version disabled the cache prematurely");
  }
  expect(fixture, kind, "old");
  const auto stored = fixture.stored_value(kind);
  if (!overflow_boundary)
    fixture.cache._subtree_versions._sequence = std::numeric_limits<std::uint64_t>::max();
  fixture.invalidate("/docs");
  check(!fixture.cache._subtree_versions.snapshot("alice", "/docs/a").enabled,
        "overflow did not disable versioned caching");
  fixture.payload = "fresh-1";
  expect(fixture, kind, "fresh-1");
  fixture.payload = "fresh-2";
  expect(fixture, kind, "fresh-2");
  const auto counters = fixture.counters(kind);
  check(fixture.loads == 3 && counters.hits == 0 && counters.admission_skips == 2,
        "disabled versioning reused or admitted a cache entry");
  check(fixture.stored_value(kind) == stored,
        "disabled versioning replaced the original ARC entry");
}

void test_collisions(Kind kind) {
  std::map<std::size_t, std::string> seen;
  std::string first, second;
  // 有限分片允许保守失效。动态选择碰撞路径，不依赖 std::hash 的具体结果。
  for (int index = 0; index <= 64; ++index) {
    const auto path = "/collision-" + std::to_string(index);
    const auto inserted = seen.emplace(SubtreeVersions::bucket_for("alice", path), path);
    if (!inserted.second) {
      first = inserted.first->second;
      second = path;
      break;
    }
  }
  check(!first.empty(), "could not select a version collision fixture");
  Fixture fixture;
  fixture.payload = "first";
  expect(fixture, kind, "first", first);
  fixture.payload = "second";
  expect(fixture, kind, "second", second);
  fixture.invalidate(first);
  fixture.payload = "first-fresh";
  expect(fixture, kind, "first-fresh", first);
  fixture.payload = "second-fresh";
  expect(fixture, kind, "second-fresh", second);
  expect(fixture, kind, "first-fresh", first);
  check(fixture.loads == 4, "version collision mixed values or prevented reuse");
}

// wait() 会释放协调器管理锁。观察真实等待队列，避免依赖固定睡眠判断是否阻塞。
bool wait_for_reader(Fixture& fixture, std::future<StorageContext<std::string>>& future) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline) {
    {
      auto& coordinator = fixture.cache._subtree_coordinator;
      std::lock_guard<std::mutex> lock(coordinator._mutex);
      if (!coordinator._waiting_requests.empty())
        return true;
    }
    if (future.wait_for(0ms) == std::future_status::ready)
      return false;
    std::this_thread::yield();
  }
  return false;
}

struct ReleaseOnExit {
  SubtreeCoordinator::Guard& guard;
  ~ReleaseOnExit() {
    guard.reset();
  }
};

void test_scope_wait(Kind kind, bool warm, bool disabled) {
  Fixture fixture(disabled);
  if (warm)
    expect(fixture, kind, "old");
  auto writer = fixture.cache._subtree_coordinator.acquire(
      {"alice", AccessMode::write, {{"/docs", PathScopeKind::subtree}}});
  std::future<StorageContext<std::string>> reader;
  // 异常清理时先释放写权限，再析构可能等待线程结束的 future。
  ReleaseOnExit release{writer};
  reader = std::async(std::launch::async, [&] {
    return fixture.read(kind);
  });
  check(wait_for_reader(fixture, reader), "metadata read bypassed subtree coordination");
  check(reader.wait_for(0ms) != std::future_status::ready,
        "metadata read completed while a conflicting subtree writer was active");

  // 模拟删除提交及版本发布。读线程必须在得到权限之后取得版本快照。
  fixture.payload = "after-writer";
  fixture.cache._subtree_versions.advance("alice", "/docs");
  writer.reset();
  check(reader.wait_for(3s) == std::future_status::ready, "metadata read did not resume");
  const auto result = reader.get();
  check(result.success && result.result == "after-writer", "reader returned a stale result");
  const int loaded = fixture.loads;
  expect(fixture, kind, "after-writer");
  check(fixture.loads == loaded + (disabled ? 1 : 0),
        "reader used a pre-wait version snapshot or ignored disabled caching");
}

void test_scope_independent(Kind kind) {
  for (bool other_user : {false, true}) {
    Fixture fixture;
    auto writer = fixture.cache._subtree_coordinator.acquire(
        {"alice", AccessMode::write, {{"/docs", PathScopeKind::subtree}}});
    std::future<StorageContext<std::string>> reader;
    ReleaseOnExit release{writer};
    reader = std::async(std::launch::async, [&] {
      return fixture.read(kind, other_user ? "/docs/a" : "/photos", other_user ? "bob" : "alice");
    });
    check(reader.wait_for(3s) == std::future_status::ready,
          "unrelated path or user was blocked by a subtree writer");
    check(reader.get().success, "independent metadata read failed");
  }
}

void test_exception_release(Kind kind) {
  for (int error : {1, 2}) {
    Fixture fixture;
    fixture.invalidate("/docs");
    fixture.loader_exception = error;
    bool caught = false;
    try {
      fixture.read(kind);
    } catch (const std::runtime_error&) {
      caught = error == 1;
    } catch (int code) {
      caught = error == 2 && code == 42;
    }
    check(caught, "loader exception was lost or changed");
    // 同一实例立即执行冲突写申请，验证异常路径释放了逻辑权限和目录锁。
    fixture.invalidate("/docs");
    fixture.loader_exception = 0;
    expect(fixture, kind, "old");
    expect(fixture, kind, "old");
    check(fixture.loads == 2, "successful read after exception was not cached");
  }
}

// 选择父目录与目标目录共用分片的输入，验证 write_subtree 的单锁分支。
std::string target_with_same_gate(Fixture& fixture) {
  auto& parent = fixture.cache.gate_for(MetadataCache::make_key("alice", "/docs"));
  for (int index = 0; index < 4096; ++index) {
    const auto target = "/docs/target-" + std::to_string(index);
    if (&fixture.cache.gate_for(MetadataCache::make_key("alice", target)) == &parent)
      return target;
  }
  throw std::runtime_error("could not select colliding parent and target gates");
}

std::string descendant_with_independent_gate(Fixture& fixture, const std::string& target) {
  auto& parent_gate = fixture.cache.gate_for(MetadataCache::make_key("alice", "/docs"));
  auto& target_gate = fixture.cache.gate_for(MetadataCache::make_key("alice", target));
  for (int index = 0; index < 4096; ++index) {
    const auto descendant = target + "/nested-" + std::to_string(index);
    auto& gate = fixture.cache.gate_for(MetadataCache::make_key("alice", descendant));
    if (&gate != &parent_gate && &gate != &target_gate)
      return descendant;
  }
  throw std::runtime_error("could not select an independent descendant gate");
}

void test_subtree_write(Kind kind, int outcome, bool collision = false) {
  Fixture fixture;
  const std::string target = collision ? target_with_same_gate(fixture) : "/docs/a";
  // 排除目录分片碰撞，否则精确目录版本可能掩盖子树版本没有推进的缺陷。
  const auto descendant = descendant_with_independent_gate(fixture, target);
  const std::array<std::string, 3> paths{"/docs", target, descendant};
  for (const auto& path : paths)
    expect(fixture, kind, "old", path);
  const auto before = fixture.cache._subtree_versions.snapshot("alice", descendant);
  int calls = 0;
  bool returned = false, caught = false, threw = false;
  try {
    returned = fixture.cache.write_subtree("alice", "/docs", target, [&] {
      ++calls;
      check(fixture.cache._subtree_versions.snapshot("alice", descendant).generation ==
                before.generation,
            "subtree version advanced before the business callback");
      fixture.payload = "after-write";
      if (outcome == 2)
        throw std::runtime_error("subtree writer failed");
      if (outcome == 3)
        throw 42;
      return outcome == 0;
    });
  } catch (const std::runtime_error& error) {
    // 不把测试断言抛出的 runtime_error 误当作预期业务异常。
    threw = true;
    caught = outcome == 2 && std::string(error.what()) == "subtree writer failed";
  } catch (int code) {
    threw = true;
    caught = outcome == 3 && code == 42;
  }
  check(calls == 1, "subtree writer was not invoked exactly once");
  check(outcome >= 2 ? (threw && caught) : (!threw && returned == (outcome == 0)),
        "subtree writer result or exception was not propagated");
  check(
      fixture.cache._subtree_versions.snapshot("alice", descendant).generation > before.generation,
      "subtree version did not advance on writer exit");
  for (const auto& path : paths)
    expect(fixture, kind, "after-write", path);
  for (const auto& path : paths)
    expect(fixture, kind, "after-write", path);
  check(fixture.loads == 6, "parent, target or descendant cache invalidation was incorrect");
  // 退出之后必须能再次申请父目录写权限，也必须已释放物理目录锁。
  check(fixture.cache.write("alice", "/docs",
                            [] {
                              return true;
                            }),
        "subtree exit retained a logical permission or directory lock");
}

struct MoveOnlyWriter {
  std::unique_ptr<int> owned = std::make_unique<int>(7);
  Fixture& fixture;
  explicit MoveOnlyWriter(Fixture& value)
      : fixture(value) {}
  MoveOnlyWriter(MoveOnlyWriter&&) = default;
  MoveOnlyWriter(const MoveOnlyWriter&) = delete;
  // 仅支持右值调用，验证 task 的引用捕获没有破坏原始 Writer 的转发语义。
  bool operator()() && {
    check(owned && *owned == 7, "move-only writer lost its resource");
    fixture.payload = "move-only";
    return true;
  }
};

void test_move_only_writer(Kind kind) {
  Fixture fixture;
  expect(fixture, kind, "old");
  check(fixture.cache.write_subtree("alice", "/docs", "/docs/a", MoveOnlyWriter(fixture)),
        "move-only writer failed");
  expect(fixture, kind, "move-only");
  expect(fixture, kind, "move-only");
  check(fixture.loads == 2, "move-only writer did not invalidate the target cache");
}

struct OpenOnExit {
  std::promise<void>& promise;
  bool opened = false;
  void open() {
    if (!opened) {
      opened = true;
      promise.set_value();
    }
  }
  ~OpenOnExit() {
    open();
  }
};

void test_real_writer_scope(Kind kind, bool parent) {
  Fixture fixture;
  const std::string directory = parent ? "/docs" : "/docs/a/nested";
  expect(fixture, kind, "old", directory);
  std::promise<void> entered, resume;
  auto entered_future = entered.get_future();
  const auto resume_future = resume.get_future().share();
  std::future<bool> writer;
  std::future<StorageContext<std::string>> reader;
  // 清理时先放行写回调，再等待异步任务析构，避免断言失败后自身死锁。
  OpenOnExit release{resume};
  writer = std::async(std::launch::async, [&] {
    return fixture.cache.write_subtree("alice", "/docs", "/docs/a", [&] {
      entered.set_value();
      resume_future.wait();
      fixture.payload = "committed";
      return true;
    });
  });
  check(entered_future.wait_for(3s) == std::future_status::ready,
        "subtree writer did not enter its callback");
  reader = std::async(std::launch::async, [&] {
    return fixture.read(kind, directory);
  });
  check(wait_for_reader(fixture, reader),
        "real subtree writer did not coordinate parent or descendant reads");
  release.open();
  check(writer.wait_for(3s) == std::future_status::ready && writer.get(),
        "subtree writer did not finish");
  check(reader.wait_for(3s) == std::future_status::ready, "waiting subtree reader did not finish");
  const auto result = reader.get();
  check(result.success && result.result == "committed", "reader observed a stale subtree value");
  expect(fixture, kind, "committed", directory);
  check(fixture.loads == 2, "post-writer cache refill did not use the published version");
}

void test_empty_list() {
  Fixture fixture;
  fixture.invalidate("/docs");
  int loads = 0;
  auto read = [&] {
    return fixture.cache.list("alice", "/docs/a", [&] {
      ++loads;
      return StorageContext<MetadataCache::FileList>{true, {}};
    });
  };
  check(read().success && read().result.empty() && loads == 1,
        "successful empty listing at a nonzero version was not cached");
  fixture.invalidate("/docs");
  check(read().success && read().result.empty() && loads == 2,
        "empty listing did not respect ancestor invalidation");
}

void run(const std::string& scenario, Kind kind) {
  if (scenario == "nonzero")
    test_nonzero(kind);
  else if (scenario == "self")
    test_invalidation(kind, "/docs/a");
  else if (scenario == "ancestor")
    test_invalidation(kind, "/docs");
  else if (scenario == "root")
    test_invalidation(kind, "/");
  else if (scenario == "repeated")
    test_repeated(kind);
  else if (scenario == "failed_load")
    test_failed_load(kind);
  else if (scenario == "directory_version")
    test_directory_version(kind);
  else if (scenario == "disabled_admission")
    test_disabled(kind, false);
  else if (scenario == "overflow_boundary")
    test_disabled(kind, true);
  else if (scenario == "collisions")
    test_collisions(kind);
  else if (scenario == "scope_hit")
    test_scope_wait(kind, true, false);
  else if (scenario == "scope_miss")
    test_scope_wait(kind, false, false);
  else if (scenario == "scope_disabled")
    test_scope_wait(kind, false, true);
  else if (scenario == "scope_independent")
    test_scope_independent(kind);
  else if (scenario == "exception_release")
    test_exception_release(kind);
  else if (scenario == "write_success")
    test_subtree_write(kind, 0);
  else if (scenario == "write_failure")
    test_subtree_write(kind, 1);
  else if (scenario == "write_exception")
    test_subtree_write(kind, 2);
  else if (scenario == "write_nonstandard_exception")
    test_subtree_write(kind, 3);
  else if (scenario == "write_collision")
    test_subtree_write(kind, 0, true);
  else if (scenario == "write_move_only")
    test_move_only_writer(kind);
  else if (scenario == "write_scope_parent")
    test_real_writer_scope(kind, true);
  else if (scenario == "write_scope_descendant")
    test_real_writer_scope(kind, false);
  else
    throw std::invalid_argument("unknown metadata subtree scenario: " + scenario);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    check(argc == 2, "expected a metadata subtree scenario");
    const std::string scenario = argv[1];
    if (scenario == "empty_list")
      test_empty_list();
    else
      for (auto kind : kinds)
        run(scenario, kind);
    std::cout << "PASS metadata subtree " << scenario << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
