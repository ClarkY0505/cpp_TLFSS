#include "ftp/metadata_cache.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace TLSS::FileStorage;
using namespace std::chrono_literals;

void check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

// 仅用于选择确实碰撞或确实独立的测试输入，避免把允许的分片碰撞误判为全局锁。
std::size_t shard(const std::string& directory) {
  const std::string key = "5:alice" + std::to_string(directory.size()) + ":" + directory;
  return std::hash<std::string>{}(key) % 64;
}

std::vector<std::string> independent_directories(std::size_t count) {
  std::array<bool, 64> used{};
  std::vector<std::string> result;
  for (int index = 0; index < 4096 && result.size() < count; ++index) {
    auto directory = "/scope-" + std::to_string(index);
    if (!used[shard(directory)]) {
      used[shard(directory)] = true;
      result.push_back(std::move(directory));
    }
  }
  check(result.size() == count, "could not select independent cache scopes");
  return result;
}

std::vector<std::string> colliding_directories() {
  std::array<std::optional<std::string>, 64> seen;
  // 65 个输入落在 64 个分片内，至少有两个不同输入碰撞。
  for (int index = 0; index <= 64; ++index) {
    const auto directory = "/collision-" + std::to_string(index);
    auto& previous = seen[shard(directory)];
    if (previous)
      return {*previous, directory};
    previous = directory;
  }
  throw std::runtime_error("could not find cache scope collision");
}

struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, released = false;
  void wait() {
    std::unique_lock<std::mutex> lock(mutex);
    entered = true;
    changed.notify_all();
    changed.wait(lock, [&] {
      return released;
    });
  }
  bool wait_entered() {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, 3s, [&] {
      return entered;
    });
  }
  void release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    changed.notify_all();
  }
};

// packaged_task 的 future 析构不等待线程。断言失败时，本对象先放行闸门再 join，
// 避免测试清理自身死锁；真正的生产加锁死锁由 CTest 的超时负责报告。
struct Tasks {
  Gate& gate;
  std::vector<std::thread> threads;
  explicit Tasks(Gate& value)
      : gate(value) {}
  template <typename Operation>
  auto launch(Operation operation) -> std::future<std::invoke_result_t<Operation>> {
    std::packaged_task<std::invoke_result_t<Operation>()> task(std::move(operation));
    auto future = task.get_future();
    threads.emplace_back(std::move(task));
    return future;
  }
  ~Tasks() {
    gate.release();
    for (auto& thread : threads)
      if (thread.joinable())
        thread.join();
  }
};

enum class Kind { list, download, cd };
constexpr std::array<Kind, 3> kinds{Kind::list, Kind::download, Kind::cd};

MetadataCacheConfig config(bool disabled) {
  MetadataCacheConfig value;
  // 这些测试只观察版本和协调锁，不依赖自然 TTL 过期。
  value.list_ttl = value.download_ttl = value.cd_ttl = 1h;
  if (disabled)
    value.list_capacity = value.download_capacity = value.cd_capacity = 0;
  return value;
}

struct Fixture {
  MetadataCache cache;
  std::atomic<int> revision{0}, loads{0};
  std::function<void(const std::string&, Kind)> on_load;
  explicit Fixture(bool disabled = false)
      : cache(config(disabled)) {}
  static std::string token(const std::string& directory, int version) {
    return directory + "#" + std::to_string(version);
  }
  std::string load(const std::string& directory, Kind kind) {
    ++loads;
    const auto result = token(directory, revision.load());
    if (on_load)
      on_load(directory, kind);
    return result;
  }
  std::string read(const std::string& directory, Kind kind) {
    if (kind == Kind::list) {
      const auto result = cache.list("alice", directory, [&] {
        return StorageContext<MetadataCache::FileList>{true,
                                                       {{load(directory, kind), "file", "c", "u"}}};
      });
      check(result.success && result.result.size() == 1, "list cache lookup failed");
      return result.result[0].file_name;
    }
    const auto loader = [&] {
      return StorageContext<std::string>{true, load(directory, kind)};
    };
    const auto result = kind == Kind::download
                            ? cache.download("alice", directory, "file.bin", loader)
                            : cache.cd("alice", directory, "child", loader);
    check(result.success, "scalar cache lookup failed");
    return result.result;
  }
  void expect(const std::string& directory, int version) {
    for (const auto kind : kinds)
      check(read(directory, kind) == token(directory, version),
            "cache returned stale or mixed namespace data");
  }
};

