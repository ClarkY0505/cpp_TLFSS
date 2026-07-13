#include <gtest/gtest.h>

extern "C" {
#include "monitor/monitoringSys.h"
#include "monitor/monttoringTimer.h"
}

namespace {

int count_callback(void *arg[])
{
    int *count = static_cast<int *>(arg[0]);
    ++(*count);
    return 0;
}

} // namespace

TEST(MonitorTimer, FiresReadyOneShotTimer)
{
    sys_mon_timer_cleanup();

    int count = 0;
    SysMonCallback_t cb = {
        "count_callback",
        count_callback,
        {&count, nullptr, nullptr, nullptr},
    };

    ASSERT_NE(nullptr, sys_mon_timer_set(&cb, SM_TIMER_ONCE, 0));

    struct timeval sleep = {};
    EXPECT_EQ(1, sys_mon_timer_check(&sleep));
    EXPECT_EQ(1, count);

    EXPECT_EQ(0, sys_mon_timer_check(&sleep));

    sys_mon_timer_cleanup();
}
