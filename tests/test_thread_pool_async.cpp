#include "common/base/tlss_thread_pool.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>

namespace {
using namespace std::chrono_literals;
using TLSS::BASE::ThreadPool;

void require(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "thread pool async test failed: %s\n", message);
    std::abort();
  }
}

void test_immediate_submission_executes_without_timeout() {
  ThreadPool pool("immediate");
  pool.set_max_queue_size(1);
  pool.set_max_wait_queue_size(1);
  pool.start(1);

  std::promise<void> executed;
  auto executed_future = executed.get_future();
  std::atomic<int> timeouts{0};
  const bool accepted = pool.submit_async(
      [&] { executed.set_value(); }, std::chrono::steady_clock::now() + 2s,
      [&] { ++timeouts; });

  require(accepted, "immediate task was rejected");
  require(executed_future.wait_for(2s) == std::future_status::ready,
          "immediate task did not execute");
  pool.stop();
  require(timeouts == 0, "executed task also timed out");
}

void test_full_queue_accepts_waiter_without_blocking_and_promotes_it() {
  ThreadPool pool("promote");
  pool.set_max_queue_size(1);
  pool.set_max_wait_queue_size(1);
  pool.start(1);

  std::promise<void> worker_started;
  auto worker_started_future = worker_started.get_future();
  std::promise<void> release_worker;
  auto release_future = release_worker.get_future().share();
  require(pool.try_run([&] {
    worker_started.set_value();
    release_future.wait();
  }), "blocking worker task was rejected");
  require(worker_started_future.wait_for(2s) == std::future_status::ready,
          "worker did not start");

  std::atomic<int> filler_runs{0};
  std::atomic<int> waiter_runs{0};
  std::atomic<int> timeouts{0};
  std::promise<void> waiter_finished;
  auto waiter_finished_future = waiter_finished.get_future();
  require(pool.try_run([&] { ++filler_runs; }), "queue filler was rejected");

  auto submission = std::async(std::launch::async, [&] {
    return pool.submit_async([&] {
                               ++waiter_runs;
                               waiter_finished.set_value();
                             },
                             std::chrono::steady_clock::now() + 2s,
                             [&] { ++timeouts; });
  });
  const bool returned_while_full =
      submission.wait_for(500ms) == std::future_status::ready;
  release_worker.set_value();
  require(returned_while_full, "submit_async blocked on a full queue");
  require(submission.get(), "waiter was rejected despite available wait capacity");
  require(waiter_finished_future.wait_for(2s) == std::future_status::ready,
          "waiter did not execute after queue freed");
  pool.stop();
  require(filler_runs == 1, "queued task did not execute");
  require(waiter_runs == 1, "waiter did not execute after queue freed");
  require(timeouts == 0, "promoted waiter timed out");
}

void test_waiter_timeout_does_not_execute_task() {
  ThreadPool pool("timeout");
  pool.set_max_queue_size(1);
  pool.set_max_wait_queue_size(1);
  pool.start(1);

  std::promise<void> worker_started;
  auto worker_started_future = worker_started.get_future();
  std::promise<void> release_worker;
  auto release_future = release_worker.get_future().share();
  require(pool.try_run([&] {
    worker_started.set_value();
    release_future.wait();
  }), "blocking worker task was rejected");
  require(worker_started_future.wait_for(2s) == std::future_status::ready,
          "worker did not start");
  require(pool.try_run([] {}), "queue filler was rejected");

  std::atomic<int> waiter_runs{0};
  std::atomic<int> timeout_calls{0};
  std::promise<void> timeout_called;
  auto timeout_future = timeout_called.get_future();
  require(pool.submit_async([&] { ++waiter_runs; },
                            std::chrono::steady_clock::now() + 100ms,
                            [&] {
                              ++timeout_calls;
                              require(pool.queue_size() == 1,
                                      "timeout callback ran with the queue lock held");
                              timeout_called.set_value();
                            }), "waiter was rejected");

  const bool timed_out = timeout_future.wait_for(2s) == std::future_status::ready;
  release_worker.set_value();
  require(timed_out, "waiter timeout callback did not run");
  pool.stop();
  require(timeout_calls == 1, "timeout callback did not run exactly once");
  require(waiter_runs == 0, "timed out task executed");
}

