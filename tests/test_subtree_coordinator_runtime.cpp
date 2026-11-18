#include "ftp/subtree_coordinator.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

// 只影响当前测试线程的一次分配故障，不干扰其他线程或生产程序。
// 逐个注入 acquire() 内的分配位置，验证入队/活动登记失败后的清理。
thread_local std::ptrdiff_t fail_allocation_after = -1;

}  // namespace

void* operator new(std::size_t size) {
  if (fail_allocation_after == 0) {
    fail_allocation_after = -1;
    throw std::bad_alloc();
  }
  if (fail_allocation_after > 0)
    --fail_allocation_after;
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

using namespace TLSS::FileStorage;
using namespace std::chrono_literals;
using Guard = SubtreeCoordinator::Guard;

static_assert(!std::is_copy_constructible_v<Guard> && !std::is_copy_assignable_v<Guard>);
static_assert(std::is_nothrow_move_constructible_v<Guard> &&
              std::is_nothrow_move_assignable_v<Guard>);
static_assert(std::is_nothrow_destructible_v<Guard>);
static_assert(!std::is_copy_constructible_v<SubtreeCoordinator>);

constexpr auto exact = PathScopeKind::exact;
constexpr auto subtree = PathScopeKind::subtree;

void check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

CoordinationRequest request(AccessMode access, const std::string& path, PathScopeKind kind = exact,
                            const std::string& user = "alice") {
  return {user, access, {{path, kind}}};
}

// 工作线程获准进入后，继续持有 Guard，直到主测试线程放行。
// 这使“已经进入”和“已经释放”成为两个可单独观察的事件。
struct Attempt {
  std::promise<void> entered_promise, finish_promise, done_promise;
  std::shared_future<void> entered = entered_promise.get_future().share();
  std::shared_future<void> finish = finish_promise.get_future().share();
  std::shared_future<void> done = done_promise.get_future().share();
  std::thread thread;
  bool finishing = false;

  void start(SubtreeCoordinator& coordinator, CoordinationRequest incoming) {
    thread = std::thread([this, &coordinator, incoming = std::move(incoming)]() mutable {
      bool admitted = false;
      try {
        {
          auto guard = coordinator.acquire(std::move(incoming));
          admitted = true;
          entered_promise.set_value();
          finish.wait();
        }  // done 只在 Guard 析构并释放之后发出。
        done_promise.set_value();
      } catch (...) {
        const auto error = std::current_exception();
        if (!admitted)
          entered_promise.set_exception(error);
        done_promise.set_exception(error);
      }
    });
  }

  bool has_entered() const {
    if (entered.wait_for(0ms) != std::future_status::ready)
      return false;
    entered.get();
    return true;
  }

  void await_entered() const {
    check(entered.wait_for(3s) == std::future_status::ready,
          "request did not enter before deadline");
    entered.get();
  }

  void allow_finish() {
    if (!finishing) {
      finishing = true;
      finish_promise.set_value();
    }
  }

  void await_done() const {
    check(done.wait_for(3s) == std::future_status::ready,
          "request did not release before deadline");
    done.get();
  }

  ~Attempt() {
    allow_finish();
    if (thread.joinable())
      thread.join();
  }
};

struct Harness {
  SubtreeCoordinator coordinator;
  std::vector<std::unique_ptr<Attempt>> attempts;

  Attempt& start(CoordinationRequest incoming) {
    auto attempt = std::make_unique<Attempt>();
    auto* pointer = attempt.get();
    // 先登记清理责任再启动线程，避免启动后的容器扩容异常遗留线程。
    attempts.push_back(std::move(attempt));
    pointer->start(coordinator, std::move(incoming));
    return *pointer;
  }

  void wait_counts(std::size_t active, std::size_t waiting) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do {
      // 仅本测试目标开放私有访问；不在生产类增加测试接口。
      // 读取队列时同样持有管理锁，不以任意 sleep 推测线程已经排队。
      std::unique_lock<std::mutex> lock(coordinator._mutex, std::try_to_lock);
      if (lock.owns_lock() && coordinator._active_requests.size() == active &&
          coordinator._waiting_requests.size() == waiting)
        return;
      if (lock.owns_lock())
        lock.unlock();
      std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("coordinator queue counts did not reach the expected state");
  }

  void finish(Attempt& attempt) {
    attempt.allow_finish();
    attempt.await_done();
  }

  ~Harness() {
    // 主测试函数的局部 Guard 必须在 Harness 之后声明，退出时会先释放。
    // 断言抛异常时先放行全部工作线程，再 join，避免测试清理自身死锁。
    for (auto& attempt : attempts)
      attempt->allow_finish();
    for (auto& attempt : attempts)
      if (attempt->thread.joinable())
        attempt->thread.join();
  }
};

void test_blocking(AccessMode held_mode, AccessMode incoming_mode) {
  Harness harness;
  auto held = harness.coordinator.acquire(request(held_mode, "/docs/a"));
  auto& incoming = harness.start(request(incoming_mode, "/docs/a"));
  harness.wait_counts(1, 1);
  check(!incoming.has_entered(), "conflicting request bypassed an active guard");
  held.reset();
  incoming.await_entered();
  harness.finish(incoming);
  harness.wait_counts(0, 0);
}

void test_parallel(const std::string& mode) {
  Harness harness;
  auto held_request = request(AccessMode::write, "/docs/a", subtree);
  auto incoming_request = request(AccessMode::write, "/docs/a", exact);
  if (mode == "readers") {
    held_request.access = incoming_request.access = AccessMode::read;
  } else if (mode == "users") {
    incoming_request.username = "bob";
  } else if (mode == "paths") {
    incoming_request.scopes[0].path = "/photos";
  } else if (mode == "exact_descendant") {
    held_request.scopes[0].kind = exact;
    incoming_request.scopes[0].path = "/docs/a/sub";
  } else if (mode == "subtree_ancestor") {
    held_request.scopes[0].path = "/docs/a/sub";
  } else if (mode == "boundary") {
    incoming_request.scopes[0] = {"/docs/ab", subtree};
  }
  auto held = harness.coordinator.acquire(held_request);
  auto& incoming = harness.start(incoming_request);
  incoming.await_entered();
  harness.wait_counts(2, 0);
  harness.finish(incoming);
  held.reset();
  harness.wait_counts(0, 0);
}

void test_subtree() {
  Harness harness;
  auto held = harness.coordinator.acquire(request(AccessMode::write, "/docs/a", subtree));
  auto& descendant = harness.start(request(AccessMode::read, "/docs/a/sub/deep"));
  harness.wait_counts(1, 1);
  check(!descendant.has_entered(), "subtree deletion did not protect a deep descendant");
  held.reset();
  descendant.await_entered();
  harness.finish(descendant);
}

void test_multiple_ranges() {
  Harness harness;
  auto held = harness.coordinator.acquire(
      {"alice", AccessMode::write, {{"/docs", exact}, {"/docs/a", subtree}}});
  auto& parent = harness.start(request(AccessMode::read, "/docs"));
  harness.wait_counts(1, 1);
  auto& descendant = harness.start(request(AccessMode::read, "/docs/a/sub"));
  harness.wait_counts(1, 2);
  auto& unrelated = harness.start(request(AccessMode::read, "/docs/other"));
  unrelated.await_entered();
  harness.wait_counts(2, 2);
  check(!parent.has_entered() && !descendant.has_entered(),
        "a multi-range request protected only its first scope");
  held.reset();
  // 两个读取互不冲突，一次释放必须唤醒全部可进入者；notify_one 会遗漏其中一个。
  parent.await_entered();
  descendant.await_entered();
  harness.finish(parent);
  harness.finish(descendant);
  harness.finish(unrelated);
  harness.wait_counts(0, 0);
}

void test_reverse_order() {
  Harness harness;
  auto held = harness.coordinator.acquire(request(AccessMode::read, "/a"));
  auto& first = harness.start({"alice", AccessMode::write, {{"/a", exact}, {"/b", exact}}});
  harness.wait_counts(1, 1);
  auto& second = harness.start({"alice", AccessMode::write, {{"/b", exact}, {"/a", exact}}});
  harness.wait_counts(1, 2);
  held.reset();
  first.await_entered();
  harness.wait_counts(1, 1);
  check(!second.has_entered(),
        "later reverse-order request bypassed the earlier conflicting request");
  harness.finish(first);
  second.await_entered();
  harness.finish(second);
  harness.wait_counts(0, 0);
}

void test_writer_priority() {
  Harness harness;
  auto held = harness.coordinator.acquire(request(AccessMode::read, "/docs/a"));
  auto& writer = harness.start(request(AccessMode::write, "/docs/a"));
  harness.wait_counts(1, 1);
  auto& later_reader = harness.start(request(AccessMode::read, "/docs/a"));
  harness.wait_counts(1, 2);
  check(!later_reader.has_entered(), "later reader overtook an already queued writer");
  held.reset();
  writer.await_entered();
  harness.wait_counts(1, 1);
  check(!later_reader.has_entered(), "reader entered while the earlier writer held its guard");
  harness.finish(writer);
  later_reader.await_entered();
  harness.finish(later_reader);
}

void test_unrelated_bypass() {
  Harness harness;
  auto held = harness.coordinator.acquire(request(AccessMode::read, "/docs/a"));
  auto& writer = harness.start(request(AccessMode::write, "/docs/a"));
  harness.wait_counts(1, 1);
  auto& another_path = harness.start(request(AccessMode::write, "/photos"));
  auto& another_user = harness.start(request(AccessMode::write, "/docs/a", exact, "bob"));
  another_path.await_entered();
  another_user.await_entered();
  harness.wait_counts(3, 1);
  check(!writer.has_entered(), "blocked writer entered before its reader finished");
  held.reset();
  writer.await_entered();
  harness.finish(writer);
  harness.finish(another_path);
  harness.finish(another_user);
}

void test_reader_identity() {
  Harness harness;
  auto first = harness.coordinator.acquire(request(AccessMode::read, "/docs/a"));
  auto second = harness.coordinator.acquire(request(AccessMode::read, "/docs/a"));
  auto& writer = harness.start(request(AccessMode::write, "/docs/a"));
  harness.wait_counts(2, 1);
  first.reset();
  harness.wait_counts(1, 1);
  check(!writer.has_entered(), "releasing one reader removed another identical reader's record");
  second.reset();
  writer.await_entered();
  harness.finish(writer);
}

void test_move_constructor() {
  Harness harness;
  auto original = harness.coordinator.acquire(request(AccessMode::write, "/docs/a"));
  Guard moved(std::move(original));
  original.reset();
  auto& waiter = harness.start(request(AccessMode::write, "/docs/a"));
  harness.wait_counts(1, 1);
  check(!waiter.has_entered(), "moved-from guard released the transferred ownership");
  moved.reset();
  waiter.await_entered();
  harness.finish(waiter);
}

void test_move_assignment() {
  Harness harness;
  auto first = harness.coordinator.acquire(request(AccessMode::write, "/a"));
  auto second = harness.coordinator.acquire(request(AccessMode::write, "/b"));
  auto& waiter_a = harness.start(request(AccessMode::write, "/a"));
  harness.wait_counts(2, 1);
  auto& waiter_b = harness.start(request(AccessMode::write, "/b"));
  harness.wait_counts(2, 2);
  first = std::move(second);
  waiter_a.await_entered();  // 原有 A 必须在赋值时释放。
  second.reset();
  harness.wait_counts(2, 1);
  check(!waiter_b.has_entered(), "move assignment failed to retain transferred scope B");
  first.reset();
  waiter_b.await_entered();
  harness.finish(waiter_a);
  harness.finish(waiter_b);
}

void test_self_move() {
  Harness harness;
  auto held = harness.coordinator.acquire(request(AccessMode::write, "/docs/a"));
  Guard& alias = held;
  held = std::move(alias);
  auto& waiter = harness.start(request(AccessMode::write, "/docs/a"));
  harness.wait_counts(1, 1);
  check(!waiter.has_entered(), "self move released its own active scope");
  held.reset();
  waiter.await_entered();
  harness.finish(waiter);
}

void test_empty_move() {
  Harness harness;
  Guard empty;
  empty.reset();
  empty.reset();
  Guard moved(std::move(empty));
  auto held = harness.coordinator.acquire(request(AccessMode::write, "/docs/a"));
  auto& waiter = harness.start(request(AccessMode::write, "/docs/a"));
  harness.wait_counts(1, 1);
  held = std::move(moved);  // 接管空凭证也必须先释放原有占用。
  waiter.await_entered();
  harness.finish(waiter);
  held.reset();
  empty.reset();
}

void test_destructor(bool unwind) {
  Harness harness;
  Attempt* waiter = nullptr;
  try {
    auto held = harness.coordinator.acquire(request(AccessMode::write, "/docs/a"));
    waiter = &harness.start(request(AccessMode::read, "/docs/a"));
    harness.wait_counts(1, 1);
    if (unwind)
      throw std::runtime_error("scope unwind");
  } catch (const std::runtime_error& error) {
    check(unwind && std::string(error.what()) == "scope unwind",
          "unexpected destructor test exception");
  }
  check(waiter != nullptr, "destructor test did not submit its waiter");
  waiter->await_entered();
  harness.finish(*waiter);
}

void test_cross_thread_release() {
  Harness harness;
  auto held = harness.coordinator.acquire(request(AccessMode::write, "/docs/a"));
  auto& waiter = harness.start(request(AccessMode::read, "/docs/a"));
  harness.wait_counts(1, 1);
  std::thread releaser([guard = std::move(held)]() mutable {
    guard.reset();
  });
  releaser.join();
  held.reset();
  waiter.await_entered();
  harness.finish(waiter);
}

void test_repeated_reset() {
  Harness harness;
  auto held = harness.coordinator.acquire(request(AccessMode::write, "/docs/a"));
  held.reset();
  held.reset();
  auto& writer = harness.start(request(AccessMode::write, "/docs/a"));
  writer.await_entered();
  auto& reader = harness.start(request(AccessMode::read, "/docs/a"));
  harness.wait_counts(1, 1);
  held.reset();
  check(!reader.has_entered(), "resetting an empty guard removed another request");
  harness.finish(writer);
  reader.await_entered();
  harness.finish(reader);
}

void test_reallocation() {
  Harness harness;
  std::vector<Guard> readers;
  // 故意让 Guard 容器和活动列表多次扩容，确认占用身份不依赖 vector 元素地址。
  for (int index = 0; index < 200; ++index)
    readers.push_back(harness.coordinator.acquire(request(AccessMode::read, "/docs/a")));
  auto& writer = harness.start(request(AccessMode::write, "/docs/a"));
  harness.wait_counts(200, 1);
  for (std::size_t index = 0; index + 1 < readers.size(); ++index)
    readers[index].reset();
  harness.wait_counts(1, 1);
  check(!writer.has_entered(), "vector growth or a partial release lost an active reader");
  readers.back().reset();
  writer.await_entered();
  harness.finish(writer);
}

void test_allocation_failures() {
  bool observed_failure = false, observed_success = false;
  for (std::ptrdiff_t index = 0; index < 64; ++index) {
    SubtreeCoordinator coordinator;
    auto incoming = request(AccessMode::write, "/");
    Guard held;
    bool succeeded = false;
    fail_allocation_after = index;
    try {
      held = coordinator.acquire(std::move(incoming));
      succeeded = true;
    } catch (const std::bad_alloc&) {
      observed_failure = true;
    } catch (...) {
      fail_allocation_after = -1;
      throw;
    }
    fail_allocation_after = -1;
    held.reset();
    {
      std::lock_guard<std::mutex> lock(coordinator._mutex);
      check(coordinator._active_requests.empty() && coordinator._waiting_requests.empty(),
            "allocation failure left an active or queued request behind");
    }
    // 失败后仍能正常使用同一个协调器，不能遗留一个永久挡住后续请求的排队记录。
    auto probe = coordinator.acquire(request(AccessMode::write, "/"));
    probe.reset();
    if (succeeded) {
      observed_success = true;
      break;
    }
  }
  check(observed_failure && observed_success,
        "allocation failure sweep did not exercise failure and recovery");
}

void test_stress() {
  Harness harness;
  std::array<std::atomic<int>, 3> in_scope{};
  for (auto& counter : in_scope)
    counter.store(0);
  std::atomic<bool> failed{false};
  std::atomic<int> completed{0};
  std::array<std::thread, 6> threads;
  try {
    for (std::size_t thread_index = 0; thread_index < threads.size(); ++thread_index) {
      threads[thread_index] = std::thread([&, thread_index] {
        try {
          for (int iteration = 0; iteration < 100; ++iteration) {
            const auto scope =
                (thread_index + static_cast<std::size_t>(iteration)) % in_scope.size();
            auto held = harness.coordinator.acquire(
                request(AccessMode::write, "/group" + std::to_string(scope)));
            if (in_scope[scope].fetch_add(1) != 0)
              failed.store(true);
            std::this_thread::yield();
            if (in_scope[scope].fetch_sub(1) != 1)
              failed.store(true);
            ++completed;
          }
        } catch (...) {
          failed.store(true);
        }
      });
    }
  } catch (...) {
    for (auto& thread : threads)
      if (thread.joinable())
        thread.join();
    throw;
  }
  for (auto& thread : threads)
    thread.join();
  check(!failed.load() && completed.load() == 600,
        "stress test admitted overlapping writers or lost requests");
  harness.wait_counts(0, 0);
}

void run(const std::string& mode) {
  if (mode == "writer_reader")
    test_blocking(AccessMode::write, AccessMode::read);
  else if (mode == "writer_writer")
    test_blocking(AccessMode::write, AccessMode::write);
  else if (mode == "reader_writer")
    test_blocking(AccessMode::read, AccessMode::write);
  else if (mode == "readers" || mode == "users" || mode == "paths" || mode == "exact_descendant" ||
           mode == "subtree_ancestor" || mode == "boundary")
    test_parallel(mode);
  else if (mode == "subtree")
    test_subtree();
  else if (mode == "multiple_ranges")
    test_multiple_ranges();
  else if (mode == "reverse_order")
    test_reverse_order();
  else if (mode == "writer_priority")
    test_writer_priority();
  else if (mode == "unrelated_bypass")
    test_unrelated_bypass();
  else if (mode == "reader_identity")
    test_reader_identity();
  else if (mode == "move_constructor")
    test_move_constructor();
  else if (mode == "move_assignment")
    test_move_assignment();
  else if (mode == "self_move")
    test_self_move();
  else if (mode == "empty_move")
    test_empty_move();
  else if (mode == "destructor")
    test_destructor(false);
  else if (mode == "unwind")
    test_destructor(true);
  else if (mode == "cross_thread_release")
    test_cross_thread_release();
  else if (mode == "repeated_reset")
    test_repeated_reset();
  else if (mode == "reallocation")
    test_reallocation();
  else if (mode == "allocation_failures")
    test_allocation_failures();
  else if (mode == "stress")
    test_stress();
  else
    throw std::invalid_argument("unknown subtree runtime scenario: " + mode);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    check(argc == 2, "expected one subtree runtime scenario");
    run(argv[1]);
    std::cout << "PASS subtree runtime " << argv[1] << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
