#include "common/base/mysql_connection.h"
#include "ftp/object_gc_worker.h"
#include "ftp/transfer_logger.h"

#include <dlfcn.h>
#include <pthread.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "spdlog/sinks/base_sink.h"

namespace {

using namespace TLSS::BASE;
using namespace TLSS::FileStorage;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

const std::string first_hash(64, 'a');
const std::string second_hash(64, 'b');
const std::string third_hash(64, 'c');

std::atomic<bool> fail_next_thread{false};
std::atomic<int> thread_attempts{0};
std::atomic<int> logging_attempts{0};

enum class Outcome { success, failure, standard_exception, unknown_exception };

struct Step {
    std::string next_cursor;
    Outcome outcome = Outcome::success;
};

struct Call {
    std::string cursor;
    std::chrono::seconds grace_period;
    std::thread::id thread;
    Clock::time_point entered;
    Clock::time_point finished;
    bool scheduler_mutex_available = false;
};

// 真实调度线程执行这份可控批处理脚本，不访问 MySQL 或对象目录。
struct Script {
    std::mutex mutex;
    std::condition_variable changed;
    ObjectGarbageCollector* collector = nullptr;
    ObjectGcWorker* worker = nullptr;
    std::vector<Step> steps;
    std::vector<Call> calls;
    std::size_t block_call = 0;  // 从 1 开始计数；0 表示不阻塞。
    bool released = false;
    bool unexpected_call = false;
    int active = 0;
    int max_active = 0;
} script;

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void release_batch() {
    {
        std::lock_guard<std::mutex> lock(script.mutex);
        script.released = true;
    }
    script.changed.notify_all();
}

bool wait_calls(std::size_t count) {
    std::unique_lock<std::mutex> lock(script.mutex);
    return script.changed.wait_for(lock, 3s, [&] { return script.calls.size() >= count; });
}

// 只读检查受锁保护的停止标志，用来确定停止请求已发生，而不猜测线程调度顺序。
// try_to_lock 保证即使生产代码错误地跨批次持锁，测试也能超时报错而不永久阻塞。
bool wait_stop_requested(ObjectGcWorker& worker) {
    const auto deadline = Clock::now() + 3s;
    while (Clock::now() < deadline) {
        {
            std::unique_lock<std::mutex> lock(worker._mutex, std::try_to_lock);
            if (lock.owns_lock() && worker._stop_requested)
                return true;
        }
        std::this_thread::yield();
    }
    return false;
}

// packaged_task 的 future 析构不会隐式等待任务；发生断言异常时，
// 本对象先释放批处理闸门再 join，防止测试清理过程自身死锁。
struct Tasks {
    std::vector<std::thread> threads;

    template <typename Operation>
    auto launch(Operation operation) -> std::future<std::invoke_result_t<Operation>> {
        std::packaged_task<std::invoke_result_t<Operation>()> task(std::move(operation));
        auto result = task.get_future();
        threads.emplace_back(std::move(task));
        return result;
    }

    ~Tasks() {
        release_batch();
        for (auto& thread : threads) {
            if (thread.joinable())
                thread.join();
        }
    }
};

}  // namespace

// 与仓库现有 thread_pool_start_failure_test 一样，在测试进程内注入创建失败。
// BASE::Thread 及其底层 std::thread 仍使用真实实现。
extern "C" int pthread_create(pthread_t* thread, const pthread_attr_t* attributes,
                              void* (*routine)(void*), void* argument) {
    using CreateFunction = decltype(&pthread_create);
    static auto real_create = reinterpret_cast<CreateFunction>(dlsym(RTLD_NEXT, "pthread_create"));
    ++thread_attempts;
    if (fail_next_thread.exchange(false))
        return EAGAIN;
    if (!real_create)
        return ENOSYS;
    return real_create(thread, attributes, routine, argument);
}

