#include "callback_registry.h"

  #include <cassert>
  #include <chrono>
  #include <condition_variable>
  #include <mutex>
  #include <utility>

  using namespace TLSSMON;

  static void test_inline_callback()
  {
      int call_count = 0;

      MonCallback spec{
          "inline",
          [&call_count] {
              ++call_count;
              return 17;
          },
          false
      };

      EnhancedCallback callback(std::move(spec));

      assert(callback.name() == "inline");
      assert(callback.activate() == 17);
      assert(call_count == 1);

      const CallbackStats stats = callback.stats();

      assert(stats._count == 1);
      assert(stats._min_us <= stats._max_us);
      assert(stats._total_us >= stats._min_us);
  }

  static void test_async_callback()
  {
      std::mutex mutex;
      std::condition_variable condition;
      bool called = false;

      MonCallback spec{
          "async",
          [&] {
              {
                  std::lock_guard<std::mutex> lock(mutex);
                  called = true;
              }
              condition.notify_one();
              return 0;
          },
          true
      };

      EnhancedCallback callback(std::move(spec));

      assert(callback.activate() == 0);

      {
          std::unique_lock<std::mutex> lock(mutex);
          assert(condition.wait_for(
              lock,
              std::chrono::seconds(1),
              [&called] {
                  return called;
              }));
      }

      callback.stop_worker();

      const CallbackStats stats = callback.stats();
      assert(stats._count == 1);
  }

  int main()
  {
      test_inline_callback();
      test_async_callback();
  }
