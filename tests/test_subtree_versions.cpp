#include "ftp/subtree_versions.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
thread_local bool count_allocations = false;
thread_local std::size_t allocation_count = 0;
}  // namespace

// 仅本测试进程替代 C++ 堆分配入口，检查 advance() 是否引入字符串拼接等分配。
void* operator new(std::size_t size) {
  if (count_allocations)
    ++allocation_count;
  if (void* memory = std::malloc(size == 0 ? 1 : size))
    return memory;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
  return ::operator new(size);
}
void operator delete(void* memory) noexcept {
  std::free(memory);
}
void operator delete[](void* memory) noexcept {
  std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
  std::free(memory);
}
void operator delete[](void* memory, std::size_t) noexcept {
  std::free(memory);
}

namespace {

using TLSS::FileStorage::SubtreeVersions;

void check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

void expect(SubtreeVersions& versions, const std::string& user, const std::string& path,
            std::uint64_t generation, bool enabled = true) {
  const auto snapshot = versions.snapshot(user, path);
  if (snapshot.enabled != enabled || snapshot.generation != generation) {
    throw std::runtime_error("snapshot for " + user + ":" + path + " expected generation " +
                             std::to_string(generation) + ", got " +
                             std::to_string(snapshot.generation));
  }
}

// 通过从目录向上寻找父目录枚举祖先，独立于生产代码从左向右扫描 '/' 的实现。
std::vector<std::string> ancestors(std::string path) {
  std::vector<std::string> result;
  for (;;) {
    result.push_back(path);
    if (path == "/")
      return result;
    const auto separator = path.rfind('/');
    path = separator == 0 ? "/" : path.substr(0, separator);
  }
}

std::set<std::size_t> buckets(const std::string& user, const std::string& path) {
  std::set<std::size_t> result;
  for (const auto& ancestor : ancestors(path))
    result.insert(SubtreeVersions::bucket_for(user, ancestor));
  return result;
}

std::string distinct_child(const std::string& user, const std::string& parent) {
  const auto used = buckets(user, parent);
  for (int index = 0; index < 4096; ++index) {
    const auto child = (parent == "/" ? parent : parent + "/") + "child_" + std::to_string(index);
    if (used.count(SubtreeVersions::bucket_for(user, child)) == 0)
      return child;
  }
  throw std::runtime_error("could not construct collision-free child fixture");
}

template <typename Work>
void parallel(std::size_t count, Work&& work) {
  std::vector<std::future<void>> tasks;
  tasks.reserve(count);
  std::promise<void> start_promise;
  const auto start = start_promise.get_future().share();
  struct Release {
    std::promise<void>& promise;
    bool opened = false;
    void open() {
      if (!opened) {
        opened = true;
        promise.set_value();
      }
    }
    ~Release() {
      open();
    }
  } release{start_promise};
  // 若线程创建抛异常，先解除起跑等待，再析构 future，避免测试清理死锁。
  for (std::size_t index = 0; index < count; ++index)
    tasks.push_back(std::async(std::launch::async, [&, start, index] {
      start.wait();
      work(index);
    }));
  release.open();
  for (auto& task : tasks)
    task.get();
}

void test_initial() {
  SubtreeVersions versions;
  for (const auto& path : std::vector<std::string>{"/", "/docs", "/docs/a/sub", "/资料/目录"})
    expect(versions, "alice", path, 0);
}

void test_self() {
  SubtreeVersions versions;
  for (std::uint64_t generation = 1; generation <= 20; ++generation) {
    versions.advance("alice", "/docs/a");
    expect(versions, "alice", "/docs/a", generation);
  }
}

void test_root() {
  SubtreeVersions versions;
  const auto child = distinct_child("alice", "/");
  versions.advance("alice", "/");
  expect(versions, "alice", "/", 1);
  expect(versions, "alice", child, 1);
  expect(versions, "alice", child + "/deep", 1);
}

void test_ancestors() {
  const std::string user = "alice";
  const auto child = distinct_child(user, "/docs/a");
  // 每一轮只更新一个祖先，避免更深的版本更新掩盖遗漏祖先的问题。
  for (const auto& ancestor : ancestors(child)) {
    SubtreeVersions versions;
    versions.advance(user, ancestor);
    expect(versions, user, child, 1);
  }
}

void test_global_sequence() {
  SubtreeVersions versions;
  const auto child = distinct_child("alice", "/docs");
  for (int index = 0; index < 10; ++index)
    versions.advance("alice", "/docs");
  expect(versions, "alice", child, 10);
  versions.advance("alice", child);
  // 不能只把目标分片从 0 加到 1，否则祖先的 10 会遮住这次变更。
  expect(versions, "alice", child, 11);
}

void test_ancestor_after_descendant() {
  SubtreeVersions versions;
  const auto child = distinct_child("alice", "/docs/a");
  versions.advance("alice", child);
  expect(versions, "alice", child, 1);
  versions.advance("alice", "/docs/a");
  expect(versions, "alice", child, 2);
}

void test_descendant_isolation() {
  SubtreeVersions versions;
  const auto child = distinct_child("alice", "/docs/a");
  versions.advance("alice", child);
  // 后代变更不向上失效；本例已排除哈希碰撞。
  expect(versions, "alice", "/docs/a", 0);
  expect(versions, "alice", child, 1);
}

void test_independent_namespaces() {
  const auto changed = SubtreeVersions::bucket_for("alice", "/docs/a");
  for (const auto& user : std::vector<std::string>{"alice", "bob"}) {
    std::string observed;
    for (int index = 0; index < 4096; ++index) {
      const auto candidate = "/photos_" + std::to_string(index);
      if (buckets(user, candidate).count(changed) == 0) {
        observed = candidate;
        break;
      }
    }
    check(!observed.empty(), "could not construct independent namespace fixture");
    SubtreeVersions versions;
    versions.advance("alice", "/docs/a");
    expect(versions, user, observed, 0);
  }
}

void test_collisions() {
  std::map<std::size_t, std::string> first_path;
  std::string first, second;
  // 65 个路径映射到 64 个分片，必定能找到一组碰撞。
  for (int index = 0; index <= 64; ++index) {
    const auto path = "/prefix_" + std::to_string(index);
    const auto inserted = first_path.emplace(SubtreeVersions::bucket_for("alice", path), path);
    if (!inserted.second) {
      first = inserted.first->second;
      second = path;
      break;
    }
  }
  check(!first.empty() && first != second, "collision fixture was not found");
  SubtreeVersions versions;
  versions.advance("alice", first);
  // 碰撞允许额外失效；仍必须使用最新序号，不能漏掉真实失效。
  expect(versions, "alice", second, 1);
  versions.advance("alice", second);
  expect(versions, "alice", first, 2);
}

void test_overflow(bool disabled_advance) {
  SubtreeVersions versions;
  // 仅测试目标开放私有访问，直接构造边界，不需要执行 2^64 次操作。
  versions._sequence = std::numeric_limits<std::uint64_t>::max() - 1;
  versions.advance("alice", "/a");
  expect(versions, "alice", "/a", std::numeric_limits<std::uint64_t>::max());
  versions.advance("alice", "/b");
  expect(versions, "alice", "/a", 0, false);
  expect(versions, "bob", "/photos", 0, false);
  if (disabled_advance) {
    const auto before = versions._versions;
    for (int index = 0; index < 10; ++index)
      versions.advance("alice", "/after_disabled");
    check(versions._sequence == std::numeric_limits<std::uint64_t>::max() &&
              versions._versions == before,
          "disabled version table changed or wrapped its sequence");
    expect(versions, "alice", "/after_disabled", 0, false);
  }
}

void test_no_allocation() {
  SubtreeVersions versions;
  const std::string user(32, 'u');
  const std::string root = "/docs/" + std::string(64, 'd');
  allocation_count = 0;
  count_allocations = true;
  for (int index = 0; index < 100; ++index)
    versions.advance(user, root);
  count_allocations = false;
  check(allocation_count == 0, "advance allocated C++ heap memory");
  expect(versions, user, root, 100);
}

void test_concurrent_updates() {
  SubtreeVersions versions;
  const std::string user = "alice", root = "/docs/a";
  parallel(4, [&](std::size_t) {
    for (int index = 0; index < 200; ++index)
      versions.advance(user, root);
  });
  expect(versions, user, root, 800);
}

void test_snapshot_monotonic() {
  SubtreeVersions versions;
  const std::string user = "alice", root = "/docs/a";
  std::atomic<bool> invalid{false};
  parallel(4, [&](std::size_t worker) {
    if (worker < 2) {
      for (int index = 0; index < 400; ++index) {
        versions.advance(user, root);
        std::this_thread::yield();
      }
    } else {
      std::uint64_t previous = 0;
      for (int index = 0; index < 1000; ++index) {
        const auto snapshot = versions.snapshot(user, root);
        if (!snapshot.enabled || snapshot.generation < previous)
          invalid.store(true);
        previous = snapshot.generation;
        std::this_thread::yield();
      }
    }
  });
  check(!invalid.load(), "concurrent snapshot moved backwards or became disabled");
  expect(versions, user, root, 800);
}

void test_instances() {
  SubtreeVersions first, second;
  first.advance("alice", "/docs/a");
  expect(first, "alice", "/docs/a", 1);
  expect(second, "alice", "/docs/a", 0);
}

void run(const std::string& mode) {
  if (mode == "initial")
    test_initial();
  else if (mode == "self")
    test_self();
  else if (mode == "root")
    test_root();
  else if (mode == "ancestors")
    test_ancestors();
  else if (mode == "global_sequence")
    test_global_sequence();
  else if (mode == "ancestor_after_descendant")
    test_ancestor_after_descendant();
  else if (mode == "descendant_isolation")
    test_descendant_isolation();
  else if (mode == "independent_namespaces")
    test_independent_namespaces();
  else if (mode == "collisions")
    test_collisions();
  else if (mode == "overflow")
    test_overflow(false);
  else if (mode == "disabled_advance")
    test_overflow(true);
  else if (mode == "no_allocation")
    test_no_allocation();
  else if (mode == "concurrent_updates")
    test_concurrent_updates();
  else if (mode == "snapshot_monotonic")
    test_snapshot_monotonic();
  else if (mode == "instances")
    test_instances();
  else
    throw std::invalid_argument("unknown subtree version scenario: " + mode);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    check(argc == 2, "expected one subtree version scenario");
    run(argv[1]);
    std::cout << "PASS subtree versions " << argv[1] << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