namespace TLSS::BASE {

// collector 构造需要连接池引用，但调度测试不会借用连接；不读配置或启动池线程。
DbConnectionPool::DbConnectionPool()
    : _port(0), _init_size(0), _max_size(0), _max_idle_time(0), _connection_timeout(0),
      _connect_timeout(0), _read_timeout(0), _write_timeout(0),
      _produce_thread([] {}), _scanner_thread([] {}), _connection_cnt(0),
      _stopping(false), _threads_joined(true), _borrowed_count(0), _active_getters(0) {}

DbConnectionPool::~DbConnectionPool() = default;

DbConnectionPool* DbConnectionPool::get_connection_pool() {
    static DbConnectionPool pool;
    return &pool;
}

DbConnection::DbConnection()
    : _conn(nullptr), _connected(false), _transaction_active(false) {}
DbConnection::~DbConnection() = default;

}  // namespace TLSS::BASE

namespace TLSS::FileStorage {

ObjectGarbageCollector::ObjectGarbageCollector(
    BASE::DbConnectionPool& pool, ObjectStorageEngine& objects, ObjectCoordinator& coordinator)
    : _pool(pool), _objects(objects), _coordinator(coordinator) {}

bool ObjectGarbageCollector::collect_batch(std::string& cursor, std::chrono::seconds grace_period) {
    const auto entered = Clock::now();
    // 从其他线程探测，避免在持锁线程内再次 try_lock 同一把 std::mutex。
    bool mutex_available = false;
    std::thread observer([&] {
        mutex_available = script.worker->_mutex.try_lock();
        if (mutex_available)
            script.worker->_mutex.unlock();
    });
    observer.join();

    std::unique_lock<std::mutex> lock(script.mutex);
    const std::size_t index = script.calls.size();
    if (this != script.collector || index >= script.steps.size())
        script.unexpected_call = true;
    ++script.active;
    if (script.active > script.max_active)
        script.max_active = script.active;
    script.calls.push_back({cursor, grace_period, std::this_thread::get_id(),
                            entered, {}, mutex_available});
    script.changed.notify_all();

    if (index + 1 == script.block_call || index >= script.steps.size())
        script.changed.wait(lock, [] { return script.released; });

    const Step step = index < script.steps.size() ? script.steps[index] : Step{};
    cursor = step.next_cursor;  // 模拟批处理在失败或抛异常前已推进游标。
    script.calls[index].finished = Clock::now();
    --script.active;
    lock.unlock();
    script.changed.notify_all();

    if (step.outcome == Outcome::standard_exception)
        throw std::runtime_error("injected batch exception");
    if (step.outcome == Outcome::unknown_exception)
        throw 7;
    return step.outcome == Outcome::success;
}

}  // namespace TLSS::FileStorage

namespace {

struct Fixture {
    ObjectStorageEngine objects;
    ObjectCoordinator coordinator;
    ObjectGarbageCollector collector;
    std::unique_ptr<ObjectGcWorker> worker;

    Fixture() : collector(*DbConnectionPool::get_connection_pool(), objects, coordinator) {
        std::lock_guard<std::mutex> lock(script.mutex);
        script.collector = &collector;
        script.worker = nullptr;
        script.steps.clear();
        script.calls.clear();
        script.block_call = 0;
        script.released = false;
        script.unexpected_call = false;
        script.active = 0;
        script.max_active = 0;
    }

    void create(ObjectGcWorkerConfig configuration = {}) {
        worker = std::make_unique<ObjectGcWorker>(collector, configuration);
        script.worker = worker.get();
    }

    void plan(std::vector<Step> steps, std::size_t block_call) {
        std::lock_guard<std::mutex> lock(script.mutex);
        script.steps = std::move(steps);
        script.block_call = block_call;
    }

