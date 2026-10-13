#include "monitor/callback_registry.h"
#include "monitor/engine_type.h"
#include "monitor/timer_manager.h"
#include "monitor/wake_pipe.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

TEST(MonitorTimer, FiresReadyOneShotTimer)
{
    TLSSMON::CallbackRegistry registry;
    TLSSMON::WakeupPipe wakeup;
    ASSERT_EQ(TLSSMON::PIPESTATUS::SUCCESSFUL, wakeup.init());

    TLSSMON::TimerManager manager(registry, wakeup);
    int count = 0;
    const auto handle = manager.add(
        TLSSMON::MonCallback{"count_callback", [&count] {
            ++count;
            return 0;
        }, false},
        TLSSMON::TimerFlags::ONCE,
        std::chrono::milliseconds{5});
    ASSERT_TRUE(handle.has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    timeval timeout{};
    manager.check(timeout);
    EXPECT_EQ(1, count);

    manager.check(timeout);
    EXPECT_EQ(1, count);
}