void test_wait_queue_capacity_rejects_excess_task() {
  ThreadPool pool("capacity");
  pool.set_max_queue_size(1);
  pool.set_max_wait_queue_size(1);
  pool.start(1);

  std::promise<void> worker_started;
  auto worker_started_future = worker_started.get_future();
  std::promise<void> release_worker;
  auto release_future = release_worker.get_future().share();
  require(pool.try_run([&] {
    worker_started.set_value();
    release_future.wait();
  }), "blocking worker task was rejected");
  require(worker_started_future.wait_for(2s) == std::future_status::ready,
          "worker did not start");
  require(pool.try_run([] {}), "queue filler was rejected");

  std::atomic<int> first_runs{0};
  std::atomic<int> second_runs{0};
  std::promise<void> first_finished;
  auto first_finished_future = first_finished.get_future();
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  const bool first_accepted = pool.submit_async([&] {
    ++first_runs;
    first_finished.set_value();
  }, deadline, [] {});
  const bool second_accepted = pool.submit_async([&] { ++second_runs; }, deadline, [] {});
  release_worker.set_value();
  require(first_accepted, "first waiter was rejected");
  require(!second_accepted, "wait queue admitted task beyond capacity");
  require(first_finished_future.wait_for(2s) == std::future_status::ready,
          "first waiter did not execute");
  pool.stop();
  require(first_runs == 1 && second_runs == 0,
          "wait queue capacity did not control task execution");
}

void test_new_earlier_deadline_wakes_timeout_thread() {
  ThreadPool pool("earlier-deadline");
  pool.set_max_queue_size(1);
  pool.set_max_wait_queue_size(2);
  pool.start(1);

  std::promise<void> worker_started;
  auto worker_started_future = worker_started.get_future();
  std::promise<void> release_worker;
  auto release_future = release_worker.get_future().share();
  require(pool.try_run([&] {
    worker_started.set_value();
    release_future.wait();
  }), "blocking worker task was rejected");
  require(worker_started_future.wait_for(2s) == std::future_status::ready,
          "worker did not start");
  require(pool.try_run([] {}), "queue filler was rejected");

  std::promise<void> long_waiter_finished;
  auto long_waiter_future = long_waiter_finished.get_future();
  std::atomic<int> long_timeouts{0};
  require(pool.submit_async([&] { long_waiter_finished.set_value(); },
                            std::chrono::steady_clock::now() + 2s,
                            [&] { ++long_timeouts; }),
          "long waiter was rejected");

  std::promise<void> short_timeout;
  auto short_timeout_future = short_timeout.get_future();
  std::atomic<int> short_runs{0};
  require(pool.submit_async([&] { ++short_runs; },
                            std::chrono::steady_clock::now() + 100ms,
                            [&] { short_timeout.set_value(); }),
          "short waiter was rejected");

  const bool short_expired =
      short_timeout_future.wait_for(1s) == std::future_status::ready;
  release_worker.set_value();
  require(short_expired, "new earlier deadline did not wake timeout thread");
  require(long_waiter_future.wait_for(2s) == std::future_status::ready,
          "long waiter did not execute after worker released");
  pool.stop();
  require(short_runs == 0, "expired short waiter executed");
  require(long_timeouts == 0, "long waiter timed out unexpectedly");
}

void test_invalid_submission_is_rejected_without_callback() {
  ThreadPool pool("invalid");
  pool.set_max_wait_queue_size(1);
  std::atomic<int> runs{0};
  std::atomic<int> timeouts{0};
  auto task = [&] { ++runs; };
  auto on_timeout = [&] { ++timeouts; };

  require(!pool.submit_async(task, std::chrono::steady_clock::now() + 1s, on_timeout),
          "unstarted pool accepted task");
  pool.start(0);
  require(!pool.submit_async(task, std::chrono::steady_clock::now() + 1s, on_timeout),
          "zero-worker pool accepted async task");
  pool.stop();
  require(runs == 0 && timeouts == 0,
          "rejected task ran or invoked timeout callback");

  ThreadPool running_pool("expired");
  running_pool.set_max_wait_queue_size(1);
  running_pool.start(1);
  require(!running_pool.submit_async(task, std::chrono::steady_clock::now() - 1s,
                                     on_timeout),
          "expired task was accepted");
  running_pool.stop();
  require(runs == 0 && timeouts == 0,
          "expired rejected task ran or invoked timeout callback");
}
}  // namespace

int main() {
  test_immediate_submission_executes_without_timeout();
  test_full_queue_accepts_waiter_without_blocking_and_promotes_it();
  test_waiter_timeout_does_not_execute_task();
  test_wait_queue_capacity_rejects_excess_task();
  test_new_earlier_deadline_wakes_timeout_thread();
  test_invalid_submission_is_rejected_without_callback();
  return 0;
}