    ~Fixture() {
        release_batch();
        worker.reset();
    }
};

void check_calls(std::size_t expected, std::chrono::seconds grace_period) {
    std::lock_guard<std::mutex> lock(script.mutex);
    check(!script.unexpected_call && script.calls.size() == expected && script.active == 0,
          "unexpected batch call, overlap, or unfinished batch");
    check(script.max_active <= 1, "GC worker ran overlapping batches");
    for (const auto& call : script.calls) {
        check(call.grace_period == grace_period, "worker changed the configured grace period");
        check(call.scheduler_mutex_available, "worker held its state mutex during a batch");
        check(call.thread != std::this_thread::get_id(), "batch ran on the caller thread");
        check(call.thread == script.calls.front().thread, "batches used different worker threads");
    }
}

void finish_blocked_worker(ObjectGcWorker& worker) {
    Tasks tasks;
    auto stopped = tasks.launch([&] { worker.stop(); });
    const bool requested = wait_stop_requested(worker);
    const bool waiting = stopped.wait_for(0ms) == std::future_status::timeout;
    release_batch();
    const bool finished = stopped.wait_for(3s) == std::future_status::ready;
    stopped.get();
    check(requested && waiting && finished, "stop did not wait for the active batch correctly");
}

void test_configuration() {
    Fixture fixture;
    const int before = thread_attempts.load();
    for (const auto configuration : {ObjectGcWorkerConfig{0ms, 600s},
                                     ObjectGcWorkerConfig{-1ms, 600s},
                                     ObjectGcWorkerConfig{10ms, 0s},
                                     ObjectGcWorkerConfig{10ms, -1s}}) {
        bool rejected = false;
        try {
            fixture.create(configuration);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "worker accepted non-positive configuration");
    }
    check(thread_attempts.load() == before, "invalid configuration started a thread");
}

void test_lifecycle() {
    Fixture fixture;
    fixture.create();  // 默认等待 30 秒，停止应当立即唤醒而不是等待超时。
    const int before = thread_attempts.load();
    check(fixture.worker->start() && fixture.worker->start(), "idempotent start failed");
    check(thread_attempts.load() == before + 1, "repeated start created another thread");
    Tasks tasks;
    auto stopped = tasks.launch([&] { fixture.worker->stop(); });
    const bool prompt = stopped.wait_for(2s) == std::future_status::ready;
    // 若实现漏了 notify，补一次测试唤醒以便清理，并在下面报告失败。
    if (!prompt)
        fixture.worker->_wake.notify_all();
    stopped.get();
    check(prompt, "stop did not wake the interval wait promptly");
    fixture.worker->stop();
    check(!fixture.worker->start(), "worker restarted after stopping");
    check_calls(0, 600s);
}

void test_stop_before_start() {
    Fixture fixture;
    fixture.create();
    const int before = thread_attempts.load();
    fixture.worker->stop();
    fixture.worker->stop();
    check(!fixture.worker->start(), "stop-before-start was not terminal");
    check(thread_attempts.load() == before, "stop-before-start created a thread");
    check_calls(0, 600s);
}

void test_interval() {
    Fixture fixture;
    fixture.plan({{first_hash}}, 1);
    fixture.create({80ms, 123s});
    const auto before = Clock::now();
    check(fixture.worker->start(), "interval worker failed to start");
    // 停止标志为 false 的通知不能提前触发批处理。
    for (int count = 0; count < 20; ++count) {
        fixture.worker->_wake.notify_all();
        std::this_thread::sleep_for(1ms);
    }
    check(wait_calls(1), "interval did not schedule a batch");
    finish_blocked_worker(*fixture.worker);
    check_calls(1, 123s);
    check(script.calls[0].entered - before >= 79ms,
          "worker ran before its first interval or treated notification as timeout");
}

void verify_progress(bool failures) {
    Fixture fixture;
    if (failures) {
        fixture.plan({{first_hash, Outcome::failure}, {second_hash, Outcome::standard_exception},
                      {third_hash, Outcome::unknown_exception}, {"", Outcome::success}}, 4);
    } else {
        fixture.plan({{first_hash}, {second_hash}, {""}}, 3);
    }
    fixture.create({10ms, 123s});
    check(fixture.worker->start(), "progress worker failed to start");
    const std::size_t count = failures ? 4 : 3;
    check(wait_calls(count), "worker stopped after a failed batch or lost periodic scheduling");
    finish_blocked_worker(*fixture.worker);
    check_calls(count, 123s);
    const std::vector<std::string> expected = failures
        ? std::vector<std::string>{"", first_hash, second_hash, third_hash}
        : std::vector<std::string>{"", first_hash, second_hash};
    for (std::size_t index = 0; index < count; ++index) {
        check(script.calls[index].cursor == expected[index], "worker reset or lost its cursor");
        if (index != 0)
            check(script.calls[index].entered - script.calls[index - 1].finished >= 9ms,
                  "worker retried immediately without an interval wait");
    }
}

void test_creation_failure() {
    Fixture fixture;
    fixture.create();
    const int before = thread_attempts.load();
    fail_next_thread.store(true);
    const bool first_start = fixture.worker->start();
    fail_next_thread.store(false);
    check(!first_start, "worker reported success after thread creation failed");
    check(fixture.worker->start(), "worker could not retry after thread creation failure");
    check(thread_attempts.load() == before + 2, "thread creation retry count differs");
    fixture.worker->stop();
    check(!fixture.worker->start(), "worker restarted after successful stop");
    check_calls(0, 600s);
}

void test_destructor() {
    Fixture fixture;
    fixture.plan({{first_hash}}, 1);
    fixture.create({5ms, 600s});
    check(fixture.worker->start() && wait_calls(1), "destructor test did not enter a batch");
    std::promise<void> entered;
    auto entering = entered.get_future();
    Tasks tasks;
    auto destroyed = tasks.launch([&] {
        entered.set_value();
        fixture.worker.reset();
    });
    entering.get();
    const bool waiting = destroyed.wait_for(20ms) == std::future_status::timeout;
    release_batch();
    const bool finished = destroyed.wait_for(3s) == std::future_status::ready;
    destroyed.get();
    check(waiting && finished, "destructor did not wait for the active worker");
    check_calls(1, 600s);
}

void test_concurrent_stop() {
    Fixture fixture;
    fixture.plan({{first_hash}}, 1);
    fixture.create({5ms, 600s});
    check(fixture.worker->start() && wait_calls(1), "concurrent stop did not enter a batch");
    Tasks tasks;
    auto first_stop = tasks.launch([&] { fixture.worker->stop(); });
    const bool requested = wait_stop_requested(*fixture.worker);
    auto second_stop = tasks.launch([&] { fixture.worker->stop(); });
    auto restart = tasks.launch([&] { return fixture.worker->start(); });
    release_batch();
    first_stop.get();
    second_stop.get();
    const bool restarted = restart.get();
    check(requested && !restarted, "concurrent start/stop violated terminal stop semantics");
    check_calls(1, 600s);
}

class ThrowingSink final : public spdlog::sinks::base_sink<std::mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg&) override {
        ++logging_attempts;
        throw std::runtime_error("injected log failure");
    }
    void flush_() override {}
};