void test_invalidation(const std::string& scenario) {
  Fixture fixture;
  auto paths = scenario == "collision" ? colliding_directories() : independent_directories(2);
  if (scenario == "same_directory")
    paths[1] = paths[0];
  const int scopes = paths[0] == paths[1] ? 1 : 2;
  for (const auto& path : paths)
    fixture.expect(path, 0);
  for (const auto& path : paths)
    fixture.expect(path, 0);
  check(fixture.loads == scopes * 3, "initial cache entries did not hit");

  int writes = 0;
  bool propagated = false;
  try {
    const bool result =
        fixture.cache.write_two_directories("alice", paths[0], paths[1], [&]() -> bool {
          ++writes;
          fixture.revision = 1;
          if (scenario == "exception")
            throw std::runtime_error("injected writer exception");
          if (scenario == "nonstandard_exception")
            throw 7;
          return scenario != "failure";
        });
    check(result == (scenario != "failure"), "pair write changed the callback result");
  } catch (const std::runtime_error& error) {
    propagated =
        scenario == "exception" && std::string(error.what()) == "injected writer exception";
    check(propagated, "pair write replaced an exception or failed unexpectedly");
  } catch (int value) {
    propagated = scenario == "nonstandard_exception" && value == 7;
    check(propagated, "pair write threw an unexpected nonstandard exception");
  }
  check(
      propagated == (scenario == "exception" || scenario == "nonstandard_exception") && writes == 1,
      "pair write swallowed an exception or invoked its callback more than once");
  for (const auto& path : paths)
    fixture.expect(path, 1);
  check(fixture.loads == scopes * 6,
        "pair write did not invalidate all three banks in both scopes");
  for (const auto& path : paths)
    fixture.expect(path, 1);
  check(fixture.loads == scopes * 6, "refreshed entries were not cached");
}

void test_readers() {
  Fixture fixture;
  const auto paths = independent_directories(2);
  for (const auto& path : paths)
    fixture.expect(path, 0);
  Gate gate;
  Tasks tasks(gate);
  auto writer = tasks.launch([&] {
    return fixture.cache.write_two_directories("alice", paths[0], paths[1], [&] {
      gate.wait();
      fixture.revision = 1;
      return true;
    });
  });
  check(gate.wait_entered(), "pair writer did not enter its callback");
  std::vector<std::future<std::string>> readers;
  std::vector<std::future<void>> started;
  for (const auto& path : paths) {
    for (const auto kind : kinds) {
      std::promise<void> signal;
      started.push_back(signal.get_future());
      readers.push_back(tasks.launch([&, path, kind, signal = std::move(signal)]() mutable {
        signal.set_value();
        return fixture.read(path, kind);
      }));
    }
  }
  bool blocked = true;
  for (auto& signal : started)
    blocked = (signal.wait_for(3s) == std::future_status::ready) && blocked;
  for (auto& reader : readers)
    blocked = (reader.wait_for(20ms) == std::future_status::timeout) && blocked;
  gate.release();
  check(writer.get() && blocked, "a list, download, or cd reader bypassed one of the two locks");
  for (std::size_t index = 0; index < readers.size(); ++index)
    check(readers[index].get() == Fixture::token(paths[index / 3], 1),
          "reader returned pre-write metadata");
}

void test_reverse_order() {
  Fixture fixture;
  const auto paths = independent_directories(2);
  Gate cleanup;
  Tasks tasks(cleanup);
  std::promise<void> start;
  auto ready = start.get_future().share();
  std::atomic<int> active{0}, writes{0};
  auto work = [&](bool reverse) {
    ready.wait();
    for (int index = 0; index < 150; ++index) {
      fixture.cache.write_two_directories(
          "alice", paths[reverse ? 1 : 0], paths[reverse ? 0 : 1], [&] {
            const int previous = active.fetch_add(1);
            ++writes;
            std::this_thread::yield();
            --active;
            check(previous == 0, "overlapping pairs executed their writers concurrently");
            return true;
          });
    }
  };
  auto first = tasks.launch([&] {
    work(false);
  });
  auto second = tasks.launch([&] {
    work(true);
  });
  start.set_value();
  first.get();
  second.get();
  check(writes == 300 && active == 0, "reversed pair writes were lost or left active");
}

void test_independent() {
  Fixture fixture;
  const auto paths = independent_directories(5);
  for (const auto& path : paths)
    fixture.expect(path, 0);
  Gate gate;
  Tasks tasks(gate);
  auto blocked = tasks.launch([&] {
    return fixture.cache.write_two_directories("alice", paths[0], paths[1], [&] {
      gate.wait();
      return true;
    });
  });
  check(gate.wait_entered(), "first pair did not start");
  auto independent = tasks.launch([&] {
    // 独立分片的读取和另一组双目录写入都应继续前进。
    fixture.expect(paths[2], 0);
    return fixture.cache.write_two_directories("alice", paths[2], paths[3], [] {
      return true;
    });
  });
  const bool progressed = independent.wait_for(1s) == std::future_status::ready;
  gate.release();
  check(blocked.get() && independent.get() && progressed, "pair writer blocked independent scopes");
  const int before = fixture.loads;
  fixture.expect(paths[4], 0);
  check(fixture.loads == before, "pair writes invalidated an unrelated scope");
  fixture.expect(paths[0], 0);
  fixture.expect(paths[1], 0);
  check(fixture.loads == before + 6, "first pair did not invalidate its own scopes");
}

