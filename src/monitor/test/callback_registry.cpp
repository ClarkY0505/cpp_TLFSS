#include "../callback_registry.h"

  #include <cassert>
  #include <iostream>
  #include <sstream>
  #include <string>

  using namespace TLSSMON;

  static void test_registered_async_callback_is_stopped()
  {
      CallbackRegistry registry;

      EnhancedCallback callback(MonCallback{
          "async",
          [] {
              return 0;
          },
          true
      });

      registry.add(&callback);
      registry.stop_workers();

      // stop_worker() 后不再接受激活。
      assert(callback.activate() == -1);
  }

  static void test_removed_callback_is_not_stopped()
  {
      CallbackRegistry registry;

      EnhancedCallback callback(MonCallback{
          "removed",
          [] {
              return 19;
          },
          false
      });

      registry.add(&callback);
      registry.remove(&callback);
      registry.stop_workers();

      // 已从 Registry 删除，不受 stop_workers() 影响。
      assert(callback.activate() == 19);
  }

  static void test_print_stats_contains_callback_name()
  {
      CallbackRegistry registry;

      EnhancedCallback callback(MonCallback{
          "printable",
          [] {
              return 0;
          },
          false
      });

      registry.add(&callback);
      assert(callback.activate() == 0);

      std::ostringstream output;
      std::streambuf* original = std::clog.rdbuf(output.rdbuf());

      registry.print_stats();

      std::clog.rdbuf(original);

      assert(output.str().find("printable") != std::string::npos);
      assert(output.str().find("count=1") != std::string::npos);
  }

  static void test_null_operations_are_ignored()
  {
      CallbackRegistry registry;

      registry.add(nullptr);
      registry.remove(nullptr);
      registry.stop_workers();
      registry.print_stats();
  }

  int main()
  {
      test_registered_async_callback_is_stopped();
      test_removed_callback_is_not_stopped();
      test_print_stats_contains_callback_name();
      test_null_operations_are_ignored();
  }

