#include "callback_registry.h"
#include "timer_manager.h"
#include "wake_pipe.h"

using TLSSMON::CallbackRegistry;
using TLSSMON::TimerManager;
using TLSSMON::WakeupPipe;

int main()
{
    CallbackRegistry registry;
    WakeupPipe wakeup;

    {
        TimerManager manager(registry, wakeup);
    }

    // 再构造一次，确认空 TimerManager 可以正常析构。
    {
        TimerManager manager(registry, wakeup);
    }

    return 0;
}