void test_logging_failure() {
    auto logger = Transfer::transfer_logger();
    logger->sinks().clear();
    logger->sinks().push_back(std::make_shared<ThrowingSink>());
    logger->set_level(spdlog::level::trace);
    logger->set_error_handler([](const std::string&) {
        throw std::runtime_error("injected log handler failure");
    });
    Fixture fixture;
    fixture.plan({{first_hash, Outcome::failure}, {"", Outcome::success}}, 2);
    fixture.create({5ms, 600s});
    check(fixture.worker->start() && wait_calls(2), "logging failure stopped the GC worker");
    finish_blocked_worker(*fixture.worker);
    check(logging_attempts.load() > 0, "logging failure was not injected");
    check_calls(2, 600s);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 2, "expected one scenario argument");
        const std::string scenario(argv[1]);
        if (scenario == "configuration")
            test_configuration();
        else if (scenario == "lifecycle")
            test_lifecycle();
        else if (scenario == "stop_before_start")
            test_stop_before_start();
        else if (scenario == "interval")
            test_interval();
        else if (scenario == "progress")
            verify_progress(false);
        else if (scenario == "failures")
            verify_progress(true);
        else if (scenario == "creation_failure")
            test_creation_failure();
        else if (scenario == "destructor")
            test_destructor();
        else if (scenario == "concurrent_stop")
            test_concurrent_stop();
        else if (scenario == "logging_failure")
            test_logging_failure();
        else
            throw std::invalid_argument("unknown scenario: " + scenario);
        std::cout << "PASS object GC worker " << scenario << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