void test_stale_fill() {
  Fixture fixture;
  const auto paths = independent_directories(2);
  fixture.expect(paths[1], 0);
  Gate gate;
  fixture.on_load = [&](const std::string& directory, Kind kind) {
    if (directory == paths[0] && kind == Kind::list && fixture.revision == 0)
      gate.wait();
  };
  Tasks tasks(gate);
  auto fill = tasks.launch([&] {
    return fixture.read(paths[0], Kind::list);
  });
  check(gate.wait_entered(), "cold cache fill did not start");
  std::promise<void> started;
  auto starting = started.get_future();
  auto writer = tasks.launch([&] {
    started.set_value();
    return fixture.cache.write_two_directories("alice", paths[0], paths[1], [&] {
      fixture.revision = 1;
      return true;
    });
  });
  const bool scheduled = starting.wait_for(3s) == std::future_status::ready;
  const bool waited = writer.wait_for(20ms) == std::future_status::timeout;
  gate.release();
  check(fill.get() == Fixture::token(paths[0], 0) && writer.get() && scheduled && waited,
        "pair write interleaved with unfinished cache fill");
  fixture.expect(paths[0], 1);
  fixture.expect(paths[1], 1);
}

void test_removed_directory() {
  MetadataCache cache(config(false));
  bool exists = true;
  int loads = 0;
  const auto list_parent = [&] {
    return cache.list("alice", "/parent", [&] {
      ++loads;
      return StorageContext<MetadataCache::FileList>{
          true, exists ? MetadataCache::FileList{{"child", "directory", "c", "u"}}
                       : MetadataCache::FileList{}};
    });
  };
  const auto list_child = [&] {
    return cache.list("alice", "/parent/child", [&] {
      ++loads;
      return StorageContext<MetadataCache::FileList>{exists, {}};
    });
  };
  const auto enter = [&] {
    return cache.cd("alice", "/parent", "child", [&] {
      ++loads;
      return StorageContext<std::string>{exists, exists ? "/parent/child" : ""};
    });
  };
  const auto leave = [&] {
    return cache.cd("alice", "/parent/child", "..", [&] {
      ++loads;
      return StorageContext<std::string>{exists, exists ? "/parent" : ""};
    });
  };
  check(list_parent().result.size() == 1 && list_child().success && enter().success &&
            leave().success,
        "initial directory metadata failed");
  list_parent();
  list_child();
  enter();
  leave();
  check(loads == 4, "directory metadata did not hit before removal");
  cache.write_two_directories("alice", "/parent", "/parent/child", [&] {
    exists = false;
    return true;
  });
  const auto parent = list_parent();
  check(parent.success && parent.result.empty() && !list_child().success && !enter().success &&
            !leave().success && loads == 8,
        "removed directory retained a cached empty listing or successful cd result");
  list_parent();
  check(loads == 8, "successful empty parent listing was not cached");
  list_child();
  enter();
  leave();
  check(loads == 11, "failed directory lookups were cached after removal");
}

void test_disabled() {
  Fixture fixture(true);
  const auto paths = independent_directories(2);
  for (const auto& path : paths)
    fixture.expect(path, 0);
  for (const auto& path : paths)
    fixture.expect(path, 0);
  check(fixture.loads == 12, "disabled banks unexpectedly cached metadata");
  int writes = 0;
  check(fixture.cache.write_two_directories("alice", paths[0], paths[1],
                                            [&] {
                                              ++writes;
                                              fixture.revision = 1;
                                              return true;
                                            }) &&
            writes == 1,
        "disabled caching prevented pair write");
  for (const auto& path : paths)
    fixture.expect(path, 1);
  check(fixture.loads == 18, "disabled cache did not reload new metadata");
}

void test_move_only() {
  Fixture fixture;
  for (const auto& paths : {independent_directories(2), colliding_directories()}) {
    int calls = 0;
    auto writer = [state = std::make_unique<int>(42), &calls]() mutable {
      ++calls;
      return *state == 42;
    };
    check(fixture.cache.write_two_directories("alice", paths[0], paths[1], std::move(writer)) &&
              calls == 1,
          "pair write copied or repeated its move-only callback");
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    check(argc == 2, "expected one scenario argument");
    const std::string scenario(argv[1]);
    if (scenario == "success" || scenario == "failure" || scenario == "exception" ||
        scenario == "nonstandard_exception" || scenario == "same_directory" ||
        scenario == "collision")
      test_invalidation(scenario);
    else if (scenario == "readers")
      test_readers();
    else if (scenario == "reverse_order")
      test_reverse_order();
    else if (scenario == "independent")
      test_independent();
    else if (scenario == "stale_fill")
      test_stale_fill();
    else if (scenario == "removed_directory")
      test_removed_directory();
    else if (scenario == "disabled")
      test_disabled();
    else if (scenario == "move_only")
      test_move_only();
    else
      throw std::invalid_argument("unknown scenario: " + scenario);
    std::cout << "PASS cache pair " << scenario << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
